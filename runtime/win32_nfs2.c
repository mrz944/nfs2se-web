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
    rt_log("GetVolumeInformationA(%s) -> %s", HSTR(A(0)) ? HSTR(A(0)) : "(cwd)", label);
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

#define N(dll, n, args) { dll, #n, args, n_##n }
const ImportDef imp_nfs2[] = {
    N("KERNEL32.DLL", GetDriveTypeA, 1), N("KERNEL32.DLL", GetLogicalDrives, 0),
    N("KERNEL32.DLL", GetLogicalDriveStringsA, 2), N("KERNEL32.DLL", GetVolumeInformationA, 8),
    N("WINMM.DLL", joyGetNumDevs, 0),
    N("DSETUP.DLL", DirectXSetupGetVersion, 2),
    { 0 },
};
