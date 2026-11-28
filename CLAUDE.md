# nfs2-webgl staging (branch `verok-patch`)

This folder is a git worktree of `../nfs2-webgl`, which is `main` and production. It runs the **staging** server for **staging.nfs2se.xyz**. The work here: rebuild the game on Verok's patch v1.0.5. **The plan is `docs/verok-plan.md`: follow it phase by phase (P0 first).**

## Layout

- Branch `verok-patch`, made from `main` at f046dd7, so it has everything main had up to then. Bring later main changes over with `git merge main`.
- Copies of production's untracked files (btrfs reflinks):
  - `game/` (the retail game; the patch files go here, never into ../nfs2-webgl/game);
  - `.venv/`;
  - `gen/` (lifted from NFS2SEA.EXE);
  - `web/dist/`;
  - `build/missing.txt`.
- `build/` is staging's own data: logs, laps, players, analytics and push keys, separate from production.
- No `web/game-source.json`: production's R2 pack only allows the nfs2se.xyz origin (docs/r2.md), so staging serves its own `game/` folder.

## Running (the user starts everything; never start or restart servers or cloudflared yourself)

`tools/staging.sh` (like nfs3's): staging has its **own tunnel "nfs2se-staging"**, apart from production's "nfs2se-web".

```sh
tools/staging.sh tunnel-setup        # once: creates the tunnel + DNS staging.nfs2se.xyz (uses ../nfs2-webgl/build/bin/cloudflared and its nfs2se.xyz cert)
tools/staging.sh server              # node web/serve.mjs 8093 --game game --staging --open (noindex, STAGING ribbon)
tools/staging.sh server --password   # the same behind a password page (tools/staging.sh password shows it)
tools/staging.sh tunnel              # cloudflared for staging only (build/cloudflare/config.yml here)
```

- Ports: production NFS2 8092, staging 8093, NFS3 8080 (NFS3 staging 8098), NFS4 8084.
- Private test servers on 8097 are fine, as long as they use `--data <scratch dir>` and are stopped afterwards. Don't stop them with `pkill -f "serve.mjs 8097"`: the pattern matches the shell running that command, so it kills itself. Use `timeout` instead.
- `web/index.html` and `web/main.js` here are served live on staging as soon as they're saved, and `./build-web.sh game` updates staging's `web/dist`. Production is not touched either way, but tell the user what changed.
- `serve.mjs --staging` and `tools/staging.sh` are staging-only changes. They are uncommitted until the user asks; if the branch is merged into main later, they can stay (both are off unless `--staging` is passed).

## Rules from the user

- Commit and push only when asked. No Claude co-author line in commits or PRs.
- Never touch production (`../nfs2-webgl`: its web/, game/ and build/ data) from here. Merging into main happens only when the user says so.
- Cloudflare: use the binary in `../nfs2-webgl/build/bin/cloudflared`, not nfs3's. Don't create services, tunnels or DNS records yourself: the user runs `tools/staging.sh tunnel-setup`, and you give them any other commands.
- Names stay at most 8 characters (the game's limit).
- In native tests, back up `game/FEDATA/PC/CONFIG` first and restore it afterwards. Remove test files afterwards (`game/replay.rpy`, test saves).
- The Verok zip is third-party: unpack it into a new, empty folder, and run Python on it only with `-I`.
- Discord posts (`../nfs3-webgl/tools/discord/changelog.mjs`) are run by the user.

## Native harness

```sh
.venv/bin/python -I tools/verok-patch.py build/verok/nfs2_se_patch_1.05 game   # once: Verok + SilentPatch into game/ (nfs2se.exe, nfs2se.p, ini, renderer, text) + build/verok-lift/
./build.sh game            # lift (if gen/ is missing, or RELIFT=1) + compile -> build/nfs2se
env RT_HEADLESS=1 RT_NOSOUND=1 RT_DUMP=N RT_DUMP_FROM=s RT_DUMP_DIR=dir RT_INPUT="ms:KEY,..." timeout T build/nfs2se game
tools/nettest.sh           # two instances online, with: node web/serve.mjs 8097 --relay-tcp <sock> --data <dir>
```

- The run log's `verok patches: N ranges checked, ..., 0 mismatched` line (first frame) says the lift matches what nfs2se.p patches at run time. A `VEROK PATCH MISMATCH` means tools/verok-patch.py needs that site.
- `call to unlifted code X` adds X to build/missing.txt; `RELIFT=1 ./build.sh game` picks it up. The NFS2SEA lift's leftovers are in build/old-nfs2sea/.
- Online, natively: relay `timeout N node web/serve.mjs 8097 --relay-tcp build/p2.sock --data <scratch>`. Use a short relative Unix socket: the scratchpad path is too long for one, and `RT_NET=127.0.0.1:port` didn't connect. Both instances get `RT_NET=unix:build/p2.sock RT_ROOM=<room>`, and `RT_NET_STATS=1` prints packets and car states per second. Inputs that reach a network race (the game list needs Enter and then the click; the lobby's focus starts on "player car"):
  - common start: `4000:RETURN,4500:RETURN,5200:RIGHT,5600:RIGHT,6000:RIGHT,6400:RIGHT,7000:DOWN,7300:DOWN,7600:DOWN,7900:DOWN,8200:DOWN,8500:DOWN,9000:RETURN,10000:DOWN,10300:DOWN,10600:DOWN,10900:DOWN,11500:RETURN,14000:RETURN`
  - host: `+ 16000:DOWN,16400:DOWN,17000:RETURN,24000:DOWN,24300:DOWN,24600:DOWN,25500:RETURN`
  - client: `+ 16000:DOWN,16400:DOWN,16800:DOWN,17400:RETURN,19500:RETURN,21000:CLICK@505;113,23000:DOWN,23300:DOWN,23600:DOWN,24300:RETURN`
