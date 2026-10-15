#!/usr/bin/env bash
# 仅用于 PC 的项目局部依赖；交叉编译使用目标 SDK。
_wenzi_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
_wenzi_host="$_wenzi_root/.deps/host/usr"
export PATH="$_wenzi_host/bin${PATH:+:$PATH}"
export CMAKE_PREFIX_PATH="$_wenzi_host${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export PKG_CONFIG_PATH="$_wenzi_host/lib/pkgconfig:$_wenzi_host/lib/x86_64-linux-gnu/pkgconfig:$_wenzi_host/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$_wenzi_root/.deps/sherpa-patch-stage/lib:$_wenzi_root/.deps/sherpa-onnx/lib:$_wenzi_host/lib:$_wenzi_host/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
unset _wenzi_root _wenzi_host
