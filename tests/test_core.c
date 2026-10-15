#define _POSIX_C_SOURCE 200809L
#include "wenzi.h"
#include "asr_local.h"

/* Release 构建也执行检查。 */
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 只替代识别器，用真实核心检查状态和收尾。 */
struct wenzi_local {
    wenzi_asr_callback callback;
    void *user;
    int finishing;
};

static wenzi_local *active_local;
static int creates, feeds, finishes, destroys;
static int hold_finish;
static const char *finish_text;

static void fake_emit(wenzi_asr_type type, const char *text, int code) {
    wenzi_asr_result result = { .type = type, .text = text, .code = code };
    assert(active_local);
    active_local->callback(active_local->user, &result);
}

wenzi_local *wenzi_local_create(const wenzi_config *config,
                              wenzi_asr_callback callback, void *user,
                              char *error, size_t size) {
    (void)config;
    (void)error;
    (void)size;
    assert(!active_local);
    active_local = calloc(1, sizeof(*active_local));
    assert(active_local);
    active_local->callback = callback;
    active_local->user = user;
    ++creates;
    return active_local;
}

int wenzi_local_feed(wenzi_local *local, const int16_t *samples, size_t count) {
    assert(local == active_local && samples && count);
    ++feeds;
    return 0;
}

int wenzi_local_finish(wenzi_local *local) {
    assert(local == active_local);
    if (local->finishing) return 0;
    local->finishing = 1;
    ++finishes;
    if (!hold_finish) {
        if (finish_text) fake_emit(WENZI_ASR_FINAL, finish_text, 0);
        fake_emit(WENZI_ASR_DONE, NULL, 0);
    }
    return 0;
}

void wenzi_local_destroy(wenzi_local *local) {
    if (!local) return;
    assert(local == active_local);
    free(local);
    active_local = NULL;
    ++destroys;
}

typedef struct {
    int count[4];
    int order, final_order, idle_order, error;
    uint64_t session, partial_utterance, final_session, final_utterance;
    wenzi_end_reason reason;
    char partial[64], final[64];
} event_log;

static void record_event(void *user, const wenzi_event *event) {
    event_log *log = user;
    assert(event->type >= WENZI_PARTIAL && event->type <= WENZI_STATUS);
    ++log->count[event->type];
    ++log->order;
    log->session = event->session_id;
    if (event->type == WENZI_PARTIAL) {
        /* 同一句确认后不能再改写，但下一句可以继续识别。 */
        if (event->session_id == log->final_session)
            assert(event->utterance_id > log->final_utterance);
        log->partial_utterance = event->utterance_id;
        snprintf(log->partial, sizeof(log->partial), "%s", event->text);
    } else if (event->type == WENZI_FINAL) {
        assert(event->text && *event->text);
        log->final_session = event->session_id;
        log->final_utterance = event->utterance_id;
        log->final_order = log->order;
        log->reason = event->reason;
        snprintf(log->final, sizeof(log->final), "%s", event->text);
    } else if (event->type == WENZI_ERROR) {
        assert(event->code > 0);
        log->error = event->code;
    } else if (event->state == WENZI_IDLE) {
        log->idle_order = log->order;
    }
}

static void reset_fake(void) {
    assert(!active_local);
    creates = feeds = finishes = destroys = 0;
    hold_finish = 0;
    finish_text = NULL;
}

static void test_snapshots_and_stop(void) {
    event_log log = {0};
    wenzi_config config = { .engine = WENZI_LOCAL, .drain_timeout_ms = 100 };
    int16_t samples[WENZI_FRAME_SAMPLES] = {0};
    reset_fake();
    wenzi *core = wenzi_create(&config, record_event, &log);
    assert(core && wenzi_get_state(core) == WENZI_IDLE);
    assert(wenzi_stop(core) == 0 && creates == 0);
    assert(wenzi_start(core) == 0 && creates == 1);
    uint64_t session = log.session;
    assert(wenzi_start(core) == 0 && creates == 1 && log.session == session);
    assert(wenzi_feed_pcm(core, samples, WENZI_FRAME_SAMPLES) == 0);

    fake_emit(WENZI_ASR_PARTIAL, "你好", 0);
    fake_emit(WENZI_ASR_PARTIAL, "你好", 0);
    fake_emit(WENZI_ASR_PARTIAL, "你好 world", 0);
    assert(wenzi_poll(core, 0) == 0);
    assert(log.count[WENZI_PARTIAL] == 2);
    assert(strcmp(log.partial, "你好 world") == 0);
    uint64_t utterance = log.partial_utterance;
    fake_emit(WENZI_ASR_FINAL, "你好 world。", 0);
    assert(log.count[WENZI_FINAL] == 1 && log.final_utterance == utterance);
    fake_emit(WENZI_ASR_PARTIAL, "下一句", 0);
    assert(log.partial_utterance > utterance);

    hold_finish = 1;
    assert(wenzi_stop(core) == 0 && wenzi_get_state(core) == WENZI_DRAINING);
    assert(wenzi_stop(core) == 0 && finishes == 1);
    assert(wenzi_start(core) == -EBUSY);
    assert(wenzi_feed_pcm(core, samples, WENZI_FRAME_SAMPLES) == -EBUSY);
    fake_emit(WENZI_ASR_FINAL, "下一句。", 0);
    assert(wenzi_get_state(core) == WENZI_DRAINING);
    fake_emit(WENZI_ASR_DONE, NULL, 0);
    assert(wenzi_poll(core, 0) == 0);
    assert(wenzi_get_state(core) == WENZI_IDLE);
    assert(log.count[WENZI_FINAL] == 2 && log.reason == WENZI_END_STOP);
    assert(log.final_order < log.idle_order);
    assert(wenzi_stop(core) == 0 && finishes == 1);
    hold_finish = 0;
    finish_text = "EOF 尾句。";
    assert(wenzi_start(core) == 0 && log.session > session);
    assert(wenzi_finish(core, WENZI_END_EOF) == 0);
    assert(wenzi_get_state(core) == WENZI_IDLE && log.reason == WENZI_END_EOF);
    assert(strcmp(log.final, finish_text) == 0 && log.final_order < log.idle_order);
    wenzi_destroy(core);
    assert(destroys == 2 && feeds == 1);
}

static void test_timeout_keeps_partial_unconfirmed(void) {
    event_log log = {0};
    wenzi_config config = { .engine = WENZI_LOCAL, .drain_timeout_ms = 1 };
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 20000000 };
    reset_fake();
    wenzi *core = wenzi_create(&config, record_event, &log);
    assert(core && wenzi_start(core) == 0);
    fake_emit(WENZI_ASR_PARTIAL, "尚未确认", 0);
    hold_finish = 1;
    assert(wenzi_stop(core) == 0);
    while (nanosleep(&delay, &delay) < 0) assert(errno == EINTR);
    assert(wenzi_poll(core, 0) == -ETIMEDOUT);
    assert(wenzi_get_state(core) == WENZI_IDLE);
    assert(log.error == ETIMEDOUT && log.count[WENZI_FINAL] == 0);
    assert(log.count[WENZI_PARTIAL] == 1 && finishes == 1 && destroys == 1);
    wenzi_destroy(core);
}

static void test_missing_cloud_keeps_local_session(void) {
    event_log log = {0};
    wenzi_config config = { .engine = WENZI_LOCAL, .drain_timeout_ms = 100 };
    int16_t samples[WENZI_FRAME_SAMPLES] = {0};
    reset_fake();
    wenzi *core = wenzi_create(&config, record_event, &log);
    assert(core && wenzi_start(core) == 0);
    fake_emit(WENZI_ASR_PARTIAL, "本地结果", 0);
    fake_emit(WENZI_ASR_FINAL, "本地结果。", 0);
    uint64_t session = log.session;
    assert(wenzi_switch_engine(core, WENZI_CLOUD) == -ENOTSUP);
    assert(wenzi_get_engine(core) == WENZI_LOCAL);
    assert(wenzi_get_state(core) == WENZI_RUNNING && log.session == session);
    assert(finishes == 0 && destroys == 0 && creates == 1);
    assert(log.count[WENZI_FINAL] == 1 && strcmp(log.final, "本地结果。") == 0);
    assert(wenzi_feed_pcm(core, samples, WENZI_FRAME_SAMPLES) == 0);
    assert(wenzi_stop(core) == 0);
    assert(wenzi_poll(core, 0) == 0 && wenzi_get_state(core) == WENZI_IDLE);
    wenzi_destroy(core);
}

static void test_error_codes_control_completion(void) {
    event_log log = {0};
    wenzi_config config = { .engine = WENZI_LOCAL, .drain_timeout_ms = 100 };
    int16_t samples[WENZI_FRAME_SAMPLES] = {0};
    reset_fake();
    wenzi *core = wenzi_create(&config, record_event, &log);
    assert(core && wenzi_start(core) == 0);
    fake_emit(WENZI_ASR_PARTIAL, "标点可降级", 0);
    fake_emit(WENZI_ASR_ERROR, "标点失败", EILSEQ);
    assert(log.error == EILSEQ && wenzi_get_state(core) == WENZI_RUNNING);
    assert(wenzi_feed_pcm(core, samples, WENZI_FRAME_SAMPLES) == 0);
    fake_emit(WENZI_ASR_FINAL, "标点可降级", 0);
    fake_emit(WENZI_ASR_DONE, NULL, 0);
    assert(wenzi_poll(core, 0) == 0 && wenzi_get_state(core) == WENZI_IDLE);
    assert(log.count[WENZI_FINAL] == 1 && log.final_order < log.idle_order);
    wenzi_destroy(core);

    /* ERROR 后的成功 DONE 不能覆盖终止错误。 */
    log = (event_log){0};
    reset_fake();
    core = wenzi_create(&config, record_event, &log);
    assert(core && wenzi_start(core) == 0);
    fake_emit(WENZI_ASR_PARTIAL, "尚未确认", 0);
    fake_emit(WENZI_ASR_ERROR, "识别失败", -EIO);
    fake_emit(WENZI_ASR_DONE, NULL, 0);
    assert(wenzi_poll(core, 0) == -EIO && wenzi_get_state(core) == WENZI_IDLE);
    assert(log.error == EIO && log.count[WENZI_FINAL] == 0 && destroys == 1);
    wenzi_destroy(core);

    log = (event_log){0};
    reset_fake();
    core = wenzi_create(&config, record_event, &log);
    assert(core && wenzi_start(core) == 0);
    fake_emit(WENZI_ASR_PARTIAL, "尚未确认", 0);
    fake_emit(WENZI_ASR_DONE, NULL, ECONNRESET);
    assert(wenzi_poll(core, 0) == -ECONNRESET);
    assert(wenzi_get_state(core) == WENZI_IDLE);
    assert(log.count[WENZI_FINAL] == 0 && destroys == 1);
    wenzi_destroy(core);
}

int main(void) {
    test_snapshots_and_stop();
    test_timeout_keeps_partial_unconfirmed();
    test_missing_cloud_keeps_local_session();
    test_error_codes_control_completion();
    puts("test_core: OK (fake ASR; real core state and drain)");
    return 0;
}
