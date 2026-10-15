#define _POSIX_C_SOURCE 200809L
#include "wenzi.h"
#include "wenzi_audio.h"
#if WENZI_WITH_NETWORK
#include "wenzi_network.h"
#endif
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PCM_QUEUE_FRAMES 50

typedef struct {
    int16_t samples[WENZI_FRAME_SAMPLES];
    size_t count;
} pcm_frame;

typedef struct {
    wenzi *core;
#if WENZI_WITH_NETWORK
    wenzi_ws_server *server;
#endif
    pthread_t producer;
    pthread_mutex_t mutex;
    pthread_cond_t space;
    pcm_frame queue[PCM_QUEUE_FRAMES];
    size_t head, queued;
    int active, cancel, done, audio_result, failed;
    uint64_t session, audio_session;
    uint64_t captured_samples, overflow_samples;
    size_t peak_queue;
    const char *wav;
    const char *device;
    char error[512];
} application;

static volatile sig_atomic_t interrupted;

static void on_signal(int number) {
    (void)number;
    interrupted = 1;
}

static void json_string(const char *text) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)(text ? text : "");
         *p; ++p) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

static void on_event(void *user, const wenzi_event *event) {
    application *app = user;
    static const char *types[] = {"partial", "final", "error", "status"};
    printf("{\"type\":\"%s\",\"engine\":\"%s\",\"state\":\"%s\","
           "\"session_id\":%llu,\"utterance_id\":%llu,\"timestamp\":%.6f",
           types[event->type], wenzi_engine_name(event->engine),
           wenzi_state_name(event->state),
           (unsigned long long)event->session_id,
           (unsigned long long)event->utterance_id, event->timestamp);
    if (event->type == WENZI_PARTIAL || event->type == WENZI_FINAL) {
        fputs(",\"text\":", stdout);
        json_string(event->text);
    } else if (event->type == WENZI_ERROR) {
        printf(",\"code\":%d,\"message\":", event->code);
        json_string(event->text);
        app->failed = 1;
    }
    puts("}");
    fflush(stdout);
#if WENZI_WITH_NETWORK
    if (app->server) wenzi_ws_publish(app->server, event);
#endif
    if (event->type == WENZI_STATUS) {
        app->session = event->session_id;
        if (event->state != WENZI_RUNNING) {
            pthread_mutex_lock(&app->mutex);
            app->cancel = 1;
            pthread_cond_broadcast(&app->space);
            pthread_mutex_unlock(&app->mutex);
        }
    }
}

static int audio_should_stop(void *user) {
    application *app = user;
    pthread_mutex_lock(&app->mutex);
    /* WAV 可等缓冲空间；麦克风满时由 on_frame 报告断点。 */
    while (app->wav && !app->cancel && app->queued == PCM_QUEUE_FRAMES)
        pthread_cond_wait(&app->space, &app->mutex);
    int cancel = app->cancel;
    pthread_mutex_unlock(&app->mutex);
    return cancel;
}

static int audio_frame(const int16_t *samples, size_t count, void *user) {
    application *app = user;
    if (!samples || !count || count > WENZI_FRAME_SAMPLES) return -1;
    pthread_mutex_lock(&app->mutex);
    app->captured_samples += count;
    int result = 0;
    if (app->cancel) result = 1;
    else if (app->queued == PCM_QUEUE_FRAMES) {
        app->overflow_samples += count;
        snprintf(app->error, sizeof(app->error), "PCM 缓冲已满，录音发生断点");
        result = -1;
    } else {
        pcm_frame *frame = &app->queue[(app->head + app->queued) %
                                        PCM_QUEUE_FRAMES];
        memcpy(frame->samples, samples, count * sizeof(*samples));
        frame->count = count;
        ++app->queued;
        if (app->queued > app->peak_queue) app->peak_queue = app->queued;
    }
    pthread_mutex_unlock(&app->mutex);
    return result;
}

static void *produce_audio(void *user) {
    application *app = user;
    wenzi_audio_callbacks callbacks = {
        .on_frame = audio_frame, .should_stop = audio_should_stop,
        .userdata = app
    };
    char error[512] = "";
    int result = app->wav ?
        wenzi_audio_read_wav(app->wav, &callbacks, error, sizeof(error)) :
        wenzi_audio_capture_alsa(app->device, &callbacks, error, sizeof(error));
    pthread_mutex_lock(&app->mutex);
    app->audio_result = result;
    if (error[0] && !app->error[0])
        snprintf(app->error, sizeof(app->error), "%s", error);
    app->done = 1;
    pthread_cond_broadcast(&app->space);
    pthread_mutex_unlock(&app->mutex);
    return NULL;
}

static int start_audio(application *app) {
    pthread_mutex_lock(&app->mutex);
    app->head = app->queued = 0;
    app->cancel = app->done = 0;
    app->audio_result = 0;
    app->error[0] = 0;
    pthread_mutex_unlock(&app->mutex);
    int result = pthread_create(&app->producer, NULL, produce_audio, app);
    if (result) {
        fprintf(stderr, "创建音频线程失败：%s\n", strerror(result));
        app->failed = 1;
        return -result;
    }
    app->active = 1;
    app->audio_session = app->session;
    return 0;
}

static int take_frame(application *app, pcm_frame *frame) {
    pthread_mutex_lock(&app->mutex);
    int available = app->queued != 0;
    if (available) {
        *frame = app->queue[app->head];
        app->head = (app->head + 1) % PCM_QUEUE_FRAMES;
        --app->queued;
        pthread_cond_signal(&app->space);
    }
    pthread_mutex_unlock(&app->mutex);
    return available;
}

static void stop_audio(application *app, int drain) {
    if (!app->active) return;
    pthread_mutex_lock(&app->mutex);
    app->cancel = 1;
    pthread_cond_broadcast(&app->space);
    pthread_mutex_unlock(&app->mutex);
    pthread_join(app->producer, NULL);
    app->active = 0;
    if (drain) {
        pcm_frame frame;
        while (wenzi_get_state(app->core) == WENZI_RUNNING &&
               take_frame(app, &frame))
            if (wenzi_feed_pcm(app->core, frame.samples, frame.count) < 0)
                break;
    }
    pthread_mutex_lock(&app->mutex);
    app->queued = 0;
    pthread_mutex_unlock(&app->mutex);
}

static void finish_core(application *app) {
    wenzi_stop(app->core);
    while (wenzi_get_state(app->core) == WENZI_DRAINING)
        wenzi_poll(app->core, 10);
}

static int run(application *app, int serve) {
    int finished = 0;
    if (!serve && wenzi_start(app->core) < 0) return 1;
    while (!interrupted) {
#if WENZI_WITH_NETWORK
        if (app->server && wenzi_ws_poll(app->server, 0) < 0) {
            fprintf(stderr, "WebSocket 服务轮询失败\n");
            app->failed = 1;
            break;
        }
#endif
        if (wenzi_poll(app->core, 0) < 0) app->failed = 1;
        wenzi_state state = wenzi_get_state(app->core);
        if (app->active && (state != WENZI_RUNNING ||
                            app->audio_session != app->session))
            stop_audio(app, 0);
        if (state == WENZI_RUNNING && !app->active) {
            if (start_audio(app) < 0) {
                finish_core(app);
                if (!serve) break;
            }
        }
        pcm_frame frame;
        if (state == WENZI_RUNNING && take_frame(app, &frame)) {
            if (wenzi_feed_pcm(app->core, frame.samples, frame.count) < 0)
                app->failed = 1;
            continue;
        }
        pthread_mutex_lock(&app->mutex);
        int audio_done = app->active && app->done && !app->queued;
        int audio_result = app->audio_result;
        pthread_mutex_unlock(&app->mutex);
        if (audio_done) {
            if (audio_result == WENZI_AUDIO_ERROR) {
                wenzi_report_error(app->core,
                                   app->overflow_samples ? EOVERFLOW : EIO,
                                   app->error[0] ? app->error : "音频输入失败");
            }
            stop_audio(app, 0);
            wenzi_finish(app->core, audio_result == WENZI_AUDIO_ERROR ?
                                   WENZI_END_STOP : WENZI_END_EOF);
            finished = 1;
        }
        if (!serve && wenzi_get_state(app->core) == WENZI_IDLE) {
            if (!finished) app->failed = 1;
            break;
        }
        struct timespec delay = {0, 5000000};
        nanosleep(&delay, NULL);
    }
    stop_audio(app, 1);
    finish_core(app);
    return app->failed ? 1 : 0;
}

static int number(const char *text, int min, int max, int *value) {
    char *end;
    errno = 0;
    long n = strtol(text, &end, 10);
    if (errno || !*text || *end || n < min || n > max) return -1;
    *value = (int)n;
    return 0;
}

static const char *model_path(const char *directory, const char *explicit_path,
                               const char *const *names, char *path, size_t size) {
    if (explicit_path) return explicit_path;
    for (size_t i = 0; names[i]; ++i) {
        int n = snprintf(path, size, "%s/%s", directory, names[i]);
        if (n < 0 || (size_t)n >= size) return NULL;
        if (access(path, R_OK) == 0) return path;
    }
    return path;
}

static void usage(FILE *out) {
    fputs("用法：wenzi transcribe --wav FILE [选项]\n"
          "      wenzi transcribe --mic DEVICE [选项]\n"
          "      wenzi serve [--mic DEVICE | --wav FILE] [选项]\n"
          "选项：--model-dir DIR --encoder FILE --decoder FILE --joiner FILE\n"
          "      --tokens FILE --punctuation FILE --threads N\n"
          "      --engine local|cloud --bind ADDRESS --port N\n"
          "      --iflytek-url URL --ca-file FILE --drain-timeout-ms N\n"
          "云端凭据：WENZI_IFLYTEK_APP_ID、WENZI_IFLYTEK_API_KEY\n", out);
}

int main(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        usage(stdout);
        return 0;
    }
    if (argc < 2 || (strcmp(argv[1], "transcribe") && strcmp(argv[1], "serve"))) {
        usage(stderr);
        return 2;
    }
    int serve = !strcmp(argv[1], "serve");
    wenzi_config config = {.engine = WENZI_LOCAL, .num_threads = 1,
                            .drain_timeout_ms = 10000};
    application app = {.device = "default"};
    const char *directory = getenv("WENZI_MODEL_DIR");
    const char *address = "127.0.0.1", *url = NULL, *ca_file = NULL;
    int port = 8765, mic_selected = 0;
    enum { OPT_ENCODER = 256, OPT_DECODER, OPT_JOINER, OPT_TOKENS, OPT_PUNCT,
           OPT_ENGINE, OPT_BIND, OPT_PORT, OPT_THREADS, OPT_URL, OPT_CA,
           OPT_DRAIN };
    static const struct option options[] = {
        {"wav", required_argument, NULL, 'w'},
        {"mic", required_argument, NULL, 'm'},
        {"model-dir", required_argument, NULL, 'd'},
        {"encoder", required_argument, NULL, OPT_ENCODER},
        {"decoder", required_argument, NULL, OPT_DECODER},
        {"joiner", required_argument, NULL, OPT_JOINER},
        {"tokens", required_argument, NULL, OPT_TOKENS},
        {"punctuation", required_argument, NULL, OPT_PUNCT},
        {"engine", required_argument, NULL, OPT_ENGINE},
        {"bind", required_argument, NULL, OPT_BIND},
        {"port", required_argument, NULL, OPT_PORT},
        {"threads", required_argument, NULL, OPT_THREADS},
        {"iflytek-url", required_argument, NULL, OPT_URL},
        {"ca-file", required_argument, NULL, OPT_CA},
        {"drain-timeout-ms", required_argument, NULL, OPT_DRAIN},
        {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}
    };
    optind = 2;
    int option;
    while ((option = getopt_long(argc, argv, "w:m:d:h", options, NULL)) != -1) {
        switch (option) {
        case 'w': app.wav = optarg; break;
        case 'm': app.device = optarg; mic_selected = 1; break;
        case 'd': directory = optarg; break;
        case OPT_ENCODER: config.encoder = optarg; break;
        case OPT_DECODER: config.decoder = optarg; break;
        case OPT_JOINER: config.joiner = optarg; break;
        case OPT_TOKENS: config.tokens = optarg; break;
        case OPT_PUNCT: config.punctuation_model = optarg; break;
        case OPT_ENGINE:
            if (!strcmp(optarg, "local")) config.engine = WENZI_LOCAL;
            else if (!strcmp(optarg, "cloud")) config.engine = WENZI_CLOUD;
            else goto invalid;
            break;
        case OPT_BIND: address = optarg; break;
        case OPT_PORT: if (number(optarg, 0, 65535, &port)) goto invalid; break;
        case OPT_THREADS:
            if (number(optarg, 1, 128, &config.num_threads)) goto invalid;
            break;
        case OPT_URL: url = optarg; break;
        case OPT_CA: ca_file = optarg; break;
        case OPT_DRAIN:
            if (number(optarg, 1, 600000, &config.drain_timeout_ms)) goto invalid;
            break;
        case 'h': usage(stdout); return 0;
        default: goto invalid;
        }
    }
    if (optind != argc || (app.wav && mic_selected) ||
        (!serve && !app.wav && !mic_selected)) goto invalid;
    char encoder[1024], decoder[1024], joiner[1024], tokens[1024];
    static const char *const encoder_names[] = {
        "encoder.int8.onnx", "encoder-epoch-99-avg-1.int8.onnx",
        "encoder.onnx", "encoder-epoch-99-avg-1.onnx", NULL};
    static const char *const decoder_names[] = {
        "decoder.onnx", "decoder-epoch-99-avg-1.onnx",
        "decoder.int8.onnx", "decoder-epoch-99-avg-1.int8.onnx", NULL};
    static const char *const joiner_names[] = {
        "joiner.int8.onnx", "joiner-epoch-99-avg-1.int8.onnx",
        "joiner.onnx", "joiner-epoch-99-avg-1.onnx", NULL};
    static const char *const token_names[] = {"tokens.txt", NULL};
    if (!directory || !*directory) directory = "models/asr";
    config.encoder = model_path(directory, config.encoder, encoder_names,
                                encoder, sizeof(encoder));
    config.decoder = model_path(directory, config.decoder, decoder_names,
                                decoder, sizeof(decoder));
    config.joiner = model_path(directory, config.joiner, joiner_names,
                               joiner, sizeof(joiner));
    config.tokens = model_path(directory, config.tokens, token_names,
                               tokens, sizeof(tokens));
    if (!config.encoder || !config.decoder || !config.joiner || !config.tokens)
        goto invalid;
    if (!config.punctuation_model)
        config.punctuation_model = getenv("WENZI_PUNCTUATION_MODEL");
#if WENZI_WITH_NETWORK
    struct wenzi_iflytek_config cloud = {
        .url = url, .ca_file = ca_file, .connect_timeout_ms = 10000,
        .drain_timeout_ms = config.drain_timeout_ms
    };
    config.cloud = &cloud;
#else
    (void)address; (void)port; (void)url; (void)ca_file;
    if (serve || config.engine == WENZI_CLOUD) {
        fprintf(stderr, "此构建未启用云端和 WebSocket\n");
        return 1;
    }
#endif
    pthread_mutex_init(&app.mutex, NULL);
    pthread_cond_init(&app.space, NULL);
    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    app.core = wenzi_create(&config, on_event, &app);
    if (!app.core) {
        fprintf(stderr, "创建识别核心失败：%s\n", strerror(errno));
        pthread_cond_destroy(&app.space);
        pthread_mutex_destroy(&app.mutex);
        return 1;
    }
    int result = 0;
#if WENZI_WITH_NETWORK
    if (serve) {
        char error[512] = "";
        app.server = wenzi_ws_create(app.core, address, port, error, sizeof(error));
        if (!app.server) {
            fprintf(stderr, "创建 WebSocket 服务失败：%s\n", error);
            result = 1;
        } else fprintf(stderr, "WebSocket 服务：%s:%d\n", address,
                        wenzi_ws_port(app.server));
    }
#endif
    if (!result) result = run(&app, serve);
    wenzi_statistics statistics = {0};
    wenzi_get_statistics(app.core, &statistics);
    uint64_t undelivered = app.captured_samples > statistics.accepted_samples ?
                           app.captured_samples - statistics.accepted_samples : 0;
    fprintf(stderr, "wenzi_statistics {\"captured_samples\":%llu,"
            "\"accepted_samples\":%llu,\"audio_seconds\":%.6f,"
            "\"local_samples\":%llu,\"local_processing_seconds\":%.6f,"
            "\"local_initialization_seconds\":%.6f,\"peak_pcm_queue_frames\":%zu,"
            "\"undelivered_samples\":%llu,\"overflow_samples\":%llu}\n",
            (unsigned long long)app.captured_samples,
            (unsigned long long)statistics.accepted_samples,
            statistics.accepted_samples / (double)WENZI_SAMPLE_RATE,
            (unsigned long long)statistics.local_samples,
            statistics.local_processing_ns / 1000000000.0,
            statistics.local_initialization_ns / 1000000000.0,
            app.peak_queue, (unsigned long long)undelivered,
            (unsigned long long)app.overflow_samples);
#if WENZI_WITH_NETWORK
    wenzi_ws_destroy(app.server);
#endif
    wenzi_destroy(app.core);
    pthread_cond_destroy(&app.space);
    pthread_mutex_destroy(&app.mutex);
    return result;
invalid:
    usage(stderr);
    return 2;
}
