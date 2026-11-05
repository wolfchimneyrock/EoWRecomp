#!/bin/sh
# Fuzz test for runtime/nvn/maxwell/translate.c (see tools/test_translate.c), no GPU needed:
#   MSL output byte-identical to the reference translator (REF), and every GLSL translation compiles
#   (glslangValidator, Vulkan 1.3) and validates (spirv-val). TT_GLSL_REF=1: the GLSL byte-identical to REF's too.
# usage: tools/test_translate.sh [count] [jobs] [mode msl|glsl|both]
# The translator's switches come from the environment, e.g.:
#   tools/test_translate.sh 4000 8 glsl                          (structured, c1 inlined + push constants: Mali / Adreno)
#   TT_CB_VEC4=1 tools/test_translate.sh 4000 8 glsl              (+ merged constant reads: Mali)
#   TT_STRUCTURE=0 TT_PUSH=0 TT_GLSL_REF=1 REF=<commit> tools/test_translate.sh 4000 8 both   (all off: as REF)
#   TT_GMEM_MIRROR=1 (no heap import: the copies' table)   TT_SINK_TEX=1 (texture fetches moved down)
# Needs a C compiler (CC, default cc), glslangValidator and spirv-val.
set -e
cd "$(dirname "$0")/.."
COUNT=${1:-4000}
JOBS=${2:-8}
MODE=${3:-both}
REF=${REF:-467242a}  # (the translator before the structured shaders and push constants)
OUT=${OUT:-build/test_translate}
mkdir -p "$OUT/old"
rm -rf "$OUT/dump"
mkdir -p "$OUT/dump"
git show "$REF:runtime/nvn/maxwell/translate.c" > "$OUT/old/translate.c"
git show "$REF:runtime/nvn/maxwell/shader.h" > "$OUT/old/shader.h"
CC=${CC:-cc}
RENAME="-Dmaxwell_translate=old_maxwell_translate -Dmaxwell_free=old_maxwell_free
 -Dmaxwell_disassemble=old_maxwell_disassemble -Dmaxwell_gmem_base=old_maxwell_gmem_base
 -Dmaxwell_gmem_size=old_maxwell_gmem_size -Dmaxwell_dbg_code=old_maxwell_dbg_code -Dmaxwell_dbg_pc=old_maxwell_dbg_pc
 -Dmaxwell_dbg_reg=old_maxwell_dbg_reg -Dmaxwell_dbg_vs=old_maxwell_dbg_vs
 -Dmaxwell_gmem_mirror=old_maxwell_gmem_mirror -Dmaxwell_native_half=old_maxwell_native_half
 -Dmaxwell_gmem_hi_word=old_maxwell_gmem_hi_word -Dmaxwell_loop_guard=old_maxwell_loop_guard
 -Dmaxwell_set_cb1=old_maxwell_set_cb1 -Dmaxwell_glsl_structured=old_maxwell_glsl_structured
 -Dmaxwell_stacks_why=old_maxwell_stacks_why -Dmaxwell_glsl_push=old_maxwell_glsl_push
 -Dmaxwell_push_base=old_maxwell_push_base -Dmaxwell_push_cap=old_maxwell_push_cap
 -Dmaxwell_cb_vec4=old_maxwell_cb_vec4 -Dmaxwell_sink_tex=old_maxwell_sink_tex"
PRE=""
case "$(uname -s)" in MINGW*|MSYS*) PRE="-include malloc.h";; esac
$CC -O2 -w $PRE $RENAME -c "$OUT/old/translate.c" -o "$OUT/old.o"
$CC -O2 -Wall -Wno-format-truncation -Wno-format-overflow -c runtime/nvn/maxwell/translate.c -o "$OUT/new.o"
$CC -O2 -Wall tools/test_translate.c "$OUT/new.o" "$OUT/old.o" -o "$OUT/test_translate" -lm
PER=$(( (COUNT + JOBS - 1) / JOBS ))
i=0
PIDS=
while [ $i -lt "$JOBS" ]; do
  "$OUT/test_translate" runtime/nvn/maxwell/translate.c "$PER" $((i * 7919 + ${SEED:-1})) "$MODE" "$OUT/dump" > "$OUT/log_$i.txt" 2>&1 &
  PIDS="$PIDS $!"
  i=$((i + 1))
done
RC=0
for p in $PIDS; do wait "$p" || RC=1; done
cat "$OUT"/log_*.txt | grep -v '^\[shader\]'
if [ "$MODE" != msl ]; then
  # each translation: nothing, or its name (and a .log with the errors)
  G="$OUT/dump/glsl"
  ls "$G" | grep -E '\.(vert|frag|comp)$' | xargs -P "$JOBS" -I{} sh -c \
    'f="$0/{}"; if glslangValidator -V --target-env vulkan1.3 "$f" -o "$f.spv" > "$f.log" 2>&1 && spirv-val --target-env vulkan1.3 "$f.spv" >> "$f.log" 2>&1; then rm -f -- "$f.log" "$f.spv"; else rm -f -- "$f.spv"; echo "{}"; fi' \
    "$G" > "$OUT/glsl_failed.txt" || true
  NF=$(wc -l < "$OUT/glsl_failed.txt")
  NT=$(ls "$G" | grep -cE '\.(vert|frag|comp)$' || true)
  echo "glslang + spirv-val: $NT GLSL translations, $NF failed (logs: $G/<file>.log)"
  [ "$NF" -eq 0 ] || RC=1
fi
exit $RC
