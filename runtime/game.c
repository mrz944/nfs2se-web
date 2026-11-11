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

/* Instant steering (RT_INSTANT_STEER=1, the start page's setting): the wheels reach full lock at
 * once instead of ramping. Each physics tick (0x468b10) the game moves a car's steering (+0x2e4)
 * toward where the controls point (+0x2e0) by at most the car's step (its spec +0x130/+0x134),
 * unless the player's race parameters say otherwise: with 0 at +0x10 of the block at
 * 0x510040 + 0x40 * slot (the car's +0x21c points at it) it sets the steering at once (0x468fe2),
 * as the AI's blocks do. The race setup (0x459890) writes 1 there for human players; nothing
 * saves it. So for the local players' slots it is held at 0 during races. Found by watching the
 * game's reads of the Left key through to the car (RT_RWATCH, a WWATCH=1 build). */
#define RACE_PARAMS 0x510040u           /* 8 x 0x40 */
#define SPLIT_SCREEN_PLAYERS 0x50ffd8u  /* 2 with two players on one screen */
static int instant = -1;
static void instant_steering(void) {
    if (instant < 0) { instant = getenv("RT_INSTANT_STEER") && atoi(getenv("RT_INSTANT_STEER")); if (instant) rt_log("game: instant steering"); }
    if (!instant) return;
    extern int net_local_car(void);
    int me = net_local_car();                                  /* -1 offline, -2 online but not known yet */
    if (me == -2) return;
    for (int k = 0; k < 8; k++) {
        int local = me >= 0 ? k == me : k == 0 || (k == 1 && RD32(SPLIT_SCREEN_PLAYERS) == 2);
        if (!local) continue;
        uint32_t a = RACE_PARAMS + 0x40u * (uint32_t)k + 0x10u;
        if (RD32(a)) WR32(a, 0);
    }
}

void game_frame(void) {
    set_player_name();
    unlock_all();
    instant_steering();
}
