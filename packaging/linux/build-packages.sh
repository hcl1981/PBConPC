#!/bin/sh
# Linux packages of PBConPC: an AppImage and a .deb, for x86-64.
#
#   packaging/linux/build-packages.sh VERSION [OUT_DIR]      e.g. 1.0.1 dist-linux
#
# The program is built in an Ubuntu 22.04 container (docker), so that it needs no newer C library
# than glibc 2.35 (Ubuntu 22.04, Debian 12, Mint 21, Fedora 36 and later), without -march=native,
# and with SDL3 built in (ci/build-linux.sh: SDL loads X11/Wayland and the sound libraries itself
# at run time), so that nothing beyond the C library is needed. The AppImage is made with
# appimagetool, which APPIMAGETOOL names (without it only the .deb is made).
set -eu
ver=${1:?usage: $0 VERSION [OUT_DIR]}
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../.." && pwd)
out=$(mkdir -p "${2:-$src/dist-linux}" && cd "${2:-$src/dist-linux}" && pwd)
build=$out/build

docker run --rm --cpus 8 -v "$src:/src:ro" -v "$out:/out" -e VER="v$ver" -e HOST_ID="$(id -u):$(id -g)" \
    ubuntu:22.04 sh /src/packaging/linux/container-build.sh

bin=$build/pbconpc
strings "$bin" | grep -qx "v$ver" && echo "version v$ver inside" || echo "warning: v$ver not found in the binary"
glibc=$(objdump -T "$bin" "$build/uf2flash" | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -Vu | tail -1)
echo "needs glibc $glibc"

# --- .deb ---
deb=$out/deb
rm -rf "$deb"
install -Dm755 "$bin" "$deb/usr/bin/pbconpc"
install -Dm755 "$build/uf2flash" "$deb/usr/bin/pbc-uf2flash"
install -Dm755 "$src/tools/pbc-addgames" "$deb/usr/bin/pbc-addgames"
install -Dm755 "$src/tools/pbc-usb-attach" "$deb/usr/lib/pbconpc/pbc-usb-attach"
install -Dm644 "$here/pbconpc.desktop" "$deb/usr/share/applications/pbconpc.desktop"
install -Dm644 "$here/pbconpc.png" "$deb/usr/share/icons/hicolor/256x256/apps/pbconpc.png"
install -Dm644 "$src/LICENSE" "$deb/usr/share/doc/pbconpc/copyright"
install -Dm644 "$src/README.md" "$deb/usr/share/doc/pbconpc/README.md"
size=$(du -sk "$deb/usr" | cut -f1)
mkdir -p "$deb/DEBIAN"
cat > "$deb/DEBIAN/control" <<EOF
Package: pbconpc
Version: $ver
Architecture: amd64
Maintainer: hcl1981 <https://github.com/hcl1981/PBConPC>
Installed-Size: $size
Depends: libc6 (>= $glibc)
Suggests: pkexec | policykit-1, linux-tools-generic | usbip, mtools
Section: otherosfs
Priority: optional
Homepage: https://github.com/hcl1981/PBConPC
Description: PicoBoy Color Plus emulator for the PC
 Runs any PicoBoy Color Plus firmware (.uf2) on an emulated RP2350 with
 16 MB flash, display, buttons, sound, tilt sensor, USB serial terminal,
 link cable between two windows, GDB server and profiler.
 .
 pkexec and usbip let "Connect USB to this PC" make the emulated USB
 device a real one (for PicoBoy Studio or Thonny); mtools serve
 pbc-addgames.
EOF
debfile=$out/pbconpc_${ver}_amd64.deb
dpkg-deb --root-owner-group -Zxz --build "$deb" "$debfile" >/dev/null
echo "built $debfile"

# --- AppImage ---
app=$out/PBConPC.AppDir
rm -rf "$app"
install -Dm755 "$bin" "$app/usr/bin/pbconpc"
install -Dm755 "$build/uf2flash" "$app/usr/bin/uf2flash"
install -Dm755 "$src/tools/pbc-usb-attach" "$app/usr/bin/pbc-usb-attach"
install -Dm644 "$here/pbconpc.desktop" "$app/pbconpc.desktop"
install -Dm644 "$here/pbconpc.png" "$app/pbconpc.png"
install -Dm644 "$here/pbconpc.png" "$app/usr/share/icons/hicolor/256x256/apps/pbconpc.png"
ln -s usr/bin/pbconpc "$app/AppRun"
tool=${APPIMAGETOOL:-}
if [ -z "$tool" ]; then
    echo "AppImage skipped: set APPIMAGETOOL to appimagetool (github.com/AppImage/appimagetool)"
    exit 0
fi
img=$out/PBConPC-$ver-x86_64.AppImage
ARCH=x86_64 VERSION=$ver APPIMAGE_EXTRACT_AND_RUN=1 "$tool" --no-appstream "$app" "$img" >/dev/null 2>"$out/appimagetool.log" \
    || { cat "$out/appimagetool.log"; exit 1; }
echo "built $img"
