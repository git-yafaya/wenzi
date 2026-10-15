#ifndef WENZI_NETWORK_H
#define WENZI_NETWORK_H

#include "wenzi.h"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wenzi_ws_server wenzi_ws_server;
typedef struct wenzi_iflytek wenzi_iflytek;

typedef enum {
    WENZI_CMD_START, WENZI_CMD_STOP, WENZI_CMD_SWITCH_ENGINE, WENZI_CMD_STATUS
} wenzi_network_command_type;
typedef struct {
    wenzi_network_command_type type;
    wenzi_engine engine;
} wenzi_network_command;

/* 仅解析协议；命令实际执行和状态始终由核心负责。 */
int wenzi_network_parse_command(const char *json, size_t length,
                               wenzi_network_command *command,
                               char *error, size_t error_size);
/* 成功返回 JSON 字节数，不含结尾 NUL；失败返回负 errno。 */
int wenzi_network_event_json(const wenzi_event *event, char *json, size_t size);

/* core 必须活到服务销毁；NULL 地址绑定 127.0.0.1，port=0 分配空闲端口。 */
wenzi_ws_server *wenzi_ws_create(wenzi *core, const char *address, int port,
                                char *error, size_t error_size);
int wenzi_ws_poll(wenzi_ws_server *server, int timeout_ms);
int wenzi_ws_port(const wenzi_ws_server *server);
int wenzi_ws_publish(wenzi_ws_server *server, const wenzi_event *event);
void wenzi_ws_destroy(wenzi_ws_server *server);

struct wenzi_iflytek_config {
    /* NULL 凭据读取 WENZI_IFLYTEK_APP_ID / WENZI_IFLYTEK_API_KEY。 */
    const char *app_id;
    const char *api_key;
    /* NULL 使用官方 WSS 地址；自定义地址用于本地协议验收。 */
    const char *url;
    const char *ca_file;
    int connect_timeout_ms;
    int drain_timeout_ms;
};

/* 固定假凭据可校验官方签名算法，不访问网络。 */
int wenzi_iflytek_signature(const char *app_id, const char *api_key, time_t ts,
                            char *signature, size_t size);

/* 单线程调用；创建不回调，回调仅在 feed/finish/poll/cancel 内发生。 */
wenzi_iflytek *wenzi_iflytek_create(const struct wenzi_iflytek_config *config,
                                   wenzi_asr_callback callback, void *user,
                                   char *error, size_t error_size);
/* 输入为本机字节序 PCM16mono；内部复制并转为 S16LE，有界队列满即失败。 */
int wenzi_iflytek_feed_pcm(wenzi_iflytek *client, const int16_t *samples,
                           size_t count);
int wenzi_iflytek_finish(wenzi_iflytek *client);
int wenzi_iflytek_poll(wenzi_iflytek *client, int timeout_ms);
void wenzi_iflytek_cancel(wenzi_iflytek *client);
void wenzi_iflytek_destroy(wenzi_iflytek *client);

#ifdef __cplusplus
}
#endif
#endif
