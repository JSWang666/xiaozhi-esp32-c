#include "ogg_demuxer.h"

#include <stdlib.h>
#include <string.h>
#include <esp_log.h>

#define TAG "OggDemuxer"

typedef enum {
    OGG_FIND_PAGE,
    OGG_PARSE_HEADER,
    OGG_PARSE_SEGMENTS,
    OGG_PARSE_DATA
} ogg_parse_state_t;

typedef struct {
    bool head_seen;
    bool tags_seen;
    int  sample_rate;
} opus_info_t;

typedef struct {
    bool    packet_continued;
    uint8_t header[27];
    uint8_t seg_table[255];
    uint8_t packet_buf[8192];
    size_t  packet_len;
    size_t  seg_count;
    size_t  seg_index;
    size_t  data_offset;
    size_t  bytes_needed;
    size_t  seg_remaining;
    size_t  body_size;
    size_t  body_offset;
} ogg_context_t;

struct ogg_demuxer {
    ogg_parse_state_t state;
    ogg_context_t ctx;
    opus_info_t opus;
    ogg_demuxer_cb_t callback;
    void *cb_ctx;
};

static inline size_t min_sz(size_t a, size_t b) { return a < b ? a : b; }

ogg_demuxer_t *ogg_demuxer_create(void)
{
    ogg_demuxer_t *d = (ogg_demuxer_t *)calloc(1, sizeof(*d));
    if (d) ogg_demuxer_reset(d);
    return d;
}

void ogg_demuxer_destroy(ogg_demuxer_t *d)
{
    free(d);
}

void ogg_demuxer_set_callback(ogg_demuxer_t *d, ogg_demuxer_cb_t cb, void *ctx)
{
    if (!d) return;
    d->callback = cb;
    d->cb_ctx = ctx;
}

void ogg_demuxer_reset(ogg_demuxer_t *d)
{
    if (!d) return;
    d->opus.head_seen = false;
    d->opus.tags_seen = false;
    d->opus.sample_rate = 48000;
    d->state = OGG_FIND_PAGE;
    memset(&d->ctx, 0, sizeof(d->ctx));
    d->ctx.bytes_needed = 4;
}

size_t ogg_demuxer_process(ogg_demuxer_t *d, const uint8_t *data, size_t size)
{
    if (!d || !data) return 0;

    ogg_context_t *ctx = &d->ctx;
    size_t processed = 0;

    while (processed < size) {
        switch (d->state) {
        case OGG_FIND_PAGE: {
            if (ctx->bytes_needed < 4) {
                size_t to_copy = min_sz(size - processed, ctx->bytes_needed);
                memcpy(ctx->header + (4 - ctx->bytes_needed), data + processed, to_copy);
                processed += to_copy;
                ctx->bytes_needed -= to_copy;
                if (ctx->bytes_needed == 0) {
                    if (memcmp(ctx->header, "OggS", 4) == 0) {
                        d->state = OGG_PARSE_HEADER;
                        ctx->data_offset = 4;
                        ctx->bytes_needed = 27 - 4;
                    } else {
                        memmove(ctx->header, ctx->header + 1, 3);
                        ctx->bytes_needed = 1;
                    }
                } else {
                    return processed;
                }
            } else if (ctx->bytes_needed == 4) {
                bool found = false;
                size_t i = 0;
                size_t remaining = size - processed;
                for (; i + 4 <= remaining; i++) {
                    if (memcmp(data + processed + i, "OggS", 4) == 0) {
                        found = true;
                        break;
                    }
                }
                if (found) {
                    processed += i + 4;
                    d->state = OGG_PARSE_HEADER;
                    ctx->data_offset = 4;
                    ctx->bytes_needed = 27 - 4;
                } else {
                    size_t partial_len = remaining - i;
                    if (partial_len > 0) {
                        memcpy(ctx->header, data + processed + i, partial_len);
                        ctx->bytes_needed = 4 - partial_len;
                        processed += i + partial_len;
                    } else {
                        processed += i;
                    }
                    return processed;
                }
            } else {
                ESP_LOGE(TAG, "Error state: bytes_needed=%zu", ctx->bytes_needed);
                ogg_demuxer_reset(d);
                return processed;
            }
            break;
        }

        case OGG_PARSE_HEADER: {
            size_t available = size - processed;
            if (available < ctx->bytes_needed) {
                memcpy(ctx->header + ctx->data_offset, data + processed, available);
                ctx->data_offset += available;
                ctx->bytes_needed -= available;
                processed += available;
                return processed;
            }
            size_t to_copy = ctx->bytes_needed;
            memcpy(ctx->header + ctx->data_offset, data + processed, to_copy);
            processed += to_copy;
            ctx->data_offset += to_copy;
            ctx->bytes_needed = 0;

            if (ctx->header[4] != 0) {
                ESP_LOGE(TAG, "Invalid Ogg version: %d", ctx->header[4]);
                d->state = OGG_FIND_PAGE;
                ctx->bytes_needed = 4;
                ctx->data_offset = 0;
                break;
            }
            ctx->seg_count = ctx->header[26];
            if (ctx->seg_count > 0 && ctx->seg_count <= 255) {
                d->state = OGG_PARSE_SEGMENTS;
                ctx->bytes_needed = ctx->seg_count;
                ctx->data_offset = 0;
            } else {
                d->state = OGG_FIND_PAGE;
                ctx->bytes_needed = 4;
                ctx->data_offset = 0;
            }
            break;
        }

        case OGG_PARSE_SEGMENTS: {
            size_t available = size - processed;
            if (available < ctx->bytes_needed) {
                memcpy(ctx->seg_table + ctx->data_offset, data + processed, available);
                ctx->data_offset += available;
                ctx->bytes_needed -= available;
                processed += available;
                return processed;
            }
            size_t to_copy = ctx->bytes_needed;
            memcpy(ctx->seg_table + ctx->data_offset, data + processed, to_copy);
            processed += to_copy;

            d->state = OGG_PARSE_DATA;
            ctx->seg_index = 0;
            ctx->data_offset = 0;
            ctx->body_size = 0;
            for (size_t i = 0; i < ctx->seg_count; ++i)
                ctx->body_size += ctx->seg_table[i];
            ctx->body_offset = 0;
            ctx->seg_remaining = 0;
            break;
        }

        case OGG_PARSE_DATA: {
            while (ctx->seg_index < ctx->seg_count && processed < size) {
                uint8_t seg_len;
                if (ctx->seg_remaining > 0) {
                    seg_len = (uint8_t)ctx->seg_remaining;
                } else {
                    seg_len = ctx->seg_table[ctx->seg_index];
                    ctx->seg_remaining = seg_len;
                }

                if (ctx->packet_len + seg_len > sizeof(ctx->packet_buf)) {
                    ESP_LOGE(TAG, "Packet buffer overflow");
                    d->state = OGG_FIND_PAGE;
                    ctx->packet_len = 0;
                    ctx->packet_continued = false;
                    ctx->seg_remaining = 0;
                    ctx->bytes_needed = 4;
                    return processed;
                }

                size_t to_copy = min_sz(size - processed, (size_t)seg_len);
                memcpy(ctx->packet_buf + ctx->packet_len, data + processed, to_copy);
                processed += to_copy;
                ctx->packet_len += to_copy;
                ctx->body_offset += to_copy;
                ctx->seg_remaining -= to_copy;

                if (ctx->seg_remaining > 0)
                    return processed;

                bool seg_continued = (ctx->seg_table[ctx->seg_index] == 255);
                if (!seg_continued) {
                    if (ctx->packet_len) {
                        if (!d->opus.head_seen) {
                            if (ctx->packet_len >= 8 && memcmp(ctx->packet_buf, "OpusHead", 8) == 0) {
                                d->opus.head_seen = true;
                                if (ctx->packet_len >= 19) {
                                    d->opus.sample_rate = ctx->packet_buf[12] |
                                                          ((int)ctx->packet_buf[13] << 8) |
                                                          ((int)ctx->packet_buf[14] << 16) |
                                                          ((int)ctx->packet_buf[15] << 24);
                                    ESP_LOGD(TAG, "OpusHead found, sample_rate=%d", d->opus.sample_rate);
                                }
                                ctx->packet_len = 0;
                                ctx->packet_continued = false;
                                ctx->seg_index++;
                                ctx->seg_remaining = 0;
                                continue;
                            }
                        }
                        if (!d->opus.tags_seen) {
                            if (ctx->packet_len >= 8 && memcmp(ctx->packet_buf, "OpusTags", 8) == 0) {
                                d->opus.tags_seen = true;
                                ESP_LOGD(TAG, "OpusTags found");
                                ctx->packet_len = 0;
                                ctx->packet_continued = false;
                                ctx->seg_index++;
                                ctx->seg_remaining = 0;
                                continue;
                            }
                        }
                        if (d->opus.head_seen && d->opus.tags_seen) {
                            if (d->callback) {
                                d->callback(ctx->packet_buf, d->opus.sample_rate, ctx->packet_len, d->cb_ctx);
                            }
                        } else {
                            ESP_LOGW(TAG, "No OpusHead/Tags yet, discarding packet");
                        }
                    }
                    ctx->packet_len = 0;
                    ctx->packet_continued = false;
                } else {
                    ctx->packet_continued = true;
                }
                ctx->seg_index++;
                ctx->seg_remaining = 0;
            }
            if (ctx->seg_index == ctx->seg_count) {
                if (!ctx->packet_continued) ctx->packet_len = 0;
                d->state = OGG_FIND_PAGE;
                ctx->bytes_needed = 4;
                ctx->data_offset = 0;
            }
            break;
        }
        }
    }
    return processed;
}
