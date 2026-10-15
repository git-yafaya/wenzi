# PC 集成验收记录

日期：2026-09-30。环境：Ubuntu 26.04.1 x86_64、glibc 2.43、AMD Ryzen 9 7945HX、约 14.8 GiB RAM；GCC 15.2.0、CMake 4.2.3、ALSA 1.2.15.3、OpenSSL 3.5.5、libwebsockets 4.3.5。应用源码全部 C，第三方通过 C API 调用。

## 真实 WAV 与标点

使用 sherpa-onnx v1.13.8、双语流式 Zipformer（int8 encoder/joiner、fp32 decoder）、CT-Transformer int8 标点，CPU 1 线程。输入按 16000 Hz 单声道 PCM16 的实际音频时长送帧。样本固定来源和 SHA256 在 `samples/SOURCES.md`。

```bash
source scripts/env.sh
model=models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16
punct=models/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx
python3 scripts/check_transcribe.py samples/mixed.wav build/acceptance/mixed -- \
  build/wenzi transcribe --wav samples/mixed.wav --model-dir "$model" --punctuation "$punct"
```

三段输入退出 0；均在 EOF 前输出 partial，final 的会话/句子编号不重复，尾句 final 先于 idle。下面为本次单次观察，尚未制定性能 SLA：

| 样本 | 音频秒数 | 初始化 ms | 首 partial ms | EOF 收尾到 final ms | 本地处理 RTF | CPU % | 峰值 RSS MiB | partial/final |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| 中文 | 5.6115 | 574.9 | 748.9 | 13.4 | 0.0274 | 12.7 | 252.3 | 10 / 1 |
| 英文 | 6.6250 | 569.4 | 748.9 | 10.7 | 0.0260 | 22.5 | 252.3 | 18 / 1 |
| 混合 | 10.0531 | 580.4 | 1070.0 | 9.2 | 0.0254 | 42.8 | 252.3 | 13 / 3 |

三段 PCM 队列峰值均为 1 帧，`overflow_samples=0`、`undelivered_samples=0`，接收采样数与 WAV 一致。每段的事件、stderr 和指标保存在 `build/acceptance/{zh,en,mixed}/`。

中文、英文和下面的服务/麦克风记录在官方预编译 C API 上采集；混合项是第三方补丁库接入后的有效模型回归。各项均为单次观察，不用于比较库版本之间的性能。

测量定义：初始化来自应用的 `local_initialization_seconds`；首 partial 是 Python 观察到 running 后至首 partial 的单调时钟差；EOF final 是观察到 draining 后至尾句 final 的差。RTF 为 `local_processing_seconds / (local_samples / 16000)`，包含 ASR、标点及同步回调，排除送帧节拍等待。CPU 是进程累计 user+system 时间除以整个运行墙钟时间，包含加载；峰值 RSS 使用 Linux 子进程资源统计。软件队列计数不能替代设备层丢帧测量。

实际 final 文本：

```text
中文：对我做了介绍，我想说的是大家如果对我的研究感兴趣。
英文：AFTER EARLY NIGHTFALL，THE YELLOW LAMPS WOULD LIGHT UP HERE AND THERE THE SQUALID QUARTER OF THE BROWS。
混合：昨天是MONDAY。 / TODAY IS。 / THEY AFTER TOMORROW是星期三。
```

这是模型实际输出，含错词或漏词；目前没有人工标准转写和准确率指标，链路通过不代表识别质量达标。

## 真实服务与麦克风

`scripts/check_ws.py` 启动完整 `wenzi serve`，使用实际模型与 mixed.wav，通过真实本地 WS 检查 status、start/stop 幂等、同引擎切换、实时 partial、stop 尾句、停止后服务继续响应。正常路径 SIGINT 退出 0。清空凭据后切 cloud 产生 ENOKEY，回到 local 后会话编号递增并恢复识别；该错误路径最终退出 1，符合 CLI 约定。检查强制使用本地云端测试地址，没有访问收费 API。

同一个服务连续完成 6 次完整混合 WAV，持续 62.9 秒，共 84 个 partial、18 个 final，EOF 后均能 status 和重新 start，SIGINT 退出 0。观察峰值 RSS 153.0 MiB，EOF 后 RSS 在 88.3–142.2 MiB 间变化；这个短时记录不能证明长时间内存稳定。命令、事件和摘要在 `build/acceptance/ws/summary.json`。

完整 CLI 的真实 ALSA 输入使用 `plughw:CARD=Generic,DEV=0`，启动后采集 2 秒再发送 SIGINT，实际送入 31,040 个采样（1.94 秒），退出 0、最终 idle，软件队列峰值 1 帧、无溢出或未交付采样。该次未提供可核对的口述内容，只证明采集、模型送入和信号收尾接线。记录在 `build/acceptance/mic/summary.json`。

```bash
python3 scripts/check_ws.py --model-dir "$model"
python3 scripts/check_mic.py build/acceptance/mic 2 -- \
  build/wenzi transcribe --mic plughw:CARD=Generic,DEV=0 --model-dir "$model" --punctuation "$punct"
```

## 检查与模拟证据

- WAV 检查通过：额外 chunk、padding、短尾帧、截断、格式拒绝、取消和信号等待。
- 核心检查通过：fake ASR 驱动真实核心，验证快照、状态、幂等、收尾及超时；只证明核心逻辑。
- 网络检查通过：真实核心及网络代码，使用本地 WS 和假凭据模拟 WSS，覆盖控制、JSON、修订、final 去重、40–42 ms 音频帧、结束标识、断线/超时、CA 和主机名拒绝。详见 `docs/network.md`。
- CLI 参数错误退出 2，缺失 WAV 退出 1；后者 stdout 有正错误码 JSON error，stderr 有输入失败日志。

构建固定链接所有应用模块，`WENZI_WITH_NETWORK=1`；core 单测中的 fake ASR 不进入应用。libwebsockets 的原生异步 DNS 已启用，重建后网络检查通过。

## 第三方初始化缺口

官方预编译 C API 已复现两个 SIGABRT：可读文本文件作为标点或 encoder 导致 Ort::Exception 越过 C API。另将有效 encoder 当标点时强制退出 255，将 decoder 当 encoder 时 SIGSEGV。初始证据在 `build/acceptance/model-negative.json` 和 `model-wrong-type.json`。

源码补丁为 `cmake/sherpa-c-api-exceptions.patch`，固定在 sherpa-onnx v1.13.8。只修改第三方三个文件：`c-api.cc` 的 OnlineRecognizer、OnlineStream、OfflinePunctuation 创建入口捕获异常并返回 NULL、设置 errno；两个模型包装对象使用完整初始化；CT-Transformer 的已验证必要元数据缺失、online transducer 工厂空指针在使用前抛异常。应用接口保持不变，自写应用源码仍全部 C。

`scripts/build_sherpa_patch.sh` 首轮配置、依赖下载和编译实测 139 秒（主要源归档先前已缓存），没有编译 ONNX Runtime。产物在 `.deps/sherpa-patch-stage`，ORT 与原始官方库 SHA256 相同，C API RUNPATH 仅 `$ORIGIN`、动态依赖无缺失；来源、补丁与产物摘要、许可证保留在该目录。产物和当前补丁摘要匹配时，脚本直接复用。

`scripts/check_models.py` 用独立 Wenzi CLI 进程重跑原始四个负例，均退出 1、发正 EINVAL（22）JSON error。两个标点失败均继续输出原文 final；两个 ASR 初始化失败不输出 final。没有 SIGABRT/SIGSEGV 或直接退出 255。实际输出及 stderr 在 `build/acceptance/model-errors/summary.json` 和相邻日志；有效 mixed.wav 加标点回归退出 0，指标见上表。

该验证覆盖本次已复现的解析异常、标点元数据缺失和 ASR 工厂空指针；没有验证任意模型类别或任意 ONNX 文件。第三方源补丁没有给未使用的模型接口增加行为承诺。

补丁接入后的现有 network 目标通过，未重跑无关长音频。下载归档、第三方临时源码/编译目录、补丁原文副本和临时模型文件已清理；清理后重建脚本仍能直接复用已验证产物。实际运行库、头文件、模型、样本、安装目录、复现脚本和验收日志保留。

## 本机安装

按 `docs/build.md` 生成 `install/bin` 和 `install/lib` 后，清除 `LD_LIBRARY_PATH` 的 `wenzi --help` 通过；`ldd` 无缺失，C API、ORT、ALSA 和 lws 指向安装目录，应用 RUNPATH 为 `$ORIGIN/../lib`。这证明本机安装布局与动态加载，不能证明其他架构或 libc 上可运行。

## 外部待验项

真实讯飞账号授权、产品配置、联网和中英识别效果未验收；本次只运行本地模拟。独立 VAD/降噪尚未单独接入，当前本地分句使用 ASR 自带端点检测。开发板和 SDK 未确定；没有实际交叉构建或实板运行证据。板端需补 CPU/ABI/libc/工具链/sysroot/音频条件，并重新测加载、延迟、RTF、CPU/RSS、积压、丢帧及持续运行。
