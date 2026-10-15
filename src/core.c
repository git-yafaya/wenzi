#define _POSIX_C_SOURCE 200809L
#include "wenzi.h"
#include "asr_local.h"
#if WENZI_WITH_NETWORK
#include "wenzi_network.h"
#endif
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct wenzi {
    wenzi_config config;
    wenzi_event_callback callback;
    void *user;
    wenzi_state state;
    wenzi_engine engine;
    wenzi_engine next_engine;
    wenzi_local *local;
#if WENZI_WITH_NETWORK
    wenzi_iflytek *cloud;
#endif
    uint64_t session_id;
    uint64_t utterance_id;
    wenzi_end_reason reason;
    char *partial;
    int done;
    int failure;
    int switching;
    int64_t deadline_ms;
    wenzi_statistics statistics;
};

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}

static int64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void emit(wenzi *ctx, wenzi_event_type type, const char *text, int code) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    wenzi_event event = {
        .type = type, .engine = ctx->engine, .state = ctx->state,
        .session_id = ctx->session_id, .utterance_id = ctx->utterance_id,
        .timestamp = ts.tv_sec + ts.tv_nsec / 1000000000.0,
        .text = text, .code = code, .reason = ctx->reason
    };
    if (ctx->callback) ctx->callback(ctx->user, &event);
}

static void on_asr(void *user, const wenzi_asr_result *result) {
    wenzi *ctx = user;
    if (ctx->state == WENZI_IDLE || ctx->done || !result) return;
    switch (result->type) {
    case WENZI_ASR_PARTIAL: {
        const char *text = result->text ? result->text : "";
        if (!*text || (ctx->partial && !strcmp(ctx->partial, text))) break;
        char *copy = strdup(text);
        if (!copy) {
            ctx->failure = -ENOMEM;
            emit(ctx, WENZI_ERROR, "识别文本内存不足", ENOMEM);
            break;
        }
        free(ctx->partial);
        ctx->partial = copy;
        emit(ctx, WENZI_PARTIAL, text, 0);
        break;
    }
    case WENZI_ASR_FINAL:
        if (result->text && *result->text)
            emit(ctx, WENZI_FINAL, result->text, 0);
        free(ctx->partial);
        ctx->partial = NULL;
        ++ctx->utterance_id;
        break;
    case WENZI_ASR_ERROR:
        emit(ctx, WENZI_ERROR, result->text ? result->text : "识别模块错误",
             result->code ? abs(result->code) : EIO);
        /* 负错误码终止；正错误码允许标点等可选处理降级。 */
        if (result->code < 0) ctx->failure = result->code;
        break;
    case WENZI_ASR_DONE:
        if (result->code) ctx->failure = -abs(result->code);
        ctx->done = 1;
        break;
    }
}

static void close_backend(wenzi *ctx) {
    wenzi_local_destroy(ctx->local);
    ctx->local = NULL;
#if WENZI_WITH_NETWORK
    wenzi_iflytek_destroy(ctx->cloud);
    ctx->cloud = NULL;
#endif
    free(ctx->partial);
    ctx->partial = NULL;
    ctx->done = 0;
    ctx->failure = 0;
}

static int supported(wenzi_engine engine) {
    if (engine == WENZI_LOCAL) return 1;
#if WENZI_WITH_NETWORK
    if (engine == WENZI_CLOUD) return 1;
#endif
    return 0;
}

static int open_backend(wenzi *ctx) {
    char error[512] = "识别引擎初始化失败";
    errno = 0;
    if (ctx->engine == WENZI_LOCAL) {
        uint64_t started = monotonic_ns();
        ctx->local = wenzi_local_create(&ctx->config, on_asr, ctx,
                                        error, sizeof(error));
        ctx->statistics.local_initialization_ns += monotonic_ns() - started;
    }
#if WENZI_WITH_NETWORK
    else
        ctx->cloud = wenzi_iflytek_create(ctx->config.cloud, on_asr, ctx,
                                          error, sizeof(error));
    int ready = ctx->local || ctx->cloud;
#else
    int ready = ctx->local != NULL;
#endif
    if (!ready) {
        int code = errno ? errno : EIO;
        emit(ctx, WENZI_ERROR, error, code);
        return -code;
    }
    ++ctx->session_id;
    ctx->utterance_id = 1;
    ctx->reason = WENZI_END_ENDPOINT;
    ctx->done = 0;
    ctx->failure = 0;
    ctx->state = WENZI_RUNNING;
    emit(ctx, WENZI_STATUS, NULL, 0);
    return 0;
}

/* 回调返回后再销毁引擎，避免释放正在执行的模块。 */
static int complete(wenzi *ctx) {
    if (!ctx->done && !ctx->failure) return 0;
    int result = ctx->failure;
    int switching = ctx->switching;
    wenzi_engine target = ctx->next_engine;
    ctx->switching = 0;
    close_backend(ctx);
    ctx->state = WENZI_IDLE;
    emit(ctx, WENZI_STATUS, NULL, 0);
    if (switching) {
        ctx->engine = target;
        result = open_backend(ctx);
        if (result < 0) emit(ctx, WENZI_STATUS, NULL, 0);
    }
    return result;
}

wenzi *wenzi_create(const wenzi_config *config, wenzi_event_callback callback,
                    void *user) {
    if (!config || (config->engine != WENZI_LOCAL &&
                    config->engine != WENZI_CLOUD)) {
        errno = EINVAL;
        return NULL;
    }
    wenzi *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->config = *config;
    ctx->config.num_threads = config->num_threads > 0 ? config->num_threads : 1;
    ctx->config.drain_timeout_ms = config->drain_timeout_ms > 0 ?
                                  config->drain_timeout_ms : 10000;
    ctx->callback = callback;
    ctx->user = user;
    ctx->engine = config->engine;
    return ctx;
}

void wenzi_destroy(wenzi *ctx) {
    if (!ctx) return;
    close_backend(ctx);
    free(ctx);
}

int wenzi_start(wenzi *ctx) {
    if (!ctx) return -EINVAL;
    if (ctx->state == WENZI_RUNNING) return 0;
    if (ctx->state == WENZI_DRAINING) return -EBUSY;
    if (!supported(ctx->engine)) {
        emit(ctx, WENZI_ERROR, "此构建未启用目标引擎", ENOTSUP);
        return -ENOTSUP;
    }
    return open_backend(ctx);
}

int wenzi_feed_pcm(wenzi *ctx, const int16_t *samples, size_t count) {
    if (!ctx || !samples || !count || count > WENZI_FRAME_SAMPLES)
        return -EINVAL;
    if (ctx->state != WENZI_RUNNING) return -EBUSY;
    int result;
    if (ctx->engine == WENZI_LOCAL) {
        uint64_t started = monotonic_ns();
        result = wenzi_local_feed(ctx->local, samples, count);
        ctx->statistics.local_processing_ns += monotonic_ns() - started;
        if (!result) ctx->statistics.local_samples += count;
    }
#if WENZI_WITH_NETWORK
    else
        result = wenzi_iflytek_feed_pcm(ctx->cloud, samples, count);
#else
    else result = -ENOTSUP;
#endif
    if (!result) ctx->statistics.accepted_samples += count;
    if (result < 0) {
        emit(ctx, WENZI_ERROR, "PCM 送入识别模块失败", -result);
        ctx->failure = result;
    }
    int completed = complete(ctx);
    return result < 0 ? result : completed;
}

int wenzi_finish(wenzi *ctx, wenzi_end_reason reason) {
    if (!ctx || reason < WENZI_END_EOF || reason > WENZI_END_SWITCH)
        return -EINVAL;
    if (ctx->state == WENZI_IDLE || ctx->state == WENZI_DRAINING) return 0;
    ctx->state = WENZI_DRAINING;
    ctx->reason = reason;
    ctx->deadline_ms = monotonic_ms() + ctx->config.drain_timeout_ms;
    emit(ctx, WENZI_STATUS, NULL, 0);
    int result;
    if (ctx->engine == WENZI_LOCAL) {
        uint64_t started = monotonic_ns();
        result = wenzi_local_finish(ctx->local);
        ctx->statistics.local_processing_ns += monotonic_ns() - started;
    }
#if WENZI_WITH_NETWORK
    else result = wenzi_iflytek_finish(ctx->cloud);
#else
    else result = -ENOTSUP;
#endif
    if (result < 0) {
        emit(ctx, WENZI_ERROR, "识别收尾失败，当前句未确认", -result);
        ctx->failure = result;
    }
    int completed = complete(ctx);
    return result < 0 ? result : completed;
}

int wenzi_stop(wenzi *ctx) {
    if (!ctx) return -EINVAL;
    ctx->switching = 0;
    return wenzi_finish(ctx, WENZI_END_STOP);
}

int wenzi_switch_engine(wenzi *ctx, wenzi_engine engine) {
    if (!ctx || (engine != WENZI_LOCAL && engine != WENZI_CLOUD))
        return -EINVAL;
    if (ctx->state == WENZI_DRAINING) return -EBUSY;
    if (engine == ctx->engine) return 0;
    if (!supported(engine)) {
        emit(ctx, WENZI_ERROR, "此构建未启用目标引擎", ENOTSUP);
        return -ENOTSUP;
    }
    if (ctx->state == WENZI_IDLE) {
        ctx->engine = engine;
        wenzi_status(ctx);
        return 0;
    }
    ctx->next_engine = engine;
    ctx->switching = 1;
    return wenzi_finish(ctx, WENZI_END_SWITCH);
}

int wenzi_poll(wenzi *ctx, int timeout_ms) {
    if (!ctx || timeout_ms < 0) return -EINVAL;
    if (ctx->state == WENZI_IDLE) return 0;
    int result = 0;
#if WENZI_WITH_NETWORK
    if (ctx->engine == WENZI_CLOUD)
        result = wenzi_iflytek_poll(ctx->cloud, timeout_ms);
    else
#endif
    if (timeout_ms) {
        struct timespec delay = {timeout_ms / 1000,
                                 (timeout_ms % 1000) * 1000000L};
        nanosleep(&delay, NULL);
    }
    if (result < 0) {
        emit(ctx, WENZI_ERROR, "识别模块轮询失败", -result);
        ctx->failure = result;
    }
    if (ctx->state == WENZI_DRAINING && !ctx->done &&
        monotonic_ms() >= ctx->deadline_ms) {
        emit(ctx, WENZI_ERROR, "识别收尾超时，当前句未确认", ETIMEDOUT);
        ctx->failure = -ETIMEDOUT;
    }
    int completed = complete(ctx);
    return result < 0 ? result : completed;
}

wenzi_state wenzi_get_state(const wenzi *ctx) {
    return ctx ? ctx->state : WENZI_IDLE;
}

wenzi_engine wenzi_get_engine(const wenzi *ctx) {
    return ctx ? ctx->engine : WENZI_LOCAL;
}

void wenzi_status(wenzi *ctx) {
    if (ctx) emit(ctx, WENZI_STATUS, NULL, 0);
}

void wenzi_report_error(wenzi *ctx, int code, const char *message) {
    if (ctx) emit(ctx, WENZI_ERROR, message, code ? abs(code) : EIO);
}

void wenzi_get_statistics(const wenzi *ctx, wenzi_statistics *statistics) {
    if (ctx && statistics) *statistics = ctx->statistics;
}

const char *wenzi_engine_name(wenzi_engine engine) {
    return engine == WENZI_LOCAL ? "local" :
           engine == WENZI_CLOUD ? "cloud" : "unknown";
}

const char *wenzi_state_name(wenzi_state state) {
    switch (state) {
    case WENZI_IDLE: return "idle";
    case WENZI_RUNNING: return "running";
    case WENZI_DRAINING: return "draining";
    }
    return "unknown";
}
