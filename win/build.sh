#!/bin/sh
# Build SCPTRACE.VXD + SCPTRACE.EXE for Windows 98 on macOS,
# using the proven rex-cfu1 rig (OW2 + JWasm + patched Win98 DDK includes).
set -e
cd "$(dirname "$0")"

CFU1WIN="$HOME/Projects/rex-cfu1/win"
export WATCOM="$HOME/tools/ow2"
export PATH="$WATCOM/armo64:$PATH"
export INCLUDE="$WATCOM/h:$WATCOM/h/nt"

echo "=== SCPTRACE.VXD ==="
# WIN40COMPAT: stamp DDB SDK 0x0400 so one binary serves all of Win9x
# (see rex-cfu1 build.sh for the whole story).
jwasm -q -coff -D BLD_COFF -D IS_32 -D MASM6 -D DEBLEVEL=0 -D WIN40COMPAT \
      -I"$CFU1WIN/ddkinc" -Fo=SCPTRACE.obj SCPTRACE.ASM
# must be DYNAMIC or CreateFile("\\\\.\\...") fails with err 2
wlink format windows vxd dynamic option quiet, map=SCPTRACE.map \
      name SCPTRACE.VXD file SCPTRACE.obj export SCPTRACE_DDB.1

python3 - <<'PYEOF'
import struct, sys
d = open('SCPTRACE.VXD', 'rb').read()
le = struct.unpack('<I', d[0x3c:0x40])[0]
flags = struct.unpack('<I', d[le+0x10:le+0x14])[0]
i = d.find(b'SCPTRACE')          # DDB_Name, exactly 8 chars -> no padding
sdk = struct.unpack('<H', d[i-0x0C+4:i-0x0C+6])[0]
ok = (flags & 0x38000) == 0x38000 and sdk == 0x0400
print("  LE flags 0x%X, DDB SDK 0x%04X, %d bytes -- %s"
      % (flags, sdk, len(d), "OK" if ok else "*** WRONG STAMP ***"))
sys.exit(0 if ok else 1)
PYEOF

echo "=== SCPTRACE.EXE ==="
wcc386 -bt=nt -zq -w4 -ox -fo=SCPTRACE.obj32 SCPTRACE.C
wlink system nt option quiet name SCPTRACE.EXE file SCPTRACE.obj32

mkdir -p dist
cp SCPTRACE.VXD SCPTRACE.EXE dist/
ls -la dist/
