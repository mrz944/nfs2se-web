# Plan: NFS II SE web on Verok's patch v1.0.5 (branch `verok-patch`)

Goal: rebuild the browser game on Verok's patched exe instead of the retail 3dfx `NFS2SEA.EXE`. The rebuild keeps everything `main` has: lap records, catch-up off, instant steering, the online pacer and the knockout fix, replays and names. On top of that it adds Verok's fixes and options and the SilentPatch fixes, then a cockpit view. All of it is developed and tried on staging.nfs2se.xyz before it reaches nfs2se.xyz.

## The patch

Sources: the gamepressure listing (https://www.gamepressure.com/download/need-for-speed-ii-veroks-patch-v105-mod/z414c9c), the PCGamingWiki file 2448 (it returns 403 to bots) and Patreon (https://www.patreon.com/posts/need-for-speed-34307361).

- The download is 1.6 MB, last updated 2024-03-05.
- It ships:
  - `nfs2se.exe`, "based on last official EA patch";
  - `nfs2se.p`, Verok's module (SilentPatch looks it up with `GetModuleHandleExW(..., L"nfs2se.p")`);
  - EA "3rash" (Thrash) renderers from NFS III–V, for DirectX and Glide/nGlide;
  - Verok's own OpenGL 1.1/3.0 renderer;
  - `config.exe`;
  - a rewritten `eacsnd.dll`.
- Features:
  - no CD check and no registry use, so the game is portable;
  - a choice of renderer and of language (EN/ES/DE/FR/IT/SV);
  - several compatibility fixes (MapMemoryB000, GlobalMemoryStatusLie, GetDiskFreeSpace);
  - 32 bpp, more resolutions, and widescreen with correct FOV;
  - higher car texture detail;
  - a choice of texture type: SE (Glide; at most 256×256, 16 bpp) or Classic (software textures: higher resolution, 256 colours);
  - on/off switches for texture filtering, object shading and fog;
  - no intro movies.
- Known issues:
  - objects (cars, world) sometimes vanish at the left and right edges of the screen;
  - car dashboards are not implemented for 3D acceleration;
  - wheel fog sprites are the wrong size at higher resolutions.

What this means here: a 3rash renderer is how NFS III draws. nfs3-webgl recompiles the renderer DLL (voodoo2a.dll) and implements Glide 3 underneath it. That Glide 3 implementation is already in this repo (`glide_fns` in `runtime/glide.c`; `runtime/modules.c` maps the renderer DLLs). So the "recompile from scratch" step means lifting Verok's `nfs2se.exe`, `nfs2se.p` and his Glide renderer DLL in place of `NFS2SEA.EXE`.

## P0: files, analysis, go/no-go (½–1 day)

1. The user downloads the zip into `~/Downloads`. It is a third-party binary, so unpack it into a new, empty folder (for example `build/verok/`) and run Python on it only with `-I`.
2. Compare the exes:
   - PE headers, imports, sections and base;
   - its timestamp against the official EA patch and against `NFS2SEA.EXE` / `NFS2SEN.EXE` (in `game/`);
   - whether it still calls Glide 2 itself or only goes through 3rash.
3. `nfs2se.p`: is it a DLL loaded through the import table or by `LoadLibrary`? Does it **write into the exe's code at load time** (hooks, jumps)? If it does, a static lift of the file on disk misses its changes. In that case, run its patching once (natively, or under the runtime), dump the patched image, and lift that image. This is the main go/no-go risk.
4. The renderers: pick the Glide one. Check whether it imports glide2x or glide3x, and compare its exports with the 45-function thrash ABI (see ARCHITECTURE.md).
5. Data: are FEDATA/GAMEDATA, `config.dat`, saves and replays (`.rpy` / `.rp3`, offsets in `web/replays.mjs`) the same layout as the retail game? Does the patch need files the retail folder lacks (language text, its own ini)?
6. The cockpit camera: is the 3dfx-only skip at the view change still there? In NFS2SEA it is at `0x44998F` in `f_004497F0`; see the cockpit notes below.
7. Decide. If the exe can't be lifted, fall back to building the same features on `NFS2SEA.EXE` (cockpit view, widescreen/FOV, toggles, SilentPatch). Tell the user before switching.

### P0 results (2026-11-27): go

Zip `nfs2_se_patch_1.05.zip` (sha256 `0ba96130…4bff`, password `pcgw`), unpacked in `build/verok/nfs2_se_patch_1.05/`. Analysis scripts and patch logs: `build/verok-analysis/` (`scripts/emu.py` emulates the module with unicorn and needs a venv with unicorn, capstone and pefile; staging's `.venv` has no unicorn).

- **`nfs2se.exe`** (sha256 `280648b7…5b33`) is EA's official 3dfx patch exe, built 1997-11-27 (retail NFS2SEA: 09-23). It is the same Watcom build as NFS2SEA (bss 0xd5a00) and still draws through Glide 2 (`glide2x.dll` strings and the function-pointer table).
  - EA's changes: TCP/IP (WSOCK32 grows from 12 to 23 imports) and new strings in `text.*`.
  - Verok's changes to the file: the DDRAW import is replaced by `NFS2SE.p!InjectPatch`, a `.rsrc` was added, and the **relocations are zeroed** (empty directory, all-zero section, flag 0x183).
  - Code moved about +0x1af0 in the game part and +0x10…+0x20 in the frontend. Data moves by different amounts in different places (+0x21e4, +0x2230).
- **`nfs2se.p`** (MSVC 6 DLL, base 0x10000000, has relocations; originally named HOOK.p) loads through the exe's import table. Its DllMain patches the exe in memory, with five helpers: jmp `0x100018b0`, nop `0x100018f0`, bytes `0x10001940`, dword `0x100019b0`, word `0x100019f0`. The `.text` patches:
  - **In DllMain, the same for every config** (101 bytes):
    - jmp hooks: 0x405cae, 0x428acf, 0x42a170, 0x42a270, 0x42a2a0, 0x433910, 0x4355b0, 0x435bcb, 0x480760, 0x481399, 0x4b2d98;
    - NOPs: 0x43020f, 0x430216, 0x430275, 0x430298, 0x4302e1, 0x463432;
    - operand rewrites: 0x430206, 0x488706, 0x4888bb, 0x48895c;
    - IAT `GlobalMemoryStatus` (0x4c6538);
    - plus the .bss/DGROUP tables.
  - **At startup:** the hook 0x4b2d98 lands in `0x10002180`. That routine reads `nfs2se.ini` and `drivers\<ThrashDriver>\thrash.ini` (it sets the `[ENV]` lines as environment variables) and `LoadLibrary`s the driver. It takes 25 `_THRASH_*` functions from it, then `0x10003ff0` swaps the Glide table at 0x4dae64… for wrappers and rewrites operands (FOV/widescreen) at 0x42480a, 0x42aa9e, 0x42abc0, 0x42ad46, 0x42aebd, 0x47bb58, 0x42c0a0, 0x426a31, 0x42a779. Most of this is the same for every config. Only two settings change `.text`:
    - `ClassicTextures=1`: hooks 0x44705d, 0x44708e and operands 0x42e925, 0x430bdf, 0x430c06, 0x430c17, 0x4324a2 (30 bytes);
    - `NoMovies=1`: 0x408202 and 0x40d73b (7 bytes).
    - Fog, Shading, Filtering, Resolution, WindowedMode, Language and the renderer only change data in `nfs2se.p` or .bss/DGROUP, which works as-is when the module runs under the runtime.
  - **After the window opens** (hook 0x428acf → `0x10004800`): `_THRASH_about`'s mode list decides 16 or 32 bpp, which is written into the immediates at 0x407e6e, 0x40802c, 0x430232, 0x49afb7 (and data 0x4dab84). The jmp at 0x47bb90 is also written then. The bpp is fixed for our renderer, so it can be baked in.
  - All 134 helper call sites are accounted for. Every direct write from the module goes to .bss/data.
- **Renderer:** `Drivers/nglide/voodoo2a.dll` and `Drivers/dgvoodoo/voodoo2a.dll` are byte-identical to the voodoo2a.dll that nfs3-webgl already lifts (sha256 `f3910855…7251`; it LoadLibrarys glide3x, and `glide_fns` already exists here). `Drivers/glide3x/` holds voodoo2z.dll (a 2000 Thrash with DDRAW), which we don't use. The web build uses `ThrashDriver=nglide` (or its own ini whose `File=voodoo2a.dll`).
- **Data:** the patch's `FeData/pc/text/text.*` (each 94 bytes longer, with the TCP/IP and H3D strings) replace retail's. `GameData/Tracks/pc/3TR0x.HRZ` is new (horizon values). config.dat, saves and replays are not checked yet: load the retail ones in P1 natively.
- **Cockpit:** the 3dfx skip is still there, unchanged, at **0x44b47f** in `f_0044B2E0` (it calls 0x44b2e0 again). The camera struct is at 0x4d5c74.
- **main's addresses** (26 distinct ones in `runtime/*.c`, plus the wrap):
  - all 11 code addresses are found by byte pattern: 0x405fc0→0x405fd0, 0x408e28→0x408e38, 0x40f0e6→0x40f106, 0x434b2d→0x43661d, 0x434b51→0x436641, 0x44e451→0x44ff41, 0x456de0→0x4588d0, 0x459890→0x45b380, 0x468b10→0x46a620, 0x468fe2→0x46aaf2;
  - the input tick `f_0045A060` → **`f_0045BB50`**;
  - the 15 data addresses are read from matched instructions (e.g. controls 0x510800→0x512a30).

How the lift works (changes P1):

1. `tools/verok-patch.py` (new) writes `NFS2SE.EXE` with every `.text` patch from the list above that is the same for every config baked in, plus the bpp. Textures are **SE** (the user's choice, 2026-11-27): the `ClassicTextures` patches are left out and the page writes `ClassicTextures=0`. Then it applies the **SilentPatch** fixes (next list) to the exe and to `nfs2se.p`. That file is lifted; nfs2se.p is lifted as its own module at 0x10000000 and runs its DllMain and startup under the runtime as usual. Its writes to .text then no longer matter; its data writes do.
2. Lifter (`tools/discover.py` / `lift.py`): a direct `jmp`/`call` to an address outside the image becomes a cross-module dispatch, not "undecodable". The return points of the mid-function hooks (the `jmp [0x100060xx]` targets) are seeded as function starts.
3. No relocations in the exe: pseudo-relocations come from a scan for code pointers. Measured on NFS2SEA: scanning the code at every byte offset finds all 1,315 real code pointers (886 false), and an aligned DGROUP scan finds 682 of 690 (57 false). False seeds only make dead functions. Filter them with an instruction-operand check, then fill gaps with `missing.txt`.
4. The `NoMovies` patch (0x408202 call → NOPs, 0x40d73b jle → nop+jmp) is not baked in. The lifter gets a list of variant instructions; at each one it emits `if (flag) <patched> else <original>`, with the flag taken from the `NoMovies` value the page writes to `nfs2se.ini`. So the Intro movies setting works as in nfs3. Classic textures could be added the same way later (all their patches are whole instructions too).

**SilentPatch on top of Verok** (CookiePLMonster/SilentPatchNFS90s, `source/SilentPatchNFS90s.cpp`). SilentPatch finds its targets by byte pattern, and every NFS2SE pattern matches **exactly once** in nfs2se.exe once Verok's patches are applied; the `nfs2se.p` pattern matches exactly once in the module. `tools/verok-patch.py` applies them by the same patterns after Verok's patches and stops with an error if a pattern doesn't match exactly once.

| Fix | Where (nfs2se.exe) | Patch | Us |
|---|---|---|---|
| Online races drawn as split screen (a bug in Verok's patch) | SetViewports 0x42a170; nfs2se.p 0x10004fec | exe: `53 51 52 83 3D` over Verok's jmp, and dword 241 at 0x42a18b (1px split-screen seam); .p: `cmp [ecx],0 / jz` → `cmp [ecx],1 / jnz` | **needed** (race mode 4 = network) |
| Movie-thread start race | calls at 0x49358f, 0x4935bd, 0x493f9a | the create / wait / signal calls go to small routines that create, wait on and set an auto-reset event (through the exe's own CreateEventA/WaitForSingleObject/SetEvent/CloseHandle imports), in the free bytes at the end of .text (291 bytes from 0x4c52dd) | yes. Test whether this is the hang that makes main skip movies |
| Button bitmask used as a count | 0x421e1b | `42 90 90 90` (inc edx) | yes (we report 32 buttons) |
| Game quits when a pad disconnects | 0x4215b0 | `F7 D0 C1 E8 1F C3` (return code >= 0) | yes (Gamepad API pads can come and go) |
| Mouse jitter in menus | 0x42209d, 0x4220ad | jne → js; call to `mov ebx,ecx / shl ebx,4 / xor eax,eax / xor edx,edx / ret` | harmless, applied |
| Poll() success codes treated as errors | 0x421a9d | je → jge | harmless, applied |
| Thread affinity | 0x4935ae | — | skipped: the runtime already runs game threads one at a time |
| Alt+F4 | 0x481973 | — | skipped: in a browser the tab/OS owns Alt+F4 |
| Ctrl+V paste into text boxes | WndProc 0x480cd3 | — | skipped for now: names come from the page; could be done later from the page's paste event |

## P1: multi-module lift and native bring-up (2–4 days)

- `build.sh` / `build-web.sh` MODULES: `nfs2se:NFS2SE.EXE:400000`, `nfs2sep:nfs2se.p:<base>`, `<renderer>:<dll>:<base>`, and `eacsnd:EACSND.DLL:b00000`.
  - Each module needs its own base.
  - Keep EA's EACSND.DLL (the runtime's audio expects it), not Verok's rewrite.
  - `build/missing.txt` seeds are per exe: start a new file for the new exe.
- The runtime provides the patch's ini/config (renderer = Glide 3rash, windowed, texture type, filtering, shading, fog, resolution) from page settings, in place of `config.exe`. Verok's DX and OpenGL renderers are not used.
- `build-info.json`: the `exe_sha256` of the new exe.
- Done when the native headless harness gets through the menu and a race with frame dumps. Then the browser build.

### P1 results (2026-11-27): done

The menu and a race run natively (headless, frame dumps) and in the browser (headless Brave against a private server on 8097).
- `tools/verok-patch.py` writes `game/nfs2se.exe` (the 54 Verok patches, SilentPatch, and a code cave for SilentPatch's routines at 0x4c52e0), `game/nfs2se.p` (SilentPatch's gameMode fix), `nfs2se.ini`, `DRIVERS/GLIDE3X/` and the patch's text and HRZ files. It also writes `build/verok-lift/` (13 seeds and the NoMovies variants). Retail's text files are backed up in `build/verok-retail-backup/`.
- Lifted modules: nfs2se.exe at 0x400000 (3958 functions), EACSND.DLL at 0xb00000, nfs2se.p at 0xc00000 (126), voodoo2a.dll at 0xd00000 (421).
- Lifter:
  - pseudo-relocations by scan when an image has none;
  - code seeded from instruction immediates;
  - pointer tables in the code section;
  - unaligned data pointers to prologues or record runs (Watcom's 6-byte startup table);
  - guessed seeds that decode to data (strings, the IAT, I/O instructions) are dropped;
  - `jmp [reg + table]` jump tables;
  - jumps out of the image become `call_indirect`;
  - `--variants`.
  On NFS2SEA, this covers 99.87% of what its real relocations reach.
- Runtime:
  - lifted DLLs in import tables get their DllMain on the game thread before the entry point;
  - MSVCRT (9 functions), GetWindowRect/GetWindowPlacement/SetWindowPlacement, DisableThreadLibraryCalls, SetProcessAffinityMask;
  - `GAME_EXE` nfs2se.exe;
  - the patch check (VirtualProtect + `verok_patch_check`): 60 ranges, 57 as in the lift, 3 as expected, 0 mismatched, so bpp 16 is right;
  - `g_variant_nomovies` from nfs2se.ini.
- Page: `nfs2se.exe` in the folder import, `hasGame()` also needs nfs2se.p (so a pre-Verok copy downloads again), and the About credits Verok's patch and SilentPatch.
- Not yet: game.c's tweaks are off (P2), and widescreen (P3; Resolution=2 = 640×480 for now).

## P2: port main's game changes to the new exe (3–5 days)

About 45 hardcoded addresses must be found again. `grep -oE '0x[45][0-9a-f]{5}' runtime/*.c`: game.c has 35, net.c 3, gpu.c 3, modules.c 2, core.c 1. Plus the `--wrap=f_0045A060` link hook (the input tick behind instant steering, in both build scripts). They include:

- laps: the car table 0x5efac8, the race's track/backward/mirrored/laps (0x6fd4ac…, race laps 0x6fd4b4);
- the replay flag 0x50ffe4;
- the race config 0x50ffc8 (+0x10 race mode 0 one player / 1 split / 4 network, +0x18 catch-up), settings 0x510b70 (+0xe catch-up);
- the menu turning catch-up on at 0x40f0e6, and the defaults at 0x408e28;
- the player controls 0x510800;
- the player name in config.dat;
- the net.c and gpu.c hooks.

Find each one by its byte pattern around the old address, and log loudly when one isn't found. Then check end to end: lap reports (flags 1/2/16/32/128), replay upload and late replays, catch-up, instant steering, name writing (`writeGameName` in main.js) and online.

### P2 results so far (2026-11-28)

- **Addresses.** Verok's exe has NFS2SEA's .bss moved by +0x2230 as one block. Every address in `runtime/*.c` was found again from the instructions that use it, with near-unanimous votes:
  - settings 0x512da0 (name +0x1ac, game type +0x9, catch-up +0xe, unlocks +0x1c/+0x1d);
  - cars 0x511b48;
  - race block 0x5121f8…0x51222c (laps 0x5121fc, mode 0x512208, catch-up 0x512210, kind 0x512214, track 0x51221c, backward 0x512228, mirrored 0x51222c);
  - race params 0x512270;
  - controls 0x512a30;
  - input tick f_0045BB50.
  91% of NFS2SEA's functions are byte-identical apart from addresses, including the physics tick, race setup, controls → car and the input tick. config.dat is the same 684-byte settings block. The replay writer (0x470470) still writes 0x1e5b4 bytes from the race config, so `web/replays.mjs` is unchanged.
- **Checked natively** (game.c's tweaks are on again):
  - the name (RT_PLAYER_NAME);
  - instant steering;
  - catch-up turned off when Game Setup leaves one player;
  - lap reports with RT_LAP_CAR=1: lap 1/4 1:14.53, track 0, car GT90, flags 0x90.
- **The lobby art.** The new exe asks for `enetwork.qfs` (client-server) and `emodem.qfs` (peer-peer), with a language letter in front. Without them "create game" never draws its lobby. `verok-patch.py` now installs copies of NETWORK.QFS and MODEM.QFS.
- **Online: not working yet in the native harness.** Name, protocol IPX, create/join game and the game list all work: the SAP query and reply, then the client's `srv` and the host's `dec` with "Hosty". But choosing the host's game sends nothing.
  - EA's patch rewrote the network layer: NFS2SEA has 13 message types (0x4af000…), nfs2se.exe has 28 (0x48c000…): ack alw anp die gam huh new nwa res rmv rta rtd sht upd. There is also a new "players: 2 / peer-peer | 2-8 / client-server" option.
  - net.c's steady beat, car sync and knockout fix were built against NFS2SEA's packets, so online needs its own work: first a real join (by hand in two browser tabs on staging, or more harness work on the list screen), then a check of net.c against the new packets.
- Natively, the relay must be a Unix socket with a short path (`--relay-tcp build/p2.sock`). The TCP form didn't connect, and the scratchpad path is too long for a socket.

## P3: page settings for the patch's options (1–2 days)

- Widescreen follows the window's aspect ratio.
- Game Settings gets: filtering, shading and fog.
- Intro movies: **off by default** (`NoMovies=1`). The existing Intro movies setting turns them on (`ensureIni`/`patchIni` in main.js, as in nfs3). Language is optional.
- Textures: SE only (`ClassicTextures=0`), the user's choice. No setting for now.
- Resolution goes through the existing render-scale cap.
- Settings are written into the game folder before start, as `writeGameName` does.

## P4: improvements (3–6 days)

- **Cockpit camera:**
  - In NFS2SEA, the per-player camera struct is at 0x4d3a90 + 0x110·player, and byte +8 is the view: 0 cockpit, 1–3 chase/far/bumper.
  - The view change `f_004497F0` takes the next view from table 0x50f6a9. Its 3dfx-only skip at 0x44998F (`cmp byte [view],0`, which calls 0x4497f0 again) hides view 0. NFS2SEN's version at 0x437e30 has no skip.
  - Add a setting for it. Draw the 3D at full height and hide the player's own car.
- **Dashboard overlay**, if Verok's exe doesn't draw one:
  - DSHxx.QFS is a refpack FSH: 'dash' 640×480 8-bit, '!pal' (index 255 transparent), 11 steering-wheel frames (38cw..00cc..38cc), plus 'mirr', 'bit1' and 'bit2'.
  - The 3dfx exe never opens these files. Find how the needles are drawn in NFS2SEN.
- **Fix the side culling** (Verok's known issue): the part of the scene that gets drawn must widen with the FOV.
- **Scale the wheel fog sprites** to the resolution.
- SilentPatch moved into P1 (see "SilentPatch on top of Verok" above).

## P5: tests

- Native headless harness: every track, forward and backward, mirrored, split screen.
- Two instances online (`tools/nettest.sh` with a relay on 8097): a single race, a knockout through several rounds, a 9 s pause then continue, quit and rejoin.
- Lap records and replays end to end against a private server (`--data` in the scratchpad).
- Browsers: desktop Chrome/Firefox/Safari, Android Chrome, iOS Safari/CriOS, and low-memory devices. In production's logs, iOS storage errors and "Out of memory" at startup are the common failures.

## P6: rollout

- staging.nfs2se.xyz (port 8093, its own tunnel nfs2se-staging, `tools/staging.sh`) for the user and testers; `server --password` closes it.
- The game pack: staging serves its own game folder (`--game game`); production uses R2 (`web/game-source.json`, CORS only for nfs2se.xyz). For production, pack Verok's files into a new R2 pack with `tools/pack-game.mjs`, and add staging.nfs2se.xyz to the R2 CORS if staging should use R2 too.
- Online: rooms must not mix the old and new exes. The relay tags rooms with the page's build/exe and keeps them apart until every page is on the new one.
- Lap records: widescreen/FOV doesn't change physics, so laps stay on the same boards. Mark laps with the exe for a while, in case it turns out to matter.
- Then What's New (`web/changelog.json`) and the merge into `main`, both only when the user says so.

## Open questions for the user

- Verok's permission to serve his files to every visitor (Patreon / VOGONS). Ask him before production.
- Widescreen on by default? Cockpit view on by default? (Textures: SE; intro movies: off by default, with a setting; SilentPatch: on top. All decided 2026-11-27.)
