"""The master server, GUI flavour.

What the window is:

    1. The server ships as two programs: this GUI, and a console version
       that runs in a command prompt.
    2. The GUI's sidebar stacks two scrolling lists one above the other:
       Bounty Hunter lobbies and co-op lobbies. Expanding a lobby shows the
       players connected to it. Right-clicking a server or a player opens a
       menu with "Kick" and "Ban IP", for dealing with anyone abusing the
       service.
    3. The larger right-hand part of the window shows the console log,
       which is fairly detailed.

    python master_server_gui.py --port 28553

THIS IS A FRONT END AND NOTHING ELSE. Every decision about lobbies, bans and
logging lives in master_server.py, which the command-prompt flavour runs with a
different sink attached. If a behaviour differs between the two windows, that is
a bug here - the point of splitting them this way is that there is only one
service to reason about.

TKINTER BECAUSE IT IS IN THE BOX. The service's whole appeal is "run it with one
command and no dependencies"; a GUI that needed a pip install would undo that,
and both flavours have to freeze into executables from the same tree.

THREADING. The HTTP server runs on a worker thread and Tk runs on the main one,
so nothing the service emits may touch a widget directly. Log records land in a
queue and the window drains it on a timer; the lobby lists are rebuilt from a
snapshot on the same timer. Every widget call in this file is therefore on the
Tk thread, which is the rule that keeps a GUI from dying in ways nobody can
reproduce.
"""

from __future__ import annotations

import os
import queue
import threading
import tkinter as tk
from tkinter import messagebox, ttk

import eventlog
import master_server as ms

# How often the window redraws the lobby lists. A directory changes on the scale
# of a heartbeat (30s), so a second is already generous; anything faster is
# spent rebuilding a tree nobody watched change.
REFRESH_MS = 1000
LOG_DRAIN_MS = 120

# The log pane keeps this many lines. Well past a day of ordinary traffic, and
# the FILES are the real history - this is the scrollback, not the record.
LOG_LINES = 4000

LEVEL_TAGS = {
    eventlog.DEBUG: ("debug", "#7d8590"),
    eventlog.INFO: ("info", "#c9d1d9"),
    eventlog.WARN: ("warn", "#e3b341"),
    eventlog.ERROR: ("error", "#f85149"),
}


class LobbyList(ttk.Frame):
    """One of the two scrollable lists in the sidebar.

    A Treeview rather than a Listbox because expanding a lobby shows the
    players connected to it - that is a tree, and
    building one out of a flat list with manual indentation is how you end up
    with a right-click menu that acts on the wrong row.
    """

    def __init__(self, parent, title, on_menu):
        super().__init__(parent)
        self.on_menu = on_menu

        ttk.Label(self, text=title, style="Head.TLabel").pack(
            fill="x", padx=6, pady=(6, 2))

        holder = ttk.Frame(self)
        holder.pack(fill="both", expand=True, padx=6, pady=(0, 6))

        self.tree = ttk.Treeview(holder, columns=("detail",), show="tree headings",
                                 selectmode="browse")
        self.tree.heading("#0", text="lobby")
        self.tree.heading("detail", text="")
        self.tree.column("#0", width=170, stretch=True)
        self.tree.column("detail", width=110, stretch=False, anchor="w")

        bar = ttk.Scrollbar(holder, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=bar.set)
        self.tree.pack(side="left", fill="both", expand=True)
        bar.pack(side="right", fill="y")

        # Windows and most X11 window managers put the context menu on button 3;
        # macOS sends button 2 for a two-finger click. Binding both costs
        # nothing and saves a bug report from whoever runs this on a Mac.
        self.tree.bind("<Button-3>", self._context)
        self.tree.bind("<Button-2>", self._context)

        # item id -> the thing it stands for, so the menu never has to parse the
        # row's text back into a lobby.
        self.targets: dict = {}
        self._open: set = set()

    def _context(self, event):
        item = self.tree.identify_row(event.y)
        if not item:
            return
        self.tree.selection_set(item)
        target = self.targets.get(item)
        if target:
            self.on_menu(event, target)

    def render(self, lobbies):
        """Rebuild from a snapshot.

        EXPANSION STATE IS PRESERVED BY LOBBY ID, not by tree position. A
        rebuild every second that collapsed the row an operator had just opened
        would make the expand feature useless, and lobbies reorder as they
        heartbeat (the listing is newest-update-first).
        """
        for item in self.tree.get_children(""):
            if self.tree.item(item, "open"):
                self._open.add(self.targets.get(item, ("", ""))[1])
            else:
                self._open.discard(self.targets.get(item, ("", ""))[1])

        selected = self.tree.selection()
        keep = self.targets.get(selected[0]) if selected else None

        self.tree.delete(*self.tree.get_children(""))
        self.targets.clear()
        restore = None

        for lobby in lobbies:
            label = "%s  [%s]" % (lobby["name"], lobby["code"] or "----")
            detail = "%d/%d  %s:%d" % (lobby["players"], lobby["max_players"],
                                       lobby["address"], lobby["port"])
            node = self.tree.insert("", "end", text=label, values=(detail,),
                                    open=lobby["id"] in self._open)
            self.targets[node] = ("lobby", lobby["id"], lobby)
            if keep and keep[0] == "lobby" and keep[1] == lobby["id"]:
                restore = node

            roster = sorted(lobby.get("roster", []),
                            key=lambda p: p.get("slot", 0))
            if not roster:
                # A host that has not reported a roster yet is not the same as
                # an empty lobby, and the window should not imply it is.
                self.tree.insert(node, "end", text="   (no roster reported)",
                                 values=("",))
                continue
            for player in roster:
                child = self.tree.insert(
                    node, "end",
                    text="   %d. %s" % (player.get("slot", 0) + 1,
                                        player.get("name", "?")),
                    values=(player.get("address", ""),))
                self.targets[child] = ("player", lobby["id"], lobby, player)
                if (keep and keep[0] == "player" and keep[1] == lobby["id"]
                        and keep[3].get("slot") == player.get("slot")):
                    restore = child

        if restore:
            self.tree.selection_set(restore)


class Window(tk.Tk):
    def __init__(self, service: ms.Service):
        super().__init__()
        self.service = service
        self.queue: queue.Queue = queue.Queue()
        self.show_debug = tk.BooleanVar(value=True)
        self.autoscroll = tk.BooleanVar(value=True)

        self.title("LEGO Star Wars II - master server  (port %d)"
                   % service.args.port)
        self.geometry("1180x680")
        self.minsize(820, 460)
        self._style()
        self._build()

        # replay=True so a window opened after the service started still shows
        # the startup and port lines point 3 asks for.
        service.log.add_sink(self.queue.put, replay=True)
        self.protocol("WM_DELETE_WINDOW", self._close)
        self.after(LOG_DRAIN_MS, self._drain)
        self.after(REFRESH_MS, self._refresh_loop)

    # ------------------------------------------------------------------ chrome

    def _style(self):
        style = ttk.Style(self)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure("Head.TLabel", font=("Segoe UI", 9, "bold"))

    def _build(self):
        panes = ttk.PanedWindow(self, orient="horizontal")
        panes.pack(fill="both", expand=True)

        sidebar = ttk.Frame(panes)
        # The two lists are stacked one above the other, so the sidebar is
        # itself a vertical pane - the operator can decide how much of it each list
        # gets rather than the window guessing.
        lists = ttk.PanedWindow(sidebar, orient="vertical")
        lists.pack(fill="both", expand=True)
        self.bounty = LobbyList(lists, "Bounty Hunter lobbies", self._menu)
        self.coop = LobbyList(lists, "Co-op lobbies", self._menu)
        lists.add(self.bounty, weight=1)
        lists.add(self.coop, weight=1)

        right = ttk.Frame(panes)
        bar = ttk.Frame(right)
        bar.pack(fill="x", padx=6, pady=(6, 0))
        ttk.Label(bar, text="Console log", style="Head.TLabel").pack(side="left")
        ttk.Checkbutton(bar, text="debug", variable=self.show_debug,
                        command=self._replay).pack(side="right")
        ttk.Checkbutton(bar, text="follow", variable=self.autoscroll).pack(
            side="right", padx=(0, 10))
        ttk.Button(bar, text="Open log folder", command=self._open_logs).pack(
            side="right", padx=(0, 10))

        holder = ttk.Frame(right)
        holder.pack(fill="both", expand=True, padx=6, pady=6)
        self.text = tk.Text(holder, wrap="none", height=10, bg="#0d1117",
                            fg="#c9d1d9", insertbackground="#c9d1d9",
                            font=("Consolas", 9), state="disabled",
                            borderwidth=0)
        vbar = ttk.Scrollbar(holder, orient="vertical", command=self.text.yview)
        hbar = ttk.Scrollbar(right, orient="horizontal", command=self.text.xview)
        self.text.configure(yscrollcommand=vbar.set, xscrollcommand=hbar.set)
        self.text.pack(side="left", fill="both", expand=True)
        vbar.pack(side="right", fill="y")
        hbar.pack(fill="x", padx=6, pady=(0, 6))
        for _, (tag, colour) in LEVEL_TAGS.items():
            self.text.tag_configure(tag, foreground=colour)

        # The sidebar is the smaller share: point 3 asks for the log on "the
        # right and greater portion".
        panes.add(sidebar, weight=1)
        panes.add(right, weight=3)

        self.status = ttk.Label(self, anchor="w", padding=(8, 3))
        self.status.pack(fill="x")

    # -------------------------------------------------------------------- log

    def _drain(self):
        try:
            while True:
                self._append(self.queue.get_nowait())
        except queue.Empty:
            pass
        self.after(LOG_DRAIN_MS, self._drain)

    def _append(self, record):
        if record.level < eventlog.DEBUG or (
                record.level == eventlog.DEBUG and not self.show_debug.get()):
            return
        tag = LEVEL_TAGS.get(record.level, ("info", "#c9d1d9"))[0]
        self.text.configure(state="normal")
        self.text.insert("end", record.format() + "\n", tag)
        # Trim from the top rather than let the widget grow without bound; a
        # service left running for a week would otherwise hold every line it
        # ever wrote in memory as well as on disk.
        excess = int(self.text.index("end-1c").split(".")[0]) - LOG_LINES
        if excess > 0:
            self.text.delete("1.0", "%d.0" % (excess + 1))
        self.text.configure(state="disabled")
        if self.autoscroll.get():
            self.text.see("end")

    def _replay(self):
        """Redraw the pane when the debug filter is toggled."""
        self.text.configure(state="normal")
        self.text.delete("1.0", "end")
        self.text.configure(state="disabled")
        for record in self.service.log.tail(LOG_LINES):
            self._append(record)

    def _open_logs(self):
        path = os.path.abspath(self.service.args.logs)
        try:
            os.makedirs(path, exist_ok=True)
            os.startfile(path)                     # noqa: S606 - Windows only
        except (OSError, AttributeError):
            # Not Windows, or no shell association. Say where it is instead of
            # failing silently - the path is the useful half of the button.
            messagebox.showinfo("Log folder", path)

    # ----------------------------------------------------------------- lobbies

    def _refresh_loop(self):
        # THE TIMER AND THE WORK ARE SEPARATE. A first version rescheduled from
        # inside the work, so the menu's "redraw now" call added a second timer
        # every time an operator used it - the window would have got faster and
        # faster until it was doing nothing else.
        self._refresh()
        self.after(REFRESH_MS, self._refresh_loop)

    def _refresh(self):
        try:
            self.service.store.sweep()
            lobbies = self.service.store.listing(operator=True)
        except Exception as exc:                   # pragma: no cover - defensive
            self.status.configure(text="directory error: %s" % exc)
            return

        bounty = [x for x in lobbies if x["kind"] == ms.KIND_BOUNTY]
        coop = [x for x in lobbies if x["kind"] == ms.KIND_COOP]
        self.bounty.render(bounty)
        self.coop.render(coop)

        players = sum(x["players"] for x in lobbies)
        self.status.configure(
            text="%d bounty  |  %d co-op  |  %d player%s  |  %d ban%s  |  %s"
                 % (len(bounty), len(coop), players, "" if players == 1 else "s",
                    len(self.service.bans.entries()),
                    "" if len(self.service.bans.entries()) == 1 else "s",
                    os.path.abspath(self.service.args.bans)))

    # -------------------------------------------------------------------- menu

    def _menu(self, event, target):
        menu = tk.Menu(self, tearoff=0)
        if target[0] == "lobby":
            lobby = target[2]
            menu.add_command(label="Kick (close this lobby)",
                             command=lambda: self._close_lobby(lobby))
            menu.add_command(label="Ban IP  %s" % lobby["address"],
                             command=lambda: self._ban(lobby["address"],
                                                       "host " + lobby["name"]))
        else:
            _, sid, lobby, player = target
            who = player.get("name", "?")
            address = player.get("address", "")
            if player.get("slot", 0) == 0:
                # SLOT 0 IS THE HOST. The transport seats the host at station 0
                # and joiners from 1, so a "kick" aimed here would be an order
                # to the host to disconnect itself - which it would look up,
                # not find, and ignore. Closing the lobby is what the operator
                # actually means, and it is the thing that works.
                menu.add_command(label="Kick %s (host - closes the lobby)" % who,
                                 command=lambda: self._close_lobby(lobby))
            else:
                menu.add_command(label="Kick %s" % who,
                                 command=lambda: self._kick(sid, lobby, player))
            if not address and player.get("slot", 0) == 0:
                # The host does not report its own address and should not be
                # believed if it did - the directory took the real one from the
                # socket the registration arrived on. Offer that.
                menu.add_command(label="Ban IP  %s" % lobby["address"],
                                 command=lambda: self._ban(lobby["address"],
                                                           "host " + who))
            elif address:
                menu.add_command(label="Ban IP  %s" % address,
                                 command=lambda: self._ban_player(sid, lobby,
                                                                  player))
            else:
                # A host that reports names but not addresses gives the operator
                # nothing to ban. Showing the item greyed says why; hiding it
                # would look like a bug in the menu.
                menu.add_command(label="Ban IP  (host reported no address)",
                                 state="disabled")
        try:
            menu.tk_popup(event.x_root, event.y_root)
        finally:
            menu.grab_release()

    def _close_lobby(self, lobby):
        if messagebox.askyesno(
                "Close lobby",
                "Close %s lobby '%s' hosted by %s?\n\n"
                "The lobby is removed from the directory. Players already in it "
                "stay connected to the host - this service does not carry game "
                "traffic and cannot disconnect them."
                % (lobby["kind"], lobby["name"], lobby["address"])):
            self.service.close_lobby(lobby["id"])
            self._refresh_now()

    def _kick(self, sid, lobby, player):
        self.service.kick(sid, player.get("slot", 0), player.get("name", ""),
                          player.get("address", ""))
        messagebox.showinfo(
            "Kick queued",
            "The host is told to drop %s on its next heartbeat, which is at "
            "most %d seconds away.\n\nThe directory has no connection to a "
            "player - only the host does." % (player.get("name", "?"),
                                              ms.TTL_SECONDS // 3))

    def _ban_player(self, sid, lobby, player):
        address = player.get("address", "")
        if not self._ban(address, "player " + player.get("name", "")):
            return
        # A BAN AT THE DIRECTORY DOES NOT END A SESSION. The player is connected
        # to the host, not to this service, so banning alone would let them keep
        # playing in the lobby they are being banned for. Queue the kick too.
        self.service.kick(sid, player.get("slot", 0), player.get("name", ""),
                          address)

    def _ban(self, address, why):
        if not messagebox.askyesno(
                "Ban IP",
                "Permanently ban %s (%s)?\n\nThe address is appended to\n%s\n\n"
                "Bans do not expire. To lift one, edit that file by hand - the "
                "service re-reads it without a restart."
                % (address, why, os.path.abspath(self.service.bans.path))):
            return False
        if not self.service.ban(address, why):
            messagebox.showinfo("Ban IP", "%s was already banned, or is not an "
                                          "IP address." % address)
        self._refresh_now()
        return True

    def _refresh_now(self):
        try:
            self._refresh()
        except Exception:                          # pragma: no cover - defensive
            pass

    # ------------------------------------------------------------------ teardown

    def _close(self):
        self.service.log.remove_sink(self.queue.put)
        self.service.log.info("stop", "console window closed, stopping")
        threading.Thread(target=self.service.shutdown, daemon=True).start()
        self.after(150, self.destroy)


def main(argv=None):
    args = ms.build_parser().parse_args(argv)
    service = ms.Service(args)
    service.attach_file_log()
    try:
        service.build()
    except OSError as exc:
        # A WINDOWED BUILD HAS NOWHERE TO PRINT. Without this the second copy
        # of the GUI dies on a traceback nobody sees - the window never opens
        # and there is no console behind it to read.
        root = tk.Tk()
        root.withdraw()
        messagebox.showerror(
            "Master server",
            "Cannot listen on %s:%d.\n\n%s\n\nAnother copy is probably already "
            "running. Close it, or start this one with a different --port."
            % (args.host, args.port, exc))
        root.destroy()
        return 1
    threading.Thread(target=service.httpd.serve_forever, daemon=True).start()

    window = Window(service)
    # After the window exists, so its sink catches these live rather than only
    # on replay - which also proves the replay and the live path agree.
    service.announce()
    window.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
