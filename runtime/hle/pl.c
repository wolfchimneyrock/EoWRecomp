// nn::pl shared system fonts. The Switch's system fonts are not part of the game, so every SharedFontType the game
// asks for is filled from, in order of preference:
//  1. EOWRECOMP_FONT=<file> (.ttf / .otf / .ttc face 0 / .bfttf)
//  2. a dump from the user's own console in fonts/, under the console's own names (plain .ttf versions work too):
//     nintendo_udsg-r_std_003 (Standard, Morisawa UD Shin Go), nintendo_udsg-r_org_zh-cn_003 (Chinese Simplified),
//     nintendo_udsg-r_ext_zh-cn_003 (its Ext), nintendo_udjxh-db_zh-tw_003 (Chinese Traditional),
//     nintendo_udsg-r_ko_003 (Korean), nintendo_ext_003 (NintendoExt button glyphs)
//  3. a substitute of that same language where the OS has one - a gothic like UD Shin Go is much closer to the
//     game's look than Arial. Without one (a Linux without a CJK font, say) the type serves Standard, which is
//     what a game asking for a language whose font is missing gets on the console too - but then that language's
//     characters are the substitute's, so Chinese / Korean text can show missing glyphs.
//     Standard's Japanese gothics are skipped when the game plays in Russian: Japanese fonts carry Cyrillic as
//     full-width characters (JIS X 0208), every letter a square apart (issue #32) - Arial / DejaVu Sans then.
//  4. Standard only, on Linux and any other platform without a known system font (not macOS / Windows), ahead of
//     the OS's own: BIZ UDPGothic shipped with the app (third_party/fonts/BIZUDPGothic, SIL OFL), Morisawa's free
//     font from the same universal-design gothic family as UD Shin Go. (Skipped for Russian like the other Japanese
//     gothics.)
//  5. Linux: fontconfig's best TrueType font for the language (`fc-match -s`, when the system has fontconfig): the
//     CJK / Korean types, and Standard where nothing above fits (Russian without DejaVu, say).
//  6. Standard only: DejaVu Sans shipped with the app (third_party/fonts/DejaVu, free license), for systems with none
//     of the above (Bazzite / Fedora Atomic have no DejaVu - issue #37).
//  7. NintendoExt only: Open Pad Glyphs shipped with the app (third_party/fonts/OpenPadGlyphs, SIL OFL), a button font
//     drawn from scratch for the console's private-use button characters (A / B / X / Y, L / R / ZL / ZR, sticks,
//     d-pad, + / -, HOME, ...), in the controller's labels when Button icons is set to (or detects) an Xbox or
//     PlayStation pad, at the console's positions (east = A = Xbox B = circle). Without it the type fell back to
//     Standard, which has none of these characters: every in-text button showed as a box.
// Substitutes must be TrueType ('glyf' outlines): the game's font engine crashes on CFF ones such as Noto Sans CJK,
// so those are passed over, as are fonts without the language's characters.
// On those same platforms a substitute (anything but the console's own files) gets the console font's OS/2 Windows
// metrics, which the game sizes text by (match_switch_metrics): without that every substitute draws about 1.5x too
// large. macOS and Windows keep Hiragino / Yu Gothic exactly as they are, at their current size (OS_FONTS_KNOWN).
// .bfttf is the console's obfuscated TTF: 8-byte header (magic, size) then every u32 XORed with a fixed key (bfttf_decode).
// .ttc collections get face 0 copied out as a standalone font (the game's font engine wants one sfnt).
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../hle.h"
#include "../input_bind.h"
#include "../paths.h"
#include "../settings.h"

// macOS and Windows have system fonts known to work well (Hiragino / Yu Gothic): the OS's fonts first, untouched.
// Everywhere else the bundled BIZ UDPGothic serves ahead of them and every substitute gets the console's metrics.
#if defined(__APPLE__) || defined(_WIN32)
#define OS_FONTS_KNOWN 1
#else
#define OS_FONTS_KNOWN 0
#endif

typedef struct { uint64_t addr, size; } Font;
// SharedFontType: 0 Standard, 1 ChineseSimple, 2 ChineseSimpleExt, 3 ChineseTraditional, 4 Korean, 5 NintendoExt
enum { F_STD, F_ZH_CN, F_ZH_CN_EXT, F_ZH_TW, F_KO, F_EXT, N_FONT_TYPES };
static Font fonts[N_FONT_TYPES];

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*len + 4);
    if (fread(p, 1, *len, f) != *len) { free(p); p = NULL; }
    fclose(f);
    return p;
}

// .bfttf -> TTF in place (returns the new length, 0 if it isn't one). Read as little-endian words: the first is
// 0x7F9A0218 ^ key, the second the font's size ^ key, then the font itself, each word XORed with the key. The words
// come in either byte order: swapped (issue #48's files; decoded without the swap they never matched), or in the
// font's own order (the font archives of a firmware dump, e.g. from an emulator's installed firmware). The magic word
// says which.
static size_t bfttf_decode(uint8_t *d, size_t len) {
    static const uint32_t KEY = 0x49621806, MAGIC = 0x7F9A0218;
    if (len < 12) return 0;
    uint32_t w0, w1;
    memcpy(&w0, d, 4);
    memcpy(&w1, d + 4, 4);
    bool swap;
    if ((w0 ^ KEY) == MAGIC) swap = true;
    else if ((w0 ^ __builtin_bswap32(KEY)) == __builtin_bswap32(MAGIC)) swap = false;
    else return 0;
    uint32_t key = swap ? KEY : __builtin_bswap32(KEY);
    uint32_t size = w1 ^ key;
    if (!swap) size = __builtin_bswap32(size);
    size_t words = ((size_t)size + 3) & ~(size_t)3;  // (the last word is padded)
    if (size < 4 || words > len - 8) return 0;
    for (size_t i = 0; i < words; i += 4) {
        uint32_t w;
        memcpy(&w, d + 8 + i, 4);
        w ^= key;
        if (swap) put32(d + i, w);  // (big-endian: the font's own byte order)
        else memcpy(d + i, &w, 4);
    }
    uint32_t ver = be32(d);
    return ver == 0x00010000u || ver == 0x4F54544Fu || ver == 0x74727565u ? size : 0;  // TrueType / 'OTTO' / 'true'
}

// .ttc -> face 0 as its own sfnt (table data copied, offsets rewritten)
static uint8_t *ttc_face0(const uint8_t *f, size_t len, size_t *out_len) {
    if (len < 16 || memcmp(f, "ttcf", 4)) return NULL;
    uint32_t o = be32(f + 12);
    if (o + 12 > len) return NULL;
    uint32_t nt = (uint32_t)f[o + 4] << 8 | f[o + 5];
    size_t total = 12 + 16 * nt;
    for (uint32_t i = 0; i < nt; i++) total += (be32(f + o + 12 + 16 * i + 12) + 3) & ~3u;
    uint8_t *out = calloc(1, total);
    memcpy(out, f + o, 12);
    size_t pos = 12 + 16 * nt;
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *e = f + o + 12 + 16 * i;
        uint32_t off = be32(e + 8), n = be32(e + 12);
        if ((size_t)off + n > len) { free(out); return NULL; }
        memcpy(out + 12 + 16 * i, e, 8);  // tag, checksum
        put32(out + 12 + 16 * i + 8, (uint32_t)pos);
        put32(out + 12 + 16 * i + 12, n);
        memcpy(out + pos, f + off, n);
        pos += (n + 3) & ~3u;
    }
    *out_len = total;
    return out;
}

// The font's glyph for this character, 0 if none (its cmap: format 4 or 12 subtables, Unicode encodings)
static uint32_t font_gid(const uint8_t *f, size_t len, uint32_t cp) {
    if (len < 12) return 0;
    uint32_t nt = (uint32_t)f[4] << 8 | f[5], cmap = 0, cmap_len = 0;
    for (uint32_t i = 0; i < nt && 12 + 16 * (i + 1) <= len; i++)
        if (!memcmp(f + 12 + 16 * i, "cmap", 4)) { cmap = be32(f + 12 + 16 * i + 8); cmap_len = be32(f + 12 + 16 * i + 12); }
    if (!cmap || (size_t)cmap + cmap_len > len || cmap_len < 4) return 0;
    const uint8_t *c = f + cmap, *end = f + cmap + cmap_len;
    uint32_t ns = (uint32_t)c[2] << 8 | c[3];
    for (uint32_t i = 0; i < ns && c + 4 + 8 * (i + 1) <= end; i++) {
        uint32_t plat = (uint32_t)c[4 + 8 * i] << 8 | c[5 + 8 * i], enc = (uint32_t)c[6 + 8 * i] << 8 | c[7 + 8 * i];
        if (!(plat == 0 || (plat == 3 && (enc == 1 || enc == 10)))) continue;  // (Unicode subtables only)
        const uint8_t *t = c + be32(c + 8 + 8 * i);
        if (t + 4 > end) continue;
        uint32_t fmt = (uint32_t)t[0] << 8 | t[1];
        if (fmt == 4 && cp <= 0xFFFF && t + 14 <= end) {
            uint32_t segs = ((uint32_t)t[6] << 8 | t[7]) / 2;
            const uint8_t *ends = t + 14, *starts = ends + 2 * segs + 2, *deltas = starts + 2 * segs, *ranges = deltas + 2 * segs;
            if (ranges + 2 * segs > end) continue;
            for (uint32_t k = 0; k < segs; k++) {
                uint32_t e = (uint32_t)ends[2 * k] << 8 | ends[2 * k + 1], st = (uint32_t)starts[2 * k] << 8 | starts[2 * k + 1];
                if (cp > e) continue;
                if (cp < st) break;
                uint32_t ro = (uint32_t)ranges[2 * k] << 8 | ranges[2 * k + 1], d = (uint32_t)deltas[2 * k] << 8 | deltas[2 * k + 1];
                if (!ro) return (cp + d) & 0xFFFF;
                const uint8_t *g = ranges + 2 * k + ro + 2 * (cp - st);
                if (g + 2 > end) break;
                return (((uint32_t)g[0] << 8 | g[1]) + (g[0] | g[1] ? d : 0)) & 0xFFFF;
            }
        } else if (fmt == 12 && t + 16 <= end) {
            uint32_t ng = be32(t + 12);
            for (uint32_t k = 0; k < ng && t + 16 + 12 * (k + 1) <= end; k++) {
                const uint8_t *gr = t + 16 + 12 * k;
                if (cp >= be32(gr) && cp <= be32(gr + 4) && be32(gr + 8) + (cp - be32(gr))) return be32(gr + 8) + (cp - be32(gr));
            }
        }
    }
    return 0;
}
static bool font_has(const uint8_t *f, size_t len, uint32_t cp) { return font_gid(f, len, cp) != 0; }

// The characters the game counts on in every font it's given: a space and "□" (U+25A1), its box for a missing
// character - it looks the box's glyph up at startup without checking for none, and crashed on a PC whose Yu Gothic
// lacked it (issue #45). A substitute without them is passed over for the next one.
static bool font_usable(const uint8_t *f, size_t len) { return font_has(f, len, 0x20) && font_has(f, len, 0x25A1); }

// TrueType outlines? (a 'glyf' table: the game's font engine can't draw CFF-flavoured OpenType fonts and crashes on them)
static bool font_truetype(const uint8_t *f, size_t len) {
    if (len < 12) return false;
    uint32_t nt = (uint32_t)f[4] << 8 | f[5];
    for (uint32_t i = 0; i < nt && 12 + 16 * (i + 1) <= len; i++)
        if (!memcmp(f + 12 + 16 * i, "glyf", 4)) return true;
    return false;
}

// The game's font engine sizes a font by its OS/2 usWinAscent + usWinDescent: 1.5 em for the console's UD Shin Go
// (1158 + 378 of 1024 units) against the usual 1.0 em, so a substitute draws about 1.5x too large. Give it the console
// font's values; its text then matches the console's size (checked against the real font on the file select screen).
static void match_switch_metrics(uint8_t *d, size_t len) {
    if (len < 12) return;
    unsigned ntab = (unsigned)d[4] << 8 | d[5];
    if (12 + 16 * (size_t)ntab > len) return;
    const uint8_t *head = NULL;
    uint8_t *os2 = NULL;
    for (unsigned i = 0; i < ntab; i++) {
        const uint8_t *e = d + 12 + 16 * i;
        uint32_t off = be32(e + 8), tlen = be32(e + 12);
        if (off > len || tlen > len - off) continue;
        if (!memcmp(e, "head", 4) && tlen >= 20) head = d + off;
        if (!memcmp(e, "OS/2", 4) && tlen >= 78) os2 = d + off;
    }
    if (!head || !os2) return;
    unsigned upem = (unsigned)head[18] << 8 | head[19];
    if (!upem) return;
    unsigned asc = (1158 * upem + 512) / 1024, desc = (378 * upem + 512) / 1024;
    os2[74] = (uint8_t)(asc >> 8), os2[75] = (uint8_t)asc;    // usWinAscent
    os2[76] = (uint8_t)(desc >> 8), os2[77] = (uint8_t)desc;  // usWinDescent
}

// check: a substitute - it must be TrueType, have the characters the game counts on and, if `sample` is given, that
// character of its language
// adjust: a last change to the font before it goes to the game (may reallocate it; NULL: none)
typedef uint8_t *(*FontAdjust)(uint8_t *d, size_t *len);
static int load_one_ex(const char *path, Font *ft, bool check, uint32_t sample, FontAdjust adjust);
static int load_one_checked(const char *path, Font *ft, bool check, uint32_t sample) {
    return load_one_ex(path, ft, check, sample, NULL);
}
static int load_one(const char *path, Font *ft) { return load_one_checked(path, ft, false, 0); }
static int load_one_ex(const char *path, Font *ft, bool check, uint32_t sample, FontAdjust adjust) {
    size_t len;
    uint8_t *d = read_file(path, &len);
    if (!d) return 0;
    size_t n = strlen(path);
    bool console_font = n > 6 && !strcasecmp(path + n - 6, ".bfttf");
    if (console_font) {
        len = bfttf_decode(d, len);
        if (!len) { LOG("[pl] %s: not a valid .bfttf\n", path); free(d); return 0; }
    } else if (len >= 4 && !memcmp(d, "ttcf", 4)) {
        size_t l2;
        uint8_t *f0 = ttc_face0(d, len, &l2);
        free(d);
        if (!f0) { LOG("[pl] %s: bad font collection\n", path); return 0; }
        d = f0;
        len = l2;
    }
    if (!console_font && !OS_FONTS_KNOWN) match_switch_metrics(d, len);  // (a .ttf of the console's own: already so)
    if (check && !font_truetype(d, len)) {
        LOG("[pl] %s: not TrueType (CFF outlines) - skipped\n", path);
        free(d);
        return 0;
    }
    if (check && !font_usable(d, len)) {
        LOG("[pl] %s: no glyph for a space or \"\u25A1\" (U+25A1) - skipped\n", path);
        free(d);
        return 0;
    }
    if (check && sample && !font_has(d, len, sample)) {
        LOG("[pl] %s: no U+%04X - skipped for this language\n", path, sample);
        free(d);
        return 0;
    }
    if (adjust && !(d = adjust(d, &len))) return 0;
    void *p = hle_alloc(len + 16);
    memcpy(p, d, len);
    free(d);
    ft->addr = (uint64_t)p;
    ft->size = len;
    LOG("[pl] system font %s (%zu bytes)\n", path, len);
    return 1;
}

#if !OS_FONTS_KNOWN && defined(__linux__)
// fontconfig's TrueType fonts for a language, best match first (`fc-match -s`, if the system has fontconfig): the first
// few that pass the checks. (Its sorted list goes on to fonts without the language: only the head is tried.)
static int load_fontconfig(const char *lang, Font *ft, uint32_t sample) {
    char cmd[200], line[1100];
    snprintf(cmd, sizeof cmd, "fc-match -s -f '%%{file}\\n' 'sans-serif:lang=%s:fontformat=TrueType' 2>/dev/null", lang);
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    int ok = 0;
    for (int tries = 0; !ok && tries < 6 && fgets(line, sizeof line, p); tries++) {
        line[strcspn(line, "\n")] = 0;
        if (line[0] == '/') ok = load_one_checked(line, ft, true, sample);
    }
    pclose(p);
    return ok;
}
#endif

// ---- Open Pad Glyphs: the in-text buttons in the controller's own labels, following the bindings
// The bundled button font (third_party/fonts/OpenPadGlyphs) has the Switch's letters on the console's codepoints and,
// for each controller family and controller button, the glyph of every character that names one Switch input at
// U+F0000 + family << 14 + button << 9 + (character - U+E000) (button: SDL_GamepadButton, triggers as PADB_*). Its
// cmap is rewritten so that each such character shows the button now bound to that input, in the family's labels.
enum { FAM_NINTENDO, FAM_XBOX, FAM_PS };
// the characters naming one Switch input (BIND_* slot): as ROLE_OF in third_party/fonts/OpenPadGlyphs/build_padglyphs.py
static const struct { uint16_t cp; uint8_t slot; } k_pad_chars[] = {
    {0xE000, BIND_A}, {0xE0A0, BIND_A}, {0xE0E0, BIND_A}, {0xE042, BIND_A}, {0xE04C, BIND_A}, {0xE0AB, BIND_A},
    {0xE001, BIND_B}, {0xE0A1, BIND_B}, {0xE0E1, BIND_B}, {0xE04D, BIND_B}, {0xE0AC, BIND_B},
    {0xE002, BIND_X}, {0xE0A2, BIND_X}, {0xE0E2, BIND_X}, {0xE04E, BIND_X}, {0xE0AD, BIND_X},
    {0xE003, BIND_Y}, {0xE0A3, BIND_Y}, {0xE0E3, BIND_Y}, {0xE04F, BIND_Y}, {0xE0AE, BIND_Y},
    {0xE004, BIND_L}, {0xE052, BIND_L}, {0xE083, BIND_L}, {0xE0A4, BIND_L}, {0xE0E4, BIND_L},
    {0xE005, BIND_R}, {0xE053, BIND_R}, {0xE084, BIND_R}, {0xE0A5, BIND_R}, {0xE0E5, BIND_R},
    {0xE054, BIND_ZL}, {0xE085, BIND_ZL}, {0xE0A6, BIND_ZL}, {0xE0E6, BIND_ZL},
    {0xE055, BIND_ZR}, {0xE086, BIND_ZR}, {0xE0A7, BIND_ZR}, {0xE0E7, BIND_ZR},
    {0xE045, BIND_PLUS}, {0xE0B3, BIND_PLUS}, {0xE0EF, BIND_PLUS}, {0xE0B5, BIND_PLUS}, {0xE0F1, BIND_PLUS},
    {0xE046, BIND_MINUS}, {0xE0B4, BIND_MINUS}, {0xE0F0, BIND_MINUS}, {0xE0B6, BIND_MINUS}, {0xE0F2, BIND_MINUS},
};

// The family whose labels the button glyphs use, once at start (the game reads its fonts once and keeps the glyphs it
// drew, so a controller changed later shows its labels from the next start). EOWRECOMP_BUTTON_GLYPHS=auto|nintendo|xbox|
// playstation; else Settings > Button icons when set; else EOWRECOMP_PAD_TYPE (tests); else the first controller SDL
// sees: by its type, and for one SDL can't type (adapters, generic pads) by name / USB id - e.g. the "Twin USB
// Joystick" PS2 adapter (0810:0001) is a PlayStation pad. Generic pads read like Xbox ones (A at the bottom).
static int pad_glyph_family(char *why, size_t n) {
    const char *e = getenv("EOWRECOMP_BUTTON_GLYPHS");
    if (e && *e && strcasecmp(e, "auto")) {
        snprintf(why, n, "EOWRECOMP_BUTTON_GLYPHS");
        return !strcasecmp(e, "xbox") ? FAM_XBOX : !strcasecmp(e, "playstation") || !strcasecmp(e, "ps") ? FAM_PS : FAM_NINTENDO;
    }
    if (g_settings.prompts != PROMPTS_AUTO) {
        snprintf(why, n, "Button icons setting");
        return g_settings.prompts == PROMPTS_XBOX ? FAM_XBOX : g_settings.prompts == PROMPTS_PLAYSTATION ? FAM_PS : FAM_NINTENDO;
    }
    if (getenv("EOWRECOMP_PAD_TYPE")) {
        snprintf(why, n, "EOWRECOMP_PAD_TYPE");
        extern int input_pad_family(void);
        return input_pad_family();
    }
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    int fam = FAM_NINTENDO;
    snprintf(why, n, "no controller");
    if (ids && count > 0) {
        SDL_JoystickID id = ids[0];
        const char *name = SDL_GetGamepadNameForID(id);
        unsigned vid = SDL_GetGamepadVendorForID(id), pid = SDL_GetGamepadProductForID(id);
        if (!name) name = "?";
        switch (SDL_GetGamepadTypeForID(id)) {
        case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: fam = FAM_PS; break;
        case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: fam = FAM_XBOX; break;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
            fam = FAM_NINTENDO; break;
        default:  // (SDL has no type for it: by name, then USB id)
            if (SDL_strcasestr(name, "playstation") || SDL_strcasestr(name, "dualshock") || SDL_strcasestr(name, "dualsense") ||
                SDL_strcasestr(name, "PS2") || SDL_strcasestr(name, "PS3") || SDL_strcasestr(name, "PS4") ||
                (vid == 0x0810 && pid == 0x0001))  // Twin USB Joystick / "Twin PlayStation Adapter"
                fam = FAM_PS;
            else if (SDL_strcasestr(name, "nintendo") || SDL_strcasestr(name, "switch") || SDL_strcasestr(name, "joy-con"))
                fam = FAM_NINTENDO;
            else fam = FAM_XBOX;
        }
        snprintf(why, n, "%s %04x:%04x", name, vid, pid);
    }
    SDL_free(ids);
    return fam;
}

// The font with its cmap replaced by one format 4 subtable (Unicode BMP: platform 0 / 3) for n ascending characters
static uint8_t *font_with_cmap(const uint8_t *f, size_t len, const uint16_t *cps, const uint16_t *gids, uint32_t n,
                               size_t *out_len) {
    uint32_t seg = n + 1, sub = 16 + 8 * seg, clen = 20 + sub;
    if (sub > 0xFFFF || len < 12) return NULL;
    uint8_t *c = calloc(1, clen);
    c[3] = 2;                                         // version 0, 2 encoding records
    c[1 + 4] = 3, c[7 + 4] = 20;                      // platform 0 encoding 3 -> offset 20
    c[1 + 12] = 3, c[3 + 12] = 1, c[7 + 12] = 20;     // platform 3 encoding 1 -> offset 20
    uint8_t *t = c + 20;
    unsigned sr = 1, es = 0;  // searchRange = 2 * the largest power of two <= seg, entrySelector = its log2
    for (; sr * 2 <= seg; sr *= 2) es++;
    sr *= 2;
#define W16(o, v) (t[(o)] = (uint8_t)((v) >> 8), t[(o) + 1] = (uint8_t)(v))
    W16(0, 4); W16(2, sub); W16(6, seg * 2); W16(8, sr); W16(10, es); W16(12, seg * 2 - sr);
    for (uint32_t i = 0; i < seg; i++) {
        uint32_t cp = i < n ? cps[i] : 0xFFFF, delta = i < n ? (uint32_t)(gids[i] - cps[i]) : 1;
        W16(14 + 2 * i, cp);                          // endCode
        W16(16 + 2 * seg + 2 * i, cp);                // startCode (after reservedPad)
        W16(16 + 4 * seg + 2 * i, delta & 0xFFFF);    // idDelta; idRangeOffset stays 0
    }
#undef W16
    uint32_t nt = (uint32_t)f[4] << 8 | f[5];
    if (12 + 16 * (size_t)nt > len) { free(c); return NULL; }
    size_t total = 12 + 16 * (size_t)nt;
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *e = f + 12 + 16 * i;
        uint32_t off = be32(e + 8), tl = be32(e + 12);
        if ((size_t)off + tl > len) { free(c); return NULL; }
        total += ((memcmp(e, "cmap", 4) ? tl : clen) + 3) & ~3u;
    }
    uint8_t *o = calloc(1, total);
    memcpy(o, f, 12 + 16 * (size_t)nt);
    size_t pos = 12 + 16 * (size_t)nt;
    for (uint32_t i = 0; i < nt; i++) {
        uint8_t *e = o + 12 + 16 * i;
        const uint8_t *src = f + be32(e + 8);
        uint32_t tl = be32(e + 12);
        if (!memcmp(e, "cmap", 4)) {
            src = c, tl = clen;
            uint32_t sum = 0;
            for (uint32_t k = 0; k < (clen + 3) / 4; k++) {
                uint8_t w[4] = {0};
                memcpy(w, c + 4 * k, clen - 4 * k < 4 ? clen - 4 * k : 4);
                sum += be32(w);
            }
            put32(e + 4, sum);
        }
        memcpy(o + pos, src, tl);
        put32(e + 8, (uint32_t)pos), put32(e + 12, tl);
        pos += (tl + 3) & ~3u;
    }
    free(c);
    *out_len = total;
    return o;
}

// FontAdjust for the bundled button font: each Switch input's characters -> the bound button in the family's labels
static uint8_t *pad_glyphs_adjust(uint8_t *d, size_t *len) {
    char why[160];
    int fam = pad_glyph_family(why, sizeof why);
    static const uint16_t keep[] = {0x0000, 0x000D, 0x0020, 0x25A0, 0x25A1, 0x3000};
    enum { NK = sizeof keep / sizeof *keep };
    uint16_t cps[NK + 0x200], gids[NK + 0x200];
    uint32_t n = 0, moved = 0;
    for (uint32_t i = 0; i < NK + 0x200; i++) {  // (the few other characters, then the button range, in order)
        uint32_t cp = i < NK ? keep[i] : 0xE000 + (i - NK);
        uint32_t g = font_gid(d, *len, cp);
        if (!g) continue;
        for (size_t i = 0; i < sizeof k_pad_chars / sizeof *k_pad_chars; i++) {
            if (k_pad_chars[i].cp != cp) continue;
            int host = bind_prompt_host(k_pad_chars[i].slot);
            uint32_t alt = host >= 0 && host < 32 ? font_gid(d, *len, 0xF0000 + ((uint32_t)fam << 14) + ((uint32_t)host << 9) + (cp - 0xE000)) : 0;
            if (alt) g = alt, moved++;
            break;
        }
        cps[n] = (uint16_t)cp, gids[n] = (uint16_t)g, n++;
    }
    size_t nl;
    uint8_t *o = font_with_cmap(d, *len, cps, gids, n, &nl);
    if (!o) { LOG("[pl] button glyphs: font not remapped (bad font)\n"); return d; }
    static const char *const fam_name[] = {"Nintendo", "Xbox", "PlayStation"};
    LOG("[pl] button glyphs: %s labels (%s), %u characters following the bindings\n", fam_name[fam], why, moved);
    free(d);
    *len = nl;
    return o;
}

static int g_fonts_loaded;
RT_ON_RESET(pl_reset) { g_fonts_loaded = 0; memset(fonts, 0, sizeof fonts); }  // (they were in the HLE heap)

static void load_fonts(void) {
    if (g_fonts_loaded) return;
    g_fonts_loaded = 1;
    const char *env = getenv("EOWRECOMP_FONT");
    // per type: the console's own file(s), then a substitute of that same language
    static const char *const std[] = {"fonts/nintendo_udsg-r_std_003.bfttf", "fonts/nintendo_udsg-r_std_003.ttf",
                                     "fonts/nintendo_udsgr_std_003.ttf", "fonts/nintendo_udsgr_std_003.bfttf",
                                     "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc", "C:/Windows/Fonts/YuGothR.ttc",
                                     "C:/Windows/Fonts/YuGothM.ttc",
                                     "/System/Library/Fonts/Supplemental/Arial.ttf", "C:/Windows/Fonts/arial.ttf",
                                     // Linux: DejaVu Sans (Debian / Ubuntu, Arch, Fedora). Not Noto Sans CJK: CFF
                                     // outlines, which the game's font engine crashes on (it wants TrueType)
                                     "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                     "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                     "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", NULL};
    static const char *const zh_cn[] = {"fonts/nintendo_udsg-r_org_zh-cn_003.bfttf",
                                        "fonts/nintendo_udsg-r_org_zh-cn_003.ttf",
                                        "/System/Library/Fonts/Hiragino Sans GB.ttc",
                                        "/System/Library/Fonts/Supplemental/Hiragino Sans GB.ttc",
                                        "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf",
                                        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
                                        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
                                        "/usr/share/fonts/wqy-zenhei/wqy-zenhei.ttc",       // (Arch, Fedora)
                                        "/usr/share/fonts/wenquanyi/wqy-zenhei/wqy-zenhei.ttc",
                                        // Droid Sans Fallback (Apache 2.0): Chinese, Japanese and Korean, TrueType
                                        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
                                        "/usr/share/fonts/google-droid-sans-fonts/DroidSansFallbackFull.ttf",
                                        "/usr/share/fonts/droid/DroidSansFallbackFull.ttf", NULL};
    static const char *const zh_cn_ext[] = {"fonts/nintendo_udsg-r_ext_zh-cn_003.bfttf",
                                            "fonts/nintendo_udsg-r_ext_zh-cn_003.ttf", NULL};
    static const char *const zh_tw[] = {"fonts/nintendo_udjxh-db_zh-tw_003.bfttf",
                                        "fonts/nintendo_udjxh-db_zh-tw_003.ttf", "C:/Windows/Fonts/msjh.ttc",
                                        // (PingFang carries both, but a collection copies only face 0 out: Simplified)
                                        "/System/Library/Fonts/PingFang.ttc",
                                        "/System/Library/Fonts/Supplemental/Songti.ttc",
                                        "/usr/share/fonts/truetype/arphic/uming.ttc",
                                        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
                                        "/usr/share/fonts/google-droid-sans-fonts/DroidSansFallbackFull.ttf",
                                        "/usr/share/fonts/droid/DroidSansFallbackFull.ttf", NULL};
    static const char *const ko[] = {"fonts/nintendo_udsg-r_ko_003.bfttf", "fonts/nintendo_udsg-r_ko_003.ttf",
                                     "/System/Library/Fonts/AppleSDGothicNeo.ttc", "C:/Windows/Fonts/malgun.ttf",
                                     "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
                                     "/usr/share/fonts/nanum/NanumGothic.ttf",
                                     "/usr/share/fonts/naver-nanum/NanumGothic.ttf",  // (Fedora)
                                     "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
                                     "/usr/share/fonts/google-droid-sans-fonts/DroidSansFallbackFull.ttf",
                                     "/usr/share/fonts/droid/DroidSansFallbackFull.ttf", NULL};
    static const char *const ext[] = {"fonts/nintendo_ext_003.bfttf", "fonts/nintendo_ext_003.ttf", NULL};
    static const char *const *const chains[N_FONT_TYPES] = {std, zh_cn, zh_cn_ext, zh_tw, ko, ext};
    // a character each substitute must have to serve that type (Standard: any, it's the fallback for all)
    static const uint32_t sample[N_FONT_TYPES] = {0, 0x4E2D /* 中 */, 0x4E2D, 0x570B /* 國 */, 0xD55C /* 한 */, 0xE0E0};
    static const char *const fc_lang[N_FONT_TYPES] = {NULL, "zh-cn", NULL, "zh-tw", "ko", NULL};
    extern const char *settings_language(void);
    bool cyrillic = !strcmp(settings_language(), "ru");
    char bundled[1100];
    for (int t = 0; t < N_FONT_TYPES; t++) {
        if (env) load_one(env, &fonts[t]);  // (a forced font: it is what the player asked for, in any language)
        for (size_t i = 0; !fonts[t].addr && chains[t][i]; i++) {
            const char *f = chains[t][i];
            bool own = strncmp(f, "fonts/", 6) == 0;  // (the console's own files: as they are)
            // Standard, before the OS's fonts where none is known: the bundled BIZ UDPGothic (a Japanese gothic
            // too: not for Russian)
            if (!OS_FONTS_KNOWN && t == F_STD && !own && !cyrillic && i && strncmp(chains[t][i - 1], "fonts/", 6) == 0 &&
                rt_resource("BIZUDPGothic-Regular.ttf", "third_party/fonts/BIZUDPGothic/BIZUDPGothic-Regular.ttf", bundled, sizeof bundled) &&
                load_one_checked(bundled, &fonts[t], true, 0))
                break;
            if (t == F_STD && cyrillic && (strstr(f, "/System/Library/Fonts/ヒラギノ") || strstr(f, "YuGoth"))) continue;
            load_one_checked(f, &fonts[t], !own, own ? 0 : sample[t]);
        }
#if !OS_FONTS_KNOWN && defined(__linux__)
        if (!fonts[t].addr && fc_lang[t]) load_fontconfig(fc_lang[t], &fonts[t], sample[t]);
#endif
    }
#if !OS_FONTS_KNOWN && defined(__linux__)
    if (!fonts[F_STD].addr) load_fontconfig(settings_language(), &fonts[F_STD], cyrillic ? 0x416 /* Ж */ : 'A');
#endif
    // NintendoExt without the console's: the bundled button font, in the controller's labels (pad_glyphs_adjust)
    if (!fonts[F_EXT].addr && rt_resource("OpenPadGlyphs.ttf", "third_party/fonts/OpenPadGlyphs/OpenPadGlyphs.ttf", bundled, sizeof bundled))
        load_one_ex(bundled, &fonts[F_EXT], true, sample[F_EXT], pad_glyphs_adjust);
    if (!fonts[F_STD].addr && rt_resource("DejaVuSans.ttf", "third_party/fonts/DejaVu/DejaVuSans.ttf", bundled, sizeof bundled))
        load_one_checked(bundled, &fonts[F_STD], true, 0);
    if (!fonts[F_STD].addr)  // (only a broken install: the app ships DejaVu Sans)
        FATAL("no font for the game's text: put the Switch fonts in fonts/, or reinstall (the app ships DejaVu Sans)\n");
}

// SharedFontType: 0 Standard, 1 ChineseSimple, 2 ChineseSimpleExt, 3 ChineseTraditional, 4 Korean, 5 NintendoExt
static const Font *font_for(uint64_t type) {
    load_fonts();
    static _Atomic uint32_t asked;
    if (type < 32 && !(atomic_fetch_or(&asked, 1u << type) & (1u << type)))
        LOG("[pl] the game uses shared font type %u%s\n", (unsigned)type,
            type < N_FONT_TYPES && fonts[type].addr ? "" : " (none loaded: it gets Standard)");
    // a type this OS gave us no font for (a Linux without a CJK font, say): Standard, as a missing dump does too
    if (type >= N_FONT_TYPES || !fonts[type].addr) return &fonts[F_STD];
    return &fonts[type];
}

HLE_FUNC("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", pl_get_load_state) { c->x[0] = 1; }
HLE_FUNC("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", pl_get_address) { c->x[0] = font_for(c->x[0])->addr; }
HLE_FUNC("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", pl_get_size) { c->x[0] = font_for(c->x[0])->size; }
