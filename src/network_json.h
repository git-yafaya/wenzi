#ifndef WENZI_NETWORK_JSON_H
#define WENZI_NETWORK_JSON_H

#include <libwebsockets.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int (*wenzi_json_visitor)(void *user, const struct lejp_ctx *ctx,
                                 unsigned char reason, const char *value,
                                 size_t length);

/* 拒绝 NUL、过长编码、代理码点和越界码点。 */
static inline int wenzi_json_utf8(const char *text, size_t length)
{
    size_t i = 0;

    if (!text && length)
        return -EILSEQ;
    while (i < length) {
        unsigned char first = (unsigned char)text[i++];
        uint32_t code, minimum;
        size_t count;

        if (first < 0x80) {
            if (!first)
                return -EILSEQ;
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            code = first & 0x1f;
            minimum = 0x80;
            count = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            code = first & 0x0f;
            minimum = 0x800;
            count = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            code = first & 0x07;
            minimum = 0x10000;
            count = 3;
        } else {
            return -EILSEQ;
        }
        if (count > length - i)
            return -EILSEQ;
        while (count--) {
            unsigned char next = (unsigned char)text[i++];

            if ((next & 0xc0) != 0x80)
                return -EILSEQ;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff))
            return -EILSEQ;
    }
    return 0;
}

/* 不加外围引号；output=NULL 时只计算长度。 */
static inline int wenzi_json_escape(const char *text, size_t length,
                                    char *output, size_t capacity)
{
    static const char hex[] = "0123456789abcdef";
    size_t needed = 0, position = 0, i;
    int result = wenzi_json_utf8(text, length);

    if (result < 0)
        return result;
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];

        needed += c < 0x20 ? 6 : c == '"' || c == '\\' ? 2 : 1;
        if (needed > INT_MAX)
            return -E2BIG;
    }
    if (!output)
        return (int)needed;
    if (needed >= capacity) {
        if (capacity)
            output[0] = '\0';
        return -ENOSPC;
    }
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];

        if (c < 0x20) {
            memcpy(output + position, "\\u00", 4);
            output[position + 4] = hex[c >> 4];
            output[position + 5] = hex[c & 15];
            position += 6;
        } else {
            if (c == '"' || c == '\\')
                output[position++] = '\\';
            output[position++] = (char)c;
        }
    }
    output[position] = '\0';
    return (int)position;
}

struct wenzi_json_parser {
    char *scratch;
    size_t size;
    size_t used;
    wenzi_json_visitor visit;
    void *user;
    int error;
    int complete;
};

static inline signed char wenzi_json_callback(struct lejp_ctx *ctx, char why)
{
    struct wenzi_json_parser *parser = ctx->user;
    unsigned char reason = (unsigned char)why;
    const char *value = NULL;
    size_t length = 0;
    int result;

    if (reason == LEJPCB_CONSTRUCTED || reason == LEJPCB_DESTRUCTED ||
        reason == LEJPCB_FAILED || reason == LEJPCB_START)
        return 0;
    if (reason == LEJPCB_COMPLETE) {
        parser->complete = 1;
        return 0;
    }
    if (reason == LEJPCB_VAL_STR_START) {
        parser->used = 0;
        return 0;
    }
    if (reason == LEJPCB_VAL_STR_CHUNK || reason == LEJPCB_VAL_STR_END) {
        if ((size_t)ctx->npos >= parser->size - parser->used) {
            parser->error = -E2BIG;
            return -1;
        }
        memcpy(parser->scratch + parser->used, ctx->buf, ctx->npos);
        parser->used += ctx->npos;
        parser->scratch[parser->used] = '\0';
        if (reason == LEJPCB_VAL_STR_CHUNK)
            return 0;
        value = parser->scratch;
        length = parser->used;
        result = wenzi_json_utf8(value, length);
        if (result < 0) {
            parser->error = result;
            return -1;
        }
    } else if (reason == LEJPCB_VAL_NUM_INT || reason == LEJPCB_VAL_NUM_FLOAT) {
        ctx->buf[ctx->npos] = '\0';
        value = ctx->buf;
        length = ctx->npos;
    } else if (reason == LEJPCB_PAIR_NAME) {
        result = wenzi_json_utf8(ctx->path, ctx->pst[ctx->pst_sp].ppos);
        if (result < 0) {
            parser->error = result;
            return -1;
        }
    }
    result = parser->visit(parser->user, ctx, reason, value, length);
    if (result) {
        parser->error = result < 0 ? result : -EINVAL;
        return -1;
    }
    return 0;
}

/* 字符串拼到 scratch 后通知；路径和数组索引由 LEJP 提供。 */
static inline int wenzi_json_parse(const char *json, size_t length,
                                   char *scratch, size_t scratch_size,
                                   wenzi_json_visitor visit, void *user)
{
    struct wenzi_json_parser parser = {
        .scratch = scratch, .size = scratch_size, .visit = visit, .user = user
    };
    struct lejp_ctx ctx;
    int remaining, result;
    size_t tail;

    if (!json || !length || !scratch || !scratch_size || !visit)
        return -EINVAL;
    if (length > INT_MAX)
        return -E2BIG;
    result = wenzi_json_utf8(json, length);
    if (result < 0)
        return result;
    lejp_construct(&ctx, wenzi_json_callback, &parser, NULL, 0);
    remaining = lejp_parse(&ctx, (const unsigned char *)json, (int)length);
    lejp_destruct(&ctx);
    if (parser.error)
        return parser.error;
    if (remaining < 0 || !parser.complete)
        return -EINVAL;
    tail = length - (size_t)remaining;
    while (tail < length) {
        char c = json[tail++];

        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            return -EINVAL;
    }
    return 0;
}

#endif
