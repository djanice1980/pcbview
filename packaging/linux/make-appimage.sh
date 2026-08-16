#!/bin/bash
# Build the pcbview AppImage from an existing Linux build.
#
#   packaging/linux/make-appimage.sh <build-dir> [out-dir]
#
# Run on Arch (or in an archlinux container -- CI does exactly that), with the
# same system packages the build itself used. The same script serves local
# builds and CI so the two cannot drift.
#
# Notes earned the hard way, kept here so they are not re-learned:
# - OIDN loads its device libraries with dlopen, which linuxdeploy cannot see;
#   the core + CPU device libs are copied in by hand. The CUDA/HIP device libs
#   are NOT bundled: they drag in the vendor compute runtimes (libamdhip64,
#   libcuda) which are absent on most machines and enormous where present.
#   OIDN falls back to its CPU device cleanly.
# - Qt 6.11 merged the wayland platform plugin into qt6-base as one
#   libqwayland.so (there is no separate -generic/-egl pair any more).
# - The KDE kimg_* imageformat extras in the system Qt plugin dir have
#   optional deps (libheif etc.) that may be broken links; pcbview needs none
#   of them, so the plugin dir is mirrored without them and served to the qt
#   plugin through a qmake wrapper.
# - NO_STRIP=1: linuxdeploy's bundled strip predates .relr.dyn sections and
#   fails on every modern Arch binary.
set -euo pipefail

BUILD_DIR=${1:?usage: make-appimage.sh <build-dir> [out-dir]}
mkdir -p "${2:-$PWD}"
OUT_DIR=$(cd "${2:-$PWD}" && pwd)  # absolute: the build cds into its workdir
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

VERSION=$(sed -n 's/^project(pcbview VERSION \([0-9.]*\).*/\1/p' "$REPO/CMakeLists.txt")

# linuxdeploy + qt plugin, cached beside the script if already downloaded.
TOOLS="$HERE/.tools"
mkdir -p "$TOOLS"
for t in linuxdeploy-x86_64.AppImage linuxdeploy-plugin-qt-x86_64.AppImage; do
    [ -f "$TOOLS/$t" ] || curl -sL -o "$TOOLS/$t" \
        "https://github.com/linuxdeploy/${t%%-x86_64*}/releases/download/continuous/$t"
    chmod +x "$TOOLS/$t"
done

APPDIR="$WORK/AppDir"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps"
cp "$BUILD_DIR/pcbview"              "$APPDIR/usr/bin/"
cp "$HERE/pcbview.desktop"           "$APPDIR/usr/share/applications/"
cp "$REPO/assets/pcbview_256.png"    "$APPDIR/usr/share/icons/hicolor/256x256/apps/pcbview.png"

# OIDN: runtime-dlopen'd pieces, CPU device only (see header).
cp /usr/lib/libOpenImageDenoise.so*            "$APPDIR/usr/lib/"
cp /usr/lib/libOpenImageDenoise_core.so*       "$APPDIR/usr/lib/"
cp /usr/lib/libOpenImageDenoise_device_cpu.so* "$APPDIR/usr/lib/"

# Qt plugin dir minus the KDE kimg_* extras, via a qmake wrapper.
cp -r /usr/lib/qt6/plugins "$WORK/qt-plugins"
rm -f "$WORK/qt-plugins/imageformats"/kimg_*.so
cat > "$WORK/qmake-wrapper" <<WRAP
#!/bin/bash
/usr/bin/qmake6 "\$@" | sed "s|^QT_INSTALL_PLUGINS:.*|QT_INSTALL_PLUGINS:$WORK/qt-plugins|"
WRAP
chmod +x "$WORK/qmake-wrapper"

cd "$WORK"
NO_STRIP=1 VERSION="$VERSION" QMAKE="$WORK/qmake-wrapper" \
    EXTRA_PLATFORM_PLUGINS="libqwayland.so" \
    APPIMAGE_EXTRACT_AND_RUN=1 \
    "$TOOLS/linuxdeploy-x86_64.AppImage" \
        --appdir "$APPDIR" --plugin qt --output appimage

mv pcbview-*.AppImage "$OUT_DIR/"
echo "built: $OUT_DIR/pcbview-$VERSION-x86_64.AppImage"
