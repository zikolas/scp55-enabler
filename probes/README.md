# probes

The throwaway diagnostic programs used to reverse-engineer the SCP-55 live over
COMrade, kept because they document how the card was figured out. Not needed to
use `GSGO.EXE`. Each builds the same way: `C:\WATCOM\BLD <name>`.

- **SCPROBE.C** — first bring-up: power the socket, verify the Roland MANFID,
  write the COR, map the I/O window, and dump the WSS codec registers. (Looked
  for the codec at base+4 and found garbage — see SCPROBE2.)
- **SCPROBE2.C** — codec identification, take two. Sweeps candidate codec bases
  and finds it at **base+8 (0x338)**; reads the CS4231 MODE2 version register
  `I25 = 0xA0`, pinning the chip as a **Crystal CS4231A**.
- **MPUTEST.C** — confirms the **MPU-401 UART at 0x330** (0xFE ACK), un-mutes the
  CS4231A mixer, and plays a GM arpeggio to prove the onboard **GS Sound Canvas**
  is audible.
- **CSPIO.C** — tests whether the CS4231A can do **DMA-less PIO playback**
  (streaming samples to the PDR at 0x33B). Result: negative — the playback-ready
  flag never asserts, so digital audio isn't possible on this card + host.
