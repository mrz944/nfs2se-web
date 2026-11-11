/* Included by generated code only (gen/<module>/funcs.h), after cpu.h.
 * Lifted functions keep guest registers in locals r0..r7 and the guest memory
 * base in a const local gm (see promote_registers in tools/lift.py). */
#pragma once
#define LOAD_REGS() \
    uint32_t r0 = c->r[0], r1 = c->r[1], r2 = c->r[2], r3 = c->r[3], \
             r4 = c->r[4], r5 = c->r[5], r6 = c->r[6], r7 = c->r[7]; \
    uint8_t *const gm = G
#define SPILL()  (c->r[0] = r0, c->r[1] = r1, c->r[2] = r2, c->r[3] = r3, \
                  c->r[4] = r4, c->r[5] = r5, c->r[6] = r6, c->r[7] = r7)
#define RELOAD() (r0 = c->r[0], r1 = c->r[1], r2 = c->r[2], r3 = c->r[3], \
                  r4 = c->r[4], r5 = c->r[5], r6 = c->r[6], r7 = c->r[7])

/* guest memory through the local base */
#ifdef RT_WWATCH_BUILD   /* RT_RWATCH=addr:len logs reads there too */
extern uint32_t g_rw_lo, g_rw_len;
void rt_rw_hit(const char *file, int line, uint32_t a, int n);
#define RW_(a, n) ((uint32_t)(a) - g_rw_lo < g_rw_len ? rt_rw_hit(__FILE__, __LINE__, (uint32_t)(a), n) : (void)0)
#else
#define RW_(a, n) ((void)0)
#endif
#define RD8(a)  ({ RW_(a, 1); gm[(uint32_t)(a)]; })
#define RD16(a) ({ RW_(a, 2); uint16_t _v; memcpy(&_v, gm + (uint32_t)(a), 2); _v; })
#define RD32(a) ({ RW_(a, 4); uint32_t _v; memcpy(&_v, gm + (uint32_t)(a), 4); _v; })
#define RD64(a) ({ uint64_t _v; memcpy(&_v, gm + (uint32_t)(a), 8); _v; })
#ifdef RT_WWATCH_BUILD   /* debugging build (build.sh with WWATCH=1): RT_WWATCH=addr:len logs writes there */
extern uint32_t g_ww_lo, g_ww_len;
void rt_ww_hit(const char *file, int line, uint32_t a, uint32_t v, int n);
#define WW_(a, v, n) do { if ((uint32_t)(a) - g_ww_lo < g_ww_len) rt_ww_hit(__FILE__, __LINE__, (uint32_t)(a), (uint32_t)(v), n); } while (0)
#else
#define WW_(a, v, n) ((void)0)
#endif
#define WR8(a, v)  do { WW_(a, v, 1); gm[(uint32_t)(a)] = (uint8_t)(v); } while (0)
#define WR16(a, v) do { uint16_t _w = (uint16_t)(v); WW_(a, _w, 2); memcpy(gm + (uint32_t)(a), &_w, 2); } while (0)
#define WR32(a, v) do { uint32_t _w = (uint32_t)(v); WW_(a, _w, 4); memcpy(gm + (uint32_t)(a), &_w, 4); } while (0)
#define WR64(a, v) do { uint64_t _w = (uint64_t)(v); memcpy(gm + (uint32_t)(a), &_w, 8); } while (0)
#define RDF32(a) ({ float _f; memcpy(&_f, gm + (uint32_t)(a), 4); _f; })
#define RDF64(a) ({ double _f; memcpy(&_f, gm + (uint32_t)(a), 8); _f; })
#define WRF32(a, v) do { float _f = (float)(v); memcpy(gm + (uint32_t)(a), &_f, 4); } while (0)
#define WRF64(a, v) do { double _f = (double)(v); memcpy(gm + (uint32_t)(a), &_f, 8); } while (0)

#undef PUSH32
#undef POP32
#define PUSH32(v) do { uint32_t _p = (v); r4 -= 4; WR32(r4, _p); } while (0)
#define POP32()   (r4 += 4, RD32(r4 - 4))
