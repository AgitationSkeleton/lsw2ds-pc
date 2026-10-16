"""Every setting must have something that ACTS on it.

WHY THIS EXISTS. Many settings in the ImGui panels, for both the base game and
Bounty Hunter, turned out to be unwired. Two audits followed and the first was
wrong:
all 47 `ImGui::` controls have their bound variable read somewhere, so dead
controls are not the problem.

The second found it. `Lsw2HostSettings::music` is stored, persisted, sent over
the wire and printed by `diagnostics.cpp` - and nothing selects a random battle
track, which is what the setting exists to do. In C it looks wired. To a player
it does nothing.

    A setting that is stored, persisted and transmitted with no consumer reads
    as finished from every direction except the only one that matters.

    python tools/sweep_setting_consumers.py

WHY THIS IS THE THIRD VERSION. The first looked for the FIELD NAME in consumer
files and flagged twenty-eight settings, all correctly wired, because `ui.cpp`
mirrors most settings into a file-static and exposes a getter that never names
the field. The second resolved that getter but only when its body was literally
`{ return g_x; }`, and excluded whole FILES as "not a consumer" - which was
wrong in three more ways, all four of them now fixed:

  * A MIRROR MAY BE RENAMED. `bh_roster_only` is mirrored into
    `g_fork_hunters_only`, so a lookup keyed on the field name found nothing.
    The chain is now followed in the direction the code actually writes it -
    `g_mirror = s.field;` - so a rename is carried rather than lost.
  * AN ACCESSOR NEED NOT RETURN. `lsw2_ui_hud_radar(int*, int*, int*)` writes
    three mirrors through out-params, which is not `return g_x;` in any form.
    Any ui.cpp function that READS a mirror is now that mirror's accessor.
  * A FIELD MAY BE READ THROUGH A STRUCT METHOD. `lobby_time` is never named
    outside hostsettings.cpp; forksession.cpp asks `settings_.lobby_seconds()`,
    and that method is where the field is read.
  * THE CONSUMER MAY BE IN AN "EXCLUDED" FILE, and this was the worst of the
    four because it hid five settings behind a rule that sounded principled.
    `ui.cpp` was excluded as "the checkbox and its mirror" - but ui.cpp is also
    the imgui shell ITSELF, so a setting whose whole subject is that shell
    (`show_metrics`, `touch_panel_x`, `floating_touch_scale`) is legitimately
    acted on there and nowhere else. `forksession.cpp` was excluded as "merely
    transmitting", and it transmits nothing: `visible_master` and `visible_lan`
    appear there as `if (settings_.visible_master)`, deciding whether the lobby
    is advertised, which is the whole of what those settings mean.

So the exclusions are now per LINE, not per file. A mention is not a consumer
when it binds an imgui control, when it persists, or when it fills a mirror -
and those three are recognisable on sight. `hostsettings.cpp` stays excluded
whole, because it genuinely is only the in-game menu, the clamps and the wire
codec, and `music` travelling through that codec is exactly the thing that must
not read as wired.

WHAT IT CANNOT DO. Tell whether a consumer is CORRECT - only that one exists.
Same limit as `sweep_orphans`, and worth having for the same reason: what it
catches is a whole feature quietly missing, not a subtle bug.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FRONTEND = ROOT / "port" / "frontend"

# Files that are ONLY the control surface, the clamps, persistence or a
# reporter. Everything else is read with the line rules below instead.
#
#   hostsettings.*   the in-game menu, the range clamps and the wire codec.
#                    `music` is packed into out[5] there, and a setting that is
#                    only transmitted is precisely what this sweep is for.
#   settings_store.* persistence
#   diagnostics.cpp  prints it - a reporter looks like a reader
NOT_A_CONSUMER = {"hostsettings.cpp", "hostsettings.h",
                  "settings_store.cpp", "settings_store.h",
                  "diagnostics.cpp"}

# Settings with no consumer YET, each owing a named clause. An entry here is a
# promise, not an exemption.
PENDING = {
    # (empty since 2026-09-23: right_stick_invert_y is consumed by frontend.cpp's
    # sample_camera, which inverts the orbit camera's height - enhanced.cpp. Its
    # entry here said the VIEW RECORD has no pitch, which stays true of the
    # record; the camera the stick now steers is the local camera object in the
    # game's mode 4, whose offset has a height.)
}

# A DATA MEMBER, and specifically not a method declaration. `[^;(]` rather than
# `[^;]` is the whole of that distinction: `int lobby_seconds() const;` matched
# with "const" captured as the field name, so Lsw2HostSettings reported eleven
# fields for its nine settings.
FIELD = re.compile(
    r"^\s*(?:bool|int|float|char|unsigned|uint\d+_t|Lsw2\w+)[^;(]*?"
    r"(\w+)\s*(?:\[[^\]]*\])?\s*(?:=|;)", re.M)

FUNC = re.compile(r"\n(?:static\s+)?[\w:*&<>\s]+?(\w+)\s*\([^;]*?\)\s*\{")
KEYWORDS = {"if", "for", "while", "switch", "else", "do", "return", "catch",
            "sizeof"}


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def struct_fields(path: Path, name: str) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"struct " + name + r" \{(.*?)\n\};", text, re.S)
    return FIELD.findall(m.group(1)) if m else []


def statement_around(text: str, at: int) -> str:
    """The whole statement containing `at`, not the line containing it.

    CLASSIFYING BY LINE DOES NOT WORK, and the audit that caught it is the only
    reason this function exists. Four settings were cleared as "consumed" by
    what turned out to be continuation lines:

        ImGui::Checkbox("Free play select",
                        &s.bh_freeplay_select);      <- the control, split
        s.right_stick_deadzone = std::clamp(int(g_store.get_number(
            "right_stick_deadzone", s.right_stick_deadzone)), 5, 90);

    The second line of each carries the field and none of the context that says
    what is being done to it. Reading from the previous statement boundary to
    the next `;` puts the ImGui:: and the g_store. back in view.
    """
    start = max(text.rfind(";", 0, at), text.rfind("{", 0, at),
                text.rfind("}", 0, at)) + 1
    end = text.find(";", at)
    return text[start:end if end >= 0 else len(text)]


def is_not_a_consumer(line: str, field: str) -> bool:
    """True when this mention is the control, persistence or a mirror fill.

    These are what a setting looks like from the SETTINGS side, and none of
    them is the setting doing anything. Everything else counts.
    """
    # ASSIGNING TO THE FIELD IS NOT READING IT. `s.hud_radar_x = 169;` is the
    # reset-to-defaults button writing a value in, and it cleared three of the
    # arena HUD settings as though something consumed them.
    if re.search(r"\.\s*" + re.escape(field) + r"\s*=(?!=)", line):
        return True
    # A NAMED LIST OF EDITING WIDGETS, not "any ImGui:: call". The broad
    # version hid `show_metrics`, whose consumer is
    # `ImGui::ShowMetricsWindow(&s.show_metrics)` - the window is the thing the
    # setting DOES, and it takes the flag by address so it can close itself.
    # That line contains "ImGui::" and "&s.field" and is not a control.
    widget = re.search(r"ImGui::(?:Checkbox|Slider\w+|Input\w+|Drag\w+|Combo"
                       r"|RadioButton|ColorEdit\w*|ListBox)\s*\(", line)
    # A CHAR ARRAY IS BOUND WITHOUT AN AMPERSAND, and requiring one let every
    # string setting clear itself with its own text box.
    # `ImGui::InputText("Player name", s.mp.player_name, sizeof ...)` passes a
    # decayed pointer, so the `&s.field` test never matched and the CONTROL was
    # accepted as the CONSUMER - for player_name, master_host and direct_host,
    # which is to say for the join-by-IP address and the player name, whose
    # default is fixed (Maclunkey).
    #
    # Widening to "the field appears at all" is safe because `widget` is a
    # NAMED list of editing widgets: the case the narrow rule was protecting -
    # `ImGui::ShowMetricsWindow(&s.show_metrics)`, where the window IS what the
    # setting does - is not in that list and never matches here.
    bound = re.search(r"&\s*\w+\.\s*" + re.escape(field), line) or \
        re.search(r"\w+\.\s*" + re.escape(field) + r"\b", line)
    if widget and bound:
        return True                                   # binds the control
    # GREYING OUT THE ROWS BELOW IS NOT ACTING ON THE SETTING. A master toggle
    # disables its dependants with `ImGui::BeginDisabled(!s.field)`, which is
    # the panel talking about itself. It cleared enhanced_controls,
    # bh_freeplay_select and arena_hud_overlay - three settings whose subject is
    # the GAME, not the panel - as though something consumed them.
    #
    # show_metrics is the deliberate contrast: its subject IS the panel, so
    # ImGui::ShowMetricsWindow really is the whole of what it does.
    if re.search(r"ImGui::(?:BeginDisabled|EndDisabled)\s*\(", line):
        return True                                   # panel bookkeeping
    # A RULE THAT WAS TRIED AND REMOVED, because the distinction it needed does
    # not exist in the source. "The setting only prints a note about itself"
    # would have been worth catching - `if (s.arena_hud_overlay) {
    # ImGui::TextDisabled("The floating touch panel...")` is prose, and that
    # setting's real consumer is arenahud.cpp asking
    # lsw2_ui_arena_hud_enabled(). But `if (!s.coop_show_room_code) {
    # ImGui::TextDisabled("Hidden."); } else { ImGui::Text("Room code: %s") }`
    # is the SAME SHAPE and is the whole of what that setting does: the design
    # puts the baseline's room code in this panel behind a checkbox that starts
    # unticked. Excluding the shape reported a correctly-wired setting, which is
    # the failure that gets a sweep switched off.
    #
    # The strength is recovered by ORDER instead - main() asks the mirror and
    # the struct method first, so a setting with a real consumer is cleared for
    # the real reason and only a setting with nothing else falls back to a
    # direct mention.
    if "g_store." in line:
        return True                                   # persistence
    # WRITING INTO A STRING SETTING IS NOT READING IT - the array form of the
    # `s.field = x` rule at the top. Loading a persisted string is
    # `std::snprintf(s.mp.direct_host, sizeof ..., "%s", h.c_str())`, and the
    # `g_store.` call that produced `h` is on the PREVIOUS statement, so the
    # persistence rule above cannot see it. The field being the DESTINATION -
    # the first argument - is what makes this a write.
    # `(?:\w+\s*\.\s*)*` and not `\w+\.` because the destination is nested:
    # these live in `UiSettings::mp`, so the argument is `s.mp.direct_host` -
    # two dotted levels, and a pattern allowing exactly one matched nothing.
    if re.search(r"(?:snprintf|strncpy|strcpy|memcpy)\s*\(\s*"
                 r"(?:\w+\s*\.\s*)*" + re.escape(field) + r"\b", line):
        return True                                   # a write, not a read
    if re.search(r"g_\w+\s*=\s*\w+\.\s*" + re.escape(field) + r"\b", line):
        return True                                   # fills a mirror
    return False


def mirrors_of(ui_text: str) -> dict[str, set[str]]:
    """field -> the file-statics ui.cpp copies it into.

    Read in the direction the code writes it, so a mirror whose name differs
    from the field's - g_fork_hunters_only for bh_roster_only - is carried
    rather than lost.
    """
    out: dict[str, set[str]] = {}
    for m in re.finditer(r"\bg_(\w+)\s*=\s*\w+\.(\w+)\s*;", ui_text):
        out.setdefault(m.group(2), set()).add("g_" + m.group(1))
    return out


def accessors_of(ui_text: str) -> dict[str, set[str]]:
    """mirror -> the ui.cpp functions that read it.

    ANY read counts, not just `return g_x;`. lsw2_ui_hud_radar writes three
    mirrors out through pointers and is every bit as much their accessor.
    """
    bounds = []
    for m in FUNC.finditer(ui_text):
        if m.group(1) not in KEYWORDS:
            bounds.append((m.end(), m.group(1)))
    bounds.append((len(ui_text), None))

    out: dict[str, set[str]] = {}
    # ANCHORED AT THE MATCH, NOT A FIXED WINDOW. This read
    # `ui_text[m.end():m.end() + 4]`, and four characters is not enough for an
    # ALIGNED assignment: ui_load_settings writes
    # `g_enhanced_controls    = s.enhanced_controls;` with the `=` padded out
    # to a column, so the `=` fell outside the slice and the write was counted
    # as a READ. That made ui_load_settings an "accessor" of nearly every
    # mirror, and since it is always called, the strongest-evidence branch
    # cleared those settings before anything else was asked. Breaking a real
    # consumer then changed nothing - which is exactly what a probe found.
    assign = re.compile(r"\s*=(?!=)")
    for m in re.finditer(r"\b(g_\w+)\b", ui_text):
        # An assignment to the mirror is not a read of it.
        if assign.match(ui_text, m.end()):
            continue
        fn = None
        for i in range(len(bounds) - 1):
            if bounds[i][0] <= m.start() < bounds[i + 1][0]:
                fn = bounds[i][1]
                break
        if fn:
            out.setdefault(m.group(1), set()).add(fn)
    return out


def struct_methods(text: str, field: str) -> set[str]:
    """Methods of the settings struct whose body reads `field`."""
    found = set()
    for m in re.finditer(r"\b(\w+)::(\w+)\s*\([^;]*?\)\s*(?:const\s*)?\{",
                         text):
        start = m.end()
        depth, i = 1, start
        while i < len(text) and depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        if re.search(r"\b" + re.escape(field) + r"\b", text[start:i]):
            found.add(m.group(2))
    return found


def called(name: str, blob: str) -> bool:
    """A call to `name`, not its definition or declaration."""
    proto = re.compile(
        r"^\s*(?:static|extern|inline|virtual)?\s*[\w:<>,&*]+[\w:<>,&*\s]*\s+"
        + re.escape(name) + r"\s*\([^;{]*\)\s*(?:const\s*)?;\s*$")
    for m in re.finditer(r"(^|[^\w:])" + re.escape(name) + r"\s*\(", blob,
                         re.M):
        tail = blob[m.end():m.end() + 300]
        # A CONST MEMBER'S DEFINITION HAS WORDS BEFORE ITS BRACE, so
        # `int Lsw2HostSettings::lobby_seconds() const {` is not caught by a
        # bare startswith("{") and counts as a call to itself. It does not bite
        # today only because hostsettings.cpp is excluded whole and so is not in
        # the blob - which is luck, not design, and the struct-method route is
        # exactly the one that would silently self-clear if that exclusion ever
        # changed. Same fix as sweep_test_only.py, where it did bite.
        after = tail[len(tail.split(")")[0]) + 1:].lstrip()
        after = re.sub(r"^(?:const|noexcept|override|final)\s*", "", after)
        after = re.sub(r"^(?:const|noexcept|override|final)\s*", "", after)
        if after.startswith("{"):
            continue
        start = blob.rfind("\n", 0, m.start()) + 1
        end = blob.find("\n", m.start())
        if proto.match(blob[start:end if end >= 0 else len(blob)]):
            continue
        return True
    return False


def main() -> int:
    # THREE STRUCTS, AND THE THIRD WAS ONCE MISSING.
    #
    # `UiMultiplayer` sits in the same header as `UiSettings`, twenty lines
    # above it, and this sweep had never looked at it. Six settings, every one
    # a required multiplayer feature - the player name (which defaults to
    # Maclunkey), the master server host and port, the listen port, and the
    # direct-connect address that "Join by IP/Room Code" is built on.
    #
    # All six turn out to be properly consumed: they reach
    # Lsw2ForkSession::configure and ::configure_direct, and direct_host_ ends
    # up at join_address(). So this found no dead setting. What it found is
    # that deleting those calls would have broken a required feature in silence,
    # because nothing was watching the struct at all.
    #
    # Same lesson as the address-versus-value sweep and the symbol-versus-guard
    # clauses: the shape of a scan decides what it cannot see, and "all 49
    # settings have a consumer" was answering a smaller question than it looked.
    targets = [(FRONTEND / "hostsettings.h", "Lsw2HostSettings"),
               (FRONTEND / "ui.h", "UiSettings"),
               (FRONTEND / "ui.h", "UiMultiplayer")]

    sources: dict[str, str] = {}
    for path in sorted(FRONTEND.glob("*.cpp")) + sorted(FRONTEND.glob("*.h")):
        if path.name in NOT_A_CONSUMER or path.name.startswith("test_"):
            continue
        sources[path.name] = strip_comments(
            path.read_text(encoding="utf-8", errors="replace"))

    ui_text = sources.get("ui.cpp", "")
    mirrors = mirrors_of(ui_text)
    accessors = accessors_of(ui_text)
    blob = "\n".join(sources.values())

    # The struct's own methods live in an excluded file - that file is the menu
    # and the codec - but a method that READS a field is a consumer of it, and
    # the calls to that method are out here.
    hs_text = strip_comments(
        (FRONTEND / "hostsettings.cpp").read_text(encoding="utf-8",
                                                  errors="replace"))

    bad, checked, pending_seen = [], 0, set()
    for path, struct in targets:
        fields = struct_fields(path, struct)
        if not fields:
            bad.append(f"{struct} not found in {path.name} - renamed?")
            continue
        for field in fields:
            # A C++ KEYWORD IS NEVER A FIELD NAME, so seeing one means FIELD has
            # started matching something that is not a data member.
            #
            # This used to read `if field == "const": continue` - a silent skip
            # for the two phantoms `int lobby_seconds() const;` produced. That
            # hid the real fault (the pattern crossed a `(`) and hid it in the
            # direction that PASSES: a phantom field named `const` finds a
            # "consumer" in every file containing the word, so it cleared
            # itself and inflated the total. The pattern is fixed, so this
            # should never fire - and if it does it must be LOUD, because the
            # same breakage could swallow a real setting just as easily.
            if field in ("const", "constexpr", "static", "inline", "return"):
                bad.append(f"{struct}::{field} is a keyword, not a field - "
                           f"the FIELD pattern is matching declarations again")
                continue
            checked += 1

            # STRONGEST EVIDENCE FIRST. An accessor on a mirror, or a method on
            # the struct, is unambiguous: something outside the settings code
            # asked for this value. A direct mention is weaker - the statement
            # rules below can tell a control and a persist and a mirror-fill
            # from a use, but not a use from a note about the setting - so it
            # is the fallback rather than the first answer. Asking in this
            # order means a setting is cleared for the best reason it has,
            # which is what makes the audit of this sweep readable.
            found = False
            for mirror in mirrors.get(field, set()):
                if any(called(fn, blob) for fn in
                       accessors.get(mirror, set())):
                    found = True
                    break

            if not found:
                for method in struct_methods(hs_text, field):
                    if called(method, blob):
                        found = True
                        break

            if not found:
                for src in sources.values():
                    for m in re.finditer(r"\.\s*" + re.escape(field) + r"\b",
                                         src):
                        if not is_not_a_consumer(
                                statement_around(src, m.start()), field):
                            found = True
                            break
                    if found:
                        break

            if found:
                continue
            if field in PENDING:
                pending_seen.add(field)
                continue
            bad.append(f"{struct}::{field} has no consumer")

    for stale in sorted(set(PENDING) - pending_seen):
        bad.append(f"PENDING lists {stale!r}, which now has a consumer or is "
                   f"no longer a setting - take it off the list")

    if bad:
        print("settings nothing acts on:")
        for line in bad:
            print(f"  {line}")
        print("\n  A setting that is stored, persisted and transmitted with no"
              "\n  consumer reads as finished from every direction except the"
              "\n  only one that matters.")
        return 1

    print(f"all {checked} settings have a consumer; "
          f"{len(pending_seen)} pending, each owing a named clause")
    return 0


if __name__ == "__main__":
    sys.exit(main())
