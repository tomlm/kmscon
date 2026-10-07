#!/usr/bin/env bash
#
# Build the Debian packages Conchix installs in place of trixie-backports'
# kmscon: this fork, and the libtsm it needs.
#
# Runs as root inside a debian:trixie container (CI does exactly that):
#
#   docker run --rm -v "$PWD:/src" -w /src debian:trixie packaging/conchix/build-debs.sh
#
# kmscon is packaged with the trixie-backports debian/ directory, laid over this
# tree, so the result installs and behaves like the stock package. This fork
# needs libtsm 4.8, which trixie-backports doesn't have (it stops at 4.7), so
# libtsm is rebuilt from forky's source first and shipped alongside.
#
# Environment:
#   CONCHIX_REV  the N in the +conchixN version suffix (default 1)
#   OUT          where the .debs are left (default ./out)
#
set -euo pipefail

CONCHIX_REV="${CONCHIX_REV:-1}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$(mkdir -p "${OUT:-$SRC/out}" && cd "${OUT:-$SRC/out}" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

export DEBIAN_FRONTEND=noninteractive
export DEBEMAIL="${DEBEMAIL:-conchix@users.noreply.github.com}"
export DEBFULLNAME="${DEBFULLNAME:-Conchix}"

if [[ ! "$CONCHIX_REV" =~ ^[0-9]+$ ]]; then
	echo "CONCHIX_REV must be a number, got '$CONCHIX_REV'" >&2
	exit 1
fi

# Sources for trixie, trixie-backports and forky; binaries from trixie and
# trixie-backports only, so nothing from forky gets installed. These replace
# the image's own debian.sources: apt refuses two entries for one suite that
# name different keyrings, and newer images name the .pgp one.
KEYRING=/usr/share/keyrings/debian-archive-keyring.pgp
[[ -f "$KEYRING" ]] || KEYRING=/usr/share/keyrings/debian-archive-keyring.gpg
rm -f /etc/apt/sources.list /etc/apt/sources.list.d/*
cat >/etc/apt/sources.list.d/conchix-build.sources <<EOF
Types: deb deb-src
URIs: http://deb.debian.org/debian
Suites: trixie trixie-updates trixie-backports
Components: main
Signed-By: $KEYRING

Types: deb deb-src
URIs: http://deb.debian.org/debian-security
Suites: trixie-security
Components: main
Signed-By: $KEYRING

Types: deb-src
URIs: http://deb.debian.org/debian
Suites: forky
Components: main
Signed-By: $KEYRING
EOF
apt-get update
apt-get install -y --no-install-recommends build-essential ca-certificates devscripts \
	dpkg-dev equivs quilt

#
# libtsm 4.8, rebuilt for trixie
#
cd "$WORK"
apt-get source libtsm/forky
cd "$(find "$WORK" -maxdepth 1 -type d -name 'libtsm-*' | head -n1)"
# The ~ sorts below forky's own version, as backports do, so a real libtsm 4.8
# for trixie would supersede this one; -b lets dch go lower than the changelog.
dch -b --newversion "$(dpkg-parsechangelog -SVersion)~conchix13+$CONCHIX_REV" \
	--distribution trixie --force-distribution \
	"Rebuild for Conchix on trixie, for the kmscon fork."
# forky's packaging wants debhelper compat 14, which only backports has.
apt-get build-dep -y -t trixie-backports ./
dpkg-buildpackage -b -us -uc
apt-get install -y "$WORK"/libtsm4_*.deb "$WORK"/libtsm-dev_*.deb

#
# kmscon, from this tree with trixie-backports' packaging
#
mkdir "$WORK/backports"
cd "$WORK/backports"
apt-get source -t trixie-backports kmscon
DEBIAN_DIR="$(find "$WORK/backports" -maxdepth 2 -type d -name debian | head -n1)"

BUILD="$WORK/kmscon"
mkdir "$BUILD"
tar -C "$SRC" --exclude=./.git --exclude=./out --exclude=./build --exclude=./debian -cf - . |
	tar -C "$BUILD" -xf -
cp -a "$DEBIAN_DIR" "$BUILD/debian"
cd "$BUILD"

# Debian's own patches, which have to keep applying to this fork. The one
# aligning kmsconvt@.service with getty@.service is already made in this tree,
# whose unit file it no longer applies to.
sed -i '/^0001-Change-kmsconvt-.service-to-match-getty-.service.patch$/d' debian/patches/series
QUILT_PATCHES=debian/patches quilt push -a

# The fork needs the libtsm built above.
sed -i 's/libtsm-dev (>= [0-9.]*)/libtsm-dev (>= 4.8.0)/' debian/control
grep -q 'libtsm-dev (>= 4.8.0)' debian/control

UPSTREAM="$(sed -n "s/^    version: '\(.*\)',$/\1/p" meson.build | head -n1)"
VERSION="$UPSTREAM+conchix$CONCHIX_REV-1"
dch --newversion "$VERSION" --distribution trixie-backports --force-distribution \
	"Conchix build of github.com/tomlm/kmscon: OSC 22 mouse pointer shapes."

apt-get build-dep -y -t trixie-backports ./
dpkg-buildpackage -b -us -uc

cp "$WORK"/libtsm4_*.deb "$WORK"/kmscon_*.deb "$OUT"/
echo "$VERSION" >"$OUT/VERSION"
ls -l "$OUT"
