# Asset overrides — the modularity layer

This is the part that makes the port *customizable* rather than merely faithful:
replace a texture, model or sound without touching game code.

## How LSW2 stores content

One archive, `rom.bin`, inside NitroFS. Its structure was read out of the ROM,
not assumed:

```
rom.bin   ROM offset 857,600, 18,334,192 bytes
  [0]     u32 offset table, 5,549 entries for 5,548 assets
          asset i runs from table[i] to table[i+1]
  [...]   payloads, starting at table[0] = 0x56B4 = 5549 * 4
```

The N+1 table is what makes this whole layer possible: an asset's extent is
derived from the table rather than stored beside the data, so **a replacement
does not have to be the same size as the original.**

`out/raw/archive/catalog.json` carries all 5,548 entries — offset, packed size,
SHA-256, wrapper type, format magic (`BMD0` for Nitro models, etc.) and the
names each is known by.

## Using it

Drop `<asset_id>.bin` into `port/assets/override/`, holding the **packed** bytes
exactly as they sit in the archive — same wrapper as the original. Then:

```sh
python tools/build_asset_rom.py                    # report what would change
python tools/build_asset_rom.py --out modded.nds   # write the cartridge
```

## Two things that will bite, and are handled

**The archive can outgrow its slot.** If the rebuilt archive no longer fits, it
is appended at the end of the image and the NitroFS FAT entry for `rom.bin` is
repointed. Verified: growing one asset by 4 KB relocated the archive and left
all 5,547 others byte-identical by SHA-256.

**A grown ROM must be padded to a power of two.** The runner serves cartridge
reads with

```c
address &= (uint32_t)(g_card_rom.size() - 1u);
```

which is only a correct wrap when the size is a power of two. The stock ROM is
exactly 32 MB, so this never shows up — but a grown image lands on something
like `0x0317D1F0`, and every read would be masked against a value full of holes,
silently returning bytes from elsewhere in the cartridge. The tool pads up to the
next power of two and sets the header's device-capacity field to match.

## The SHA-1 consequence — do not skip this

The runner registers our recompiled banks **only** for a cartridge whose SHA-1
matches `NDS_TITLE_ROM_SHA1`. Any modded ROM has a different hash, so the runner
must be reconfigured with the new value or the game silently falls back to the
interpreter and crawls.

The expensive part does not repeat. Banks are keyed to the **ARM9 and ARM7 image
hashes**, and this tool touches neither — only NitroFS data. So a mod costs a
CMake reconfigure and a relink, not a re-emit of 44,331 functions.

```sh
cmake -G Ninja -DNDS_TITLE_ROM_SHA1=<new hash> ... -B <build> <framework>/runner
cmake --build <build> -j
```

## Validation

Both paths are tested rather than asserted:

| test | result |
|---|---|
| identity override (one asset written back unchanged) | rebuilt cartridge is **byte-identical** — SHA-1 `7c1afa07…c443` |
| grow one asset by 4,096 bytes | archive relocated, ROM padded to 64 MB, **5,547 / 5,547** other assets intact |

The identity case is the important one: it proves the offset table, payload
concatenation, FAT repointing and header CRC are all reproduced exactly, because
any error in any of them would change the hash.
