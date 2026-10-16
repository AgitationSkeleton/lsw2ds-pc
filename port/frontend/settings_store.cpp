/* See settings_store.h. */

#include "settings_store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

/* A FLAT JSON PARSER, and no more than that.
 *
 * Writing one is the right call here against pulling in a library: the file is
 * a single object of scalars that this program both writes and reads, the
 * parser is forty lines, and the port already refuses external dependencies it
 * does not need. The cost of being wrong is bounded too - a key that fails to
 * parse is skipped, not fatal.
 *
 * It deliberately does NOT support nesting, arrays, or escapes beyond the ones
 * emitted below. If the settings ever need those, this should be replaced
 * rather than extended. */

void skip_ws(const char*& p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
}

/* Read a JSON string body into `out`, with p positioned at the opening quote.
 * Returns false on an unterminated string, which is the one malformation that
 * would otherwise run off the end of the buffer. */
bool read_string(const char*& p, std::string& out) {
    if (*p != '"') return false;
    ++p;
    out.clear();
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            ++p;
            switch (*p) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                /* Anything else escaped is itself - covers \" and \\, which are
                 * the only two the writer produces. */
                default:  out.push_back(*p); break;
            }
            ++p;
        } else {
            out.push_back(*p++);
        }
    }
    if (*p != '"') return false;
    ++p;
    return true;
}

std::string encode_string(const std::string& in) {
    std::string out = "\"";
    for (char c : in) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

/* Numbers are written with %.6g and trailing zeros trimmed, so an int stays an
 * int in the file. A settings file full of "2.000000" invites the reader to
 * think the value is a float when it is a window scale. */
std::string encode_number(double v) {
    char buf[64];
    /* A multi-word type name cannot take a functional cast - `long long(v)` is
     * a syntax error, not a conversion. static_cast is the form that works and
     * is clearer about intent anyway. */
    const long long as_int = static_cast<long long>(v);
    if (v == static_cast<double>(as_int) && v > -1e15 && v < 1e15)
        std::snprintf(buf, sizeof buf, "%lld", as_int);
    else
        std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

std::string read_file(const std::string& path, bool* existed) {
    if (existed) *existed = false;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    if (existed) *existed = true;
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

}  // namespace

const SettingsStore::Entry* SettingsStore::find(const std::string& key) const {
    for (const auto& e : entries_) if (e.key == key) return &e;
    return nullptr;
}

SettingsStore::Entry* SettingsStore::find(const std::string& key) {
    for (auto& e : entries_) if (e.key == key) return &e;
    return nullptr;
}

bool SettingsStore::load(const std::string& path) {
    bool existed = false;
    const std::string text = read_file(path, &existed);
    if (!existed) return true;      /* first run; defaults stand */
    if (text.empty()) return false;

    const char* p = text.c_str();

    /* SKIP A UTF-8 BOM, because rejecting the whole file over three bytes is a
     * silent and total settings loss.
     *
     * FOUND BY ACCIDENT, and the accident is the argument for the
     * fix. A test script rewrote settings.json with PowerShell's
     * `Set-Content -Encoding utf8`, which on Windows PowerShell 5.1 writes
     * UTF-8 WITH a BOM. The port then logged
     *
     *     [settings] could not read settings.json; using defaults
     *     [settings] imported 36 values from settings.ini
     *
     * and fell back to the legacy .ini - so every JSON-only setting silently
     * reverted, and the only trace was one line in a log nobody reads during
     * play. It looked exactly like a separate, recurring customization bug,
     * and a control run is what showed it was not.
     *
     * ANY EDITOR CAN DO THIS. Notepad writes a BOM by default; several others
     * offer it. A player who opens settings.json to change one value and saves
     * it can lose all of them, with no visible error. Three bytes should not
     * cost that, and `skip_ws` cannot help because a BOM is not whitespace. */
    if (text.size() >= 3 && static_cast<unsigned char>(p[0]) == 0xEFu &&
        static_cast<unsigned char>(p[1]) == 0xBBu &&
        static_cast<unsigned char>(p[2]) == 0xBFu) {
        p += 3;
    }

    skip_ws(p);
    if (*p != '{') return false;
    ++p;
    for (;;) {
        skip_ws(p);
        if (*p == '}' || !*p) break;
        if (*p == ',') { ++p; continue; }

        std::string key;
        if (!read_string(p, key)) break;   /* malformed key: stop, keep what we have */
        skip_ws(p);
        if (*p != ':') break;
        ++p;
        skip_ws(p);

        Entry e;
        e.key = key;
        if (*p == '"') {
            std::string value;
            if (!read_string(p, value)) break;
            e.value = value;
            e.is_string = true;
        } else {
            const char* start = p;
            while (*p && *p != ',' && *p != '}' && *p != '\n' && *p != '\r') ++p;
            e.value.assign(start, size_t(p - start));
            /* Trim trailing spaces so get_number does not see " 2". */
            while (!e.value.empty() && (e.value.back() == ' ' || e.value.back() == '\t'))
                e.value.pop_back();
            e.is_string = false;
            /* An empty or non-numeric value is skipped rather than stored: it
             * would round-trip as garbage and there is nothing useful to do
             * with it. */
            if (e.value.empty()) continue;
        }
        /* LAST ONE WINS, and it replaces in place. A duplicated key in a
         * hand-edited file should not silently become two entries that fight
         * on every save. */
        if (Entry* existing = find(key)) { *existing = e; }
        else entries_.push_back(std::move(e));
    }
    return true;
}

bool SettingsStore::import_ini(const std::string& path) {
    bool existed = false;
    const std::string text = read_file(path, &existed);
    if (!existed) return false;

    size_t pos = 0;
    int imported = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (value.empty()) continue;
        /* The .ini held numbers only, so everything imports as a number. */
        Entry e{key, value, false};
        if (Entry* existing = find(key)) *existing = e;
        else entries_.push_back(std::move(e));
        ++imported;
    }
    if (imported)
        std::fprintf(stderr, "[settings] imported %d values from %s\n",
                     imported, path.c_str());
    return imported > 0;
}

bool SettingsStore::save(const std::string& path) const {
    /* ATOMIC: write beside the target, then rename over it. An interrupted
     * write leaves the old file rather than half a new one. */
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;

    std::fprintf(f, "{\n");
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        std::fprintf(f, "  %s: %s%s\n",
                     encode_string(e.key).c_str(),
                     e.is_string ? encode_string(e.value).c_str() : e.value.c_str(),
                     i + 1 < entries_.size() ? "," : "");
    }
    std::fprintf(f, "}\n");
    const bool ok = std::fflush(f) == 0;
    std::fclose(f);
    if (!ok) { std::remove(tmp.c_str()); return false; }

    /* rename() will not replace an existing file on Windows, so the target goes
     * first. The window between the two is why the temporary is written in full
     * beforehand - if the rename fails, the temporary is still a complete,
     * readable settings file rather than nothing. */
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::fprintf(stderr,
            "[settings] could not replace %s; your settings are in %s\n",
            path.c_str(), tmp.c_str());
        return false;
    }
    return true;
}

bool SettingsStore::has(const std::string& key) const { return find(key) != nullptr; }

std::string SettingsStore::get_string(const std::string& key,
                                      const std::string& fallback) const {
    const Entry* e = find(key);
    return e ? e->value : fallback;
}

double SettingsStore::get_number(const std::string& key, double fallback) const {
    const Entry* e = find(key);
    if (!e) return fallback;
    char* end = nullptr;
    const double v = std::strtod(e->value.c_str(), &end);
    /* strtod returning 0 with no characters consumed means "not a number", and
     * that must fall back rather than become 0 - a corrupted line should not
     * silently turn a setting off. */
    if (end == e->value.c_str()) return fallback;
    return v;
}

bool SettingsStore::get_bool(const std::string& key, bool fallback) const {
    const Entry* e = find(key);
    if (!e) return fallback;
    if (e->value == "true") return true;
    if (e->value == "false") return false;
    return get_number(key, fallback ? 1.0 : 0.0) != 0.0;
}

void SettingsStore::set_string(const std::string& key, const std::string& value) {
    mark_touched(key);
    if (Entry* e = find(key)) { e->value = value; e->is_string = true; return; }
    entries_.push_back(Entry{key, value, true});
}

void SettingsStore::set_number(const std::string& key, double value) {
    mark_touched(key);
    const std::string encoded = encode_number(value);
    if (Entry* e = find(key)) { e->value = encoded; e->is_string = false; return; }
    entries_.push_back(Entry{key, encoded, false});
}

void SettingsStore::set_int(const std::string& key, int value) {
    set_number(key, double(value));
}

void SettingsStore::set_bool(const std::string& key, bool value) {
    mark_touched(key);
    if (Entry* e = find(key)) { e->value = value ? "true" : "false"; e->is_string = false; return; }
    entries_.push_back(Entry{key, value ? "true" : "false", false});
}

void SettingsStore::begin_tracking() { tracking_ = true; touched_.clear(); }

void SettingsStore::mark_touched(const std::string& key) {
    if (!tracking_) return;
    for (const auto& k : touched_) if (k == key) return;
    touched_.push_back(key);
}

std::vector<std::string> SettingsStore::untouched_keys() const {
    std::vector<std::string> out;
    if (!tracking_) return out;
    for (const auto& e : entries_) {
        bool found = false;
        for (const auto& k : touched_) if (k == e.key) { found = true; break; }
        if (!found) out.push_back(e.key);
    }
    return out;
}

std::string lsw2_settings_path(const char* requested) {
    std::string path = (requested && *requested) ? requested : "settings.json";
    const size_t n = path.size();
    if (n > 4 && path.compare(n - 4, 4, ".ini") == 0)
        path.replace(n - 4, 4, ".json");
    else if (n < 5 || path.compare(n - 5, 5, ".json") != 0)
        path += ".json";
    return path;
}
