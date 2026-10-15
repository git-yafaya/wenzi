# Wenzi 命令

在项目目录执行，开发依赖由构建模块准备；`source scripts/env.sh` 设置项目局部运行库路径。`./build/wenzi --help` 是选项的完整来源。

## 本地 WAV

```sh
source scripts/env.sh
./build/wenzi transcribe \
  --model-dir models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16 \
  --wav models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16/test_wavs/0.wav \
  --punctuation models/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx
```

输出为逐行 JSON：partial 是当前句完整快照，final 是确认结果，status 给出状态，error 给出错误。WAV 按采样时间送帧；EOF 完成尾句后退出。也可通过 WENZI_MODEL_DIR、WENZI_PUNCTUATION_MODEL 配置路径，或单独传入 --encoder/--decoder/--joiner/--tokens。

## 麦克风与服务

```sh
./build/wenzi transcribe --mic default --model-dir "$WENZI_MODEL_DIR"
./build/wenzi serve --mic default --model-dir "$WENZI_MODEL_DIR" --port 8765
```

服务默认绑定 127.0.0.1:8765，启动时 idle。客户端发送以下 JSON；WAV 服务验收可用 --wav FILE 替换 --mic default。

```json
{"cmd":"start"}
{"cmd":"status"}
{"cmd":"switch_engine","engine":"cloud"}
{"cmd":"switch_engine","engine":"local"}
{"cmd":"stop"}
```

stop 只停止识别，WebSocket 服务保留，可再次 start。重复 start/stop 幂等。SIGINT/SIGTERM 停止采集，收尾识别后退出；收尾上限由 --drain-timeout-ms 控制，默认 10000。

## 云端

讯飞 RTASR 凭据读取 WENZI_IFLYTEK_APP_ID、WENZI_IFLYTEK_API_KEY。配置后可用 --engine cloud 启动转写；云端 final 保留服务提供的标点。开发验收的 --iflytek-url、--ca-file 用于指定协议端点和 CA，正常使用默认官方地址。

切换会暂停收音，旧引擎收尾并销毁后再创建目标；目标失败时处于 idle，旧句已确认结果保留。状态与失败语义见 architecture.md。

## 检查

```sh
ctest --test-dir build --output-on-failure
```

test_core 使用 fake 识别器检查真实核心的幂等、快照、收尾和错误语义；真实识别由 CLI 的 WAV 验收覆盖。参数错误退出 2，执行错误退出 1，正常 EOF 或信号收尾退出 0。可恢复标点错误仍保留原文 final，并通过 error 明确报告。

退出时 stderr 的 wenzi_statistics 记录接收音频秒数、队列峰值、溢出及未交付采样数；未交付数为采集回调产生的采样数与识别模块接受数之差。本地处理耗时累计 local feed/finish，包含 ASR、标点和同步结果回调，不包含 WAV 节拍等待；模型及标点初始化单独计时。计算实时率使用 local_processing_seconds / (local_samples / 16000)，不使用实时送帧的总墙钟时间。
