#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
source scripts/env.sh
model="${WENZI_MODEL_DIR:-$root/models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16}"
punctuation="${WENZI_PUNCTUATION_MODEL:-$root/models/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx}"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
mkdir -p build/acceptance
check_exit() {
    local expected="$1" name="$2" status=0
    shift 2
    build/wenzi "$@" > "build/acceptance/$name.stdout" 2> "build/acceptance/$name.stderr" || status=$?
    [[ "$status" == "$expected" ]] || { printf '%s: exit=%s，预期%s\n' "$name" "$status" "$expected" >&2; return 1; }
    [[ -s "build/acceptance/$name.stderr" ]]
}
check_exit 2 invalid_arguments transcribe --wav samples/zh.wav --threads 0
missing="$(mktemp -d "$root/build/acceptance/missing-XXXXXX")"
trap 'rmdir "$missing"' EXIT
check_exit 1 missing_wav transcribe --wav "$missing/absent.wav" --model-dir "$model"
for language in zh en mixed; do
    python3 scripts/check_transcribe.py "samples/$language.wav" "build/acceptance/$language" -- \
        build/wenzi transcribe --wav "samples/$language.wav" --model-dir "$model" \
        --punctuation "$punctuation"
done
python3 scripts/check_ws.py --model-dir "$model"
python3 scripts/check_models.py --model-dir "$model"

# 不同机器的设备名由实际 ALSA 条件确定。
if [[ -n "${WENZI_MIC_DEVICE:-}" ]]; then
    python3 scripts/check_mic.py build/acceptance/mic 2 -- build/wenzi transcribe \
        --mic "$WENZI_MIC_DEVICE" --model-dir "$model" --punctuation "$punctuation"
else
    printf '本次未运行麦克风检查；实际设备用 WENZI_MIC_DEVICE 指定。\n'
fi
printf '集成验收通过；日志保存在 build/acceptance/。\n'
