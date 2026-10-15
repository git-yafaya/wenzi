#include "asr_local.h"
#include "punctuation.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sherpa-onnx/c-api/c-api.h"

struct wenzi_local {
    const SherpaOnnxOnlineRecognizer *recognizer;
    const SherpaOnnxOnlineStream *stream;
    const SherpaOnnxOnlineRecognizerResult *previous;
    wenzi_punctuation *punctuation;
    wenzi_asr_callback callback;
    void *user;
    int finished;
    int punctuation_error_code;
    char punctuation_error[256];
};

static void emit(wenzi_local *local, wenzi_asr_type type, const char *text,
                 int code) {
    wenzi_asr_result result = {.type = type, .text = text, .code = code};
    local->callback(local->user, &result);
}

static void report_punctuation_error(wenzi_local *local) {
    if (!local->punctuation_error_code) return;
    emit(local, WENZI_ASR_ERROR, local->punctuation_error,
         local->punctuation_error_code);
    local->punctuation_error_code = 0;
}

static int publish_result(wenzi_local *local, int final) {
    const SherpaOnnxOnlineRecognizerResult *result =
        SherpaOnnxGetOnlineStreamResult(local->recognizer, local->stream);
    if (!result || !result->text) {
        SherpaOnnxDestroyOnlineRecognizerResult(result);
        return -EIO;
    }

    if (final) {
        if (*result->text) {
            const char *punctuated = local->punctuation ?
                wenzi_punctuation_apply(local->punctuation, result->text) : NULL;
            if (local->punctuation && (!punctuated || !*punctuated))
                emit(local, WENZI_ASR_ERROR, "补标点失败，保留识别原文", EIO);
            emit(local, WENZI_ASR_FINAL,
                 punctuated && *punctuated ? punctuated : result->text, 0);
            wenzi_punctuation_free_text(punctuated);
        }
        SherpaOnnxDestroyOnlineRecognizerResult(local->previous);
        local->previous = NULL;
    } else if ((*result->text || local->previous) &&
               (!local->previous || strcmp(result->text, local->previous->text))) {
        /* 保留库的快照，下一帧只比较文本。 */
        emit(local, WENZI_ASR_PARTIAL, result->text, 0);
        SherpaOnnxDestroyOnlineRecognizerResult(local->previous);
        local->previous = result;
        return 0;
    }
    SherpaOnnxDestroyOnlineRecognizerResult(result);
    return 0;
}

static int decode_pending(wenzi_local *local, int finalizing) {
    while (SherpaOnnxIsOnlineStreamReady(local->recognizer, local->stream)) {
        SherpaOnnxDecodeOnlineStream(local->recognizer, local->stream);
        if (!finalizing) {
            int endpoint = SherpaOnnxOnlineStreamIsEndpoint(local->recognizer,
                                                           local->stream);
            int rc = publish_result(local, endpoint);
            if (rc < 0) return rc;
            if (endpoint)
                SherpaOnnxOnlineStreamReset(local->recognizer, local->stream);
        }
    }
    return 0;
}

static void fail_recognition(wenzi_local *local, int code) {
    local->finished = 1;
    emit(local, WENZI_ASR_ERROR, "无法读取本地识别结果", -code);
    emit(local, WENZI_ASR_DONE, NULL, -code);
}

wenzi_local *wenzi_local_create(const wenzi_config *config,
                              wenzi_asr_callback callback, void *user,
                              char *error, size_t size) {
    if (error && size) error[0] = '\0';
    if (!config || !callback) {
        if (error && size) snprintf(error, size, "本地识别配置或回调为空");
        errno = EINVAL;
        return NULL;
    }
    const char *paths[] = {config->encoder, config->decoder, config->joiner,
                           config->tokens};
    const char *names[] = {"encoder", "decoder", "joiner", "tokens"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        struct stat info;
        int code = 0;
        if (!paths[i] || !*paths[i]) code = EINVAL;
        else if (stat(paths[i], &info) < 0 || access(paths[i], R_OK) < 0) code = errno;
        else if (!S_ISREG(info.st_mode) || info.st_size == 0) code = EINVAL;
        if (code) {
            if (error && size)
                snprintf(error, size, "无法读取 %s 文件 %s: %s", names[i],
                         paths[i] ? paths[i] : "", strerror(code));
            errno = code;
            return NULL;
        }
    }

    wenzi_local *local = calloc(1, sizeof(*local));
    if (!local) {
        if (error && size) snprintf(error, size, "本地识别内存不足");
        errno = ENOMEM;
        return NULL;
    }
    local->callback = callback;
    local->user = user;
    SherpaOnnxOnlineRecognizerConfig recognizer_config = {0};
    recognizer_config.feat_config.sample_rate = WENZI_SAMPLE_RATE;
    recognizer_config.feat_config.feature_dim = 80;
    recognizer_config.model_config.transducer.encoder = config->encoder;
    recognizer_config.model_config.transducer.decoder = config->decoder;
    recognizer_config.model_config.transducer.joiner = config->joiner;
    recognizer_config.model_config.tokens = config->tokens;
    recognizer_config.model_config.num_threads = config->num_threads > 0 ?
        config->num_threads : 1;
    recognizer_config.model_config.provider = "cpu";
    recognizer_config.decoding_method = "greedy_search";
    recognizer_config.enable_endpoint = 1;
    local->recognizer = SherpaOnnxCreateOnlineRecognizer(&recognizer_config);
    if (!local->recognizer) {
        if (error && size) snprintf(error, size, "无法初始化本地流式识别模型");
        wenzi_local_destroy(local);
        errno = EINVAL;
        return NULL;
    }
    local->stream = SherpaOnnxCreateOnlineStream(local->recognizer);
    if (!local->stream) {
        if (error && size) snprintf(error, size, "无法创建本地识别流");
        wenzi_local_destroy(local);
        errno = ENOMEM;
        return NULL;
    }
    if (config->punctuation_model && *config->punctuation_model) {
        local->punctuation = wenzi_punctuation_create(config->punctuation_model,
            config->num_threads, local->punctuation_error,
            sizeof(local->punctuation_error));
        if (!local->punctuation)
            local->punctuation_error_code = errno ? errno : EINVAL;
    }
    return local;
}

int wenzi_local_feed(wenzi_local *local, const int16_t *samples, size_t count) {
    if (!local || (!samples && count)) return -EINVAL;
    if (local->finished) return -EPIPE;
    if (count > WENZI_FRAME_SAMPLES) return -EMSGSIZE;
    if (!count) return 0;
    float frame[WENZI_FRAME_SAMPLES];
    for (size_t i = 0; i < count; ++i) frame[i] = samples[i] / 32768.0f;
    SherpaOnnxOnlineStreamAcceptWaveform(local->stream, WENZI_SAMPLE_RATE,
                                       frame, (int32_t)count);
    report_punctuation_error(local);
    int rc = decode_pending(local, 0);
    /* PCM 已整块接收；后续失败通过事件报告，避免调用者重送。 */
    if (rc < 0) fail_recognition(local, rc);
    return 0;
}

int wenzi_local_finish(wenzi_local *local) {
    if (!local) return -EINVAL;
    if (local->finished) return 0;
    local->finished = 1;
    report_punctuation_error(local);
    /* 与官方 Zipformer 示例一致，补 0.3 秒尾部。 */
    const float silence[WENZI_FRAME_SAMPLES] = {0};
    for (int i = 0; i < 15; ++i)
        SherpaOnnxOnlineStreamAcceptWaveform(local->stream, WENZI_SAMPLE_RATE,
                                           silence, WENZI_FRAME_SAMPLES);
    SherpaOnnxOnlineStreamInputFinished(local->stream);
    decode_pending(local, 1);
    int rc = publish_result(local, 1);
    if (rc < 0) emit(local, WENZI_ASR_ERROR, "本地识别收尾失败", -rc);
    emit(local, WENZI_ASR_DONE, NULL, -rc);
    return 0;
}

void wenzi_local_destroy(wenzi_local *local) {
    if (!local) return;
    SherpaOnnxDestroyOnlineRecognizerResult(local->previous);
    wenzi_punctuation_destroy(local->punctuation);
    SherpaOnnxDestroyOnlineStream(local->stream);
    SherpaOnnxDestroyOnlineRecognizer(local->recognizer);
    free(local);
}
