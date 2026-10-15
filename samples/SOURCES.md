# 测试音频来源与验证

获取与验证日期：2026-09-30。三段音频均为 16000 Hz、单声道、16-bit PCM WAV。

| 文件 | 语言 | 时长（秒） | SHA256 |
| --- | --- | ---: | --- |
| `zh.wav` | 中文 | 5.6115 | `668bf8df51a10027b84d5d8816a1ce11ae93545538dc05cfe2aa6811d399c250` |
| `en.wav` | 英文 | 6.625 | `6bc58a4efdf20daac252b6b1502632601a71efe0308f6757dc1eda34891a7e4f` |
| `mixed.wav` | 中英混说 | 10.053125 | `7d93384ca14702cc584a7a33fe2fed92e89e708549161cb12ea38c916882103b` |

来源与许可证：

- `zh.wav`：官方维护者的[中文 14M 模型仓库](https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/tree/204ad334e2e683fd295359930cc16fc0432a23ac)中 [test_wavs/0.wav](https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/204ad334e2e683fd295359930cc16fc0432a23ac/test_wavs/0.wav)，仓库模型卡声明 `Apache-2.0`。
- `en.wav`：官方维护者的[英文 20M 模型仓库](https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-en-20M-2023-02-17/tree/d42f2d9f7ca24806fb667456a18a9f1b60f70d16)中 [test_wavs/0.wav](https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-en-20M-2023-02-17/resolve/d42f2d9f7ca24806fb667456a18a9f1b60f70d16/test_wavs/0.wav)，仓库模型卡声明 `Apache-2.0`。
- `mixed.wav`：链接到官方已下载模型包的 `models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16/test_wavs/0.wav`。同一文件也发布在[维护者的配套模型仓库](https://huggingface.co/csukuangfj/sherpa-ncnn-streaming-zipformer-small-bilingual-zh-en-2023-02-16/tree/5fb2fb06fe78a776dbda78226c42d435359064f7/test_wavs)，模型卡声明 `Apache-2.0`。本地文件的 Git blob SHA1 与该固定版本的 `test_wavs/0.wav` 相同：`6ef8fc2940e6b87d14ccd80274a50465ad978b35`。

实际识别检查使用 `.deps/sherpa-onnx/bin/sherpa-onnx`、`model_type=zipformer`、CPU 2 线程，模型四个文件见 `models/SOURCES.md`。三个输入均成功返回文本；以下是本机模型输出，不是人工标准转写：

```text
zh.wav: 对我做了介绍那么我想说的是大家如果对我的研究感兴趣呢
en.wav: AFTER EARLY NIGHTFALL THE YELLOW LAMPS WOULD LIGHT UP HERE AND THERE THE SQUALID QUARTER OF THE BROWS
mixed.wav: 昨天是 MONDAY TODAY IS THEY AFTER TOMORROW是星期三
```

本次单次运行的识别 RTF 约为中文 `0.019`、英文 `0.019`、混说 `0.054`；模型初始化约 `0.23–0.24` 秒。该记录用于确认本机库和模型能够运行。

标点检查使用 `.deps/sherpa-onnx/bin/sherpa-onnx-offline-punctuation`，CT-Transformer int8 模型、CPU 1 线程，成功返回：

```text
输入：这是一个测试你好吗How are you我很好thank you are you ok谢谢你
输出：这是一个测试，你好吗？How are you？我很好？thank you，are you ok，谢谢你。
```
