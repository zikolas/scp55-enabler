# probes

The diagnostic programs used to reverse-engineer the SCP-55 live over a serial
link, kept because they document how the card was figured out.

The early ones build on the DOS box with `C:\WATCOM\BLD <name>`. The 2026-08
additions cross-build from a host with `./build-dos.sh <name>` (Open Watcom,
16-bit real mode, `wcc -ms -0 -bt=dos`).

## Bring-up

- **SCPROBE.C** — first bring-up: power the socket, verify the Roland MANFID,
  write the COR, map the I/O window, and dump the WSS codec registers. (Looked
  for the codec at base+4 and found garbage — see SCPROBE2.)
- **SCPROBE2.C** — codec identification, take two. Sweeps candidate codec bases
  and finds it at **base+8 (0x338)**; reads the CS4231 MODE2 version register
  `I25 = 0xA0`, pinning the chip as a **Crystal CS4231A**.
- **MPUTEST.C** — confirms the **MPU-401 UART at 0x330** (0xFE ACK), un-mutes the
  CS4231A mixer, and plays a GM arpeggio to prove the onboard **GS Sound Canvas**
  is audible.

## The digital-audio hunt

- **CSPIO.C / CSPIO2.C** — first attempts at DMA-less PIO playback, streaming
  samples to what we believed was the Playback Data Register at `0x33B`. Both
  concluded digital audio was impossible on this card because the playback-ready
  flag never asserted.
  **That conclusion was wrong, and it stood for seven weeks.** `0x33B` is not
  R3 on this card. See SCPR2.
- **SCPGLUE.C** — maps the undocumented block at `0x332-0x337`. Establishes that
  those ports do not alias the codec's index registers, that `0x336` tracks codec
  state, and — with `/SNAP` — reads the card live while the vendor Windows driver
  is playing, which is how the real mechanism was captured.
- **SCPPRDY.C** — two eliminations: `0x33A`/`0x33B` do **not** alias `0x338`/`0x339`
  (A1 is decoded), and PRDY is not merely waiting to be primed.
- **SCPR2.C** — the one that cracked it. Arms the codec in **PIO mode with the
  FIFO deliberately starved** — the discriminating case, since PRDY is meaningless
  in DMA mode, which is how every earlier sweep had been run. `0x336` reads `DF`
  (SER set, matching the underrun in I11; PRDY set) and shows PRDY high on
  20000/20000 polls, while `0x33A` shows it on 0/20000.

  **The codec's four registers are split across two pairs on this card:**

  | register | address |
  |---|---|
  | R0 Index Address | `base+8` |
  | R1 Indexed Data | `base+9` |
  | **R2 Status** | **`base+6`** |
  | **R3 Playback Data** | **`base+7`** |

- **SCPCHAR.C** — characterisation sweep. All eight sample rates work, 5.5 kHz
  through 44.1 kHz; stereo works (the PL/R channel flag alternates exactly 50/50);
  8- and 16-bit both work.
- **SCPPLAY.C** — minimal proof: arms the codec and feeds a tone on PRDY.
- **SCPWAV.C** — plays a real WAV file. Walks RIFF chunks properly, picks the
  nearest available rate and warns about pitch error, handles mono/stereo and
  8/16-bit. Single-buffered, so it clicks once per disk refill.
- **SCPPUMP.C** — gapless playback. An RTC (IRQ 8) interrupt tops up the codec
  FIFO from a ring buffer while the main loop refills the ring from disk, so a
  disk read never stalls the DAC. 30 seconds at 11.025 kHz: 30,722 interrupts,
  one of which found the ring empty.
- **MKWAV.C** — writes a long test tone on the box, so playback can be caught
  mid-stream without pushing a large file over the serial link.

## Notes for anyone reading the code

Two measurement rules the hard way, both of which produced false conclusions here:

- **Never judge a flag with a measurement path slower than the flag's own
  re-assert time.** A starved DAC re-sets its underrun flag in ~90 µs at 11 kHz,
  but an indexed register read costs ~200 µs of settling. Read underrun from
  SER (Status bit 4) — a single `inp` — not from I11.
- **SER clears on read**, so a tight poll loop wipes it faster than the hardware
  can set it. Count how many polls *saw* it, never "is it set right now".
