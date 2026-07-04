# GSGO — a DOS enabler for the Roland SCP-55 PCMCIA sound card

A single-command DOS **point enabler** that brings the **Roland SCP-55** PC Card
to life — with its onboard **Sound Canvas** module, playable as a General
MIDI / MPU-401 device — with no Card Services and no Socket Services. It programs
the PCMCIA host controller and the card directly, then gets out of the way.
The only pre-existing no-Card-Services setup for this card is specific to the 
**HP 200LX**; GSGO isn't tied to one machine — it works on Intel 82365-class 
controllers generally (developed and tested on an IBM PC110).

## Features

| | Feature |
|:---:|---|
| ✅ | **MIDI** via MPU-401 UART port and internal SC |
| ✅ | **SysEx sender** (`/MIDI`, `/SYSEX`) — set the synth mode, fire a setup `.MID`, or send raw SysEx from the command line |
| ✅ | **MT-32 emulation mode** (`/MT32`) — with your own supplied `MT32EMUL.MID` file adjacent |
| ✅ | **CS4231A mixer** (levels / routing) |
| ✅ | **Line-in pass-through** (`/LINE`) — mix an external source to the output |

## Usage

Run it once; the configuration sticks in the controller until power-off or suspend
(it is **not** a TSR). GSGO only programs the hardware — then set your game's music
device to **General MIDI / MPU-401 / Roland** at the port it reports (`0x330`):

```
GSGO
```

With no `/S`, GSGO scans every socket, identifies the SCP-55 from its CIS, and
configures it (tagged `(auto)` in its output).

```
GSGO [/MPU=330] [/I=5] [/MT32] [/MIDI=file] [/SYSEX=hex] [/RESET]
     [/LINE[=0..8]] [/S=0..7] [/W=D000] [/OFF]
  /MPU=hex     MPU-401 base — one of 330 / 370 / 3B0 / 3F0 (picks the matching
               card config; the CS4231A codec follows at MPU base + 8)
  /I=dec       IRQ to route to the socket (default 5; /I=0 for none)
  /MT32        switch the Sound Canvas into MT-32 emulation mode by sending
               MT32EMUL.MID from the current folder (see below)
  /MIDI=file   send any Standard MIDI File once (e.g. a synth setup .MID)
  /SYSEX=hex   send a raw MIDI/SysEx message from the command line, as hex bytes
               (e.g. /SYSEX=F04110421240007F0041F7 — non-hex chars are ignored)
  /RESET       GS reset the Sound Canvas (built-in standard GS Reset SysEx)
  /LINE[=n]    pass the line-in jack straight through to the output. n is the
               gain, 0 = 0 dB up to 8 = max (+12 dB), ~1.5 dB per step;
               /LINE alone = max. Mixes alongside the synth — no DMA needed.
  /S=dec       socket number 0..7 (default: auto-detect the card)
  /W=hex       attribute-memory window segment used to write config (default D000)
  /OFF         power the card's socket down and exit
```

Most MPU-401 game music is output-only and needs no IRQ; the default `IRQ 5` is
there for the games that expect one.

**Sound Canvas mode (`/MT32`, `/RESET`, `/MIDI`, `/SYSEX`).** The SCP-55's synth
speaks both Roland **GS** and an **MT-32 emulation** mode. `/RESET` sends the
standard GS Reset (built in). `/MT32` switches to MT-32 emulation by playing
**your own** `MT32EMUL.MID` — the setup file from the Roland SCP-55 driver disk —
placed in the current folder; GSGO ships no Roland content, it just fires the file
you supply (and tells you if it can't find it). `/MIDI=file` is the general form:
send any Standard MIDI File once, so it works for other synth-setup `.MID`s too.
GSGO reads the file, streams its SysEx + channel messages to the MPU-401 (honoring
the file's own timing), and clears any stuck notes first.

`/SYSEX=hex` sends a raw MIDI/SysEx message straight from the command line — give
it the bytes as hex (separators like spaces or commas are ignored), e.g.
`GSGO /SYSEX=F04110421240007F0041F7` is the GS reset. Handy for a one-off SysEx (a
custom patch, a device-specific setup, another synth) without making a `.MID`.

**Line-in pass-through** (`/LINE`) un-mutes the CS4231A's analog Line input so an
external source (a laptop, a phone) plugged into the card's line-in jack is mixed
straight to the card's output — handy for using the connected speakers for other
audio. It's a purely analog mixer path, so it needs no DMA and coexists with the
MIDI setup. If it's too quiet or too hot, dial the gain with `/LINE=0..8`.

## The tool behind it

This project exists because of one tool: **[COMrade](https://github.com/yyzkevin/COMrade)**
by **yyzkevin**. It's a small resident program that runs on the vintage machine
and bridges it to a modern assistant over a plain serial cable — exposing the raw
hardware: read or write any I/O port, peek and poke memory, run DOS commands, and
push files onto the machine and build them there.

That turned a 30-year-old palmtop — an **IBM PC110** with an Intel 82365-class
PCMCIA controller — into something you could develop against like a live REPL. The
whole enabler was written, compiled **on the actual hardware** with Open Watcom,
run, and refined without anyone needing to sit in front of the machine.

The loop:

1. **Reason** from the card's own CIS and the public Intel 82365SL and AD1848 /
   CS4231 register models — what each register does.
2. **Poke it live** over COMrade — power the socket, map a PCIC window, write the
   config register, prod the codec — and **read the result back** the same instant.
3. **Build on the machine** — deploy the C source and compile it with Open Watcom
   right there on the palmtop.
4. **Run it, and listen.** The one thing a serial cable can't carry is sound — so
   the human at the other end simply says *"I can hear it!"*

The clearest moment here: the card identified cleanly, the codec answered — but
where was the synth? The Windows Sound System layout put the CS4231A at an unusual
offset (`base+8`, not the textbook `base+4`), and once that was found, the ports
just below it started looking like an MPU-401. A MIDI arpeggio streamed to `0x330`
over COMrade — and a piano rang out of the headphone jack. **Thank you,
yyzkevin** — none of it happens without COMrade.

## Building

Built with **Open Watcom 1.9** (16-bit, real mode). `GSGO.EXE` is included
prebuilt; to rebuild:

```
wcc -ms GSGO.C -fo=GSGO.obj
wlink system dos name GSGO.exe file GSGO.obj
```

### Clean-room note

This implementation derives **only** from (a) the card's own CIS, read off the
hardware, and (b) public specifications: the Intel 82365SL controller register set,
the PCMCIA standard, and the public AD1848 / Crystal CS4231 register model. No part
of it comes from disassembling any vendor driver — Roland's plain-text `.INF`/`.INI`
resource declarations were read only to confirm the card's identity and factory
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
