#include "korvo_media.h"
#include "nj2me/nj_codec.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "png.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>

#define IMAGE_MAX_PIXELS (4u * 1024u * 1024u)

static void image_free(const nj_codec_req *req, nj_image *image)
{
    if (image) {
        req->dealloc(req->ud, image->pixels);
        req->dealloc(req->ud, image);
    }
}

static nj_image *image_error(const nj_codec_req *req, const char *message)
{
    if (req->err) *req->err = message;
    return NULL;
}

typedef struct {
    const nj_codec_req *req;
    nj_u4 offset;
    nj_image *image;
} png_context;

static png_voidp png_alloc(png_structp png, png_alloc_size_t size)
{
    const nj_codec_req *req = png_get_mem_ptr(png);
    return size <= UINT32_MAX ? req->alloc(req->ud, (nj_size)size) : NULL;
}

static void png_free_mem(png_structp png, png_voidp ptr)
{
    const nj_codec_req *req = png_get_mem_ptr(png);
    req->dealloc(req->ud, ptr);
}

static void png_read_mem(png_structp png, png_bytep dest, png_size_t size)
{
    png_context *ctx = png_get_io_ptr(png);
    if (size > ctx->req->size - ctx->offset) png_error(png, "truncated PNG");
    memcpy(dest, (const uint8_t *)ctx->req->data + ctx->offset, size);
    ctx->offset += size;
}

static void png_fail(png_structp png, png_const_charp message)
{
    (void)message;
    png_longjmp(png, 1);
}

static void png_warning_ignore(png_structp png, png_const_charp message)
{
    (void)png;
    (void)message;
}

static int png_accepts(const void *data, nj_u4 size)
{
    return data && size >= 8 && !png_sig_cmp(data, 0, 8);
}

static nj_image *png_decode(const nj_codec_req *req)
{
    if (req->out_format != NJ_CODEC_OUT_ANY && req->out_format != NJ_PIXFMT_ARGB8888)
        return image_error(req, "libpng requires ARGB8888 output");
    /* Keep mutable cleanup state off the stack across libpng's longjmp. */
    png_context *ctx = req->alloc(req->ud, sizeof(*ctx));
    if (!ctx) return image_error(req, "out of memory decoding PNG");
    *ctx = (png_context){ .req = req };
    png_structp png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, NULL,
        png_fail, png_warning_ignore, (void *)req, png_alloc, png_free_mem);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info) {
        png_destroy_read_struct(&png, &info, NULL);
        req->dealloc(req->ud, ctx);
        return image_error(req, "out of memory creating PNG decoder");
    }
    if (setjmp(png_jmpbuf(png))) {
        image_free(req, ctx->image);
        png_destroy_read_struct(&png, &info, NULL);
        req->dealloc(req->ud, ctx);
        return image_error(req, "invalid PNG or insufficient decoder memory");
    }
    png_set_read_fn(png, ctx, png_read_mem);
    png_set_user_limits(png, 65535, 65535);
    png_set_chunk_malloc_max(png, 256 * 1024);
    png_set_chunk_cache_max(png, 32);
    /* Some games rewrite palette/tRNS bytes without updating chunk CRCs.
     * Match nano_j2me's default policy while retaining libpng bounds checks. */
    png_set_crc_action(png, PNG_CRC_QUIET_USE, PNG_CRC_QUIET_USE);
    png_read_info(png, info);
    png_uint_32 width = png_get_image_width(png, info);
    png_uint_32 height = png_get_image_height(png, info);
    if (!width || !height || (uint64_t)width * height > IMAGE_MAX_PIXELS)
        png_error(png, "PNG dimensions exceed board limit");
    int depth = png_get_bit_depth(png, info);
    int color = png_get_color_type(png, info);
    int alpha = (color & PNG_COLOR_MASK_ALPHA) || png_get_valid(png, info, PNG_INFO_tRNS);
    if (depth == 16) png_set_strip_16(png);
    if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if (!alpha) png_set_add_alpha(png, 255, PNG_FILLER_AFTER);
    /* ESP32-S31 is little endian: BGRA bytes form 0xAARRGGBB words. */
    png_set_bgr(png);
    int passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);
    if (png_get_rowbytes(png, info) != width * 4u)
        png_error(png, "unexpected PNG row layout");
    ctx->image = nj_codec_image_new(req, width, height, NJ_PIXFMT_ARGB8888, alpha);
    if (!ctx->image) png_error(png, "out of memory for PNG pixels");
    for (int pass = 0; pass < passes; ++pass)
        for (png_uint_32 y = 0; y < height; ++y)
            png_read_row(png, (png_bytep)(ctx->image->pixels + y * width), NULL);
    png_read_end(png, NULL);
    nj_image *image = ctx->image;
    png_destroy_read_struct(&png, &info, NULL);
    req->dealloc(req->ud, ctx);
    return image;
}

static nj_status png_probe(const void *data, nj_u4 size, nj_image_info *out)
{
    const uint8_t *p = data;
    if (!out || size < 33 || !png_accepts(data, size) || memcmp(p + 8, "\0\0\0\rIHDR", 8))
        return NJ_ERR_FORMAT;
    uint32_t w = (uint32_t)p[16] << 24 | (uint32_t)p[17] << 16 | (uint32_t)p[18] << 8 | p[19];
    uint32_t h = (uint32_t)p[20] << 24 | (uint32_t)p[21] << 16 | (uint32_t)p[22] << 8 | p[23];
    if (!w || !h || w > INT_MAX || h > INT_MAX) return NJ_ERR_FORMAT;
    *out = (nj_image_info){ .width = (int)w, .height = (int)h,
        .has_alpha = p[25] == PNG_COLOR_TYPE_GRAY_ALPHA || p[25] == PNG_COLOR_TYPE_RGBA,
        .bits_per_pixel = p[24], .format = NJ_CODEC_FMT_PNG };
    return NJ_OK;
}

static nj_image *jpeg_decode(const nj_codec_req *req)
{
    /* MIDP Image adopts width*height Java int pixels without converting
     * nj_image.format. ANY must therefore return ARGB8888, independently of
     * the RGB565 display format. Explicit native callers can still ask for 565. */
    int format = req->out_format == NJ_CODEC_OUT_ANY ? NJ_PIXFMT_ARGB8888 : (int)req->out_format;
    if (format != NJ_PIXFMT_RGB565 && format != NJ_PIXFMT_RGB888 && format != NJ_PIXFMT_ARGB8888)
        return image_error(req, "unsupported JPEG output format");
    if (req->size > INT_MAX) return image_error(req, "JPEG input too large");
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = format == NJ_PIXFMT_RGB565 ? JPEG_PIXEL_FORMAT_RGB565_LE : JPEG_PIXEL_FORMAT_RGB888;
    jpeg_dec_handle_t decoder = NULL;
    jpeg_dec_header_info_t header = {0};
    jpeg_dec_io_t io = { .inbuf = (uint8_t *)req->data, .inbuf_len = (int)req->size };
    nj_image *image = NULL;
    void *scratch = NULL;
    int bytes = 0;
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK) goto done;
    if (jpeg_dec_parse_header(decoder, &io, &header) != JPEG_ERR_OK) goto done;
    if (!header.width || !header.height || (uint64_t)header.width * header.height > IMAGE_MAX_PIXELS) {
        jpeg_dec_close(decoder);
        return image_error(req, "JPEG dimensions exceed board limit");
    }
    if (jpeg_dec_get_outbuf_len(decoder, &bytes) != JPEG_ERR_OK || bytes <= 0) goto done;
    size_t packed = (size_t)header.width * header.height * (format == NJ_PIXFMT_RGB565 ? 2u : 3u);
    if ((size_t)bytes < packed || (size_t)bytes > IMAGE_MAX_PIXELS * 4u) goto done;
    image = nj_codec_image_new(req, header.width, header.height, format, 0);
    if (!image) goto done;
    /* Most VM allocations are aligned already. Other callers may supply
     * weaker alignment; never pass those directly to esp_new_jpeg. */
    if (format == NJ_PIXFMT_ARGB8888 || (uintptr_t)image->pixels % 16 || (size_t)bytes != packed) {
        scratch = req->alloc(req->ud, (nj_size)bytes + 15u);
        if (!scratch) goto failed;
        io.outbuf = (uint8_t *)(((uintptr_t)scratch + 15u) & ~(uintptr_t)15u);
    } else io.outbuf = (uint8_t *)image->pixels;
    if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK) goto failed;
    if (format == NJ_PIXFMT_ARGB8888) {
        for (size_t i = 0; i < (size_t)header.width * header.height; ++i) {
            const uint8_t *rgb = io.outbuf + i * 3u;
            image->pixels[i] = 0xff000000u | (uint32_t)rgb[0] << 16 | (uint32_t)rgb[1] << 8 | rgb[2];
        }
    } else if (scratch) memcpy(image->pixels, io.outbuf, packed);
    goto done;
failed:
    image_free(req, image);
    image = NULL;
done:
    if (scratch) req->dealloc(req->ud, scratch);
    if (decoder) jpeg_dec_close(decoder);
    /* The upstream decoder also handles CMYK and unusual sampling modes.
     * The registry itself does not retry lower-priority providers. */
    if (!image) image = nj_jpeg_decode(req);
    return image;
}

static const nj_codec png_codec = {
    .name = "espressif-libpng",
    .caps = { .formats = NJ_CODEC_FMT_PNG, .outputs = NJ_CODEC_CAN_ARGB8888,
        .scales = NJ_CODEC_SCALE_1, .max_pixels = IMAGE_MAX_PIXELS, .progressive = 1 },
    .accepts = png_accepts, .info = png_probe, .decode = png_decode,
};

static const nj_codec jpeg_codec = {
    .name = "espressif-esp-new-jpeg",
    .caps = { .formats = NJ_CODEC_FMT_JPEG,
        .outputs = NJ_CODEC_CAN_RGB565 | NJ_CODEC_CAN_RGB888 | NJ_CODEC_CAN_ARGB8888,
        .scales = NJ_CODEC_SCALE_1, .max_pixels = IMAGE_MAX_PIXELS },
    .accepts = nj_jpeg_accepts, .info = nj_jpeg_info, .decode = jpeg_decode,
};

void korvo_image_install(void *ud, nj_vm *vm)
{
    (void)ud;
    if (nj_codec_register(vm, &png_codec, NJ_CODEC_PRIO_PLATFORM) != NJ_OK ||
        nj_codec_register(vm, &jpeg_codec, NJ_CODEC_PRIO_PLATFORM) != NJ_OK)
        ESP_LOGE("j2me-media", "Failed to register platform image decoders");
}
