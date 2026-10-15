#define _POSIX_C_SOURCE 200809L

#include "wenzi_audio.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum { EXTRA = 1, TRUNCATED = 2, UNSUPPORTED = 4, ODD_DATA = 8, DATA_FIRST = 16 };

typedef struct {
    size_t samples, frames, sizes[3];
    int stop_after, reject;
} receiver;

static volatile sig_atomic_t stopped;

static void stop_signal(int signal_number)
{
    (void)signal_number;
    stopped = 1;
}

static int should_stop(void *userdata)
{
    (void)userdata;
    return stopped != 0;
}

static int16_t sample_at(size_t index)
{
    static const int16_t values[] = { INT16_MIN, -1, 0, INT16_MAX };
    return values[index % 4];
}

static int receive(const int16_t *samples, size_t count, void *userdata)
{
    receiver *state = userdata;
    assert(count > 0 && count <= WENZI_AUDIO_FRAME_SAMPLES);
    assert(state->frames < sizeof(state->sizes) / sizeof(state->sizes[0]));
    for (size_t i = 0; i < count; ++i)
        assert(samples[i] == sample_at(state->samples + i));
    state->sizes[state->frames++] = count;
    state->samples += count;
    if (state->reject)
        return -1;
    return state->stop_after && state->frames >= (size_t)state->stop_after;
}

static void write_u16(FILE *file, uint16_t value)
{
    unsigned char bytes[] = { (unsigned char)value, (unsigned char)(value >> 8) };
    assert(fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes));
}

static void write_u32(FILE *file, uint32_t value)
{
    write_u16(file, (uint16_t)value);
    write_u16(file, (uint16_t)(value >> 16));
}

static void write_format(FILE *file, int flags)
{
    assert(fwrite("fmt ", 1, 4, file) == 4);
    write_u32(file, 18);
    write_u16(file, flags & UNSUPPORTED ? 3 : 1);
    write_u16(file, 1);
    write_u32(file, WENZI_AUDIO_SAMPLE_RATE);
    write_u32(file, 32000);
    write_u16(file, 2);
    write_u16(file, 16);
    write_u16(file, 0);
}

static void write_data(FILE *file, size_t count, int flags)
{
    assert(fwrite("data", 1, 4, file) == 4);
    write_u32(file, (uint32_t)(count * 2 + !!(flags & ODD_DATA)));
    for (size_t i = 0; i < count; ++i)
        write_u16(file, (uint16_t)sample_at(i));
    if (flags & ODD_DATA) {
        assert(fputc(0, file) != EOF);
        assert(fputc(0, file) != EOF);
    }
}

static void make_wav(const char *path, size_t count, int flags)
{
    FILE *file = fopen(path, "w+b");
    long length;
    assert(file);
    assert(fwrite("RIFF", 1, 4, file) == 4);
    write_u32(file, 0);
    assert(fwrite("WAVE", 1, 4, file) == 4);
    if (flags & EXTRA) {
        assert(fwrite("JUNK", 1, 4, file) == 4);
        write_u32(file, 3);
        assert(fwrite("abc\0", 1, 4, file) == 4);
    }
    if (flags & DATA_FIRST)
        write_data(file, count, flags);
    write_format(file, flags);
    if (!(flags & DATA_FIRST))
        write_data(file, count, flags);
    if (flags & EXTRA) {
        assert(fwrite("LIST", 1, 4, file) == 4);
        write_u32(file, 4);
        assert(fwrite("tail", 1, 4, file) == 4);
    }
    length = ftell(file);
    assert(length >= 12 && fseek(file, 4, SEEK_SET) == 0);
    write_u32(file, (uint32_t)(length - 8));
    assert(fflush(file) == 0);
    if (flags & TRUNCATED)
        assert(ftruncate(fileno(file), length - 1) == 0);
    assert(fclose(file) == 0);
}

static double now(void)
{
    struct timespec time;
    assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (double)time.tv_sec + (double)time.tv_nsec / 1000000000.0;
}

#ifdef WENZI_AUDIO_TEST_ALSA
static double mic_deadline;
static size_t mic_frames, mic_samples;
static int mic_peak;

static int receive_mic(const int16_t *samples, size_t count, void *userdata)
{
    (void)userdata;
    ++mic_frames;
    mic_samples += count;
    for (size_t i = 0; i < count; ++i) {
        int magnitude = samples[i] < 0 ? -(int)samples[i] : samples[i];
        if (magnitude > mic_peak)
            mic_peak = magnitude;
    }
    return 0;
}

static int stop_mic(void *userdata)
{
    (void)userdata;
    return stopped || now() >= mic_deadline;
}

static int check_mic(const char *device)
{
    char error[256];
    wenzi_audio_callbacks callbacks = { receive_mic, stop_mic, NULL };
    int result;
    mic_deadline = now() + 2.0;
    result = wenzi_audio_capture_alsa(device, &callbacks, error, sizeof(error));
    printf("device=%s frames=%zu samples=%zu peak=%d result=%d error=%s\n",
           device, mic_frames, mic_samples, mic_peak, result, error);
    return result == WENZI_AUDIO_STOPPED && mic_frames > 0 ? 0 : 1;
}
#endif

int main(int argc, char **argv)
{
    char path[] = "/tmp/wenzi-audio-test-XXXXXX", error[160];
    int fd;
    receiver state = {0};
    wenzi_audio_callbacks callbacks = { receive, should_stop, &state };
    double start;
#ifdef WENZI_AUDIO_TEST_ALSA
    /* 仅显式传 --mic 时打开设备，普通测试始终使用 WAV。 */
    if (argc >= 2 && !strcmp(argv[1], "--mic"))
        return check_mic(argc >= 3 ? argv[2] : "default");
#else
    (void)argv;
#endif
    if (argc != 1) {
        fputs("usage: test_audio [--mic DEVICE]\n", stderr);
        return 2;
    }
    fd = mkstemp(path);
    assert(fd >= 0 && close(fd) == 0);

    /* 同时核对额外 chunk、padding、扩展 fmt 和短末帧。 */
    make_wav(path, 641, EXTRA);
    start = now();
    assert(wenzi_audio_read_wav(path, &callbacks, error, sizeof(error)) == WENZI_AUDIO_OK);
    assert(!error[0] && state.samples == 641 && state.frames == 3);
    assert(state.sizes[0] == 320 && state.sizes[1] == 320 && state.sizes[2] == 1);
    assert(now() - start >= 641.0 / 16000.0 - 0.002);

    state = (receiver){0};
    make_wav(path, 1, DATA_FIRST);
    assert(wenzi_audio_read_wav(path, &callbacks, NULL, 0) == WENZI_AUDIO_OK);
    assert(state.samples == 1 && state.frames == 1);

    state = (receiver){0};
    make_wav(path, 0, 0);
    assert(wenzi_audio_read_wav(path, &callbacks, error, sizeof(error)) == WENZI_AUDIO_OK);
    assert(state.frames == 0);

    for (int flags = TRUNCATED; flags <= ODD_DATA; flags *= 2) {
        make_wav(path, 321, flags);
        assert(wenzi_audio_read_wav(path, &callbacks, error, sizeof(error)) == WENZI_AUDIO_ERROR);
        assert(error[0] && state.frames == 0);
    }

    /* 已经停止、回调取消和信号打断等待都不能继续交帧。 */
    make_wav(path, 641, 0);
    stopped = 1;
    assert(wenzi_audio_read_wav(path, &callbacks, error, sizeof(error)) == WENZI_AUDIO_STOPPED);
    assert(!error[0] && state.frames == 0);
    stopped = 0;
    state.stop_after = 1;
    assert(wenzi_audio_read_wav(path, &callbacks, error, sizeof(error)) == WENZI_AUDIO_STOPPED);
    assert(state.frames == 1 && state.samples == 320);

    state = (receiver){ .reject = 1 };
    assert(wenzi_audio_read_wav(path, &callbacks, error, sizeof(error)) == WENZI_AUDIO_ERROR);
    assert(error[0] && state.frames == 1);

    {
        struct sigaction action = {0}, previous;
        struct itimerval timer = { .it_value = { .tv_usec = 5000 } };
        action.sa_handler = stop_signal;
        assert(sigemptyset(&action.sa_mask) == 0);
        assert(sigaction(SIGALRM, &action, &previous) == 0);
        state = (receiver){0};
        assert(setitimer(ITIMER_REAL, &timer, NULL) == 0);
        assert(wenzi_audio_read_wav(path, &callbacks, error, sizeof(error)) == WENZI_AUDIO_STOPPED);
        assert(stopped && !error[0] && state.frames == 0);
        timer = (struct itimerval){0};
        assert(setitimer(ITIMER_REAL, &timer, NULL) == 0);
        assert(sigaction(SIGALRM, &previous, NULL) == 0);
    }

    assert(unlink(path) == 0);
    puts("audio WAV checks passed");
    return 0;
}
