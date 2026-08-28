#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright XCENA Inc.
#
# install.sh -- put a built cmed on this host and leave it ready to start.
#
# Every mode here is set by install(1) rather than by a copy, because a copy creates the destination
# through the umask and a permissive one would widen what the daemon refuses to read.
#
#   sudo daemon/deploy/install.sh --build build
#
# The config it leaves behind is the example, which names a shm region and /run/cmed. Edit it before
# starting the daemon anywhere the region is not shm:/cme-region.

set -euo pipefail

BUILD=build
ACCOUNT=cme

# A staging root, the way a package build asks for one. Only where files are laid down: the paths
# inside the unit stay absolute, because they are what the running daemon reads.
DESTDIR=

# Not options: the unit beside this file names all three, so moving one here would leave it
# pointing at nothing. An operator who wants them elsewhere edits the unit.
PREFIX=/usr/local/bin
CONFDIR=/etc/cme
UNITDIR=/etc/systemd/system

usage() {
	cat <<'EOF'
install.sh -- install cmed, its config and its unit.

  --build DIR      where cmed was built, relative to the repository root (default: build)
  --account NAME   the system account the daemon runs as (default: cme)
  --destdir DIR    lay the files under DIR instead of on this host, for a package build or a
                   look at what an install would do. Skips the account and systemctl.
  --uninstall      take all of it away again
  --help           this

The binary goes to /usr/local/bin, the config to /etc/cme and the unit to /etc/systemd/system,
because the unit beside this script names those three.
EOF
}

UNINSTALL=false
while [ $# -gt 0 ]; do
	case "$1" in
	--build) BUILD=$2; shift 2 ;;
	--account) ACCOUNT=$2; shift 2 ;;
	--destdir) DESTDIR=$2; shift 2 ;;
	--uninstall) UNINSTALL=true; shift ;;
	--help) usage; exit 0 ;;
	*) echo "install.sh: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
	esac
done

# The repository root, from this script's own location. A caller in any directory then names the
# build with the same relative path the build commands use.
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$HERE/../.." && pwd)

if [ -z "$DESTDIR" ] && [ "$(id -u)" -ne 0 ]; then
	echo "install.sh: needs root to write $PREFIX, $CONFDIR and $UNITDIR" >&2
	echo "install.sh: or pass --destdir DIR to lay the same files somewhere harmless" >&2
	exit 1
fi

if [ "$UNINSTALL" = true ]; then
	systemctl stop cmed.service 2>/dev/null || true
	systemctl disable cmed.service 2>/dev/null || true
	rm -f "$UNITDIR/cmed.service" "$PREFIX/cmed"
	systemctl daemon-reload
	echo "removed the binary and the unit. $CONFDIR is left alone, since it is yours."
	exit 0
fi

BINARY="$ROOT/$BUILD/daemon/cmed"
if [ ! -x "$BINARY" ]; then
	echo "install.sh: no cmed at $BINARY. Build it first:" >&2
	echo "  cmake -S . -B $BUILD && cmake --build $BUILD -j" >&2
	exit 1
fi

# Unprivileged and unable to log in: the daemon needs a uid of its own for the socket's admission
# boundary, and nothing else that an account usually carries.
if [ -z "$DESTDIR" ] && ! id -u "$ACCOUNT" >/dev/null 2>&1; then
	useradd --system --shell /usr/sbin/nologin --no-create-home "$ACCOUNT"
	echo "created the system account '$ACCOUNT'"
fi

install -D -m 0755 "$BINARY" "$DESTDIR$PREFIX/cmed"
install -d -m 0755 "$DESTDIR$CONFDIR"

# Never overwritten. A second run is an upgrade of the binary, and a config the operator edited is
# the one thing here that cannot be regenerated.
if [ -e "$DESTDIR$CONFDIR/cmed.yaml" ]; then
	echo "kept $CONFDIR/cmed.yaml as it is"
else
	install -m 0644 "$HERE/cmed.example.yaml" "$DESTDIR$CONFDIR/cmed.yaml"
	echo "installed $CONFDIR/cmed.yaml from the example"
fi

install -D -m 0644 "$HERE/cmed.example.service" "$DESTDIR$UNITDIR/cmed.service"
if [ -z "$DESTDIR" ]; then
	systemctl daemon-reload
fi

cat <<EOF
installed:
  $PREFIX/cmed
  $CONFDIR/cmed.yaml
  $UNITDIR/cmed.service

Edit $CONFDIR/cmed.yaml for this host, then:
  systemctl start cmed && systemctl status cmed

Type=notify makes that start the test: it blocks until the daemon can answer a requester.
EOF
