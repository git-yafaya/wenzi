# 构建与部署

## PC 本地依赖

在项目根目录运行 README 的构建命令。`source scripts/env.sh` 设置 PC 的 `PATH`、`CMAKE_PREFIX_PATH`、`PKG_CONFIG_PATH`、`LD_LIBRARY_PATH`，不修改系统目录。局部依赖目录如下：

| 内容 | 路径 |
|---|---|
| CMake、ALSA、libwebsockets | `.deps/host/usr` |
| 原始官方 SDK | `.deps/sherpa-onnx` |
| 重建的 sherpa C API 头 | `.deps/sherpa-patch-stage/include/sherpa-onnx/c-api/c-api.h` |
| 重建的 C API、ONNX Runtime | `.deps/sherpa-patch-stage/lib` |
| 双语模型 | `models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16` |
| 标点模型 | `models/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx` |
| 官方测试音频 | `samples/zh.wav`、`samples/en.wav`、`samples/mixed.wav` |

使用其他已安装依赖时，设置 `CMAKE_PREFIX_PATH` 和 `PKG_CONFIG_PATH`，并通过 `-DSHERPA_ONNX_ROOT=/实际安装前缀` 指定 C API。应用使用 C 编译器；预编译 sherpa C API 的 C++ 运行库依赖仍需由运行系统提供。

主机依赖版本、下载地址和 SHA256 在 `scripts/setup_deps.sh` 与 `.deps/host/SOURCES.tsv`；模型和识别库的固定来源在 `scripts/setup_assets.sh` 及 `samples/SOURCES.md`。下载归档在安装成功后清理，头文件、运行库和模型保留。

第三方补丁由 `scripts/build_sherpa_patch.sh` 复现，固定 sherpa 源码 commit `11afbd009a7f8c08f4bcf2fc1b265d0df4670fbf`，仅重建 C API 和必要静态依赖。构建的库、来源与摘要在 `.deps/sherpa-patch-stage`。本机 libwebsockets 使用 TLS、LEJP、poll 和原生异步 DNS；[上游说明](https://libwebsockets.org/lws-api-doc-master/html/md_READMEs_README_8async-dns.html)说明该选项用于事件循环上的非阻塞解析。

## 安装 PC 运行目录

```bash
source scripts/env.sh
cmake --install build --prefix "$PWD/install"
mkdir -p install/lib install/share/licenses
cp -a .deps/sherpa-patch-stage/lib/*.so* install/lib/
cp -a .deps/host/usr/lib/libwebsockets.so* install/lib/
cp -a .deps/host/usr/lib/x86_64-linux-gnu/libasound.so* install/lib/
cp -a .deps/sherpa-patch-stage/share/licenses/. install/share/licenses/
env -u LD_LIBRARY_PATH install/bin/wenzi --help
```

应用安装 RPATH 为 `$ORIGIN/../lib`，sherpa 库从自身目录找到 ONNX Runtime。OpenSSL、libstdc++、libgcc、libc 及其依赖由相同 ABI 的目标系统提供。模型通过 `--model-dir`、`--punctuation` 或 README 的环境变量指定，可放在应用目录之外。

这份 x86_64 目录用于当前 PC。换架构或 libc 后，需要重新取得匹配的第三方库并构建应用。

## 交叉编译入口

工具链文件 `cmake/linux-cross.cmake` 接受实际 SDK 参数，限制头文件、库和包搜索到 sysroot。以下变量必须由 SDK 信息填写：

```bash
export WENZI_SDK_CC=/实际SDK/C编译器路径
export WENZI_SDK_SYSROOT=/实际SDK/sysroot
export WENZI_SDK_PROCESSOR=实际目标架构
export WENZI_SDK_PKGCONFIG=目标库的pkgconfig目录列表
export WENZI_SDK_SHERPA=/目标平台sherpa安装前缀

PKG_CONFIG_LIBDIR="$WENZI_SDK_PKGCONFIG" cmake -S . -B build-cross \
  -DCMAKE_TOOLCHAIN_FILE=cmake/linux-cross.cmake \
  -DCMAKE_C_COMPILER="$WENZI_SDK_CC" \
  -DCMAKE_SYSTEM_PROCESSOR="$WENZI_SDK_PROCESSOR" \
  -DCMAKE_SYSROOT="$WENZI_SDK_SYSROOT" \
  -DCMAKE_PREFIX_PATH=/usr \
  -DSHERPA_ONNX_ROOT="$WENZI_SDK_SHERPA" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-cross -j 4
```

`WENZI_SDK_PKGCONFIG` 为冒号分隔的目标目录，须包含 ALSA、libwebsockets；OpenSSL 和 sherpa 也必须匹配目标 ABI。CMake 本身是主机工具，可使用已准备的 PC CMake。交叉构建生成目标程序后，用 SDK 的 `readelf -h/-d` 核对架构、动态加载器及 NEEDED 项，再将相应目标运行库和模型复制到板端。

目标 sherpa 源码也需应用同一 C API 补丁；目标 libwebsockets 启用 `LWS_WITH_SYS_ASYNC_DNS`。本机的第三方重建脚本用于本机库，不能把其产物直接复制到其他 ABI 的板卡。

## 板端待提供条件

- 板卡/SoC、CPU 架构和位数、大小端、浮点 ABI、指令集、核数、RAM 与存储。
- Linux、libc 和动态加载器版本；SDK 来源、工具链版本、target triplet、C/C++ 运行库及完整 sysroot。
- ALSA 驱动、麦克风接口、设备名、权限、可用采样率/通道/格式、period/buffer 范围。
- 第三方 C API 与模型在该架构上的构建支持、运行资源和许可证。

板端先运行同一批 WAV，再运行真实麦克风、WS、云端及持续运行检查。交叉链接成功只证明能够生成目标程序；部署通过须有实际板卡日志。
