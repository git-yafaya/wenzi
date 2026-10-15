#define _POSIX_C_SOURCE 200809L
#include "wenzi_network.h"
#include "network_json.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define WS_COMMAND_MAX 1024
#define WS_MESSAGE_MAX 65536
#define WS_QUEUE_BYTES 65536
#define WS_QUEUE_COUNT 64

struct command_parser {
    wenzi_network_command command;
    char cmd[32];
    char engine[16];
    char provider[16];
    const char *error;
    unsigned seen;
    unsigned field;
    int depth;
};

static int command_visit(void *user, const struct lejp_ctx *ctx,
                         unsigned char reason, const char *value, size_t length)
{
    struct command_parser *parser = user;
    char *target;
    size_t capacity;
    unsigned field;

    switch (reason) {
    case LEJPCB_OBJECT_START:
        if (parser->depth || ctx->path[0])
            break;
        parser->depth = 1;
        return 0;
    case LEJPCB_OBJECT_END:
        if (parser->depth != 1)
            break;
        parser->depth = 0;
        return 0;
    case LEJPCB_PAIR_NAME:
        if (!strcmp(ctx->path, "cmd"))
            field = 1;
        else if (!strcmp(ctx->path, "engine"))
            field = 2;
        else if (!strcmp(ctx->path, "provider"))
            field = 4;
        else {
            parser->error = "未知命令字段";
            return -EINVAL;
        }
        if (parser->seen & field) {
            parser->error = "命令字段重复";
            return -EINVAL;
        }
        parser->seen |= field;
        parser->field = field;
        return 0;
    case LEJPCB_VAL_STR_END:
        if (parser->depth != 1 || !parser->field)
            break;
        if (parser->field == 1) {
            target = parser->cmd;
            capacity = sizeof(parser->cmd);
        } else if (parser->field == 2) {
            target = parser->engine;
            capacity = sizeof(parser->engine);
        } else {
            target = parser->provider;
            capacity = sizeof(parser->provider);
        }
        if (length >= capacity) {
            parser->error = "命令字段过长";
            return -E2BIG;
        }
        memcpy(target, value, length + 1);
        parser->field = 0;
        return 0;
    default:
        break;
    }
    parser->error = "命令必须为扁平 JSON 对象，字段必须为字符串";
    return -EINVAL;
}

static int protocol_error(char *error, size_t size, int code, const char *message)
{
    if (error && size)
        snprintf(error, size, "%s", message);
    return code;
}

int wenzi_network_parse_command(const char *json, size_t length,
                               wenzi_network_command *command,
                               char *error, size_t error_size)
{
    struct command_parser parser = {0};
    char scratch[WS_COMMAND_MAX + 1];
    int result;

    if (error && error_size)
        error[0] = '\0';
    if (!command)
        return protocol_error(error, error_size, -EINVAL, "命令输出为空");
    if (length > WS_COMMAND_MAX)
        return protocol_error(error, error_size, -E2BIG, "命令超过 1024 字节");
    result = wenzi_json_parse(json, length, scratch, sizeof(scratch),
                              command_visit, &parser);
    if (result < 0)
        return protocol_error(error, error_size, result,
                              parser.error ? parser.error : "无效 JSON 或 UTF-8 命令");
    if (!(parser.seen & 1))
        return protocol_error(error, error_size, -EINVAL, "缺少 cmd 字段");
    if (!strcmp(parser.cmd, "start"))
        parser.command.type = WENZI_CMD_START;
    else if (!strcmp(parser.cmd, "stop"))
        parser.command.type = WENZI_CMD_STOP;
    else if (!strcmp(parser.cmd, "status"))
        parser.command.type = WENZI_CMD_STATUS;
    else if (!strcmp(parser.cmd, "switch_engine"))
        parser.command.type = WENZI_CMD_SWITCH_ENGINE;
    else
        return protocol_error(error, error_size, -EINVAL, "未知 cmd 命令");
    if (parser.command.type == WENZI_CMD_SWITCH_ENGINE) {
        if (!strcmp(parser.engine, "local"))
            parser.command.engine = WENZI_LOCAL;
        else if (!strcmp(parser.engine, "cloud"))
            parser.command.engine = WENZI_CLOUD;
        else
            return protocol_error(error, error_size, -EINVAL, "engine 必须为 local 或 cloud");
        if ((parser.seen & 4) && (parser.command.engine != WENZI_CLOUD ||
                                  strcmp(parser.provider, "iflytek")))
            return protocol_error(error, error_size, -EINVAL, "provider 仅支持 cloud/iflytek");
    } else if (parser.seen & 6) {
        return protocol_error(error, error_size, -EINVAL, "此命令不接受 engine/provider");
    }
    *command = parser.command;
    return 0;
}

int wenzi_network_event_json(const wenzi_event *event, char *json, size_t size)
{
    static const char *const types[] = {"partial", "final", "error", "status"};
    static const char *const reasons[] = {"endpoint", "eof", "stop", "switch"};
    const char *text;
    double stamp;
    uint64_t seconds;
    unsigned micros;
    size_t used;
    int written;

    if (!event || !json || !size)
        return -EINVAL;
    json[0] = '\0';
    if ((unsigned)event->type > WENZI_STATUS ||
        (unsigned)event->engine > WENZI_CLOUD ||
        (unsigned)event->state > WENZI_DRAINING || !isfinite(event->timestamp))
        return -EINVAL;
    stamp = event->timestamp < 0 ? -event->timestamp : event->timestamp;
    if (stamp >= (double)UINT64_MAX)
        return -EINVAL;
    seconds = (uint64_t)stamp;
    micros = (unsigned)((stamp - (double)seconds) * 1000000.0);
    /* 整数拼时间，避免 LC_NUMERIC 改变 JSON 小数点。 */
    written = snprintf(json, size,
                       "{\"type\":\"%s\",\"session_id\":%" PRIu64
                       ",\"utterance_id\":%" PRIu64
                       ",\"engine\":\"%s\",\"state\":\"%s\",\"timestamp\":%s%"
                       PRIu64 ".%06u,\"code\":%d",
                       types[event->type], event->session_id, event->utterance_id,
                       wenzi_engine_name(event->engine), wenzi_state_name(event->state),
                       event->timestamp < 0 ? "-" : "", seconds, micros, event->code);
    if (written < 0 || (size_t)written >= size)
        return -ENOSPC;
    used = (size_t)written;
    if (event->type == WENZI_FINAL) {
        if ((unsigned)event->reason > WENZI_END_SWITCH)
            return -EINVAL;
        written = snprintf(json + used, size - used, ",\"reason\":\"%s\"",
                           reasons[event->reason]);
        if (written < 0 || (size_t)written >= size - used)
            return -ENOSPC;
        used += (size_t)written;
    }
    if (event->type != WENZI_STATUS) {
        written = snprintf(json + used, size - used, ",\"%s\":\"",
                           event->type == WENZI_ERROR ? "message" : "text");
        if (written < 0 || (size_t)written >= size - used)
            return -ENOSPC;
        used += (size_t)written;
        text = event->text ? event->text : "";
        written = wenzi_json_escape(text, strlen(text), json + used, size - used);
        if (written < 0)
            return written;
        used += (size_t)written;
        if (size - used < 2)
            return -ENOSPC;
        json[used++] = '"';
    }
    if (size - used < 2)
        return -ENOSPC;
    json[used++] = '}';
    json[used] = '\0';
    return used > INT_MAX ? -E2BIG : (int)used;
}

struct ws_message {
    struct ws_message *next;
    size_t length;
    unsigned char data[];
};

struct ws_client {
    struct ws_client *next;
    struct lws *wsi;
    struct ws_message *head;
    struct ws_message *tail;
    size_t queued_bytes;
    size_t queued_count;
    size_t input_length;
    int receiving;
    int closing;
    char input[WS_COMMAND_MAX + 1];
};

struct wenzi_ws_server {
    wenzi *core;
    struct lws_context *context;
    struct ws_client *clients;
    lws_sorted_usec_list_t poll_timer;
    uint64_t session_id;
    uint64_t utterance_id;
    uint64_t statuses;
    uint64_t errors;
    int port;
};

static void free_messages(struct ws_client *client)
{
    while (client->head) {
        struct ws_message *message = client->head;

        client->head = message->next;
        free(message);
    }
    client->tail = NULL;
    client->queued_bytes = client->queued_count = 0;
}

static int queue_message(struct ws_client *client, const char *json, size_t length)
{
    struct ws_message *message;
    int result;

    if (client->closing)
        return -ENOBUFS;
    if (length > WS_QUEUE_BYTES - client->queued_bytes ||
        client->queued_count >= WS_QUEUE_COUNT) {
        result = -ENOBUFS;
        goto close;
    }
    message = malloc(sizeof(*message) + LWS_PRE + length);
    if (!message) {
        result = -ENOMEM;
        goto close;
    }
    message->next = NULL;
    message->length = length;
    memcpy(message->data + LWS_PRE, json, length);
    if (client->tail)
        client->tail->next = message;
    else
        client->head = message;
    client->tail = message;
    client->queued_bytes += length;
    ++client->queued_count;
    lws_callback_on_writable(client->wsi);
    return 0;
close:
    /* 队列满就断开，让客户端明确重连；final 不静默丢弃。 */
    client->closing = 1;
    lws_callback_on_writable(client->wsi);
    lws_set_timeout(client->wsi, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_ASYNC);
    return result;
}

static int reply(wenzi_ws_server *server, struct ws_client *client,
                 int code, const char *text)
{
    struct timespec now;
    char json[2048];
    wenzi_event event = {
        .type = code < 0 ? WENZI_ERROR : WENZI_STATUS,
        .engine = wenzi_get_engine(server->core),
        .state = wenzi_get_state(server->core),
        .session_id = server->session_id,
        .utterance_id = server->utterance_id,
        .text = text, .code = code < 0 ? -code : code
    };
    int length;

    clock_gettime(CLOCK_REALTIME, &now);
    event.timestamp = now.tv_sec + now.tv_nsec / 1000000000.0;
    length = wenzi_network_event_json(&event, json, sizeof(json));
    return length < 0 ? length : queue_message(client, json, (size_t)length);
}

static int handle_command(wenzi_ws_server *server, struct ws_client *client)
{
    wenzi_network_command command;
    char error[256];
    uint64_t statuses = server->statuses;
    uint64_t errors = server->errors;
    int result = wenzi_network_parse_command(client->input, client->input_length,
                                              &command, error, sizeof(error));

    if (result < 0)
        return reply(server, client, result, error);
    switch (command.type) {
    case WENZI_CMD_START:
        result = wenzi_start(server->core);
        break;
    case WENZI_CMD_STOP:
        result = wenzi_stop(server->core);
        break;
    case WENZI_CMD_SWITCH_ENGINE:
        result = wenzi_switch_engine(server->core, command.engine);
        break;
    case WENZI_CMD_STATUS:
        wenzi_status(server->core);
        result = 0;
        break;
    }
    if (result < 0) {
        /* 核心已发布此命令的错误时，不重复补发。 */
        if (errors != server->errors)
            return 0;
        snprintf(error, sizeof(error), "命令执行失败: %s", strerror(-result));
        return reply(server, client, result, error);
    }
    /* 同步状态已由核心回调发布时，不再补发一份。 */
    if (statuses == server->statuses && command.type != WENZI_CMD_STATUS)
        wenzi_status(server->core);
    return statuses == server->statuses ? reply(server, client, 0, NULL) : 0;
}

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                        void *user, void *input, size_t length)
{
    wenzi_ws_server *server = lws_context_user(lws_get_context(wsi));
    struct ws_client *client = user;

    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
        client->wsi = wsi;
        client->next = server->clients;
        server->clients = client;
        break;
    case LWS_CALLBACK_RECEIVE:
        if (client->closing)
            return -1;
        if (!client->receiving) {
            if (lws_frame_is_binary(wsi)) {
                lws_close_reason(wsi, LWS_CLOSE_STATUS_UNACCEPTABLE_OPCODE,
                                 (unsigned char *)"text commands only", 18);
                return -1;
            }
            client->receiving = 1;
            client->input_length = 0;
        }
        if (length > WS_COMMAND_MAX - client->input_length) {
            lws_close_reason(wsi, LWS_CLOSE_STATUS_MESSAGE_TOO_LARGE,
                             (unsigned char *)"command too large", 17);
            return -1;
        }
        memcpy(client->input + client->input_length, input, length);
        client->input_length += length;
        if (lws_is_final_fragment(wsi) && !lws_remaining_packet_payload(wsi)) {
            client->input[client->input_length] = '\0';
            client->receiving = 0;
            if (handle_command(server, client) < 0)
                return -1;
        }
        break;
    case LWS_CALLBACK_SERVER_WRITEABLE: {
        struct ws_message *message;
        int written;

        if (client->closing) {
            lws_close_reason(wsi, LWS_CLOSE_STATUS_POLICY_VIOLATION,
                             (unsigned char *)"output queue full", 17);
            return -1;
        }
        message = client->head;
        if (!message)
            break;
        written = lws_write(wsi, message->data + LWS_PRE,
                            message->length, LWS_WRITE_TEXT);
        if (written < 0 || (size_t)written != message->length)
            return -1;
        client->head = message->next;
        if (!client->head)
            client->tail = NULL;
        client->queued_bytes -= message->length;
        --client->queued_count;
        free(message);
        if (client->head)
            lws_callback_on_writable(wsi);
        break;
    }
    case LWS_CALLBACK_CLOSED: {
        struct ws_client **link = &server->clients;

        while (*link && *link != client)
            link = &(*link)->next;
        if (*link)
            *link = client->next;
        free_messages(client);
        break;
    }
    case LWS_CALLBACK_HTTP:
        return -1;
    default:
        break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    {.name = "wenzi", .callback = ws_callback,
     .per_session_data_size = sizeof(struct ws_client),
     .rx_buffer_size = WS_COMMAND_MAX},
    {0}
};

wenzi_ws_server *wenzi_ws_create(wenzi *core, const char *address, int port,
                                char *error, size_t error_size)
{
    struct lws_context_creation_info info = {0};
    struct lws_vhost *vhost;
    wenzi_ws_server *server;

    if (error && error_size)
        error[0] = '\0';
    if (!core || port < 0 || port > 65535) {
        errno = EINVAL;
        protocol_error(error, error_size, -EINVAL, "无效核心或端口");
        return NULL;
    }
    server = calloc(1, sizeof(*server));
    if (!server) {
        protocol_error(error, error_size, -ENOMEM, "WebSocket 服务内存不足");
        return NULL;
    }
    server->core = core;
    info.iface = address ? address : "127.0.0.1";
    info.port = port;
    info.protocols = protocols;
    info.user = server;
    info.gid = info.uid = -1;
    info.vhost_name = "wenzi";
    info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;
    errno = 0;
    server->context = lws_create_context(&info);
    if (!server->context) {
        int code = errno ? errno : EIO;

        free(server);
        errno = code;
        if (error && error_size)
            snprintf(error, error_size, "WebSocket 绑定失败: %s", strerror(code));
        return NULL;
    }
    vhost = lws_get_vhost_by_name(server->context, "wenzi");
    server->port = vhost ? lws_get_vhost_listen_port(vhost) : -1;
    if (server->port <= 0) {
        wenzi_ws_destroy(server);
        errno = EIO;
        protocol_error(error, error_size, -EIO, "无法取得监听端口");
        return NULL;
    }
    return server;
}

static void poll_wakeup(lws_sorted_usec_list_t *timer)
{
    wenzi_ws_server *server = lws_container_of(timer, wenzi_ws_server, poll_timer);

    lws_cancel_service(server->context);
}

int wenzi_ws_poll(wenzi_ws_server *server, int timeout_ms)
{
    int result;

    if (!server || timeout_ms < 0)
        return -EINVAL;
    /* 新版 lws 忽略 service 超时，库内定时器负责唤醒。 */
    if (timeout_ms)
        lws_sul_schedule(server->context, 0, &server->poll_timer, poll_wakeup,
                         (lws_usec_t)timeout_ms * 1000);
    else
        lws_cancel_service(server->context);
    result = lws_service(server->context, 0);
    lws_sul_cancel(&server->poll_timer);
    return result < 0 ? -EIO : 0;
}

int wenzi_ws_port(const wenzi_ws_server *server)
{
    return server ? server->port : -EINVAL;
}

int wenzi_ws_publish(wenzi_ws_server *server, const wenzi_event *event)
{
    char json[WS_MESSAGE_MAX];
    struct ws_client *client;
    int length, result = 0;

    if (!server || !event)
        return -EINVAL;
    length = wenzi_network_event_json(event, json, sizeof(json));
    if (length < 0) {
        /* 输出不完整时结束连接，不能让 final 消失在已连接会话中。 */
        for (client = server->clients; client; client = client->next) {
            client->closing = 1;
            lws_callback_on_writable(client->wsi);
            lws_set_timeout(client->wsi, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_ASYNC);
        }
        return length;
    }
    server->session_id = event->session_id;
    server->utterance_id = event->utterance_id;
    if (event->type == WENZI_STATUS)
        ++server->statuses;
    if (event->type == WENZI_ERROR)
        ++server->errors;
    for (client = server->clients; client; client = client->next) {
        int queued = queue_message(client, json, (size_t)length);

        if (queued < 0)
            result = queued;
    }
    return result;
}

void wenzi_ws_destroy(wenzi_ws_server *server)
{
    if (!server)
        return;
    lws_sul_cancel(&server->poll_timer);
    if (server->context)
        lws_context_destroy(server->context);
    free(server);
}
