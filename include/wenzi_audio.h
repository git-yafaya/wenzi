#ifndef WENZI_AUDIO_H
#define WENZI_AUDIO_H

#include <stddef.h>
#include <stdint.h>

enum {
    WENZI_AUDIO_SAMPLE_RATE = 16000,
    WENZI_AUDIO_FRAME_SAMPLES = 320,
    WENZI_AUDIO_OK = 0,
    WENZI_AUDIO_STOPPED = 1,
    WENZI_AUDIO_ERROR = -1
};

typedef struct {
    /* 单声道本机字节序 PCM；样本只在回调期间有效。 */
    /* 返回 0 继续、正数停止、负数失败；不得等待推理或网络。 */
    int (*on_frame)(const int16_t *samples, size_t count, void *userdata);
    /* 返回非零取消；跨线程读取的停止状态由调用方同步。 */
    int (*should_stop)(void *userdata);
    void *userdata;
} wenzi_audio_callbacks;

/* 同步运行；EOF 返回 OK，取消返回 STOPPED，失败返回 ERROR。 */
/* 取消丢弃尚未提交的样本；停止返回后不再调用回调。 */
/* 错误文字写入 error；允许 error 为 NULL 或 error_size 为 0。 */
int wenzi_audio_read_wav(const char *path,
                         const wenzi_audio_callbacks *callbacks,
                         char *error, size_t error_size);

/* NULL/空设备名使用 default；plughw 可用 ALSA 转换格式。 */
/* xrun 结束当前流并报告数据断点。 */
int wenzi_audio_capture_alsa(const char *device,
                            const wenzi_audio_callbacks *callbacks,
                            char *error, size_t error_size);

#endif
