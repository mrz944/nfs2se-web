/* Imports NFS II SE (NFS2SEA.EXE, 1997) uses on top of NFS3's set.
 *
 * The game insists on its CD: install.win marks some folders (movies, music, the
 * "se" tracks) as left on the disc, and the start-up looks for a CD-ROM drive.
 * Here C: is the hard disk and D: a CD-ROM labelled like the original disc
 * (NFSIISE). Both map onto the one game folder (fs_map ignores the drive letter),
 * which holds everything the CD and the installer would have provided.
 */
#define _GNU_SOURCE
#include "rt.h"
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>

#define CD_LABEL "NFSIISE"

static int drive_of(uint32_t p) { const char *s = HSTR(p); return s && s[0] ? (s[0] | 32) : 'c'; }

/* ---- KERNEL32: drives ---- */
static uint32_t n_GetDriveTypeA(CPU *c, const uint32_t *a) {
    (void)c;
    switch (drive_of(A(0))) {
    case 'c': return 3;   /* DRIVE_FIXED */
    case 'd': return 5;   /* DRIVE_CDROM */
    default: return 1;    /* DRIVE_NO_ROOT_DIR */
    }
}
static uint32_t n_GetLogicalDrives(CPU *c, const uint32_t *a) { (void)c; (void)a; return (1u << 2) | (1u << 3); }
static uint32_t n_GetLogicalDriveStringsA(CPU *c, const uint32_t *a) {
    (void)c;
    static const char s[] = "C:\\\0D:\\\0";   /* + the final NUL of the literal */
    if (A(0) < sizeof s) return sizeof s;
    memcpy(G + A(1), s, sizeof s);
    return sizeof s - 1;
}
/* GetVolumeInformationA(root, volName, volNameSize, serial*, maxComponentLen*, fsFlags*, fsName, fsNameSize) */
static uint32_t n_GetVolumeInformationA(CPU *c, const uint32_t *a) {
    (void)c;
    int d = drive_of(A(0));
    if (d != 'c' && d != 'd') { tls_lasterr = 3; return 0; }
    const char *label = d == 'd' ? CD_LABEL : "GAME", *fs = d == 'd' ? "CDFS" : "FAT";
    if (A(1) && A(2) > strlen(label)) strcpy(HSTR(A(1)), label);
    if (A(3)) WR32(A(3), d == 'd' ? 0x2A1997EAu : 0x12345678u);
    if (A(4)) WR32(A(4), d == 'd' ? 110 : 255);
    if (A(5)) WR32(A(5), d == 'd' ? 0x00080004u : 0x3u);   /* CD: read-only, Unicode on disk */
    if (A(6) && A(7) > strlen(fs)) strcpy(HSTR(A(6)), fs);
    static int logged[2];   /* the game asks again and again while it loads: log each drive once */
    if (!logged[d == 'd']++) rt_log("GetVolumeInformationA(%s) -> %s", HSTR(A(0)) ? HSTR(A(0)) : "(cwd)", label);
    return 1;
}

/* ---- WINMM ---- */
static uint32_t n_joyGetNumDevs(CPU *c, const uint32_t *a) { (void)c; (void)a; return 16; }

/* ---- DSETUP.DLL (loaded by name): DirectXSetupGetVersion(DWORD *version, DWORD *revision) ---- */
static uint32_t n_DirectXSetupGetVersion(CPU *c, const uint32_t *a) {
    (void)c;
    if (A(0)) WR32(A(0), 0x00040005u);   /* DirectX 5 */
    if (A(1)) WR32(A(1), 0x0000009Bu);   /* 4.05.00.0155 */
    return 1;
}

/* MSVCRT, for Verok's nfs2se.p (MSVC 6): all cdecl. _ftol takes ST0 (popped) and returns it
 * truncated in EDX:EAX; ceil/floor take a double and return it in ST0. */
static uint32_t m_malloc(CPU *c, const uint32_t *a) { (void)c; return galloc(A(0) ? A(0) : 1); }
static uint32_t m_free(CPU *c, const uint32_t *a) { (void)c; if (A(0)) gfree(A(0)); return 0; }
static uint32_t m__strupr(CPU *c, const uint32_t *a) {
    (void)c; for (uint32_t p = A(0); p && G[p]; p++) G[p] = (uint8_t)toupper(G[p]);
    return A(0);
}
static uint32_t m_strchr(CPU *c, const uint32_t *a) {
    (void)c; const char *s = HSTR(A(0)), *r = s ? strchr(s, (int)(A(1) & 0xFF)) : NULL;
    return r ? A(0) + (uint32_t)(r - s) : 0;
}
static uint32_t m_strrchr(CPU *c, const uint32_t *a) {
    (void)c; const char *s = HSTR(A(0)), *r = s ? strrchr(s, (int)(A(1) & 0xFF)) : NULL;
    return r ? A(0) + (uint32_t)(r - s) : 0;
}
static double arg_double(const uint32_t *a, int i) { uint64_t v = (uint64_t)a[i] | (uint64_t)a[i + 1] << 32; double d; memcpy(&d, &v, 8); return d; }
static uint32_t m_ceil(CPU *c, const uint32_t *a) { fpush(c, ceil(arg_double(a, 0))); return 0; }
static uint32_t m_floor(CPU *c, const uint32_t *a) { fpush(c, floor(arg_double(a, 0))); return 0; }
static uint32_t m__ftol(CPU *c, const uint32_t *a) {
    (void)a; double v = ST(0); fpop(c);
    int64_t r = (v != v || v >= 9.2233720368547758e18 || v < -9.2233720368547758e18) ? INT64_MIN : (int64_t)v;
    c->r[EDX] = (uint32_t)((uint64_t)r >> 32);
    return (uint32_t)r;
}
static uint32_t m_sprintf(CPU *c, const uint32_t *a) {   /* %d %i %u %x %X %s %c %f, flags/width */
    char *out = HSTR(A(0)); const char *f = HSTR(A(1)); int ai = 2; char *o = out;
    while (*f) {
        if (*f != '%') { *o++ = *f++; continue; }
        char spec[32]; int k = 0; spec[k++] = *f++;
        if (*f == '%') { *o++ = '%'; f++; continue; }
        while (*f && strchr("-+ #0123456789.", *f) && k < 28) spec[k++] = *f++;
        while (*f == 'l' || *f == 'h') f++;
        char t = *f++; spec[k++] = t; spec[k] = 0;
        uint32_t v = RD32(c->r[ESP] + 4 + 4 * ai++);
        if (t == 's') o += sprintf(o, spec, v ? HSTR(v) : "(null)");
        else if (t == 'f' || t == 'g' || t == 'e') { uint32_t h = RD32(c->r[ESP] + 4 + 4 * ai++); uint32_t w[2] = { v, h }; o += sprintf(o, spec, arg_double(w, 0)); }
        else o += sprintf(o, spec, v);
    }
    *o = 0;
    return (uint32_t)(o - out);
}

#define N(dll, n, args) { dll, #n, args, n_##n }
#define M(n) { "MSVCRT.DLL", #n, -1, m_##n }
const ImportDef imp_nfs2[] = {
    N("KERNEL32.DLL", GetDriveTypeA, 1), N("KERNEL32.DLL", GetLogicalDrives, 0),
    N("KERNEL32.DLL", GetLogicalDriveStringsA, 2), N("KERNEL32.DLL", GetVolumeInformationA, 8),
    N("WINMM.DLL", joyGetNumDevs, 0),
    N("DSETUP.DLL", DirectXSetupGetVersion, 2),
    M(malloc), M(free), M(_strupr), M(strchr), M(strrchr), M(ceil), M(floor), M(_ftol), M(sprintf),
    { 0 },
};
