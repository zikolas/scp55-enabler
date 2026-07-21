# SCP55GO — DOS enabler for the Roland SCP-55 PCMCIA sound card

Single-command DOS enablers that bring the **Roland SCP-55** PC Card to life —
with its onboard **Sound Canvas** module, playable as a General MIDI / MPU-401
device. Two tools, one repo:

- **SCP55GO** (`SCP55GO.COM`, assembly) — the current enabler: **three host
  backends in one 11 KB .COM**, auto-detected at run time.
- **GSGO** (`legacy/GSGO.C`, C) — the original 82365 point enabler this
  project started with; simpler, single-backend, kept in [`legacy/`](legacy/)
  as the readable reference.

## SCP55GO

One binary, three ways onto the bus:

| Backend | What it drives | Notes |
|---|---|---|
| `/PCIC` | Intel 82365-class controllers, programmed directly | no Card Services or Socket Services needed |
| `/CS` | any PCMCIA **Card Services 2.1** stack | stays resident: hot-plug re-enable, live reconfigure |
| `/OB` | HP **OmniBook 300/425/430** ROM Socket Services | with a polite I/O window allocator |

With no mode switch the host is auto-detected: Card Services first (if a CS
arbiter is loaded we must go through it), then the OmniBook Socket Services
signature, then an 82365 probe at `3E0h`. The only pre-existing
no-Card-Services setup for this card is specific to the **HP 200LX**; these
enablers aren't tied to one machine.

### Usage

Configure the card, then set your game's music device to **General MIDI /
MPU-401 / Roland** at the port it reports (`0x330`):

```
SCP55GO
```

In `/PCIC` and `/OB` modes the configuration sticks in the controller until
power-off or suspend (nothing stays resident). In `/CS` mode SCP55GO stays
resident as a Card Services client and re-enables the card on hot-plug; a
later run hands new options to the resident copy.

```
SCP55GO [/PCIC|/CS|/OB] [/MPU=330] [/I=5] [/LINE[=0..8]] [/RESET]
        [/MT32] [/MIDI=file] [/SYSEX=hex] [/S=n] [/W=D000]
        [/FORCE] [/OFF] [/?]
  /PCIC /CS /OB  pick the host backend (default: auto-detect, CS first)
  /MPU=hex     MPU-401 base — one of 330 / 370 / 3B0 / 3F0 (picks the matching
               card config; the CS4231A codec follows at MPU base + 8)
  /I=dec       IRQ to route: 5, 7, 9, 10, 11 or 12 (default 5; /I=0 for none —
               MPU-401 game music is output-only and needs no IRQ)
  /LINE[=n]    pass the line-in jack straight through to the output. n is the
               gain, 0 = 0 dB up to 8 = max (+12 dB), ~1.5 dB per step;
               /LINE alone = max. Mixes alongside the synth — no DMA needed.
  /RESET       GS reset the Sound Canvas (built-in standard GS Reset SysEx)
  /MT32        switch the Sound Canvas into MT-32 emulation mode by sending
               MT32EMUL.MID from the current folder (see below)
  /MIDI=file   send any Standard MIDI File once (e.g. a synth setup .MID)
  /SYSEX=hex   send a raw MIDI/SysEx message from the command line, as hex bytes
               (e.g. /SYSEX=F04110421240007F0041F7 — non-hex chars are ignored)
  /S=dec       socket number (PCIC 0..7, OB 1..2; CS: probe only this socket;
               default: scan and auto-detect the card)
  /W=hex       attribute-memory window segment for the CIS/config access
               (PCIC mode; default D000, auto-relocated if another card's
               memory window already sits there)
  /FORCE       configure without the CIS identity check (needs /S)
  /OFF         PCIC: power the socket down; CS: release the card and go dormant
  /?           show this switch reference
```

**Sound Canvas mode (`/MT32`, `/RESET`, `/MIDI`, `/SYSEX`).** The SCP-55's synth
speaks both Roland **GS** and an **MT-32 emulation** mode. `/RESET` sends the
standard GS Reset (built in; in `/CS` mode it re-fires on hot-plug too). `/MT32`
switches to MT-32 emulation by playing **your own** `MT32EMUL.MID` — the setup
file from the Roland SCP-55 driver disk — placed in the current folder; SCP55GO
ships no Roland content, it just fires the file you supply (and tells you if it
can't find it). `/MIDI=file` is the general form: send any Standard MIDI File
once, so it works for other synth-setup `.MID`s too. SCP55GO reads the file,
streams its SysEx + channel messages to the MPU-401 (honoring the file's own
timing), and clears any stuck notes first.

`/SYSEX=hex` sends a raw MIDI/SysEx message straight from the command line — give
it the bytes as hex (separators like spaces or commas are ignored). Handy for a
one-off SysEx (a custom patch, a device-specific setup, another synth) without
making a `.MID`.

**Line-in pass-through** (`/LINE`) un-mutes the CS4231A's analog Line input so an
external source (a laptop, a phone) plugged into the card's line-in jack is mixed
straight to the card's output — handy for using the connected speakers for other
audio. It's a purely analog mixer path, so it needs no DMA and coexists with the
MIDI setup. If it's too quiet or too hot, dial the gain with `/LINE=0..8`.

### The card, as its CIS tells it

`MANFID C00C/0001`, VERS_1 `"Roland" "PCMCIA Sound Card"`. Config registers at
attribute `400h`; the config index picks the I/O base (330/370/3B0/3F0). One
16-byte I/O block: MPU-401 UART at `base+0/+1` feeding the onboard Sound
Canvas, and the CS4231A codec at `base+8` — **not** the textbook `base+4`,
found the hard way. The codec powers up muted, and the synth reaches the
output through the codec's Aux mixer inputs — the un-mute is what makes it
audible. No FM synth, no gameport, no DMA declared; the Sound Canvas needs
none of that.

## GSGO — the original C enabler

[`legacy/GSGO.C`](legacy/GSGO.C) is where this project started: a **point
enabler** for Intel 82365-class controllers only, in one readable C file. It
takes the same mode-control switches (`/MT32`, `/MIDI`, `/SYSEX`, `/RESET`,
`/LINE`) and remains fully functional — if your machine has a plain
82365-class controller and you want the simplest tool (or want to read how
the card works), build it from [`legacy/`](legacy/) and run:

```
GSGO [/MPU=330] [/I=5] [/MT32] [/MIDI=file] [/SYSEX=hex] [/RESET]
     [/LINE[=0..8]] [/S=0..7] [/W=D000] [/OFF]
```

## The tool behind it

This project exists because of one tool: **[COMrade](https://github.com/yyzkevin/COMrade)**
by **yyzkevin**. It's a small resident program that runs on the vintage machine
and bridges it to a modern assistant over a plain serial cable — exposing the raw
hardware: read or write any I/O port, peek and poke memory, run DOS commands, and
push files onto the machine and build them there.

That turned a 30-year-old palmtop — an **IBM PC110** with an Intel 82365-class
PCMCIA controller — into something you could develop against like a live REPL. The
whole enabler was written, compiled **on the actual hardware** with Open Watcom,
run, and refined without anyone needing to sit in front of the machine. SCP55GO
then grew the same way: the OmniBook Socket Services backend was probed and
verified live over the same serial link, on an **HP OmniBook 425**.

The loop:

1. **Reason** from the card's own CIS and the public Intel 82365SL and AD1848 /
   CS4231 register models — what each register does.
2. **Poke it live** over COMrade — power the socket, map a PCIC window, write the
   config register, prod the codec — and **read the result back** the same instant.
3. **Build on the machine** — deploy the source and compile it right there on
   the palmtop.
4. **Run it, and listen.** The one thing a serial cable can't carry is sound — so
   the human at the other end simply says *"I can hear it!"*

The clearest moment here: the card identified cleanly, the codec answered — but
where was the synth? The Windows Sound System layout put the CS4231A at an unusual
offset (`base+8`, not the textbook `base+4`), and once that was found, the ports
just below it started looking like an MPU-401. A MIDI arpeggio streamed to `0x330`
over COMrade — and a piano rang out of the headphone jack. **Thank you,
yyzkevin** — none of it happens without COMrade.

## Building

**SCP55GO** is a NASM flat binary — one line, on a modern host or on the DOS
box itself, and the prebuilt `SCP55GO.COM` is committed:

```
nasm -f bin SCP55GO.ASM -o SCP55GO.COM
```

**GSGO** (in [`legacy/`](legacy/)) builds with **Open Watcom 1.9** (16-bit,
real mode) via `legacy/BUILD.BAT`, or directly:

```
wcc -ms GSGO.C -fo=GSGO.obj
wlink system dos name GSGO.exe file GSGO.obj
```

The compiled `GSGO.EXE` is not committed — build it from the source in
`legacy/`.

### Clean-room note

This implementation derives **only** from (a) the card's own CIS, read off the
hardware, and (b) public specifications and live probing: the Intel 82365SL
controller register set, the PCMCIA standard — the Card Services and Socket
Services APIs via **RBIL61** and the published SystemSoft CardSoft technical
guide (API binding) — the HP OmniBook Socket Services behavior probed live on
the hardware, the public AD1848 / Crystal CS4231 register model, and the
published MPU-401 UART and Roland GS Reset commands. No part of it comes from
disassembling any vendor driver — Roland's plain-text `.INF`/`.INI` resource
declarations were read only to confirm the card's identity and factory
resources (I/O `330-33B`, gameport `201`, IRQ `5`, no DMA).

## Credits

- **[COMrade](https://github.com/yyzkevin/COMrade)** by **yyzkevin** — the
  serial/MCP bridge that made remote, real-hardware development possible. This
  project simply would not exist without it.

## License

Copyright (c) 2026 zikolas. MIT License — see [LICENSE](LICENSE). The MIT license
covers this project's own source code only.

## Trademarks

Roland, GS, Sound Canvas, MT-32, and SCP-55 are trademarks of Roland Corporation.
This project is independent and is not affiliated with, sponsored by, or endorsed
by Roland. Those names are used only to identify the hardware and its functions.

## Disclaimer

A hobby project, shared in the spirit of retro tinkering. It pokes PCMCIA
controller and sound-chip registers directly on 30-year-old hardware:

- **Provided as-is, no guarantees** — it may or may not work with your card, your
  machine, or your particular phase of the moon.
- **No warranty and no responsibility for any damage** — to hardware, data,
  software, or sanity. You run it entirely at your own risk.
- It's only been exercised on the hardware named above; elsewhere, your mileage may
  vary.
