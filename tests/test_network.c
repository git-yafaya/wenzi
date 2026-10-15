#define _POSIX_C_SOURCE 200809L
#include "wenzi_network.h"
#include "network_json.h"

/* Release 构建也执行断言；只使用本机服务和假凭据。 */
#undef NDEBUG
#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/tcp.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum { LOG_CAPACITY = 96, FRAME_BYTES = 1280, SAMPLE_COUNT = 1920 };
static char temp_dir[128], cert_path[160], key_path[160];
static int16_t audio[SAMPLE_COUNT];

static int64_t now_ms(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void pause_ms(int milliseconds)
{
    struct timespec delay = {milliseconds / 1000, milliseconds % 1000 * 1000000L};
    while (nanosleep(&delay, &delay) < 0)
        assert(errno == EINTR);
}

static void remove_certificate(void)
{
    if (temp_dir[0]) {
        assert(unlink(cert_path) == 0);
        assert(unlink(key_path) == 0);
        assert(rmdir(temp_dir) == 0);
        temp_dir[0] = '\0';
    }
}

static void certificate_extension(X509 *cert, int nid, const char *value)
{
    X509V3_CTX ctx;
    X509V3_set_ctx(&ctx, cert, cert, NULL, NULL, 0);
    X509_EXTENSION *extension = X509V3_EXT_conf_nid(NULL, &ctx, nid, (char *)value);
    assert(extension && X509_add_ext(cert, extension, -1) == 1);
    X509_EXTENSION_free(extension);
}

static void make_certificate(void)
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    EVP_PKEY *key = NULL;
    X509 *cert = X509_new();
    assert(ctx && cert);
    assert(EVP_PKEY_keygen_init(ctx) == 1);
    assert(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) == 1);
    assert(EVP_PKEY_keygen(ctx, &key) == 1);
    EVP_PKEY_CTX_free(ctx);
    assert(X509_set_version(cert, 2) == 1);
    assert(ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) == 1);
    assert(X509_gmtime_adj(X509_getm_notBefore(cert), -60));
    assert(X509_gmtime_adj(X509_getm_notAfter(cert), 3600));
    assert(X509_set_pubkey(cert, key) == 1);
    X509_NAME *subject = X509_get_subject_name(cert);
    assert(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                    (const unsigned char *)"localhost", -1, -1, 0) == 1);
    assert(X509_set_issuer_name(cert, subject) == 1);
    certificate_extension(cert, NID_basic_constraints, "critical,CA:TRUE");
    certificate_extension(cert, NID_key_usage, "critical,digitalSignature,keyEncipherment,keyCertSign");
    certificate_extension(cert, NID_subject_alt_name, "DNS:localhost");
    assert(X509_sign(cert, key, EVP_sha256()) > 0);
    strcpy(temp_dir, "/tmp/wenzi-network-XXXXXX");
    assert(mkdtemp(temp_dir));
    snprintf(cert_path, sizeof(cert_path), "%s/cert.pem", temp_dir);
    snprintf(key_path, sizeof(key_path), "%s/key.pem", temp_dir);
    FILE *file = fopen(cert_path, "w");
    assert(file && PEM_write_X509(file, cert) == 1 && fclose(file) == 0);
    file = fopen(key_path, "w");
    assert(file && PEM_write_PrivateKey(file, key, NULL, NULL, 0, NULL, NULL) == 1);
    assert(fclose(file) == 0);
    X509_free(cert);
    EVP_PKEY_free(key);
    assert(atexit(remove_certificate) == 0);
}

static void test_commands(void)
{
    static const struct {
        const char *json;
        wenzi_network_command_type type;
        wenzi_engine engine;
    } valid[] = {
        {"{\"cmd\":\"start\"}", WENZI_CMD_START, WENZI_LOCAL},
        {" {\"cmd\":\"stop\"} \r\n", WENZI_CMD_STOP, WENZI_LOCAL},
        {"{\"cmd\":\"status\"}", WENZI_CMD_STATUS, WENZI_LOCAL},
        {"{\"cmd\":\"\\u0073tatus\"}", WENZI_CMD_STATUS, WENZI_LOCAL},
        {"{\"\\u0063md\":\"status\"}", WENZI_CMD_STATUS, WENZI_LOCAL},
        {"{\"cmd\":\"switch_engine\",\"engine\":\"local\"}", WENZI_CMD_SWITCH_ENGINE, WENZI_LOCAL},
        {"{\"cmd\":\"switch_engine\",\"engine\":\"cloud\"}", WENZI_CMD_SWITCH_ENGINE, WENZI_CLOUD},
        {"{\"provider\":\"iflytek\",\"engine\":\"cloud\",\"cmd\":\"switch_engine\"}", WENZI_CMD_SWITCH_ENGINE, WENZI_CLOUD}
    };
    static const char *const invalid[] = {
        "", "null", "[]", "{}", "\"status\"", "{\"cmd\":1}",
        "{\"cmd\":1.5}", "{\"cmd\":true}", "{\"cmd\":null}",
        "{\"cmd\":[\"status\"]}", "{\"cmd\":{\"cmd\":\"status\"}}",
        "{\"cmd\":\"unknown\"}", "{\"cmd\":\"status\",\"extra\":\"值\"}",
        "{\"cmd\":\"status\",\"cmd\":\"stop\"}",
        "{\"cmd\":\"status\",\"\\u0063md\":\"stop\"}",
        "{\"cmd\":\"start\",\"engine\":\"local\"}",
        "{\"cmd\":\"stop\",\"provider\":\"iflytek\"}",
        "{\"cmd\":\"switch_engine\"}",
        "{\"cmd\":\"switch_engine\",\"engine\":2}",
        "{\"cmd\":\"switch_engine\",\"engine\":\"unknown\"}",
        "{\"cmd\":\"switch_engine\",\"provider\":\"iflytek\"}",
        "{\"cmd\":\"switch_engine\",\"engine\":\"local\",\"provider\":\"iflytek\"}",
        "{\"cmd\":\"switch_engine\",\"engine\":\"cloud\",\"provider\":\"other\"}",
        "{\"cmd\":\"switch_engine\",\"engine\":\"cloud\",\"provider\":false}",
        "{\"cmd\":\"switch_engine\",\"engine\":\"cloud\",\"engine\":\"local\"}",
        "{\"cmd\":\"switch_engine\",\"engine\":\"cloud\",\"provider\":\"iflytek\",\"provider\":\"iflytek\"}",
        "{\"cmd\":\"status\"}garbage", "{\"cmd\":\"status\"}{}",
        "{\"cmd\":\"status\",}", "{\"cmd\":\"status\"", "{\"cmd\":\"status\"\"}",
        "{\"cmd\":\"status\\u0000\"}", "{\"cmd\\u0000\":\"status\"}",
        "{\"cmd\":\"\\ud800\"}", "{\"cmd\":\"\xc0\xaf\"}",
        "{\"cmd\":\"\xed\xa0\x80\"}", "{\"cmd\":\"\xf4\x90\x80\x80\"}"
    };
    wenzi_network_command command;
    char error[256];
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        assert(wenzi_network_parse_command(valid[i].json, strlen(valid[i].json),
                                            &command, error, sizeof(error)) == 0);
        assert(!*error && command.type == valid[i].type);
        if (command.type == WENZI_CMD_SWITCH_ENGINE)
            assert(command.engine == valid[i].engine);
    }
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        int rc = wenzi_network_parse_command(invalid[i], strlen(invalid[i]),
                                             &command, error, sizeof(error));
        if (rc >= 0)
            fprintf(stderr, "错误命令被接受 [%zu]: %s\n", i, invalid[i]);
        assert(rc < 0 && *error);
    }
    const char nul[] = "{\"cmd\":\"status\"}\0garbage";
    assert(wenzi_network_parse_command(nul, sizeof(nul) - 1, &command, error, sizeof(error)) < 0);
    char long_json[1025];
    memset(long_json, ' ', sizeof(long_json));
    assert(wenzi_network_parse_command(long_json, sizeof(long_json), &command, error, sizeof(error)) == -E2BIG);
    assert(wenzi_network_parse_command("{}", 2, NULL, error, sizeof(error)) < 0);
}

typedef struct {
    char type[16], engine[16], state[16], reason[16], text[256];
    uint64_t session, utterance;
    double timestamp;
    int code;
    unsigned seen;
} json_event;

static int event_visit(void *user, const struct lejp_ctx *ctx,
                       unsigned char reason, const char *value, size_t length)
{
    json_event *event = user;
    char *target = NULL;
    size_t capacity = 0;
    unsigned bit = 0;
    if (reason == LEJPCB_VAL_STR_END) {
        if (!strcmp(ctx->path, "type")) { target = event->type; capacity = sizeof(event->type); bit = 1; }
        else if (!strcmp(ctx->path, "engine")) { target = event->engine; capacity = sizeof(event->engine); bit = 2; }
        else if (!strcmp(ctx->path, "state")) { target = event->state; capacity = sizeof(event->state); bit = 4; }
        else if (!strcmp(ctx->path, "reason")) { target = event->reason; capacity = sizeof(event->reason); bit = 128; }
        else if (!strcmp(ctx->path, "text") || !strcmp(ctx->path, "message")) { target = event->text; capacity = sizeof(event->text); bit = 256; }
        if (target) {
            assert(length < capacity && !(event->seen & bit));
            memcpy(target, value, length + 1);
            event->seen |= bit;
        }
    } else if (reason == LEJPCB_VAL_NUM_INT || reason == LEJPCB_VAL_NUM_FLOAT) {
        char *end;
        if (!strcmp(ctx->path, "session_id")) { event->session = strtoull(value, &end, 10); bit = 8; }
        else if (!strcmp(ctx->path, "utterance_id")) { event->utterance = strtoull(value, &end, 10); bit = 16; }
        else if (!strcmp(ctx->path, "timestamp")) { event->timestamp = strtod(value, &end); bit = 32; }
        else if (!strcmp(ctx->path, "code")) { event->code = (int)strtol(value, &end, 10); bit = 64; }
        if (bit) {
            assert(!*end && !(event->seen & bit));
            event->seen |= bit;
        }
    }
    return 0;
}

static json_event decode_event(const char *json, size_t length)
{
    char scratch[512];
    json_event event = {0};
    assert(wenzi_json_parse(json, length, scratch, sizeof(scratch), event_visit, &event) == 0);
    assert((event.seen & 127) == 127);
    return event;
}

static void test_event_json(void)
{
    static const char text[] = "引号\" 反斜线\\\n\r\t\b\f\001 中文😀";
    static const char *const types[] = {"partial", "final", "error", "status"};
    wenzi_event event = {.engine = WENZI_CLOUD, .state = WENZI_DRAINING,
        .session_id = UINT64_MAX, .utterance_id = 41, .timestamp = 1700000000.25,
        .text = text, .code = EPROTO, .reason = WENZI_END_STOP};
    char json[1024];
    for (int type = WENZI_PARTIAL; type <= WENZI_STATUS; ++type) {
        event.type = (wenzi_event_type)type;
        int length = wenzi_network_event_json(&event, json, sizeof(json));
        assert(length > 0 && (size_t)length == strlen(json));
        json_event decoded = decode_event(json, (size_t)length);
        assert(!strcmp(decoded.type, types[type]));
        assert(!strcmp(decoded.engine, "cloud") && !strcmp(decoded.state, "draining"));
        assert(decoded.session == event.session_id && decoded.utterance == event.utterance_id);
        assert(decoded.timestamp == event.timestamp && decoded.code == event.code);
        assert(type == WENZI_STATUS ? !(decoded.seen & 256) : !strcmp(decoded.text, text));
        if (type == WENZI_FINAL)
            assert(!strcmp(decoded.reason, "stop"));
    }
    assert(wenzi_network_event_json(&event, json, 2) == -ENOSPC);
    event.type = WENZI_PARTIAL;
    event.text = "\xc0\xaf";
    assert(wenzi_network_event_json(&event, json, sizeof(json)) == -EILSEQ);
    event.text = text;
    event.timestamp = NAN;
    assert(wenzi_network_event_json(&event, json, sizeof(json)) == -EINVAL);
    event.timestamp = 1;
    event.type = (wenzi_event_type)99;
    assert(wenzi_network_event_json(&event, json, sizeof(json)) == -EINVAL);
    char signature[29];
    assert(wenzi_iflytek_signature("test-app", "test-key", 1700000000, signature, sizeof(signature)) == 0);
    assert(!strcmp(signature, "2RJndfgpUCMjT0agTHRPj7G6Zu4="));
    assert(wenzi_iflytek_signature("test-app", "test-key", 1700000000, signature, 28) == -EINVAL);
}

typedef struct { int fd; SSL *tls; } connection;

static int transfer(connection *peer, void *buffer, size_t length, int writing)
{
    unsigned char *data = buffer;
    while (length) {
        int amount = peer->tls ? (writing ? SSL_write(peer->tls, data, (int)length) : SSL_read(peer->tls, data, (int)length)) :
                     (int)(writing ? send(peer->fd, data, length, MSG_NOSIGNAL) : recv(peer->fd, data, length, 0));
        if (amount <= 0)
            return -1;
        length -= (size_t)amount;
        data += amount;
    }
    return 0;
}

static void write_frame(connection *peer, int opcode, int final,
                         int masked, const void *payload, size_t length)
{
    unsigned char frame[4096], mask[4] = {0x12, 0x34, 0x56, 0x78};
    size_t used = 2;
    assert(length < sizeof(frame) - 8);
    frame[0] = (unsigned char)((final ? 128 : 0) | opcode);
    frame[1] = (unsigned char)((masked ? 128 : 0) | (length < 126 ? length : 126));
    if (length >= 126) {
        frame[used++] = (unsigned char)(length >> 8);
        frame[used++] = (unsigned char)length;
    }
    if (masked) { memcpy(frame + used, mask, 4); used += 4; }
    for (size_t i = 0; i < length; ++i)
        frame[used + i] = ((const unsigned char *)payload)[i] ^ (masked ? mask[i % 4] : 0);
    assert(transfer(peer, frame, used + length, 1) == 0);
}

static int read_frame(connection *peer, unsigned char *payload, size_t capacity,
                      size_t *length)
{
    unsigned char header[2], mask[4];
    if (transfer(peer, header, 2, 0) < 0)
        return -1;
    assert((header[0] & 128) && (header[1] & 128));
    *length = header[1] & 127;
    if (*length == 126) {
        unsigned char extended[2];
        assert(transfer(peer, extended, 2, 0) == 0);
        *length = (size_t)extended[0] * 256 + extended[1];
    }
    assert(*length < capacity);
    assert(transfer(peer, mask, 4, 0) == 0 && transfer(peer, payload, *length, 0) == 0);
    for (size_t i = 0; i < *length; ++i)
        payload[i] ^= mask[i % 4];
    payload[*length] = 0;
    return header[0] & 15;
}

enum mock_mode { MOCK_FINAL, MOCK_TIMEOUT, MOCK_DROP, MOCK_TLS_REJECT, MOCK_HOST_REJECT };
typedef struct {
    int listener, port, frames, end_seen, upgraded;
    enum mock_mode mode;
    int64_t frame_time[3];
    pthread_t thread;
} mock_server;

static void send_result(connection *peer, int type, const char *text)
{
    char inner[512], escaped[1024], outer[1280];
    int n = snprintf(inner, sizeof(inner), "{\"seg_id\":7,\"cn\":{\"st\":{\"type\":%d,\"rt\":[{\"ws\":[{\"cw\":[{\"w\":\"%s\"}]}]}]}}}", type, text);
    assert(n > 0 && (size_t)n < sizeof(inner));
    assert(wenzi_json_escape(inner, (size_t)n, escaped, sizeof(escaped)) > 0);
    n = snprintf(outer, sizeof(outer), "{\"action\":\"result\",\"code\":\"0\",\"data\":\"%s\"}", escaped);
    assert(n > 0 && (size_t)n < sizeof(outer));
    write_frame(peer, 1, 1, 0, outer, (size_t)n);
}

static int read_upgrade(connection *peer, char *request, size_t size)
{
    size_t used = 0;
    while (used < size - 1) {
        if (transfer(peer, request + used, 1, 0) < 0)
            return -1;
        request[++used] = '\0';
        if (used >= 4 && !strcmp(request + used - 4, "\r\n\r\n"))
            return 0;
    }
    return -1;
}

static void answer_upgrade(connection *peer, const char *request)
{
    char key[128] = "", input[256], accept[64], response[512];
    const char *line = request;
    while (line && *line) {
        if (!strncasecmp(line, "Sec-WebSocket-Key:", 18)) {
            line += 18;
            while (*line == ' ') ++line;
            size_t length = strcspn(line, "\r\n");
            assert(length < sizeof(key));
            memcpy(key, line, length);
            key[length] = '\0';
            break;
        }
        line = strstr(line, "\r\n");
        if (line) line += 2;
    }
    assert(*key);
    int length = snprintf(input, sizeof(input), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_size;
    assert(EVP_Digest(input, (size_t)length, hash, &hash_size, EVP_sha1(), NULL) == 1);
    assert(EVP_EncodeBlock((unsigned char *)accept, hash, (int)hash_size) == 28);
    length = snprintf(response, sizeof(response), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
    assert(transfer(peer, response, (size_t)length, 1) == 0);
}

static void *serve_mock(void *user)
{
    mock_server *server = user;
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    assert(ctx && SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM) == 1);
    assert(SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) == 1);
    connection peer = {.fd = accept(server->listener, NULL, NULL)};
    assert(peer.fd >= 0);
    struct timeval timeout = {3, 0};
    int yes = 1;
    assert(setsockopt(peer.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    assert(setsockopt(peer.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    assert(setsockopt(peer.fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == 0);
    peer.tls = SSL_new(ctx);
    assert(peer.tls && SSL_set_fd(peer.tls, peer.fd) == 1);
    if (SSL_accept(peer.tls) != 1) {
        assert(server->mode >= MOCK_TLS_REJECT);
        goto done;
    }
    char request[4096] = "";
    if (read_upgrade(&peer, request, sizeof(request)) < 0) {
        assert(server->mode >= MOCK_TLS_REJECT);
        goto done;
    }
    assert(server->mode < MOCK_TLS_REJECT);
    assert(strstr(request, "GET /v1/ws?appid=test-app&ts="));
    assert(strstr(request, "&signa=") && strstr(request, "&lang=cn&engLangType=1"));
    answer_upgrade(&peer, request);
    server->upgraded = 1;
    static const char started[] = "{\"action\":\"started\",\"code\":0,\"data\":\"\"}";
    write_frame(&peer, 1, 1, 0, started, sizeof(started) - 1);
    unsigned char payload[2048];
    for (int frame = 0; frame < 3; ++frame) {
        size_t length;
        assert(read_frame(&peer, payload, sizeof(payload), &length) == 2 && length == FRAME_BYTES);
        server->frame_time[frame] = now_ms();
        for (size_t i = 0; i < length / 2; ++i) {
            uint16_t sample = (uint16_t)audio[(size_t)frame * (FRAME_BYTES / 2) + i];
            assert(payload[i * 2] == (unsigned char)sample);
            assert(payload[i * 2 + 1] == (unsigned char)(sample >> 8));
        }
        ++server->frames;
    }
    send_result(&peer, 1, "先前快照");
    send_result(&peer, 1, "修订快照");
    if (server->mode == MOCK_DROP)
        goto close_socket;
    size_t length;
    assert(read_frame(&peer, payload, sizeof(payload), &length) == 2);
    assert(length == 12 && !memcmp(payload, "{\"end\":true}", length));
    server->end_seen = 1;
    if (server->mode == MOCK_TIMEOUT) {
        pause_ms(600);
        goto done;
    }
    send_result(&peer, 0, "确认句子。");
    send_result(&peer, 0, "确认句子。");
close_socket: {
        unsigned char close_message[] = {3, 232};
        write_frame(&peer, 8, 1, 0, close_message, sizeof(close_message));
    }
done:
    SSL_free(peer.tls);
    close(peer.fd);
    close(server->listener);
    SSL_CTX_free(ctx);
    return NULL;
}

static void start_mock(mock_server *server, enum mock_mode mode)
{
    memset(server, 0, sizeof(*server));
    server->mode = mode;
    server->listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(server->listener >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(server->listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(server->listener, (struct sockaddr *)&address, &size) == 0);
    server->port = ntohs(address.sin_port);
    assert(listen(server->listener, 1) == 0);
    assert(pthread_create(&server->thread, NULL, serve_mock, server) == 0);
}

static void join_mock(mock_server *server)
{
    assert(pthread_join(server->thread, NULL) == 0);
    if (server->mode >= MOCK_TLS_REJECT) {
        assert(!server->upgraded && !server->frames && !server->end_seen);
        return;
    }
    assert(server->frames == 3 && server->upgraded);
    for (int i = 1; i < 3; ++i) {
        int64_t interval = server->frame_time[i] - server->frame_time[i - 1];
        assert(interval >= 35 && interval < 500);
    }
    assert(server->frame_time[2] - server->frame_time[0] >= 75);
    assert(server->end_seen == (server->mode != MOCK_DROP));
    printf("模拟 RTASR: 3 × 1280B S16LE，间隔 %lld/%lldms\n",
           (long long)(server->frame_time[1] - server->frame_time[0]),
           (long long)(server->frame_time[2] - server->frame_time[1]));
}

static struct wenzi_iflytek_config mock_config(const mock_server *server, char *url,
                                               size_t size, int trusted)
{
    snprintf(url, size, "wss://%s:%d/v1/ws",
             server->mode == MOCK_HOST_REJECT ? "127.0.0.1" : "localhost", server->port);
    struct wenzi_iflytek_config config = {.app_id = "test-app", .api_key = "test-key",
        .url = url, .ca_file = trusted ? cert_path : NULL,
        .connect_timeout_ms = 2000, .drain_timeout_ms = 200};
    return config;
}

typedef struct {
    int count[4], code, order, final_order, idle_order;
    char partial[2][64], final[64];
    uint64_t session, utterance;
    wenzi_ws_server *ws;
} event_log;

static void record_event(void *user, const wenzi_event *event)
{
    event_log *log = user;
    assert((unsigned)event->type <= WENZI_STATUS && event->timestamp > 0);
    ++log->order;
    if (event->type == WENZI_PARTIAL) {
        assert(log->count[WENZI_PARTIAL] < 2);
        snprintf(log->partial[log->count[WENZI_PARTIAL]], sizeof(log->partial[0]), "%s", event->text);
        log->session = event->session_id;
        log->utterance = event->utterance_id;
    } else if (event->type == WENZI_FINAL) {
        assert(event->session_id == log->session && event->utterance_id == log->utterance);
        snprintf(log->final, sizeof(log->final), "%s", event->text);
        log->final_order = log->order;
    } else if (event->type == WENZI_ERROR) {
        assert(event->code > 0);
        log->code = event->code;
    } else if (event->state == WENZI_IDLE) {
        log->idle_order = log->order;
    }
    ++log->count[event->type];
    /* 回调只记录和发布，控制命令由外部轮询执行。 */
    if (log->ws)
        assert(wenzi_ws_publish(log->ws, event) == 0);
}

static wenzi *create_core(event_log *log, const struct wenzi_iflytek_config *cloud,
                           wenzi_engine engine)
{
    wenzi_config config = {.engine = engine, .cloud = cloud, .drain_timeout_ms = 400};
    wenzi *core = wenzi_create(&config, record_event, log);
    char error[256];
    assert(core && wenzi_get_state(core) == WENZI_IDLE);
    log->ws = wenzi_ws_create(core, NULL, 0, error, sizeof(error));
    assert(log->ws && !*error && wenzi_ws_port(log->ws) > 0);
    return core;
}

typedef struct {
    connection peer;
    unsigned char buffer[32768];
    size_t used;
    int upgraded, closed, count;
    json_event events[LOG_CAPACITY];
} ws_client;

static void read_client(ws_client *client)
{
    for (;;) {
        ssize_t amount = recv(client->peer.fd, client->buffer + client->used,
                              sizeof(client->buffer) - client->used - 1, MSG_DONTWAIT);
        if (amount < 0) { assert(errno == EAGAIN || errno == EWOULDBLOCK); break; }
        if (!amount) { client->closed = 1; break; }
        client->used += (size_t)amount;
        assert(client->used < sizeof(client->buffer) - 1);
    }
    if (!client->upgraded) {
        client->buffer[client->used] = '\0';
        char *end = strstr((char *)client->buffer, "\r\n\r\n");
        if (!end) return;
        assert(strstr((char *)client->buffer, "101 Switching Protocols"));
        assert(strstr((char *)client->buffer, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
        size_t length = (size_t)(end - (char *)client->buffer) + 4;
        memmove(client->buffer, client->buffer + length, client->used - length);
        client->used -= length;
        client->upgraded = 1;
    }
    while (client->used >= 2) {
        size_t header = 2, length = client->buffer[1] & 127;
        assert((client->buffer[0] & 128) && !(client->buffer[1] & 128));
        if (length == 126) {
            if (client->used < 4) return;
            length = (size_t)client->buffer[2] * 256 + client->buffer[3];
            header = 4;
        }
        assert(length < 32768);
        if (client->used < header + length) return;
        int opcode = client->buffer[0] & 15;
        if (opcode == 8) client->closed = 1;
        else {
            assert(opcode == 1 && client->count < LOG_CAPACITY);
            client->events[client->count++] = decode_event((char *)client->buffer + header, length);
        }
        memmove(client->buffer, client->buffer + header + length, client->used - header - length);
        client->used -= header + length;
    }
}

static void pump(wenzi *core, event_log *log, ws_client *client)
{
    assert(wenzi_ws_poll(log->ws, 0) == 0);
    int rc = wenzi_poll(core, 0);
    assert(rc <= 0);
    assert(wenzi_ws_poll(log->ws, 0) == 0);
    if (client) read_client(client);
    pause_ms(2);
}

static void connect_client(ws_client *client, wenzi *core, event_log *log)
{
    memset(client, 0, sizeof(*client));
    client->peer.fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(client->peer.fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons((uint16_t)wenzi_ws_port(log->ws))};
    assert(connect(client->peer.fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    static const char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    assert(transfer(&client->peer, (void *)request, sizeof(request) - 1, 1) == 0);
    int64_t deadline = now_ms() + 2000;
    while (!client->upgraded && now_ms() < deadline) pump(core, log, client);
    assert(client->upgraded && !client->closed);
}

static json_event command(ws_client *client, wenzi *core, event_log *log, const char *json)
{
    int previous = client->count;
    write_frame(&client->peer, 1, 1, 1, json, strlen(json));
    int64_t deadline = now_ms() + 2000;
    while (client->count == previous && now_ms() < deadline) pump(core, log, client);
    assert(client->count > previous && !client->closed);
    return client->events[client->count - 1];
}

static void check_start_error(ws_client *client, wenzi *core, event_log *log)
{
    int previous = client->count, core_errors = log->count[WENZI_ERROR];
    json_event event = command(client, core, log, "{\"cmd\":\"start\"}");
    assert(!strcmp(event.type, "error") && !strcmp(event.state, "idle") && !strcmp(event.engine, "local"));
    assert(event.code == EINVAL && event.code == log->code);
    assert(wenzi_get_state(core) == WENZI_IDLE && log->count[WENZI_ERROR] == core_errors + 1);
    /* status 作后续标记，保证此前的重复错误已全部收齐。 */
    event = command(client, core, log, "{\"cmd\":\"status\"}");
    assert(!strcmp(event.type, "status") && !strcmp(event.state, "idle"));
    int errors = 0;
    for (int i = previous; i < client->count; ++i) {
        if (!strcmp(client->events[i].type, "error")) {
            assert(client->events[i].code == log->code);
            ++errors;
        }
    }
    assert(errors == 1);
}

static void test_ws_errors(void)
{
    event_log log = {0};
    wenzi *core = create_core(&log, NULL, WENZI_LOCAL);
    ws_client client;
    connect_client(&client, core, &log);
    json_event event = command(&client, core, &log, "{\"cmd\":\"status\",\"cmd\":\"start\"}");
    assert(!strcmp(event.type, "error") && event.code == EINVAL && !strcmp(event.state, "idle"));
    assert(wenzi_get_state(core) == WENZI_IDLE && !log.count[WENZI_ERROR]);
    check_start_error(&client, core, &log);
    close(client.peer.fd);
    wenzi_ws_destroy(log.ws);
    log.ws = NULL;
    wenzi_destroy(core);
}

static void feed_audio(wenzi *core)
{
    for (int i = 0; i < SAMPLE_COUNT / WENZI_FRAME_SAMPLES; ++i)
        assert(wenzi_feed_pcm(core, audio + i * WENZI_FRAME_SAMPLES, WENZI_FRAME_SAMPLES) == 0);
}

static void test_ws_and_core(void)
{
    mock_server server;
    start_mock(&server, MOCK_FINAL);
    char url[128];
    struct wenzi_iflytek_config cloud = mock_config(&server, url, sizeof(url), 1);
    event_log log = {0};
    wenzi *core = create_core(&log, &cloud, WENZI_LOCAL);
    ws_client client;
    connect_client(&client, core, &log);
    json_event event = command(&client, core, &log, "{\"cmd\":\"status\"}");
    assert(!strcmp(event.type, "status") && !strcmp(event.state, "idle") && !strcmp(event.engine, "local"));
    event = command(&client, core, &log, "{\"cmd\":\"status\",\"cmd\":\"start\"}");
    assert(!strcmp(event.type, "error") && event.code == EINVAL && !strcmp(event.state, "idle"));
    assert(wenzi_get_state(core) == WENZI_IDLE);
    event = command(&client, core, &log, "{\"cmd\":\"switch_engine\",\"engine\":\"cloud\",\"provider\":\"iflytek\"}");
    assert(!strcmp(event.engine, "cloud") && !strcmp(event.state, "idle"));
    int previous = client.count;
    write_frame(&client.peer, 1, 0, 1, "{\"cmd\":\"sta", 11);
    for (int i = 0; i < 10; ++i) pump(core, &log, &client);
    assert(wenzi_get_state(core) == WENZI_IDLE && client.count == previous);
    write_frame(&client.peer, 0, 1, 1, "rt\"}", 4);
    int64_t deadline = now_ms() + 2000;
    while (wenzi_get_state(core) != WENZI_RUNNING && now_ms() < deadline) pump(core, &log, &client);
    assert(wenzi_get_state(core) == WENZI_RUNNING && wenzi_get_engine(core) == WENZI_CLOUD);
    feed_audio(core);
    deadline = now_ms() + 2500;
    while (log.count[WENZI_PARTIAL] < 2 && now_ms() < deadline) pump(core, &log, &client);
    assert(log.count[WENZI_PARTIAL] == 2 && !strcmp(log.partial[0], "先前快照") && !strcmp(log.partial[1], "修订快照"));
    event = command(&client, core, &log, "{\"cmd\":\"stop\"}");
    assert(!strcmp(event.state, "draining") && event.session == log.session);
    deadline = now_ms() + 2500;
    while ((wenzi_get_state(core) != WENZI_IDLE || client.count < 8) && now_ms() < deadline) pump(core, &log, &client);
    assert(wenzi_get_state(core) == WENZI_IDLE && !client.closed);
    assert(log.count[WENZI_FINAL] == 1 && !strcmp(log.final, "确认句子。"));
    assert(!log.count[WENZI_ERROR] && log.final_order < log.idle_order);
    int partials = 0, finals = 0;
    for (int i = 0; i < client.count; ++i) {
        if (!strcmp(client.events[i].type, "partial")) {
            assert(partials < 2 && !strcmp(client.events[i].text, log.partial[partials]));
            ++partials;
        } else if (!strcmp(client.events[i].type, "final")) {
            ++finals;
            assert(!strcmp(client.events[i].text, log.final) && !strcmp(client.events[i].reason, "stop"));
            assert(client.events[i].session == log.session && client.events[i].utterance == log.utterance);
        }
    }
    assert(partials == 2 && finals == 1);
    event = command(&client, core, &log, "{\"cmd\":\"status\"}");
    assert(!strcmp(event.state, "idle") && !strcmp(event.engine, "cloud"));
    event = command(&client, core, &log, "{\"cmd\":\"switch_engine\",\"engine\":\"local\"}");
    assert(!strcmp(event.engine, "local") && wenzi_get_engine(core) == WENZI_LOCAL);
    check_start_error(&client, core, &log);
    close(client.peer.fd);
    wenzi_ws_destroy(log.ws);
    log.ws = NULL;
    wenzi_destroy(core);
    join_mock(&server);
}

typedef struct { int count[4], code; char partial[2][64], final[64]; } asr_log;

static void record_asr(void *user, const wenzi_asr_result *event)
{
    asr_log *log = user;
    assert((unsigned)event->type <= WENZI_ASR_DONE);
    if (event->type == WENZI_ASR_PARTIAL) {
        assert(log->count[event->type] < 2);
        snprintf(log->partial[log->count[event->type]], sizeof(log->partial[0]), "%s", event->text);
    } else if (event->type == WENZI_ASR_FINAL) {
        snprintf(log->final, sizeof(log->final), "%s", event->text);
    } else if (event->code) log->code = event->code;
    ++log->count[event->type];
}

static void test_done_once(void)
{
    mock_server server;
    start_mock(&server, MOCK_FINAL);
    char url[128], error[256];
    struct wenzi_iflytek_config config = mock_config(&server, url, sizeof(url), 1);
    asr_log log = {0};
    wenzi_iflytek *client = wenzi_iflytek_create(&config, record_asr, &log, error, sizeof(error));
    assert(client && !*error && !log.count[WENZI_ASR_DONE]);
    assert(wenzi_iflytek_feed_pcm(client, audio, SAMPLE_COUNT) == 0);
    int64_t deadline = now_ms() + 2500;
    while (log.count[WENZI_ASR_PARTIAL] < 2 && now_ms() < deadline)
        assert(wenzi_iflytek_poll(client, 2) == 0);
    assert(log.count[WENZI_ASR_PARTIAL] == 2);
    assert(!strcmp(log.partial[0], "先前快照") && !strcmp(log.partial[1], "修订快照"));
    assert(wenzi_iflytek_finish(client) == 0 && wenzi_iflytek_finish(client) == 0);
    assert(wenzi_iflytek_feed_pcm(client, audio, 1) == -ESHUTDOWN);
    deadline = now_ms() + 2500;
    while (!log.count[WENZI_ASR_DONE] && now_ms() < deadline)
        assert(wenzi_iflytek_poll(client, 2) == 0);
    assert(log.count[WENZI_ASR_DONE] == 1 && log.count[WENZI_ASR_FINAL] == 1);
    assert(!log.count[WENZI_ASR_ERROR] && !log.code && !strcmp(log.final, "确认句子。"));
    for (int i = 0; i < 3; ++i) {
        assert(wenzi_iflytek_poll(client, 0) == 0);
        assert(wenzi_iflytek_finish(client) == 0);
        wenzi_iflytek_cancel(client);
    }
    wenzi_iflytek_destroy(client);
    assert(log.count[WENZI_ASR_DONE] == 1);
    join_mock(&server);
}

static void test_core_failure(enum mock_mode mode)
{
    mock_server server;
    start_mock(&server, mode);
    char url[128];
    struct wenzi_iflytek_config cloud = mock_config(&server, url, sizeof(url), mode != MOCK_TLS_REJECT);
    event_log log = {0};
    wenzi *core = create_core(&log, &cloud, WENZI_CLOUD);
    int rc = wenzi_start(core);
    if (mode < MOCK_TLS_REJECT) {
        assert(rc == 0);
        feed_audio(core);
    }
    int64_t deadline = now_ms() + 2500;
    while (wenzi_get_state(core) != WENZI_IDLE && now_ms() < deadline) {
        pump(core, &log, NULL);
        if (mode == MOCK_TIMEOUT && log.count[WENZI_PARTIAL] == 2 && wenzi_get_state(core) == WENZI_RUNNING)
            assert(wenzi_stop(core) == 0);
    }
    assert(wenzi_get_state(core) == WENZI_IDLE && log.count[WENZI_ERROR] >= 1 && !log.count[WENZI_FINAL]);
    if (mode >= MOCK_TLS_REJECT) assert(!log.count[WENZI_PARTIAL]);
    else {
        assert(log.count[WENZI_PARTIAL] == 2 && !strcmp(log.partial[1], "修订快照"));
        assert(log.code == (mode == MOCK_TIMEOUT ? ETIMEDOUT : ECONNRESET));
    }
    wenzi_ws_destroy(log.ws);
    log.ws = NULL;
    wenzi_destroy(core);
    join_mock(&server);
}

int main(int argc, char **argv)
{
    assert(argc == 1 || (argc == 2 && (!strcmp(argv[1], "hostname") || !strcmp(argv[1], "ws-errors"))));
    assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    setvbuf(stdout, NULL, _IONBF, 0);
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);
    for (int i = 0; i < SAMPLE_COUNT; ++i) audio[i] = (int16_t)(i * 37 - 30000);
    if (argc == 1 || !strcmp(argv[1], "ws-errors")) {
        puts("检查控制 JSON、事件转义与固定假签名");
        test_commands();
        test_event_json();
    }
    if (argc == 2 && !strcmp(argv[1], "ws-errors")) {
        puts("检查真实 WebSocket 的正 errno 与单次核心错误");
        test_ws_errors();
        puts("test_network: OK (JSON + real core/WS errors; no cloud API)");
        return 0;
    }
    make_certificate();
    if (argc == 1) {
        puts("检查真实 WebSocket、核心状态与模拟 WSS RTASR");
        test_ws_and_core();
        test_done_once();
        puts("检查收尾超时、未确认 partial 断线与不受信任 CA");
        test_core_failure(MOCK_TIMEOUT);
        test_core_failure(MOCK_DROP);
        test_core_failure(MOCK_TLS_REJECT);
    }
    puts("检查可信 CA 下的主机名不匹配");
    test_core_failure(MOCK_HOST_REJECT);
    remove_certificate();
    puts("test_network: OK (real core + local WS/WSS mocks; no real cloud API)");
    return 0;
}
