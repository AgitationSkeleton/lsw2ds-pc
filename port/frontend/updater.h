/* UPDATES: find a newer release on GitHub, download its game zip, install it
 * over this folder, restart. The same design as the LEGO Battles PC port's
 * (its lb_update.cpp): only a downloaded release updates itself, and the
 * player's own files - settings, saves, BIOS identity, ROM, logs - are never
 * written.
 *
 * A release zip is one top folder, LSW2DS-PC-v<version>/, holding build/,
 * port/, rom/ and licenses/ (tools/package_build.py). The install is the
 * folder ABOVE build/, where the running exe sits; build/version.txt beside
 * the exe marks it as a release and names its version. */
#pragma once

#include <string>

/* This build's version: build/version.txt's text ("0.3.4"), or "" for a
 * development build, which has none. */
const std::string& lsw2_update_this_version();

/* True when this is an installed release that can update itself. */
bool lsw2_update_supported();

/* THE CHECK, on a thread of its own. quiet = the startup check, which only
 * speaks (a notice) when a newer release exists - and installs it when
 * auto_install is set. */
void lsw2_update_check(bool quiet, bool auto_install);

struct Lsw2UpdateInfo {
    int state = 0;       /* 0 not checked, 1 checking, 2 done, 3 failed */
    std::string tag, name, notes, url, error;
    std::string asset;   /* the release's game zip */
    bool newer = false;  /* a release later than this build */
};
Lsw2UpdateInfo lsw2_update_info();

/* THE INSTALL: download and unpack `asset` (a release's game zip). False when
 * one is running already or this build cannot update itself. */
bool lsw2_update_install(const std::string& asset, const std::string& tag);

/* 0 idle, 1 downloading, 2 installing, 3 installed (restart to use), 4 failed;
 * *msg says what happened. */
int lsw2_update_state(std::string* msg);

/* Start the installed exe with this one's command line, then quit this one. */
void lsw2_update_restart();

/* At startup: remove the .old files the last update left (in use then). And
 * the one-line notice the startup check posted, for ui_draw to show; empty
 * when there is none. */
void lsw2_update_cleanup();
std::string lsw2_update_notice(unsigned* posted_ms);

/* Version order, for the test: 1 when a is later than b, -1 earlier, 0 same.
 * "v0.3.4" and "0.3.4" are the same; a suffix ("0.4.0-rc.1") is earlier than
 * the plain version. */
int lsw2_update_compare(const std::string& a, const std::string& b);
