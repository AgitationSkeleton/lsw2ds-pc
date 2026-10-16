# Releasing

## Versions and tags

- Versions are `MAJOR.MINOR.PATCH`. Until 1.0, a minor version adds features
  and a patch version only fixes bugs. A suffix marks a test build, as in
  `0.2.0-rc.1`.
- Each release is the annotated tag `v<version>` on `main`, plus a GitHub
  release of the same name with the game zip attached. Releases before 1.0 are
  marked pre-release.
- `CHANGELOG.md` is the source of each release's notes. Changes go under
  **Unreleased** as they land.

## Making a release

You need the build prerequisites from [BUILDING.md](BUILDING.md): the ROM and
BIOS dumps. The release binary is built from your checkout, never in CI.

1. **Update the changelog.** Rename `## Unreleased` to
   `## 0.2.0 - <date>`, then add a fresh empty `## Unreleased` above it.
   Commit.
2. **Dry run.** This builds and packages without changing anything:

       py -3.13 tools/release.py 0.2.0 --dry-run

   Open `dist/LSW2DS-PC-v0.2.0.zip`, unzip it somewhere new, and play it once.
   The first start should ask for the ROM.
3. **Release:**

       py -3.13 tools/release.py 0.2.0

   This rebuilds and runs the unit suite (a red suite stops it), then packages
   and tags `v0.2.0`. It pushes `main` and the tag, then uploads the zip and
   `SHA256SUMS.txt` to the GitHub release. The push starts
   `.github/workflows/release.yml`, which re-checks the tree and keeps the
   release notes in step with the changelog. The two may run in either order.
4. If the upload fails partway (a network drop, for example), finish it with:

       py -3.13 tools/release.py 0.2.0 --upload-only

The upload needs a GitHub token that can write this repository's contents:
`GITHUB_TOKEN` in the environment, or the credential `git push` already uses.

## What a release contains

`tools/package_build.py` decides, and refuses to package anything else:

    LSW2DS-PC-v0.2.0/
      README.txt                       how to play; the source URL and commit
      build/lsw2_bountyhunter.exe      Bounty Hunter
      build/lsw2.exe                   the game as shipped, with online co-op
      build/*.dll, build/assets/fonts/ runtime libraries; the game's fonts, from the ROM
      port/game.toml                   the title config
      port/saves/unlocked-template.sav seeds Bounty Hunter's unlocks
      rom/                             empty: the first start fills it

A release never contains a ROM, BIOS or firmware dumps, saves, settings or logs.

**About the executables.** They contain code translated from the cartridge. That
is the nature of a static recompilation, and it is why the ROM is never in the
repository. The fonts in `build/assets/fonts/` are also extracted from the ROM at
build time. Because the executables link GPL-3.0-or-later code, every zip's
README names the exact source commit it was built from.

## Where changes come from

Development happens in a separate working tree, and its source and
documentation are exported here. Anything derived from the ROM is left behind.
Changes made directly in this repository are welcome: they are carried back to
the working tree before the next export.
