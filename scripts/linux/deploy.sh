#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# scripts/linux/deploy.sh
#
# Copies the built executable into a deployment folder, or installs it into a
# prefix with a .desktop entry so it shows up in the application menu.
#
# Usage:
#     scripts/linux/deploy.sh [-y] [DEST]
#
#     DEST   where to deploy (default: deploy/ inside the project folder,
#            which clean.sh removes and .gitignore keeps out of git)
#     -y     do not ask before emptying the deployment folder
#
#     scripts/linux/deploy.sh --install [PREFIX]
#
#     Installs to PREFIX/bin and writes PREFIX/share/applications/
#     libtorrent_peer_collector_qt.desktop. PREFIX defaults to ~/.local, which
#     needs no root. Use /usr/local with sudo for a system-wide install.
#
# Unlike the Windows deploy there is no windeployqt step. On Linux the
# executable expects Qt 6 and libtorrent-rasterbar to be installed as system
# packages on the target machine; the runtime packages are listed at the end
# with their current versions so you know what the target needs.
#
# Environment overrides:
#     SOURCE_EXE   executable to deploy
#                  (default: build/libtorrent_peer_collector_qt, then
#                   build-debug/libtorrent_peer_collector_qt)
# ============================================================================

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "$script_dir/../.." && pwd)"
exe_name="libtorrent_peer_collector_qt"

mode="copy"
assume_yes=0
dest=""

while [ $# -gt 0 ]; do
	case "$1" in
		-y)
			assume_yes=1
			;;
		--install)
			mode="install"
			;;
		-h|--help)
			sed -n '4,30p' -- "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
			exit 0
			;;
		-*)
			echo "ERROR: unknown option: $1" >&2
			exit 2
			;;
		*)
			if [ -n "$dest" ]; then
				echo "ERROR: more than one destination given." >&2
				exit 2
			fi
			dest="$1"
			;;
	esac
	shift
done

# ---- locate the executable to deploy ----
source_exe="${SOURCE_EXE:-}"
if [ -z "$source_exe" ]; then
	for candidate in \
		"$project_root/build/$exe_name" \
		"$project_root/build-debug/$exe_name"
	do
		if [ -x "$candidate" ]; then
			source_exe="$candidate"
			break
		fi
	done
fi

if [ -z "$source_exe" ]; then
	echo "ERROR: no built executable was found under $project_root/build" >&2
	echo "Build it first:" >&2
	echo "    $script_dir/build.sh" >&2
	echo "or set SOURCE_EXE to the full path of $exe_name." >&2
	exit 1
fi

if [ ! -x "$source_exe" ]; then
	echo "ERROR: source executable not found or not executable: $source_exe" >&2
	exit 1
fi

# Runtime packages the target machine needs. Read from the executable itself so
# the list is always what this build actually links.
print_runtime_needs() {
	echo "Runtime libraries this executable links (target machine needs them):"
	if command -v ldd >/dev/null 2>&1; then
		ldd -- "$source_exe" | awk '/libQt6|libtorrent|libssl|libcrypto|libboost/ { print "    " $1 }' | sort -u
	else
		echo "    (ldd not available)"
	fi
	echo "Debian/Ubuntu: libqt6widgets6 libqt6network6 qt6-qpa-plugins libtorrent-rasterbar2.0t64"
	echo "               (libtorrent-rasterbar2.0 on releases before Ubuntu 24.04)"
	echo "Fedora:        qt6-qtbase qt6-qtbase-gui rb_libtorrent"
	echo "See docs/build-requirements.html for the full list."
}

if [ "$mode" = "install" ]; then
	prefix="${dest:-$HOME/.local}"
	bin_dir="$prefix/bin"
	app_dir="$prefix/share/applications"
	desktop_file="$app_dir/$exe_name.desktop"

	echo
	echo "Source:   $source_exe"
	echo "Install:  $bin_dir/$exe_name"
	echo "Desktop:  $desktop_file"
	echo

	mkdir -p -- "$bin_dir" "$app_dir"
	install -m 755 -- "$source_exe" "$bin_dir/$exe_name"

	cat > "$desktop_file" <<EOF
[Desktop Entry]
Type=Application
Name=Libtorrent Peer Collector
Comment=Collect peers seen by a libtorrent session for a torrent
Exec=$bin_dir/$exe_name
Terminal=false
Categories=Network;P2P;Qt;
EOF

	if command -v update-desktop-database >/dev/null 2>&1; then
		update-desktop-database -q -- "$app_dir" 2>/dev/null || true
	fi

	echo "Install completed."
	case ":$PATH:" in
		*":$bin_dir:"*) ;;
		*) echo "Note: $bin_dir is not on PATH." ;;
	esac
	echo
	print_runtime_needs
	echo
	exit 0
fi

# ---- copy mode ----
deploy_dir="${dest:-$project_root/deploy}"
deploy_dir="$(cd -- "$(dirname -- "$deploy_dir")" 2>/dev/null && pwd)/$(basename -- "$deploy_dir")"

# The folder is deleted and recreated, so refuse anything that would take the
# source tree or the system with it.
case "$deploy_dir" in
	/|/home|"$HOME")
		echo "ERROR: refusing to use $deploy_dir as the deployment folder." >&2
		exit 1
		;;
esac
case "$project_root/" in
	"$deploy_dir"/*)
		echo "ERROR: refusing a deployment folder that holds the project: $deploy_dir" >&2
		echo "It is the project folder or one of its parents, and the deployment" >&2
		echo "folder is deleted before it is written." >&2
		exit 1
		;;
esac

echo
echo "Source:    $source_exe"
echo "Deploy to: $deploy_dir"
echo

if [ -e "$deploy_dir" ]; then
	if [ "$assume_yes" -ne 1 ]; then
		printf 'This DELETES everything in %s. Continue? [y/N] ' "$deploy_dir"
		read -r confirm
		case "$confirm" in
			y|Y) ;;
			*)
				echo "Cancelled."
				exit 1
				;;
		esac
	fi
	echo "Cleaning deploy directory: $deploy_dir"
	rm -rf -- "$deploy_dir"
fi

mkdir -p -- "$deploy_dir"
install -m 755 -- "$source_exe" "$deploy_dir/$exe_name"

echo
echo "Deploy completed."
echo "Output folder: $deploy_dir"
echo
print_runtime_needs
echo
