#!/usr/bin/env bash
#
# build_presets.sh — build the ThumbyOne release firmware presets reproducibly.
#
# Each preset's flag set is pinned here as the SINGLE source of truth, so the
# release UF2s can never drift the way ad-hoc `build_*/` dirs have (e.g. stray
# ELITE/ROGUE=ON, or a preset silently built with the wrong FAT offset). Every
# preset passes the COMPLETE 12-flag set explicitly — nothing is left to CMake
# defaults or a stale cache.
#
# Layout rules baked in:
#   * Standard build = everything ON + ThumbyCraft slot; Cue and Indemnity are
#     Mote games (they live in /mote/), so WITH_ELITE and WITH_ROGUE stay OFF —
#     except the explicit `rogue` preset, which ships Cue as a slot on purpose.
#
# Usage:
#   ./build_presets.sh                 # build the 3 main release UF2s (default, nomd, nodoom)
#   ./build_presets.sh all             # build every preset defined below
#   ./build_presets.sh nodoom nomd     # build only the named presets
#   ./build_presets.sh list            # list preset names + their flags
#
# Env knobs:
#   JOBS=8        parallel build jobs (default: nproc)
#   OUT_DIR=...   where finished firmware_thumbyone*.uf2 are copied
#                 (default: the workspace root one level up — the folder the
#                  device is flashed from: \\wsl...\home\maustin\thumby-color)
#   CLEAN=1       wipe each preset's build dir before configuring
#   PICO_SDK_PATH=...  override the SDK (else CMakeLists' default is used)
#
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Default to the workspace root (parent of the repo) — the folder the device is
# flashed from. Override with OUT_DIR=... (e.g. the repo dir) if needed.
OUT_DIR="${OUT_DIR:-$(dirname "$REPO_DIR")}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 8)}"

# Complete standard flag set — every WITH_* stated explicitly.
STD="\
-DTHUMBYONE_WITH_NES=ON  -DTHUMBYONE_WITH_P8=ON    -DTHUMBYONE_WITH_DOOM=ON \
-DTHUMBYONE_WITH_MPY=ON  -DTHUMBYONE_WITH_SCUMM=ON -DTHUMBYONE_WITH_MOTE=ON \
-DTHUMBYONE_WITH_CRAFT=ON -DTHUMBYONE_WITH_MD=ON   -DTHUMBYONE_WITH_PCE=ON \
-DTHUMBYONE_WITH_ROGUE=OFF -DTHUMBYONE_WITH_ELITE=OFF -DTHUMBYONE_WITH_ROGUE9=OFF"

# Preset table: "suffix|<flags>". Empty suffix => firmware_thumbyone.uf2.
# A later -D overrides the STD value it repeats, so each line is the full,
# unambiguous flag set for that preset.
PRESETS=(
  "|$STD"                                                        # default — full build
  "_nomd|$STD -DTHUMBYONE_WITH_MD=OFF"                           # no Mega Drive (+1 MB FAT)
  "_nodoom|$STD -DTHUMBYONE_WITH_DOOM=OFF"                       # no DOOM (+2.4 MB FAT)
  "_nodoom_nomd|$STD -DTHUMBYONE_WITH_DOOM=OFF -DTHUMBYONE_WITH_MD=OFF"
  "_nompy|$STD -DTHUMBYONE_WITH_MPY=OFF"                         # no MicroPython slot
  "_nodoom_nompy|$STD -DTHUMBYONE_WITH_DOOM=OFF -DTHUMBYONE_WITH_MPY=OFF"
  "_nocraft|$STD -DTHUMBYONE_WITH_CRAFT=OFF"                     # no ThumbyCraft slot
  "_rogue|$STD -DTHUMBYONE_WITH_ROGUE=ON"                        # ships Cue as a slot (non-standard)
  # Single-system / flavour presets — REVIEW the flag set before a release:
  #   these drop most slots and their exact intent isn't pinned yet.
  # "_mpyonly|-DTHUMBYONE_WITH_NES=OFF -DTHUMBYONE_WITH_P8=OFF -DTHUMBYONE_WITH_DOOM=OFF -DTHUMBYONE_WITH_SCUMM=OFF -DTHUMBYONE_WITH_MOTE=OFF -DTHUMBYONE_WITH_CRAFT=OFF -DTHUMBYONE_WITH_MD=OFF -DTHUMBYONE_WITH_PCE=OFF -DTHUMBYONE_WITH_MPY=ON"
  # "_scummonly|-DTHUMBYONE_WITH_NES=OFF -DTHUMBYONE_WITH_P8=OFF -DTHUMBYONE_WITH_DOOM=OFF -DTHUMBYONE_WITH_MPY=OFF -DTHUMBYONE_WITH_MOTE=OFF -DTHUMBYONE_WITH_CRAFT=OFF -DTHUMBYONE_WITH_MD=OFF -DTHUMBYONE_WITH_PCE=OFF -DTHUMBYONE_WITH_SCUMM=ON"
  # "_retro|... TODO: confirm which cores"
  # "_revert|... TODO: confirm intent"
)

MAIN_PRESETS=("" "_nomd" "_nodoom")   # what a no-arg run builds

flags_for() { local want="$1" e s; for e in "${PRESETS[@]}"; do s="${e%%|*}"; [ "$s" = "$want" ] && { printf '%s' "${e#*|}"; return 0; }; done; return 1; }

list_presets() {
  local e s f
  printf '%-16s %s\n' "UF2" "FLAGS"
  for e in "${PRESETS[@]}"; do
    s="${e%%|*}"; f="${e#*|}"
    printf '%-16s %s\n' "firmware_thumbyone${s}.uf2" "$f"
  done
}

build_one() {
  local suffix="$1" flags bdir uf2 out
  flags="$(flags_for "$suffix")" || { echo "!! unknown preset '${suffix:-<default>}'"; return 1; }
  bdir="$REPO_DIR/build_rel${suffix:-_default}"
  out="$OUT_DIR/firmware_thumbyone${suffix}.uf2"
  echo "=================================================================="
  echo ">> preset: firmware_thumbyone${suffix}.uf2"
  echo "   flags:  $flags"
  echo "   build:  $bdir"
  [ "${CLEAN:-0}" = "1" ] && rm -rf "$bdir"
  # shellcheck disable=SC2086
  cmake -S "$REPO_DIR" -B "$bdir" $flags >/dev/null
  cmake --build "$bdir" -j"$JOBS"
  uf2="$bdir/thumbyone.uf2"
  [ -f "$uf2" ] || { echo "!! build produced no $uf2"; return 1; }
  cp "$uf2" "$out"
  echo "   -> $out  ($(du -h "$out" | cut -f1))"
}

# ---- arg handling ----
case "${1:-}" in
  list)  list_presets; exit 0 ;;
  all)   TARGETS=(); for e in "${PRESETS[@]}"; do TARGETS+=("${e%%|*}"); done ;;
  "")    TARGETS=("${MAIN_PRESETS[@]}") ;;
  *)     TARGETS=(); for a in "$@"; do [ "$a" = "default" ] && a=""; TARGETS+=("$a"); done ;;
esac

mkdir -p "$OUT_DIR"
declare -a OK=() FAIL=()
for t in "${TARGETS[@]}"; do
  if build_one "$t"; then OK+=("firmware_thumbyone${t}.uf2"); else FAIL+=("firmware_thumbyone${t}.uf2"); fi
done

echo "=================================================================="
echo "built ${#OK[@]}: ${OK[*]:-none}"
[ "${#FAIL[@]}" -gt 0 ] && { echo "FAILED ${#FAIL[@]}: ${FAIL[*]}"; exit 1; }
echo "all requested presets built -> $OUT_DIR"
