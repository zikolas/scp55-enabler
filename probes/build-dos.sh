#!/bin/sh
# Build a 16-bit real-mode DOS probe with Open Watcom on macOS.
#   ./build-dos.sh SCPGLUE
set -e
cd "$(dirname "$0")"
N="$1"
export WATCOM="$HOME/tools/ow2"
export PATH="$WATCOM/armo64:$PATH"
export INCLUDE="$WATCOM/h"
wcc -ms -0 -zq -bt=dos -fo="$N.obj" "$N.C"
wlink system dos option quiet name "$N.EXE" file "$N.obj"
ls -la "$N.EXE"
