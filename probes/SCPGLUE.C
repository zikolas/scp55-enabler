/* SCPGLUE.C - Roland SCP-55 glue-block ladder.
 *
 * Answers, in safety order, the three things that have never been tested on
 * this card:
 *   A  does 0x334-0x337 MIRROR the codec?  (if so the whole unknown glue is
 *      just 0x332/0x333 and the search space collapses)
 *   B  is base+3 really the codec's PIO port?  (capture-PIO delivers data =>
 *      the decode is fine and the fault is playback-specific; constant junk
 *      => the glue steals base+3, and CSPIO2 was writing to the right port
 *      in the wrong codec mode all along)
 *   C  with the codec armed in DMA mode (PPIO=0 - every previous test held
 *      PPIO=1, which disables the PDRQ/PDAK path a local DMA engine would
 *      use), does any bit in the glue block TOGGLE?  A flickering bit is the
 *      glue asking for data.
 *
 * SAFETY
 *   - Never touches the PCIC.  It only does IN/OUT inside the card's already
 *     enabled window, so it cannot misprogram a window or reach COM1.
 *   - Default run writes NOTHING to the glue block (0x330-0x337).  Phases 0/A/B/C
 *     are glue-read-only; the only writes are to the codec's own IAR/IDR, which
 *     are documented and reversible.
 *   - Never selects XTAL2 (I8 D0=1) - that is the known wedge; XTAL1 only.
 *   - /W adds phase D: exactly ONE byte per glue port, with before/after state.
 *   - /FEED=xxx adds phase E: a bounded burst into ONE named port.
 *   - Every loop is bounded.  Nothing here spins waiting on the card.
 *   Recovery from any wedge: re-run SCP55GO, or eject+reinsert the card.
 *
 * Run SCP55GO first.  Build: see build-dos.sh (Open Watcom, 16-bit real mode).
 */
#include <stdio.h>
#include <conio.h>
#include <string.h>

static unsigned BASE = 0x330;
static int g_mirror[8];      /* set by phase A: port aliases the codec */
#define IAR (BASE+8)
#define IDR (BASE+9)
#define SRP (BASE+10)
#define PDR (BASE+11)

static void iod(unsigned long n){ while (n--) (void)inp(0x80); }
#define MS(x) iod((unsigned long)(x) * 1000UL)

/* --- proven codec helpers, lifted verbatim from CSPIO2.C ----------------- */
static void ci_wait(void)
{ unsigned long i; for(i=0;i<400000UL;i++) if(!(inp(IAR)&0x80)) return; }
static void ci_put(unsigned char idx, unsigned char v)
{ ci_wait(); outp(IAR, idx); iod(200); outp(IDR, v); iod(200); }
static unsigned char ci_get(unsigned char idx)
{ ci_wait(); outp(IAR, idx); iod(200); return (unsigned char)inp(IDR); }
static void ci_put_mce(unsigned char idx, unsigned char v)
{ ci_wait(); outp(IAR,(unsigned char)(0x40|idx)); iod(200); outp(IDR,v); iod(200);
  ci_wait(); outp(IAR, idx); iod(200); MS(2); }

static void glue_read(unsigned char *out)   /* 0x330..0x337 -> out[0..7] */
{ int i; for (i = 0; i < 8; i++) out[i] = (unsigned char)inp(BASE + i); }

static void show_glue(const char *tag, unsigned char *g)
{ int i; printf("%s", tag);
  for (i = 0; i < 8; i++) printf(" %02X", g[i]); printf("\n"); }

/* -------------------------------------------------------------------------
 * phase 0 - presence
 * ---------------------------------------------------------------------- */
static int phase0(void)
{
    unsigned char g[8]; int i, ok = 1;

    printf("\n== 0. presence ==\n");
    for (i = 0; i < 8; i++) {
        outp(IAR, (unsigned char)i); iod(200);
        if ((inp(IAR) & 0x1F) != i) ok = 0;
    }
    printf("IAR readback sweep : %s\n", ok ? "CLEAN" : "*** FAILED - run SCP55GO first ***");
    if (!ok) return 0;

    printf("codec I0-I15       :");
    for (i = 0; i < 16; i++) printf(" %02X", ci_get((unsigned char)i));
    printf("\n");
    for (i = 0; i < 3; i++) { glue_read(g); show_glue("glue 330-337       :", g); MS(5); }
    return 1;
}

/* -------------------------------------------------------------------------
 * phase A - does 0x334-0x337 mirror the codec?   (no glue writes)
 * ---------------------------------------------------------------------- */
static void phaseA(void)
{
    static unsigned char probe[4] = { 0x01, 0x06, 0x0B, 0x14 };
    int hits[8], t, i;
    unsigned char g[8];

    printf("\n== A. address map: does 334-337 alias the codec? ==\n");
    for (i = 0; i < 8; i++) { hits[i] = 0; g_mirror[i] = 0; }

    for (t = 0; t < 4; t++) {
        outp(IAR, probe[t]); iod(400);          /* set codec index, no side effects */
        glue_read(g);
        printf("IAR<-%02X  glue:", probe[t]);
        for (i = 0; i < 8; i++) printf(" %02X", g[i]);
        printf("\n");
        for (i = 0; i < 8; i++) if (g[i] == probe[t]) hits[i]++;
    }
    for (i = 0; i < 8; i++) if (hits[i] == 4) g_mirror[i] = 1;
    printf("mirrors IAR (4/4)  :");
    for (i = 0; i < 8; i++) if (g_mirror[i]) printf(" %03X", BASE + i);
    printf("\n");
    if (g_mirror[4]) { g_mirror[5] = g_mirror[6] = g_mirror[7] = 1; }
    printf("VERDICT: ");
    if (g_mirror[4])
        printf("334 MIRRORS the codec -> unknown glue is only 332/333;\n"
               "         334-337 are now WRITE-PROTECTED in phases D/E.\n");
    else
        printf("no port 330-337 tracks IAR -> 332-337 are their own registers.\n");
}

/* -------------------------------------------------------------------------
 * phase B - is base+3 the codec's PIO port?  capture-PIO test
 * ---------------------------------------------------------------------- */
static void phaseB(void)
{
    unsigned char v, lo = 0xFF, hi = 0x00, first, srseen = 0;
    int i, distinct = 0;
    unsigned char seen[256];

    printf("\n== B. is %03X the codec's own PIO port? (capture side) ==\n", PDR);
    memset(seen, 0, sizeof seen);

    ci_put_mce(0x08, 0x00);                 /* XTAL1, 8kHz, 8-bit unsigned mono */
    ci_put_mce(0x09, 0x82);                 /* CPIO | CEN  - capture via PIO     */
    MS(20);

    first = (unsigned char)inp(PDR);
    for (i = 0; i < 128; i++) {
        srseen = (unsigned char)(srseen | inp(SRP));
        v = (unsigned char)inp(PDR);
        if (!seen[v]) { seen[v] = 1; distinct++; }
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        iod(50);
    }
    ci_put_mce(0x09, 0x00);                 /* capture off */

    printf("PDR first=%02X lo=%02X hi=%02X distinct=%d   SR bits seen=%02X\n",
           first, lo, hi, distinct, srseen);
    printf("VERDICT: ");
    if (distinct >= 4)
        printf("VARYING data -> %03X IS the codec's PIO port; decode is fine,\n"
               "         the fault is playback-specific.\n", PDR);
    else
        printf("CONSTANT (%d value) -> the codec is not delivering PIO here;\n"
               "         suspect the glue owns %03X.\n", distinct, PDR);
}

/* -------------------------------------------------------------------------
 * phase C - arm playback in DMA mode, watch the glue for a live bit
 * ---------------------------------------------------------------------- */
static void watch(int arm, int mpu)
{
    unsigned char orm[8], andm[8], g[8], i11 = 0, srm = 0;
    int i, n, live = 0;
    int lo = mpu ? 0 : 2;

    printf("\n== C. codec %s, sweep %03X-%03X ==\n",
           arm ? "ARMED (PPIO=0)" : "IDLE (control)", BASE + lo, BASE + 7);

    if (arm) {
        ci_put_mce(0x08, 0x00);             /* XTAL1 8kHz 8-bit mono */
        ci_put_mce(0x09, 0x01);             /* PEN only: PPIO=0 -> PDRQ/PDAK path */
        ci_put(0x0F, 0xFF);                 /* I15 lower count first ... */
        ci_put(0x0E, 0x0F);                 /* ... then I14 upper: loads the count */
        outp(SRP, 0);                       /* clear INT (write clears) */
        MS(10);
    }
    printf("state: I9=%02X I11=%02X SR=%02X\n",
           ci_get(0x09), ci_get(0x0B), (unsigned char)inp(SRP));

    for (i = 0; i < 8; i++) { orm[i] = 0x00; andm[i] = 0xFF; }
    /* 332-337 only: reading 330 pops the MPU receive FIFO, and 331 is its
       status - both would look "live" for reasons that are not the glue. */
    for (n = 0; n < 3000; n++) {
        for (i = lo; i < 8; i++) g[i] = (unsigned char)inp(BASE + i);
        for (i = lo; i < 8; i++) { orm[i] = (unsigned char)(orm[i] | g[i]);
                                  andm[i] = (unsigned char)(andm[i] & g[i]); }
        if ((n & 255) == 0) { srm = (unsigned char)(srm | inp(SRP)); }
    }
    i11 = ci_get(0x0B);

    printf("port  OR  AND  CHANGING\n");
    for (i = lo; i < 8; i++) {
        unsigned char chg = (unsigned char)(orm[i] & ~andm[i]);
        printf(" %03X  %02X   %02X    %02X %s\n", BASE + i, orm[i], andm[i], chg,
               chg ? "  <<< LIVE BIT" : "");
        if (chg && i >= 2) live = 1;
    }
    printf("after 3000 samples: I11(PUR)=%02X  SR bits seen=%02X\n", i11, srm);
    for (i = 6; i < 8; i++) {
        printf("raw %03X x24 :", BASE + i);
        for (n = 0; n < 24; n++) printf(" %02X", (unsigned char)inp(BASE + i));
        printf("\n");
    }
    if (arm) ci_put_mce(0x09, 0x00);         /* disarm */
    printf("VERDICT: ");
    if (live) printf("a glue bit TOGGLES while the codec starves - handshake found.\n");
    else      printf("glue block is STATIC while the codec starves.\n");
}

/* -------------------------------------------------------------------------
 * phase D - one byte per glue port, with before/after state   (/W only)
 * ---------------------------------------------------------------------- */
static void phaseD(void)
{
    unsigned char b[8], a[8], i11b, i11a, srb, sra;
    int p;

    printf("\n== D. single-byte write screen (0x80 = PCM silence) ==\n");
    for (p = 2; p <= 7; p++) {
        if (g_mirror[p]) { printf("%03X       : SKIPPED (codec alias)\n", BASE + p); continue; }
        glue_read(b); i11b = ci_get(0x0B); srb = (unsigned char)inp(SRP);
        outp(BASE + p, 0x80);
        iod(400);
        glue_read(a); i11a = ci_get(0x0B); sra = (unsigned char)inp(SRP);

        printf("%03X <- 80 :", BASE + p);
        {   int i, delta = 0;
            for (i = 0; i < 8; i++) if (a[i] != b[i]) {
                printf(" %03X %02X->%02X", BASE + i, b[i], a[i]); delta = 1; }
            if (i11a != i11b) { printf(" I11 %02X->%02X", i11b, i11a); delta = 1; }
            if (sra  != srb ) { printf(" SR %02X->%02X",  srb,  sra ); delta = 1; }
            if (!delta) printf(" no change");
        }
        printf("\n");
    }
}

/* -------------------------------------------------------------------------
 * phase E - bounded burst into one named port   (/W /FEED=xxx)
 * ---------------------------------------------------------------------- */
static void phaseE(unsigned port)
{
    unsigned char srm = 0, i11, lvl = 0x40;
    int n, purclear = 0;

    printf("\n== E. bounded feed into %03X (4000 bytes, codec armed) ==\n", port);
    if (port >= BASE && port < BASE + 8 && g_mirror[port - BASE]) {
        printf("REFUSED: %03X is a codec alias - writing PCM there would land in\n"
               "         the codec's indexed registers.\n", port);
        return;
    }
    if (port == BASE || port == BASE + 1) {
        printf("REFUSED: %03X is the MPU-401 pair.\n", port);
        return;
    }

    ci_put_mce(0x08, 0x00);
    ci_put_mce(0x09, 0x01);                 /* PEN, PPIO=0 */
    ci_put(0x0F, 0xFF);
    ci_put(0x0E, 0x0F);
    outp(SRP, 0);
    MS(10);

    for (n = 0; n < 4000; n++) {
        outp(port, lvl);
        if ((n & 7) == 0) lvl = (unsigned char)(lvl ^ 0x80);   /* ~500Hz square */
        if ((n & 63) == 0) {
            srm = (unsigned char)(srm | inp(SRP));
            if (!(ci_get(0x0B) & 0x40)) purclear = 1;
        }
    }
    i11 = ci_get(0x0B);
    ci_put_mce(0x09, 0x00);                 /* disarm */

    printf("SR bits seen=%02X  final I11=%02X  PUR ever clear: %s\n",
           srm, i11, purclear ? "YES" : "no");
    printf("VERDICT: ");
    if (purclear) printf("*** PUR CLEARED - bytes reached the codec through %03X ***\n", port);
    else          printf("PUR stayed set - %03X did not feed the codec.\n", port);
}


/* -------------------------------------------------------------------------
 * /SNAP - read-only snapshot, safe to run WHILE the vendor driver is playing.
 *
 * This is the whole point of a Windows 3.1 box: with no VxD owning the window,
 * a DOS-box read reaches real hardware, so we can catch the vendor engine in
 * its RUNNING state.  Answers in one shot:
 *   - is the codec in PIO or DMA mode during real playback?   (I9 bit6 PPIO)
 *   - is it even armed?                                        (I9 bit0 PEN)
 *   - what do the glue registers read while the engine runs?   (vs CC/80/AA idle)
 *   - is it underrunning?                                      (I11 PUR)
 * Politeness: saves and restores IAR so the driver's indexed access survives,
 * and never reads 330 (that would pop the MPU receive FIFO out from under it).
 * ---------------------------------------------------------------------- */
static void snap(int rounds)
{
    unsigned char save, i, r;
    FILE *f = fopen("C:\\SCPSNAP.TXT", "w");

    printf("\n== SNAP: read-only, %d rounds ==\n", rounds);
    printf("run this WHILE a wave file is playing.\n\n");

    for (r = 0; r < rounds; r++) {
        save = (unsigned char)inp(IAR);          /* be polite: remember the */
                                                 /* driver's current index  */
        printf("glue 331-337:");
        if (f) fprintf(f, "glue 331-337:");
        for (i = 1; i < 8; i++) {
            unsigned char v = (unsigned char)inp(BASE + i);
            printf(" %02X", v); if (f) fprintf(f, " %02X", v);
        }
        printf("   SR=%02X\n", (unsigned char)inp(SRP));
        if (f) fprintf(f, "   SR=%02X\n", (unsigned char)inp(SRP));

        printf("codec I0-15 :");
        if (f) fprintf(f, "codec I0-15 :");
        for (i = 0; i < 16; i++) {
            unsigned char v = ci_get(i);
            printf(" %02X", v); if (f) fprintf(f, " %02X", v);
        }
        printf("\n"); if (f) fprintf(f, "\n");

        /* Burst-read the status port.  With playback genuinely running, a real
         * R2 must show life: PRDY (D1) toggling at the sample rate, PL/R (D2)
         * alternating, CL/R (D6) set.  A flat value here while PEN=1 says 0x33A
         * is not the codec's Status register. */
        printf("raw %03X x24 :", SRP);
        if (f) fprintf(f, "raw %03X x24 :", SRP);
        for (i = 0; i < 24; i++) {
            unsigned char v = (unsigned char)inp(SRP);
            printf(" %02X", v); if (f) fprintf(f, " %02X", v);
        }
        printf("\n"); if (f) fprintf(f, "\n");

        {   unsigned char i12 = ci_get(0x0C);
            if (i12 & 0x40) {                    /* MODE2 latched: I16-I31 exist */
                printf("codec I16-31:");
                if (f) fprintf(f, "codec I16-31:");
                for (i = 16; i < 32; i++) {
                    unsigned char v = ci_get(i);
                    printf(" %02X", v); if (f) fprintf(f, " %02X", v);
                }
                printf("\n"); if (f) fprintf(f, "\n");
            }
        }
        outp(IAR, save);                         /* ... and put it back */
        {   unsigned char i9 = ci_get(0x09), i11 = ci_get(0x0B);
            outp(IAR, save);
            printf("  -> I9=%02X  PEN=%d PPIO=%d CEN=%d CPIO=%d | I11=%02X PUR=%d\n\n",
                   i9, i9 & 1, (i9 >> 6) & 1, (i9 >> 1) & 1, (i9 >> 7) & 1,
                   i11, (i11 >> 6) & 1);
            if (f) fprintf(f, "  -> I9=%02X PEN=%d PPIO=%d CEN=%d CPIO=%d | "
                              "I11=%02X PUR=%d\n\n",
                           i9, i9 & 1, (i9 >> 6) & 1, (i9 >> 1) & 1,
                           (i9 >> 7) & 1, i11, (i11 >> 6) & 1);
        }
        MS(250);
    }
    if (f) fclose(f);
    printf("wrote C:\\SCPSNAP.TXT\n");
    printf("VERDICT KEY: PPIO=1 -> the driver uses the codec's own PIO engine\n"
           "             PPIO=0 + PEN=1 -> the glue feeds the codec by local DMA\n"
           "             PEN=0 -> the codec is not the playback path at all\n");
}

int main(int argc, char **argv)
{
    int dowrite = 0, dosnap = 0; unsigned feed = 0; int i;

    for (i = 1; i < argc; i++) {
        if (!stricmp(argv[i], "/W")) dowrite = 1;
        else if (!stricmp(argv[i], "/SNAP")) dosnap = 1;
        else if (!strnicmp(argv[i], "/FEED=", 6)) sscanf(argv[i] + 6, "%x", &feed);
        else if (!strnicmp(argv[i], "/BASE=", 6)) sscanf(argv[i] + 6, "%x", &BASE);
        else { printf("usage: SCPGLUE [/BASE=330] [/SNAP] [/W] [/FEED=xxx]\n"); return 1; }
    }

    printf("SCPGLUE - SCP-55 glue ladder, base %03X, codec %03X\n", BASE, IAR);
    printf("mode: %s\n", dowrite ? "READ + WRITE (adds phase D)"
                                 : "READ-ONLY (no writes to 330-337)");
    if (dowrite && feed) printf("      + feed phase E on %03X\n", feed);
    printf("never touches the PCIC; never selects XTAL2.\n");

    if (dosnap) { snap(6); printf("\ndone (snapshot only, nothing written).\n"); return 0; }

    if (!phase0()) return 1;
    phaseA();
    watch(0, 0);       /* control: idle codec */
    phaseB();
    watch(1, 0);       /* armed, 332-337 sweep */
    watch(1, 1);       /* armed, 330-337 sweep - does reading the MPU
                          pair animate 337, as it seemed to in run 1? */
    if (dowrite) phaseD();
    if (dowrite && feed) phaseE(feed);

    printf("\ndone. codec left disarmed.\n");
    return 0;
}
