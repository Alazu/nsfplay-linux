#!/usr/bin/env bash
# Build NSFPlay-x86_64.AppImage.
#
#   ./build-appimage.sh            build inside an Ubuntu 22.04 container (podman/docker) for wide glibc compatibility
#   ./build-appimage.sh --native   build with the host toolchain (AppImage only runs on distros with glibc >= host)
#
# Tools (linuxdeploy, linuxdeploy-plugin-qt, appimagetool) are downloaded to $TOOLS (default ~/.cache/nsfplay-tools).
set -euo pipefail
SRC="$(cd "$(dirname "$0")" && pwd)"
TOOLS="${TOOLS:-$HOME/.cache/nsfplay-tools}"
IMAGE="${IMAGE:-docker.io/library/ubuntu:22.04}"

fetch_tools() {
    mkdir -p "$TOOLS"
    local base=https://github.com
    [ -x "$TOOLS/linuxdeploy" ] || curl -fsSL -o "$TOOLS/linuxdeploy" "$base/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
    [ -x "$TOOLS/linuxdeploy-plugin-qt" ] || curl -fsSL -o "$TOOLS/linuxdeploy-plugin-qt" "$base/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"
    [ -x "$TOOLS/appimagetool" ] || curl -fsSL -o "$TOOLS/appimagetool" "$base/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
    chmod +x "$TOOLS"/*
}

# Runs with the sources at $1, output dir $2 (inside container or natively)
inner_build() {
    local src="$1" out="$2" bld="$2/build" appdir="$2/AppDir"
    export APPIMAGE_EXTRACT_AND_RUN=1 NO_STRIP=1
    rm -rf "$bld" "$appdir"
    cmake -S "$src" -B "$bld" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build "$bld" -j"$(nproc)"
    DESTDIR="$appdir" cmake --install "$bld"

    export QMAKE="${QMAKE:-$(command -v qmake6 || echo /usr/lib/qt6/bin/qmake)}"
    export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so;libqoffscreen.so"
    [ -e "$("$QMAKE" -query QT_INSTALL_PLUGINS)/platforms/libqwayland.so" ] && EXTRA_PLATFORM_PLUGINS="libqwayland.so;libqoffscreen.so"
    export EXTRA_QT_MODULES="waylandcompositor"
    "$TOOLS/linuxdeploy" --appdir "$appdir" -e "$appdir/usr/bin/nsfplay" \
        -d "$appdir/usr/share/applications/nsfplay.desktop" \
        -i "$appdir/usr/share/icons/hicolor/256x256/apps/nsfplay.png" --plugin qt

    # prune heavy plugins the player never needs (KDE image formats, virtual keyboard, ...)
    find "$appdir/usr/plugins/imageformats" -type f ! -name 'libqico.so' ! -name 'libqsvg.so' ! -name 'libqgif.so' ! -name 'libqjpeg.so' -delete 2>/dev/null || true
    rm -f "$appdir/usr/plugins/platforminputcontexts/libqtvirtualkeyboardplugin.so"
    rm -rf "$appdir/usr/plugins/iconengines/"*kiconthemes* 2>/dev/null || true
    # drop bundled libs no remaining ELF file depends on
    local changed=1
    while [ $changed = 1 ]; do
        changed=0
        local needed
        needed=$(find "$appdir/usr" -type f \( -name '*.so*' -o -path '*/bin/*' \) -exec sh -c 'objdump -p "$1" 2>/dev/null | awk "/NEEDED/{print \$2}"' _ {} \; | sort -u)
        for lib in "$appdir"/usr/lib/*.so*; do
            local n; n=$(basename "$lib")
            case "$n" in libQt6*Wayland*|libQt6XcbQpa*|libQt6DBus*) continue;; esac
            if ! grep -qx "$n" <<<"$needed"; then rm -f "$lib"; changed=1; fi
        done
    done
    # custom launcher; also exposes CLI tools (./NSFPlay.AppImage nsf2wav ...)
    # AppRun is a symlink to usr/bin/nsfplay after linuxdeploy: remove it, don't write through it
    rm -f "$appdir/AppRun"
    cat > "$appdir/AppRun" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/usr/plugins"
case "$1" in
  nsf2wav|nsfmeta) tool="$1"; shift; exec "$HERE/usr/bin/$tool" "$@";;
esac
exec "$HERE/usr/bin/nsfplay" "$@"
EOF
    chmod +x "$appdir/AppRun"
    rm -f "$appdir/AppRun.wrapped" "$appdir/apprun-hooks/"* 2>/dev/null || true
    ARCH=x86_64 "$TOOLS/appimagetool" --no-appstream "$appdir" "$out/NSFPlay-x86_64.AppImage"
}

fetch_tools
mkdir -p "$SRC/dist"
if [ "${1:-}" = "--native" ]; then
    TOOLS="$TOOLS" inner_build "$SRC" "$SRC/dist"
else
    RT=$(command -v podman || command -v docker)
    "$RT" run --rm -v "$SRC:/src:Z" -v "$TOOLS:/tools:Z" -e TOOLS=/tools "$IMAGE" bash -euc '
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends build-essential cmake file binutils \
            qt6-base-dev qt6-wayland libqt6svg6 qt6-qpa-plugins libasound2-dev libgl1-mesa-dev \
            libxkbcommon-dev libfuse2 ca-certificates >/dev/null
        export QMAKE=/usr/lib/qt6/bin/qmake
        source <(sed -n "/^inner_build()/,/^}/p" /src/build-appimage.sh)
        cp -r /src /tmp/src && rm -rf /tmp/src/build /tmp/src/dist /tmp/src/AppDir
        inner_build /tmp/src /src/dist
    '
fi
ls -la "$SRC/dist/NSFPlay-x86_64.AppImage"
