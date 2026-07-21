# legacy — the original C point enabler

`GSGO.C` here is the **original 82365-only** DOS point enabler this project
grew from (Open Watcom, 16-bit real mode). It is kept for reference and
history; it is **superseded** by the unified assembly enabler
`../SCP55GO.ASM` (→ `SCP55GO.COM`), which adds the Card Services and
OmniBook Socket Services backends alongside the direct-PCIC path.

GSGO is still a complete, working enabler for a plain 82365-class
controller, and the single readable C file is the clearest place to see how
the card is brought up. The mode-control switches (`/MT32`, `/MIDI`,
`/SYSEX`, `/RESET`, `/LINE`) work the same as in SCP55GO.

The compiled `GSGO.EXE` is intentionally **not** committed; build it from
this source with `BUILD.BAT`, or directly:

```
wcc -ms GSGO.C -fo=GSGO.obj
wlink system dos name GSGO.exe file GSGO.obj
```
