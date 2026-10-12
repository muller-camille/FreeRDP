#!/usr/bin/env bash
# Windows client of FreeRDP (wfreerdp.exe) built from the patches branch of this fork, at the commit pinned below
# (verified after the fetch), statically linked with OpenSSL (pinned version, archive verified by its SHA-256 sum): a
# single executable that only uses Windows DLLs. Cross-compiled from Linux (mingw-w64, cmake, ninja, perl, make).
#
# Output in out/: wfreerdp.exe, the licenses of FreeRDP, OpenSSL and the mingw-w64 runtime, BUILD.txt (versions and
# fingerprint). The release workflow publishes them as release "wfreerdp-<FreeRDP version>-<release>".
set -euo pipefail

freerdp_repository=https://github.com/muller-camille/FreeRDP
freerdp_version=3.32.1
freerdp_commit=5d580f5e87981131e81833758256ca1dc1da6752
# Release number for this FreeRDP version: bump it for any change to this script or to the pinned commit.
release=6
openssl_version=3.5.9
openssl_sha256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
openssl_url=https://github.com/openssl/openssl/releases/download/openssl-$openssl_version/openssl-$openssl_version.tar.gz

root=$(cd "$(dirname "$0")" && pwd)
tools="$root"
out="$root/out"

if [ "${1:-}" = "--tag" ]; then
  echo "wfreerdp-$freerdp_version-$release"
  exit 0
fi
if [ "${1:-}" = "--commit" ]; then
  echo "$freerdp_commit"
  exit 0
fi

for tool in x86_64-w64-mingw32-gcc-posix x86_64-w64-mingw32-objdump cmake ninja perl make git curl sha256sum; do
  command -v "$tool" >/dev/null || { echo "Missing tool: $tool (packages mingw-w64, cmake, ninja-build)" >&2; exit 1; }
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/dl"
jobs=$(nproc 2>/dev/null || echo 2)

# ---- OpenSSL, static: no module, engine or DSO (nothing loaded at run time), no configuration file read
# automatically. The folders compiled into the library are the Windows OpenSSL ones, reserved for administrators
# (not Linux paths that any user could create on C:).
archive="$work/dl/openssl-$openssl_version.tar.gz"
if [ ! -f "$archive" ] || ! echo "$openssl_sha256  $archive" | sha256sum -c --status; then
  curl -sSfL -o "$archive" "$openssl_url"
fi
echo "$openssl_sha256  $archive" | sha256sum -c
ssl_prefix="$work/openssl-install"
if [ ! -f "$ssl_prefix/lib/libcrypto.a" ]; then
  rm -rf "$work/openssl"
  mkdir -p "$work/openssl"
  tar xf "$archive" --strip-components=1 -C "$work/openssl"
  (
    cd "$work/openssl"
    CC=gcc-posix ./Configure mingw64 no-shared no-module no-dso no-engine no-autoload-config no-tests no-docs \
      no-apps no-ui-console --cross-compile-prefix=x86_64-w64-mingw32- --prefix="$ssl_prefix" --libdir=lib
    make -j"$jobs" build_libs \
      "OPENSSLDIR=C:/Program Files/Common Files/SSL" \
      "ENGINESDIR=C:/Program Files/OpenSSL/lib/engines-3" \
      "MODULESDIR=C:/Program Files/OpenSSL/lib/ossl-modules"
    make install_dev
  )
fi
if strings "$ssl_prefix/lib/libcrypto.a" | grep -q "$work"; then
  echo "OpenSSL: a build path was left in the library." >&2
  exit 1
fi

# ---- FreeRDP and its patches, at the pinned (verified) commit.
src="$work/freerdp"
rm -rf "$src"
git init --quiet "$src"
git -C "$src" fetch --quiet --depth 1 "$freerdp_repository" "$freerdp_commit"
git -C "$src" checkout --quiet --detach FETCH_HEAD
test "$(git -C "$src" rev-parse HEAD)" = "$freerdp_commit"

build="$work/freerdp-build"
rm -rf "$build"
# WITH_DEBUG_CLIPRDR: clipboard messages of the Windows client (window messages and errors, never the clipboard
# contents), at DEBUG level: only shown with /log-filters:com.freerdp.client.windows:DEBUG.
cmake -S "$src" -B "$build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$tools/mingw64.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXE_LINKER_FLAGS="-static -static-libgcc" \
  -DOPENSSL_ROOT_DIR="$ssl_prefix" -DOPENSSL_USE_STATIC_LIBS=ON \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DBUILD_TESTING_INTERNAL=OFF \
  -DWITH_CLIENT_WINDOWS=ON -DWITH_CLIENT_SDL=OFF -DWITH_CLIENT_SDL2=OFF -DWITH_CLIENT_SDL3=OFF \
  -DWITH_SERVER=OFF -DWITH_SHADOW=OFF -DWITH_PROXY=OFF -DWITH_SAMPLE=OFF -DWITH_PLATFORM_SERVER=OFF \
  -DWITH_WINPR_TOOLS=OFF -DWITH_MANPAGES=OFF -DWITH_CCACHE=OFF -DWITH_CLANG_FORMAT=OFF -DUSE_UNWIND=OFF \
  -DWITH_FFMPEG=OFF -DWITH_SWSCALE=OFF -DWITH_DSP_FFMPEG=OFF -DWITH_VIDEO_FFMPEG=OFF -DWITH_CAIRO=OFF \
  -DWITH_JPEG=OFF -DWITH_OPUS=OFF -DWITH_FDK_AAC=OFF -DWITH_LAME=OFF -DWITH_FAAD2=OFF -DWITH_FAAC=OFF \
  -DWITH_SOXR=OFF -DWITH_OPENH264=OFF -DWITH_WEBVIEW=OFF \
  -DWITH_JSON_DISABLED=ON -DWITH_AAD=OFF -DWITH_SMARTCARD_EMULATE=OFF -DWITH_PKCS11=OFF \
  -DCHANNEL_URBDRC=OFF -DWITH_FREERDP_DEPRECATED_COMMANDLINE=OFF \
  -DWITH_INTERNAL_RC4=ON -DWITH_INTERNAL_MD4=ON -DWITH_INTERNAL_MD5=ON -DWITH_VERBOSE_WINPR_ASSERT=OFF \
  -DWITH_DEBUG_CLIPRDR=ON
cmake --build "$build" --target wfreerdp -j "$jobs"

exe=$(find "$build" -name wfreerdp.exe -type f | head -n 1)
test -n "$exe"
x86_64-w64-mingw32-strip "$exe"

# Nothing but Windows DLLs: no library must be looked for next to the executable.
x86_64-w64-mingw32-objdump -p "$exe" | sed -n 's/^\s*DLL Name: //p' | sort -u > "$work/imports.txt"
cat "$work/imports.txt"
if grep -viE '^(advapi32|bcrypt|comctl32|comdlg32|crypt32|credui|dbghelp|gdi32|imm32|iphlpapi|kernel32|msimg32|msvcrt|ncrypt|ntdll|ole32|oleaut32|rpcrt4|secur32|setupapi|shell32|shlwapi|user32|userenv|uuid|version|winmm|winscard|winspool\.drv|ws2_32|wtsapi32|dwmapi|uxtheme|mfplat|mfuuid|mf|api-ms-win-[a-z0-9-]+)\.dll$|^winspool\.drv$' "$work/imports.txt"; then
  echo "wfreerdp.exe depends on a DLL that is not part of Windows (above)." >&2
  exit 1
fi

rm -rf "$out"
mkdir -p "$out"
cp "$exe" "$out/wfreerdp.exe"
cp "$src/LICENSE" "$out/LICENSE.FreeRDP.txt"
cp "$work/openssl/LICENSE.txt" "$out/LICENSE.OpenSSL.txt" 2>/dev/null || tar xOf "$archive" "openssl-$openssl_version/LICENSE.txt" > "$out/LICENSE.OpenSSL.txt"
# mingw-w64 runtime libraries linked statically (winpthreads...): their licenses.
cp /usr/share/doc/mingw-w64-x86-64-dev/copyright "$out/LICENSE.mingw-w64.txt"
{
  echo "FreeRDP $freerdp_version + patches ($freerdp_repository, branch patches-$freerdp_version, commit $freerdp_commit)"
  echo "OpenSSL $openssl_version ($openssl_url, SHA-256 $openssl_sha256)"
  echo "wfreerdp.exe SHA-256 $(sha256sum "$out/wfreerdp.exe" | cut -d' ' -f1)"
} > "$out/BUILD.txt"
cat "$out/BUILD.txt"
