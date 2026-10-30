#!/bin/bash
# Bazarish project (c) 2026
# Builds, tests and packs the daemon as a .deb for Debian 13: build.sh <out-dir>.
# Runs from the repository root of a git checkout on a Debian 13 host with the build
# dependencies.
set -euo pipefail

readonly kOut=$1
readonly kJobs=${BAZARISH_JOBS:-4}
readonly kPackage=bazarish-bot
readonly kBuild=build-deb
readonly kStage=$kBuild/stage

# The tag a release is cut from, or dev and the commit, counted so that apt takes a
# later build for newer.
if tag=$(git describe --tags --exact-match 2>/dev/null); then
    if [[ ! $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo "a release tag is v and three numbers, as in v1.2.3: $tag" >&2
        exit 1
    fi
    readonly kVersion=${tag#v}
else
    readonly kVersion=0~dev.$(git rev-list --count HEAD)+$(git rev-parse --short HEAD)
fi

cmake -S . -B "$kBuild" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$kBuild" -j"$kJobs"
ctest --test-dir "$kBuild" -j"$kJobs" --output-on-failure

rm -rf "$kStage"
install -D -m 0755 "$kBuild/bazarish-bot" "$kStage/usr/lib/$kPackage/bazarish-bot"
strip --strip-unneeded "$kStage/usr/lib/$kPackage/bazarish-bot"
mkdir -p "$kStage/usr/lib/$kPackage/web"
cp -r web/. "$kStage/usr/lib/$kPackage/web/"
install -D -m 0644 packaging/bazarish-bot.service \
    "$kStage/usr/lib/systemd/system/bazarish-bot.service"
install -D -m 0644 config.example.json "$kStage/usr/share/$kPackage/bot.json"
install -D -m 0644 README.md "$kStage/usr/share/doc/$kPackage/README.md"
mkdir -p "$kStage/usr/bin"
ln -s "../lib/$kPackage/bazarish-bot" "$kStage/usr/bin/bazarish-bot"

# dpkg-shlibdeps reads the package name from a control file beside the binary.
readonly kProbe=$kBuild/shlibs
rm -rf "$kProbe"
mkdir -p "$kProbe/debian"
printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' "$kPackage" "$kPackage" \
    > "$kProbe/debian/control"
depends=$(cd "$kProbe" && dpkg-shlibdeps -O "../../$kStage/usr/lib/$kPackage/bazarish-bot" \
    | sed -n 's/^shlibs:Depends=//p')

readonly kArch=$(dpkg --print-architecture)
mkdir -p "$kStage/DEBIAN"
cat > "$kStage/DEBIAN/control" <<CONTROL
Package: $kPackage
Version: $kVersion
Architecture: $kArch
Maintainer: Bazarish <noreply@bazari.sh>
Depends: adduser, systemd, $depends
Section: net
Priority: optional
Homepage: https://bazari.sh
Description: Bazarish accounts that run as a service
 Bots and groups on one embedded I2P router, a REST API for automation and a
 web interface for the operator.
CONTROL
install -m 0755 packaging/deb/postinst "$kStage/DEBIAN/postinst"
install -m 0755 packaging/deb/prerm "$kStage/DEBIAN/prerm"
install -m 0755 packaging/deb/postrm "$kStage/DEBIAN/postrm"

mkdir -p "$kOut"
dpkg-deb --root-owner-group --build "$kStage" "$kOut/${kPackage}_${kVersion}_${kArch}.deb"
