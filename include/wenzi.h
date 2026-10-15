#ifndef WENZI_H
#define WENZI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WENZI_SAMPLE_RATE 16000
#define WENZI_FRAME_SAMPLES 320

typedef struct wenzi wenzi;
struct wenzi_iflytek_config;

typedef enum { WENZI_LOCAL, WENZI_CLOUD } wenzi_engine;
typedef enum { WENZI_IDLE, WENZI_RUNNING, WENZI_DRAINING } wenzi_state;
typedef enum {
    WENZI_PARTIAL, WENZI_FINAL, WENZI_ERROR, WENZI_STATUS
} wenzi_event_type;
typedef enum {
    WENZI_END_ENDPOINT, WENZI_END_EOF, WENZI_END_STOP, WENZI_END_SWITCH
} wenzi_end_reason;

typedef struct {
    wenzi_event_type type;
    wenzi_engine engine;
    wenzi_state state;
    uint64_t session_id;
    uint64_t utterance_id;
    double timestamp;
    const char *text;
    int code;
    wenzi_end_reason reason;
} wenzi_event;

/* 文本仅在回调期间有效；需要保留时复制。 */
typedef void (*wenzi_event_callback)(void *user, const wenzi_event *event);

typedef struct {
    wenzi_engine engine;
    const char *encoder;
    const char *decoder;
    const char *joiner;
    const char *tokens;
    const char *punctuation_model;
    int num_threads;
    const struct wenzi_iflytek_config *cloud;
    int drain_timeout_ms;
} wenzi_config;

typedef struct {
    uint64_t accepted_samples;
    uint64_t local_samples;
    uint64_t local_processing_ns;
    uint64_t local_initialization_ns;
} wenzi_statistics;

/* 配置字符串由调用者持有，直到 destroy；所有调用和回调在同一线程。 */
wenzi *wenzi_create(const wenzi_config *config, wenzi_event_callback callback,
                    void *user);
void wenzi_destroy(wenzi *ctx);
int wenzi_start(wenzi *ctx);
/* 单块 1..320 个采样，成功整块接收，失败后不重试该块。 */
int wenzi_feed_pcm(wenzi *ctx, const int16_t *samples, size_t count);
int wenzi_finish(wenzi *ctx, wenzi_end_reason reason);
int wenzi_stop(wenzi *ctx);
int wenzi_switch_engine(wenzi *ctx, wenzi_engine engine);
int wenzi_poll(wenzi *ctx, int timeout_ms);
wenzi_state wenzi_get_state(const wenzi *ctx);
wenzi_engine wenzi_get_engine(const wenzi *ctx);
void wenzi_status(wenzi *ctx);
void wenzi_report_error(wenzi *ctx, int code, const char *message);
void wenzi_get_statistics(const wenzi *ctx, wenzi_statistics *statistics);
const char *wenzi_engine_name(wenzi_engine engine);
const char *wenzi_state_name(wenzi_state state);

/* ERROR 负 errno 为终止错误，正 errno 为可恢复错误；DONE 非零为失败。 */
/* DONE 表示收尾完成，不把未确认的 partial 升级为 final。 */
typedef enum {
    WENZI_ASR_PARTIAL, WENZI_ASR_FINAL, WENZI_ASR_ERROR, WENZI_ASR_DONE
} wenzi_asr_type;
typedef struct {
    wenzi_asr_type type;
    const char *text;
    int code;
} wenzi_asr_result;
typedef void (*wenzi_asr_callback)(void *user, const wenzi_asr_result *result);

#ifdef __cplusplus
}
#endif
#endif
