#define _POSIX_C_SOURCE 200809L
#include "wenzi_network.h"
#include "network_json.h"
#include <errno.h>
#include <limits.h>
#include <libwebsockets.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { AUDIO_CAPACITY = 16000 * 2 * 5, MESSAGE_CAPACITY = 65536,
       TEXT_CAPACITY = 32768, FRAME_BYTES = 1280 };

struct wenzi_iflytek {
    struct lws_context *context;
    struct lws *socket;
    wenzi_asr_callback callback;
    void *user;
    unsigned char audio[AUDIO_CAPACITY];
    size_t head, queued, received;
    char message[MESSAGE_CAPACITY + 1];
    char partial[TEXT_CAPACITY], text[TEXT_CAPACITY];
    char host[256], path[2048];
    int port, ssl, ready, finishing, end_sent, done, sent_audio, peer_finished;
    int connect_timeout_ms, drain_timeout_ms;
    int64_t connect_deadline, drain_deadline, next_send;
    int64_t last_final_sequence;
};

static int64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void result(wenzi_iflytek *client, wenzi_asr_type type,
                   const char *text, int code) {
    wenzi_asr_result event = { type, text, code };
    if (client->callback) client->callback(client->user, &event);
}

static void complete(wenzi_iflytek *client, int code, const char *message) {
    if (client->done) return;
    client->done = 1;
    if (code) result(client, WENZI_ASR_ERROR, message, code);
    result(client, WENZI_ASR_DONE, "", 0);
}

int wenzi_iflytek_signature(const char *app_id, const char *api_key, time_t ts,
                            char *signature, size_t size) {
    char input[256], hex[33];
    unsigned char md5[EVP_MAX_MD_SIZE], mac[EVP_MAX_MD_SIZE];
    unsigned int md5_size = 0, mac_size = 0;
    if (!app_id || !api_key || !signature || size < 29 ||
        strlen(api_key) > INT_MAX) return -EINVAL;
    int length = snprintf(input, sizeof(input), "%s%lld", app_id, (long long)ts);
    if (length < 0 || (size_t)length >= sizeof(input)) return -E2BIG;
    if (!EVP_Digest(input, (size_t)length, md5, &md5_size, EVP_md5(), NULL) ||
        md5_size != 16) return -EIO;
    for (unsigned int i = 0; i < md5_size; ++i)
        snprintf(hex + i * 2, 3, "%02x", md5[i]);
    if (!HMAC(EVP_sha1(), api_key, (int)strlen(api_key),
              (const unsigned char *)hex, 32, mac, &mac_size) || mac_size != 20)
        return -EIO;
    EVP_EncodeBlock((unsigned char *)signature, mac, (int)mac_size);
    return 0;
}

static int url_encode(const char *input, char *output, size_t size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p; ++p) {
        int plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || strchr("-._~", *p);
        size_t needed = plain ? 1 : 3;
        if (used + needed >= size) return -E2BIG;
        if (plain) output[used++] = (char)*p;
        else {
            output[used++] = '%';
            output[used++] = hex[*p >> 4];
            output[used++] = hex[*p & 15];
        }
    }
    output[used] = '\0';
    return 0;
}

static int number(const char *value, int64_t *output) {
    char *end;
    if (!value || !*value) return -EINVAL;
    errno = 0;
    long long n = strtoll(value, &end, 10);
    if (errno || *end) return -EINVAL;
    *output = n;
    return 0;
}

struct parsed_result {
    wenzi_iflytek *client;
    char action[16];
    int code_seen, action_seen, data_seen, type_seen, sequence_seen;
    int64_t code, type, sequence;
    size_t text_size;
};

static int parse_inner(void *user, const struct lejp_ctx *ctx,
                       unsigned char reason, const char *value, size_t size) {
    struct parsed_result *parsed = user;
    int scalar = reason == LEJPCB_VAL_STR_END || reason == LEJPCB_VAL_NUM_INT;
    if (!strcmp(ctx->path, "cn.st.type") && scalar) {
        if (parsed->type_seen++ || number(value, &parsed->type)) return -EINVAL;
    } else if (!strcmp(ctx->path, "seg_id") && reason == LEJPCB_VAL_NUM_INT) {
        if (parsed->sequence_seen++ || number(value, &parsed->sequence) ||
            parsed->sequence < 0) return -EINVAL;
    } else if (!strcmp(ctx->path, "cn.st.rt[].ws[].cw[].w") &&
               reason == LEJPCB_VAL_STR_END) {
        if (size >= TEXT_CAPACITY - parsed->text_size) return -E2BIG;
        memcpy(parsed->client->text + parsed->text_size, value, size);
        parsed->text_size += size;
        parsed->client->text[parsed->text_size] = '\0';
    }
    return 0;
}

static int parse_outer(void *user, const struct lejp_ctx *ctx,
                       unsigned char reason, const char *value, size_t size) {
    struct parsed_result *parsed = user;
    if (!strcmp(ctx->path, "action") && reason == LEJPCB_VAL_STR_END) {
        if (parsed->action_seen++ || size >= sizeof(parsed->action)) return -EINVAL;
        memcpy(parsed->action, value, size + 1);
    } else if (!strcmp(ctx->path, "code") &&
               (reason == LEJPCB_VAL_STR_END || reason == LEJPCB_VAL_NUM_INT)) {
        if (parsed->code_seen++ || number(value, &parsed->code)) return -EINVAL;
    } else if (!strcmp(ctx->path, "data") && reason == LEJPCB_VAL_STR_END) {
        if (parsed->data_seen++) return -EINVAL;
        if (!size) return 0;
        char *scratch = malloc(size + 1);
        if (!scratch) return -ENOMEM;
        int rc = wenzi_json_parse(value, size, scratch, size + 1, parse_inner, parsed);
        free(scratch);
        return rc;
    }
    return 0;
}

static int receive_result(wenzi_iflytek *client) {
    struct parsed_result parsed = { .client = client };
    char *scratch = malloc(client->received + 1);
    if (!scratch) return -ENOMEM;
    client->text[0] = '\0';
    int rc = wenzi_json_parse(client->message, client->received, scratch,
                              client->received + 1, parse_outer, &parsed);
    free(scratch);
    if (rc || !parsed.action_seen || !parsed.code_seen) return rc ? rc : -EPROTO;
    if (!strcmp(parsed.action, "error") || parsed.code) {
        complete(client, -EIO, "讯飞返回识别错误");
        return -EIO;
    }
    if (!strcmp(parsed.action, "started")) {
        client->ready = 1;
        client->next_send = monotonic_ms();
        return 0;
    }
    if (strcmp(parsed.action, "result") || !client->ready || !parsed.data_seen ||
        !parsed.type_seen || !parsed.sequence_seen ||
        (parsed.type != 0 && parsed.type != 1)) return -EPROTO;
    if (parsed.sequence <= client->last_final_sequence) return 0;
    if (parsed.type == 1) {
        if (strcmp(client->partial, client->text)) {
            strcpy(client->partial, client->text);
            if (*client->text) result(client, WENZI_ASR_PARTIAL, client->text, 0);
        }
    } else {
        client->last_final_sequence = parsed.sequence;
        client->partial[0] = '\0';
        if (*client->text) result(client, WENZI_ASR_FINAL, client->text, 0);
    }
    return 0;
}

static int cloud_callback(struct lws *socket, enum lws_callback_reasons reason,
                          void *user, void *input, size_t length) {
    (void)user;
    wenzi_iflytek *client = lws_context_user(lws_get_context(socket));
    if (!client) return 0;
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        client->socket = socket;
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (client->done) return -1;
        if (lws_frame_is_binary(socket) || length > MESSAGE_CAPACITY - client->received) {
            complete(client, -EPROTO, "讯飞结果格式或长度无效");
            return -1;
        }
        memcpy(client->message + client->received, input, length);
        client->received += length;
        if (lws_is_final_fragment(socket) && !lws_remaining_packet_payload(socket)) {
            client->message[client->received] = '\0';
            int rc = receive_result(client);
            client->received = 0;
            if (rc) {
                complete(client, rc, "讯飞结果协议无效");
                return -1;
            }
        }
        break;
    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        if (!client->ready || client->done || client->end_sent ||
            monotonic_ms() < client->next_send) break;
        unsigned char frame[LWS_PRE + FRAME_BYTES];
        size_t amount = client->queued >= FRAME_BYTES ? FRAME_BYTES : client->queued;
        if (amount < FRAME_BYTES && !client->finishing) break;
        if (amount) {
            for (size_t i = 0; i < amount; ++i)
                frame[LWS_PRE + i] = client->audio[(client->head + i) % AUDIO_CAPACITY];
            if (lws_write(socket, frame + LWS_PRE, amount, LWS_WRITE_BINARY) != (int)amount) {
                complete(client, -EIO, "讯飞音频发送失败");
                return -1;
            }
            client->head = (client->head + amount) % AUDIO_CAPACITY;
            client->queued -= amount;
            client->sent_audio = 1;
            /* 不追赶积压突发，始终按真实音频时长发送。 */
            client->next_send = monotonic_ms() + (int64_t)(amount * 1000 / 32000);
        } else if (client->finishing) {
            static const char end[] = "{\"end\":true}";
            memcpy(frame + LWS_PRE, end, sizeof(end) - 1);
            if (lws_write(socket, frame + LWS_PRE, sizeof(end) - 1,
                          LWS_WRITE_BINARY) != (int)(sizeof(end) - 1)) {
                complete(client, -EIO, "讯飞结束标识发送失败");
                return -1;
            }
            client->end_sent = 1;
        }
        break;
    }
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        client->socket = NULL;
        complete(client, -ECONNREFUSED, "讯飞 TLS 或网络连接失败");
        break;
    case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
        /* TCP 断线不能冒充云端的正常结束握手。 */
        client->peer_finished = length == 0 ||
            (length >= 2 && ((((unsigned char *)input)[0] << 8) |
                             ((unsigned char *)input)[1]) == LWS_CLOSE_STATUS_NORMAL);
        break;
    case LWS_CALLBACK_CLIENT_CLOSED:
        client->socket = NULL;
        if (client->end_sent && client->peer_finished && !*client->partial)
            complete(client, 0, "");
        else complete(client, -ECONNRESET, "讯飞连接结束，当前句尚未确认");
        break;
    default:
        break;
    }
    return 0;
}

static const struct lws_protocols cloud_protocols[] = {
    { .name = "wenzi-iflytek", .callback = cloud_callback,
      .rx_buffer_size = 4096 },
    { .name = NULL }
};

static wenzi_iflytek *creation_error(wenzi_iflytek *client, char *error,
                                    size_t size, const char *message) {
    if (error && size) snprintf(error, size, "%s", message);
    wenzi_iflytek_destroy(client);
    return NULL;
}

wenzi_iflytek *wenzi_iflytek_create(const struct wenzi_iflytek_config *config,
                                   wenzi_asr_callback callback, void *user,
                                   char *error, size_t error_size) {
    const char *app_id = config && config->app_id ? config->app_id : getenv("WENZI_IFLYTEK_APP_ID");
    const char *api_key = config && config->api_key ? config->api_key : getenv("WENZI_IFLYTEK_API_KEY");
    if (!app_id || !*app_id || !api_key || !*api_key) {
        errno = ENOKEY;
        if (error && error_size)
            snprintf(error, error_size, "未配置 WENZI_IFLYTEK_APP_ID / WENZI_IFLYTEK_API_KEY");
        return NULL;
    }
    wenzi_iflytek *client = calloc(1, sizeof(*client));
    if (!client) {
        if (error && error_size) snprintf(error, error_size, "网络内存不足");
        return NULL;
    }
    client->callback = callback;
    client->user = user;
    client->last_final_sequence = -1;
    client->connect_timeout_ms = config && config->connect_timeout_ms > 0 ? config->connect_timeout_ms : 10000;
    client->drain_timeout_ms = config && config->drain_timeout_ms > 0 ? config->drain_timeout_ms : 5000;
    char uri[1024];
    const char *url = config && config->url ? config->url : "wss://rtasr.xfyun.cn/v1/ws";
    if (strlen(url) >= sizeof(uri))
        return creation_error(client, error, error_size, "云端地址过长");
    strcpy(uri, url);
    const char *scheme, *host, *path;
    if (lws_parse_uri(uri, &scheme, &host, &client->port, &path) ||
        strcmp(scheme, "wss") || !*host || strlen(host) >= sizeof(client->host) ||
        strchr(path, '?') || strchr(path, '#'))
        return creation_error(client, error, error_size, "云端地址必须是无查询参数的 WSS 地址");
    strcpy(client->host, host);
    client->ssl = LCCSCF_USE_SSL;
    char signature[29], encoded_signature[128], encoded_id[512];
    time_t ts = time(NULL);
    if (wenzi_iflytek_signature(app_id, api_key, ts, signature, sizeof(signature)) ||
        url_encode(signature, encoded_signature, sizeof(encoded_signature)) ||
        url_encode(app_id, encoded_id, sizeof(encoded_id)))
        return creation_error(client, error, error_size, "讯飞签名生成失败");
    int n = snprintf(client->path, sizeof(client->path),
                     "/%s?appid=%s&ts=%lld&signa=%s&lang=cn&engLangType=1", path,
                     encoded_id, (long long)ts, encoded_signature);
    if (n < 0 || (size_t)n >= sizeof(client->path))
        return creation_error(client, error, error_size, "讯飞请求过长");
    struct lws_context_creation_info info = {0};
    /* 关闭库的请求调试日志，鉴权查询参数不能进入日志。 */
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = cloud_protocols;
    info.user = client;
    info.gid = info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.client_ssl_ca_filepath = config ? config->ca_file : NULL;
    client->context = lws_create_context(&info);
    if (!client->context)
        return creation_error(client, error, error_size, "讯飞 TLS 上下文创建失败");
    struct lws_client_connect_info connect = {0};
    connect.context = client->context;
    connect.address = client->host;
    connect.port = client->port;
    connect.path = client->path;
    connect.host = client->host;
    connect.ssl_connection = client->ssl;
    connect.local_protocol_name = cloud_protocols[0].name;
    connect.pwsi = &client->socket;
    client->connect_deadline = monotonic_ms() + client->connect_timeout_ms;
    /* 创建阶段抑制回调，失败由错误缓冲区返回。 */
    client->callback = NULL;
    struct lws *socket = lws_client_connect_via_info(&connect);
    client->callback = callback;
    if (!socket || client->done)
        return creation_error(client, error, error_size, "讯飞连接创建失败");
    client->socket = socket;
    if (error && error_size) *error = '\0';
    return client;
}

int wenzi_iflytek_feed_pcm(wenzi_iflytek *client, const int16_t *samples,
                           size_t count) {
    if (!client || (!samples && count)) return -EINVAL;
    if (client->done || client->finishing) return -ESHUTDOWN;
    if (count > (AUDIO_CAPACITY - client->queued) / 2) {
        complete(client, -ENOBUFS, "云端音频积压超过五秒");
        return -ENOBUFS;
    }
    size_t tail = (client->head + client->queued) % AUDIO_CAPACITY;
    for (size_t i = 0; i < count; ++i) {
        uint16_t sample = (uint16_t)samples[i];
        client->audio[tail] = (unsigned char)sample;
        tail = (tail + 1) % AUDIO_CAPACITY;
        client->audio[tail] = (unsigned char)(sample >> 8);
        tail = (tail + 1) % AUDIO_CAPACITY;
    }
    client->queued += count * 2;
    return 0;
}

int wenzi_iflytek_finish(wenzi_iflytek *client) {
    if (!client) return -EINVAL;
    if (client->done || client->finishing) return 0;
    client->finishing = 1;
    client->drain_deadline = monotonic_ms() + client->drain_timeout_ms;
    /* 未发音频时取消，不制造空音频识别请求。 */
    if (!client->queued && !client->sent_audio) wenzi_iflytek_cancel(client);
    return 0;
}

int wenzi_iflytek_poll(wenzi_iflytek *client, int timeout_ms) {
    if (!client || timeout_ms < 0) return -EINVAL;
    if (client->done) return 0;
    int64_t now = monotonic_ms();
    if ((!client->ready && now >= client->connect_deadline) ||
        (client->finishing && now >= client->drain_deadline)) {
        complete(client, -ETIMEDOUT, "讯飞连接或收尾超时");
        return -ETIMEDOUT;
    }
    if (client->socket && client->ready && !client->end_sent &&
        now >= client->next_send &&
        (client->queued >= FRAME_BYTES || client->finishing))
        lws_callback_on_writable(client->socket);
    /* 短轮询保证音频节奏，控制等待由核心决定。 */
    lws_cancel_service(client->context);
    int rc = lws_service(client->context, 0);
    if (timeout_ms) {
        int delay_ms = timeout_ms > 20 ? 20 : timeout_ms;
        struct timespec delay = { 0, delay_ms * 1000000L };
        nanosleep(&delay, NULL);
    }
    if (rc < 0) complete(client, -EIO, "讯飞网络轮询失败");
    return rc < 0 ? -EIO : 0;
}

void wenzi_iflytek_cancel(wenzi_iflytek *client) {
    if (!client || client->done) return;
    client->queued = 0;
    complete(client, 0, "");
}

void wenzi_iflytek_destroy(wenzi_iflytek *client) {
    if (!client) return;
    /* 销毁不发事件，旧会话结果不能穿过切换边界。 */
    client->callback = NULL;
    if (client->context) lws_context_destroy(client->context);
    free(client);
}
