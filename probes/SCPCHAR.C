/* SCPCHAR.C - characterise the SCP-55's digital path, so the VSBPCMCIA backend
 * can be designed from numbers instead of guesses.
 *
 * Register map (proven 2026-08-24 by SCPR2 - NOT the datasheet's default):
 *     R0 IAR = base+8   R1 IDR = base+9
 *     R2 STATUS = base+6   R3 PDR = base+7
 *
 * This card populates ONE crystal, 16.9344 MHz, on XI1 - so selecting C2SL=0
 * yields the datasheet's XTAL2 rate column.  Measured: I8=02 gives ~11,008
 * samples/sec, matching the 11.025 kHz entry.  We never set C2SL=1: there is
 * no second crystal and the resync hangs.
 *
 * For each format it arms the codec, feeds as fast as PRDY allows for one
 * second, and reports achieved rate vs expected, plus SER (underrun) and the
 * poll/sample ratio - which is the real headroom figure: close to 1.0 means the
 * CPU is saturated and a background driver has nothing left.
 *
 * Also checks PL/R (R2 D2) alternation, which is how a stereo feed knows which
 * channel the codec wants next.
 *
 * SAFETY: codec registers only.  No PCIC, no glue writes, never C2SL=1.
 * Every loop wall-clock bounded.  Disarms on exit.
 */
#include <stdio.h>
#include <conio.h>
#include <string.h>
#include <i86.h>

static unsigned BASE = 0x330;
#define IAR (BASE+8)
#define IDR (BASE+9)
#define SRP (BASE+6)
#define PDR (BASE+7)

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

/* this card's real rate table = datasheet XTAL2 column, chosen with C2SL=0 */
static unsigned rate_of[8] = { 5512, 11025, 18900, 22050, 37800, 44100, 33075, 6620 };

static void run(const char *tag, unsigned char i8, int bytes_per_frame)
{
    unsigned long polls = 0, wrote = 0, ser = 0, plr = 0;
    unsigned long t0, tend, ach, exp;
    unsigned char sr, i11;
    int v = 0, d = 12;

    ci_put_mce(0x08, i8);
    ci_put_mce(0x09, 0xC8);                 /* PPIO | CPIO | CAL0 */
    ci_put(0x06, 0x03); ci_put(0x07, 0x03);
    ci_put(0x09, 0xC9);                     /* + PEN */
    MS(10);
    (void)inp(SRP);

    t0 = bios_ticks(); tend = t0 + 18UL;    /* ~1 second */
    while (bios_ticks() < tend) {
        sr = (unsigned char)inp(SRP);
        if (sr & SR_SER) ser++;
        if (sr & SR_PRDY) {
            if (sr & SR_PLR) plr++;
            v += d;
            if (v >  110) { v =  110; d = -d; }
            if (v < -110) { v = -110; d = -d; }
            outp(PDR, (unsigned char)(128 + v));
            wrote++;
        }
        polls++;
    }
    i11 = ci_get(0x0B);
    ci_put(0x09, 0xC8);

    exp = rate_of[(i8 >> 1) & 7];
    ach = wrote / (unsigned long)bytes_per_frame;

    printf("%-12s I8=%02X  exp %5lu  got %5lu  %3lu%%  SER %6lu  PUR %d  poll/s %lu\n",
           tag, i8, exp, ach, exp ? (ach * 100UL / exp) : 0UL,
           ser, (i11 >> 6) & 1, wrote ? polls / wrote : 0UL);
    if (plr) printf("             PL/R high on %lu of %lu writes\n", plr, wrote);
}

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++)
        if (!strnicmp(argv[i], "/BASE=", 6)) sscanf(argv[i]+6, "%x", &BASE);

    printf("SCPCHAR - SCP-55 digital path characterisation\n");
    printf("R2=%03X R3=%03X (proven map)   1 s per format, feed = PRDY-driven\n\n",
           SRP, PDR);

    for (i = 0; i < 8; i++) {
        outp(IAR, (unsigned char)i); iod(200);
        if ((inp(IAR) & 0x1F) != i) { printf("codec not responding\n"); return 1; }
    }
    ci_put(0x0C, (unsigned char)(ci_get(0x0C) | 0x40));   /* MODE2 */
    ci_put(0x0A, 0x02);                                   /* IEN   */
    ci_put(0x15, 0x00); ci_put(0x14, 0x42);
    ci_put(0x10, 0xC0);                                   /* OLB|TE */

    printf("-- mono 8-bit, every rate (C2SL=0; never selects the absent XTAL2) --\n");
    for (i = 0; i < 8; i++) {
        char tag[16];
        sprintf(tag, "CFS=%d", i);
        run(tag, (unsigned char)(i << 1), 1);
    }

    printf("\n-- format variants at 11.025 kHz --\n");
    run("stereo 8b",  0x12, 2);           /* +0x10 = stereo            */
    run("mono 16b",   0x42, 2);           /* +0x40 = linear 16-bit LE  */
    run("stereo 16b", 0x52, 4);

    ci_put(0x10, 0x00);
    printf("\ndone. codec disarmed.\n");
    printf("poll/s near 1 = CPU saturated; a background driver needs headroom.\n");
    return 0;
}
