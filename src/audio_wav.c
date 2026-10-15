#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "wenzi_audio.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int fail(char *error, size_t size, const char *message)
{
    if (error && size)
        snprintf(error, size, "%s", message);
    return WENZI_AUDIO_ERROR;
}

static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)((unsigned int)p[0] | (unsigned int)p[1] << 8);
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int cancelled(const wenzi_audio_callbacks *callbacks)
{
    return callbacks->should_stop && callbacks->should_stop(callbacks->userdata);
}

int wenzi_audio_read_wav(const char *path,
                         const wenzi_audio_callbacks *callbacks,
                         char *error, size_t error_size)
{
    FILE *file;
    unsigned char header[16], bytes[WENZI_AUDIO_FRAME_SAMPLES * 2];
    int16_t samples[WENZI_AUDIO_FRAME_SAMPLES];
    uint64_t riff_end, position, data_offset = 0;
    uint32_t data_size = 0;
    off_t file_size;
    int found_format = 0, found_data = 0, result = WENZI_AUDIO_ERROR;
    struct timespec deadline;

    if (error && error_size)
        error[0] = '\0';
    if (!path || !*path || !callbacks || !callbacks->on_frame)
        return fail(error, error_size, "WAV 路径或音频回调无效");
    if (cancelled(callbacks))
        return WENZI_AUDIO_STOPPED;
    file = fopen(path, "rb");
    if (!file)
        return fail(error, error_size, "无法打开 WAV 文件");

    /* 先核对文件边界，截断文件不能被当成正常 EOF。 */
    if (fseeko(file, 0, SEEK_END) || (file_size = ftello(file)) < 0 ||
        fseeko(file, 0, SEEK_SET) || fread(header, 1, 12, file) != 12) {
        fail(error, error_size, "WAV 文件不可定位或文件头截断");
        goto done;
    }
    if (memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) {
        fail(error, error_size, "只支持 RIFF/WAVE 文件");
        goto done;
    }
    riff_end = (uint64_t)le32(header + 4) + 8;
    if (riff_end < 12 || riff_end > (uint64_t)file_size) {
        fail(error, error_size, "WAV RIFF 长度无效或文件截断");
        goto done;
    }

    /* chunk 可以带扩展数据，也可以按不同顺序出现。 */
    for (position = 12; position < riff_end;) {
        uint32_t chunk_size;
        uint64_t next;
        if (cancelled(callbacks)) {
            result = WENZI_AUDIO_STOPPED;
            goto done;
        }
        if (riff_end - position < 8 || fread(header, 1, 8, file) != 8) {
            fail(error, error_size, "WAV chunk 文件头截断");
            goto done;
        }
        chunk_size = le32(header + 4);
        next = position + 8 + (uint64_t)chunk_size + (chunk_size & 1u);
        if (next > riff_end) {
            fail(error, error_size, "WAV chunk 长度超过 RIFF 边界");
            goto done;
        }
        if (!memcmp(header, "fmt ", 4)) {
            if (found_format || chunk_size < 16 || fread(header, 1, 16, file) != 16) {
                fail(error, error_size, "WAV fmt chunk 无效或重复");
                goto done;
            }
            if (le16(header) != 1 || le16(header + 2) != 1 ||
                le32(header + 4) != WENZI_AUDIO_SAMPLE_RATE ||
                le16(header + 14) != 16) {
                fail(error, error_size, "只支持 16kHz 单声道 PCM16 WAV");
                goto done;
            }
            if (le16(header + 12) != 2 || le32(header + 8) != 32000) {
                fail(error, error_size, "WAV PCM 对齐或字节速率无效");
                goto done;
            }
            found_format = 1;
        } else if (!memcmp(header, "data", 4)) {
            if (found_data || (chunk_size & 1u)) {
                fail(error, error_size, "WAV data chunk 重复或 PCM 样本不完整");
                goto done;
            }
            found_data = 1;
            data_offset = position + 8;
            data_size = chunk_size;
        }
        if (fseeko(file, (off_t)next, SEEK_SET)) {
            fail(error, error_size, "无法定位 WAV chunk");
            goto done;
        }
        position = next;
    }
    if (!found_format || !found_data) {
        fail(error, error_size, "WAV 缺少 fmt 或 data chunk");
        goto done;
    }
    if (fseeko(file, (off_t)data_offset, SEEK_SET) ||
        clock_gettime(CLOCK_MONOTONIC, &deadline)) {
        fail(error, error_size, "无法开始 WAV 流式读取");
        goto done;
    }

    while (data_size) {
        size_t size = data_size < sizeof(bytes) ? data_size : sizeof(bytes);
        size_t count = size / 2;
        int wait_result, accepted;
        if (cancelled(callbacks)) {
            result = WENZI_AUDIO_STOPPED;
            goto done;
        }
        if (fread(bytes, 1, size, file) != size) {
            fail(error, error_size, "WAV PCM 数据读取失败或截断");
            goto done;
        }
        for (size_t i = 0; i < count; ++i) {
            uint16_t value = le16(bytes + i * 2);
            samples[i] = (int16_t)(value < 32768u ? (int32_t)value : (int32_t)value - 65536);
        }

        /* 绝对单调时钟避免把回调耗时累加到每帧间隔。 */
        deadline.tv_nsec += (long)count * 62500L;
        if (deadline.tv_nsec >= 1000000000L) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000L;
        }
        do {
            if (cancelled(callbacks)) {
                result = WENZI_AUDIO_STOPPED;
                goto done;
            }
            wait_result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        } while (wait_result == EINTR);
        if (wait_result) {
            fail(error, error_size, "WAV 实时节奏等待失败");
            goto done;
        }
        if (cancelled(callbacks)) {
            result = WENZI_AUDIO_STOPPED;
            goto done;
        }
        accepted = callbacks->on_frame(samples, count, callbacks->userdata);
        if (accepted) {
            result = accepted > 0 ? WENZI_AUDIO_STOPPED :
                     fail(error, error_size, "音频接收方拒绝 PCM 数据");
            goto done;
        }
        data_size -= (uint32_t)size;
    }
    result = cancelled(callbacks) ? WENZI_AUDIO_STOPPED : WENZI_AUDIO_OK;

done:
    fclose(file);
    return result;
}
