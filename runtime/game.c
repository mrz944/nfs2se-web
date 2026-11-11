/* Changes to how the game plays (NFS II SE). Called once per presented frame (gpu.c).
 *
 * RT_PLAYER_NAME=name: the name the game offers at CONNECT ("enter name") and uses in the
 * network lobby. The page passes the player's lobby name, so the in-game name matches the
 * room's without typing. It lives in the game's settings block (saved as
 * FEDATA/PC/CONFIG/config.dat, loaded at 0x510b70): this machine's own network name at +0x1ac,
 * 9 bytes (8 characters and a NUL). Found by saving two races with different typed names.
 * It is set once, after the game has loaded or defaulted its settings, so a name typed in the
 * game still counts for the rest of the session (and the game saves it as usual). */
#include "rt.h"

#define OWN_NAME 0x510d1cu
#define OWN_NAME_LEN 9

static int name_valid(const uint8_t *p) {           /* the settings are in: a printable name */
    int n = 0;
    while (n < OWN_NAME_LEN && p[n]) { if (p[n] < 32 || p[n] > 126) return 0; n++; }
    return n > 0 && n < OWN_NAME_LEN;
}

static void set_player_name(void) {
    static int done = -1;
    if (done < 0) done = !getenv("RT_PLAYER_NAME") || !*getenv("RT_PLAYER_NAME");
    if (done || !name_valid(G + OWN_NAME)) return;
    char name[OWN_NAME_LEN] = { 0 };
    int n = 0;
    for (const char *s = getenv("RT_PLAYER_NAME"); *s && n < OWN_NAME_LEN - 1; s++)
        if (*s >= 32 && *s <= 126) name[n++] = *s;
    while (n && name[n - 1] == ' ') name[--n] = 0;      /* "Bob the Racer" -> "Bob the" */
    if (n) { memcpy(G + OWN_NAME, name, OWN_NAME_LEN); rt_log("player name: %s", name); }
    done = 1;
}

/* Unlocks (on unless RT_LOCKED=1): the bonus track and cars the Tournament (or the main-menu
 * codes "hollywood", "bomber", "tombstone", "fzr2000") would open. Two bytes of the settings block,
 * found by snapshotting memory before and after each code: +0x1c Monolithic Studios (0/1) and
 * +0x1d the bonus car level (1 Bomber BFS, 2 + Tombstone, 3 + FZR 2000). The game saves them with
 * its settings, as it would after winning. */
#define SETTINGS 0x510b70u
static void unlock_all(void) {
    static int done = -1;
    if (done < 0) { const char *e = getenv("RT_LOCKED"); done = e && *e && *e != '0'; }
    if (done || !name_valid(G + OWN_NAME)) return;
    if (G[SETTINGS + 0x1c] != 1 || G[SETTINGS + 0x1d] < 3) {
        G[SETTINGS + 0x1c] = 1;
        if (G[SETTINGS + 0x1d] < 3) G[SETTINGS + 0x1d] = 3;
        rt_log("unlocked: Monolithic Studios and the bonus cars");
    }
    done = 1;
}

void game_frame(void) {
    set_player_name();
    unlock_all();
}
