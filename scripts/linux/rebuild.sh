#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# scripts/linux/rebuild.sh
#
# Full rebuild: clean.sh followed by build.sh, so nothing is reused from a
# previous build.
#
# Usage:
#     scripts/linux/rebuild.sh [release|debug]
#
# The same environment overrides as build.sh apply: QMAKE, MAKE and JOBS.
# ============================================================================

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

config="${1:-release}"
case "$config" in
	release|debug)
		;;
	*)
		echo "ERROR: unknown configuration: $config" >&2
		echo "Usage: $(basename -- "${BASH_SOURCE[0]}") [release|debug]" >&2
		exit 2
		;;
esac

echo
echo "Rebuild: cleaning first."
"$script_dir/clean.sh"

echo "Rebuild: building $config."
"$script_dir/build.sh" "$config"
