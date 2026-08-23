/* SCPPLAY.C - Roland SCP-55: DMA-less digital audio, built from the vendor
 * driver's LIVE register set captured mid-playback on the PC110 under
 * Windows 3.11 (SCPGLUE /SNAP, 2026-08-24, six identical rounds):
 *
 *   I9  = C9   PEN=1 PPIO=1 CPIO=1     the CS4231A's own PIO engine
 *   I11 = 00   PUR=0                   no underrun - the DAC really is fed
 *   I16 = C0   OLB=1 TE=1              internal timer running
 *   I20 = 42   I21 = 00                base 66 x ~10us = ~660us, auto-reload
 *   I8  = 02   XTAL1, 16 kHz, mono, 8-bit unsigned
 *   I10 = 02   IEN                     I0/I1=C0  I2/I3=0A  I6/I7=03  I26=C0
 *
 * We clone that set exactly rather than hand-rolling one, then run the
 * datasheet's actual PIO protocol in the inner loop:
 *
 *   R2 (base+2) D1 PRDY:  1 = "data stale, ready for next host write"
 *                         0 = "data still valid, do not overwrite"
 *
 * so: poll SR, write ONE sample each time PRDY reads 1.  The timer is enabled
 * to match the vendor but the feed itself is PRDY-driven.
 *
 * MEASUREMENT RULE, learned the hard way twice: never judge a flag with a
 * measurement path slower than the flag's own re-assert time.  A starved DAC
 * re-sets PUR in ~125us at 8 kHz, but an indexed I11 read costs ~200us of
 * settling - so underrun is read here from SR bit4 (SER = COR|PUR), which is
 * a single inp.  And SER is cleared BY READING IT, so a tight poll loop will
 * wipe it faster than it can set: we count how many polls SAW it, never
 * "is it set right now".
 *
 * SAFETY: codec registers only.  Never the PCIC, never the glue block at
 * 0x330-0x337, never XTAL2.  Wall-clock bounded.  Disarms on exit.
 *
 * Run in raw DOS with SCPENA (or SCP55GO) loaded - not under Windows, or the
 * vendor driver is driving the same chip.
 *
 * Build: ./build-dos.sh SCPPLAY
 */
#include <stdio.h>
#include <conio.h>
#include <string.h>
#include <i86.h>

static unsigned BASE = 0x330;
#define IAR (BASE+8)
#define IDR (BASE+9)
/* 2026-08-24: PROVEN by SCPR2 - this card splits the codec's four registers
 * across two pairs.  R2/R3 are at base+6/base+7, NOT base+2/base+3.  With the
 * FIFO starved, base+6 reads DF (SER=1 matching I11's PUR=1, PRDY=1) while
 * base+2 reads a flat 01.  Every PIO attempt since 2026-07-03 fed 0x33B, a
 * dead port. */
#define SRP (BASE+6)
#define PDR (BASE+7)
#define SRP_OLD (BASE+10)

#define SR_INT  0x01
#define SR_PRDY 0x02
#define SR_PLR  0x04
#define SR_SER  0x10

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

/* the vendor's exact mixer/feature set, index then value */
static unsigned char vend[][2] = {
    {0x00,0xC0},{0x01,0xC0},{0x02,0x0A},{0x03,0x0A},{0x04,0x88},{0x05,0x88},
    {0x06,0x03},{0x07,0x03},{0x0A,0x02},{0x12,0x08},{0x13,0x08},{0x1A,0xC0}
};

int main(int argc, char **argv)
{
    unsigned fmt = 0x02, tbase = 66, secs = 4;
    int blast = 0, i, v = 0, d = 12;
    unsigned char sr, srm = 0, i9, i11, i16, i24;
    unsigned long polls = 0, samples = 0, prdy = 0, ser = 0;
    unsigned long t0, tend;

    for (i = 1; i < argc; i++) {
        if      (!strnicmp(argv[i],"/BASE=",6)) sscanf(argv[i]+6,"%x",&BASE);
        else if (!strnicmp(argv[i],"/FMT=", 5)) sscanf(argv[i]+5,"%x",&fmt);
        else if (!strnicmp(argv[i],"/TB=",  4)) sscanf(argv[i]+4,"%u",&tbase);
        else if (!strnicmp(argv[i],"/SECS=",6)) sscanf(argv[i]+6,"%u",&secs);
        else if (!stricmp (argv[i],"/BLAST"))   blast = 1;
        else { printf("usage: SCPPLAY [/BASE=330] [/FMT=02] [/TB=66] [/SECS=4]"
                      " [/BLAST]\n"
                      "  default: PRDY-driven feed (the datasheet protocol)\n"
                      "  /BLAST : ignore PRDY, write on timer ticks instead\n");
               return 1; }
    }

    printf("SCPPLAY - vendor-cloned PIO on the SCP-55, base %03X\n", BASE);
    printf("I8=%02X (16kHz XTAL1 mono 8-bit)  timer base=%u  %u s  feed=%s\n\n",
           fmt, tbase, secs, blast ? "BLAST (timer)" : "PRDY-driven");

    for (i = 0; i < 8; i++) {
        outp(IAR, (unsigned char)i); iod(200);
        if ((inp(IAR) & 0x1F) != i) {
            printf("codec not responding at %03X - run SCPENA first\n", IAR);
            return 1;
        }
    }

    /* ---- MODE 2, then the vendor's register set ----------------------- */
    ci_put(0x0C, (unsigned char)(ci_get(0x0C) | 0x40));
    if (!(ci_get(0x0C) & 0x40)) { printf("MODE2 would not latch\n"); return 1; }

    ci_put_mce(0x08, (unsigned char)fmt);       /* rate + format           */
    ci_put_mce(0x09, 0xC8);                     /* PPIO|CPIO|CAL0, no PEN  */
    for (i = 0; i < (int)(sizeof vend / sizeof vend[0]); i++)
        ci_put(vend[i][0], vend[i][1]);

    ci_put(0x0F, 0x00);                         /* I15 then I14: vendor    */
    ci_put(0x0E, 0x00);                         /* had both at 00 in PIO   */

    ci_put(0x15, (unsigned char)(tbase >> 8));  /* I21 upper first ...     */
    ci_put(0x14, (unsigned char)(tbase & 0xFF));/* ... then I20 lower      */
    ci_put(0x10, 0xC0);                         /* I16 = OLB | TE          */

    ci_put(0x09, 0xC9);                         /* PEN on the fly          */

    i9 = ci_get(0x09); i16 = ci_get(0x10); i24 = ci_get(0x18); i11 = ci_get(0x0B);
    printf("armed: I9=%02X (PEN=%d PPIO=%d)  I16=%02X (TE=%d)  I24=%02X  I11=%02X\n",
           i9, i9 & 1, (i9 >> 6) & 1, i16, (i16 >> 6) & 1, i24, i11);

    /* ---- does PRDY ever assert?  measure it properly, before feeding --- */
    {   unsigned long n, hi = 0, seen = 0;
        unsigned char o = 0;
        for (n = 0; n < 200000UL; n++) {
            sr = (unsigned char)inp(SRP);
            o = (unsigned char)(o | sr);
            if (sr & SR_PRDY) hi++;
            if (sr & SR_SER)  seen++;
        }
        printf("pre-flight: %lu/200000 polls saw PRDY, %lu saw SER, SR bits=%02X\n",
               hi, seen, o);
        printf("            (starving + PEN=1 should show PRDY high nearly always)\n");
    }

    /* ---- feed ---------------------------------------------------------- */
    t0 = bios_ticks();
    tend = t0 + (unsigned long)secs * 18UL + 1UL;

    if (!blast) {
        while (bios_ticks() < tend) {
            sr = (unsigned char)inp(SRP);
            srm = (unsigned char)(srm | sr);
            if (sr & SR_SER)  ser++;
            if (sr & SR_PRDY) {
                prdy++;
                v += d;
                if (v >  110) { v =  110; d = -d; }
                if (v < -110) { v = -110; d = -d; }
                outp(PDR, (unsigned char)(128 + v));
                samples++;
            }
            polls++;
        }
    } else {
        ci_wait(); outp(IAR, 0x18); iod(200);   /* park on I24, poll TI    */
        outp(IDR, 0x00);
        while (bios_ticks() < tend) {
            if (inp(IDR) & 0x40) {
                outp(IDR, 0x00);
                prdy++;
                for (i = 0; i < 11; i++) {
                    v += d;
                    if (v >  110) { v =  110; d = -d; }
                    if (v < -110) { v = -110; d = -d; }
                    outp(PDR, (unsigned char)(128 + v));
                    samples++;
                }
            }
            polls++;
        }
    }

    i11 = ci_get(0x0B);
    ci_put(0x09, 0xC8);                         /* PEN off                 */
    ci_put(0x10, 0x00);                         /* timer off               */

    printf("\npolls=%lu  %s=%lu  samples=%lu  SER seen on %lu polls\n",
           polls, blast ? "ticks" : "PRDY-high", prdy, samples, ser);
    printf("SR bits seen=%02X   final I11=%02X\n", srm, i11);
    printf("feed rate = %lu samples/sec   (DAC drains %u)\n",
           samples / (unsigned long)(secs ? secs : 1), fmt == 0x02 ? 16000 : 8000);

    printf("\nVERDICT: ");
    if (!samples)
        printf("PRDY never asserted - the codec never asked for data.\n");
    else if (samples / (unsigned long)(secs ? secs : 1) > 12000UL)
        printf("*** feeding at rate, PRDY handshake alive - LISTEN. ***\n");
    else
        printf("PRDY asserted but the feed rate is short of the drain rate.\n");
    return 0;
}
