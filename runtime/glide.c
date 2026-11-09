/* Glide 3 (glide3x.dll) for the recompiled voodoo2a.dll thrash driver.
 *
 * The game talks to EA's Voodoo2 thrash driver, which we recompile like the
 * rest; that driver talks Glide to us. Here Glide state is tracked, vertices
 * are read through grVertexLayout, texture memory (TMU) is emulated and
 * textures are converted to RGBA8 when selected, and every frame is recorded
 * as draw batches (gpu.h) that the GL renderer replays.
 *
 * Constants follow the Glide 3 headers (glide.h / sst1vid.h).
 */
#define _GNU_SOURCE
#include "rt.h"
#include "gpu.h"
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

typedef struct { const char *name; hostfn fn; } ModFn;

#define TMEM_SIZE (8u << 20)
static uint8_t *tmem;                       /* emulated texture memory, TMU0 */

static inline float F(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static inline float RF(uint32_t a) { float f; memcpy(&f, G + a, 4); return f; }

/* ------------------------------------------------------------- state */
static GState st;                           /* current state */
static GFrame *cur;                         /* frame being recorded */
static void front_flush(void);              /* front-buffer drawing (grRenderBuffer, below) */
static uint32_t cur_state = UINT32_MAX;     /* index of st in cur->states, if unchanged */
static int win_w = 640, win_h = 480;
static int color_format;                    /* GR_COLORFORMAT_ARGB/ABGR/RGBA/BGRA */
static int origin_lower;                    /* GR_ORIGIN_LOWER_LEFT */
static int cull_mode;
static uint8_t fog_table[64];
static uint8_t fog_lut[256];

/* vertex layout (byte offsets, -1 = absent) */
enum { P_XY, P_Z, P_W, P_Q, P_A, P_RGB, P_PARGB, P_ST0, P_Q0, P_N };
static int voff[P_N] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };

/* current texture source */
static struct { uint32_t start; int small, large, aspect, fmt; int slot; float sscale, tscale; } tsrc = { .slot = -1 };

static uint32_t argb_from(uint32_t c) {      /* app color format -> ARGB */
    switch (color_format) {
    case 1: return (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);                 /* ABGR */
    case 2: return (c >> 8) | (c << 24);                                                     /* RGBA */
    case 3: return ((c & 0xFF) << 24) | ((c & 0xFF00) << 8) | ((c >> 8) & 0xFF00) | (c >> 24); /* BGRA */
    default: return c;
    }
}

static void frame_begin(void) {
    if (!cur) { cur = gpu_frame_new(); cur->w = win_w; cur->h = win_h; cur_state = UINT32_MAX; }
}
#define GROW(arr, n, cap, T) do { if ((n) >= (cap)) { (cap) = (cap) ? (cap) * 2 : 256; (arr) = realloc((arr), (size_t)(cap) * sizeof(T)); } } while (0)

static void state_changed(void) { cur_state = UINT32_MAX; }

static GCmd *cmd_new(uint8_t type) {
    frame_begin();
    GROW(cur->cmds, cur->ncmds, cur->capcmds, GCmd);
    GCmd *c = &cur->cmds[cur->ncmds++];
    memset(c, 0, sizeof *c); c->type = type;
    return c;
}

static void tex_mark_drawn(void);
static void emit_tri(const GVert v[3]) {
    frame_begin();
    tex_mark_drawn();
    if (cur_state == UINT32_MAX || cur->ncmds == 0 || cur->cmds[cur->ncmds - 1].type != GC_DRAW ||
        cur->cmds[cur->ncmds - 1].state != cur_state) {
        if (cur_state == UINT32_MAX) {
            if (!cur->nstates || memcmp(&cur->states[cur->nstates - 1], &st, sizeof st)) {
                GROW(cur->states, cur->nstates, cur->capstates, GState);
                cur->states[cur->nstates++] = st;
            }
            cur_state = cur->nstates - 1;
        }
        GCmd *last = cur->ncmds ? &cur->cmds[cur->ncmds - 1] : NULL;
        if (!(last && last->type == GC_DRAW && last->state == cur_state)) {
            GCmd *c = cmd_new(GC_DRAW); c->state = cur_state; c->first = cur->nverts; c->count = 0;
        }
    }
    if (cur->nverts + 3 > cur->capverts) { cur->capverts = cur->capverts ? cur->capverts * 2 : 4096; cur->verts = realloc(cur->verts, cur->capverts * sizeof(GVert)); }
    memcpy(&cur->verts[cur->nverts], v, 3 * sizeof(GVert));
    cur->nverts += 3;
    cur->cmds[cur->ncmds - 1].count += 3;
}

static int glide2;                          /* Glide 2 API (glide2x.dll): fixed GrVertex, reversed LOD/aspect codes */
#define G2_SNAP 786432.0f                   /* 3 << 18: Glide 2 apps add it to x and y to snap to 1/16 pixel */

static void read_vertex(uint32_t p, GVert *o) {
    o->x = RF(p + (uint32_t)voff[P_XY]); o->y = RF(p + (uint32_t)voff[P_XY] + 4);
    if (glide2) { if (o->x > G2_SNAP / 2) o->x -= G2_SNAP; if (o->y > G2_SNAP / 2) o->y -= G2_SNAP; }
    if (origin_lower) o->y = (float)win_h - o->y;
    float oow = 1.0f;
    if (voff[P_Q] >= 0) oow = RF(p + (uint32_t)voff[P_Q]);
    else if (voff[P_W] >= 0) oow = RF(p + (uint32_t)voff[P_W]);
    if (!(oow > 1e-9f)) oow = 1e-9f;
    o->w = 1.0f / oow;
    if (st.depth_mode == 1 || st.depth_mode == 3) {           /* Z buffer: z in 0..65535 */
        float z = voff[P_Z] >= 0 ? RF(p + (uint32_t)voff[P_Z]) : 0.f;
        o->depth = z / 65535.0f;
    } else {                                                  /* W buffer (or none): linear in 1/w */
        o->depth = 1.0f - (oow > 1.0f ? 1.0f : oow);
    }
    if (st.depth_bias) o->depth += (float)st.depth_bias / 65536.0f;
    if (voff[P_PARGB] >= 0) {
        uint32_t c = argb_from(RD32(p + (uint32_t)voff[P_PARGB]));
        o->a = (float)(c >> 24) / 255.f; o->r = (float)((c >> 16) & 255) / 255.f;
        o->g = (float)((c >> 8) & 255) / 255.f; o->b = (float)(c & 255) / 255.f;
    } else {
        if (voff[P_RGB] >= 0) {
            o->r = RF(p + (uint32_t)voff[P_RGB]) / 255.f; o->g = RF(p + (uint32_t)voff[P_RGB] + 4) / 255.f; o->b = RF(p + (uint32_t)voff[P_RGB] + 8) / 255.f;
        } else o->r = o->g = o->b = 1.f;
        o->a = voff[P_A] >= 0 ? RF(p + (uint32_t)voff[P_A]) / 255.f : 1.f;
    }
    if (voff[P_ST0] >= 0) {
        /* Glide 3 vertices carry s/w and t/w in "256 space" */
        float sow = RF(p + (uint32_t)voff[P_ST0]), tow = RF(p + (uint32_t)voff[P_ST0] + 4);
        float q = oow;
        if (voff[P_Q0] >= 0) { float q0 = RF(p + (uint32_t)voff[P_Q0]); if (q0 > 1e-9f) q = q0; }
        o->s = sow / q * tsrc.sscale; o->t = tow / q * tsrc.tscale;
    } else o->s = o->t = 0.f;
}

/* ----------------------------------------------------------- textures */
static int fmt_bpp(int fmt) { return fmt < 8 ? 1 : 2; }
static void lod_dims(int lod, int aspect, int *w, int *h) {
    int big = 1 << lod;
    if (aspect >= 0) { *w = big; *h = big >> aspect; } else { *h = big; *w = big >> -aspect; }
    if (*w < 1) *w = 1; if (*h < 1) *h = 1;
}
static uint32_t mip_bytes(int small, int large, int aspect, int fmt) {
    uint32_t n = 0;
    for (int l = large; l >= small; l--) { int w, h; lod_dims(l, aspect, &w, &h); n += (uint32_t)(w * h * fmt_bpp(fmt)); }
    return n;
}

static uint32_t palette[256];
static uint32_t pal_version = 1;   /* bumped by palette and NCC table downloads: textures using them convert again */
/* NCC (YIQ) tables, GuNccTable: yRGB[16] (u8), iRGB[4][3] and qRGB[4][3] (s16), packed data (unused here).
 * The game never calls grTexNCCTable, so table 0 is the one in use. */
static struct { int16_t y[16], i[4][3], q[4][3]; } ncc[2];
static void ncc_load(int t, uint32_t p) {
    for (int k = 0; k < 16; k++) ncc[t].y[k] = G[p + k];
    for (int k = 0; k < 4; k++) for (int c = 0; c < 3; c++) {
        ncc[t].i[k][c] = (int16_t)RD16(p + 16 + (uint32_t)(k * 3 + c) * 2);
        ncc[t].q[k][c] = (int16_t)RD16(p + 40 + (uint32_t)(k * 3 + c) * 2);
    }
    pal_version++;
}

typedef struct { uint32_t start, size; int small, large, aspect, fmt; uint32_t pal_ver; int dirty, used; uint32_t gone, drawn; } TexKey;
static TexKey tkeys[GPU_MAX_TEX];
static int ntkeys;
/* A texture whose TMU memory another texture was downloaded over is gone: its slot is dropped
 * (without this the cache only grew, a slot and its pixels for every texture ever seen, until
 * the heap ran out after an hour or so of racing). A dropped slot keeps its pixels and is not
 * handed out again for TEX_GRACE swaps, since frames already published may still draw with it. */
#define TEX_GRACE 3
static uint32_t swaps;
static int ngone;
static void tex_mark_drawn(void) { if (st.tex >= 0 && st.tex < ntkeys) tkeys[st.tex].drawn = swaps + 1; }
static void tex_drop_overlapping(int slot) {
    const TexKey *k = &tkeys[slot];
    for (int i = 0; i < ntkeys; i++) {
        TexKey *o = &tkeys[i];
        if (i == slot || !o->used || !o->dirty) continue;
        if (k->start < o->start + o->size && o->start < k->start + k->size) { o->used = 0; o->gone = swaps; ngone++; }
    }
}
static void tex_sweep(void) {                                     /* free the pixels of slots dropped a while ago */
    if (!ngone) return;
    int left = 0;
    gpu_tex_lock();
    for (int i = 0; i < ntkeys; i++) {
        GTex *t = gpu_tex(i);
        if (tkeys[i].used || !t->rgba) continue;
        if (swaps - tkeys[i].gone < TEX_GRACE) left++; else { free(t->rgba); t->rgba = NULL; }
    }
    gpu_tex_unlock();
    ngone = left;
}

static inline uint32_t px_argb(int fmt, const uint8_t *s) {
    uint32_t p = fmt_bpp(fmt) == 2 ? (uint32_t)(s[0] | (s[1] << 8)) : s[0];
    uint32_t a = 255, r, g, b;
    switch (fmt) {
    case 0x0: r = (p >> 5) & 7; g = (p >> 2) & 7; b = p & 3; r = (r << 5) | (r << 2) | (r >> 1); g = (g << 5) | (g << 2) | (g >> 1); b *= 85; break;
    case 0x2: a = p; r = g = b = 255; break;
    case 0x3: r = g = b = p; break;
    case 0x4: a = (p >> 4) * 17; r = g = b = (p & 15) * 17; break;
    case 0x5: case 0x6: { uint32_t c = palette[p]; r = (c >> 16) & 255; g = (c >> 8) & 255; b = c & 255; if (fmt == 6) a = c >> 24; break; }
    case 0x8: a = p >> 8; r = (p >> 5) & 7; g = (p >> 2) & 7; b = p & 3; r = (r << 5) | (r << 2) | (r >> 1); g = (g << 5) | (g << 2) | (g >> 1); b *= 85; break;
    case 0xa: r = (p >> 11) & 31; g = (p >> 5) & 63; b = p & 31; r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2); break;
    case 0xb: a = (p & 0x8000) ? 255 : 0; r = (p >> 10) & 31; g = (p >> 5) & 31; b = p & 31; r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2); break;
    case 0xc: a = (p >> 12) * 17; r = ((p >> 8) & 15) * 17; g = ((p >> 4) & 15) * 17; b = (p & 15) * 17; break;
    case 0xd: a = p >> 8; r = g = b = p & 255; break;
    case 0xe: { uint32_t c = palette[p & 255]; a = p >> 8; r = (c >> 16) & 255; g = (c >> 8) & 255; b = c & 255; break; }
    case 0x1: case 0x9: {                                             /* YIQ_422 / AYIQ_8422: NCC table 0 */
        uint32_t t = p & 255, Y = t >> 4, I = (t >> 2) & 3, Q = t & 3; int v[3];
        if (fmt == 9) a = p >> 8;
        for (int n = 0; n < 3; n++) { v[n] = ncc[0].y[Y] + ncc[0].i[I][n] + ncc[0].q[Q][n]; v[n] = v[n] < 0 ? 0 : v[n] > 255 ? 255 : v[n]; }
        r = (uint32_t)v[0]; g = (uint32_t)v[1]; b = (uint32_t)v[2]; break;
    }
    default: r = 255; g = 0; b = 255; break;                          /* not supported: magenta */
    }
    return (a << 24) | (b << 16) | (g << 8) | r;                     /* RGBA8 bytes in memory: r,g,b,a */
}

static int tex_convert(int slot) {
    TexKey *k = &tkeys[slot];
    { static int lg = -1; if (lg < 0) lg = getenv("RT_TEX_LOG") ? atoi(getenv("RT_TEX_LOG")) : 0;
      if (lg) { int w0, h0; lod_dims(k->large, k->aspect, &w0, &h0); int mg = 0; const uint8_t *s0 = tmem + k->start;
        for (int i = 0; i < w0 * h0; i++) { uint32_t v = px_argb(k->fmt, s0 + (uint32_t)i * (uint32_t)fmt_bpp(k->fmt)); if ((v & 0xFFFFFF) == 0xFF00FF) mg++; }
        if (mg || lg > 1) rt_log("tex %d: fmt %#x lod %d..%d aspect %d at %#x: %d magenta texels, chroma %u", slot, k->fmt, k->small, k->large, k->aspect, k->start, mg, st.chroma_mode); } }
    tex_drop_overlapping(slot);
    int w, h; lod_dims(k->large, k->aspect, &w, &h);
    int levels = k->large - k->small + 1;
    uint32_t total = 0;
    for (int l = k->large; l >= k->small; l--) { int lw, lh; lod_dims(l, k->aspect, &lw, &lh); total += (uint32_t)(lw * lh); }
    uint32_t *out = malloc(total * 4);
    const uint8_t *src = tmem + k->start;
    uint32_t o = 0, bpp = (uint32_t)fmt_bpp(k->fmt);
    for (int l = k->large; l >= k->small; l--) {
        int lw, lh; lod_dims(l, k->aspect, &lw, &lh);
        for (int i = 0; i < lw * lh; i++) out[o + (uint32_t)i] = px_argb(k->fmt, src + (uint32_t)i * bpp);
        src += (uint32_t)(lw * lh) * bpp; o += (uint32_t)(lw * lh);
    }
    gpu_tex_lock();
    GTex *t = gpu_tex(slot);
    free(t->rgba);
    t->rgba = out; t->w = w; t->h = h; t->levels = levels; t->version++;
    gpu_tex_unlock();
    k->dirty = 0; k->pal_ver = pal_version;
    return slot;
}

static int tex_lookup(uint32_t start, int small, int large, int aspect, int fmt) {
    for (int i = 0; i < ntkeys; i++) {
        TexKey *k = &tkeys[i];
        if (k->used && k->start == start && k->large == large && k->small == small && k->aspect == aspect && k->fmt == fmt) {
            int stale = k->dirty || ((fmt == 5 || fmt == 6 || fmt == 0xe || fmt == 1 || fmt == 9) && k->pal_ver != pal_version);
            if (!stale) return i;
            /* new pixels for a texture this frame already draws with (NFS II SE's dialogs load
             * every backdrop tile into the same TMU spot, drawing each in between): the frame is
             * drawn later, so keep the old pixels in their slot and give the new ones a fresh slot */
            if (k->drawn == swaps + 1) { k->used = 0; k->gone = swaps; ngone++; break; }
            return tex_convert(i);
        }
    }
    int slot = -1;
    for (int i = 0; i < ntkeys; i++) if (!tkeys[i].used && swaps - tkeys[i].gone >= TEX_GRACE) { slot = i; break; }
    if (slot < 0) { if (ntkeys >= GPU_MAX_TEX) { static int w; if (!w++) rt_log("textures: all %d slots in use, reusing slot 0", GPU_MAX_TEX); slot = 0; } else slot = ntkeys++; }
    tkeys[slot] = (TexKey){ start, mip_bytes(small, large, aspect, fmt), small, large, aspect, fmt, 0, 1, 1, 0, 0 };
    return tex_convert(slot);
}

static void tmem_written(uint32_t start, uint32_t len) {
    for (int i = 0; i < ntkeys; i++) {
        TexKey *k = &tkeys[i];
        if (!k->used) continue;
        if (start < k->start + k->size && k->start < start + len) {
            /* a different texture overwrote this one: drop it; same place again: reconvert lazily */
            k->dirty = 1;
        }
    }
    if (tsrc.slot >= 0 && tkeys[tsrc.slot].dirty) {
        tsrc.slot = tex_lookup(tsrc.start, tsrc.small, tsrc.large, tsrc.aspect, tsrc.fmt);
        if (st.tex != tsrc.slot) { st.tex = tsrc.slot; state_changed(); }
    }
}

/* -------------------------------------------------------------- API */
#define A(i) (a[i])
#define GL_(name) static uint32_t gr_##name(CPU *c, const uint32_t *a)
#define UNUSED (void)c; (void)a

static uint32_t guest_str(const char *s) {
    uint32_t p = galloc((uint32_t)strlen(s) + 1); memcpy(G + p, s, strlen(s) + 1); return p;
}

GL_(GlideInit) {
    UNUSED;
    if (!tmem) tmem = calloc(1, TMEM_SIZE);
    st = (GState){ .tex = -1, .cc_func = 1 /* LOCAL */, .ac_func = 1, .tc_rgb_func = 1, .tc_a_func = 1,
                   .alpha_test = 7, .blend_src = 4 /* ONE */, .blend_dst = 0, .blend_asrc = 4, .blend_adst = 0,
                   .depth_func = 1, .depth_mask = 1, .color_mask = 1, .constant_color = 0xFFFFFFFFu,
                   .clip = { 0, 0, 640, 480 } };
    return 0;
}
GL_(GlideShutdown) { UNUSED; return 0; }
GL_(ErrorSetCallback) { UNUSED; return 0; }
GL_(SstSelect) { UNUSED; return 0; }
GL_(Finish) { UNUSED; front_flush(); return 0; }
GL_(DitherMode) { UNUSED; return 0; }
GL_(Enable) { UNUSED; return 0; }
GL_(Disable) { UNUSED; return 0; }
GL_(CoordinateSpace) { UNUSED; return 0; }
GL_(TexLodBiasValue) { UNUSED; return 0; }
GL_(GetProcAddress) { UNUSED; return 0; }
GL_(SstWinClose) { UNUSED; return 1; }
GL_(gammaRGB) { UNUSED; return 0; }

GL_(Get) {
    (void)c;
    uint32_t pname = A(0), plen = A(1), p = A(2);
    uint32_t v[4] = { 0 }; int n = 1;
    switch (pname) {
    case 0x01: v[0] = 16; break;                                  /* GR_BITS_DEPTH */
    case 0x02: v[0] = 5; v[1] = 6; v[2] = 5; v[3] = 0; n = 4; break;  /* GR_BITS_RGBA */
    case 0x04: v[0] = 64; break;                                  /* GR_FOG_TABLE_ENTRIES */
    case 0x05: v[0] = 32; break;                                  /* GR_GAMMA_TABLE_ENTRIES */
    case 0x06: v[0] = 1024; break;                                /* GR_GLIDE_STATE_SIZE */
    case 0x07: v[0] = 256; break;                                 /* GR_GLIDE_VERTEXLAYOUT_SIZE */
    case 0x0a: v[0] = 256; break;                                 /* GR_MAX_TEXTURE_SIZE */
    case 0x0b: v[0] = 3; break;                                   /* GR_MAX_TEXTURE_ASPECT_RATIO */
    case 0x0c: v[0] = 4u << 20; break;                            /* GR_MEMORY_FB */
    case 0x0d: v[0] = TMEM_SIZE; break;                           /* GR_MEMORY_TMU */
    case 0x0f: v[0] = 1; break;                                   /* GR_NUM_BOARDS */
    case 0x11: v[0] = 1; break;                                   /* GR_NUM_FB */
    case 0x13: v[0] = 1; break;                                   /* GR_NUM_TMU */
    case 0x15: case 0x16: v[0] = 2; break;                        /* GR_REVISION_FB/TMU */
    case 0x14: v[0] = 0; break;                                   /* GR_PENDING_BUFFERSWAPS */
    case 0x23: v[0] = 1; break;                                   /* GR_SUPPORTS_PASSTHRU */
    case 0x24: v[0] = 16; break;                                  /* GR_TEXTURE_ALIGN */
    case 0x25: {                                                  /* GR_VIDEO_POSITION: emulated 60 Hz raster, 0 in vblank */
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        double ph = fmod((double)t.tv_sec * 60.0 + t.tv_nsec * 60e-9, 1.0);
        uint32_t line = (uint32_t)(ph * 525.0);
        if (line < 480) {
            /* the driver spins on this: sleep towards the blank instead of burning the CPU */
            double left = (480.0 / 525.0 - ph) / 60.0;
            if (left > 0.0015) rt_usleep((useconds_t)((left - 0.001) * 1e6));
            v[0] = line + 1;
        } else v[0] = 0;
        break;
    }
    case 0x26: v[0] = 0; v[1] = 0; v[2] = (uint32_t)win_w; v[3] = (uint32_t)win_h; n = 4; break;  /* GR_VIEWPORT */
    case 0x27: v[0] = 0; v[1] = 65535; n = 2; break;              /* GR_WDEPTH_MIN_MAX */
    case 0x28: v[0] = 65535; v[1] = 0; n = 2; break;              /* GR_ZDEPTH_MIN_MAX */
    case 0x2a: v[0] = 8; break;                                   /* GR_BITS_GAMMA */
    default: v[0] = 0; break;
    }
    if (rt_verbose > 1) rt_log("grGet(%#x) -> %u", pname, v[0]);
    uint32_t bytes = (uint32_t)n * 4; if (bytes > plen) bytes = plen;
    if (p) memcpy(G + p, v, bytes);
    return bytes;
}
GL_(GetString) {
    (void)c;
    static uint32_t s[5];
    int i = (int)A(0) - 0xa0; if (i < 0 || i > 4) return 0;
    static const char *txt[5] = { "", "Voodoo2", "Glide", "3Dfx Interactive", "3.10.00.0658" };
    if (!s[i]) s[i] = guest_str(txt[i]);
    return s[i];
}
/* Modes by GR_RESOLUTION_* code. Glide's own list stops at 4:3 and a few odd sizes (no
 * 1920x1080), so the codes nobody uses now carry widescreen modes. The game learns the
 * sizes from a table in the 3dfx driver (voodoo2a.dll), which glide_patch_driver() rewrites
 * to match when the driver loads. 640x480, 800x600, 856x480 and 1024x768 keep their codes
 * (configs saved before still pick the same mode). 0 = not offered. Nothing wider than
 * 16:9: the game (like the Modern Patch says, 5:4 to 16:9) never finishes loading a race
 * in such a mode; the page letterboxes 16:9 on an ultrawide screen instead. */
#define NRES 24
static int res_w[NRES] = {
    1280, 1366, 1600, 1920, 2560, 3840, 1280,  640,  800, 1440,  856, 1680,
    1024, 1280, 1600, 1920, 1152, 1280, 2560,    0,    0, 1920, 2048,    0 };
static int res_h[NRES] = {
     720,  768,  900, 1080, 1440, 2160,  800,  480,  600,  900,  480, 1050,
     768, 1024, 1200, 1200,  864,  960, 1600,    0,    0, 1440, 1536,    0 };
void glide_patch_driver(uint32_t base, uint32_t size) {
    /* the driver's table: (u16 w, u16 h) for codes 0..23, Glide's standard sizes */
    static const uint16_t orig[] = { 320, 200, 320, 240, 400, 256, 512, 384, 640, 200, 640, 350, 640, 400, 640, 480 };
    for (uint32_t o = 0; o + NRES * 4 <= size; o += 2) {
        if (memcmp(G + base + o, orig, sizeof orig)) continue;
        for (int i = 0; i < NRES; i++) { WR16(base + o + 4 * i, (uint16_t)res_w[i]); WR16(base + o + 4 * i + 2, (uint16_t)res_h[i]); }
        rt_log("glide: driver mode table at %#x now has the widescreen modes", base + o);
        return;
    }
}
GL_(QueryResolutions) {
    (void)c;
    /* every mode in res_w/res_h (see there) @60 Hz, 2 color buffers, 1 aux; the game sorts
     * them for its graphics options. A template with a set resolution (not GR_QUERY_ANY)
     * narrows the list. */
    uint32_t modes[32], nm = 0;
    const char *e = getenv("RT_GLIDE_MODES");     /* e.g. "c" = only 1024x768 (testing) */
    if (e && *e) { for (char *q = (char *)e; *q && nm < 32;) { modes[nm++] = (uint32_t)strtoul(q, &q, 16); while (*q == ',') q++; } }
    else {
        /* RT_SCREEN=WxH (the page passes the screen's pixels): like a PC offering only what its
         * monitor shows, hide modes bigger than that (the game starts in the biggest one) */
        int sw = 0, sh = 0; const char *s = getenv("RT_SCREEN");
        if (s) sscanf(s, "%dx%d", &sw, &sh);
        for (uint32_t i = 0; i < NRES; i++) {
            if (!res_w[i]) continue;
            int fits = sw <= 0 || (res_w[i] <= sw && res_h[i] <= sh) || res_w[i] <= 800;
            if (fits) modes[nm++] = i;
        }
    }
    uint32_t want = A(0) ? RD32(A(0)) : 0xFFFFFFFFu, n = 0;
    if (want > 0x17) want = 0xFFFFFFFFu;          /* not a GR_RESOLUTION_* code: list all */
    for (unsigned i = 0; i < nm; i++) {
        if (want != 0xFFFFFFFFu && want != modes[i]) continue;
        if (A(1)) { uint32_t r[4] = { modes[i], 0, 2, 1 }; memcpy(G + A(1) + n * 16, r, 16); }
        n++;
    }
    static int logged; if (logged++ < 40) rt_log("grQueryResolutions(want %#x): %u mode(s)", want, n);
    return (int)(n * 16);
}
GL_(SstWinOpen) {
    (void)c;
    uint32_t res = A(1);
    if (res < NRES && res_w[res]) { win_w = res_w[res]; win_h = res_h[res]; }
    color_format = (int)A(3); origin_lower = A(4) == 1;
    st.clip[0] = 0; st.clip[1] = 0; st.clip[2] = (int16_t)win_w; st.clip[3] = (int16_t)win_h;
    rt_log("grSstWinOpen(res=%#x -> %dx%d, colorfmt=%u, origin=%u, bufs=%u/%u)", res, win_w, win_h, A(3), A(4), A(5), A(6));
    return 1;
}
/* Front-buffer drawing. NFS II SE's network screens switch to the front buffer
 * (grRenderBuffer(GR_BUFFER_FRONTBUFFER)) or write the front buffer through the LFB, draw a
 * panel straight onto the visible picture and then wait for the other players without
 * swapping. Those draws are recorded in a frame of their own and published as an incremental
 * frame (no full clear: the renderer draws it over the last picture) when the game goes back to
 * the back buffer, finishes an LFB write, or waits for the chip (grSstIdle / grFinish).
 * Back-buffer work in progress is set aside meanwhile. */
static GFrame *back_saved; static int front_mode;
static void front_begin(void) { if (!front_mode) { back_saved = cur; cur = NULL; state_changed(); front_mode = 1; } }
static void front_flush(void) {
    if (!front_mode || !cur || !cur->ncmds) return;
    memcpy(cur->fog_lut, fog_lut, sizeof fog_lut);
    GFrame *f = cur; cur = NULL; state_changed();
    gpu_publish(f);
}
static void front_end(void) { if (front_mode) { front_flush(); gpu_frame_free(cur); cur = back_saved; back_saved = NULL; state_changed(); front_mode = 0; } }
GL_(RenderBuffer) {
    (void)c;
    { static int lg = -1; if (lg < 0) lg = getenv("RT_LFB_LOG") != NULL; if (lg) rt_log("grRenderBuffer(%u) from %#x", A(0), rt_caller()); }
    if (A(0) == 0) front_begin(); else front_end();      /* 0 front, 1 back (Glide 2 and 3) */
    return 0;
}

GL_(VertexLayout) {
    (void)c;
    uint32_t param = A(0), off = A(1), mode = A(2);
    int i = -1;
    switch (param) {
    case 0x01: i = P_XY; break; case 0x02: i = P_Z; break; case 0x03: i = P_W; break; case 0x04: i = P_Q; break;
    case 0x10: i = P_A; break; case 0x20: i = P_RGB; break; case 0x30: i = P_PARGB; break;
    case 0x40: i = P_ST0; break; case 0x50: i = P_Q0; break;
    default: if (rt_verbose) rt_log("grVertexLayout: param %#x ignored", param); return 0;
    }
    voff[i] = mode ? (int)off : -1;
    if (rt_verbose) rt_log("grVertexLayout(param=%#x, offset=%u, mode=%u)", param, off, mode);
    return 0;
}

static void draw3(uint32_t pa, uint32_t pb, uint32_t pc) {
    GVert v[3];
    static int dumped;
    if (rt_verbose > 2 && dumped < 6) {
        dumped++;
        char h[400]; int k = 0;
        for (int i = 0; i < 64; i += 4) { uint32_t w = RD32(pa + (uint32_t)i); k += snprintf(h + k, sizeof h - k, "%08x(%g) ", w, (double)F(w)); }
        rt_log("vertex @%#x: %s", pa, h);
    }
    read_vertex(pa, &v[0]); read_vertex(pb, &v[1]); read_vertex(pc, &v[2]);
    if (cull_mode) {
        float area = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[2].x - v[0].x) * (v[1].y - v[0].y);
        if (origin_lower) area = -area;
        if ((cull_mode == 1 && area < 0) || (cull_mode == 2 && area > 0)) return;
    }
    emit_tri(v);
}
GL_(DrawTriangle) { (void)c; draw3(A(0), A(1), A(2)); return 0; }
GL_(AADrawTriangle) { (void)c; draw3(A(0), A(1), A(2)); return 0; }
static void draw_line(uint32_t pa, uint32_t pb) {
    GVert v[2]; read_vertex(pa, &v[0]); read_vertex(pb, &v[1]);
    float dx = v[1].x - v[0].x, dy = v[1].y - v[0].y, len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-6f) return;
    float nx = -dy / len * 0.5f, ny = dx / len * 0.5f;
    GVert q[4] = { v[0], v[1], v[1], v[0] };
    q[0].x += nx; q[0].y += ny; q[1].x += nx; q[1].y += ny; q[2].x -= nx; q[2].y -= ny; q[3].x -= nx; q[3].y -= ny;
    GVert t1[3] = { q[0], q[1], q[2] }, t2[3] = { q[0], q[2], q[3] };
    emit_tri(t1); emit_tri(t2);
}
GL_(DrawLine) { (void)c; draw_line(A(0), A(1)); return 0; }
GL_(DrawPoint) {
    (void)c;
    GVert v; read_vertex(A(0), &v);
    GVert q[4] = { v, v, v, v };
    q[0].x -= .5f; q[0].y -= .5f; q[1].x += .5f; q[1].y -= .5f; q[2].x += .5f; q[2].y += .5f; q[3].x -= .5f; q[3].y += .5f;
    GVert t1[3] = { q[0], q[1], q[2] }, t2[3] = { q[0], q[2], q[3] };
    emit_tri(t1); emit_tri(t2);
    return 0;
}

GL_(BufferClear) {
    (void)c;
    GCmd *k = cmd_new(GC_CLEAR);
    k->color = argb_from(A(0)); k->color = (k->color & 0x00FFFFFFu) | ((A(1) & 0xFF) << 24);
    k->clear_color_on = st.color_mask;
    k->clear_depth_on = st.depth_mode != 0 && st.depth_mask;
    k->depth = (float)(A(2) & 0xFFFF) / 65535.0f;
    memcpy(k->clip, st.clip, sizeof k->clip);
    return 0;
}
GL_(BufferSwap) {
    (void)c; (void)a;
    frame_begin();
    memcpy(cur->fog_lut, fog_lut, sizeof fog_lut);
    GFrame *f = cur; cur = NULL;
    gpu_publish(f);
    swaps++; tex_sweep();
    return 0;
}

GL_(ClipWindow) {
    (void)c;
    int16_t cl[4] = { (int16_t)A(0), (int16_t)A(1), (int16_t)A(2), (int16_t)A(3) };
    if (origin_lower) { int16_t y0 = (int16_t)(win_h - cl[3]), y1 = (int16_t)(win_h - cl[1]); cl[1] = y0; cl[3] = y1; }
    memcpy(st.clip, cl, sizeof cl); state_changed(); return 0;
}
GL_(CullMode) { (void)c; cull_mode = (int)A(0); return 0; }

GL_(ColorCombine) { (void)c; st.cc_func = (uint8_t)A(0); st.cc_factor = (uint8_t)A(1); st.cc_local = (uint8_t)A(2); st.cc_other = (uint8_t)A(3); st.cc_invert = (uint8_t)A(4); state_changed(); return 0; }
GL_(AlphaCombine) { (void)c; st.ac_func = (uint8_t)A(0); st.ac_factor = (uint8_t)A(1); st.ac_local = (uint8_t)A(2); st.ac_other = (uint8_t)A(3); st.ac_invert = (uint8_t)A(4); state_changed(); return 0; }
GL_(TexCombine) {
    (void)c;
    if (A(0) != 0) return 0;
    st.tc_rgb_func = (uint8_t)A(1); st.tc_rgb_factor = (uint8_t)A(2); st.tc_a_func = (uint8_t)A(3); st.tc_a_factor = (uint8_t)A(4);
    st.tc_rgb_invert = (uint8_t)A(5); st.tc_a_invert = (uint8_t)A(6); state_changed(); return 0;
}
GL_(AlphaBlendFunction) { (void)c; st.blend_src = (uint8_t)A(0); st.blend_dst = (uint8_t)A(1); st.blend_asrc = (uint8_t)A(2); st.blend_adst = (uint8_t)A(3); state_changed(); return 0; }
GL_(AlphaTestFunction) { (void)c; st.alpha_test = (uint8_t)A(0); state_changed(); return 0; }
GL_(AlphaTestReferenceValue) { (void)c; st.alpha_ref = (uint8_t)A(0); state_changed(); return 0; }
GL_(DepthBufferMode) { (void)c; st.depth_mode = (uint8_t)A(0); state_changed(); return 0; }
GL_(DepthBufferFunction) { (void)c; st.depth_func = (uint8_t)A(0); state_changed(); return 0; }
GL_(DepthMask) { (void)c; st.depth_mask = (uint8_t)(A(0) != 0); state_changed(); return 0; }
GL_(DepthBiasLevel) { (void)c; st.depth_bias = (int16_t)A(0); return 0; }
GL_(FogMode) { (void)c; st.fog_mode = (uint8_t)(A(0) & 0xFF); state_changed(); return 0; }
GL_(FogColorValue) { (void)c; st.fog_color = argb_from(A(0)); state_changed(); return 0; }
/* fog_lut[i]: density at w = 2^(i/16), interpolated from the 64-entry table
 * indexed like guFogTableIndexToW: w(i) = 2^(3 + i/4) / (8 - i%4) */
GL_(FogTable) {
    (void)c;
    memcpy(fog_table, G + A(0), 64);
    double tw[64];
    for (int i = 0; i < 64; i++) tw[i] = pow(2.0, 3.0 + (double)(i >> 2)) / (8 - (i & 3));
    for (int j = 0; j < 256; j++) {
        double w = pow(2.0, j / 16.0);
        int i = 0; while (i < 63 && tw[i + 1] < w) i++;
        double f;
        if (w <= tw[0]) f = fog_table[0];
        else if (i >= 63) f = fog_table[63];
        else { double t = (w - tw[i]) / (tw[i + 1] - tw[i]); f = fog_table[i] + (fog_table[i + 1] - fog_table[i]) * t; }
        fog_lut[j] = (uint8_t)(f + 0.5);
    }
    return 0;
}
GL_(ChromakeyMode) { (void)c; if (getenv("RT_TEX_LOG") && st.chroma_mode != (uint8_t)A(0)) rt_log("grChromakeyMode(%u)", A(0)); st.chroma_mode = (uint8_t)A(0); state_changed(); return 0; }
GL_(ChromakeyValue) { (void)c; static uint32_t lastv = 1; if (A(0) != lastv && rt_verbose > 1) rt_log("grChromakeyValue(%#x)", A(0)); lastv = A(0); st.chroma_color = argb_from(A(0)); state_changed(); return 0; }

GL_(TexMinAddress) { UNUSED; return 0; }
GL_(TexMaxAddress) { UNUSED; return TMEM_SIZE - 8; }
GL_(TexCalcMemRequired) { (void)c; return (mip_bytes((int)A(0), (int)A(1), (int)A(2), (int)A(3)) + 7) & ~7u; }
GL_(TexClampMode) { (void)c; if (A(0) == 0) { st.tex_clamp_s = (uint8_t)A(1); st.tex_clamp_t = (uint8_t)A(2); state_changed(); } return 0; }
GL_(TexFilterMode) { (void)c; if (A(0) == 0) { st.tex_min_linear = (uint8_t)(A(1) != 0); st.tex_mag_linear = (uint8_t)(A(2) != 0); state_changed(); } return 0; }
GL_(TexMipMapMode) { (void)c; if (A(0) == 0) { st.tex_mip = (uint8_t)(A(1) != 0); state_changed(); } return 0; }

static void read_info(uint32_t p, int *small, int *large, int *aspect, int *fmt, uint32_t *data) {
    *small = (int32_t)RD32(p); *large = (int32_t)RD32(p + 4); *aspect = (int32_t)RD32(p + 8); *fmt = (int32_t)RD32(p + 12); *data = RD32(p + 16);
    /* Glide 2: GR_LOD_256 = 0 .. GR_LOD_1 = 8 and GR_ASPECT_8x1 = 0 .. 1x8 = 6; Glide 3 uses log2 sizes and log2 aspects */
    if (glide2) { *small = 8 - *small; *large = 8 - *large; *aspect = 3 - *aspect; }
}
GL_(TexDownloadMipMap) {
    (void)c;
    if (A(0) != 0) return 0;
    int small, large, aspect, fmt; uint32_t data;
    read_info(A(3), &small, &large, &aspect, &fmt, &data);
    uint32_t start = A(1), n = mip_bytes(small, large, aspect, fmt);
    if (start + n > TMEM_SIZE) { rt_log("grTexDownloadMipMap: %#x+%#x outside TMU memory", start, n); return 0; }
    memcpy(tmem + start, G + data, n);
    tmem_written(start, n);
    return 0;
}
GL_(TexDownloadMipMapLevelPartial) {
    (void)c;
    if (A(0) != 0) return 1;
    uint32_t start = A(1); int lod = (int)A(2), large = (int)A(3), aspect = (int)A(4), fmt = (int)A(5);
    uint32_t data = A(7); int r0 = (int)A(8), r1 = (int)A(9);
    uint32_t off = 0;
    for (int l = large; l > lod; l--) { int w, h; lod_dims(l, aspect, &w, &h); off += (uint32_t)(w * h * fmt_bpp(fmt)); }
    int w, h; lod_dims(lod, aspect, &w, &h);
    uint32_t row = (uint32_t)(w * fmt_bpp(fmt));
    uint32_t dst = start + off + (uint32_t)r0 * row, n = (uint32_t)(r1 - r0 + 1) * row;
    if (dst + n > TMEM_SIZE) return 1;
    memcpy(tmem + dst, G + data, n);
    tmem_written(dst, n);
    return 1;
}
GL_(TexDownloadTable) {
    (void)c;
    if (A(0) == 2 || A(0) == 3) { memcpy(palette, G + A(1), sizeof palette); pal_version++; }
    else if (A(0) <= 1) ncc_load((int)A(0), A(1));
    return 0;
}
GL_(TexSource) {
    (void)c;
    if (A(0) != 0) return 0;
    int small, large, aspect, fmt; uint32_t data;
    read_info(A(3), &small, &large, &aspect, &fmt, &data);
    tsrc.start = A(1); tsrc.small = small; tsrc.large = large; tsrc.aspect = aspect; tsrc.fmt = fmt;
    /* s,t run 0..256 along the longer side */
    tsrc.sscale = 1.0f / (aspect >= 0 ? 256.0f : (float)(256 >> -aspect));
    tsrc.tscale = 1.0f / (aspect >= 0 ? (float)(256 >> aspect) : 256.0f);
    tsrc.slot = tex_lookup(A(1), small, large, aspect, fmt);
    if (st.tex != tsrc.slot) { st.tex = tsrc.slot; state_changed(); }
    return 0;
}

/* ---- linear frame buffer: the game writes pixels into a buffer we give it;
 * on unlock they're sent to the renderer as an overlay (unwritten pixels stay
 * transparent: the buffer is pre-filled with a key the game never writes). */
#define LFB_KEY 0xF81Fu   /* RGB565 magenta with the low green bit clear */
static uint32_t lfb_buf, lfb_cap; static int lfb_locked, lfb_stride, lfb_pipe, lfb_buffer;
static const uint16_t *lfb_snap;            /* what the lock started with (the shown picture), or NULL */
GL_(LfbLock) {
    (void)c;
    uint32_t type = A(0) & 1, info = A(5);
    lfb_stride = win_w * 2;
    if (lfb_cap < (uint32_t)(win_w * win_h * 2)) {   /* grows with the mode */
        if (lfb_buf) gfree(lfb_buf);
        lfb_cap = (uint32_t)(win_w * win_h * 2); lfb_buf = galloc(lfb_cap);
    }
    /* the game reads the screen through the lock too (write-only or not: a Voodoo's LFB reads
     * back), e.g. to cut the menu backdrop into tiles under a sliding panel. So fill the buffer
     * with the picture last shown, and keep a copy: on unlock only pixels that changed are drawn.
     * Without a picture (no renderer) unwritten pixels keep a key colour instead. */
    static uint16_t *snap; static int snap_cap;
    if (snap_cap < win_w * win_h) { free(snap); snap_cap = win_w * win_h; snap = malloc((size_t)snap_cap * 2); }
    uint16_t *p = (uint16_t *)(G + lfb_buf);
    lfb_snap = gpu_readback565(snap, win_w, win_h) ? snap : NULL;
    if (lfb_snap) memcpy(p, snap, (size_t)win_w * win_h * 2);
    else for (int i = 0; i < win_w * win_h; i++) p[i] = type == 1 ? LFB_KEY : 0;
    lfb_locked = (int)type + 1; lfb_pipe = (int)A(4); lfb_buffer = (int)A(1);
    { static int lg = -1; if (lg < 0) lg = getenv("RT_LFB_LOG") != NULL; if (lg) rt_log("grLfbLock(type=%#x buffer=%u mode=%u origin=%u pipe=%u) from %#x", A(0), A(1), A(2), A(3), A(4), rt_caller()); }
    WR32(info + 4, lfb_buf); WR32(info + 8, (uint32_t)lfb_stride); WR32(info + 12, 0 /* 565 */); WR32(info + 16, 0);
    if (rt_verbose > 1) rt_log("grLfbLock(type=%#x buf=%u mode=%u)", A(0), A(1), A(2));
    return 1;
}
static void lfb_submit(const uint16_t *p, int x0, int y0, int w, int h, int stride_px, const uint16_t *snap) {
    { static int lg = -1; if (lg < 0) lg = getenv("RT_LFB_LOG") != NULL;
      if (lg) { int key = 0; for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) key += p[y * stride_px + x] == 0xF81F; static uint32_t hist[65536]; memset(hist, 0, sizeof hist); int top = 0; for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint16_t v = p[y * stride_px + x]; if (++hist[v] > hist[top]) top = v; }
        rt_log("lfb: %dx%d at %d,%d (%d key px) top %#06x x%u, chroma mode %u value %#x, pipeline %d, from %#x", w, h, x0, y0, key, top, hist[top], st.chroma_mode, st.chroma_color, lfb_pipe, rt_caller()); } }
    frame_begin();
    GROW(cur->lfbs, cur->nlfbs, cur->caplfbs, GLfb);
    GLfb *l = &cur->lfbs[cur->nlfbs++];
    l->w = win_w; l->h = win_h; l->rgba = calloc((size_t)(win_w * win_h), 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint16_t v = p[y * stride_px + x];
        if (snap ? v == snap[y * stride_px + x] : v == LFB_KEY) continue;
        int X = x0 + x, Y = y0 + y; if (X < 0 || Y < 0 || X >= win_w || Y >= win_h) continue;
        uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
        l->rgba[Y * win_w + X] = 0xFF000000u | (b << 16) | (g << 8) | r;
    }
    GCmd *k = cmd_new(GC_LFB); k->lfb = cur->nlfbs - 1;
}
/* LFB pixels for the front buffer (0) show at once, as an incremental frame; for the back buffer
 * they join the frame being recorded */
static void lfb_submit_to(int buffer, const uint16_t *p, int x0, int y0, int w, int h, int stride_px, const uint16_t *snap) {
    if (buffer != 0) { lfb_submit(p, x0, y0, w, h, stride_px, snap); return; }
    int was = front_mode;
    front_begin(); lfb_submit(p, x0, y0, w, h, stride_px, snap); front_flush();
    if (!was) front_end();
}
GL_(LfbUnlock) {
    (void)c; (void)a;
    if (lfb_locked == 2) lfb_submit_to(lfb_buffer, (const uint16_t *)(G + lfb_buf), 0, 0, win_w, win_h, win_w, lfb_snap);
    lfb_locked = 0;
    return 1;
}
GL_(LfbWriteRegion) {
    (void)c;
    /* dst_buffer, dst_x, dst_y, src_format, src_width, src_height, pixelPipeline, src_stride, src_data */
    if (A(3) != 0) { if (rt_verbose) rt_log("grLfbWriteRegion: format %u unsupported", A(3)); return 1; }
    lfb_submit((const uint16_t *)(G + A(8)), (int)A(1), (int)A(2), (int)A(4), (int)A(5), (int)(A(7) / 2), NULL);
    return 1;
}
GL_(LfbReadRegion) {
    (void)c;
    /* we can't read the GPU frame synchronously: hand back black */
    uint32_t w = A(3), h = A(4), stride = A(5), dst = A(6);
    { static int lg = -1; if (lg < 0) lg = getenv("RT_LFB_LOG") != NULL; if (lg) rt_log("grLfbReadRegion(buffer=%u, %u,%u %ux%u stride %u) from %#x", A(0), A(1), A(2), w, h, stride, rt_caller()); }
    for (uint32_t y = 0; y < h; y++) memset(G + dst + y * stride, 0, w * 2);
    return 1;
}


/* ------------------------------------------------------------ Glide 2
 * NFS II SE for 3Dfx loads glide2x.dll itself and calls 55 functions. Most take the same
 * arguments as Glide 3 and share the code above; the differences are handled here:
 *  - vertices are the fixed GrVertex (x y z r g b ooz a oow, then s/w t/w oow per TMU),
 *    with the snap constant still added to x and y (read_vertex);
 *  - LOD and aspect codes run the other way (read_info);
 *  - a Voodoo 1: one TMU with 2 MB, like the original card (and NFSIISE's wrapper). */
#define G2_TMEM (2u << 20)
GL_(G2GlideInit) {
    glide2 = 1;
    gr_GlideInit(c, a);
    /* GrVertex offsets */
    for (int i = 0; i < P_N; i++) voff[i] = -1;
    voff[P_XY] = 0; voff[P_RGB] = 12; voff[P_Z] = 24 /* ooz */; voff[P_A] = 28; voff[P_Q] = 32 /* oow */; voff[P_ST0] = 36;
    color_format = 0;   /* ARGB */
    rt_log("glide2: grGlideInit");
    return 0;
}
/* grBufferSwap(swap_interval): a Voodoo waited for that many vertical retraces before showing
 * the frame, and NFS II SE's front end leans on it for its pace (uncapped, its menus spin at
 * thousands of frames per second). A 60 Hz retrace clock; RT_VSYNC=0 turns it off. */
GL_(G2BufferSwap) {
    static int on = -1; static double next;
    if (on < 0) { const char *e = getenv("RT_VSYNC"); on = !e || atoi(e) != 0; }
    uint32_t interval = A(0);
    if (on && interval) {
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        double now = (double)t.tv_sec + t.tv_nsec * 1e-9, period = (double)interval / 60.0;
        if (next < now - period) next = now;          /* fell behind (loading): start over, no catch-up burst */
        next += period;
        if (next > now) rt_usleep((unsigned)((next - now) * 1e6));
    }
    return gr_BufferSwap(c, a);
}
GL_(G2GlideGetVersion) { (void)c; if (A(0)) strcpy(HSTR(A(0)), "Glide Version 2.46"); return 0; }
GL_(G2SstQueryHardware) {
    (void)c;
    /* GrHwConfiguration: num_sst, then SSTs[0] { type, VoodooConfig { fbRam, fbiRev, nTexelfx, sliDetect, tmuConfig[3] { tmuRev, tmuRam } } } */
    uint32_t p = A(0);
    if (p) {
        memset(G + p, 0, 4 + 4 * (4 + 4 * 4 + 3 * 8));
        WR32(p, 1); WR32(p + 4, 0 /* GR_SSTTYPE_VOODOO */); WR32(p + 8, 4); WR32(p + 12, 2); WR32(p + 16, 1); WR32(p + 20, 0);
        WR32(p + 24, 1); WR32(p + 28, G2_TMEM >> 20);
    }
    return 1;
}
GL_(G2SstIdle) { UNUSED; front_flush(); return 0; }
GL_(G2SstIsBusy) { UNUSED; return 0; }
GL_(G2SstStatus) { UNUSED; return 0x0FFFF03Fu; }   /* idle, no swaps pending, not in retrace */
GL_(G2GammaCorrectionValue) { UNUSED; return 0; }
GL_(G2TexMaxAddress) { UNUSED; return G2_TMEM - 8; }
GL_(G2TexCalcMemRequired) {
    (void)c;
    int small = 8 - (int)A(0), large = 8 - (int)A(1), aspect = 3 - (int)A(2);
    return (mip_bytes(small, large, aspect, (int)A(3)) + 7) & ~7u;
}
GL_(G2TexDownloadTable) {   /* (tmu, type, data) */
    (void)c;
    if (A(1) == 2 || A(1) == 3) { memcpy(palette, G + A(2), sizeof palette); pal_version++; }
    else if (A(1) <= 1) ncc_load((int)A(1), A(2));
    return 0;
}
GL_(G2TexCombineFunction) {
    /* the old one-call form of grTexCombine (GR_TEXTURECOMBINE_*) */
    (void)c;
    if (A(0) != 0) return 0;
    uint8_t f = 1, fac = 0, inv = 0;          /* DECAL: texture color and alpha */
    switch (A(1)) {
    case 0: f = 0; break;                     /* ZERO */
    case 2: f = 3; fac = 8; break;            /* OTHER: scale other by one */
    case 3: f = 4; fac = 8; break;            /* ADD */
    case 4: f = 3; fac = 1; break;            /* MULTIPLY: other * local */
    case 5: f = 6; fac = 8; break;            /* SUBTRACT */
    case 10: f = 0; inv = 1; break;           /* ONE */
    }
    st.tc_rgb_func = st.tc_a_func = f; st.tc_rgb_factor = st.tc_a_factor = fac; st.tc_rgb_invert = st.tc_a_invert = inv;
    state_changed();
    return 0;
}
GL_(G2LfbWriteRegion) {
    /* dst_buffer, dst_x, dst_y, src_format, src_width, src_height, src_stride, src_data (no pixelPipeline) */
    (void)c;
    if (A(3) != 0) { if (rt_verbose) rt_log("grLfbWriteRegion: format %u unsupported", A(3)); return 1; }
    lfb_submit_to((int)A(0), (const uint16_t *)(G + A(7)), (int)A(1), (int)A(2), (int)A(4), (int)A(5), (int)(A(6) / 2), NULL);
    return 1;
}
GL_(G2FogGenerateExp) {
    /* guFogGenerateExp(GrFog_t table[64], float density), as in the Glide 2 SDK */
    (void)c;
    float density = F(A(1));
    double wmax = pow(2.0, 3.0 + 15) / (8 - 3), dp = density * wmax;
    double scale = 255.0 / (1.0 - exp(-dp));
    for (int i = 0; i < 64; i++) {
        double w = pow(2.0, 3.0 + (double)(i >> 2)) / (8 - (i & 3));
        double f = (1.0 - exp(-density * w)) * scale;
        WR8(A(0) + (uint32_t)i, (uint8_t)(f > 255.0 ? 255.0 : f < 0.0 ? 0.0 : f));
    }
    return 0;
}

#define E(n, fn) { n, gr_##fn }
const ModFn glide_fns[] = {
    E("_grGlideInit@0", GlideInit), E("_grGlideShutdown@0", GlideShutdown), E("_grErrorSetCallback@4", ErrorSetCallback),
    E("_grGet@12", Get), E("_grGetString@4", GetString), E("_grGetProcAddress@4", GetProcAddress),
    E("_grQueryResolutions@8", QueryResolutions), E("_grSstSelect@4", SstSelect), E("_grSstWinOpen@28", SstWinOpen),
    E("_grSstWinClose@4", SstWinClose), E("_grRenderBuffer@4", RenderBuffer), E("_grFinish@0", Finish),
    E("_grVertexLayout@12", VertexLayout), E("_grCoordinateSpace@4", CoordinateSpace),
    E("_grDrawTriangle@12", DrawTriangle), E("_grAADrawTriangle@24", AADrawTriangle), E("_grDrawLine@8", DrawLine), E("_grDrawPoint@4", DrawPoint),
    E("_grBufferClear@12", BufferClear), E("_grBufferSwap@4", BufferSwap), E("_grClipWindow@16", ClipWindow), E("_grCullMode@4", CullMode),
    E("_grColorCombine@20", ColorCombine), E("_grAlphaCombine@20", AlphaCombine), E("_grTexCombine@28", TexCombine),
    E("_grAlphaBlendFunction@16", AlphaBlendFunction), E("_grAlphaTestFunction@4", AlphaTestFunction),
    E("_grAlphaTestReferenceValue@4", AlphaTestReferenceValue),
    E("_grDepthBufferMode@4", DepthBufferMode), E("_grDepthBufferFunction@4", DepthBufferFunction), E("_grDepthMask@4", DepthMask),
    E("_grDepthBiasLevel@4", DepthBiasLevel), E("_grDitherMode@4", DitherMode), E("_grEnable@4", Enable), E("_grDisable@4", Disable),
    E("_grFogMode@4", FogMode), E("_grFogColorValue@4", FogColorValue), E("_grFogTable@4", FogTable),
    E("_grChromakeyMode@4", ChromakeyMode), E("_grChromakeyValue@4", ChromakeyValue),
    E("_grTexMinAddress@4", TexMinAddress), E("_grTexMaxAddress@4", TexMaxAddress), E("_grTexCalcMemRequired@16", TexCalcMemRequired),
    E("_grTexClampMode@12", TexClampMode), E("_grTexFilterMode@12", TexFilterMode), E("_grTexMipMapMode@12", TexMipMapMode),
    E("_grTexLodBiasValue@8", TexLodBiasValue), E("_grTexDownloadMipMap@16", TexDownloadMipMap),
    E("_grTexDownloadMipMapLevelPartial@40", TexDownloadMipMapLevelPartial), E("_grTexDownloadTable@8", TexDownloadTable),
    E("_grTexSource@16", TexSource),
    E("_grLfbLock@24", LfbLock), E("_grLfbUnlock@8", LfbUnlock), E("_grLfbWriteRegion@36", LfbWriteRegion), E("_grLfbReadRegion@28", LfbReadRegion),
    E("_guGammaCorrectionRGB@12", gammaRGB),
    { 0 },
};

/* Glide 2 (glide2x.dll), NFS II SE for 3Dfx: shared entries where the arguments match Glide 3 */
#define E2(n, fn) { n, gr_##fn }
const ModFn glide2_fns[] = {
    E2("_grGlideInit@0", G2GlideInit), E2("_grGlideShutdown@0", GlideShutdown), E2("_grGlideGetVersion@4", G2GlideGetVersion),
    E2("_grSstQueryHardware@4", G2SstQueryHardware), E2("_grSstSelect@4", SstSelect), E2("_grSstWinOpen@28", SstWinOpen),
    E2("_grSstWinClose@0", SstWinClose), E2("_grSstIdle@0", G2SstIdle), E2("_grSstIsBusy@0", G2SstIsBusy), E2("_grSstStatus@0", G2SstStatus),
    E2("_grRenderBuffer@4", RenderBuffer),
    E2("_grDrawTriangle@12", DrawTriangle), E2("_grDrawLine@8", DrawLine), E2("_grDrawPoint@4", DrawPoint),
    E2("_grBufferClear@12", BufferClear), E2("_grBufferSwap@4", G2BufferSwap), E2("_grClipWindow@16", ClipWindow), E2("_grCullMode@4", CullMode),
    E2("_grColorCombine@20", ColorCombine), E2("_grAlphaCombine@20", AlphaCombine), E2("_grTexCombine@28", TexCombine),
    E2("_grTexCombineFunction@8", G2TexCombineFunction),
    E2("_grAlphaBlendFunction@16", AlphaBlendFunction), E2("_grAlphaTestFunction@4", AlphaTestFunction),
    E2("_grAlphaTestReferenceValue@4", AlphaTestReferenceValue),
    E2("_grDepthBufferMode@4", DepthBufferMode), E2("_grDepthBufferFunction@4", DepthBufferFunction), E2("_grDepthMask@4", DepthMask),
    E2("_grDepthBiasLevel@4", DepthBiasLevel), E2("_grDitherMode@4", DitherMode),
    E2("_grFogMode@4", FogMode), E2("_grFogColorValue@4", FogColorValue), E2("_grFogTable@4", FogTable), E2("_guFogGenerateExp@8", G2FogGenerateExp),
    E2("_grChromakeyMode@4", ChromakeyMode), E2("_grChromakeyValue@4", ChromakeyValue), E2("_grGammaCorrectionValue@4", G2GammaCorrectionValue),
    E2("_grTexMinAddress@4", TexMinAddress), E2("_grTexMaxAddress@4", G2TexMaxAddress), E2("_grTexCalcMemRequired@16", G2TexCalcMemRequired),
    E2("_grTexClampMode@12", TexClampMode), E2("_grTexFilterMode@12", TexFilterMode), E2("_grTexMipMapMode@12", TexMipMapMode),
    E2("_grTexDownloadMipMap@16", TexDownloadMipMap), E2("_grTexDownloadTable@12", G2TexDownloadTable), E2("_grTexSource@16", TexSource),
    E2("_grLfbLock@24", LfbLock), E2("_grLfbUnlock@8", LfbUnlock), E2("_grLfbWriteRegion@32", G2LfbWriteRegion), E2("_grLfbReadRegion@28", LfbReadRegion),
    { 0 },
};
