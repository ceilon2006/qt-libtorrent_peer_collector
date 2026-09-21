#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# scripts/linux/build.sh
#
# Incremental out-of-tree qmake build of libtorrent_peer_collector_qt.
#
# Usage:
#     scripts/linux/build.sh [release|debug]
#
# Release builds land in build/, debug builds in build-debug/, so the two
# configurations never share object files. Run the script from anywhere; it
# always builds the project it lives in.
#
# The project links libtorrent-rasterbar through pkg-config, so the
# development package for it must be installed as well as Qt 6.
#
# Environment overrides:
#     QMAKE   qmake executable       (default: qmake6, then qmake)
#     MAKE    make executable        (default: make)
#     JOBS    parallel make jobs     (default: number of processors)
# ============================================================================

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "$script_dir/../.." && pwd)"
project_file="$project_root/libtorrent_peer_collector_qt.pro"

config="${1:-release}"
case "$config" in
	release)
		build_dir="$project_root/build"
		;;
	debug)
		build_dir="$project_root/build-debug"
		;;
	*)
		echo "ERROR: unknown configuration: $config" >&2
		echo "Usage: $(basename -- "${BASH_SOURCE[0]}") [release|debug]" >&2
		exit 2
		;;
esac

if [ ! -f "$project_file" ]; then
	echo "ERROR: project file not found: $project_file" >&2
	exit 1
fi

qmake_bin="${QMAKE:-}"
if [ -z "$qmake_bin" ]; then
	if command -v qmake6 >/dev/null 2>&1; then
		qmake_bin=qmake6
	elif command -v qmake >/dev/null 2>&1; then
		qmake_bin=qmake
	else
		echo "ERROR: neither qmake6 nor qmake was found in PATH." >&2
		echo "Install the Qt 6 development tools, for example:" >&2
		echo "    sudo apt install build-essential pkg-config qt6-base-dev" >&2
		echo "    sudo dnf install gcc-c++ make pkgconf qt6-qtbase-devel" >&2
		exit 1
	fi
fi

make_bin="${MAKE:-make}"
if ! command -v "$make_bin" >/dev/null 2>&1; then
	echo "ERROR: make executable not found: $make_bin" >&2
	exit 1
fi

# The .pro file uses PKGCONFIG += libtorrent-rasterbar. Check it up front so
# the failure is a clear message instead of a qmake "Package not found".
if ! command -v pkg-config >/dev/null 2>&1; then
	echo "ERROR: pkg-config was not found in PATH." >&2
	echo "    sudo apt install pkg-config" >&2
	exit 1
fi

if ! pkg-config --exists libtorrent-rasterbar; then
	echo "ERROR: pkg-config cannot find libtorrent-rasterbar." >&2
	echo "Install the libtorrent development package, for example:" >&2
	echo "    sudo apt install libtorrent-rasterbar-dev" >&2
	echo "    sudo dnf install rb_libtorrent-devel" >&2
	exit 1
fi

jobs="${JOBS:-$(nproc 2>/dev/null || echo 1)}"

echo
echo "Project:       $project_root"
echo "Configuration: $config"
echo "Build folder:  $build_dir"
echo "qmake:         $($qmake_bin -query QMAKE_VERSION 2>/dev/null || echo unknown) ($qmake_bin)"
echo "Qt version:    $($qmake_bin -query QT_VERSION 2>/dev/null || echo unknown)"
echo "libtorrent:    $(pkg-config --modversion libtorrent-rasterbar 2>/dev/null || echo unknown)"
echo

mkdir -p -- "$build_dir"
cd -- "$build_dir"

# qmake is re-run on every build so that a changed .pro is picked up.
"$qmake_bin" "$project_file" "CONFIG+=$config" "CONFIG-=$([ "$config" = release ] && echo debug || echo release)"
"$make_bin" -j"$jobs"

echo
echo "Build completed."
echo "Executable: $build_dir/libtorrent_peer_collector_qt"
echo
