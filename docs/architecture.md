# Wenzi 公共接口

自写应用代码使用 C，第三方库通过 C API 调用。公共定义以 `include/wenzi.h` 为准；音频、网络模块先读取此文件再接入。

## PCM 与事件

PCM 固定为 16000 Hz、单声道、有符号 int16_t，内存为主机字节序，外部二进制为 S16LE。单块为 1..320 个采样，末块可更短。feed 成功表示整块已接收；失败结束当前处理，调用方不重试该块。数据只借用至返回。

partial 是当前句完整快照，允许改写；相同快照不重复发送。final 每句最多一次且不再改写，空句不发文本。事件文本为 UTF-8，只在回调期间有效。timestamp 是事件产生的 Unix 秒。session_id 在实际启动后递增，utterance_id 标识句子；旧引擎销毁后不得继续回调。

所有公开核心调用、识别模块回调及事件处理在同一线程；回调内只消费事件，不重入控制函数。返回 0 表示接受，负值为 -errno。通过 status 事件或 get_state 查询是否完成。

## 生命周期

start 在 running 时幂等，在 draining 时返回 -EBUSY。stop 在 idle/draining 时幂等；在运行时禁止继续 feed，等待识别模块真正收尾，进入 idle，服务继续存活。EOF 与 stop 使用同一路径。SIGINT/SIGTERM 触发 stop，收尾后退出。

switch_engine 同引擎时幂等。未编译的目标直接拒绝并保留当前会话。idle 时只更新选定引擎；running 时先收尾并销毁旧引擎，再创建目标，不同时预载两个模型。draining 期间 feed 返回 -EBUSY，收音暂停且不重放 PCM。旧句的 final/error 先于新会话状态；目标初始化失败留在 idle 并报告 error，已确认结果保留。stop 可取消等待中的切换。云端收尾达到 drain_timeout_ms 时报告未完成错误并结束旧会话，不把 partial 当作 final。

本地自然端点发 final 后 reset；EOF/stop/switch 补尾部静音后 InputFinished，排空可解码内容，再发 final 和 DONE。原始 ASR 回调类型定义于公共头；网络模块把增量/修订结果归一化成整句快照。仅 DONE 表示 finish 已完成。

## 模块接入约定

音频接口以 include/wenzi_audio.h 为准：wenzi_audio_read_wav / wenzi_audio_capture_alsa 使用同步回调。采集线程的 on_frame 只复制 PCM，主线程消费 50 帧有界缓冲并推理。音频模块负责 WAV 节拍；WAV 空间不足时在 should_stop 等待，麦克风缓冲满则报告断点并结束识别。取消状态通过 mutex 同步，停止时唤醒并回收采集线程。音频失败通过核心 error 事件转发给终端和服务客户端，再收尾到 idle。

网络接口以 include/wenzi_network.h 为准：云端使用 wenzi_iflytek_create/feed_pcm/finish/poll/destroy，配置为 RTASR 的 app_id/api_key 及超时；创建不回调，销毁后不再回调。WebSocket 使用 wenzi_ws_create/poll/publish/destroy。两者都由核心线程轮询。finish 可异步，最后发 ASR_DONE；终止错误发 ASR_ERROR（负 errno），可恢复错误使用正 errno，DONE 的非零 code 也表示失败。

WebSocket 仅解析 start/stop/switch_engine/status 并调用核心；核心是状态与命令行为的唯一实现。输出 partial/final/error/status，并保留 session_id、utterance_id、engine、state、timestamp。停止识别不会关闭服务。

本地模块及其私有头由核心聊天维护。构建聊天维护 CMakeLists.txt、README、依赖/模型和集成验收；WENZI_WITH_NETWORK 定义为 1 时编译云端与 WebSocket 接入。先链接本地链路，再启用网络。

## 标点

本地 final 在发布前通过 sherpa-onnx 离线 CT-Transformer C API 补标点。partial 保持直接输出。标点加载失败报告 error 并保留原文；云端已有标点时保留云端结果。独立 VAD 根据真实录音需求再接入。
