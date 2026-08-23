/* SCPWAV.C - play a WAV file on the Roland SCP-55 with no DMA anywhere.
 *
 * Reference implementation of the path proven on 2026-08-24.  The card's codec
 * registers are NOT at the datasheet's default offsets:
 *
 *     R0 IAR = base+8    R1 IDR = base+9
 *     R2 STATUS = base+6   R3 PDR = base+7        <-- the whole discovery
 *
 * Feed protocol: read R2, and whenever PRDY (D1) is set, write one byte to R3.
 * Underrun shows up as SER (D4) in the same read - one inp, no indexed access,
 * which matters because an indexed read costs ~200us and a starved DAC re-sets
 * its error flag in ~90us at 11 kHz.
 *
 * This card populates ONE crystal, 16.9344 MHz, on XI1, so C2SL=0 selects the
 * datasheet's XTAL2 rate column.  We never set C2SL=1 - there is no second
 * crystal and the resync hangs until the socket is power-cycled.
 *
 * Formats: 8-bit unsigned and 16-bit signed LE, mono or stereo, all verified.
 *
 * Usage:  SCPWAV file.wav [/BASE=330] [/CFS=n]
 *
 * SAFETY: codec registers only.  No PCIC, no glue writes, never C2SL=1.
 * Disarms and restores mute on exit, including on a keypress abort.
 */
#include <stdio.h>
#include <conio.h>
#include <string.h>
#include <stdlib.h>
#include <i86.h>

static unsigned BASE = 0x330;
#define IAR (BASE+8)
#define IDR (BASE+9)
#define SRP (BASE+6)
#define PDR (BASE+7)

#define SR_PRDY 0x02
#define SR_SER  0x10

#define BUFSZ 16384

static void iod(unsigned long n){ while (n--) (void)inp(0x80); }
#define MS(x) iod((unsigned long)(x) * 1000UL)
static void ci_wait(void)
{ unsigned long i; for(i=0;i<400000UL;i++) if(!(inp(IAR)&0x80)) return; }
static void ci_put(unsigned char idx, unsigned char v)
{ ci_wait(); outp(IAR, idx); iod(200); outp(IDR, v); iod(200); }
static unsigned char ci_get(unsigned char idx)
{ ci_wait(); outp(IAR, idx); iod(200); return (unsigned char)inp(IDR); }
static void ci_put_mce(unsigned char idx, unsigned char v)
{ ci_wait(); outp(IAR,(unsigned char)(0x40|idx)); iod(200); outp(IDR,v); iod(200);
  ci_wait(); outp(IAR, idx); iod(200); MS(2); }
static unsigned long bios_ticks(void)
{ unsigned long far *t = (unsigned long far *)MK_FP(0x40, 0x6C); return *t; }

/* this card's rate table: datasheet XTAL2 column, selected with C2SL=0 */
static unsigned long rate_of[8] =
    { 5512UL, 11025UL, 18900UL, 22050UL, 37800UL, 44100UL, 33075UL, 6620UL };

static unsigned char buf[BUFSZ];

static unsigned long rd32(unsigned char *p)
{ return (unsigned long)p[0] | ((unsigned long)p[1]<<8)
       | ((unsigned long)p[2]<<16) | ((unsigned long)p[3]<<24); }
static unsigned rd16(unsigned char *p)
{ return (unsigned)p[0] | ((unsigned)p[1]<<8); }

int main(int argc, char **argv)
{
    char *fname = NULL;
    int i, cfs = -1, best = 1;
    unsigned chan = 0, bits = 0;
    unsigned long srate = 0, dlen = 0, played = 0, ser = 0, t0, t1;
    unsigned char hdr[64], i8, i11;
    FILE *f;

    for (i = 1; i < argc; i++) {
        if      (!strnicmp(argv[i], "/BASE=", 6)) sscanf(argv[i]+6, "%x", &BASE);
        else if (!strnicmp(argv[i], "/CFS=",  5)) sscanf(argv[i]+5, "%d", &cfs);
        else if (argv[i][0] != '/') fname = argv[i];
        else { printf("usage: SCPWAV file.wav [/BASE=330] [/CFS=n]\n"); return 1; }
    }
    if (!fname) { printf("usage: SCPWAV file.wav [/BASE=330] [/CFS=n]\n"); return 1; }

    /* ---- parse the WAV: walk chunks, do not assume data is at offset 36 --- */
    f = fopen(fname, "rb");
    if (!f) { printf("cannot open %s\n", fname); return 1; }
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4)
                                   || memcmp(hdr + 8, "WAVE", 4)) {
        printf("%s is not a RIFF/WAVE file\n", fname); fclose(f); return 1;
    }
    for (;;) {
        unsigned long clen;
        if (fread(hdr, 1, 8, f) != 8) { printf("no data chunk\n"); fclose(f); return 1; }
        clen = rd32(hdr + 4);
        if (!memcmp(hdr, "fmt ", 4)) {
            unsigned char fmt[32];
            unsigned want = (unsigned)(clen > 32 ? 32 : clen);
            if (fread(fmt, 1, want, f) != want) { printf("short fmt\n"); fclose(f); return 1; }
            if (rd16(fmt) != 1) { printf("not PCM (format %u)\n", rd16(fmt));
                                  fclose(f); return 1; }
            chan  = rd16(fmt + 2);
            srate = rd32(fmt + 4);
            bits  = rd16(fmt + 14);
            if (clen > want) fseek(f, (long)(clen - want), SEEK_CUR);
        } else if (!memcmp(hdr, "data", 4)) {
            dlen = clen; break;
        } else {
            fseek(f, (long)(clen + (clen & 1)), SEEK_CUR);   /* pad to even */
        }
    }
    if (!chan || !bits) { printf("no fmt chunk\n"); fclose(f); return 1; }

    printf("SCPWAV - %s: %lu Hz, %u ch, %u-bit, %lu bytes\n",
           fname, srate, chan, bits, dlen);
    if (chan > 2 || (bits != 8 && bits != 16)) {
        printf("unsupported format\n"); fclose(f); return 1;
    }

    /* ---- nearest available rate ---------------------------------------- */
    if (cfs < 0) {
        unsigned long bd = 0xFFFFFFFFUL;
        for (i = 0; i < 8; i++) {
            unsigned long d = rate_of[i] > srate ? rate_of[i] - srate
                                                 : srate - rate_of[i];
            if (d < bd) { bd = d; best = i; }
        }
        cfs = best;
    }
    printf("using CFS=%d (%lu Hz)", cfs, rate_of[cfs]);
    if (rate_of[cfs] != srate)
        printf("  -- file is %lu Hz, so pitch will be off by %ld%%",
               srate, (long)((rate_of[cfs] * 100UL) / srate) - 100L);
    printf("\n");

    i8 = (unsigned char)((cfs & 7) << 1);
    if (chan == 2)  i8 |= 0x10;                  /* stereo            */
    if (bits == 16) i8 |= 0x40;                  /* linear 16-bit LE  */

    /* ---- codec presence + arm ------------------------------------------ */
    for (i = 0; i < 8; i++) {
        outp(IAR, (unsigned char)i); iod(200);
        if ((inp(IAR) & 0x1F) != i) {
            printf("codec not responding at %03X - run the enabler first\n", IAR);
            fclose(f); return 1;
        }
    }
    ci_put(0x0C, (unsigned char)(ci_get(0x0C) | 0x40));   /* MODE2         */
    ci_put_mce(0x08, i8);
    ci_put_mce(0x09, 0xC8);                               /* PPIO | CPIO   */
    ci_put(0x00, 0xC0); ci_put(0x01, 0xC0);
    ci_put(0x02, 0x0A); ci_put(0x03, 0x0A);
    ci_put(0x06, 0x03); ci_put(0x07, 0x03);               /* DAC unmuted   */
    ci_put(0x0A, 0x02);                                   /* IEN           */
    ci_put(0x1A, 0xC0);
    ci_put(0x15, 0x00); ci_put(0x14, 0x42);
    ci_put(0x10, 0xC0);                                   /* OLB | TE      */
    ci_put(0x09, 0xC9);                                   /* + PEN         */
    MS(10);
    (void)inp(SRP);

    printf("playing (I8=%02X) - any key to stop\n", i8);
    t0 = bios_ticks();

    while (dlen) {
        unsigned n, k = 0;
        unsigned want = (unsigned)(dlen > (unsigned long)BUFSZ
                                   ? (unsigned long)BUFSZ : dlen);
        n = (unsigned)fread(buf, 1, want, f);
        if (!n) break;
        dlen -= n;
        while (k < n) {
            unsigned char sr = (unsigned char)inp(SRP);
            if (sr & SR_SER) ser++;
            if (sr & SR_PRDY) { outp(PDR, buf[k]); k++; played++; }
        }
        if (kbhit()) { (void)getch(); break; }
    }

    t1 = bios_ticks();
    i11 = ci_get(0x0B);
    ci_put(0x09, 0xC8);                          /* PEN off               */
    ci_put(0x10, 0x00);                          /* timer off             */
    ci_put(0x06, 0x80); ci_put(0x07, 0x80);      /* re-mute, as found     */
    fclose(f);

    printf("\n%lu bytes fed in %lu ticks (%lu.%lu s)\n", played,
           t1 - t0, (t1 - t0) / 18UL, ((t1 - t0) % 18UL) * 10UL / 18UL);
    printf("underrun polls (SER): %lu   final I11=%02X (PUR=%d)\n",
           ser, i11, (i11 >> 6) & 1);
    printf("note: the gaps are the disk reads - a %u byte buffer refills with\n"
           "      playback stopped.  A real driver double-buffers or uses the\n"
           "      timer ISR; this is the reference path, not the final pump.\n",
           BUFSZ);
    return 0;
}
