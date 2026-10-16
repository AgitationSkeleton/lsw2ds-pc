/* Render a melonDS savestate through the port's own 2D engine.
 *
 * WHY THIS EXISTS. video2d.c is the one part of this port that is written
 * rather than translated, because the DS rasteriser is silicon and there is no
 * code in the cartridge to derive it from. That makes it the one part with
 * nothing to check it against - and a renderer that is subtly wrong produces a
 * picture, not an error, so it can be wrong for a long time without saying so.
 *
 * A savestate closes that off. It is a complete machine captured from the real
 * ROM running: every VRAM bank, the palettes, OAM, and both 2D engines' register
 * blocks, at an instant when the game was demonstrably drawing something. Push
 * that state through this port's renderer and the output either looks like the
 * game or it does not.
 *
 * NOTHING HERE IS GUESSED. The savestate layout is read from the melonDS source
 * vendored in this repository - GPU::DoSavestate and GPU2D::Unit::DoSavestate -
 * and the version in the file (12.1) matches SAVESTATE_MAJOR/MINOR in that same
 * tree, so the writer and the description are the same code. The two section
 * sizes confirm it independently: the layout predicts 669 payload bytes for
 * engine A and 113 for engine B, and the file contains exactly those.
 *
 * The one thing NOT taken from the state is POWCNT1, which decides which engine
 * drives which physical screen and is stored in a different section. Rather than
 * assume, the outputs are named by ENGINE rather than by screen.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ghidra_intrinsics.h"
#include "lsw2_gx.h"

bool lsw2_decomp_map(const char *root);
void lsw2_io_write(uint32_t addr, int bytes, uint32_t value);
uint32_t lsw2_io_read(uint32_t addr, int bytes);
void LSW2_HOSTCALL lsw2_vram_bank_write(unsigned index, unsigned char value);
void LSW2_HOSTCALL lsw2_video_render(uint32_t *top, uint32_t *bottom);
void LSW2_HOSTCALL lsw2_video_report(void);
void LSW2_HOSTCALL lsw2_vram_report(void);
void LSW2_HOSTCALL lsw2_gx_begin_frame(uint32_t clear_rgba, int32_t cleardepth);
void LSW2_HOSTCALL lsw2_gx_submit(const GxPolygon *p);
void LSW2_HOSTCALL lsw2_gx_get_frame(uint32_t *out);
void LSW2_HOSTCALL lsw2_gx_report(void);

/* Feed the rasteriser a polygon list captured from the real machine.
 *
 * The file is what tools/read_gx_state.py exported: a magic, a count, then one
 * record per polygon with its vertices already resolved. Reading it here rather
 * than parsing the savestate in C keeps all knowledge of the melonDS layout in
 * the one Python tool that verifies that layout against the section length. */
static int32_t rd32at(const unsigned char *p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)
                   | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static int load_and_draw(const char *path) {
    FILE *f = fopen(path, "rb");
    long len;
    unsigned char *d;
    uint32_t count, i;
    const unsigned char *q;

    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    d = (unsigned char *)malloc((size_t)len);
    if (!d || fread(d, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "cannot read %s\n", path); fclose(f); return 0;
    }
    fclose(f);
    if (len < 12 || memcmp(d, "LSW2GX01", 8)) {
        fprintf(stderr, "%s is not an exported polygon list\n", path);
        return 0;
    }
    count = (uint32_t)rd32at(d + 8);
    q = d + 12;

    /* Clear to transparent: the 3D layer is composited under the 2D engine,
     * so an uncovered pixel has to stay uncovered rather than become black. */
    lsw2_gx_begin_frame(0u, 0x7FFFFFFF);

    for (i = 0; i < count; ++i) {
        GxPolygon poly;
        int j;
        uint32_t nv = (uint32_t)rd32at(q);
        poly.nverts = (int)nv;
        poly.attr = (uint32_t)rd32at(q + 4);
        poly.texparam = (uint32_t)rd32at(q + 8);
        poly.texpalette = (uint32_t)rd32at(q + 12);
        poly.wbuffer = rd32at(q + 16) != 0;
        q += 20;
        if (nv > LSW2_GX_MAX_VERTS) return 0;
        for (j = 0; j < (int)nv; ++j) {
            int32_t cr, cg, cb;
            poly.v[j].x = rd32at(q + 0) * 16;   /* FinalPosition is integer */
            poly.v[j].y = rd32at(q + 4) * 16;
            poly.v[j].w = rd32at(q + 8);
            poly.v[j].z = rd32at(q + 12);
            cr = rd32at(q + 16); cg = rd32at(q + 20); cb = rd32at(q + 24);
            poly.v[j].r = (uint8_t)(cr < 0 ? 0 : cr > 63 ? 63 : cr);
            poly.v[j].g = (uint8_t)(cg < 0 ? 0 : cg > 63 ? 63 : cg);
            poly.v[j].b = (uint8_t)(cb < 0 ? 0 : cb > 63 ? 63 : cb);
            /* The exporter writes nine 32-bit fields and THEN the two 16-bit
             * texture coordinates, so they start at +36, not +32. Reading them
             * at +32 picked up padding, every vertex got s = t = 0, and each
             * polygon sampled a single texel - which renders as a correct but
             * entirely flat-shaded scene. */
            poly.v[j].s = (int16_t)(q[36] | (q[37] << 8));
            poly.v[j].t = (int16_t)(q[38] | (q[39] << 8));
            q += 42;
        }
        lsw2_gx_submit(&poly);
    }
    printf("rasterised %u polygon(s) from %s\n", count, path);
    return 1;
}

/* A section header is magic(4) + length(4) + 8 reserved bytes; the length
 * counts the header. Savestate::Section in the vendored source. */
#define SECTION_HEADER 16

static unsigned char *state;
static long state_len;

static unsigned char *section(const char *magic, uint32_t *payload_len) {
    long off = 16;   /* MELN, version, size */
    while (off + SECTION_HEADER <= state_len) {
        uint32_t len;
        memcpy(&len, state + off + 4, 4);
        if (len < SECTION_HEADER || off + (long)len > state_len) break;
        if (!memcmp(state + off, magic, 4)) {
            *payload_len = len - SECTION_HEADER;
            return state + off + SECTION_HEADER;
        }
        off += (long)len;
    }
    return NULL;
}

static uint16_t u16at(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t u32at(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The nine banks, in the order GPU::DoSavestate writes them. */
static const uint32_t BANK_BYTES[9] = {
    128*1024, 128*1024, 128*1024, 128*1024, 64*1024,
    16*1024, 16*1024, 32*1024, 16*1024
};
static const uint32_t BANK_LCDC[9] = {
    0x06800000u, 0x06820000u, 0x06840000u, 0x06860000u, 0x06880000u,
    0x06890000u, 0x06894000u, 0x06898000u, 0x068A0000u
};

/* GPU2D::Unit::DoSavestate, field for field. Offsets are cumulative from the
 * start of the payload and are not adjustable: they come from the writer. */
static void load_engine(const unsigned char *p, uint32_t io) {
    int i;
    lsw2_io_write(io + 0x00u, 4, u32at(p + 0));            /* DispCnt */
    for (i = 0; i < 4; ++i)
        lsw2_io_write(io + 0x08u + 2u * (uint32_t)i, 2, u16at(p + 4 + 2*i));
    for (i = 0; i < 4; ++i) {                              /* BGXPos/BGYPos */
        lsw2_io_write(io + 0x10u + 4u * (uint32_t)i, 2, u16at(p + 12 + 2*i));
        lsw2_io_write(io + 0x12u + 4u * (uint32_t)i, 2, u16at(p + 20 + 2*i));
    }
    for (i = 0; i < 2; ++i) {   /* BG2 params at 0x20, BG3 at 0x30 */
        uint32_t b = io + 0x20u + 0x10u * (uint32_t)i;
        lsw2_io_write(b + 0x00u, 2, u16at(p + 60 + 2*i));  /* PA */
        lsw2_io_write(b + 0x02u, 2, u16at(p + 64 + 2*i));  /* PB */
        lsw2_io_write(b + 0x04u, 2, u16at(p + 68 + 2*i));  /* PC */
        lsw2_io_write(b + 0x06u, 2, u16at(p + 72 + 2*i));  /* PD */
        /* The INTERNAL reference point is the one the hardware is actually
         * sampling from at this scanline; the written one may be stale. */
        lsw2_io_write(b + 0x08u, 4, u32at(p + 44 + 4*i));
        lsw2_io_write(b + 0x0Cu, 4, u32at(p + 52 + 4*i));
    }
    lsw2_io_write(io + 0x40u, 4, u32at(p + 76));           /* Win0Coords */
    lsw2_io_write(io + 0x44u, 4, u32at(p + 80));           /* Win1Coords */
    lsw2_io_write(io + 0x48u, 4, u32at(p + 84));           /* WinCnt */
    lsw2_io_write(io + 0x4Cu, 2, u16at(p + 88));           /* BG mosaic */
    lsw2_io_write(io + 0x50u, 2, u16at(p + 96));           /* BlendCnt */
    lsw2_io_write(io + 0x52u, 2, u16at(p + 98));           /* BlendAlpha */
    lsw2_io_write(io + 0x54u, 2, p[102]);                  /* EVY */
    lsw2_io_write(io + 0x6Cu, 2, u16at(p + 103));          /* MasterBrightness */
}

/* Portable, dependency-free output. Converted to PNG by the caller. */
static int write_ppm(const char *path, const uint32_t *fb) {
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) return 0;
    fprintf(f, "P6\n256 192\n255\n");
    for (i = 0; i < 256 * 192; ++i) {
        unsigned char rgb[3];
        rgb[0] = (unsigned char)(fb[i] >> 16);
        rgb[1] = (unsigned char)(fb[i] >> 8);
        rgb[2] = (unsigned char)fb[i];
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 1;
}

LSW2_HOSTCALL int main(int argc, char **argv) {
    const char *statepath, *outdir, *root;
    FILE *f;
    unsigned char *gpug, *ga, *gb;
    uint32_t glen, alen, blen, off;
    static uint32_t top[256 * 192], bottom[256 * 192];
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc < 4) {
        fprintf(stderr, "usage: render_savestate <state.mlN> <outdir> <root>\n");
        return 2;
    }
    statepath = argv[1];
    outdir = argv[2];
    root = argv[3];

    f = fopen(statepath, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", statepath); return 1; }
    fseek(f, 0, SEEK_END);
    state_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    state = (unsigned char *)malloc((size_t)state_len);
    if (!state || fread(state, 1, (size_t)state_len, f) != (size_t)state_len) {
        fprintf(stderr, "cannot read %s\n", statepath); return 1;
    }
    fclose(f);

    if (memcmp(state, "MELN", 4)) {
        fprintf(stderr, "not a melonDS savestate\n"); return 1;
    }
    printf("state %s: version %u.%u, %ld bytes\n", statepath,
           u16at(state + 4), u16at(state + 6), state_len);

    if (!lsw2_decomp_map(root)) {
        fprintf(stderr, "could not reserve the DS address space\n"); return 1;
    }

    gpug = section("GPUG", &glen);
    ga = section("GP2A", &alen);
    gb = section("GP2B", &blen);
    if (!gpug || !ga || !gb) {
        fprintf(stderr, "the state is missing a GPU section\n"); return 1;
    }
    printf("GPUG %u bytes, GP2A %u, GP2B %u\n", glen, alen, blen);
    /* The layout predicts these exactly. If they differ, the state was written
     * by a different melonDS than the one vendored here and every offset below
     * is suspect, so say so rather than render something wrong. */
    if (alen != 669u || blen != 113u) {
        fprintf(stderr, "unexpected 2D section sizes (%u, %u) - expected "
                        "669 and 113. Refusing to guess at the layout.\n",
                alen, blen);
        return 1;
    }

    /* Palette then OAM then the nine banks, after a 16-byte scalar header. */
    memcpy((void *)(uintptr_t)0x05000000u, gpug + 16, 2048);
    memcpy((void *)(uintptr_t)0x07000000u, gpug + 16 + 2048, 2048);

    off = 16 + 2048 + 2048;
    for (i = 0; i < 9; ++i) {
        /* Put each bank at its LCDC address first, then apply the real
         * VRAMCNT and let the port's own bank logic move it - which tests
         * that logic at the same time. */
        lsw2_vram_bank_write((unsigned)i, 0x80u);
        memcpy((void *)(uintptr_t)BANK_LCDC[i], gpug + off, BANK_BYTES[i]);
        off += BANK_BYTES[i];
    }
    for (i = 0; i < 9; ++i)
        lsw2_vram_bank_write((unsigned)i, gpug[off + i]);

    load_engine(ga, 0x04000000u);
    load_engine(gb, 0x04001000u);

    /* An optional fourth argument restricts engine B to a single layer, which
     * is how a defect gets attributed to the layer that causes it rather than
     * guessed at from the composite. 0-3 are the backgrounds, 4 is OBJ. */
    if (argc >= 5) {
        int layer = atoi(argv[4]);
        uint32_t dc = lsw2_io_read(0x04001000u, 4);
        dc &= ~0x1F00u;
        dc |= (layer == 4) ? 0x1000u : (1u << (8 + layer));
        lsw2_io_write(0x04001000u, 4, dc);
        printf("engine B restricted to layer %d\n", layer);
    }

    /* Engine A to the first output, B to the second, regardless of the swap
     * bit - which lives in a section this harness does not read. */
    lsw2_io_write(0x04000304u, 4, 0x8000u);

    lsw2_video_render(top, bottom);

    {
        char path[512];
        snprintf(path, sizeof path, "%s/engine_a.ppm", outdir);
        if (!write_ppm(path, top)) { fprintf(stderr, "cannot write %s\n", path); return 1; }
        snprintf(path, sizeof path, "%s/engine_b.ppm", outdir);
        if (!write_ppm(path, bottom)) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    }
    printf("wrote engine_a.ppm and engine_b.ppm to %s\n", outdir);

    /* An optional fifth argument is a captured polygon list; with it the 3D
     * layer is rasterised too and written alongside the 2D screens. */
    if (argc >= 6) {
        static uint32_t gx[256 * 192];
        char path[512];
        if (load_and_draw(argv[5])) {
            lsw2_gx_get_frame(gx);
            snprintf(path, sizeof path, "%s/engine_3d.ppm", outdir);
            write_ppm(path, gx);
            printf("wrote engine_3d.ppm\n");
            lsw2_gx_report();
        }
    }

    lsw2_vram_report();
    lsw2_video_report();
    return 0;
}
