"""Each feature area names its parts; each part names a symbol; is it wired?

WHY THIS EXISTS. Every ledger in this project has carried a percentage per
feature area, and those percentages were repeatedly WRONG - not by a little.
One round of live testing found six features scored as built that did not work,
and the two most embarrassing were not even reachable: the pause menu's
corrections sat behind a condition that could never be true, and the whole
multiplayer flow sat behind a console entity that refused to open.

The percentages are a judgement and will stay one. But the *floor* under them
need not be: "the code for this exists AND something calls it" is mechanically
checkable, and that is exactly the property the six broken features lacked.

So this maps each feature's sub-clauses to the symbol that implements them and
asserts the symbol is defined and reached. It cannot tell you a feature WORKS -
only that it is not obviously absent, which is a lower bar and an honest one.

AND IT NOW CHECKS WHAT THAT SENTENCE SAYS. An earlier version counted MENTIONS
and failed below three, on the reasoning "declaration + definition = 2; anything
live has a caller as well". That is a heuristic, and it passed a deliberately
broken tree: renaming a definition left the declaration and three call sites
standing, so the count was four and the gate was satisfied by a symbol that no
longer existed. The docstring claimed "defined and reached" and the code checked
neither. Definitions and calls are now counted separately, prototypes excluded
from both, and each branch is negative-tested.

    python tools/sweep_features.py

Exit 1 if any listed symbol is missing or uncalled.

WHAT IT DOES NOT DO. It does not check behaviour, correctness, or that the code
runs at the right moment - the pause menu bug would have PASSED this check,
because the function existed and was called from a place that never executed.
`sweep_lr_checks.py` is what catches that shape. This one catches deletion and
never-wiring.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = [ROOT / "port" / "frontend", ROOT / "port" / "net"]

# (feature area, what the clause provides, the symbol that implements it)
CLAUSES = [
    ("boot", "warp to the arena lobby", "lsw2_cheats_warp"),
    ("boot", "party of one", "lsw2_cheats_remove_party_member"),
    ("boot", "the exit door is disabled", "lsw2_cheats_pin_level"),
    ("boot", "...and its warp-back backstop", "lsw2_lobbypin_should_warp_back"),
    ("boot", "Greedo or the last-played hunter", "lsw2_last_hunter_apply"),
    # ADDED AFTER THE FACT. The boot warp has SIX sub-parts and only five were
    # mapped: nothing asserted that the boot sequence's splash titles and the
    # save select are skipped. It is implemented - lsw2_fork_hiding_boot() gates
    # the fork's own screen over the boot in three places in frontend.cpp -
    # but deleting it would have passed. Found by counting the feature's
    # clauses against its mappings.
    ("boot", "the splash and save select are hidden", "lsw2_fork_hiding_boot"),
    ("unlocks", "unlock characters, parts and extras", "lsw2_cheats_unlock_everything"),
    ("pause-menu", "hidden pause rows stay hidden", "lsw2_pause_row_hidden"),
    ("pause-menu", "...and the survivors are re-centred", "lsw2_pause_row_y"),
    ("pause-menu", "...and the cursor skips them", "lsw2_pause_escape"),
    ("pause-menu", "host settings persist", "lsw2_host_settings_clamp"),
    ("customization", "customization is filtered by the host's policy",
     "lsw2_customization_filter"),
    ("customization", "...and re-applied through the game's own gate",
     "lsw2_cheats_request_appearance"),
    ("customization", "peers' customization is shown", "lsw2_peerparts_tick"),
    # ADDED AFTER THE FACT. The customization feature has around ten sub-parts
    # and three were mapped. Most glaring: nothing asserted the DRESSING - the
    # half that puts the parts on the character - which had just shipped. The
    # page restriction and the persistence across restarts were unmapped too.
    ("customization", "the actor is actually dressed", "lsw2_gearapply_tick"),
    ("customization", "the settings survive a restart", "lsw2_customization_load"),
    ("multiplayer", "the arena menu is relabelled", "lsw2_arena_menu_build"),
    # The heads, portrait and ring-colour features were added later and at
    # first had no mappings at all - three whole features that could have been
    # deleted silently. The gate only covers what someone lists in it, which is
    # its standing limitation and the reason this audit exists.
    ("heads", "heads survive a customize hat", "lsw2_headfix_after_apply"),
    ("portrait", "the portrait follows a Free Play switch", "lsw2_partyportrait_tick"),
    ("ring-colour", "the ring takes the player's slot colour", "lsw2_slotcolour_tick"),
    ("multiplayer", "the ten-row host menu", "lsw2_host_menu_build"),
    ("multiplayer", "the Extras submenu", "lsw2_extras_menu_build"),
    ("multiplayer", "...and the Extras actually take effect", "lsw2_cheats_set_extras"),
    ("multiplayer", "the console's six options", "lsw2_console_menu_build"),
    # ADDED when the menus were found not to respond. Building a menu and
    # ACTING on it are different clauses and only the first was asserted,
    # which is how on_menu_event came to be called from tests alone.
    ("multiplayer", "...and the menus act on a press", "on_menu_event"),
    ("multiplayer", "the kick list", "lsw2_kick_menu_build"),
    ("multiplayer", "the master server browser", "lsw2_master_list_set"),
    # ADDED AFTER THE FACT. The networking feature has six capabilities and
    # only the master list had a mapping. All five of these are implemented -
    # port/net/ has the files - but nothing asserted them, so any one could
    # have been deleted and the gate would still have said networking was
    # reached. Found by enumerating the feature's clauses against the list.
    ("multiplayer", "direct-IP connections", "Lsw2TcpTransport"),
    ("multiplayer", "desync-free lockstep", "Lsw2LockstepSession"),
    ("multiplayer", "miniupnpc port forwarding", "lsw2_upnp_map_port"),
    ("multiplayer", "LAN games are announced", "Lsw2LanBeacon"),
    ("multiplayer", "...and found", "Lsw2LanBrowser"),
    ("multiplayer", "the room code screen", "lsw2_join_menu_build"),
    # A CLASS, not a free function, so the type name is the symbol. Two of the
    # names in this list were guessed wrong on the first run - the tool doing its
    # job on its own author. A clause list is only as good as its symbols.
    ("multiplayer", "the lobby countdown", "Lsw2LobbyTimer"),
    ("multiplayer", "the round start", "lsw2_netmsg_start_round"),
    ("multiplayer", "non-Hunters skips the statue select", "lsw2_selectskip_should"),
    ("multiplayer", "the 15s score screen", "lsw2_score_should_press"),
    ("multiplayer", "join and disconnect popups", "lsw2_raise_join_popup"),
    # ADDED when Music: RANDOM stopped being pending. The set of tracks came
    # out of the ROM (0x020FF674's per-level records, byte +1) rather than
    # from listening to the running game, which is what had blocked it.
    ("multiplayer", "Music: RANDOM picks a track", "lsw2_music_pick"),
    ("multiplayer", "...and applies it on arena arrival", "lsw2_music_tick"),
    # ADDED TOGETHER WITH THE CORRECTION THAT REQUIRED THEM. The host's
    # character picker is the cartridge's own Free Play touch-screen menu,
    # driven by that menu's normal controls and logic. So the host choice is
    # not a button the fork invents - it is a read of the widget the cartridge
    # already built, and these two are the read.
    #
    # BOTH, NOT JUST THE SECOND. lsw2_pick_widget_choice on its own would go on
    # returning -1 forever if nothing ever recorded the pointer, and -1 is a
    # NORMAL answer meaning "not chosen yet" - it would not look broken. The
    # capture is the half that fails silently, so it is the half that most
    # needs a clause of its own.
    #
    # REPOINTED (freeplaypick.h). The read is now the widget's own unlock query
    # on the CONFIRMING tap - freeplaypick.cpp's override of 0x02086534, a
    # dispatch-table entry and so not a call this sweep can see - and it
    # reaches the session through lsw2_freeplaypick_host_take, the call pinned
    # here. lsw2_pick_widget_choice read the highlight instead, took every
    # browsing tap as the host's decision, and is deleted.
    ("multiplayer", "the host's pick is read off the cartridge's own select widget",
     "lsw2_freeplaypick_host_take"),
    ("multiplayer", "...and that widget's pointer is captured when the game builds it",
     "lsw2_selectwidget_note"),
    # THE RANGE IS THE OTHER HALF OF THAT PICK, and it was missing entirely.
    # The customization clamp was written once, at the Free Play constructor's
    # return, and never again - so by the time the host opened the Character
    # row the widget was already cut to pages 6 and 7 and R5-D4 (page 8) and
    # Gonk Droid (page 10), the two named examples of unlikely picks, could not
    # be reached. ui.cpp's hunters-only stand-down passed every gate while
    # changing nothing, because nothing re-read it.
    ("multiplayer", "...on a widget whose page range is maintained, not set once",
     "lsw2_selectwidget_set_pages"),
    ("customization", "...and one policy decides that range for both writers",
     "lsw2_hle_fork_select_range"),
    # NOT A LEVER, AND SAID SO. This clause used to read "the player count the
    # message layer needs" against `lsw2_netcount_tick`, and it passed for days
    # while writing a byte the pump does not read and that the game recomputes
    # anyway. The networking feature's enabling mechanism is UNIMPLEMENTED;
    # what exists is the measurement of the value it would have to produce, and
    # that is what this clause now claims. A sweep that asserts more than the
    # code does is worse than no sweep.
    ("multiplayer", "the view mask the message layer derives from is read",
     "lsw2_viewmask_guest"),
    ("multiplayer", "...and the connection layer's report is published",
     "lsw2_viewmask_publish"),
    ("hud", "the match HUD", "lsw2_arena_hud_composite"),
    ("hud", "...and the touch screen is hidden", "lsw2_arena_hud_active"),
    ("controls", "non-tank arena movement", "lsw2_enhanced_view_write"),
    ("controls", "the client's Off beats the host's Enhanced",
     "lsw2_enhanced_effective"),
    ("controls", "the camera yaw is reachable", "lsw2_enhanced_camera_yaw"),
    # ADDED AFTER THE FACT. The enhanced controls apply in arena gameplay only
    # (the lobby keeps the usual controls), and that holds for BOTH halves,
    # but only the movement half was gated on it. The camera half used "the
    # view manager resolves" as a proxy for "in a match" - and the recordings
    # refute that: 31 of 149 lobby frames have a live manager, because it is
    # not torn down when a match ends. So the right stick turned the camera in
    # the lobby, and no gate could see it. The predicate is what the clause is
    # about, so the predicate is what gets asserted.
    ("controls", "...and only on an arena map", "lsw2_cheats_in_arena_match"),
    # ADDED the same day it shipped - the right stick's camera step went in
    # without a mapping, which is how the networking feature got to be five
    # unasserted clauses.
    ("controls", "the right stick turns the camera", "lsw2_enhanced_camera_step"),
    # CORRECTED. This said `lsw2_host_extras_mask`, which computes an Extras
    # bitmask for the settings file and has nothing to do with advertising a
    # lobby kind - so the co-op announce clause would have passed with
    # set_coop deleted, which is precisely the deletion this gate exists to
    # catch. Found by hand-checking the clauses with the least evidence behind
    # them rather than by the gate itself.
    ("coop-announce", "co-op advertises its own kind", "set_coop"),
    ("coop-list", "the cartridge's own lobby list", "lsw2_coopjoin_publish"),
    ("coop-list", "...and its rows", "lsw2_coophosts_rows"),
    # ADDED AFTER THE FACT. The baseline's co-op lobby list shows LAN entries
    # at the top in green, and that was implemented and tested (masterlist.cpp
    # sorts on lan_first and colours with kLsw2MenuGreen, test_masterlist
    # asserts both) but never mapped here - so deleting either half would have
    # passed. Found by counting the clause's sub-parts against its mappings
    # rather than by the gate.
    ("coop-list", "LAN lobbies first and in green", "lsw2_master_list_set"),
]


# Clauses whose implementation is a GUARD rather than a function, so the symbol
# test cannot see them: (feature area, clause, file, regex that must match, why).
#
# THE CHEATS TAB IS THE REASON THIS EXISTS. In the Bounty Hunter fork the
# cheats menu is hidden from the ImGui panel and cannot be operated. That is
# implemented, and implemented well - the tab is absent rather than greyed -
# but it is an ABSENCE, and an absence has no symbol to point at. So it sat at
# ZERO clauses while being among the easiest features to break: deleting two
# characters would have put the cheats tab back into the fork, and every gate
# in this repo would still have passed.
#
# The same is true of the fork executable, whose whole implementation is a `cp`
# in build.sh.
GUARDS = [
    ("no-cheats", "the fork has no cheats tab", "port/frontend/ui.cpp",
     r"!\s*g_fork_build\s*&&\s*ImGui::BeginTabItem\(\"Cheats\"",
     "the tab must be ABSENT in the fork, not merely disabled"),
    ("build", "the fork exe is built beside the baseline", "port/build.sh",
     r"lsw2_bountyhunter\.exe",
     "the file name is fixed: it sits beside lsw2.exe on the same assets"),

    # THE MASTER SERVER'S FEATURES LIVE OUTSIDE THIS GATE'S REACH ENTIRELY.
    # SRC is port/frontend and port/net, and the master server is Python under
    # services/ - so the features describing it had ZERO clauses, the same
    # position the fork executable and the cheats tab were in. They are
    # implemented; nothing was watching.
    #
    # test_master_gui.py does not cover this ground either, and says so in its
    # own docstring: "not the layout - nobody can assert a window". It tests
    # the tree model, which is the right thing for it to test and leaves the
    # GUI's structural promises (the right-click menu, the coloured log)
    # unasserted.
    # THE PAUSE MENU'S HIDDEN ROWS MUST BE BOTH INOPERABLE AND HIDDEN, AND
    # THOSE ARE TWO PROMISES.
    #
    # Three clauses cover HIDDEN - lsw2_pause_row_hidden, lsw2_pause_row_y and
    # lsw2_pause_escape - and between them a hidden row is not drawn, does not
    # take a cursor stop, and does not leave a gap. None of them makes it
    # INOPERABLE: the row's hit-test record is still live, so it can still be
    # TAPPED.
    #
    # dispatch_overrides.cpp closes that by clearing each hidden row's enabled
    # byte at `+0x01` of its record. Delete that loop and the three rows stay
    # invisible and stay tappable - a player opens Extras from a row that is
    # not there. pauselayout.h records that this exact failure came back TWICE
    # before the touch path was closed, which is why "the cursor nudge alone
    # was not enough" is written above the loop.
    #
    # No symbol to point at: it is four lines inside the pause override.
    ("pause-menu", "the hidden rows are inoperable, not merely invisible",
     "port/frontend/dispatch_overrides.cpp",
     r"for \(uint32_t row : kLsw2PauseHiddenRows\)\s*\{[\s\S]{0,200}?"
     r"write_guest_byte\(rec \+ kPauseRowEnabled, 0\)",
     "hidden is not inoperable - without this the rows are untouchable to the "
     "eye and still live to the stylus"),

    # THE HOST PICKER MUST REACH EVERY CHARACTER, AND THE LOBBY CUSTOMIZATION
    # NARROWS THE SAME SCREEN TO TWO PAGES.
    #
    #   host picker:    the host picks the match's single character from every
    #                   playable one, R5-D4 and Gonk Droid included
    #   lobby select:   the Free Play select shows only the two Bounty Hunter
    #                   tabs
    #
    # The picker is on the Free Play touch-screen menu, so these are two
    # features wanting opposite things from ONE widget.
    # lsw2_hle_fork_hunters_only breaks the tie by who is asking: it returns
    # false while the session is on the Character screen.
    #
    # characterpick.h used to argue this conflict away by claiming the picker
    # never touched that widget. It does, so the tie-break is load-bearing:
    # remove it and the host picking "One" can choose between Greedo and the
    # other hunters and nothing else, which is the exact case R5-D4 and Gonk
    # Droid are named to rule out.
    ("multiplayer", "the host picker outranks the lobby's hunters-only clamp",
     "port/frontend/ui.cpp",
     r"screen\(\)\s*==\s*Lsw2ForkScreen::Character\s*\)\s*\n\s*return false;",
     "without it the lobby's two-page clamp follows the host into the picker "
     "and 'any playable character' becomes 'any of sixteen'"),

    # THE READ HAS TO LAND SOMEWHERE. lsw2_pick_widget_choice being reachable
    # says a caller exists; it does not say the caller USES the answer. Delete
    # the assignment below and keep the call, and sweep_features, sweep_orphans,
    # sweep_exe_reachable and sweep_call_reachable all stay green while the
    # host's tap changes nothing - the same shape of failure as the netcount
    # clause above, which passed for days while writing a byte nothing read.
    #
    # PINNED TO settings().character SPECIFICALLY, not to any assignment: that
    # setting is what the lobby broadcasts and what the round starts with, so
    # naming it is what makes this a claim about the host's pick rather than
    # about fork.cpp.
    #
    # REPINNED (freeplaypick.h). The pick is no longer the widget's highlight,
    # which took every browsing tap as the host's decision: it is the
    # confirming double tap, caught by the override of the unlock query, and it
    # lands through finish_character_pick - so the claim is now two pins, the
    # hand-over in fork.cpp and the write in forksession.cpp.
    ("multiplayer", "...and what it reads becomes the session's character setting",
     "port/frontend/fork.cpp",
     r"lsw2_fork_session\(\)\.finish_character_pick\(picked\);",
     "the grid's confirmed pick would be a result nothing acts on, and the "
     "host's double tap on the free play grid would never reach the match"),
    ("multiplayer", "...and the hand-over writes the session's character setting",
     "port/frontend/forksession.cpp",
     r"if \(character >= 0\) settings_\.character = character;",
     "the host's confirmed pick would close the picker and change nothing"),

    # AND ONLY ON THAT SCREEN. The lobby customization puts this same widget on
    # the lobby's touch screen for every player to pick their own hunter with,
    # so a picker that is not gated on the host being on the Character screen
    # would take a joining player's browsing as the host's setting.
    ("multiplayer", "...and only while the host is on the Character screen",
     "port/frontend/fork.cpp",
     r"screen\(\)\s*==\s*Lsw2ForkScreen::Character\s*\)\s*\{\s*\n"
     r"\s*if \(!lsw2_freeplaypick_host_active\(\)\) "
     r"lsw2_freeplaypick_host_begin\(\);",
     "any tap on the lobby's Free Play select would rewrite the match's character"),

    # THE LOBBY SELECT'S TWO UNGUARDED PROMISES, both in its Free Play half and
    # neither expressible as a symbol.
    #
    # A flag chooses whether the lobby select shows all characters or only the
    # Bounty Hunters, and it DEFAULTS TO ONLY THE BOUNTY HUNTERS.
    #
    # A stated default, the same shape as the enhanced controls', the
    # portrait fix's and the player name's Maclunkey. Flipping it would open
    # the whole roster in the lobby, which is the opposite of what the feature
    # is for, and sweep_default_claims would only object if the comment
    # disagreed.
    ("customization", "the roster flag defaults to hunters-only",
     "port/frontend/ui.h",
     r"bool bh_roster_only\s*=\s*true\s*;",
     "the default is fixed at hunters-only; false shows every character in the "
     "lobby picker"),

    # The restricted select is shown ONLY while in the Arena lobby map.
    #
    # A predicate, like the enhanced controls' arena gate - and the same failure
    # if it goes: the clamp would follow the Free Play widget into story levels,
    # where it does not belong and where two pages is simply wrong. It is one
    # term of `forced` in dispatch_overrides.cpp and nothing else asserts it.
    # ANCHORED ON `forced`, NOT ON THE PREDICATE ALONE. The bare
    # `lsw2_cheats_current_level() == kArenaLobbyLevel` appears FIVE times in
    # this file, so a guard on it would keep passing with the one that matters
    # deleted - it would be watching four other people's copies. Found because
    # the probe asserted its own match count before editing.
    ("customization", "...and the Free Play clamp is the arena lobby only",
     "port/frontend/dispatch_overrides.cpp",
     r"const bool forced =[\s\S]{0,240}?"
     r"lsw2_cheats_current_level\(\)\s*==\s*kLsw2ArenaLobbyLevel\s*;",
     "without this term in `forced` the two-page clamp follows the Free Play "
     "widget into story levels"),

    # AND NOW A SECOND COPY OF THAT PREDICATE HAS TO BE GUARDED, because the
    # clamp gained a second writer. fork.cpp maintains the range every frame
    # and does NOT go through `forced`; it asks lsw2_hle_fork_select_range,
    # whose own level test is the only thing keeping the per-frame writer out
    # of story levels. The guard above would stay green with this one deleted.
    #
    # ANCHORED ON THE FUNCTION HEAD. The bare `!=` form appears three times in
    # the port - twice in fork.cpp - so the predicate alone would be watching
    # somebody else's copy, which is the mistake the note above records.
    ("customization", "...including for the per-frame writer, which does not use `forced`",
     "port/frontend/ui.cpp",
     r"bool lsw2_hle_fork_select_range\([\s\S]{0,900}?"
     r"lsw2_cheats_current_level\(\)\s*!=\s*kLsw2ArenaLobbyLevel",
     "the per-frame range writer would clamp an ordinary Free Play select to "
     "two pages in any story level"),

    # THE HOST PICKER'S RANGE HAS TO BE APPLIED EVERY FRAME, not at the
    # picker's open. A one-shot widen when the host enters the Character row
    # would leave the widget wide when they left it, and the player's own
    # select would still be showing all twelve pages - the lobby clamp gone
    # with no gate noticing, since the constructor path would have run
    # correctly hours earlier.
    ("multiplayer", "...and the range is re-applied every frame, not once at open",
     "port/frontend/fork.cpp",
     r"lsw2_hle_fork_select_range\(&low, &high\)\s*&&\s*\n"
     r"\s*lsw2_selectwidget_set_pages\(low, high\)",
     "the widened range would stick after the host left the picker, or the "
     "narrowed one would never widen at all"),

    # THE JOIN-IN-PROGRESS RULE. A match already under way may be joined; a
    # player who arrives as a round begins is given, automatically, the
    # hunter they had in the lobby.
    #
    # lsw2_last_hunter_tick implements it, and the whole rule lives in one
    # condition. `g_saw_select` is the distinction the rule turns on: a player
    # who sat through the statue select CHOSE, and must not be overruled; a
    # player who arrived on an arena map without ever seeing that screen never
    # chose, and gets their lobby hunter.
    #
    # Dropping the `!g_saw_select` term is the dangerous edit - it inverts the
    # feature into one that overwrites every player's own pick a frame after
    # they make it, which is worse than not having it. `!g_applied_on_arena`
    # keeps it to once per arrival. Guarding the condition rather than the
    # function catches both.
    #
    # (An earlier audit recorded that this rule had "no code or comment". That
    # was wrong: it is in lasthunter_observe.cpp, and the search had looked in
    # roster.cpp. Checked before reporting an absence a second time.)
    ("multiplayer", "joining mid-round keeps the hunter picked in the lobby",
     "port/frontend/lasthunter_observe.cpp",
     r"lsw2_cheats_in_arena_match\(\)\s*&&\s*!g_saw_select\s*&&\s*"
     r"!g_applied_on_arena",
     "without !g_saw_select this overwrites the choice of every player who "
     "DID use the statue select"),

    # THE HOST MENU'S THREE ENUMERATED VALUE SETS, each fixed exactly:
    #
    #   Roster:         Hunters / ALL / Random Hunter / Random ALL / One
    #   Customization:  OFF / HATS / ALL
    #   Lobby Time:     OFF / 30s / 60s / 300s
    #
    # test_hostsettings asserts the DEFAULT of each row - Hunters, ALL, 30s -
    # and that Character hides and unhides as Roster passes One, which is real
    # coverage of the behaviour. What it cannot see is a LABEL: renaming
    # "Random Hunter" to "Rnd Hunter", or swapping "HATS" and "ALL", leaves
    # every one of those checks passing, because they read the value at index 0
    # or compare an enum. The labels are part of the design, so the strings are
    # the clause.
    #
    # Each guard spans the whole list in order, so a reorder fails as loudly as
    # a rename.
    ("multiplayer", "the Roster row's five values, in their fixed order",
     "port/frontend/hostsettings.cpp",
     r"roster\.values\[0\] = \"Hunters\";[\s\S]*?"
     r"roster\.values\[1\] = \"ALL\";[\s\S]*?"
     r"roster\.values\[2\] = \"Random Hunter\";[\s\S]*?"
     r"roster\.values\[3\] = \"Random ALL\";[\s\S]*?"
     r"roster\.values\[4\] = \"One\";",
     "the Roster set is fixed in full, and Roster != One is what "
     "skips the character-select phase"),
    ("multiplayer", "the Customization row's three values",
     "port/frontend/hostsettings.cpp",
     r"custom\.values\[0\] = \"OFF\";[\s\S]*?"
     r"custom\.values\[1\] = \"HATS\";[\s\S]*?"
     r"custom\.values\[2\] = \"ALL\";",
     "\"Customization: <OFF/HATS/ALL>\" - OFF and HATS mean different things "
     "to what a client may equip"),
    ("multiplayer", "the Lobby Time row's four values",
     "port/frontend/hostsettings.cpp",
     r"lobby\.values\[0\] = \"OFF\";[\s\S]*?"
     r"lobby\.values\[1\] = \"30s\";[\s\S]*?"
     r"lobby\.values\[2\] = \"60s\";[\s\S]*?"
     r"lobby\.values\[3\] = \"300s\";",
     "\"LOBBY TIME <OFF/30s/60s/300s>\" - OFF means the host starts manually, "
     "so the first entry is not a duration at all"),

    # THE PAUSE-MENU RENAME, which is two exact strings and a host/client split
    # in one line. In multiplayer, the pause menu's "Quit Game" becomes
    # "Disconnect" - once confirmed, it takes the player back to the lobby,
    # offline; for the host it reads "Shut Down Lobby" instead.
    #
    # pause_quit_relabel() does it and is called from the tick, so the symbol
    # is reached - but a symbol clause would not notice the two labels being
    # swapped, or the host test being dropped so that everyone reads
    # "Disconnect". Both are the kind of change that looks harmless in a diff
    # and hands a host a row that says the wrong thing about their own lobby.
    ("multiplayer", "Quit Game becomes Disconnect, or Shut Down Lobby for the host",
     "port/frontend/dispatch_overrides.cpp",
     r"session\.is_host\(\)\s*\?\s*\"Shut Down Lobby\"\s*:\s*\"Disconnect\"",
     "both labels are fixed, and so is which side each belongs to"),

    # THE MULTIPLAYER FEATURE'S ONE FIXED DEFAULT: the player's name is set in
    # a Multiplayer tab of the ImGui menu and DEFAULTS TO "Maclunkey".
    #
    # THREE SITES CARRY IT AND ALL THREE MUST AGREE:
    #
    #   ui.h              the ImGui field the player actually edits
    #   forksession.h     the session's own default, for a session configured
    #                     before the shell has set anything
    #   forksession.cpp   the sanitiser's fallback when a name comes back empty
    #
    # They cannot share one constant without either inverting the ui.h ->
    # forksession.h dependency or giving up the `char[24]` member initialiser,
    # which cannot be initialised from another array. So each site is guarded
    # separately: if any one drifts, that one fires and the other two do not,
    # which also says WHICH copy moved.
    ("multiplayer", "the player name defaults to Maclunkey (the imgui field)",
     "port/frontend/ui.h",
     r"char player_name\[\d+\]\s*=\s*\"Maclunkey\"",
     "the player name's default is fixed as Maclunkey"),
    ("multiplayer", "...and the session's default agrees",
     "port/frontend/forksession.h",
     r"char player_name_\[\d+\]\s*=\s*\"Maclunkey\"",
     "a session configured before the shell would otherwise announce a "
     "different name than the tab shows"),
    ("multiplayer", "...and so does the sanitiser's fallback",
     "port/frontend/forksession.cpp",
     r"lsw2_sanitise_name\([^;]*\"Maclunkey\"\)",
     "an empty or fully-stripped name falls back here, and it goes to the "
     "master server and into the game's own room title"),

    # THE CO-OP ANNOUNCE'S SECOND HALF: co-op games can BROADCAST TO THE
    # MASTER SERVER.
    #
    # The first half - co-op games instantiated from the base recomp - is the
    # set_coop clause above: the baseline calls set_coop(!fork) from the same
    # place the build flag is set, so its lobbies announce themselves as `coop`
    # rather than inheriting the constructor's `bounty` default.
    #
    # The half that reaches the wire is this line, and dropping it is a
    # specific, named harm that forksession.cpp spells out: "a baseline co-op
    # lobby advertised as `bounty` would appear in the fork's browser and be
    # joined by players expecting an arena". An empty kind is the same failure
    # with the label missing instead of wrong.
    #
    # IT IS ASSERTED ELSEWHERE ONLY BY A TEST THAT DOES NOT RUN.
    # test_master_client.cpp has "kinds - arena and co-op share one directory",
    # which checks exactly this round-trip - and the suite reports it as
    # "master_client skipped (needs the master server; pass --with-server)".
    # A skipped test is not evidence, so the line gets a guard that runs.
    ("coop-announce", "...and the co-op kind reaches the announce body",
     "port/net/lsw2_master.cpp",
     r"body \+= \",\\\"kind\\\":\\\"\" \+ kind_",
     "a lobby that announces no kind is indistinguishable from a bounty "
     "lobby in the directory"),

    # TWO DEFAULTS THAT ARE FIXED OUTRIGHT.
    #
    # sweep_default_claims already checks that a comment saying "off by
    # default" does not sit over a `true` - but that is comment-against-value.
    # Nothing checked value-against-REQUIREMENT, so flipping either of these to
    # false would break a stated requirement in silence, with the comment
    # obligingly updated alongside it.
    #
    # Only the enhanced controls and the portrait fix are asserted here,
    # because only their defaults are fixed outright: the enhanced controls
    # default to on, and the portrait fix defaults to on in the base recomp
    # and in the fork alike. The heads and ring-colour defaults are this
    # project's own reasoning ("for the same reason the enhanced controls'
    # is"), which is sound but is not a stated requirement, and a gate should
    # not claim more than was required.
    ("controls", "the enhanced control set defaults to on",
     "port/frontend/ui.h",
     r"bool enhanced_controls\s*=\s*true\s*;",
     "the enhanced controls default to on"),
    ("portrait", "the portrait fix defaults to on",
     "port/frontend/ui.h",
     r"bool enhanced_portrait\s*=\s*true\s*;",
     "the portrait fix defaults to on, in the base recomp and the fork alike"),

    # THE PORTRAIT FIX'S MECHANISM, which is specified unusually precisely:
    # a refresh path exists that a Free Play switch just never runs, so the
    # work is to work out which refresh a tag sets off and set that same
    # refresh off on the switch, INSTEAD OF PAINTING THE PORTRAIT IN THE PORT.
    #
    # So calling the cartridge's own party_refresh and redraw_page through
    # lsw2_guest_call is not an implementation detail, it is the clause. A
    # reimplementation that painted the portrait would satisfy the symptom and
    # violate the design, and no other check would notice - the same shape as
    # the ring-colour feature's pixel copy.
    # THE PAIR WAS REDUCED TO ONE CALL, AND ONLY BY MEASUREMENT.
    #
    # This used to require redraw_page as well. It cannot: called from the host
    # it stalls inside the ITCM DMA routine and never returns, which was
    # retaken after lsw2_guest_call was repaired so it is a fact about the
    # routine and not about a broken seam:
    #
    #     [guestcall] 02068D74 did not return: unwinding=1 r15=01FFD584
    #
    # and it stops half way through drawing, so the select page is left with
    # empty portrait circles and the guest is left unwinding, taking every
    # later host call down with it. On screen it showed as the portraits on
    # the bottom screen refreshing on a change instead of the top-screen one -
    # which also says what redraw_page actually is: the TOUCH screen's page
    # grid, not the top-screen portrait this feature is about.
    #
    # So the clause keeps the half that is both correct and possible - the
    # game's own party_refresh, through the guest seam - and the ban below
    # keeps the half that matters most, which is that the port must not paint
    # the portrait itself. Dropping the redraw_page requirement is not a
    # relaxation of the design; requiring a call that damages the game would
    # have been a misreading of it.
    ("portrait", "...by triggering the game's own refresh, not drawing it",
     "port/frontend/partyportrait.cpp",
     # UPDATED. The guard checked for the guest-call form, and that call NEVER
     # ONCE RETURNED - the machine's event queue was measured empty between
     # frames, so a function that waits for an interrupt cannot complete
     # there. The refresh now runs through the trampoline hop instead
     # (gearhop.cpp), which does complete. The CLAUSE is unchanged - the game's
     # own refresh path must still be triggered rather than the port drawing
     # the portrait - so the guard tracks the mechanism that satisfies it, not
     # the one that failed to.
     r"lsw2_gearhop_request_refresh\(g_widget",
     "the game's own refresh path must be triggered; the portrait must not "
     "be drawn by the port"),

    # THE RING-COLOUR FEATURE'S SECOND HALF, which has no symbol because it
    # has no code.
    #
    # The HUD's mirrored column must draw the remaining three players in their
    # respective colours as well - the HOST among them whenever someone else
    # is the POV player. That is satisfied by construction: the column is a PIXEL
    # COPY out of the sub engine's finished framebuffer, and the cartridge has
    # already drawn each row in its player's palette. The colours arrive
    # because they were never removed.
    #
    # So the clause to assert is the COPY. If the composite ever synthesises
    # portraits - its own boxes, or one row blitted and tinted - half of the
    # feature breaks and nothing about the column's layout would look wrong.
    # The feature had one clause for two halves; this is the other half.
    ("ring-colour", "...and the mirrored column keeps each player's own colour",
     "port/frontend/arenahud.cpp",
     r"blit_scaled\(touch,",
     "the column is copied from the game's own pixels, which already carry "
     "each row's palette - synthesising it would drop the colours silently"),

    # AND THE RING REWRITE MUST NOT EAT THAT COLUMN, which is the same half of
    # the feature seen from the writing side.
    #
    # slotcolour.cpp recolours any main-engine sprite wearing palette 4..7, on
    # the reasoning that the in-match top screen is "portrait plus hearts only"
    # so nothing else there can wear a player colour. Sampling every 25th of
    # out/work's 286,097 captured frames across all 85 streams: 5,332 frames
    # carry such a sprite and ALL 5,332 carry all four colours at once - four
    # 40x40 portrait cells in a column. Without this count the tick would
    # repaint three other players to the local player's colour AND destroy the
    # palette bank read_rows() identifies rows by, which is the guard above
    # failing from the other direction.
    #
    # ANCHORED ON THE DECLINE, not on the counting loop: a loop that computes
    # `distinct` and does not act on it is the failure being guarded against.
    ("ring-colour", "...and the ring rewrite declines when it is looking at a column",
     "port/frontend/slotcolour.cpp",
     r"if \(distinct > 1\)\s*\{[\s\S]{0,600}?return;\s*\n\s*\}",
     "the tick would repaint the other players' portrait cells and break the "
     "HUD's palette identification of the mirrored rows"),

    # AND BOTH OF THEM MUST ASK WHICH ENGINE, not assume one.
    #
    # nds_vram_renderer_oam() is indexed by ENGINE while POWCNT1 bit 15 decides
    # which engine drives which screen, and this game moves it - gpudump_arena's
    # frame has the slot column on the sub engine, gpudump_lobbywait/inst0
    # f025386 has it on the main one. A hardcoded index does not fail loudly: it
    # returns a valid OAM image for the OTHER screen, so every sprite decodes
    # and the caller reasons about the wrong picture. That is precisely how the
    # ring rewrite came to be looking at four players' portraits.
    #
    # TWO CLAUSES BECAUSE THEY ARE TWO FILES AND TWO DIRECTIONS - the ring
    # rewrite reads the top screen, the HUD reads the touch screen, and getting
    # either one wrong points it at the other's furniture.
    ("ring-colour", "...and it resolves which engine is on the top screen",
     "port/frontend/slotcolour.cpp",
     r"nds_vram_renderer_oam\(lsw2_screen_engine\(kLsw2ScreenTop\)\)",
     "a hardcoded engine index silently hands back the touch screen's OAM "
     "when the game swaps the displays"),

    ("hud", "the mirrored column resolves which engine is on the touch screen",
     "port/frontend/arenahud.cpp",
     r"nds_vram_renderer_oam\(lsw2_screen_engine\(kLsw2ScreenTouch\)\)",
     "read_rows would identify players from the top screen's sprites when the "
     "game swaps the displays"),

    # AND THE STARFIELD SUPPRESSION, which is the same bug with a picture change
    # behind it. Engine B is the touch screen only while POWCNT1 says so; with
    # the engines swapped, hiding the starfield layers on engine B takes them off
    # the screen the player is looking at and leaves the stars in the pixels
    # being mirrored. The suppression is now patch 0016's render-time mask
    # (hiddenbgs.h) rather than a DISPCNT poke - the poke only ever cleared
    # BG2, and BG3 carries stars too - and the clause follows it: the mask must
    # be aimed at the engine resolved for the touch screen.
    ("hud", "...and the starfield suppression is aimed at the touch screen's own engine",
     "port/frontend/arenahud.cpp",
     r"const int engine = lsw2_screen_engine\(kLsw2ScreenTouch\);\s*"
     r"lsw2_hidden_bgs_set\(Lsw2HiddenBgOwner::ArenaStarfield, engine,",
     "a hardcoded engine would hide backgrounds on the visible screen and leave "
     "the starfield in the mirror"),

    ("master-server", "two flavours of executable",
     "services/master-server-selfhost/build.py",
     r"\"name\":\s*\"lsw2-master\"[\s\S]{0,400}\"name\":\s*\"lsw2-master-gui\"",
     "both are required: a command-prompt instance and a GUI"),
    ("master-server", "kick and ban from a right-click menu",
     "services/master-server-selfhost/master_server_gui.py",
     r"bind\(\"<Button-3>\"",
     "any server or player row offers a right-click menu"),
    ("master-server", "...with both of the options it names",
     "services/master-server-selfhost/master_server_gui.py",
     r"add_command\(label=\"Kick[\s\S]*add_command\(label=\"Ban IP",
     "the menu offers 'Kick' and 'Ban IP' specifically"),
    ("master-server", "a verbose console log, coloured by level",
     "services/master-server-selfhost/master_server_gui.py",
     r"eventlog\.DEBUG:[\s\S]{0,200}eventlog\.ERROR:",
     "the console log is fairly verbose - debug, startup, port and IP "
     "information - and the levels are how verbose is made readable"),
]


def check_guards() -> list[str]:
    """Guard-shaped clauses: the code is a condition or a build step, not a
    symbol, so `defined and called` cannot be asked of it."""
    out = []
    for point, what, rel, pattern, why in GUARDS:
        path = ROOT / rel
        if not path.is_file():
            out.append(f"{point}: {what} - {rel} is missing entirely")
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if not re.search(pattern, text):
            out.append(f"{point}: {what} - the guard is gone from {rel} ({why})")
    return out


def main() -> int:
    bodies = {}
    for d in SRC:
        if not d.is_dir():
            continue
        for p in sorted(list(d.glob("*.cpp")) + list(d.glob("*.h"))):
            if p.name.startswith("test_"):
                continue
            t = p.read_text(encoding="utf-8", errors="replace")
            bodies[p.name] = re.sub(r"/\*.*?\*/", "", t, flags=re.S)

    bad = []
    for point, what, sym in CLAUSES:
        # A DEFINITION AND A CALL, SEPARATELY - not a mention count.
        #
        # This used to fail only when a symbol appeared fewer than three times,
        # on the reasoning "declaration + definition = 2; anything live has a
        # caller as well". That is a heuristic, and it once passed a
        # deliberately broken tree: renaming lsw2_fork_hiding_boot's DEFINITION
        # left its three call sites and its declaration standing, so the count
        # was four and the gate was satisfied by a symbol that no longer
        # existed. The docstring claimed "defined and reached"; the code
        # checked neither.
        #
        # A definition is `<something> sym(` with a brace, on one line or the
        # next; a call is `sym(` that is not that. Both are required, so
        # deleting either half now fails.
        defined = False
        calls = 0
        for name, text in bodies.items():
            for m in re.finditer(r"(^|[^\w])" + re.escape(sym) + r"\s*\(",
                                 text, re.M):
                ls = text.rfind(chr(10), 0, m.start()) + 1
                le = text.find(chr(10), m.start())
                line = text[ls:le if le >= 0 else len(text)]
                # m.start() points at the character BEFORE the name
                # (the regex captures it), so the name begins after
                # that group - using m.start() put a stray "." or ")"
                # into `before` and turned every method call into a
                # prototype.
                name_at = m.start() + len(m.group(1))
                before = text[ls:name_at].strip()
                tail = text[m.end():m.end() + 300]

                # A PROTOTYPE IS NEITHER A DEFINITION NOR A CALL.
                #
                # `bool sym();` in a header matches `sym(` exactly as a call
                # does. Counting it as a call meant "defined but never CALLED"
                # could not fire while any header declared the symbol - which
                # is always. Counting it as a definition would be worse.
                #
                # The difference is what sits BEFORE the name on the line: a
                # prototype and a definition are preceded by a return type, a
                # call is preceded by nothing, an operator, or an argument
                # separator. Then the two are told apart by whether a brace or
                # a semicolon closes it.
                # "*" is NOT an exclusion: a pointer return type ends with one
                # (`const uint32_t* lsw2_arena_hud_composite(`) and dropping it
                # classified every such definition as a call.
                # A QUALIFIED DEFINITION ends with "::" - `void
                # Lsw2ForkSession::set_coop(bool) {` - and a single ":" would
                # otherwise be excluded as a label, so it is tested first.
                qualified = before.endswith("::")
                declares = qualified or (bool(before) and not before.endswith(
                    ("(", ",", "=", "!", "&&", "||", "return", "->", ".", ")",
                     ";", "{", "}", ":")))
                closes_with_brace = "{" in tail.split(";")[0]

                if declares and closes_with_brace:
                    defined = True
                elif declares:
                    pass                     # a prototype: neither
                else:
                    calls += 1

        # USED AS A TYPE COUNTS AS USED.
        #
        # Lsw2LanBeacon and Lsw2LanBrowser are classes held as members -
        # `Lsw2LanBeacon lan_;` - so their constructors are defined and nothing
        # ever writes `Lsw2LanBeacon(`. The function test above therefore
        # reported them "defined but never CALLED" while forksession.h holds
        # one of each, which is exactly the false alarm that gets a gate
        # switched off. A declaration `Sym name;` or a reference `Sym&` is use.
        type_uses = 0
        for text in bodies.values():
            type_uses += len(re.findall(
                r"\b" + re.escape(sym) + r"\s*[&*]?\s+\w+\s*[;,)=]", text))
        if calls == 0 and type_uses > 0:
            continue

        # A TYPE IS NOT A FUNCTION. Some clauses map to a class rather than a
        # call - `Lsw2LobbyTimer` is declared as a member, never invoked - so
        # `sym(` finds nothing and the function test above is the wrong one.
        # Fall back to the old mention count for those, and say so, rather than
        # reporting a type that is used everywhere as missing.
        if not defined and calls == 0:
            mentions = sum(len(re.findall(re.escape(sym), x))
                           for x in bodies.values())
            if mentions == 0:
                bad.append((point, what, sym, "NOT FOUND"))
            elif mentions < 3:
                bad.append((point, what, sym,
                            f"not a function; only {mentions} mentions"))
            continue

        if not defined and calls == 0:
            bad.append((point, what, sym, "NOT FOUND"))
        elif not defined:
            bad.append((point, what, sym,
                        f"called {calls}x but never DEFINED - deleted?"))
        elif calls == 0:
            bad.append((point, what, sym,
                        "defined but never CALLED - unwired"))

    guard_bad = check_guards()
    if guard_bad:
        print("feature clauses whose GUARD has gone:")
        for line in guard_bad:
            print(f"  {line}")
        return 1

    if bad:
        print("feature clauses whose implementation is missing or unwired:")
        for point, what, sym, why in bad:
            print(f"  {point}: {what}")
            print(f"      {sym}  -  {why}")
        return 1

    # GUARDS COUNT TOWARD THE PER-FEATURE TALLY. This summary is what gets read
    # to decide "which features have coverage", and with guards left out of it
    # the fork executable, the cheats tab and the master server still read as
    # zero - which is the exact impression that let them go unwatched in the
    # first place.
    by_point = {}
    for point, _, _ in CLAUSES:
        by_point[point] = by_point.get(point, 0) + 1
    for point, _, _, _, _ in GUARDS:
        by_point[point] = by_point.get(point, 0) + 1
    summary = "  ".join(f"{k} x{v}" for k, v in sorted(by_point.items()))
    print(f"all {len(CLAUSES)} feature clauses are implemented and reached, "
          f"and {len(GUARDS)} guard-shaped clauses still guard")
    print(f"  {summary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
