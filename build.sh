#!/bin/sh
# Native (Linux) build of the recompiled game, for bring-up and debugging.
#   ./build.sh <game dir>        lift (if needed) + compile
#   RELIFT=1 ./build.sh <dir>    force re-lift
# Lifted modules (Verok's patch v1.0.5 with SilentPatch, docs/verok-plan.md): nfs2se.exe and
# nfs2se.p as written by tools/verok-patch.py, the Glide 3 renderer voodoo2a.dll (Glide 3 is
# provided by the runtime) and EA's EACSND.DLL.
set -e
cd "$(dirname "$0")"
GAME="${1:-game}"
SEEDS=""; [ -f build/missing.txt ] && SEEDS="--seeds build/missing.txt"
# name, pe, base: every module gets a distinct base so lifted addresses never collide
MODULES="nfs2se:nfs2se.exe:400000 eacsnd:EACSND.DLL:b00000 nfs2sep:nfs2se.p:c00000 voodoo2a:DRIVERS/GLIDE3X/voodoo2a.dll:d00000"
[ -f "$GAME/nfs2se.p" ] && [ -f build/verok-lift/variants.json ] || {
  echo "no patched game in $GAME: run .venv/bin/python -I tools/verok-patch.py <unpacked patch> $GAME"; exit 1; }
for m in $MODULES; do
  n=${m%%:*}; r=${m#*:}; pe=${r%%:*}; base=${r##*:}
  # the exe: the return points of nfs2se.p's hooks, and the NoMovies variants (tools/verok-patch.py)
  EXTRA=""; [ "$n" = nfs2se ] && EXTRA="--seeds build/verok-lift/seeds.txt --variants build/verok-lift/variants.json"
  if [ ! -f "gen/$n/dispatch.c" ] || [ -n "$RELIFT" ]; then
    rm -rf "gen/$n"; .venv/bin/python -I tools/lift.py "$GAME/$pe" "gen/$n" $SEEDS $EXTRA --name "$n" --base "$base"
  fi
done
{
  echo '#include <stddef.h>'; echo '#include <stdint.h>'
  echo 'typedef struct { uint32_t addr; void (*fn)(void *); } FuncEnt;'
  for m in $MODULES; do n=${m%%:*}; echo "extern const FuncEnt g_functab_$n[]; extern const size_t g_functab_${n}_len;"; done
  echo 'const struct { const char *name; const FuncEnt *tab; const size_t *len; uint32_t base; } g_lifted[] = {'
  for m in $MODULES; do n=${m%%:*}; r=${m#*:}; pe=${r%%:*}; base=${r##*:}
    echo "  {\"$(basename "$pe")\", g_functab_$n, &g_functab_${n}_len, 0x${base}u},"; done
  echo '  {0}};'
} > gen/modules.c
OBJ=build/obj; OUT=build/nfs2se
CFLAGS="-O2 -g -w -fno-strict-aliasing -Iruntime $(pkg-config --cflags sdl2 2>/dev/null)"
# WWATCH=1: a debugging build that can log the game's writes to an address range (RT_WWATCH, core.c)
[ -n "$WWATCH" ] && { OBJ=build/obj-wwatch; OUT=build/nfs2se-wwatch; CFLAGS="$CFLAGS -DRT_WWATCH_BUILD"; }
mkdir -p $OBJ
ls gen/*/*.c gen/modules.c runtime/*.c | xargs -P"$(nproc)" -I{} sh -c '
  d=$(dirname {}); inc=""; [ "$d" != runtime ] && inc="-I$d"
  o='"$OBJ"'/$(echo {} | tr / _ | sed "s/\.c$/.o/")
  if [ ! -f "$o" ] || [ {} -nt "$o" ] || [ runtime/cpu.h -nt "$o" ] || [ runtime/rt.h -nt "$o" ]; then clang '"$CFLAGS"' $inc -c {} -o "$o" || exit 255; fi'
clang $OBJ/*.o -o $OUT -Wl,--wrap=f_0045BB50 -lm -lpthread $(pkg-config --libs sdl2) -lGLESv2 -lEGL
echo "built $OUT"
