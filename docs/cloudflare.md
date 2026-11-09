# Public hosting at nfs2se.xyz

Cloudflare terminates HTTPS and forwards requests through `cloudflared` to the local server at
`http://127.0.0.1:8092` (nfs3 keeps 8080, nfs4 8084). The tunnel carries the page, the
WebAssembly build, the lobby and the `/net` WebSocket relay. The game files come from R2
(`docs/r2.md`) once `web/game-source.json` points there; until then the server sends them itself
(`--game game`).

## What is set up on this host

| | |
|---|---|
| Tunnel | `nfs2se-web`, UUID `873125bf-61db-4604-8c62-8ed08a51dfd4` |
| Config / credentials | `build/cloudflare/config.yml`, `build/cloudflare/credentials.json` (git-ignored, keep them) |
| cloudflared | `build/bin/cloudflared`, official release 2026.10.7 (own copy, git-ignored). OpenSnitch needs an allow rule for this path |
| Login for nfs2se.xyz | `build/cloudflare/home/.cloudflared/cert.pem`, made by the DNS step below (`~/.cloudflared/cert.pem` is the nfs3.xyz one) |

## DNS (one time)

Either log in for the nfs2se.xyz zone and let cloudflared add the record:

```sh
CF=build/bin/cloudflared
HOME=$PWD/build/cloudflare/home $CF tunnel login          # pick nfs2se.xyz in the browser
$CF tunnel --origincert build/cloudflare/home/.cloudflared/cert.pem route dns nfs2se-web nfs2se.xyz
```

or in the dashboard: nfs2se.xyz → DNS → add `CNAME @ → 873125bf-61db-4604-8c62-8ed08a51dfd4.cfargotunnel.com`,
proxied (replace an existing apex A/AAAA/CNAME record if there is one).

## Run

Nothing runs as a service. Start both by hand from the repository root, in two terminals:

```sh
node web/serve.mjs 8092 --game game
build/bin/cloudflared tunnel --no-autoupdate --config build/cloudflare/config.yml run
```

Restart the server after `./build-web.sh` or page changes. Players' browser storage belongs to
the domain: the first visit downloads the game (466 MB gzip) once.

## Checks

```sh
curl -sI https://nfs2se.xyz/ | grep -i cross-origin       # COOP same-origin, COEP require-corp
curl -s -o /dev/null -w '%{http_code}\n' https://nfs2se.xyz/dist/nfs2se.wasm
```

Then open the page and make sure the lobby shows "online" (the `/net` WebSocket) before testing
multiplayer.
