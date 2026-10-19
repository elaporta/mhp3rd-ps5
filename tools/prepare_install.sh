#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Public source-only entry point. Compatible with Bash 3.2.
set -eu
export PYTHONDONTWRITEBYTECODE=1
if [ "${BASH_VERSINFO[0]}" -lt 3 ] || { [ "${BASH_VERSINFO[0]}" -eq 3 ] && [ "${BASH_VERSINFO[1]}" -lt 2 ]; }; then
    printf '%s\n' 'ERROR: Bash 3.2 or newer is required' >&2
    exit 2
fi
command -v python3 >/dev/null 2>&1 || { printf '%s\n' 'ERROR: Python 3.9 or newer is required' >&2; exit 2; }
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 2)' || { printf '%s\n' 'ERROR: Python 3.9 or newer is required' >&2; exit 2; }
SCRIPT_DIR=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
CONTAINER=0
for ARG in "$@"; do
    [ "$ARG" != '--container' ] || CONTAINER=1
done
if [ "$(uname -s)" = Darwin ] || [ "$CONTAINER" -eq 1 ]; then
    exec python3 "$SCRIPT_DIR/container_install.py" "$@"
fi
exec python3 "$SCRIPT_DIR/prepare_install.py" "$@"
