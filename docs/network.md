# 网络模块

应用接口在 `include/wenzi_network.h`。两模块均由核心主线程调用，通过 `poll` 推进，不创建工作线程。核心负责状态、会话编号、时间戳和切换；回调内仅消费事件，不重入控制函数。创建不产生识别回调，销毁后不再回调。

## 本地 WebSocket

`wenzi_ws_create(core, NULL, port, error, size)` 默认只绑定 `127.0.0.1`。端口为 0 时请求空闲端口，实际端口用 `wenzi_ws_port` 查询。CLI 的服务入口和系统信号由核心接线。

输入保留以下控制命令：

```json
{"cmd":"start"}
{"cmd":"stop"}
{"cmd":"switch_engine","engine":"cloud","provider":"iflytek"}
{"cmd":"switch_engine","engine":"local"}
{"cmd":"status"}
```

`provider` 可省略，指定时必须是 cloud 的 iflytek。网络层解析后调用核心，不自行改变运行状态。客户端断开不等同 `stop`；识别会话属于核心，终端和服务端仍可继续使用。

核心事件回调调用 `wenzi_ws_publish`，输出 `partial/final/error/status`，包含核心给出的 `session_id`、`utterance_id`、`engine`、`state`、`timestamp`。partial 的 text 是当前句完整快照，final 每句提交一次；error 使用 message 和正 errno 的 code。内部函数返回值与 ASR 终止错误仍使用负 errno。命令执行时核心已经发布 error，WS 不再补发同一失败。status 反映核心实际状态，stop 保留 WebSocket 服务。

命令输入验证 JSON 类型、UTF-8、长度、重复字段和尾部内容；文本输出正确转义。分片在完整消息后处理；慢客户端达到有界队列上限时断开，音频处理不等待客户端。

## 讯飞 RTASR 标准版

使用 [实时语音转写标准版官方协议](https://static.xfyun.cn/doc/asr/rtasr/API.html)，固定默认地址为 `wss://rtasr.xfyun.cn/v1/ws`，语言参数 `lang=cn&engLangType=1`。这与单次最多 60 秒的 [语音听写流式版](https://static.xfyun.cn/doc/asr/voicedictation/API.html) 属于不同产品。

凭据来自 `wenzi_iflytek_config.app_id/api_key`，NULL 时读取本项目环境变量 `WENZI_IFLYTEK_APP_ID` 和 `WENZI_IFLYTEK_API_KEY`。不用 APISecret。程序不从其他项目寻找密钥，也不把凭据放入 CLI 参数。日志不输出凭据或鉴权 URL。

握手使用 `appid/ts/signa`；`ts` 为当前 Unix 秒，签名为 `Base64(HMAC-SHA1(APIKey, lowercase_hex(MD5(appid + ts))))`。查询参数分别 URL 编码。OpenSSL 提供摘要和 HMAC；libwebsockets 使用正常证书链、主机名验证及 SNI，不启用忽略证书的选项。NULL `ca_file` 使用系统 CA，板端需要提供可用 CA 和准确时钟。

feed 接受本机字节序、16 kHz、单声道 int16_t PCM，复制到五秒容量的队列并转换为 S16LE。按单调时钟每 40 ms 发一包 1280 字节 binary 数据；末包可短。不突发追赶积压；队列满报告 error 和 DONE。持续会话保留真实静音，不能因 VAD 丢弃长段音频，官方 15 秒无音频会断开。

外层 result 的 data 是 JSON 字符串，需要再次解析。按 [官方 JS 示例](https://xfyun-doc.xfyun.cn/static%2F16934793610341218%2Fjs_raasr.zip)，type=1 替换当前 partial，type=0 提交当前整段。保留词和空格，不累计全文；重复的相同 partial、已提交序号的 final 不重复输出。云端 final 已有标点，核心保留结果，避免再次加本地标点。

finish 停止接收 PCM，按节奏排空队列后发送 binary `{"end":true}`，继续接收至服务端正常 WebSocket 关闭握手；直接 TCP 断线不能代替正常完成。默认连接等待 10 秒，收尾等待 5 秒，可通过 config 调整。连接失败、识别错误、未确认文本断线或超时先发 ERROR 再 DONE，不把 partial 升为 final，不自动重连或切换引擎。无音频的 finish 直接取消。cancel 丢弃待发音频并发 DONE；destroy 不再发回调。

每次核心启动新识别会话创建新客户端；旧客户端在回调返回后销毁，避免晚到结果混入新会话。

## 依赖与验收

网络库为 libwebsockets 4.3+（SSL、LEJP、内置 poll）和 OpenSSL。JSON 复用 lws 的 LEJP，不需要 libcurl 或独立 JSON 库。依赖版本及安装、CMake 链接和交叉编译由构建模块统一维护。

连接超时覆盖网络 poll 阶段。若所链接的 lws 未启用原生异步 DNS，创建时的系统域名解析仍由 libc/resolver 管理，不受该超时控制；构建模块负责选择并验证这一库配置。

`tests/test_network.c` 使用固定假签名向量及本地模拟 WS/WSS，检查协议、消息、帧节奏和收尾。模拟不证明真实讯飞账号授权、中英准确率或目标板联网。实际产品开通、APPID/APIKey、时长与并发、IP 白名单、实机 CA/时钟/网络和识别效果需要另外的本项目授权验收。

2026-09-30 PC Linux 模块验收：libwebsockets 4.3.5、OpenSSL 3.5.5、sherpa-onnx 1.13.8，真实核心与网络源码链接，严格 C11 编译无警告。控制 JSON 的字段/类型/重复键/UTF-8/NUL/尾部验证、事件转义往返和固定假签名向量通过；真实本地 WS 客户端完成分片控制、start/stop/switch_engine/status 和实际核心状态核对。

本地模拟 WSS 收到三帧 1280 字节 S16LE，间隔 40–42 ms；嵌套 JSON 的两次 partial 修订、final 去重、binary 结束标识和 DONE 一次通过。收尾超时与未确认文本断线不输出 final；不受信任 CA 和可信证书的主机名不匹配均拒绝，主机名负例显示 `IP address mismatch`。测试证书、私钥和临时二进制已删除，未访问真实讯飞 API。

公开错误码修正后，`test_network ws-errors` 定向检查通过：非法命令回复正 EINVAL，缺模型的 start 只发送一次错误，code 与核心的正 EINVAL 一致；内部负 errno 和收尾语义保持原有约定。严格编译无警告，临时二进制已清理。

已由构建模块接入 `test_network` CMake/CTest 目标；依赖环境就绪后可运行：

```bash
source scripts/env.sh
cmake --build build --target test_network
ctest --test-dir build -R '^network$' --output-on-failure
```
