/* A settings file that does not lose what it does not understand.
 *
 * THE DEFECT THIS EXISTS TO FIX. The old writer emitted the keys the running
 * binary knew about and nothing else, so the file was effectively rebuilt from
 * scratch on every clean exit. Three consequences, all of which bit:
 *
 *   1. A KEY THE RUNNING BINARY DID NOT KNOW WAS DELETED. The baseline and the
 *      fork share one file, and the fork has settings the baseline never shows
 *      (the whole Multiplayer tab). Running the baseline once wiped them.
 *
 *   2. A NEW BUILD LOST THE OLD BUILD'S SETTINGS whenever a key was renamed or
 *      added, because the loader skipped what it did not recognise and the
 *      writer then did not write it back.
 *
 *   3. THE LOADER STOPPED AT THE FIRST LINE IT COULD NOT PARSE, so one bad line
 *      silently discarded every setting after it.
 *
 * THE FIX is to treat the file as a store rather than as a serialisation of the
 * settings struct. Load every key into a map, overlay the ones we understand
 * onto UiSettings, and on save update only those keys and write the WHOLE map
 * back. Anything written by another build, another mode, or a future version
 * survives untouched.
 *
 * That is what "respecting both versions' settings" requires: the two builds
 * are not the same program and never will be, so neither may assume its own key
 * set is the complete one.
 *
 * FORMAT: flat JSON - one object, string and number values, no nesting. Flat
 * because the settings are flat, and hand-editable because a user who wants to
 * fix a bad binding should not need a tool. Insertion order is preserved so the
 * file does not reshuffle itself on every write and produce a meaningless diff.
 */

#ifndef LSW2_SETTINGS_STORE_H
#define LSW2_SETTINGS_STORE_H

#include <string>
#include <utility>
#include <vector>

class SettingsStore {
public:
    /* Read a flat JSON object. Missing file is not an error - it is a first
     * run, and the caller's defaults stand.
     *
     * Returns false only when the file exists and could not be read at all. A
     * malformed VALUE is skipped rather than fatal, for the same reason the old
     * loader's early exit was a bug: one bad line must not cost the rest. */
    bool load(const std::string& path);

    /* Write the whole store back, atomically: the bytes go to a temporary file
     * beside the target which then replaces it. A crash or a full disk during
     * the write leaves the previous settings intact rather than a truncated
     * file, which is the failure mode that loses a session of rebinding. */
    bool save(const std::string& path) const;

    /* IMPORT AN OLD key=value FILE. Called once, when the JSON does not exist
     * but the .ini does, so nobody's existing bindings are lost to the format
     * change. Keys and values transfer unchanged; the .ini is left on disk. */
    bool import_ini(const std::string& path);

    bool has(const std::string& key) const;
    std::string get_string(const std::string& key, const std::string& fallback) const;
    double get_number(const std::string& key, double fallback) const;
    bool get_bool(const std::string& key, bool fallback) const;

    /* Set replaces in place when the key exists, keeping its position, and
     * appends otherwise. */
    void set_string(const std::string& key, const std::string& value);
    void set_number(const std::string& key, double value);
    void set_int(const std::string& key, int value);
    void set_bool(const std::string& key, bool value);

    /* Keys this build did not touch during a save - i.e. what was preserved
     * from another build or mode. Only for reporting; the store keeps them
     * regardless. */
    std::vector<std::string> untouched_keys() const;

    /* Call before applying a build's own keys, so untouched_keys() means
     * something. */
    void begin_tracking();
    void mark_touched(const std::string& key);

private:
    struct Entry {
        std::string key;
        std::string value;   /* JSON-encoded: quoted for strings, bare for numbers */
        bool is_string = false;
    };
    std::vector<Entry> entries_;
    std::vector<std::string> touched_;
    bool tracking_ = false;

    const Entry* find(const std::string& key) const;
    Entry* find(const std::string& key);
};

/* Resolve the settings path. Given anything ending in ".ini" this returns the
 * ".json" beside it, so the launchers' existing LSW2_SETTINGS values keep
 * working and point at the new file without every launcher needing an edit. */
std::string lsw2_settings_path(const char* requested);

#endif /* LSW2_SETTINGS_STORE_H */
