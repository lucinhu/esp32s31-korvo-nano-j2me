#include "nj2me/nj.h"
#include "nj2me/nj_codec.h"
#include "nj2me/nj_zip.h"
#include "korvo_media.h"
#include "korvo_font.h"
#include "zlib.h"
#include "esp_jpeg_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "png.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/* Run the actual PNG library and VM registry. ESP-only binary codecs are
 * replaced at their public SDK boundaries to exercise adapter contracts. */
static int allocations, fail_after = -1, handles;
_Atomic int sdk_allocations;
_Atomic size_t sdk_allocation_limit;
_Atomic int sdk_refused_allocations;
void test_midi(void);
static bool misalign;
static void *tracked_alloc(void *ud, nj_size size)
{
    (void)ud;
    if (fail_after == 0) return NULL;
    if (fail_after > 0) --fail_after;
    void *base = malloc((size_t)size + 32);
    if (!base) return NULL;
    void *p = (uint8_t *)base + (misalign ? 24 : 16);
    memcpy((uint8_t *)p - sizeof(base), &base, sizeof(base));
    ++allocations;
    return p;
}
static void tracked_free(void *ud, void *ptr)
{
    (void)ud;
    if (ptr) {
        void *base;
        memcpy(&base, (uint8_t *)ptr - sizeof(base), sizeof(base));
        --allocations; free(base);
    }
}
void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    size_t limit = atomic_load(&sdk_allocation_limit);
    if (limit && size > limit) {
        ++sdk_refused_allocations;
        return NULL;
    }
    void *ptr = malloc(size);
    if (ptr) ++sdk_allocations;
    return ptr;
}
void *heap_caps_realloc(void *ptr, size_t size, unsigned caps)
{
    (void)caps;
    void *result = realloc(ptr, size);
    if (!ptr && result) ++sdk_allocations;
    return result;
}
void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    void *ptr = heap_caps_malloc(count * size, caps);
    if (ptr) memset(ptr, 0, count * size);
    return ptr;
}
void heap_caps_free(void *ptr) { if (ptr) { --sdk_allocations; free(ptr); } }
static void release_image(nj_image *image)
{
    assert(image);
    tracked_free(NULL, image->pixels);
    tracked_free(NULL, image);
}

static jpeg_pixel_format_t jpeg_format;
static bool jpeg_reject;
jpeg_error_t jpeg_dec_open(jpeg_dec_config_t *cfg, jpeg_dec_handle_t *handle)
{
    jpeg_format = cfg->output_type;
    *handle = malloc(1);
    assert(*handle);
    ++handles;
    return JPEG_ERR_OK;
}
jpeg_error_t jpeg_dec_parse_header(jpeg_dec_handle_t handle, jpeg_dec_io_t *io, jpeg_dec_header_info_t *header)
{
    (void)handle; (void)io;
    *header = (jpeg_dec_header_info_t){ .width = 3, .height = 2 };
    return jpeg_reject ? JPEG_ERR_INVALID_PARAM : JPEG_ERR_OK;
}
jpeg_error_t jpeg_dec_get_outbuf_len(jpeg_dec_handle_t handle, int *len)
{
    (void)handle;
    *len = jpeg_format == JPEG_PIXEL_FORMAT_RGB565_LE ? 12 : 18;
    return JPEG_ERR_OK;
}
jpeg_error_t jpeg_dec_process(jpeg_dec_handle_t handle, jpeg_dec_io_t *io)
{
    (void)handle;
    assert(((uintptr_t)io->outbuf & 15) == 0);
    for (int i = 0; i < 6; ++i) {
        if (jpeg_format == JPEG_PIXEL_FORMAT_RGB565_LE) {
            io->outbuf[i * 2] = 0x00; io->outbuf[i * 2 + 1] = 0xf8;
        } else {
            io->outbuf[i * 3] = 255; io->outbuf[i * 3 + 1] = 0; io->outbuf[i * 3 + 2] = 0;
        }
    }
    return JPEG_ERR_OK;
}
jpeg_error_t jpeg_dec_close(jpeg_dec_handle_t handle) { free(handle); --handles; return JPEG_ERR_OK; }

static int audio_mode, audio_calls, resize_count, eos_count;
typedef struct { int frames; } fake_audio;
esp_audio_err_t esp_audio_dec_register_default(void) { return ESP_AUDIO_ERR_OK; }
esp_audio_err_t esp_audio_simple_dec_register_default(void) { return ESP_AUDIO_ERR_OK; }
void esp_audio_dec_unregister_default(void) {}
void esp_audio_simple_dec_unregister_default(void) {}
esp_audio_err_t esp_audio_simple_dec_open(esp_audio_simple_dec_cfg_t *cfg, esp_audio_simple_dec_handle_t *handle)
{
    assert(cfg->dec_type != ESP_AUDIO_SIMPLE_DEC_TYPE_NONE);
    *handle = calloc(1, sizeof(fake_audio));
    assert(*handle);
    ++handles;
    return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_audio_simple_dec_process(esp_audio_simple_dec_handle_t handle,
    esp_audio_simple_dec_raw_t *raw, esp_audio_simple_dec_out_t *out)
{
    fake_audio *state = handle;
    ++audio_calls;
    unsigned needed = audio_mode == 5 ? 65536 : 8192;
    if (out->len < needed) {
        out->needed_size = needed;
        ++resize_count;
        return ESP_AUDIO_ERR_BUFF_NOT_ENOUGH;
    }
    if (audio_mode == 1) { raw->consumed = raw->len + 1; return ESP_AUDIO_ERR_OK; }
    if (audio_mode == 2) return ESP_AUDIO_ERR_OK;
    if (audio_mode == 6) { out->needed_size = 65537; return ESP_AUDIO_ERR_BUFF_NOT_ENOUGH; }
    raw->consumed = audio_mode == 5 ? 1 : raw->len;
    out->decoded_size = audio_mode == 5 ? 65536 : 16;
    memset(out->buffer, 0x34 + state->frames, out->decoded_size);
    ++state->frames;
    if (raw->eos) ++eos_count;
    return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_audio_simple_dec_get_info(esp_audio_simple_dec_handle_t handle, esp_audio_simple_dec_info_t *info)
{
    fake_audio *state = handle;
    *info = (esp_audio_simple_dec_info_t){ .sample_rate = 22050, .channel = 2, .bits_per_sample = 16 };
    if (audio_mode == 3) info->bits_per_sample = 24;
    if (audio_mode == 4 && state->frames > 1) info->sample_rate = 44100;
    return ESP_AUDIO_ERR_OK;
}
void esp_audio_simple_dec_close(esp_audio_simple_dec_handle_t handle)
{
    free(handle); --handles;
}

typedef struct { uint8_t bytes[4096]; size_t size; } png_buffer;
static void write_png(png_structp png, png_bytep data, png_size_t size)
{
    png_buffer *buf = png_get_io_ptr(png);
    assert(buf->size + size <= sizeof(buf->bytes));
    memcpy(buf->bytes + buf->size, data, size);
    buf->size += size;
}
static void flush_png(png_structp png) { (void)png; }
static png_buffer make_png(int color, int depth, int interlace)
{
    png_buffer buffer = {0};
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    assert(png && info);
    assert(!setjmp(png_jmpbuf(png)));
    png_set_write_fn(png, &buffer, write_png, flush_png);
    png_set_IHDR(png, info, 3, 2, depth, color, interlace, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_color palette[] = {{255,0,0}, {0,255,0}, {0,0,255}, {255,255,255}};
    png_byte alpha[] = {255, 128, 0, 255};
    if (color == PNG_COLOR_TYPE_PALETTE) {
        png_set_PLTE(png, info, palette, 4);
        png_set_tRNS(png, info, alpha, 4, NULL);
    }
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_RGB) {
        png_color_16 transparent = { .gray = 0, .red = 0, .green = 0, .blue = 0 };
        png_set_tRNS(png, info, NULL, 0, &transparent);
    }
    png_write_info(png, info);
    int channels = color == PNG_COLOR_TYPE_RGBA ? 4 : color == PNG_COLOR_TYPE_RGB ? 3 :
        color == PNG_COLOR_TYPE_GRAY_ALPHA ? 2 : 1;
    uint8_t pixels[2][24] = {{0}};
    int mask = (1 << depth) - 1;
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 3; ++x) {
        unsigned value = (x + y) & (color == PNG_COLOR_TYPE_PALETTE ? 3 : mask);
        if (depth < 8) pixels[y][x * depth / 8] |= value << (8 - depth - x * depth % 8);
        else for (int c = 0; c < channels; ++c) {
            uint8_t v = color == PNG_COLOR_TYPE_PALETTE ? value : (x * 73 + y * 41 + c * 29) & 255;
            pixels[y][(x * channels + c) * (depth / 8)] = v;
            if (depth == 16) pixels[y][(x * channels + c) * 2 + 1] = 255 - v;
        }
    }
    png_bytep rows[] = {pixels[0], pixels[1]};
    png_write_image(png, rows);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    return buffer;
}

static void test_png(nj_vm *vm)
{
    const int colors[] = {PNG_COLOR_TYPE_GRAY, PNG_COLOR_TYPE_RGB, PNG_COLOR_TYPE_PALETTE,
        PNG_COLOR_TYPE_GRAY_ALPHA, PNG_COLOR_TYPE_RGBA};
    const int depths[] = {1,2,4,8,16};
    int cases = 0;
    for (int c = 0; c < 5; ++c) for (int d = 0; d < 5; ++d) for (int interlace = 0; interlace < 2; ++interlace) {
        int color = colors[c], depth = depths[d];
        if ((color != PNG_COLOR_TYPE_GRAY && color != PNG_COLOR_TYPE_PALETTE && depth < 8) ||
            (color == PNG_COLOR_TYPE_PALETTE && (depth == 1 || depth == 16))) continue;
        png_buffer buffer = make_png(color, depth, interlace);
        const char *error = NULL;
        nj_codec_req req = { .data = buffer.bytes, .size = buffer.size, .want_alpha = 1,
            .out_format = NJ_CODEC_OUT_ANY, .alloc = tracked_alloc, .dealloc = tracked_free, .err = &error };
        const nj_codec *provider = nj_codec_find(vm, req.data, req.size, NULL);
        assert(provider && !strcmp(provider->name, "espressif-libpng"));
        nj_image *actual = nj_image_decode(vm, &req, &error);
        nj_image *reference = nj_codec_png_software.decode(&req);
        assert(actual && reference && actual->format == NJ_PIXFMT_ARGB8888);
        assert(actual->width == 3 && actual->height == 2);
        assert(!memcmp(actual->pixels, reference->pixels, 24));
        release_image(actual); release_image(reference);
        /* Game-style stale CRCs still decode, including palette/tRNS. */
        buffer.bytes[29] ^= 1;
        actual = nj_image_decode(vm, &req, &error);
        assert(actual); release_image(actual);
        buffer.bytes[29] ^= 1;
        for (nj_u4 length = 8; length < buffer.size; ++length) {
            req.size = length;
            actual = nj_image_decode(vm, &req, &error);
            if (actual) release_image(actual);
            assert(allocations == 0);
        }
        req.size = buffer.size;
        for (int fail = 0; fail < 25; ++fail) {
            fail_after = fail;
            actual = nj_image_decode(vm, &req, &error);
            if (actual) release_image(actual);
            assert(allocations == 0);
        }
        fail_after = -1;
        ++cases;
    }
    printf("PNG: %d color/depth/interlace cases, exact pixels, stale CRC, truncation, allocation failures\n", cases);
}

static void test_jpeg(nj_vm *vm)
{
    const uint8_t input[] = {0xff,0xd8,0xff,0xe0};
    const int formats[] = {NJ_PIXFMT_RGB565, NJ_PIXFMT_RGB888, NJ_PIXFMT_ARGB8888};
    for (int f = 0; f < 7; ++f) {
        int format = formats[f % 3];
        misalign = f >= 3;
        const char *error = NULL;
        nj_codec_req req = { .data = input, .size = sizeof(input), .out_format = format,
            .alloc = tracked_alloc, .dealloc = tracked_free, .err = &error };
        if (f == 6) { req.out_format = NJ_CODEC_OUT_ANY; format = NJ_PIXFMT_ARGB8888; }
        const nj_codec *codec = nj_codec_find(vm, input, sizeof(input), NULL);
        assert(codec && !strcmp(codec->name, "espressif-esp-new-jpeg"));
        nj_image *image = nj_image_decode(vm, &req, &error);
        assert(image && image->format == format);
        if (format == NJ_PIXFMT_RGB565) assert(((uint16_t *)image->pixels)[5] == 0xf800);
        if (format == NJ_PIXFMT_RGB888) assert(((uint8_t *)image->pixels)[15] == 255);
        if (format == NJ_PIXFMT_ARGB8888) assert(image->pixels[5] == 0xffff0000);
        release_image(image);
        jpeg_reject = true;
        assert(!nj_image_decode(vm, &req, &error));
        jpeg_reject = false;
        assert(allocations == 0 && handles == 0);
    }
    misalign = false;
    puts("JPEG adapter: registry priority, aligned buffers, RGB565/RGB888/ARGB8888, failure cleanup");
}

static void test_audio(nj_port *port)
{
    uint8_t input[8200] = {'I','D','3'};
    nj_u4 length, rate, channels;
    assert(port->audio_decode(NULL, input, sizeof(input), NULL, 0, &length, &rate, &channels) == NJ_OK);
    assert(length == 48 && rate == 22050 && channels == 2);
    uint8_t output[48];
    assert(port->audio_decode(NULL, input, sizeof(input), output, sizeof(output), &length, &rate, &channels) == NJ_OK);
    assert(output[0] == 0x34 && output[16] == 0x35 && output[32] == 0x36);
    assert(eos_count == 2 && resize_count == 2);
    assert(port->audio_decode(NULL, input, sizeof(input), output, 47, &length, &rate, &channels) != NJ_OK);
    for (audio_mode = 1; audio_mode <= 6; ++audio_mode) {
        assert(port->audio_decode(NULL, input, sizeof(input), NULL, 0, &length, &rate, &channels) != NJ_OK);
        assert(length == 0 && rate == 0 && channels == 0);
        assert(handles == 0 && sdk_allocations == 0);
    }
    audio_mode = 0;
    input[0] = 0;
    int calls = audio_calls;
    assert(port->audio_decode(NULL, input, sizeof(input), NULL, 0, &length, &rate, &channels) == NJ_ERR_UNSUPPORTED);
    assert(calls == audio_calls);
    puts("Audio adapter: sizing/write agreement, resize retry, eos, short output, invalid data/format, PCM limit, cleanup");
}

static void test_inflate(nj_port *port)
{
    const uint8_t input[] = "A JAR class or M3G compressed section. A JAR class or M3G compressed section.";
    uint8_t compressed[256], output[sizeof(input)];
    for (int raw = 0; raw < 2; ++raw) {
        z_stream stream = { .next_in = (Bytef *)input, .avail_in = sizeof(input),
            .next_out = compressed, .avail_out = sizeof(compressed) };
        assert(deflateInit2(&stream, 6, Z_DEFLATED, raw ? -MAX_WBITS : MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
        assert(deflate(&stream, Z_FINISH) == Z_STREAM_END);
        nj_u4 size = stream.total_out;
        deflateEnd(&stream);
        nj_status (*decode)(void *, const nj_u1 *, nj_u4, nj_u1 *, nj_u4) = raw ? port->inflate_raw : port->inflate;
        assert(decode(NULL, compressed, size, output, sizeof(output)) == NJ_OK);
        assert(!memcmp(input, output, sizeof(input)));
        assert(decode(NULL, compressed, size - 1, output, sizeof(output)) != NJ_OK);
        assert(decode(NULL, compressed, size, output, sizeof(output) - 1) != NJ_OK);
        compressed[size] = 0;
        assert(decode(NULL, compressed, size + 1, output, sizeof(output)) != NJ_OK);
        assert(sdk_allocations == 0);
    }
    const uint8_t empty_raw[] = {3, 0};
    assert(port->inflate_raw(NULL, empty_raw, 2, NULL, 0) == NJ_OK);
    puts("zlib: raw JAR and wrapped M3G streams, exact lengths, truncation, empty entries, cleanup");
}

static void test_font(void)
{
    FILE *file = fopen(KORVO_TEST_FONT, "rb");
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length > 0 && !fseek(file, 0, SEEK_SET));
    uint8_t *data = malloc(length);
    assert(data && fread(data, 1, length, file) == (size_t)length);
    fclose(file);
    korvo_font *font = korvo_font_open(data, length);
    assert(font);
    nj_run_opts opts = {0};
    korvo_font_use(font, &opts);
    assert(opts.primary_count == NJ_FONTSET_SLOTS && opts.fallback_count == NJ_FONTSET_SLOTS);
    for (int i = 0; i < NJ_FONTSET_SLOTS; ++i) {
        const nj_font *face = opts.primary_fonts[i];
        assert(face && face->coverage == 1 && face->height == opts.fallback_fonts[i]->height);
        const nj_u2 characters[] = {'A', 'g', ' ', 0x4e2d, 0x6587};
        for (int c = 0; c < 5; ++c) {
            int w, h;
            const nj_u1 *pixels = face->glyph_lookup(face->glyph_ud, characters[c], &w, &h);
            assert(pixels && w > 0 && h > 0 && face->glyph_advance(face->glyph_ud, characters[c]) > 0);
            unsigned ink = 0;
            for (int p = 0; p < w * h; ++p) ink += pixels[p];
            assert(c == 2 ? ink == 0 : ink > 0);
        }
        for (int c = 33; c < 127; ++c) {
            int w, h;
            assert(face->glyph_lookup(face->glyph_ud, c, &w, &h));
        }
        int w, h;
        assert(!face->glyph_lookup(face->glyph_ud, 0xffff, &w, &h));
    }
    korvo_font_close(font);
    assert(!korvo_font_open(data, 4));
    assert(sdk_allocations == 0);
    free(data);
    puts("FreeType: embedded Chinese/Latin font, six slots, metrics, spaces, cache eviction, malformed font, cleanup");
}

static void test_jar(nj_port *port)
{
    FILE *file = fopen(KORVO_TEST_JAR, "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length > 0 && !fseek(file, 0, SEEK_SET));
    uint8_t *data = malloc(length);
    assert(data && fread(data, 1, length, file) == (size_t)length);
    fclose(file);
    nj_zip *zip = nj_zip_open(data, length, NULL);
    assert(zip);
    nj_zip_set_inflate(zip, port->inflate_raw, port->ud);
    int entries = nj_zip_count(zip), deflated = 0;
    assert(entries > 0);
    for (int i = 0; i < entries; ++i) {
        const nj_zip_entry *entry = nj_zip_entry_at(zip, i);
        uint8_t *out = malloc(entry->uncomp_size + 1u);
        assert(out && nj_zip_extract(zip, entry, out, entry->uncomp_size) == (nj_s4)entry->uncomp_size);
        if (entry->method == 8) ++deflated;
        free(out);
    }
    assert(deflated > 0 && sdk_allocations == 0);
    nj_zip_close(zip);
    free(data);
    printf("JAR: extracted %d demo entries through platform zlib (%d deflated)\n", entries, deflated);
}

int main(void)
{
    nj_port port;
    assert(nj_port_headless(&port) == NJ_OK);
    korvo_media_port(&port);
    nj_vm *vm = nj_vm_create(&port, 1024 * 1024);
    assert(vm);
    test_png(vm);
    test_jpeg(vm);
    test_audio(&port);
    test_inflate(&port);
    test_jar(&port);
    test_font();
    test_midi();
    nj_vm_destroy(vm);
    if (port.shutdown) port.shutdown(port.ud);
    assert(allocations == 0 && sdk_allocations == 0 && handles == 0);
    puts("Media adapter tests passed");
}
