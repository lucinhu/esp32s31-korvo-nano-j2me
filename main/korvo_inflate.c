#include "korvo_media.h"
#include "esp_heap_caps.h"
#include "zlib.h"
#include <stdint.h>

static voidpf zalloc_psram(voidpf ud, uInt count, uInt size)
{
    (void)ud;
    if (size && count > SIZE_MAX / size) return NULL;
    return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void zfree_psram(voidpf ud, voidpf ptr)
{
    (void)ud;
    heap_caps_free(ptr);
}

static nj_status inflate_stream(const nj_u1 *in, nj_u4 in_len, nj_u1 *out, nj_u4 out_len, int window)
{
    if (!in || (!out && out_len)) return NJ_ERR_FORMAT;
    uint8_t empty;
    z_stream stream = { .next_in = (Bytef *)in, .avail_in = in_len,
        .next_out = out_len ? out : &empty, .avail_out = out_len ? out_len : 1,
        .zalloc = zalloc_psram, .zfree = zfree_psram };
    int result = inflateInit2(&stream, window);
    if (result != Z_OK) return result == Z_MEM_ERROR ? NJ_ERR_NOMEM : NJ_ERR_FORMAT;
    result = inflate(&stream, Z_FINISH);
    int valid = result == Z_STREAM_END && stream.total_out == out_len && stream.total_in == in_len;
    inflateEnd(&stream);
    return valid ? NJ_OK : result == Z_MEM_ERROR ? NJ_ERR_NOMEM : NJ_ERR_FORMAT;
}

static nj_status inflate_zlib(void *ud, const nj_u1 *in, nj_u4 in_len, nj_u1 *out, nj_u4 out_len)
{
    (void)ud;
    return inflate_stream(in, in_len, out, out_len, MAX_WBITS);
}

static nj_status inflate_deflate(void *ud, const nj_u1 *in, nj_u4 in_len, nj_u1 *out, nj_u4 out_len)
{
    (void)ud;
    return inflate_stream(in, in_len, out, out_len, -MAX_WBITS);
}

void korvo_inflate_port(nj_port *port)
{
    port->inflate = inflate_zlib;
    port->inflate_raw = inflate_deflate;
}
