/* The co-op arrows were visible in the lobby - and in the ARENA MAPS too, which
 * took a second sighting to find.
 *
 * SCOPE CORRECTED. This shipped gated to level 9 on the strength of a reading
 * of the marker as level-9 content. The co-op arrow was then seen briefly in
 * the Bounty Hunter arena level, and a probe
 * confirmed the identical signature - parameter 0803682C, mask 001F78FF - on
 * LEVEL 5, on both consoles, during a live deathmatch. So the reading was
 * wrong and the gate was hiding the arrow in exactly one of the three places it
 * appears.
 *
 * The scope is now the fork's three maps: the arena lobby and the two arenas.
 * Story levels are still refused, because there the arrow is doing the co-op
 * job it was drawn for. See the gate in arrowhide.cpp for why that caution is
 * kept rather than widened to "any level".
 *
 * AND THE ARENA'S ARROW IS A DIFFERENT TEXTURE, which is why widening the scope
 * alone changed nothing. Measured 2026-09-11 by reading the child's polygons in
 * a live deathmatch: four alpha-0x17 polygons sitting over
 * the character, attr 02178080 exactly like the lobby's, but texture 036B00 /
 * palette 03DA / raw texparam 08036D60 against the lobby's 034160 / 0246 /
 * 0803682C. The signature scan had found exactly ONE struct at level 5 - the
 * lobby's leftover, still resident - cleared it, and the arrow drew from a
 * struct the scan could not match. The locator now carries both parameters
 * (kArrowTexParams); the mask is the same.
 *
 * WHAT THIS IS, AND IT IS NOT A REPAIR. The green marker over a partner's head
 * is the CARTRIDGE'S OWN level-9 content, and its draw depends on a peer being
 * present - which is what a
 * co-op partner marker is for. The retail Cantina with two players would show
 * it too. The fork wants it gone because the fork's Cantina is a LOBBY, where
 * connected players stand and navigate rather than follow one another.
 *
 * So this is a DELIBERATE DIVERGENCE FROM THE ROM and ships recorded as one.
 * Nothing here is fixing a defect the port introduced.
 *
 * HOW IT WORKS, AND WHY IT IS FOUR BYTES FROM AN EARLIER ATTEMPT THAT FAILED.
 *
 * The arrow is four polygons carrying TEXIMAGE_PARAM 0x0803682C. An
 * ITCM routine binds a material for them, and its own first branch is an off
 * switch the cartridge already takes:
 *
 *     01FFBC64  LDR r1,[r4,#0xC]
 *     01FFBC68  TST r1,#0x1F0000      ; the alpha field
 *     01FFBC6C  BEQ 01FFBCF4          ; ZERO ALPHA -> skip the whole bind
 *
 * The material it tests is not a copy of anything: it is EMBEDDED at ctx+0xF4
 * in a per-draw DTCM render context, so it cannot be poked from
 * outside. What CAN be reached is the main-RAM source struct the context is
 * filled from, and the fill is a MASKED MERGE of two of its fields:
 *
 *     +0x0C  the VALUE   00170080   <- alpha 0x17. THE LEVER.
 *     +0x10  the MASK    001F78FF
 *     +0x14  the texture parameter   0803682C
 *
 * An earlier attempt zeroed +0x10, the MASK, and nothing changed on screen - because
 * clearing alpha in a mask only means "do not take alpha from this source",
 * leaving the context's own. That elimination looked sound and was aimed four
 * bytes past the answer. Zeroing the alpha of the VALUE makes the merged alpha
 * zero, and the cartridge skips the bind BY ITS OWN BRANCH rather than by
 * anything invented here.
 *
 * Demonstrated on screen 2026-09-07 with a peer admitted: arrow present, one
 * write, arrow gone, still gone six seconds later, both consoles healthy.
 *
 * THE ADDRESS IS FOUND, NOT HARDCODED. The first poke used 0x02207438, which
 * is where the struct happened to live in that build and boot. The struct
 * identifies itself - the parameter at +0x14 with the known mask at +0x10 - so
 * it is located by that signature and re-validated before every write. A
 * hardcoded address would be a fix that works until something moves.
 */
#ifndef LSW2_ARROWHIDE_H
#define LSW2_ARROWHIDE_H

/* Once a frame from the fork tick. Silent and nearly free outside the arena
 * lobby; the scan is amortised and its result cached. */
void lsw2_arrowhide_tick();

#endif /* LSW2_ARROWHIDE_H */
