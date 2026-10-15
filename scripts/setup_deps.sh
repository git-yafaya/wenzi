#!/usr/bin/env bash
set -euo pipefail

# 仅准备 Ubuntu 26.04 的 x86_64 主机依赖，不修改系统目录。
[[ "$(dpkg --print-architecture)" == amd64 ]] || { echo "需要 amd64 主机" >&2; exit 1; }
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
host="$root/.deps/host"
prefix="$host/usr"
export PATH="$prefix/bin${PATH:+:$PATH}"
export CMAKE_PREFIX_PATH="$prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig:$prefix/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$prefix/lib:$prefix/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# 已安装的固定依赖直接复用，下载包不是运行资产。
mkdir -p "$host/downloads"
cd "$host/downloads"
if [[ ! -x "$prefix/bin/cmake" || ! -r "$prefix/include/alsa/asoundlib.h" ||
      ! -e "$prefix/lib/x86_64-linux-gnu/libasound.so" ]]; then
printf 'package\tversion\tsha256\tsource\n' > "$host/SOURCES.tsv"
: > SHA256SUMS

# 下载固定包；已有文件只核验并解包。
deb() {
    local package="$1" version="$2" arch="$3" sha="$4" directory="$5" file
    file="${package}_${version}_${arch}.deb"
    [[ -f "$file" ]] || apt-get download "$package:$arch=$version"
    printf '%s  %s\n' "$sha" "$file" | sha256sum --check -
    printf '%s  %s\n' "$sha" "$file" >> SHA256SUMS
    printf '%s\t%s\t%s\t%s\n' "$package" "$version" "$sha" "https://archive.ubuntu.com/ubuntu/$directory/$file" >> "$host/SOURCES.tsv"
    dpkg-deb --extract "$file" "$host"
}
deb cmake 4.2.3-2ubuntu2 amd64 87c496f1e37b64bc0b4ad9c5fe0e75c09655f5399ed39695b876afba55becc61 pool/main/c/cmake
deb cmake-data 4.2.3-2ubuntu2 all 9f4cc6f77e7c09929f80fc5534b638b95d257a90d6bf43c59d2232514759eb9d pool/main/c/cmake
deb libjsoncpp26 1.9.6-5 amd64 2b42b79a81f9407d0ae9650541dca2e416b76b6ad9f7794737bac8c6d6f56823 pool/main/libj/libjsoncpp
deb librhash1 1.4.6-1.1 amd64 cb935821161fdfdbd30afdbcb8ceeab736f9a4df84f02df883a41d911951e398 pool/main/r/rhash
deb libasound2-dev 1.2.15.3-1ubuntu1.1 amd64 8e4bca41a403957cece9c42c494d3e994fd6a4a72e7eb34481acd08e8eb95c02 pool/main/a/alsa-lib
deb libasound2t64 1.2.15.3-1ubuntu1.1 amd64 96e20cf5c9095ef54ba46fccffea5c01cf2de17a1012c5683a67e43504a6d726 pool/main/a/alsa-lib

# deb 内的 ALSA 路径固定为 /usr，改为本地前缀。
sed -i "s|^prefix=/usr$|prefix=$prefix|" "$prefix/lib/x86_64-linux-gnu/pkgconfig/alsa.pc"
fi

# 原生异步 DNS 避免连接初始化阻塞核心线程。
if [[ ! -f "$host/.lws-4.3.5-poll-dns" || ! -e "$prefix/lib/libwebsockets.so.19" ]]; then
lws_url=https://codeload.github.com/warmcat/libwebsockets/tar.gz/refs/tags/v4.3.5
lws_sha=87f99ad32803ed325fceac5327aae1f5c1b417d54ee61ad36cffc8df5f5ab276
[[ -f libwebsockets-4.3.5.tar.gz ]] || curl --fail --location --retry 2 "$lws_url" -o libwebsockets-4.3.5.tar.gz
printf '%s  libwebsockets-4.3.5.tar.gz\n' "$lws_sha" | sha256sum --check -
if ! rg -q "$lws_sha" "$host/SOURCES.tsv"; then
    printf '%s  libwebsockets-4.3.5.tar.gz\n' "$lws_sha" >> SHA256SUMS
    printf 'libwebsockets\t4.3.5\t%s\t%s\n' "$lws_sha" "$lws_url" >> "$host/SOURCES.tsv"
fi
    tar -xzf libwebsockets-4.3.5.tar.gz -C "$host"
    # GCC 15 对旧版上游的 const 写法产生新警告，保留警告但不当作错误。
    cmake -S "$host/libwebsockets-4.3.5" -B "$host/build-lws" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_STANDARD=11 \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DDISABLE_WERROR=ON -DLWS_WITH_SHARED=ON -DLWS_WITH_STATIC=OFF \
        -DLWS_WITH_SSL=ON -DLWS_WITH_LEJP=ON -DLWS_WITH_SYS_ASYNC_DNS=ON \
        -DLWS_WITH_LIBUV=OFF \
        -DLWS_WITH_LIBEV=OFF -DLWS_WITH_LIBEVENT=OFF -DLWS_WITH_GLIB=OFF \
        -DLWS_WITH_EVLIB_PLUGINS=OFF -DLWS_WITHOUT_TESTAPPS=ON \
        -DLWS_WITH_MINIMAL_EXAMPLES=OFF -DLWS_WITH_HTTP_STREAM_COMPRESSION=OFF
    cmake --build "$host/build-lws" --parallel 4
    cmake --install "$host/build-lws"
    touch "$host/.lws-4.3.5-poll-dns"
    rm -f "$host/.lws-4.3.5-poll"
for directory in "$host/build-lws" "$host/libwebsockets-4.3.5"; do
    [[ ! -d "$directory" ]] || find "$directory" -depth -delete
done
fi

# 编译运行一次，检查本地头文件、动态链接、TLS 和 JSON 解析。
cat > "$host/check_deps.c" <<'C'
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <alsa/asoundlib.h>
#include <libwebsockets.h>
#if !defined(LWS_WITH_TLS) || !defined(LWS_WITH_LEJP) || !defined(LWS_WITH_SYS_ASYNC_DNS)
#error "需要 TLS、LEJP 和异步 DNS"
#endif
static signed char on_json(struct lejp_ctx *ctx, char reason) {
    (void)ctx;
    (void)reason;
    return 0;
}
int main(void) {
    /* 同时检查头文件和实际加载的库。 */
    struct lejp_ctx parser;
    const unsigned char json[] = "{\"ok\":true}";
    struct lws_context_creation_info info = {0};
    assert(strcmp(snd_asoundlib_version(), "1.2.15.3") == 0);
    assert(strncmp(lws_get_library_version(), "4.3.5", 5) == 0);
    lejp_construct(&parser, on_json, NULL, NULL, 0);
    assert(lejp_parse(&parser, json, sizeof(json) - 1) == 0);
    lejp_destruct(&parser);
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    struct lws_context *context = lws_create_context(&info);
    assert(context != NULL);
    lws_context_destroy(context);
    puts("ALSA 1.2.15.3 / libwebsockets 4.3.5 TLS LEJP poll: OK");
    return 0;
}
C
cc -std=c11 -D_POSIX_C_SOURCE=200809L $(pkg-config --cflags alsa libwebsockets) "$host/check_deps.c" \
    -o "$host/check_deps" $(pkg-config --libs alsa libwebsockets)
"$host/check_deps"
rm -f "$host/check_deps.c" "$host/check_deps"
ldd "$prefix/bin/cmake" "$prefix/lib/libwebsockets.so.19" \
    "$prefix/lib/x86_64-linux-gnu/libasound.so.2" > "$host/ldd.txt"
if rg -q 'not found' "$host/ldd.txt"; then cat "$host/ldd.txt"; exit 1; fi
cmake --version
pkg-config --cflags --libs alsa libwebsockets
rm -f "$host/downloads/"*.deb "$host/downloads/libwebsockets-4.3.5.tar.gz"
