# Wenzi

C 编写的 Linux 流式语音识别后端，支持本地 sherpa-onnx、讯飞云端和本地 WebSocket 控制。终端及 WebSocket 输出逐行 JSON 事件：`partial`、`final`、`error`、`status`。本地 final 可使用 CT-Transformer 补标点。

## PC Linux 构建

本次局部依赖准备脚本用于 Ubuntu 26.04、x86_64。需要已有 GCC/G++、make、pkg-config、OpenSSL 开发文件、curl、apt-get、dpkg-deb、tar、patch、rg 和 sha256sum；Python 3 只用于集成验收脚本。

```bash
cd /home/yafaya/App/wenzi
bash scripts/setup_deps.sh
bash scripts/setup_assets.sh
bash scripts/build_sherpa_patch.sh
source scripts/env.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

`setup_deps.sh` 将 CMake 4.2.3、ALSA 1.2.15.3、libwebsockets 4.3.5 放在 `.deps/host/usr`；OpenSSL 复用系统安装。`setup_assets.sh` 准备 sherpa-onnx v1.13.8 C API、双语流式 Zipformer、标点模型和官方 WAV。已有安装及模型文件会复用。

`build_sherpa_patch.sh` 使用固定第三方源码重建 C API 到 `.deps/sherpa-patch-stage`，应用默认链接此目录。补丁让已验证的模型初始化失败返回 NULL，并修正本次实际遇到的错误模型元数据路径；补丁文件在 `cmake/sherpa-c-api-exceptions.patch`。ONNX Runtime 使用官方预编译库。

## 运行

```bash
source scripts/env.sh
export WENZI_MODEL_DIR="$PWD/models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16"
export WENZI_PUNCTUATION_MODEL="$PWD/models/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx"

build/wenzi transcribe --wav samples/mixed.wav
build/wenzi serve --wav samples/mixed.wav --bind 127.0.0.1 --port 8765
```

WAV 输入必须是 16000 Hz、单声道、PCM16；按录音时长送帧。`serve` 启动后等待 WebSocket `start`，`stop` 收尾后服务仍保持运行。控制消息及选项见 [commands.md](docs/commands.md)。

本机已验证的麦克风设备为 `plughw:CARD=Generic,DEV=0`，实际机器需换成自己的 ALSA 设备名：

```bash
build/wenzi transcribe --mic plughw:CARD=Generic,DEV=0
```

`Ctrl+C` 请求收尾退出。识别事件写 stdout，运行日志写 stderr。云端凭据从 `WENZI_IFLYTEK_APP_ID`、`WENZI_IFLYTEK_API_KEY` 读取；真实云端连接需另外验收。

## 集成与部署

```bash
bash scripts/accept.sh
```

设置 `WENZI_MIC_DEVICE=plughw:CARD=Generic,DEV=0` 后运行同一命令，会一并验收本机麦克风。

构建、安装和交叉编译参数见 [build.md](docs/build.md)，实测结果及证据边界见 [acceptance.md](docs/acceptance.md)，模块契约见 [architecture.md](docs/architecture.md)。开发板与 SDK 尚未确定，当前运行结果仅适用于本次 PC。
