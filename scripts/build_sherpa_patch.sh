#!/usr/bin/env bash
set -euo pipefail

# 固定上游源码；产物只写独立目录。
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source "$root/scripts/env.sh"
work="$root/.deps/sherpa-build"
source_dir="$work/source"
build_dir="$work/build"
stage="$root/.deps/sherpa-patch-stage"
patch_file="$root/cmake/sherpa-c-api-exceptions.patch"
commit=11afbd009a7f8c08f4bcf2fc1b265d0df4670fbf
source_sha=0a8db6c55dd318f4a688faba85f7760b99a6c92e8ef8864479d418531bee1ac2
ort_sha=c4f8994d56191d9d2c92a961b39fe790459f2c5d155f912b239506ea31359534
ort_file=onnxruntime-linux-x64-glibc2_17-Release-1.28.2.zip
started=$SECONDS

# 当前补丁和已安装文件未变时直接复用。
if [[ -r "$stage/SHA256SUMS" && -r "$stage/build-info.txt" &&
      -r "$stage/share/licenses/sherpa-onnx/LICENSE" &&
      -r "$stage/share/licenses/onnxruntime/LICENSE" ]] &&
   sha256sum --status --check "$stage/SHA256SUMS"; then
    printf '复用 C API stage: %s\n' "$stage"
    exit 0
fi
mkdir -p "$work/downloads" "$source_dir" "$build_dir" "$stage/lib"

# 失败保留缓存，成功的归档每次核验。
download() {
    local file="$work/downloads/$1" sha="$2" url="$3"
    if [[ ! -f "$file" ]]; then
        curl --fail --location --retry 2 --connect-timeout 15 --max-time 180 \
            "$url" -o "$file.part"
        mv "$file.part" "$file"
    fi
    printf '%s  %s\n' "$sha" "$file" | sha256sum --check -
}
download sherpa-onnx-v1.13.8.tar.gz "$source_sha" \
    "https://codeload.github.com/k2-fsa/sherpa-onnx/tar.gz/$commit"
download "$ort_file" "$ort_sha" \
    "https://github.com/csukuangfj/onnxruntime-libs/releases/download/v1.28.2/$ort_file"
if [[ ! -f "$source_dir/CMakeLists.txt" ]]; then
    tar -xzf "$work/downloads/sherpa-onnx-v1.13.8.tar.gz" \
        -C "$source_dir" --strip-components=1
fi
if patch --batch --forward --dry-run -p1 -d "$source_dir" < "$patch_file" >/dev/null; then
    patch --batch --forward -p1 -d "$source_dir" < "$patch_file"
else
    patch --batch --reverse --dry-run -p1 -d "$source_dir" < "$patch_file" >/dev/null
fi
ln -sf "$work/downloads/$ort_file" "$build_dir/$ort_file"

# ONNX Runtime 使用固定预编译包，只构建 C API 及其静态依赖。
cmake -S "$source_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$stage" \
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
    -DBUILD_SHARED_LIBS=ON -DSHERPA_ONNX_ENABLE_C_API=ON \
    -DSHERPA_ONNX_ENABLE_BINARY=OFF -DSHERPA_ONNX_BUILD_C_API_EXAMPLES=OFF \
    -DSHERPA_ONNX_ENABLE_PORTAUDIO=OFF -DSHERPA_ONNX_ENABLE_WEBSOCKET=OFF \
    -DSHERPA_ONNX_ENABLE_TTS=OFF -DSHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION=OFF \
    -DSHERPA_ONNX_USE_PRE_INSTALLED_ONNXRUNTIME_IF_AVAILABLE=OFF
cmake --build "$build_dir" --target sherpa-onnx-c-api --parallel 4
install -Dm644 "$source_dir/sherpa-onnx/c-api/c-api.h" \
    "$stage/include/sherpa-onnx/c-api/c-api.h"
install -Dm755 "$build_dir/lib/libsherpa-onnx-c-api.so" \
    "$stage/lib/libsherpa-onnx-c-api.so"
cp -a "$build_dir/_deps/onnxruntime-src/lib/"libonnxruntime.so* "$stage/lib/"
install -Dm644 "$source_dir/LICENSE" "$stage/share/licenses/sherpa-onnx/LICENSE"
install -Dm644 "$build_dir/_deps/onnxruntime-src/LICENSE" \
    "$stage/share/licenses/onnxruntime/LICENSE"
printf 'package\tversion\tsha256\tsource\n' > "$stage/SOURCES.tsv"
printf 'sherpa-onnx\t1.13.8\t%s\t%s\n' "$source_sha" \
    "https://codeload.github.com/k2-fsa/sherpa-onnx/tar.gz/$commit" >> "$stage/SOURCES.tsv"
printf 'onnxruntime\t1.28.2\t%s\t%s\n' "$ort_sha" \
    "https://github.com/csukuangfj/onnxruntime-libs/releases/download/v1.28.2/$ort_file" >> "$stage/SOURCES.tsv"
printf 'sherpa_commit=%s\nort_version=1.28.2\nlast_run_seconds=%s\n' \
    "$commit" "$((SECONDS - started))" > "$stage/build-info.txt"
readelf -d "$stage/lib/libsherpa-onnx-c-api.so" > "$stage/dynamic.txt"
rpath="$(awk '/\(RPATH\)|\(RUNPATH\)/ {gsub(/[\[\]]/, "", $NF); print $NF}' "$stage/dynamic.txt")"
[[ "$rpath" == '$ORIGIN' ]] || { echo "C API RPATH 不符: $rpath" >&2; exit 1; }
LD_LIBRARY_PATH="$stage/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    ldd "$stage/lib/libsherpa-onnx-c-api.so" > "$stage/ldd.txt"
if rg -q 'not found' "$stage/ldd.txt"; then cat "$stage/ldd.txt"; exit 1; fi
sha256sum "$patch_file" "$stage/lib/libsherpa-onnx-c-api.so" \
    "$stage/lib/libonnxruntime.so" "$stage/include/sherpa-onnx/c-api/c-api.h" \
    "$stage/share/licenses/sherpa-onnx/LICENSE" "$stage/share/licenses/onnxruntime/LICENSE" \
    "$stage/SOURCES.tsv" > "$stage/SHA256SUMS"
printf 'C API stage: %s；耗时 %s 秒\n' "$stage" "$((SECONDS - started))"
