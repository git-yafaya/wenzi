#define _POSIX_C_SOURCE 200809L

#include "wenzi_audio.h"

#include <alloca.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdio.h>

static int alsa_error(char *error, size_t size, const char *message, int code)
{
    if (code == -EPIPE)
        message = "录音缓冲溢出，发生音频断点";
    else if (code == -ESTRPIPE)
        message = "录音设备挂起，音频中断";
    else if (code == -ENODEV)
        message = "录音设备已拔出或不可用";
    if (error && size) {
        if (code < 0)
            snprintf(error, size, "%s：%s", message, snd_strerror(code));
        else
            snprintf(error, size, "%s", message);
    }
    return WENZI_AUDIO_ERROR;
}

int wenzi_audio_capture_alsa(const char *device,
                            const wenzi_audio_callbacks *callbacks,
                            char *error, size_t error_size)
{
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *params;
    snd_pcm_format_t format = SND_PCM_FORMAT_S16, actual_format;
    snd_pcm_access_t access;
    snd_pcm_uframes_t buffer_size, period_size;
    unsigned int rate, channels;
    int16_t frame[WENZI_AUDIO_FRAME_SAMPLES];
    size_t filled = 0;
    int rc, result = WENZI_AUDIO_ERROR;

    if (error && error_size)
        error[0] = '\0';
    if (!callbacks || !callbacks->on_frame)
        return alsa_error(error, error_size, "录音缺少采样回调", 0);
    if (callbacks->should_stop && callbacks->should_stop(callbacks->userdata))
        return WENZI_AUDIO_STOPPED;

    rc = snd_pcm_open(&pcm, device && *device ? device : "default",
                      SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (rc < 0)
        return alsa_error(error, error_size, "无法打开录音设备", rc);

    snd_pcm_hw_params_alloca(&params);
    if ((rc = snd_pcm_hw_params_any(pcm, params)) < 0) {
        alsa_error(error, error_size, "无法查询录音设备格式", rc);
        goto done;
    }
    /* 优先本机字节序；大端主机也可读取设备的小端 S16。 */
    if (snd_pcm_hw_params_test_format(pcm, params, format) < 0) {
        if (format == SND_PCM_FORMAT_S16_LE ||
            snd_pcm_hw_params_test_format(pcm, params, SND_PCM_FORMAT_S16_LE) < 0) {
            alsa_error(error, error_size, "设备不支持本机或小端 S16 录音", 0);
            goto done;
        }
        format = SND_PCM_FORMAT_S16_LE;
    }
    /* plug PCM 可用现有 ALSA 转换；原生 hw 不额外造转换器。 */
    /* 以 80 ms 起调，缓冲由设备协商。 */
    rc = snd_pcm_set_params(pcm, format, SND_PCM_ACCESS_RW_INTERLEAVED,
                            1, WENZI_AUDIO_SAMPLE_RATE, 1, 80000);
    if (rc < 0) {
        alsa_error(error, error_size, "无法设置 16 kHz 单声道录音", rc);
        goto done;
    }
    if ((rc = snd_pcm_hw_params_current(pcm, params)) < 0 ||
        (rc = snd_pcm_hw_params_get_rate(params, &rate, NULL)) < 0 ||
        (rc = snd_pcm_hw_params_get_channels(params, &channels)) < 0 ||
        (rc = snd_pcm_hw_params_get_format(params, &actual_format)) < 0 ||
        (rc = snd_pcm_hw_params_get_access(params, &access)) < 0 ||
        (rc = snd_pcm_get_params(pcm, &buffer_size, &period_size)) < 0) {
        alsa_error(error, error_size, "无法核验录音参数", rc);
        goto done;
    }
    if (rate != WENZI_AUDIO_SAMPLE_RATE || channels != 1 ||
        actual_format != format || access != SND_PCM_ACCESS_RW_INTERLEAVED) {
        alsa_error(error, error_size, "设备实际参数不符合 16 kHz 单声道 S16", 0);
        goto done;
    }
    if (!period_size || period_size > buffer_size) {
        alsa_error(error, error_size, "录音实际缓冲或周期无效", 0);
        goto done;
    }
    if (callbacks->should_stop && callbacks->should_stop(callbacks->userdata)) {
        result = WENZI_AUDIO_STOPPED;
        goto done;
    }
    if ((rc = snd_pcm_start(pcm)) < 0) {
        alsa_error(error, error_size, "无法启动录音", rc);
        goto done;
    }

    for (;;) {
        snd_pcm_sframes_t count;

        if (callbacks->should_stop && callbacks->should_stop(callbacks->userdata)) {
            result = WENZI_AUDIO_STOPPED;
            break;
        }
        count = snd_pcm_readi(pcm, frame + filled,
                              WENZI_AUDIO_FRAME_SAMPLES - filled);
        if (count == -EINTR)
            continue;
        if (count == -EAGAIN || count == 0) {
            /* 有限等待保证停止状态最多在 100 ms 后再次检查。 */
            rc = snd_pcm_wait(pcm, 100);
            if (rc >= 0 || rc == -EINTR)
                continue;
            alsa_error(error, error_size, "等待录音数据失败", rc);
            break;
        }
        if (count < 0) {
            alsa_error(error, error_size, "读取录音数据失败", (int)count);
            break;
        }
        filled += (size_t)count;
        if (callbacks->should_stop && callbacks->should_stop(callbacks->userdata)) {
            result = WENZI_AUDIO_STOPPED;
            break;
        }
        if (filled != WENZI_AUDIO_FRAME_SAMPLES)
            continue;
        if (format != SND_PCM_FORMAT_S16) {
            unsigned char *bytes = (unsigned char *)frame;
            for (size_t i = 0; i < sizeof(frame); i += 2) {
                unsigned char first = bytes[i];
                bytes[i] = bytes[i + 1];
                bytes[i + 1] = first;
            }
        }
        rc = callbacks->on_frame(frame, filled, callbacks->userdata);
        if (rc != 0) {
            result = rc > 0 ? WENZI_AUDIO_STOPPED :
                alsa_error(error, error_size, "录音采样回调失败", 0);
            break;
        }
        filled = 0;
    }

done:
    /* 取消丢弃设备数据及未交付的短尾；断点不恢复、不拼接旧流。 */
    rc = snd_pcm_drop(pcm);
    if (rc < 0 && result == WENZI_AUDIO_STOPPED)
        result = alsa_error(error, error_size, "停止录音设备失败", rc);
    rc = snd_pcm_close(pcm);
    if (rc < 0 && result == WENZI_AUDIO_STOPPED)
        result = alsa_error(error, error_size, "关闭录音设备失败", rc);
    return result;
}
