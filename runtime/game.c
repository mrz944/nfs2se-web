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
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#endif

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

/* Lap times for the start page's lap records. Each car (pointers at 0x50f918, car k = player k)
 * counts its finished laps at +0x230 and keeps each lap's time at +0x238 + 4 * lap (in 1/64 s;
 * 8 laps at most); +0x1dc is its model code ("MCF1"). The race: track number at 0x50ffec (the
 * TRxx file: 0 Proving Grounds, 2 Outback ... 8 Monolithic Studios), laps at 0x50ffcc, backwards
 * at 0x50fff8, mirrored at 0x50fffc. Found by snapshotting the cars while the AI drove laps, and
 * races set up each way. Each lap the local player finishes goes to the page (Module.onLap).
 * Not while a replay plays (Options > Load/Save > View Replay): the cars drive their recorded
 * race again, laps and all. The race config's +0x1c (0x50ffe4) is 2 then (the replay writer
 * sets it in the file, and playing it loads that config); 0 in races. */
#define CAR_TABLE 0x50f918u
#define RACE_TRACK 0x50ffecu
#define RACE_LAPS 0x50ffccu
#define RACE_BACKWARD 0x50fff8u
#define RACE_MIRRORED 0x50fffcu
#define RACE_KIND 0x50ffe4u     /* 2: a replay plays */
/* Instant steering (RT_INSTANT_STEER=1, the start page's setting): the wheels reach full lock at
 * once instead of ramping. Each physics tick (0x468b10) the game moves a car's steering (+0x2e4)
 * toward where the controls point (+0x2e0) by at most the car's step (its spec +0x130/+0x134),
 * unless the player's race parameters say otherwise: with 0 at +0x10 of the block at
 * 0x510040 + 0x40 * slot (the car's +0x21c points at it) it sets the steering at once (0x468fe2),
 * as the AI's blocks do. The race setup (0x459890) writes 1 there for human players; nothing
 * saves it. So for the local players' slots it is held at 0 during races. Found by watching the
 * game's reads of the Left key through to the car (RT_RWATCH, a WWATCH=1 build). */
#define RACE_PARAMS 0x510040u           /* 8 x 0x40 */
#define SPLIT_SCREEN_PLAYERS 0x50ffd8u  /* the race mode: 0 one player, 1 split screen, 4 network */
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

/* Catch-up (Game Setup's "catch up": cars that fall behind go faster). The game keeps it at
 * settings +0x0e (0x510b7e) and copies it into the race at 0x50ffe0 (0x405fc0); a race gives
 * the cars the catch-up handlers when that is on and the race mode (0x50ffd8) is split screen or
 * network (0x44e451). In one player the menu greys the row out and clears it; switching Game
 * Setup to split screen or network turns it back on (0x40f0e6), and the defaults have it on
 * (0x408e28), so every online race had it. Laps with it are much faster for whoever is behind:
 * each lap tells the page (flag 32, FLAG in web/laps.mjs), and the lap records keep them apart.
 * Here it starts off: when the settings are in and each time Game Setup leaves one player
 * (for a few frames, in case the menu catches up a frame later). Turning it on in Game Setup
 * still works. RT_CATCHUP=1 keeps the game's own way. */
#define GAME_TYPE (SETTINGS + 0x9u)     /* 0 one player, else split screen or network */
#define CATCH_UP (SETTINGS + 0xeu)
#define RACE_CATCH_UP 0x50ffe0u
static void catch_up_off(void) {
    static int keep = -1, type = -1, hold;
    if (keep < 0) { const char *e = getenv("RT_CATCHUP"); keep = e && *e && *e != '0'; }
    if (keep || !name_valid(G + OWN_NAME)) return;
    int t = G[GAME_TYPE];
    if (type < 0 || (type == 0 && t != 0)) hold = 10;
    type = t;
    if (hold > 0) {
        hold--;
        if (G[CATCH_UP]) { G[CATCH_UP] = 0; rt_log("game: catch-up off (turn it on in Game Setup)"); }
    }
}
static int race_catch_up(void) { return RD32(RACE_CATCH_UP) && (int32_t)RD32(SPLIT_SCREEN_PLAYERS) >= 1; }

static void lap_report(uint32_t car, int lap, uint32_t ticks) {
    uint32_t code = RD32(car + 0x1dc);
    int track = (int)RD32(RACE_TRACK), laps = (int)RD32(RACE_LAPS);
    /* 128: this runtime tells catch-up (32); the page knows older ones didn't */
    int flags = (RD32(RACE_BACKWARD) ? 1 : 0) | (RD32(RACE_MIRRORED) ? 2 : 0) | (instant > 0 ? 16 : 0) | (race_catch_up() ? 32 : 0) | 128;
    char cc[5] = { 0 }; memcpy(cc, &code, 4);
    for (int i = 0; i < 4; i++) if (cc[i] < 32 || cc[i] > 126) cc[i] = '?';
    rt_log("game: lap %d/%d %u:%02u.%02u (track %d, flags %#x, car %s)", lap, laps, ticks / 64 / 60, ticks / 64 % 60, ticks % 64 * 100 / 64, track, flags, cc);
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_ASYNC_EM_ASM({ if (Module.onLap) Module.onLap($0, $1, $2, $3, $4, $5); }, track, flags, (int)code, lap, laps, (int)ticks);
#endif
}
static void lap_watch(void) {
    extern int net_local_car(void);
    static uint32_t seen_car; static int done;               /* laps of seen_car already reported */
    int me = net_local_car();
    if (me == -2) return;                                      /* online, own car not known yet */
    static int watch = -2;                                     /* RT_LAP_CAR=k: report car k's laps instead (testing with the AI) */
    if (watch == -2) watch = getenv("RT_LAP_CAR") ? atoi(getenv("RT_LAP_CAR")) & 7 : -1;
    uint32_t car = RD32(CAR_TABLE + 4u * (uint32_t)(watch >= 0 ? watch : me >= 0 ? me : 0));
    if (car < 0x400000u || car + 0x690 >= GUEST_SIZE) { seen_car = 0; return; }
    int n = (int)RD32(car + 0x230);
    if (n < 0 || n > 64) return;
    int replay = RD32(RACE_KIND) == 2;
    static int replay_was;
    if (replay != replay_was) rt_log("game: %s", replay ? "a replay plays: its laps aren't the player's" : "the replay is over");
    replay_was = replay;
    if (car != seen_car) {                                     /* laps from before: not now */
        if (RD32(SPLIT_SCREEN_PLAYERS) && !replay) rt_log("game: race mode %d, catch-up %s", (int)RD32(SPLIT_SCREEN_PLAYERS), race_catch_up() ? "on" : "off");
        seen_car = car; done = n; return;
    }
    if (n < done) done = n;                                    /* a new race, or a restart */
    if (replay) { done = n; return; }                          /* a replay: its laps were driven before */
    while (done < n && done < 8) {
        uint32_t ticks = RD32(car + 0x238 + 4u * (uint32_t)done);
        if (!ticks) break;                                     /* written a moment later */
        done++;
        lap_report(car, done, ticks);
    }
}

/* Partial steering from the page's touch controls (the thumb stick and tilting the phone). Each
 * input tick the game builds every local player's controls at 0x510800 (0x45a060), 4 bytes
 * [steer, gas, brake, buttons], from the bindings at 0x5106a0: steer = (right - left) / 2, each
 * read 0..255, so -127 (full left) .. 127 (full right); the car's steering target follows it
 * (0x456de0 writes car +0x2e0). Keys only ever give the ends. The link wraps 0x45a060
 * (--wrap=f_0045A060, build.sh and build-web.sh) so that, after it, player 1's steer byte takes
 * the page's value while the page steers (anything but 0); a centred stick leaves the keys'.
 * Online that block is also what goes to the host, so others see the same steering.
 * RT_TOUCH_STEER=-127..127 sets it natively (testing). */
#define CONTROLS 0x510800u
static volatile int32_t touch_steer;
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE volatile int32_t *rt_web_touch_steer(void) { return &touch_steer; }
#endif
void __real_f_0045A060(CPU *c);
void __wrap_f_0045A060(CPU *c) {
    __real_f_0045A060(c);
    static int test = -1000;
    if (test == -1000) test = getenv("RT_TOUCH_STEER") ? atoi(getenv("RT_TOUCH_STEER")) : 0;
    int32_t s = test ? test : __atomic_load_n(&touch_steer, __ATOMIC_RELAXED);
    if (!s) return;
    if (s > 127) s = 127; else if (s < -127) s = -127;
    G[CONTROLS] = (uint8_t)(int8_t)s;
}

void game_frame(void) {
    set_player_name();
    unlock_all();
    catch_up_off();
    instant_steering();
    lap_watch();
}
