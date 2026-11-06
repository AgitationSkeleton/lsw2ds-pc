/* UPDATES - see updater.h. Built on the LEGO Battles PC port's updater
 * (lb_update.cpp, lb_https.cpp and the check in lb_overlay.cpp), adapted to
 * this release's layout: the exe lives in build/, one level below the
 * install, and two exes share that folder. */
#include "updater.h"

#include <SDL.h>
#include <zlib.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

namespace {
namespace fs = std::filesystem;

constexpr const char* kRepo = "AgitationSkeleton/lsw2ds-pc";
constexpr const char* kReleasesUrl = "https://github.com/AgitationSkeleton/lsw2ds-pc/releases";
constexpr const char* kTopPrefix = "LSW2DS-PC-";

/* ------------------------------------------------------------- where we are */

fs::path exe_path() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH] = L"";
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH)) return fs::path(buf);
#endif
    return fs::path();
}

/* The install: the folder above build/. */
fs::path install_dir() { return exe_path().parent_path().parent_path(); }

/* ------------------------------------------------------------------- HTTPS */

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &w[0], n);
    return w;
}
#endif

/* A GET over HTTPS through Windows' own WinHTTP (certificates are checked by
 * Windows; no TLS library ships with the game). Redirects are followed, which
 * a release download needs. `max` bounds the body. */
bool https_get(const std::string& host, const std::string& path, std::string* data,
               size_t max, bool want_200, std::string* error) {
    data->clear();
#ifdef _WIN32
    HINTERNET session = WinHttpOpen(L"LSW2DS-PC updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { *error = "HTTPS unavailable"; return false; }
    WinHttpSetTimeouts(session, 8000, 8000, 15000, 30000);
    HINTERNET conn = WinHttpConnect(session, widen(host).c_str(),
                                    INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", widen(path).c_str(), nullptr,
                                              WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE)
                         : nullptr;
    bool ok = false;
    if (req && WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n",
                                  DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, len = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                            WINHTTP_NO_HEADER_INDEX);
        ok = want_200 ? status == 200 : (status >= 200 && status < 500);
        if (!ok) *error = "HTTP status " + std::to_string(unsigned(status));
        while (ok) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::string chunk(size_t(avail), '\0');
            DWORD got = 0;
            if (!WinHttpReadData(req, &chunk[0], avail, &got) || got == 0) break;
            data->append(chunk.data(), got);
            if (data->size() > max) { ok = false; *error = "the file is too large"; }
        }
    } else {
        *error = "cannot reach " + host;
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    if (!ok) data->clear();
    return ok;
#else
    (void)host; (void)path; (void)max; (void)want_200;
    *error = "updates are Windows-only";
    return false;
#endif
}

/* -------------------------------------------------------------- the check */

std::mutex g_mu;
Lsw2UpdateInfo g_info;
std::atomic<bool> g_check_busy{false};
std::string g_notice;
unsigned g_notice_ms = 0;

int g_state = 0;   /* under g_mu: 0 idle, 1 downloading, 2 installing, 3 installed, 4 failed */
std::string g_msg;
std::atomic<bool> g_install_busy{false};

void set_state(int state, const std::string& msg) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_state = state;
        g_msg = msg;
    }
    std::fprintf(stderr, "[update] %s\n", msg.c_str());
    std::fflush(stderr);
}

void post_notice(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_notice = text;
    g_notice_ms = SDL_GetTicks();
}

/* The string value of the first "key" in a JSON text, unescaped. */
std::string json_str(const std::string& s, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    for (size_t p = s.find(pat); p != std::string::npos; p = s.find(pat, p + 1)) {
        size_t q = p + pat.size();
        while (q < s.size() && (s[q] == ' ' || s[q] == ':')) ++q;
        if (q >= s.size() || s[q] != '"') continue;
        std::string out;
        for (++q; q < s.size() && s[q] != '"'; ++q) {
            if (s[q] == '\\' && q + 1 < s.size()) {
                const char c = s[++q];
                if (c == 'n') out += '\n';
                else if (c == 'r') {}
                else if (c == 't') out += ' ';
                else if (c == 'u' && q + 4 < s.size()) {
                    const unsigned cp = unsigned(std::strtoul(s.substr(q + 1, 4).c_str(), nullptr, 16));
                    out += cp < 0x80 ? std::string(1, char(cp)) : std::string("?");
                    q += 4;
                } else out += c;
            } else out += s[q];
        }
        return out;
    }
    return std::string();
}

void check_run(bool quiet, bool auto_install) {
    std::string reply, error;
    /* The list, newest first, rather than releases/latest: that skips
     * pre-releases, and every release so far is one. */
    const bool ok = https_get("api.github.com",
                              std::string("/repos/") + kRepo + "/releases?per_page=1",
                              &reply, 1u << 20, false, &error);
    Lsw2UpdateInfo r;
    if (!ok) {
        r.state = 3;
        r.error = "Could not reach GitHub (" + error + ").";
    } else if (reply.find("\"tag_name\"") == std::string::npos) {
        r.state = 2;
        r.error = "No release has been published yet.";
    } else {
        r.state = 2;
        r.tag = json_str(reply, "tag_name");
        r.name = json_str(reply, "name");
        r.notes = json_str(reply, "body");
        r.url = json_str(reply, "html_url");
        if (r.url.compare(0, 8, "https://") != 0) r.url = kReleasesUrl;
        if (r.notes.size() > 3000) r.notes = r.notes.substr(0, 3000) + "...";
        const std::string& mine = lsw2_update_this_version();
        r.newer = !mine.empty() && lsw2_update_compare(r.tag, mine) > 0;
        /* The game zip among the assets: LSW2DS-PC-<tag>.zip. */
        const std::string key = "\"browser_download_url\":";
        for (size_t p = reply.find(key); p != std::string::npos; p = reply.find(key, p + 1)) {
            const size_t q0 = reply.find('"', p + key.size());
            const size_t q1 = q0 == std::string::npos ? q0 : reply.find('"', q0 + 1);
            if (q1 == std::string::npos) break;
            const std::string u = reply.substr(q0 + 1, q1 - q0 - 1);
            if (u.size() > 4 && u.compare(u.size() - 4, 4, ".zip") == 0 &&
                u.find(std::string("/") + kTopPrefix) != std::string::npos)
                r.asset = u;
        }
    }
    std::fprintf(stderr, "[update] %s: %s, newest \"%s\", newer=%d, this build \"%s\"%s%s\n",
                 quiet ? "startup check" : "check", r.state == 3 ? "failed" : "ok",
                 r.tag.c_str(), r.newer ? 1 : 0, lsw2_update_this_version().c_str(),
                 r.error.empty() ? "" : " - ", r.error.c_str());
    std::fflush(stderr);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_info = r;
    }
    if (quiet && r.newer) {
        if (auto_install && !r.asset.empty() && lsw2_update_install(r.asset, r.tag))
            post_notice("Installing " + r.tag + " (you have " + lsw2_update_this_version() +
                        "). F1 > Settings > Updates says when it is ready.");
        else
            post_notice("A newer version is out: " + r.tag + " (you have " +
                        lsw2_update_this_version() + "). F1 > Settings > Updates installs it.");
    }
    g_check_busy = false;
}

/* --------------------------------------------------------------- the zip */

uint32_t le16(const std::string& d, size_t at) {
    return uint32_t(uint8_t(d[at])) | (uint32_t(uint8_t(d[at + 1])) << 8);
}
uint32_t le32(const std::string& d, size_t at) { return le16(d, at) | (le16(d, at + 2) << 16); }

bool inflate_entry(const std::string& zip, size_t local, uint32_t method, uint32_t csize,
                   uint32_t usize, std::string* out) {
    if (local + 30 > zip.size() || le32(zip, local) != 0x04034B50u) return false;
    const size_t data = local + 30 + le16(zip, local + 26) + le16(zip, local + 28);
    if (data + csize > zip.size() || usize > (400u << 20)) return false;
    if (method == 0) {
        out->assign(zip, data, csize);
        return csize == usize;
    }
    if (method != 8) return false;
    out->assign(usize, '\0');
    if (usize == 0) return true;
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(zip.data() + data));
    z.avail_in = csize;
    z.next_out = reinterpret_cast<Bytef*>(&(*out)[0]);
    z.avail_out = usize;
    const int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return r == Z_STREAM_END && z.total_out == usize;
}

struct Entry { std::string rel; uint32_t method, csize, usize, local; };

/* THE PLAYER'S OWN FILES, never written by an update (the zip holds none of
 * them either; this is the second lock): settings, saves (the shipped unlock
 * template included - it lives among the player's saves), the generated BIOS
 * identity, the ROM, logs and crash dumps. */
bool players_own(const std::string& rel) {
    std::string r = rel;
    for (char& c : r) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const char* d : {"port/saves/", "port/bios/", "rom/", "build/logs/"})
        if (r.compare(0, std::strlen(d), d) == 0) return true;
    if (r.size() > 4 && r.compare(r.size() - 4, 4, ".dmp") == 0) return true;
    if (r.find("settings") != std::string::npos && r.size() > 5 &&
        r.compare(r.size() - 5, 5, ".json") == 0) return true;
    return false;
}

/* The zip's files below its one top folder, or false if it is not a release. */
bool list_release(const std::string& zip, std::vector<Entry>* out, std::string* why) {
    size_t eocd = std::string::npos;
    if (zip.size() >= 22) {
        for (size_t i = zip.size() - 22;; --i) {
            if (le32(zip, i) == 0x06054B50u) { eocd = i; break; }
            if (i == 0 || zip.size() - i > 66000) break;
        }
    }
    if (eocd == std::string::npos) { *why = "not a zip"; return false; }
    const uint32_t count = le16(zip, eocd + 10);
    size_t at = le32(zip, eocd + 16);
    std::string top;
    bool have_exe = false, have_version = false;
    for (uint32_t k = 0; k < count && at + 46 <= zip.size(); ++k) {
        if (le32(zip, at) != 0x02014B50u) { *why = "a damaged zip"; return false; }
        Entry e;
        e.method = le16(zip, at + 10);
        e.csize = le32(zip, at + 20);
        e.usize = le32(zip, at + 24);
        const uint32_t nlen = le16(zip, at + 28), xlen = le16(zip, at + 30),
                       clen = le16(zip, at + 32);
        e.local = le32(zip, at + 42);
        std::string name = zip.substr(at + 46, nlen);
        at += 46 + nlen + xlen + clen;
        for (char& c : name) if (c == '\\') c = '/';
        const size_t slash = name.find('/');
        if (slash == std::string::npos || slash == 0) {
            *why = "a file outside the release folder";
            return false;
        }
        const std::string t = name.substr(0, slash);
        if (top.empty()) top = t;
        if (t != top || t.compare(0, std::strlen(kTopPrefix), kTopPrefix) != 0) {
            *why = "not an LSW2DS-PC release";
            return false;
        }
        e.rel = name.substr(slash + 1);
        if (e.rel.empty() || e.rel.back() == '/') continue;   /* a folder */
        if (e.rel.find("..") != std::string::npos || e.rel.find(':') != std::string::npos ||
            e.rel[0] == '/') {
            *why = "an unsafe path in the zip";
            return false;
        }
        if (e.rel == "build/lsw2_bountyhunter.exe") have_exe = true;
        if (e.rel == "build/version.txt") have_version = true;
        out->push_back(e);
    }
    if (!have_exe || !have_version) { *why = "a zip without the game in it"; return false; }
    return true;
}

bool write_file(const fs::path& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), std::streamsize(data.size()));
    f.close();
    return bool(f);
}

void install_run(std::string url, std::string tag) {
    set_state(1, "Downloading " + tag + "...");
    /* https://github.com/<repo>/releases/download/<tag>/<file>.zip, and
     * nothing else: the URL came from GitHub's reply, but it is checked. */
    const std::string pre = "https://github.com";
    const std::string want = std::string("/") + kRepo + "/releases/download/";
    if (url.compare(0, pre.size(), pre) != 0 ||
        url.compare(pre.size(), want.size(), want) != 0) {
        set_state(4, "That download is not one of this game's releases.");
        g_install_busy = false;
        return;
    }
    std::string zip, err;
    if (!https_get("github.com", url.substr(pre.size()), &zip, 400u << 20, true, &err)) {
        set_state(4, "The download failed: " + err);
        g_install_busy = false;
        return;
    }
    set_state(2, "Installing " + tag + "...");
    std::vector<Entry> files;
    std::string why;
    if (!list_release(zip, &files, &why)) {
        set_state(4, "The download is " + why + "; nothing was changed.");
        g_install_busy = false;
        return;
    }
    /* Unpack everything first: a broken file stops the update before
     * anything in the install is touched. */
    std::vector<std::pair<std::string, std::string>> data;
    for (const Entry& e : files) {
        if (players_own(e.rel)) continue;
        std::string bytes;
        if (!inflate_entry(zip, e.local, e.method, e.csize, e.usize, &bytes)) {
            set_state(4, "The download is damaged (" + e.rel + "); nothing was changed.");
            g_install_busy = false;
            return;
        }
        data.push_back({e.rel, std::move(bytes)});
    }
    zip.clear();
    zip.shrink_to_fit();
    /* A file Windows holds open - the running exe, its DLLs - cannot be
     * overwritten but can be renamed: each replaced file becomes <name>.old,
     * deleted at the next start. */
    const fs::path dir = install_dir();
    std::error_code ec;
    int written = 0;
    for (const auto& f : data) {
        const fs::path target = dir / fs::u8path(f.first);
        fs::create_directories(target.parent_path(), ec);
        if (fs::exists(target, ec)) {
            fs::path old = target;
            old += ".old";
            fs::remove(old, ec);
            fs::rename(target, old, ec);
            if (ec) {
                set_state(4, "Could not replace " + f.first + " (" + ec.message() +
                                 "). The update stopped part way: download it from the "
                                 "release page instead.");
                g_install_busy = false;
                return;
            }
        }
        if (!write_file(target, f.second)) {
            set_state(4, "Could not write " + f.first + ". The update stopped part way: "
                             "download it from the release page instead.");
            g_install_busy = false;
            return;
        }
        ++written;
    }
    set_state(3, tag + " is installed (" + std::to_string(written) +
                     " files). Restart the game to use it.");
    g_install_busy = false;
}

}  // namespace

/* ----------------------------------------------------------------- public */

const std::string& lsw2_update_this_version() {
    static const std::string v = [] {
        std::ifstream f(exe_path().parent_path() / "version.txt");
        std::string s;
        if (f) std::getline(f, s);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
        return s;
    }();
    return v;
}

bool lsw2_update_supported() {
#ifdef _WIN32
    const fs::path exe = exe_path();
    const std::string name = exe.filename().string();
    return (name == "lsw2_bountyhunter.exe" || name == "lsw2.exe") &&
           exe.parent_path().filename() == "build" && !lsw2_update_this_version().empty();
#else
    return false;
#endif
}

void lsw2_update_check(bool quiet, bool auto_install) {
    if (g_check_busy.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_info.state = 1;
    }
    std::thread([quiet, auto_install] { check_run(quiet, auto_install); }).detach();
}

Lsw2UpdateInfo lsw2_update_info() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_info;
}

bool lsw2_update_install(const std::string& asset, const std::string& tag) {
    if (asset.empty() || !lsw2_update_supported()) return false;
    if (g_install_busy.exchange(true)) return false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_state = 1;
    }
    std::thread([asset, tag] {
        try {
            install_run(asset, tag);
        } catch (const std::exception& e) {
            set_state(4, std::string("The update failed: ") + e.what());
            g_install_busy = false;
        }
    }).detach();
    return true;
}

int lsw2_update_state(std::string* msg) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (msg) *msg = g_msg;
    return g_state;
}

void lsw2_update_restart() {
#ifdef _WIN32
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = GetCommandLineW();
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                       &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        SDL_Event q{};
        q.type = SDL_QUIT;
        SDL_PushEvent(&q);
    } else {
        set_state(4, "Could not start the new version: start the game again by hand.");
    }
#endif
}

void lsw2_update_cleanup() {
    if (!lsw2_update_supported()) return;
    std::error_code ec;
    int n = 0;
    const fs::path dir = install_dir();
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path& p = it->path();
        const std::string rel = fs::relative(p, dir, ec).generic_string();
        if (players_own(rel + (it->is_directory(ec) ? "/" : ""))) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && p.extension() == ".old") {
            std::error_code e2;
            if (fs::remove(p, e2)) ++n;
        }
    }
    if (n) {
        std::fprintf(stderr, "[update] removed %d file(s) left by the last update\n", n);
        std::fflush(stderr);
    }
}

std::string lsw2_update_notice(unsigned* posted_ms) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (posted_ms) *posted_ms = g_notice_ms;
    return g_notice;
}

int lsw2_update_compare(const std::string& a, const std::string& b) {
    auto parse = [](std::string s, int n[3], std::string* suffix) {
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
        n[0] = n[1] = n[2] = 0;
        size_t i = 0;
        for (int k = 0; k < 3; ++k) {
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
                n[k] = n[k] * 10 + (s[i++] - '0');
            if (i < s.size() && s[i] == '.' && k < 2) ++i;
            else break;
        }
        *suffix = i < s.size() ? s.substr(i) : std::string();
    };
    int na[3], nb[3];
    std::string sa, sb;
    parse(a, na, &sa);
    parse(b, nb, &sb);
    for (int k = 0; k < 3; ++k)
        if (na[k] != nb[k]) return na[k] > nb[k] ? 1 : -1;
    if (sa == sb) return 0;
    if (sa.empty()) return 1;    /* 0.4.0 is later than 0.4.0-rc.1 */
    if (sb.empty()) return -1;
    return sa > sb ? 1 : -1;
}
