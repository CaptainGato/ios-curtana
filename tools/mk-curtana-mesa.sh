#!/bin/bash
# Build Mesa Freedreno (Adreno 618 ir3 + softpipe) + AIR-to-NIR Metal translator + EAGL bridge
# for Xiaomi Redmi Note 9S / Pro (curtana, SM7125, chip_id 0x06010800) running XNU / iOS 16.
#
# Outputs in build/mesa/out/:
#   - libOSMesa.8.dylib      (/redwood/lib/libOSMesa.8.dylib — Freedreno + ir3 + AIR2NIR)
#   - libhakios.gl.dylib     (/redwood/lib/libhakios.gl.dylib — EAGLContext / OpenGLES bridge)
#   - gl_test                (/redwood/bin/gl_test — offscreen/onscreen Adreno 618 GLES test)
#   - air_run                (/redwood/bin/air_run — Metal .metallib AIR -> NIR -> Adreno 618 ir3 runner)
#   - metal_test             (/redwood/bin/metal_test — CoreImage / Metal / EAGL diagnostic)
#
# Usage:
#   tools/mk-curtana-mesa.sh [--install <MOUNTED_IOS_ROOT>]

set -euo pipefail

VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
B="${BUILD_DIR:-$REPO_ROOT/build/mesa}"
SDK="${IOS_SDK:-${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk}"
CC_BIN="${XNU_CLANG:-clang}"

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
	echo "Usage: $0 [--install <MOUNTED_IOS_ROOT>]"
	exit 0
fi

INSTALL_ROOT=""
if [ "${1:-}" = "--install" ] && [ -n "${2:-}" ]; then
	INSTALL_ROOT="$2"
fi

mkdir -p "$B/out"
cd "$B"

PATCHES=$(cat "$REPO_ROOT"/tools/mesa/*.patch | sha256sum | cut -d' ' -f1)
if [ ! -d "mesa-$VERSION" ] || [ "$(cat "mesa-$VERSION/.curtana-patches" 2>/dev/null || true)" != "$PATCHES" ]; then
	[ -f "mesa-$VERSION.tar.xz" ] || curl -sL --fail -o "mesa-$VERSION.tar.xz" "https://archive.mesa3d.org/mesa-$VERSION.tar.xz"
	echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf "mesa-$VERSION" build-*
	tar xJf "mesa-$VERSION.tar.xz"
	for p in "$REPO_ROOT"/tools/mesa/*.patch; do
		patch -s --no-backup-if-mismatch -d "mesa-$VERSION" -p1 < "$p"
	done
	echo "$PATCHES" > "mesa-$VERSION/.curtana-patches"
fi

rm -rf "mesa-$VERSION/src/hakios-drm" "mesa-$VERSION/src/compiler/air"
cp -r "$REPO_ROOT/tools/mesa/hakios-drm" "mesa-$VERSION/src/hakios-drm"
cp -r "$REPO_ROOT/tools/mesa/air" "mesa-$VERSION/src/compiler/air"

if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install mako pyyaml packaging
fi
export PATH="$B/venv/bin:$PATH"

cat > cross.ini <<INI
[binaries]
c = ['$CC_BIN', '-arch', 'arm64', '-isysroot', '$SDK']
cpp = ['$CC_BIN++', '-arch', 'arm64', '-isysroot', '$SDK', '-fuse-cxa-atexit']
objc = ['$CC_BIN', '-arch', 'arm64', '-isysroot', '$SDK']
ar = 'llvm-ar'
ranlib = 'llvm-ranlib'
strip = 'llvm-strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI

DRIVERS="${DRIVERS:-softpipe,freedreno}"
BUILD="build-${DRIVERS//,/-}"
if [ ! -f "$BUILD/build.ninja" ]; then
	meson setup "$BUILD" "mesa-$VERSION" --cross-file cross.ini --wrap-mode=nofallback 		--prefix=/redwood --libdir=lib --buildtype=release -Db_ndebug=true 		-Dplatforms= -Dgallium-drivers="$DRIVERS" -Dfreedreno-kmds=msm -Dvulkan-drivers= 		-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dosmesa=true -Dllvm=disabled 		-Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true 		-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled 		-Dvalgrind=disabled -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= 		-Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled -Dgallium-xa=disabled 		-Dlmsensors=disabled -Dbuild-tests=false
fi

ninja -C "$BUILD"
ninja -C "$BUILD" src/gallium/drivers/freedreno/air_run
cp "$BUILD/src/gallium/drivers/freedreno/air_run" out/air_run

llvm-strip -x -o out/libOSMesa.8.dylib "$BUILD/src/gallium/targets/osmesa/libOSMesa.8.dylib"
install_name_tool -id /redwood/lib/libOSMesa.8.dylib out/libOSMesa.8.dylib
nm -gU out/libOSMesa.8.dylib | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > out/exported-gl.txt
cp "mesa-$VERSION/src/mapi/glapi/registry/gl.xml" out/gl.xml

# Build EAGL bridge (libhakios.gl.dylib) and test binaries
EAGL_DIR="$REPO_ROOT/tools/mesa/eagl"
python3 "$EAGL_DIR/mkeagl.py" "$EAGL_DIR/eagl_slots.txt" out/gl.xml out/exported-gl.txt "$EAGL_DIR/eagl.c" > out/eagl_gen.h

DYN="$CC_BIN -arch arm64 -isysroot $SDK -O2"
$DYN -dynamiclib -install_name /redwood/lib/libhakios.gl.dylib -Iout -I"$EAGL_DIR" 	-framework CoreFoundation -framework IOKit -framework IOSurface -lobjc 	out/libOSMesa.8.dylib -o out/libhakios.gl.dylib "$EAGL_DIR/eagl.c" "$EAGL_DIR/gl_osmesa.c"
$DYN -I"$EAGL_DIR" -framework CoreFoundation -framework IOKit out/libOSMesa.8.dylib -o out/gl_test "$EAGL_DIR/gl_test.c"
$DYN -fobjc-arc -Wno-deprecated-declarations -framework Foundation -framework CoreGraphics 	-framework CoreImage -framework IOKit -framework Metal -framework OpenGLES 	-lMobileGestalt -o out/metal_test "$EAGL_DIR/metal_test.m"

if [ -n "$INSTALL_ROOT" ] && [ -d "$INSTALL_ROOT" ]; then
	echo "[*] Installing Mesa Freedreno + EAGL + AIR2NIR into $INSTALL_ROOT/redwood/ ..."
	mkdir -p "$INSTALL_ROOT/redwood/lib" "$INSTALL_ROOT/redwood/bin"
	cp -v out/libOSMesa.8.dylib out/libhakios.gl.dylib "$INSTALL_ROOT/redwood/lib/"
	cp -v out/gl_test out/air_run out/metal_test "$INSTALL_ROOT/redwood/bin/"
fi

echo "[✓] Built Adreno 618 Mesa Freedreno + EAGL + AIR2NIR in $B/out"
