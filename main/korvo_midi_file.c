#include "korvo_midi_file.h"
#include "esp_heap_caps.h"
#include <stdbool.h>
#include <string.h>

#define MAX_INPUT (512u * 1024u)
#define MAX_OUTPUT (2u * MAX_INPUT)
#define MAX_TRACKS 32u
#define MAX_DELTA 0x0fffffffu

typedef struct {
    uint32_t offset, end, body, length;
    uint64_t tick;
    unsigned running, status;
    bool ready;
} track_cursor;

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int vlq(const uint8_t *data, uint32_t end, uint32_t *offset, uint32_t *value)
{
    *value = 0;
    for (unsigned i = 0; i < 4 && *offset < end; ++i) {
        unsigned b = data[(*offset)++];
        *value = (*value << 7) | (b & 127u);
        if (!(b & 128u)) return 1;
    }
    return 0;
}

/* Read one bounded event. Running status belongs to its source track. */
static int next_event(const uint8_t *data, track_cursor *track)
{
    uint32_t offset = track->offset, delta, count;
    track->ready = false;
    if (offset == track->end) return 1;
    if (!vlq(data, track->end, &offset, &delta) || offset >= track->end) return 0;
    track->tick += delta;
    unsigned status = data[offset];
    if (status & 128u) {
        ++offset;
        if (status < 240u) track->running = status;
    } else status = track->running;
    track->body = offset;
    track->status = status;
    if (status >= 128u && status < 240u) {
        count = (status & 224u) == 192u ? 1u : 2u;
        if (count > track->end - offset) return 0;
        while (count--) if (data[offset++] & 128u) return 0;
    } else if (status == 255u || status == 240u || status == 247u) {
        if (status == 255u) {
            if (offset >= track->end) return 0;
            ++offset;
        } else track->running = 0;
        if (!vlq(data, track->end, &offset, &count) || count > track->end - offset) return 0;
        if (status == 255u && data[track->body] == 47u && count != 0) return 0;
        offset += count;
    } else return 0;
    track->offset = offset;
    track->length = offset - track->body;
    track->ready = true;
    return 1;
}

static uint32_t write_delta(uint8_t *output, uint64_t delta)
{
    if (delta > MAX_DELTA) return 0;
    unsigned bytes = 1;
    for (uint64_t value = delta; value >>= 7;) ++bytes;
    if (output) for (unsigned i = 0; i < bytes; ++i)
        output[i] = ((delta >> (7u * (bytes - 1u - i))) & 127u) | (i + 1u < bytes ? 128u : 0u);
    return bytes;
}

/* Two passes give an exact allocation without storing an event list. Stable
 * ordering by tick, then source track, preserves same-tick controller changes. */
static uint32_t merge(const uint8_t *data, const track_cursor *source,
                      unsigned count, uint8_t *output)
{
    track_cursor tracks[MAX_TRACKS];
    memcpy(tracks, source, count * sizeof(*tracks));
    uint32_t size = 22;
    uint64_t previous = 0, finish = 0;
    for (unsigned i = 0; i < count; ++i) if (!next_event(data, &tracks[i])) return 0;
    while (true) {
        unsigned selected = count;
        for (unsigned i = 0; i < count; ++i) {
            if (tracks[i].tick > finish) finish = tracks[i].tick;
            if (tracks[i].ready && (selected == count || tracks[i].tick < tracks[selected].tick)) selected = i;
        }
        if (selected == count) break;
        track_cursor *track = &tracks[selected];
        /* Source EOTs terminate only their own tracks, never the merged song. */
        if (track->status == 255u && data[track->body] == 47u) {
            if (track->offset != track->end) return 0;
            track->ready = false;
            continue;
        }
        uint32_t delta_size = write_delta(NULL, track->tick - previous);
        if (!delta_size || size > MAX_OUTPUT - delta_size - 1u ||
            track->length > MAX_OUTPUT - size - delta_size - 1u) return 0;
        if (output) {
            write_delta(output + size, track->tick - previous);
            output[size + delta_size] = track->status;
            memcpy(output + size + delta_size + 1u, data + track->body, track->length);
        }
        size += delta_size + 1u + track->length;
        previous = track->tick;
        if (!next_event(data, track)) return 0;
    }
    uint32_t delta_size = write_delta(NULL, finish - previous);
    if (!delta_size || size > MAX_OUTPUT - delta_size - 3u) return 0;
    if (output) {
        write_delta(output + size, finish - previous);
        output[size + delta_size] = 255;
        output[size + delta_size + 1u] = 47;
        output[size + delta_size + 2u] = 0;
    }
    return size + delta_size + 3u;
}

int korvo_midi_file_prepare(const uint8_t *data, uint32_t size,
                           uint8_t **output, uint32_t *output_size)
{
    if (!output || !output_size) return 0;
    *output = NULL; *output_size = 0;
    if (!data || size < 14u || size > MAX_INPUT || memcmp(data, "MThd", 4) ||
        data[8] || data[9] > 1 || data[10] || !data[11] || data[11] > MAX_TRACKS ||
        (data[9] == 0 && data[11] != 1) || (data[12] & 128u) || !(data[12] | data[13])) return 0;
    uint32_t header = be32(data + 4);
    if (header < 6u || header > size - 8u) return 0;
    uint32_t offset = 8u + header;
    unsigned count = data[11];
    track_cursor tracks[MAX_TRACKS] = {0};
    for (unsigned i = 0; i < count; ++i) {
        if (size - offset < 8u || memcmp(data + offset, "MTrk", 4)) return 0;
        uint32_t length = be32(data + offset + 4);
        offset += 8;
        if (!length || length > size - offset) return 0;
        tracks[i].offset = offset; tracks[i].end = offset + length;
        offset += length;
    }
    if (offset != size) return 0;
    uint32_t normalized_size = merge(data, tracks, count, NULL);
    if (!normalized_size) return 0;
    /* Keep format-0 bytes unchanged after validation, including original
     * running-status encoding. Multi-track input always becomes format 0. */
    uint32_t bytes = data[9] == 0 ? size : normalized_size;
    uint8_t *copy = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) return 0;
    if (data[9] == 0) memcpy(copy, data, bytes);
    else {
        if (merge(data, tracks, count, copy) != bytes) { heap_caps_free(copy); return 0; }
        memcpy(copy, "MThd\0\0\0\6\0\0\0\1", 12);
        copy[12] = data[12]; copy[13] = data[13];
        memcpy(copy + 14, "MTrk", 4);
        uint32_t length = bytes - 22u;
        for (unsigned i = 0; i < 4; ++i) copy[18 + i] = length >> (24u - i * 8u);
    }
    *output = copy; *output_size = bytes;
    return 1;
}
