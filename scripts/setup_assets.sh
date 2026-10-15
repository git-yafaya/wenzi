#!/usr/bin/env bash
set -euo pipefail

# 固定官方资产；只下载缺失文件，成功后清理压缩包。
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$root/.deps" "$root/models" "$root/samples"
temporary="$(mktemp -d "$root/.deps/assets-XXXXXX")"
trap 'rm -rf "$temporary"' EXIT
sherpa="$root/.deps/sherpa-onnx"
asr=sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16
punct=sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8

download() {
    curl --fail --location --retry 2 "$1" -o "$2"
    printf '%s  %s\n' "$3" "$2" | sha256sum --check -
}
verify() {
    printf '%s  %s\n' "$1" "$2" | sha256sum --check -
}

if [[ ! -r "$sherpa/include/sherpa-onnx/c-api/c-api.h" ||
      ! -r "$sherpa/lib/libsherpa-onnx-c-api.so" ||
      ! -r "$sherpa/lib/libonnxruntime.so" ]]; then
    download \
        https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.13.8/sherpa-onnx-v1.13.8-linux-x64-shared-no-tts.tar.bz2 \
        "$temporary/sherpa.tar.bz2" d0f96c8b65c6cd0974fada22737e337de81bc8cd2abbec2e39caf358b1eec5fc
    mkdir -p "$sherpa"
    tar -xjf "$temporary/sherpa.tar.bz2" -C "$sherpa" --strip-components=1
fi
verify 2a1b95084be8fd1deb3228fcad2fd3f7f0258b64582f7402281ec174c7b7f4ce "$sherpa/include/sherpa-onnx/c-api/c-api.h"
verify c067b18cd746e49eb5016ee428196d21e1df14c14a3c54f128c1ac95f6aaed50 "$sherpa/lib/libsherpa-onnx-c-api.so"
verify 4b3607aebd1784b26b6f9b20e4bd974c7ab8287043e4d095cb7d2cb40b5e566e "$sherpa/lib/libonnxruntime.so"

if [[ ! -r "$root/models/$asr/encoder-epoch-99-avg-1.int8.onnx" ||
      ! -r "$root/models/$asr/decoder-epoch-99-avg-1.onnx" ||
      ! -r "$root/models/$asr/joiner-epoch-99-avg-1.int8.onnx" ||
      ! -r "$root/models/$asr/tokens.txt" ||
      ! -r "$root/models/$asr/test_wavs/0.wav" ]]; then
    download "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/$asr.tar.bz2" \
        "$temporary/asr.tar.bz2" 2b7c63322b32e5e0f2526043a1103366119ca58dd615cd7105a37c01db9553d7
    tar -xjf "$temporary/asr.tar.bz2" -C "$root/models" \
        "$asr/encoder-epoch-99-avg-1.int8.onnx" "$asr/decoder-epoch-99-avg-1.onnx" \
        "$asr/joiner-epoch-99-avg-1.int8.onnx" "$asr/tokens.txt" "$asr/test_wavs"
fi
verify db6f51551762e40e549166fe041ea3e45464370b595e9ad23f06478ec3794fbb "$root/models/$asr/encoder-epoch-99-avg-1.int8.onnx"
verify 89be509a83175261695bdef5fd1c7b9ab1129a663d1284e7ba9f8507b21e0906 "$root/models/$asr/decoder-epoch-99-avg-1.onnx"
verify bdda356d6f9b8c2d7cee9ee0e26075fa537490f7fd06520be408d287073667b9 "$root/models/$asr/joiner-epoch-99-avg-1.int8.onnx"
verify a8e0e4ec53810e433789b54a5c0134a7eaa2ffca595a6334d54c00da858841d3 "$root/models/$asr/tokens.txt"

if [[ ! -r "$root/models/$punct/model.int8.onnx" ]]; then
    download "https://github.com/k2-fsa/sherpa-onnx/releases/download/punctuation-models/$punct.tar.bz2" \
        "$temporary/punct.tar.bz2" c0d5aa5f8eeb686032345e180bedf39319dc2e0556781c6264bcadba8328a6e1
    tar -xjf "$temporary/punct.tar.bz2" -C "$root/models"
fi
verify 65a3fb9f5ad7bfb96bf69e0dc4481df97f6ee60513c1d94ce981ba6effd524b1 "$root/models/$punct/model.int8.onnx"

if [[ ! -r "$root/samples/zh.wav" ]]; then
    download https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/resolve/204ad334e2e683fd295359930cc16fc0432a23ac/test_wavs/0.wav \
        "$root/samples/zh.wav" 668bf8df51a10027b84d5d8816a1ce11ae93545538dc05cfe2aa6811d399c250
fi
if [[ ! -r "$root/samples/en.wav" ]]; then
    download https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-en-20M-2023-02-17/resolve/d42f2d9f7ca24806fb667456a18a9f1b60f70d16/test_wavs/0.wav \
        "$root/samples/en.wav" 6bc58a4efdf20daac252b6b1502632601a71efe0308f6757dc1eda34891a7e4f
fi
[[ -e "$root/samples/mixed.wav" ]] || ln -s "../models/$asr/test_wavs/0.wav" "$root/samples/mixed.wav"
verify 668bf8df51a10027b84d5d8816a1ce11ae93545538dc05cfe2aa6811d399c250 "$root/samples/zh.wav"
verify 6bc58a4efdf20daac252b6b1502632601a71efe0308f6757dc1eda34891a7e4f "$root/samples/en.wav"
verify 7d93384ca14702cc584a7a33fe2fed92e89e708549161cb12ea38c916882103b "$root/samples/mixed.wav"

# 运行库和模型的许可证来源保留在资产目录。
if [[ ! -r "$sherpa/LICENSE.sherpa-onnx" ]]; then
    download https://raw.githubusercontent.com/k2-fsa/sherpa-onnx/v1.13.8/LICENSE \
        "$sherpa/LICENSE.sherpa-onnx" cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30
fi
if [[ ! -r "$sherpa/LICENSE.onnxruntime" ]]; then
    download https://raw.githubusercontent.com/microsoft/onnxruntime/v1.28.2/LICENSE \
        "$sherpa/LICENSE.onnxruntime" 2f07c72751aed99790b8a4869cf2311df85a860b22ded05fa22803587a48922c
fi
printf 'sherpa-onnx v1.13.8、双语流式模型、标点模型、三段 WAV: OK\n'
