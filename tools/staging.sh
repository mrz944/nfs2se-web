#!/bin/sh
# Staging site for the verok-patch branch: staging.nfs2se.xyz, port 8093, its own data, kept out
# of search engines, open or closed with a password. Run from the staging worktree
# (~/DEV/nfs2-webgl-staging); see docs/verok-plan.md. Each part is started by hand:
#
#   tools/staging.sh setup         once: reflink copies of the live game folder, lifted code, venv
#                                  and web build (the patch changes the game files, so no links)
#   tools/staging.sh tunnel-setup  once: the Cloudflare tunnel "nfs2se-staging" and the DNS name
#   tools/staging.sh server        the staging server, open to anyone with the address (Ctrl+C stops it)
#   tools/staging.sh server --password   the same, closed with a password page
#   tools/staging.sh tunnel        the tunnel to it (Ctrl+C stops it)
#   tools/staging.sh password      the testers' password
#
# The live site is never touched: its tunnel (nfs2se-web), port 8092, game/, web/dist and build/
# stay as they are. Game files: staging serves its own game/ folder (--game), since the R2 bucket's
# CORS rule only allows nfs2se.xyz (docs/r2.md).
set -e
cd "$(dirname "$0")/.."
here=$(pwd)
main=$(cd "$(git rev-parse --git-common-dir)/.." && pwd)    # the main checkout (nfs2se.xyz)
[ "$here" != "$main" ] || { echo "run this from the staging worktree, not the live checkout"; exit 1; }
PORT=8093
HOST=staging.nfs2se.xyz
NAME=nfs2se-staging
cf="$main/build/bin/cloudflared"
cert="$main/build/cloudflare/home/.cloudflared/cert.pem"   # the nfs2se.xyz login (docs/cloudflare.md)

case "$1" in
setup)
  # reflink copies (btrfs: no extra space until they change); never links, so a staging build
  # or Verok's files can't write into the live ones
  for d in game gen .venv web/dist build/missing.txt; do
    [ -e "$d" ] || { mkdir -p "$(dirname "$d")"; cp -a --reflink=auto "$main/$d" "$d"; }
  done
  echo "staging ready in $here (game files: its own game/ folder)"
  ;;
tunnel-setup)
  mkdir -p build/cloudflare
  if [ ! -e build/cloudflare/credentials.json ]; then
    "$cf" tunnel --origincert "$cert" create --cred-file build/cloudflare/credentials.json "$NAME"
    chmod 600 build/cloudflare/credentials.json
  fi
  id=$(sed -n 's/.*"TunnelID": *"\([^"]*\)".*/\1/p' build/cloudflare/credentials.json)
  cat > build/cloudflare/config.yml <<EOF
tunnel: $id
credentials-file: $here/build/cloudflare/credentials.json

ingress:
  - hostname: $HOST
    service: http://127.0.0.1:$PORT
  - service: http_status:404
EOF
  "$cf" tunnel --origincert "$cert" route dns "$NAME" "$HOST"
  echo "tunnel $NAME ($id) -> $HOST -> 127.0.0.1:$PORT"
  ;;
server)
  if [ "$2" = "--password" ]; then exec node web/serve.mjs "$PORT" --game game --staging; fi
  exec node web/serve.mjs "$PORT" --game game --staging --open
  ;;
tunnel)
  exec "$cf" tunnel --no-autoupdate --config build/cloudflare/config.yml run
  ;;
password)
  cat build/staging-password 2>/dev/null || echo "start the server once with --password: it makes build/staging-password"
  ;;
*)
  sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
  ;;
esac
