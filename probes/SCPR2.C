/* SCPR2.C - is the codec's R2 at 0x336 (and therefore R3 at 0x337)?
 *
 * The live vendor capture showed 0x336 walking CC -> CD -> CE, which against
 * R2's layout (D7 CU/L, D6 CL/R, D5 CRDY, D4 SER, D3 PU/L, D2 PL/R, D1 PRDY,
 * D0 INT) reads as: idle -> +INT -> +PRDY.  Exactly R2's behaviour.
 *
 * Every previous glue sweep armed the codec in DMA mode (PPIO=0), where PRDY
 * is meaningless.  This one arms PIO mode (PPIO=1, PEN=1) and leaves the FIFO
 * STARVED, which is the discriminating case: a real R2 must then show
 * PRDY=1 ("data stale, ready for next host write") and SER=1 (= COR|PUR, and
 * I11 says PUR=1).  0x33A shows neither, which is why it is not R2.
 *
 * NOTE on SER: it clears on read, so a tight loop wipes it faster than it can
 * set.  We count how many polls SAW it rather than asking "is it set now".
 *
 * READ-ONLY with respect to the glue block.  Codec registers only, no PCIC,
 * no XTAL2, bounded loops, disarms on exit.
 */
#include <stdio.h>
#include <conio.h>
#include <string.h>

static unsigned BASE = 0x330;
#define IAR (BASE+8)
#define IDR (BASE+9)

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

static void bits(unsigned char v)
{
    static const char *n[8] = {"INT","PRDY","PL/R","PU/L","SER","CRDY","CL/R","CU/L"};
    int b; int first = 1;
    for (b = 7; b >= 0; b--)
        if (v & (1 << b)) { printf("%s%s", first ? "" : "+", n[b]); first = 0; }
    if (first) printf("-none-");
}

int main(int argc, char **argv)
{
    int i, p;
    unsigned char i9, i11;
    unsigned long seen[8], n;

    for (i = 1; i < argc; i++)
        if (!strnicmp(argv[i], "/BASE=", 6)) sscanf(argv[i]+6, "%x", &BASE);

    printf("SCPR2 - is R2 at %03X?  (codec armed in PIO mode, FIFO starved)\n",
           BASE + 6);

    for (i = 0; i < 8; i++) {
        outp(IAR, (unsigned char)i); iod(200);
        if ((inp(IAR) & 0x1F) != i) { printf("codec not responding\n"); return 1; }
    }

    /* arm PIO playback and deliberately feed it nothing */
    ci_put(0x0C, (unsigned char)(ci_get(0x0C) | 0x40));   /* MODE2 */
    ci_put_mce(0x08, 0x02);                               /* 16k XTAL1 mono */
    ci_put_mce(0x09, 0xC8);                               /* PPIO | CPIO    */
    ci_put(0x06, 0x03); ci_put(0x07, 0x03);
    ci_put(0x0A, 0x02);
    ci_put(0x15, 0x00); ci_put(0x14, 0x42);
    ci_put(0x10, 0xC0);                                   /* OLB | TE       */
    ci_put(0x09, 0xC9);                                   /* + PEN          */
    MS(20);

    i9 = ci_get(0x09); i11 = ci_get(0x0B);
    printf("armed: I9=%02X (PEN=%d PPIO=%d)   I11=%02X (PUR=%d)\n\n",
           i9, i9 & 1, (i9 >> 6) & 1, i11, (i11 >> 6) & 1);
    printf("PUR=1 means the FIFO is empty, so a real R2 must read PRDY=1.\n\n");

    /* raw look at every candidate, plus 33A for comparison */
    for (p = 2; p <= 7; p++) {
        printf("%03X x16 :", BASE + p);
        for (i = 0; i < 16; i++) printf(" %02X", (unsigned char)inp(BASE + p));
        printf("\n");
    }
    printf("%03X x16 :", BASE + 10);
    for (i = 0; i < 16; i++) printf(" %02X", (unsigned char)inp(BASE + 10));
    printf("   <- the port we assumed was R2\n\n");

    /* how often does each candidate show PRDY / SER over many polls? */
    for (p = 2; p <= 7; p++) seen[p] = 0;
    for (n = 0; n < 20000UL; n++)
        for (p = 2; p <= 7; p++)
            if (inp(BASE + p) & 0x02) seen[p]++;      /* D1 = PRDY */

    printf("PRDY (D1) high, out of 20000 polls:\n");
    for (p = 2; p <= 7; p++)
        printf("  %03X : %lu%s\n", BASE + p, seen[p],
               seen[p] > 10000UL ? "   <<< behaves like a starved R2" : "");

    printf("\ndecode of %03X : ", BASE + 6);
    bits((unsigned char)inp(BASE + 6));
    printf("\ndecode of %03X : ", BASE + 10);
    bits((unsigned char)inp(BASE + 10));
    printf("\n");

    ci_put(0x09, 0xC8); ci_put(0x10, 0x00);
    printf("\ndone. codec disarmed.\n");
    return 0;
}
