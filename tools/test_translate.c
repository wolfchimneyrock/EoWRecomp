// Fuzz test for the Maxwell shader translator (runtime/nvn/maxwell/translate.c).
//
//  1. MSL must not change: every program is translated by the current translate.c (key.glsl = 0) and by the
//     reference translator (translate.c of REF compiled with old_* symbol names, see tools/test_translate.sh); the
//     MSL text and the reference's ShaderInfo fields must be identical.
//  2. GLSL must compile: the same program with key.glsl = 1 is written to <dump_dir>/glsl/<seed>_<n>.<vert|frag|comp>,
//     which tools/test_translate.sh compiles with glslangValidator (Vulkan 1.3) and validates with spirv-val. (Out of
//     process: the build machines have the tools, not the shaderc / SPIRV-Tools libraries.)
//  3. TT_GLSL_REF=1: the GLSL text must also be identical to the reference's (the new switches off: TT_STRUCTURE=0
//     TT_PUSH=0, TT_CB_VEC4 / TT_SINK_TEX unset).
// The translator's switches: TT_STRUCTURE=0 / TT_PUSH=0 (structured shaders, c1 inlined + push constants: on by default,
// as on Mali / Adreno), TT_CB_VEC4=1 (merged constant reads: Mali), TT_SINK_TEX=1, TT_GMEM_MIRROR=1 (no heap import:
// the copies' table), and the translator's own EOWRECOMP_VK_*. Every program gets random compiler constants (c1:
// inlined, BRX tables) and, for fragment shaders, a push base after a vertex shader's words.
//
// Programs are synthesized from the opcode patterns translate.c recognizes (every OPM(value, mask) and the
// half-precision `(op & mask) == value` tests, read from the source file): the unmasked bits are random, with
// register / predicate / cbuf / texture / attribute / branch fields biased towards meaningful values.
//
// usage: test_translate <translate.c> <count> <seed> [msl|glsl|both] [dump_dir]
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/nvn/maxwell/shader.h"

#include <stddef.h>
#include <sys/stat.h>

// the reference translator (same ShaderKey prefix and ShaderInfo layout)
ShaderInfo *old_maxwell_translate(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key);
void old_maxwell_free(ShaderInfo *s);
extern uint64_t old_maxwell_gmem_base, old_maxwell_gmem_size;
extern bool old_maxwell_gmem_mirror;
void old_maxwell_set_cb1(const uint8_t *data, uint32_t size);

static uint64_t rng_state, g_seed;
static uint64_t rnd64(void) {  // splitmix64
    uint64_t z = (rng_state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static uint32_t rnd(uint32_t n) { return (uint32_t)(rnd64() % n); }

typedef struct { uint64_t val, mask; } Pat;
static Pat pats[512];
static int npats;

static void load_patterns(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1);
    fread(s, 1, n, f);
    s[n] = 0;
    fclose(f);
    for (char *p = s; (p = strstr(p, "OPM(0x")); p += 4) {
        unsigned long long v, m;
        if (sscanf(p, "OPM(0x%llxull, 0x%llxull)", &v, &m) == 2 && npats < 512) {
            bool dup = false;
            for (int i = 0; i < npats; i++) dup |= pats[i].val == v && pats[i].mask == m;
            if (!dup) pats[npats++] = (Pat){v, m};
        }
    }
    for (char *p = s; (p = strstr(p, "(op & 0x")); p += 4) {  // emit_half: 16-bit opcode tests
        unsigned m, v;
        if (sscanf(p, "(op & 0x%x) == 0x%x", &m, &v) == 2 && npats < 512) pats[npats++] = (Pat){(uint64_t)v << 48, (uint64_t)m << 48};
    }
    free(s);
    if (npats < 50) { fprintf(stderr, "only %d patterns found in %s\n", npats, path); exit(1); }
}

static uint64_t setf(uint64_t w, int lo, int n, uint64_t v) {
    uint64_t m = ((1ull << n) - 1) << lo;
    return (w & ~m) | ((v << lo) & m);
}
static uint64_t getf(uint64_t w, int lo, int n) { return (w >> lo) & ((1ull << n) - 1); }
static bool is(uint64_t w, uint64_t v, uint64_t m) { return (w & m) == v; }

static uint32_t rreg(void) { uint32_t x = rnd(100); return x < 8 ? 255 : x < 12 ? 250 + rnd(5) : rnd(24); }

static const uint32_t attr_addrs[] = {0x70, 0x74, 0x78, 0x7c, 0x80, 0x84, 0x88, 0x8c, 0x90, 0x94, 0x98, 0x9c, 0xa0,
                                      0xb0, 0xc0, 0x100, 0x270, 0x27c, 0x2f8, 0x2fc, 0x3fc, 0x6c, 0x2e0, 0x2e4, 0x40};

// one random instruction at byte offset pc of a program of `total` bytes
static uint64_t gen_insn(uint32_t pc, uint32_t total) {
    if (rnd(400) == 0) {  // unknown instruction (but no cbuf index >= 17: the translators index arrays of 18)
        uint64_t w = rnd64();
        w = setf(w, 34, 5, rnd(17));
        return setf(w, 36, 5, rnd(17)) & ~(1ull << 38);
    }
    const Pat *p = &pats[rnd(npats)];
    uint64_t w = rnd64();
    if (rnd(100) < 90) w = setf(w, 0, 8, rreg());
    if (rnd(100) < 90) w = setf(w, 8, 8, rreg());
    if (rnd(100) < 70) w = setf(w, 20, 8, rreg());
    if (rnd(100) < 80) w = setf(w, 39, 8, rreg());
    w = setf(w, 16, 4, rnd(100) < 70 ? 7 : rnd(16));  // predicate guard (mostly PT)
    w = (w & ~p->mask) | p->val;
    // cbuf index fields: hardware has 18 buffers; 17 clashes with the gmem binding (both translators reject it)
    if (is(w, 0xef90000000000000ull, 0xfff8000000000000ull)) w = setf(w, 36, 5, rnd(17));  // LDC
    else if (getf(w, 34, 5) >= 17) w = setf(w, 34, 5, rnd(17));
    // texture index: few textures so that slots get reused
    if (is(w, 0xd000000000000000ull, 0xf400000000000000ull) || is(w, 0xc038000000000000ull, 0xfc38000000000000ull) ||
        is(w, 0xdf48000000000000ull, 0xfff8000000000000ull) || is(w, 0xdeb8000000000000ull, 0xfff8000000000000ull))
        w = setf(w, 36, 13, rnd(100) < 90 ? rnd(4) : rnd(8192));
    if (is(w, 0xdf48000000000000ull, 0xfff8000000000000ull) && rnd(100) < 90) w = setf(w, 22, 6, 1);  // TXQ dimension
    // attribute addresses (LD / ST a[], IPA)
    uint32_t aa = attr_addrs[rnd(sizeof attr_addrs / sizeof attr_addrs[0])] + (rnd(100) < 30 ? 4 * rnd(4) : 0);
    if (is(w, 0xefd8000000000000ull, 0xfff8000000000000ull) || is(w, 0xeff0000000000000ull, 0xfff8000000000000ull)) {
        w = setf(w, 20, 10, aa);
        if (rnd(100) < 85) w = setf(w, 8, 8, 255);  // not indexed
    }
    if (is(w, 0xe000000000000000ull, 0xff00000000000000ull)) {
        w = setf(w, 28, 10, aa);
        if (rnd(100) < 90) w = setf(w, 38, 1, 0);
        w = (w & ~p->mask) | p->val;
    }
    if (is(w, 0xf0c8000000000000ull, 0xfff8000000000000ull)) {  // S2R
        static const uint32_t srs[] = {0x00, 0x12, 0x21, 0x22, 0x23, 0x25, 0x26, 0x27, 0x50};
        w = setf(w, 20, 8, srs[rnd(sizeof srs / sizeof srs[0])]);
    }
    if (is(w, 0x5080000000000000ull, 0xfff8000000000000ull) && rnd(100) < 90) {  // MUFU: valid functions
        static const uint32_t fns[] = {0, 1, 2, 3, 4, 5, 8};
        w = setf(w, 20, 4, fns[rnd(7)]);
    }
    if (is(w, 0x5bc0000000000000ull, 0xffe0000000000000ull) || is(w, 0x4bc0000000000000ull, 0xffe0000000000000ull) ||
        is(w, 0x36c0000000000000ull, 0xfee0000000000000ull) || is(w, 0x53c0000000000000ull, 0xffe0000000000000ull))
        if (rnd(100) < 90) w = setf(w, 48, 4, 0);  // PRMT default mode
    // P2R / R2P .CC and BFE .BREV are unsupported: mostly off
    static const uint64_t bit40_ops[] = {0x38e8, 0x5ce8, 0x4ce8, 0x38f0, 0x5cf0, 0x4cf0, 0x5c00, 0x4c00, 0x3800};
    for (int i = 0; i < 9; i++)
        if ((w >> 48 & (bit40_ops[i] & 0xff00 ? (bit40_ops[i] >> 8 == 0x38 ? 0xfef8 : 0xfff8) : 0)) == bit40_ops[i] && rnd(100) < 90)
            w = setf(w, 40, 1, 0);
    if (is(w, 0x5098000000000000ull, 0xfff8000000000000ull) && rnd(100) < 90) return gen_insn(pc, total);  // CSET fails
    // branch targets inside the program
    uint64_t top12 = w & 0xfff0000000000000ull;
    if (top12 == 0xe240000000000000ull || top12 == 0xe290000000000000ull || top12 == 0xe2a0000000000000ull ||
        top12 == 0xe2b0000000000000ull || top12 == 0xe250000000000000ull) {
        if (rnd(100) < 90) {
            int64_t target = (int64_t)rnd(total / 8) * 8;
            int64_t off = target - (int64_t)pc - 8;
            if (off == -8) off = 8;  // not a self-branch (that ends the program)
            w = setf(w, 20, 24, (uint64_t)off & 0xFFFFFF);
        }
        w = setf(w, 5, 1, 0);
        w = (w & ~p->mask) | p->val;
    }
    return w;
}

static size_t gen_program(uint8_t *buf, size_t cap, int stage) {
    memset(buf, 0, cap);
    size_t hdr = stage == STAGE_COMPUTE ? 0 : 0x80;
    if (stage == STAGE_FRAGMENT) {
        uint32_t omap = rnd(100) < 40 ? 0xF : rnd(100) < 50 ? (uint32_t)rnd64() : (uint32_t)rnd64() & 0xFF;
        memcpy(buf + 0x30 + 0x48, &omap, 4);
        uint32_t dw = rnd(4) == 0 ? 2 : 0;
        memcpy(buf + 0x30 + 0x4C, &dw, 4);
    }
    int groups = 1 + rnd(rnd(100) < 70 ? 3 : 10);
    uint32_t total = (uint32_t)(groups + 1) * 32;
    uint64_t *code = (uint64_t *)(buf + hdr);
    int i = 0;
    for (int g = 0; g < groups; g++) {
        code[i++] = rnd64();  // scheduling word
        for (int k = 0; k < 3; k++, i++) code[i] = gen_insn((uint32_t)i * 8, total);
        // SUST.B (compute GLSL): mostly 2D RGBA with its handle loaded just before from the bound-image table, as the
        // game's shaders do (MOV Rh, c[2][0x120 + 8 * slot]); otherwise the translation always fails
        for (int k = 1; k < 3; k++) {
            uint64_t *s = &code[i - 3 + k];
            if (!is(*s, 0xeb20000000000000ull, 0xfff8000000000000ull) || rnd(100) >= 80) continue;
            *s = setf(setf(*s, 33, 3, 3), 20, 4, 15);
            uint32_t h = (uint32_t)getf(*s, 39, 8) == 255 ? 3 : (uint32_t)getf(*s, 39, 8);
            *s = setf(*s, 39, 8, h);
            s[-1] = 0x4c98000000000000ull | 0xfull << 39 | 2ull << 34 | (uint64_t)((0x120 + 8 * rnd(4)) / 4) << 20 | 7ull << 16 | h;
        }
    }
    code[i++] = 0x001f8000fc0007e0ull;
    code[i++] = 0xe30000000007000full;  // EXIT
    code[i++] = 0xe2400fffff87000full;  // BRA self (end of program)
    code[i++] = 0xe2400fffff87000full;
    return hdr + (size_t)i * 8 + 64;
}

static void gen_key(ShaderKey *k, int stage) {
    memset(k, 0, sizeof *k);
    for (int i = 0; i < 32; i++) k->attr_kind[i] = rnd(100) < 80 ? 0 : rnd(3);
    for (int i = 0; i < 8; i++) k->rt_kind[i] = rnd(100) < 80 ? 0 : rnd(3);
    k->depth_minus_one = rnd(2);
    k->flip_y = rnd(2);
    if (stage == STAGE_FRAGMENT && rnd(2)) k->vs_out_mask = (uint32_t)rnd64();
}

static bool same_info(const ShaderInfo *a, const ShaderInfo *b) {  // (b: the reference's - the fields up to error[])
    if ((a->msl == NULL) != (b->msl == NULL)) return false;
    if (a->msl && strcmp(a->msl, b->msl)) return false;
    ShaderInfo x, y;
    size_t n = offsetof(ShaderInfo, error) + sizeof x.error;
    memcpy(&x, a, n), memcpy(&y, b, n);
    x.msl = y.msl = NULL;
    return !memcmp(&x, &y, n);
}

static void dump(const char *dir, const char *tag, long n, const char *src, const char *err, const uint8_t *prog, size_t len) {
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s_%llu_%ld.txt", dir, tag, (unsigned long long)g_seed, n);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "// %s\n", err ? err : "");
    if (prog) {
        fprintf(f, "// program:");
        for (size_t i = 0; i < len; i += 8) fprintf(f, " %016llx", (unsigned long long)*(const uint64_t *)(prog + i));
        fprintf(f, "\n");
    }
    fputs(src ? src : "(null)", f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <translate.c> <count> <seed> [msl|glsl|both] [dump_dir]\n", argv[0]); return 2; }
    load_patterns(argv[1]);
    long count = atol(argv[2]);
    rng_state = g_seed = strtoull(argv[3], NULL, 0);
    const char *mode = argc > 4 ? argv[4] : "both";
    const char *dir = argc > 5 ? argv[5] : NULL;
    bool do_msl = strcmp(mode, "glsl") != 0, do_glsl = strcmp(mode, "msl") != 0;
    maxwell_gmem_base = old_maxwell_gmem_base = 0x80000000ull;
    maxwell_gmem_size = old_maxwell_gmem_size = 0x10000000ull;
    // TT_GMEM_MIRROR=1: every stage through the address table, no heap behind it (no host import: exec.c gmir_)
    maxwell_gmem_mirror = getenv("TT_GMEM_MIRROR") && atoi(getenv("TT_GMEM_MIRROR")) != 0;
    char gdir[512] = "";
    if (do_glsl && dir) { snprintf(gdir, sizeof gdir, "%s/glsl", dir); mkdir(gdir, 0755); }
    old_maxwell_gmem_mirror = maxwell_gmem_mirror;
    maxwell_glsl_structured = !(getenv("TT_STRUCTURE") && atoi(getenv("TT_STRUCTURE")) == 0);
    maxwell_glsl_push = !(getenv("TT_PUSH") && atoi(getenv("TT_PUSH")) == 0);
    maxwell_cb_vec4 = getenv("TT_CB_VEC4") && atoi(getenv("TT_CB_VEC4")) != 0;
    maxwell_sink_tex = getenv("TT_SINK_TEX") && atoi(getenv("TT_SINK_TEX")) != 0;
    bool glsl_ref = getenv("TT_GLSL_REF") && atoi(getenv("TT_GLSL_REF")) != 0;
    static uint8_t consts[512];
    static uint8_t prog[0x80 + 64 * 32 + 128];
    long msl_ok = 0, msl_fail = 0, msl_diff = 0, gl_ok = 0, gl_tfail = 0, gl_tfail_only = 0, gl_diff = 0;
    long gl_pcloop = 0, gl_fwd = 0, gl_struct = 0, gl_push = 0;
    long by_stage[3] = {0};
    char reasons[64][96];
    long reason_n[64];
    int nreasons = 0;
    for (long n = 0; n < count; n++) {
        int stage = rnd(3) == 0 ? STAGE_VERTEX : rnd(2) ? STAGE_FRAGMENT : STAGE_COMPUTE;
        size_t len = gen_program(prog, sizeof prog, stage);
        for (size_t i = 0; i < sizeof consts; i++) consts[i] = (uint8_t)rnd64();
        uint32_t csize = rnd(4) ? sizeof consts : 0;  // (c1; sometimes none)
        ShaderKey key;
        gen_key(&key, stage);
        bool msl_good = true;
        if (do_msl) {
            ShaderInfo *a = maxwell_translate(prog, len, stage, &key);
            ShaderInfo *b = old_maxwell_translate(prog, len, stage, &key);
            if (!same_info(a, b)) {
                if (msl_diff < 20) dump(dir, "msl_diff", n, a->msl, b->msl, prog, len);
                msl_diff++;
            }
            if (a->msl) msl_ok++; else msl_fail++;
            msl_good = a->msl != NULL;
            maxwell_free(a);
            old_maxwell_free(b);
        }
        if (do_glsl) {
            key.glsl = 1;
            maxwell_set_cb1(consts, csize);
            maxwell_push_cap = rnd(4) ? 64 : 32;
            maxwell_push_base = stage == STAGE_FRAGMENT ? 4 * rnd(17) : 0;  // (after a vertex shader's words)
            ShaderInfo *g = maxwell_translate(prog, len, stage, &key);
            maxwell_set_cb1(NULL, 0);
            if (glsl_ref) {
                old_maxwell_set_cb1(consts, csize);
                ShaderInfo *o = old_maxwell_translate(prog, len, stage, &key);
                old_maxwell_set_cb1(NULL, 0);
                if (!same_info(g, o)) {
                    if (gl_diff < 20) dump(dir, "glsl_diff", n, g->msl, o->msl, prog, len);
                    gl_diff++;
                }
                old_maxwell_free(o);
            }
            if (!g->msl) {
                gl_tfail++;
                if (msl_good && do_msl) gl_tfail_only++;
                {  // tally the reasons (GLSL-only: the MSL translation succeeded)
                    char r[96];
                    snprintf(r, sizeof r, "%s%.24s", msl_good && do_msl ? "GLSL-only " : "", g->error);
                    for (char *q = r; *q; q++) if (*q >= '0' && *q <= '9') *q = '#';
                    int i = 0;
                    while (i < nreasons && strcmp(reasons[i], r)) i++;
                    if (i == nreasons && nreasons < 64) { strcpy(reasons[nreasons], r); reason_n[nreasons++] = 0; }
                    if (i < 64) reason_n[i]++;
                }
            } else {
                gl_ok++;
                by_stage[stage == STAGE_VERTEX ? 0 : stage == STAGE_FRAGMENT ? 1 : 2]++;
                if (strstr(g->msl, "switch (pc)")) gl_pcloop++;
                else if (strstr(g->msl, "uint pc = 8u;")) gl_fwd++;
                else if (strstr(g->msl, "uint loops = 0u;")) gl_struct++;
                if (g->npush) gl_push++;
                if (*gdir) {
                    char path[600];
                    snprintf(path, sizeof path, "%s/%llu_%ld.%s", gdir, (unsigned long long)g_seed, n,
                             stage == STAGE_VERTEX ? "vert" : stage == STAGE_FRAGMENT ? "frag" : "comp");
                    FILE *f = fopen(path, "wb");
                    if (f) {
                        fprintf(f, "// program:");
                        for (size_t i = 0; i < len; i += 8) fprintf(f, " %016llx", (unsigned long long)*(const uint64_t *)(prog + i));
                        fprintf(f, "\n");
                        fputs(g->msl, f), fclose(f);
                    }
                }
            }
            maxwell_free(g);
        }
    }
    if (do_msl) printf("MSL: %ld programs, %ld translated, %ld failed in both, %ld DIFFERENCES\n", count, msl_ok, msl_fail, msl_diff);
    if (do_glsl) {
        printf("GLSL: %ld translated (vs %ld / fs %ld / cs %ld; %ld with the pc loop, %ld FWD blocks, %ld structured, %ld with "
               "push constants), %ld translation failures (%ld where MSL translated)%s",
               gl_ok, by_stage[0], by_stage[1], by_stage[2], gl_pcloop, gl_fwd, gl_struct, gl_push, gl_tfail, gl_tfail_only,
               glsl_ref ? "" : "\n");
        if (glsl_ref) printf(", %ld GLSL DIFFERENCES from the reference\n", gl_diff);
        for (int i = 0; i < nreasons; i++) printf("  translation failure: %-36s %ld\n", reasons[i], reason_n[i]);
    }
    return (msl_diff || gl_diff) ? 1 : 0;
}
