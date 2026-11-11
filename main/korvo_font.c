#include "korvo_font.h"
#include "esp_heap_caps.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_SYSTEM_H
#include FT_SYNTHESIS_H
#include <limits.h>
#include <string.h>

#define GLYPH_CACHE 32
#define GLYPH_MAX_SIDE 64

typedef struct {
    nj_u2 code;
    int width, height, advance;
    uint8_t *pixels;
} cached_glyph;

typedef struct {
    FT_Face face;
    nj_font font;
    cached_glyph cache[GLYPH_CACHE];
    unsigned next;
    int height, ascent;
} font_face;

struct korvo_font {
    struct FT_MemoryRec_ memory;
    FT_Library library;
    font_face faces[NJ_FONTSET_SLOTS];
    const nj_font *primary[NJ_FONTSET_SLOTS];
    const nj_font *fallback[NJ_FONTSET_SLOTS];
};

static void *font_alloc(FT_Memory memory, long size)
{
    (void)memory;
    return size > 0 ? heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
}
static void font_free(FT_Memory memory, void *ptr) { (void)memory; heap_caps_free(ptr); }
static void *font_realloc(FT_Memory memory, long old_size, long new_size, void *ptr)
{
    (void)memory; (void)old_size;
    if (new_size <= 0) { heap_caps_free(ptr); return NULL; }
    return heap_caps_realloc(ptr, (size_t)new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static cached_glyph *find_glyph(font_face *face, nj_u2 code)
{
    for (int i = 0; i < GLYPH_CACHE; ++i)
        if (face->cache[i].pixels && face->cache[i].code == code) return &face->cache[i];
    FT_UInt index = FT_Get_Char_Index(face->face, code);
    if (!index || FT_Load_Glyph(face->face, index, FT_LOAD_DEFAULT)) return NULL;
    if (face->font.style & NJ_FONT_STYLE_BOLD) FT_GlyphSlot_Embolden(face->face->glyph);
    if (FT_Render_Glyph(face->face->glyph, FT_RENDER_MODE_NORMAL)) return NULL;
    FT_GlyphSlot slot = face->face->glyph;
    FT_Bitmap *bitmap = &slot->bitmap;
    int advance = (int)((slot->advance.x + 32) / 64);
    int left = slot->bitmap_left > 0 ? slot->bitmap_left : 0;
    int width = (int)bitmap->width + left;
    if (width < advance) width = advance;
    if (advance < width) advance = width;
    if (width < 1) width = 1;
    if (width > GLYPH_MAX_SIDE || bitmap->rows > GLYPH_MAX_SIDE ||
        (bitmap->pixel_mode != FT_PIXEL_MODE_GRAY && bitmap->pixel_mode != FT_PIXEL_MODE_MONO)) return NULL;
    cached_glyph *entry = &face->cache[face->next++ % GLYPH_CACHE];
    uint8_t *pixels = heap_caps_realloc(entry->pixels, (size_t)width * face->height,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) return NULL;
    *entry = (cached_glyph){ .code = code, .width = width, .height = face->height,
        .advance = advance > 0 ? advance : width, .pixels = pixels };
    memset(pixels, 0, (size_t)width * face->height);
    for (unsigned y = 0; y < bitmap->rows; ++y) {
        int dy = face->ascent - slot->bitmap_top + (int)y;
        if (dy < 0 || dy >= face->height) continue;
        const uint8_t *row = bitmap->buffer + (bitmap->pitch >= 0 ? (int)y : (int)bitmap->rows - 1 - (int)y) *
            (bitmap->pitch >= 0 ? bitmap->pitch : -bitmap->pitch);
        for (unsigned x = 0; x < bitmap->width; ++x) {
            int dx = left + (int)x;
            if (dx < 0 || dx >= width) continue;
            pixels[dy * width + dx] = bitmap->pixel_mode == FT_PIXEL_MODE_GRAY ? row[x] :
                (row[x / 8] & (0x80u >> (x % 8)) ? 255 : 0);
        }
    }
    return entry;
}

static const nj_u1 *glyph_lookup(void *ud, nj_u2 code, int *width, int *height)
{
    cached_glyph *entry = find_glyph(ud, code);
    if (!entry) return NULL;
    *width = entry->width;
    *height = entry->height;
    return entry->pixels;
}

static int glyph_advance(void *ud, nj_u2 code)
{
    cached_glyph *entry = find_glyph(ud, code);
    return entry ? entry->advance : 0;
}

korvo_font *korvo_font_open(const void *data, nj_u4 size)
{
    if (!data || !size || size > 0x7fffffffu) return NULL;
    korvo_font *font = heap_caps_calloc(1, sizeof(*font), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!font) return NULL;
    font->memory = (struct FT_MemoryRec_){ .alloc = font_alloc, .free = font_free, .realloc = font_realloc };
    if (FT_New_Library(&font->memory, &font->library)) goto failed;
    FT_Add_Default_Modules(font->library);
    const int sizes[] = { NJ_FONT_SIZE_SMALL, NJ_FONT_SIZE_MEDIUM, NJ_FONT_SIZE_LARGE };
    for (int i = 0; i < NJ_FONTSET_SLOTS; ++i) {
        font_face *face = &font->faces[i];
        const nj_font *builtin = nj_font_builtin(sizes[i / NJ_FONT_SLOT_STYLES], i % NJ_FONT_SLOT_STYLES);
        if (FT_New_Memory_Face(font->library, data, (FT_Long)size, 0, &face->face)) goto failed;
        if (FT_Select_Charmap(face->face, FT_ENCODING_UNICODE)) goto failed;
        FT_Face ft = face->face;
        if (ft->ascender <= ft->descender) goto failed;
        /* CJK fonts reserve generous typographic line spacing. Scaling their
         * ascender/descender into the old Latin line box made SMALL Hanzi
         * only eight pixels tall. Size the em itself, then report a line box
         * that contains the font's actual outlines, including bold growth. */
        unsigned em = (unsigned)(builtin->height > 1 ? builtin->height - 1 : 1);
        if (FT_Set_Pixel_Sizes(ft, 0, em ? em : 1)) goto failed;
        FT_Pos top = FT_MulFix(ft->bbox.yMax, ft->size->metrics.y_scale);
        FT_Pos bottom = FT_MulFix(-ft->bbox.yMin, ft->size->metrics.y_scale);
        face->ascent = (int)((top > 0 ? top + 63 : 0) / 64) + 1;
        int descent = (int)((bottom > 0 ? bottom + 63 : 0) / 64);
        face->height = face->ascent + descent;
        if (face->height <= 0 || face->height > GLYPH_MAX_SIDE) goto failed;
        face->font = *builtin;
        face->font.name = "Espressif FreeType";
        face->font.height = face->height;
        face->font.ascent = face->ascent;
        face->font.descent = descent;
        face->font.leading = 0;
        face->font.glyph_h = face->height;
        face->font.glyph_w = (int)((em + 1) / 2);
        face->font.proportional = 1;
        face->font.bitmap = NULL;
        face->font.glyph_lookup = glyph_lookup;
        face->font.glyph_advance = glyph_advance;
        face->font.glyph_ud = face;
        face->font.coverage = 1;
        face->font.glyph_ascent = face->ascent;
        font->primary[i] = &face->font;
        font->fallback[i] = builtin;
    }
    return font;
failed:
    korvo_font_close(font);
    return NULL;
}

void korvo_font_use(korvo_font *font, nj_run_opts *opts)
{
    if (!font || !opts) return;
    opts->primary_fonts = font->primary;
    opts->primary_count = NJ_FONTSET_SLOTS;
    opts->fallback_fonts = font->fallback;
    opts->fallback_count = NJ_FONTSET_SLOTS;
}

void korvo_font_close(korvo_font *font)
{
    if (!font) return;
    for (int i = 0; i < NJ_FONTSET_SLOTS; ++i) {
        font_face *face = &font->faces[i];
        for (int j = 0; j < GLYPH_CACHE; ++j) heap_caps_free(face->cache[j].pixels);
        if (face->face) FT_Done_Face(face->face);
    }
    if (font->library) FT_Done_Library(font->library);
    heap_caps_free(font);
}
