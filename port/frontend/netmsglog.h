/* Naming the cartridge's own network messages as they arrive.
 *
 * WHY. The protocol is mapped - 70 types, 64 implemented - but not which
 * type is the round start, the pick or the score screen. Reading 64 handler
 * bodies would answer it; so would one match with the types printed as they
 * arrive, and the second is far cheaper.
 *
 * WHY THIS NEEDS NO OVERRIDE. The pump's own data is at a fixed address. It is
 * called from exactly one place, `0x0205EE20`, with a LITERAL:
 *
 *     0205EE1C  LDR r0, =0x0210794C
 *     0205EE20  BL  0x020B0410
 *
 * and inside, each station's slots are walked from `owner + station*0x344`:
 *
 *     020B0430  MOV  r0, #0x344
 *     020B0438  ADD  r8, r9, r2            owner + station*0x344
 *     020B0468  ADD  r0, r8, r4, LSL #2    + slot*4
 *     020B0470  LDR  r2, [r0 + 0xA3C]      the slot entry
 *     020B0480  AND  r2, r2, #0xFF         its low byte is the DELTA to the next
 *     020B0484  ADD  r2, r4, r2
 *     020B048C  LDRBNE r0, [r1 + 1]        byte 1 is the TYPE
 *     020B04C8  TST  r0, #0xFF             stop when a slot's delta is 0
 *
 * So it is a linked list of 4-byte entries, and the port can walk the same list
 * read-only. That matters: `0x020B1248` has exactly one caller, so overriding it
 * would mean reimplementing all 70 types (the rule in
 * `tools/sweep_override_targets.py`). Reading costs nothing and replaces
 * nothing.
 *
 * DEDUPLICATION IS THE GAME'S OWN. `[entry + 2]` is a sequence number, compared
 * at `0x020B0488` against a per-station expected value and dispatched only when
 * they differ. This mirrors that, so the log names each message once instead of
 * every frame it sits in the buffer. */

#ifndef LSW2_NETMSGLOG_H
#define LSW2_NETMSGLOG_H

#include <cstdint>

constexpr uint32_t kLsw2MsgOwner = 0x0210794Cu;
constexpr uint32_t kLsw2MsgStationStride = 0x344u;
constexpr uint32_t kLsw2MsgSlotBase = 0xA3Cu;
constexpr int kLsw2MsgStations = 4;

/* THE PER-STATION SEQUENCE RECORD, which was declared in two more files.
 *
 * netmsg.cpp had it as kSeqBase/kSeqStride and diagnostics.cpp as
 * kNetSeqBase/kNetStationStride, and BOTH sat directly beneath a comment
 * saying kLsw2MsgOwner and the offsets had been collapsed into this header. The
 * earlier pass took the three constants it was looking for and left these two
 * behind, under a note asserting the job was finished.
 *
 * THE LOOP THEY COME FROM, at 0x020B0450:
 *
 *     020B044C  MOV   r0, #0xA
 *     020B0450  MLA   r1, r5, r0, r9      r1 = obj + station * 0x0A
 *     020B0460  SUB   r6, r1, #1          pre-decrement, OUTSIDE the slot loop
 *     020B0474  ADD   r6, r6, #1          ...re-incremented per SLOT
 *     020B047C  LDRB  r0, [r6, #0x40]     the last sequence seen
 *     020B04A4  STRB  r3, [r6, #0x40]     ...and stored back
 *
 * WHY THE STRIDE IS TEN. r6 walks one byte per message slot, so the record is
 * ten SEQUENCE BYTES, one per slot - the 0x0A is a slot count, not a padding
 * or an alignment. An earlier reading recorded this as `[r1 + 0x40]` and
 * elided the SUB/ADD pair, which reads at first glance like an off-by-one; it is not.
 * The pre-decrement cancels the first re-increment, so slot 0 lands exactly on
 * `r1 + 0x40`. Checked against the ROM before it was written down as a bug.
 *
 * Both are now PINNED by tools/sweep_rom_constants.py against those two
 * instructions, which is the whole reason the duplicates mattered: this
 * header's constants are checked against the ROM and the copies were not. */
constexpr uint32_t kLsw2MsgSeqBase = 0x40u;
constexpr uint32_t kLsw2MsgSeqStride = 0x0Au;

/* THE WALK ITSELF, pure. `read32` returns the 4-byte slot entry at an address;
 * `out` is called once per entry with (slot, type, sequence). Returns how many
 * entries were visited.
 *
 * BOUNDED, because the list is guest data and guest data can be garbage. A
 * delta of 0 ends the list normally; a cycle or a wild delta is stopped by the
 * visit cap rather than hanging the frame. */
int lsw2_netmsg_walk(uint32_t station_base,
                     uint32_t (*read32)(uint32_t addr, void* ctx), void* ctx,
                     void (*out)(int slot, int type, int seq, void* ctx));

/* Once a frame from the fork's tick. Names each message once. */
void lsw2_netmsglog_tick();

#endif /* LSW2_NETMSGLOG_H */
