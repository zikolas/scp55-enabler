# SCPTRACE — SCP-55 wave-path I/O tracer (Windows 9x)

A dynamically-loadable Win9x VxD that traps ring-3 port I/O on the SCP-55's
window (`0x330-0x33F`) and the PCIC index/data pair (`0x3E0/0x3E1`), logs
every access with a millisecond timestamp, and passes it through to real
hardware. Retargeted from `YTRACE` (pcc10xg), which was built and run on
this same 235 + Win98 bench.

Goal: find out how the vendor driver feeds wave audio, given that the
CS4231A's own PIO engine provably refuses data on this card (`probes/CSPIO2.C`,
and the 2026-07-26 VSBPCMCIA retest).

## Build

    ./build.sh

Needs the rex-cfu1 rig: `$HOME/tools/ow2` (Open Watcom, `armo64`) plus the
patched Win98 DDK includes at `~/Projects/rex-cfu1/win/ddkinc`. Produces
`dist/SCPTRACE.VXD` (~17.7 KB) and `dist/SCPTRACE.EXE`. The build verifies
the LE flags (`0x38000` = DYNAMIC) and the DDB SDK stamp (`0x0400`); a wrong
stamp fails the build rather than failing mysteriously at `CreateFile`.

## Deploy

Both files to `C:\` on the 235 via COMrade `file_write` with `src_path`
(stream it, never split — CRC-32 verified end to end). They must sit in the
same directory; the EXE loads the VxD from its own path first.

## Runs, in order

**1. Hook-mask probe — 5 minutes, decides everything else.**

    SCPTRACE

Read the `hooked` value it prints (also written to `C:\SCPTRACE.TXT`
immediately, before anything else happens). Bits 0-15 are ports
`0x330-0x33F`, bit 16 is `0x3E0`, bit 17 is `0x3E1`.

* `3FFFF` — everything hooked. The wave feed is trappable; go to run 3.
* card bits missing — `SCP95.VXD` owns the window and does its I/O at
  ring 0, exactly as the Yamaha driver did (that run came back `0x300`).
  Ring-3 trapping is then structurally blocked; see *Escape hatches*.

**2. Card-init trace — works regardless of who owns the card window.**

    SCPTRACE /PCIC

Then make the driver re-run init: eject and reinsert the card, or Device
Manager disable → enable. `0x3E0/0x3E1` hooking is proven on this bench.
The dump annotates every `0x3E1` access with the register the preceding
`0x3E0` write selected, and flags PCIC **memory-window** registers
(`0x10-0x15`, `0x18-0x1D`, …) loudly. Memory-window writes here would mean
the driver maps card memory at init — which is the leading suspect for the
wave channel, since the CIS declares 512 bytes of function-specific common
memory (`CISTPL_DEVICE = D9 00`) that no probe of ours has ever mapped.

**3. Wave-feed trace** (only if run 1 came back clean):

    SCPTRACE

Play a short wave file, press a key. The per-port histogram at the bottom of
`C:\SCPTRACE.TXT` says which port carries the samples.

## Reading the dump

`C:\SCPTRACE.TXT` is decoded text, `C:\SCPTRACE.BIN` the raw event dwords
(`[7:0]` value, `[15:8]` port low byte, `[16]` dir 1=OUT, `[31:17]` ms).
Port names come from `SCPROBE2`: `330/331` MPU-401, `332-337` the
undocumented glue block, `338-33B` CS4231A IAR/IDR/status/PDR.

The buffer is capture-**first**-4096, not circular, so an init sequence
survives even when a wave feed overruns it. Word and string I/O arrive
decomposed into byte events (`Emulate_Non_Byte_IO`), so a `REP OUTSW` feed
shows up as byte pairs.

## Known limits

* `Install_IO_Handler` traps VM (ring-3 / V86) I/O only. **A VxD's own ring-0
  port I/O is invisible to it**, and a second handler cannot be placed on a
  port another VxD already owns.
* Every trapped access is a ring transition. Expect playback to stutter or
  slow during a capture; that is the instrument, not a fault in the card.
* Hardware access from a Win98 DOS box on this machine is untrustworthy
  (port arbitration, then virtualized garbage reads), and the 235 socket
  loses power on warm reboot — which is why this is a VxD and not a DOS
  probe.

## Escape hatches if run 1 says ring-0

* **Win3.1 driver under Win98.** `OEMSETUP.INF` shows `scp55.drv,
  "MIDI,WAVE,AUX"` with **no VxD at all** — full wave playback from a plain
  ring-3 16-bit driver. Installed as the Win98 wave driver
  (`SYSTEM.INI [drivers] wave=`, 32-bit driver disabled, socket point-enabled
  in real mode), the feed moves into ring-3 code in the System VM, where this
  tracer can see it.
* **CR4.DE debug-register I/O breakpoints.** Catch I/O at any CPL, ring 0
  included. Four ports per pass on the 235's Pentium MMX — specced during the
  Yamaha work, never built.
