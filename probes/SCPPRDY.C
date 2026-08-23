/* SCPPRDY.C - the two tests that decide whether DMA-less digital audio is
 * reachable on the SCP-55.  Run in raw DOS with SCPENA (or SCP55GO) loaded.
 *
 * TEST A - ALIAS.  Does base+2/base+3 actually reach the codec's R2/R3, or do
 *   they alias base+0/base+1 because A1 never gets there?  Park a distinctive
 *   index in IAR and a distinctive value in a scratch indexed register, then
 *   read 0x33A/0x33B and see which one comes back.  I15 (Playback Lower Base)
 *   is the scratch: the datasheet says reads return exactly what was written.
 *   This has been described for hours and never actually run - phase A only
 *   ever swept the GLUE block at 330-337, never the codec's own upper pair.
 *
 * TEST B - PRIME.  R2's reset state has PRDY=0 ("data still valid, do not
 *   overwrite"), so a purely PRDY-gated feed can never issue a first write -
 *   which is exactly how the last build deadlocked at samples=0.  Write ONE
 *   sample unconditionally, then watch PRDY.  If it starts toggling, PRDY just
 *   needed bootstrapping and the feed works.  If it stays flat through a single
 *   prime AND a full 16-sample FIFO prime, PRDY is genuinely dead here.
 *
 * SAFETY: codec registers only - never the PCIC, never the glue block, never
 * XTAL2.  All loops bounded.  Disarms on exit.
 *
 * Build: ./build-dos.sh SCPPRDY
 */
#include <stdio.h>
#include <conio.h>
#include <string.h>

static unsigned BASE = 0x330;
#define IAR (BASE+8)
#define IDR (BASE+9)
#define SRP (BASE+10)
#define PDR (BASE+11)

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

/* ------------------------------------------------------------------ TEST A */
static void test_alias(void)
{
    static unsigned char idxv[3] = { 0x0F, 0x14, 0x15 };
    static unsigned char valv[3] = { 0xA5, 0x5A, 0x33 };
    int t, aliasIAR = 0, aliasIDR = 0;

    printf("\n== TEST A: does 33A/33B alias 338/339 (is A1 reaching the codec)? ==\n");
    printf("idx val | 338 339 33A 33B | verdict\n");

    for (t = 0; t < 3; t++) {
        unsigned char idx = idxv[t], val = valv[t];
        unsigned char r0, r1, r2, r3;

        ci_put(0x0F, val);                  /* I15 scratch = val            */
        ci_wait(); outp(IAR, idx); iod(400);/* park a distinctive index     */

        r0 = (unsigned char)inp(BASE + 8);
        r1 = (unsigned char)inp(BASE + 9);
        r2 = (unsigned char)inp(BASE + 10);
        r3 = (unsigned char)inp(BASE + 11);

        printf(" %02X  %02X | %02X  %02X  %02X  %02X  |", idx, val, r0, r1, r2, r3);
        if ((r2 & 0x1F) == idx) { printf(" 33A==IAR"); aliasIAR++; }
        if (idx == 0x0F && r3 == val) { printf(" 33B==IDR"); aliasIDR++; }
        printf("\n");
    }

    printf("VERDICT A: ");
    if (aliasIAR >= 2)
        printf("33A MIRRORS IAR -> A1 is NOT reaching the codec.  R2/R3 are\n"
               "           unreachable at these addresses; that is the whole bug.\n");
    else if (aliasIDR)
        printf("33B mirrors IDR -> A1 partially decoded.\n");
    else
        printf("no aliasing - 33A/33B are their own ports, A1 IS decoded.\n");
}

/* ------------------------------------------------------------------ TEST B */
static unsigned long watch_prdy(unsigned long n, unsigned char *seen)
{
    unsigned long i, hi = 0;
    unsigned char o = 0, sr;
    for (i = 0; i < n; i++) {
        sr = (unsigned char)inp(SRP);
        o = (unsigned char)(o | sr);
        if (sr & SR_PRDY) hi++;
    }
    *seen = o;
    return hi;
}

static void test_prime(void)
{
    unsigned char o, i9, i11;
    unsigned long hi;
    int i;

    printf("\n== TEST B: does PRDY need priming? ==\n");

    ci_put(0x0C, (unsigned char)(ci_get(0x0C) | 0x40));   /* MODE2          */
    ci_put_mce(0x08, 0x02);                               /* 16k XTAL1 mono */
    ci_put_mce(0x09, 0xC8);                               /* PPIO|CPIO|CAL0 */
    ci_put(0x06, 0x03); ci_put(0x07, 0x03);               /* DAC unmuted    */
    ci_put(0x0A, 0x02);                                   /* IEN            */
    ci_put(0x15, 0x00); ci_put(0x14, 0x42);               /* timer base 66  */
    ci_put(0x10, 0xC0);                                   /* OLB | TE       */
    ci_put(0x09, 0xC9);                                   /* + PEN          */
    MS(20);

    i9 = ci_get(0x09); i11 = ci_get(0x0B);
    printf("armed: I9=%02X (PEN=%d PPIO=%d)  I11=%02X (PUR=%d)\n",
           i9, i9 & 1, (i9 >> 6) & 1, i11, (i11 >> 6) & 1);

    hi = watch_prdy(50000UL, &o);
    printf("  before any write : PRDY high on %lu/50000 polls, SR bits=%02X\n", hi, o);

    outp(PDR, 0x80);                                      /* ONE prime write */
    hi = watch_prdy(50000UL, &o);
    printf("  after 1 prime    : PRDY high on %lu/50000 polls, SR bits=%02X\n", hi, o);

    for (i = 0; i < 16; i++) outp(PDR, (unsigned char)(0x80 + (i & 7) * 8));
    hi = watch_prdy(50000UL, &o);
    printf("  after 16 primes  : PRDY high on %lu/50000 polls, SR bits=%02X\n", hi, o);

    /* if PRDY ever moved, try a real primed feed and report the rate */
    if (hi) {
        unsigned long samples = 0, polls = 0;
        int v = 0, d = 12;
        printf("\n  PRDY is alive - running a 2 s primed feed...\n");
        for (polls = 0; polls < 3000000UL; polls++) {
            if ((unsigned char)inp(SRP) & SR_PRDY) {
                v += d;
                if (v >  110) { v =  110; d = -d; }
                if (v < -110) { v = -110; d = -d; }
                outp(PDR, (unsigned char)(128 + v));
                samples++;
            }
        }
        printf("  fed %lu samples\n", samples);
    }

    i11 = ci_get(0x0B);
    ci_put(0x09, 0xC8); ci_put(0x10, 0x00);               /* disarm, timer off */

    printf("VERDICT B: ");
    if (hi)
        printf("PRDY ASSERTS once primed - the handshake works and the feed\n"
               "           loop simply had to write first.  LISTEN.\n");
    else
        printf("PRDY stays flat through a single prime AND a full 16-sample\n"
               "           FIFO prime.  It is not a bootstrap problem - PRDY is\n"
               "           genuinely dead at %03X (final I11=%02X).\n", SRP, i11);
}

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (!strnicmp(argv[i], "/BASE=", 6)) sscanf(argv[i]+6, "%x", &BASE);
        else { printf("usage: SCPPRDY [/BASE=330]\n"); return 1; }
    }

    printf("SCPPRDY - alias + prime tests, base %03X, codec %03X\n", BASE, IAR);
    for (i = 0; i < 8; i++) {
        outp(IAR, (unsigned char)i); iod(200);
        if ((inp(IAR) & 0x1F) != i) {
            printf("codec not responding at %03X - run SCPENA first\n", IAR);
            return 1;
        }
    }

    test_alias();
    test_prime();
    printf("\ndone. codec disarmed.\n");
    return 0;
}
