// Maxwell (GM20B) shader translation to Metal Shading Language (macOS) or Vulkan GLSL (Windows; compiled to
// SPIR-V at runtime with shaderc). ShaderKey.glsl selects the dialect; the MSL output does not depend on it.
//
// GLSL contract (what the Vulkan renderer binds):
//  - `#version 460`, entry point `main`. Descriptor set: vertex 0, fragment 1, compute 0.
//  - hardware cbuf k -> binding k: `layout(std140, set=S, binding=k) uniform CB<k> { uvec4 cb<k>[N]; };`
//    (anonymous block). N = max(cbuf_size[k] / 16, highest constant-index vec4 + 1), and at least 4096 when
//    cbuf_dynamic has bit k (the renderer binds up to 64 KB). Register-indexed reads are clamped to N - 1.
//  - global memory (LDG/STG) -> binding GMEM_BUFFER_INDEX (17):
//    `layout(std430, set=S, binding=17) [readonly] buffer GMem { uint w[]; } gmem_;`, 32-bit words relative to
//    maxwell_gmem_base. `readonly` unless the shader stores (needs vertexPipeline/fragmentStoresAndAtomics then);
//    byte / short stores are atomicAnd + atomicOr on the containing word.
//  - rscale table -> binding 18: `uniform RScale { vec4 rscale[65]; };` (std140, the MSL buffer 18 contents).
//  - per-draw tint -> binding 19: `uniform Tint { vec4 ltint[2]; };` (fragment, when uses_tint).
//  - texture slot i (ShaderInfo.tex[i]) -> binding 32 + i, a combined image sampler: sampler1D/2D/3D/Cube/
//    1DArray/2DArray/CubeArray, or its *Shadow variant when tex[i].shadow.
//  - vertex inputs `layout(location=i) in vec4|ivec4|uvec4 a<i>;` (key->attr_kind); varyings: location i =
//    generic attribute i, `vec4 v<i>` (fragment side flat / noperspective as in MSL); `invariant gl_Position`;
//    gl_PointSize when writes_point_size. Fragment outputs `layout(location=rt) out vec4|ivec4|uvec4 c<rt>;`,
//    gl_FragDepth when writes_depth.
//  - clip space exactly as the MSL output (key->flip_y / depth_minus_one epilogue unchanged): the renderer uses
//    a negative-height viewport so Vulkan matches Metal's y-up NDC, clip z in [0, 1].
//  - compute: `layout(local_size_x_id = 0, local_size_y_id = 1, local_size_z_id = 2) in;` (spec constants 0..2).
//  - extensions: GL_EXT_shader_explicit_arithmetic_types_int64 (64-bit carry / address math -> shaderInt64),
//    GL_KHR_shader_subgroup_basic/_shuffle/_shuffle_relative (SHFL, FSWZADD lane: fragment/compute subgroup
//    support), GL_EXT_texture_shadow_lod (explicit-LOD array / cube shadow lookups).
//  - not in GLSL: the maxwell_dbg_vs position dump (buffer 29) is MSL only.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { STAGE_VERTEX = 0, STAGE_FRAGMENT = 1, STAGE_GEOMETRY = 2, STAGE_TESS_CTRL = 3, STAGE_TESS_EVAL = 4,
       STAGE_COMPUTE = 5 };

enum { TEXTYPE_1D, TEXTYPE_2D, TEXTYPE_3D, TEXTYPE_CUBE, TEXTYPE_2D_ARRAY, TEXTYPE_CUBE_ARRAY, TEXTYPE_1D_ARRAY,
       TEXTYPE_BUFFER, TEXTYPE_2D_MS };

typedef struct {
    uint8_t attr_kind[32];  // vertex inputs: 0 float, 1 signed int, 2 unsigned int
    uint8_t rt_kind[8];     // fragment outputs: 0 float, 1 signed int, 2 unsigned int
    bool depth_minus_one;   // NVN depth mode [-1,1]: remap clip z
    bool flip_y;            // lower-left window origin
    bool flip_x;            // mirror the image left to right (mirror mode's scene composite). (In the padding
                            // after flip_y: keys without it hash as before - shader caches stay valid.)
    uint32_t vs_out_mask;   // fragment: varyings the vertex shader provides
    uint8_t glsl;           // emit Vulkan GLSL (see the contract above) instead of MSL
    uint16_t stencil_tex;   // (GLSL) texture slots bound to the stencil of a depth-stencil texture: integer samplers,
                            // the stencil value returned as raw bits (in the padding: other keys hash as before)
} ShaderKey;

typedef struct {
    uint32_t handle_offset;  // byte offset of the texture handle in the bound texture cbuf
    uint8_t type;            // TEXTYPE_*
    bool shadow, is_array, is_image, bindless;
    uint32_t bindless_cbuf, bindless_offset;
} ShaderTexture;

typedef struct {
    int stage;
    char *msl;              // complete MSL or GLSL source (MSL entry "main0", GLSL "main"); NULL on failure
    uint32_t cbuf_mask;     // hardware constant buffers read
    uint32_t cbuf_size[18];
    uint32_t cbuf_dynamic;  // cbufs indexed with a register: bind the whole buffer
    int ntex;
    ShaderTexture tex[64];
    uint32_t attr_mask;     // vertex: input attributes; fragment: input varyings
    uint32_t out_mask;      // vertex: output varyings; fragment: render targets written
    bool writes_depth, uses_discard, writes_point_size;
    bool uses_gmem;         // global memory access: bind the guest heap at GMEM_BUFFER_INDEX
    bool uses_gmem_map;     // (GLSL compute; every stage with maxwell_gmem_mirror) gmem loads look their address up in GMap (uniform, binding
                            // GMAP_BUFFER_INDEX) and read GFast (storage, GFAST_BUFFER_INDEX) - a device-local copy of the
                            // dispatch's storage buffers - when found, stores too (copied back after the dispatch).
                            // runtime/nvn/vulkan/exec.c
    bool uses_gmem_store;   // writes global memory (STG)
    bool uses_tint;        // fragment: reads the per-draw tint (buffer 19, float4[2]) applied to color target 0
    bool uses_rscale;       // reads the resolution-scale table (fragment/compute buffer 18): float4 [0] = render
                            // target scale, [1 + i] = allocation scale of texture i
    uint32_t local_mem;     // bytes of local memory
    uint32_t num_insts;
    char error[256];        // set when translation fails
    // (GLSL vertex / fragment, maxwell_glsl_push) constant buffer words moved into push constants (pcw[k]: the word at
    // byte push_off[k] of cbuf push_cb[k]): the most read, loops first. (After error[]: the fields before it are the
    // reference translator's in tools/test_translate.)
    uint8_t npush, push_cb[64];
    uint16_t push_off[64], push_base;  // (push_base: the block's offset in the push range, bytes)
} ShaderInfo;

// Global memory (LDG/STG) is addressed through one buffer that wraps the guest heap.
#define GMEM_BUFFER_INDEX 17
// (GLSL compute, Vulkan) the fast copy of a dispatch's storage buffers: its address table and its data
#define GMAP_BUFFER_INDEX 20
#define GFAST_BUFFER_INDEX 21
// (GLSL, maxwell_gmem_hi_word set) the guest heap's upper part, for GPUs whose storage buffers reach less than the heap:
// gmem_ covers words [0, GMEM_LO_WORDS), gmemh_ words from maxwell_gmem_hi_word on (translate.c gmem_split)
#define GMEMH_BUFFER_INDEX 22
#define GMEM_LO_WORDS 0x20000000u  // 2 GB
#define GMAP_MAX 16  // ranges in the table (GMap { uvec4 gmap[GMAP_MAX + 1]; }: [i] = addr lo, addr hi, size, word offset;
                     // [GMAP_MAX].x = count)
extern uint64_t maxwell_gmem_base, maxwell_gmem_size;
// (GLSL) global memory through buffer addresses instead of a storage buffer: binding GMEM_BUFFER_INDEX is then
// `uniform GMemBase { uvec4 gmem_base[GMEM_CHUNKS]; }`, [i].xy the address of the heap's i-th GMEM_CHUNK_BYTES (the
// heap's memory comes in pieces of that size, maxwell_gmem_size in all), read with GL_EXT_buffer_reference(_uvec2).
// For drivers that can't import guest memory as a host pointer (Android / Adreno). Either way the shaders reach a
// 32-bit word of global memory as gmw(word index).
extern bool maxwell_gmem_bda;
#define GMEM_CHUNK_LOG2 29  // 512 MB
#define GMEM_CHUNK_BYTES (1ull << GMEM_CHUNK_LOG2)
#define GMEM_CHUNKS 8       // (HEAP_MAX, 3.25 GB, in 512 MB pieces: 7)
extern bool maxwell_native_half;  // GLSL fragment / compute: halves through unpackHalf2x16 (translate.c)
extern uint32_t maxwell_gmem_hi_word;  // 0: one heap buffer; else the word where gmemh_ (binding 22) starts
// (GLSL / Vulkan) No heap import (Turnip, PanVK: no VK_EXT_external_memory_host; LAS Recomp issue #483): every stage
// reaches its storage buffers through the address table (GMap / GFast, as compute does), which points into copies of
// the guest memory they cover (vulkan/exec.c gmir_). There is no heap buffer (gmem_) then: accesses outside the table
// read 0 and drop their stores. Set once before the first translation, never changed after (read by every thread).
// Not with maxwell_gmem_bda (that heap is reachable).
extern bool maxwell_gmem_mirror;
// (GLSL) Structured shaders: the branch stacks resolved at translation and the branches as nested if / else /
// do-while (goto elimination) instead of the pc state machine or the FWD blocks. The backend's default for this GPU
// (exec.c: Mali, Adreno); EOWRECOMP_VK_STATIC_STACKS / EOWRECOMP_VK_STRUCTURE =1 / =0 override.
extern bool maxwell_glsl_structured;
// (GLSL) c1 inlined (statically indexed compiler constants as literals: maxwell_set_cb1's data) and the hottest
// constant buffer words as push constants (ShaderInfo.npush). The backend's default (exec.c: Mali, Adreno);
// EOWRECOMP_VK_INLINE_C1 / EOWRECOMP_VK_PUSH_CB =1 / =0 override. The fragment shader's push words start at
// maxwell_push_base (bytes: after the vertex shader's), at most maxwell_push_cap words in all: set by the caller around
// maxwell_translate, on its own thread.
extern bool maxwell_glsl_push;
extern _Thread_local uint32_t maxwell_push_base, maxwell_push_cap;
// The program's compiler constants (bound as c1) for the following maxwell_translate calls on this thread, for the c1
// inlining. size 0 = unknown (c1 stays a uniform buffer).
void maxwell_set_cb1(const uint8_t *data, uint32_t size);
// (GLSL) Texture fetches moved down to their first use (EOWRECOMP_VK_SINK_TEX=1, off): from LAS Recomp's fork.
extern bool maxwell_sink_tex;
// (GLSL) Constant buffer reads at one register offset merged into uvec4 loads (EOWRECOMP_VK_CB_VEC4 overrides; the
// backend's default: Mali, a loss on Turnip): from LAS Recomp's fork.
extern bool maxwell_cb_vec4;
// Debug: when translating the shader whose code starts at maxwell_dbg_code, output register
// maxwell_dbg_reg as captured after the instruction at maxwell_dbg_pc (greyscale, all targets).
extern const uint8_t *maxwell_dbg_code;
extern uint32_t maxwell_dbg_pc;
extern int maxwell_dbg_reg;
// Debug: the vertex shader at maxwell_dbg_vs writes each vertex's position to buffer 18 (slot iid*8+vid).
extern const uint8_t *maxwell_dbg_vs;
#define DBG_VS_BUFFER_INDEX 29

// code: shader binary (with the 0x50-byte SPH header for graphics stages)
ShaderInfo *maxwell_translate(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key);
void maxwell_free(ShaderInfo *s);
// Disassemble for debugging (writes text to f).
void maxwell_disassemble(const uint8_t *code, size_t max_size, int stage, void *file);

extern uint32_t maxwell_loop_guard;  // translate.c
extern bool maxwell_nmin;  // translate.c: (GLSL) fmin / fmax / saturate as GLSL.std.450 NMin / NMax / NClamp
