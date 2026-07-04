/* SCPROBE.C - Roland SCP-55 probe: bring the card up (clean-room, no Card
 * Services) and dump the WSS / Crystal CS4231 codec so we can pin the exact
 * chip revision and capabilities.  One-shot on purpose: the PC110 sleep clears
 * the card's volatile COR, so enable+configure+probe must not be split across
 * separate host round-trips.
 *
 * PC110: Intel 82365SL PCIC at index 0x3E0 / data 0x3E1, card in socket 0.
 * Codec sits at (I/O base + 4): IAR / IDR / status / PIO.
 *
 * Build (Open Watcom, 16-bit real mode):  C:\WATCOM\BLD SCPROBE
 */
#include <stdio.h>
#include <conio.h>
#include <i86.h>

#define PCIC 0x3E0
static unsigned pidx = PCIC, soff = 0;
static unsigned char rd(unsigned r){ outp(pidx, soff + r); return (unsigned char)inp(pidx + 1); }
static void          wr(unsigned r, unsigned v){ outp(pidx, soff + r); outp(pidx + 1, v); }
static void iod(unsigned long n){ while (n--) (void)inp(0x80); }
#define MS(x) iod((unsigned long)(x) * 1000UL)

/* ---- CIS walk (attribute bytes at 2x host spacing) ---- */
static unsigned g_manf = 0, g_card = 0, g_cfg = 0x400;
static char     g_vers[80];
static void read_cis(unsigned seg)
{
    unsigned char __far *p = (unsigned char __far *)MK_FP(seg, 0);
    unsigned off = 0; int g, vi = 0, m;
    g_manf = g_card = 0; g_vers[0] = 0; g_cfg = 0x400;
    for (g = 0; g < 64; g++) {
        unsigned char code = p[off], link;
        if (code == 0xFF) break;
        if (code == 0x00) { off += 2; continue; }
        link = p[off + 2];
        if (code == 0x20) {                       /* CISTPL_MANFID */
            g_manf = (unsigned)p[off + 4] | ((unsigned)p[off + 6] << 8);
            g_card = (unsigned)p[off + 8] | ((unsigned)p[off + 10] << 8);
        } else if (code == 0x1A) {                /* CISTPL_CONFIG: COR base */
            int rasz = (p[off + 4] & 0x03) + 1;
            g_cfg = p[off + 8];
            if (rasz >= 2) g_cfg |= (unsigned)p[off + 10] << 8;
        } else if (code == 0x15) {                /* CISTPL_VERS_1 */
            for (m = 2; m < link; m++) {
                unsigned char c = p[off + 4 + 2 * m];
                if (vi < 79) g_vers[vi++] = c ? (char)c : ' ';
            }
            g_vers[vi] = 0;
        }
        if (link == 0xFF) break;
        off += ((unsigned)link + 2) * 2;
        if (off >= 0x3000) break;
    }
    while (vi > 0 && g_vers[vi - 1] == ' ') g_vers[--vi] = 0;
}

/* ---- CS4231/AD1848 codec.  cb = codec base (I/O base + 4).
 * cb+0 = IAR (bit7 INIT, bit6 MCE, bits5:0 index), cb+1 = IDR, cb+2 = status. */
static int cwait(unsigned cb)
{
    unsigned long i;
    for (i = 0; i < 60000UL; i++) if (!(inp(cb) & 0x80)) return 1;
    return 0;
}
static unsigned char cread(unsigned cb, unsigned char idx)
{
    cwait(cb); outp(cb, idx); return (unsigned char)inp(cb + 1);
}
static void cwrite(unsigned cb, unsigned char idx, unsigned char val)
{
    cwait(cb); outp(cb, idx); outp(cb + 1, val);
}
/* write with MCE held (needed for mode-change regs like I12 MODE2, I8, I9) */
static void cwrite_mce(unsigned cb, unsigned char idx, unsigned char val)
{
    cwait(cb); outp(cb, (unsigned char)(0x40 | idx)); outp(cb + 1, val);
    cwait(cb); outp(cb, idx);                    /* drop MCE */
}

int main(void)
{
    unsigned memseg = 0xD000, iobase = 0x330, cb = 0x334;
    unsigned char __far *cor;
    unsigned char idrev, ifs, i12, i25, t1, t2;
    unsigned start, stop, woff;
    int i, init_ok;

    pidx = PCIC; soff = 0;
    idrev = rd(0x00);
    printf("PCIC IDREV=%02X  ", idrev);
    if ((idrev & 0xC0) != 0x80) { printf("not an 82365 - abort\n"); return 1; }
    ifs = rd(0x01);
    printf("IntfStat=%02X\n", ifs);
    if ((ifs & 0x0C) != 0x0C) { printf("no card in socket 0 - abort\n"); return 1; }

    wr(0x02, 0x95); MS(30);                       /* power 5V */
    if (!(rd(0x01) & 0x40)) { printf("no power-up - abort\n"); wr(0x02, 0); return 1; }
    wr(0x03, 0x40); MS(10);                        /* reset off, memory mode */

    start = memseg >> 8; stop = (memseg >> 8) + 3;
    woff = ((unsigned)(0 - (memseg >> 8)) & 0x3FFF) | 0x4000;
    wr(0x10, start & 0xFF); wr(0x11, (start >> 8) & 0x3F);
    wr(0x12, stop  & 0xFF); wr(0x13, (stop  >> 8) & 0x3F);
    wr(0x14, woff  & 0xFF); wr(0x15, (woff  >> 8) & 0xFF);
    wr(0x06, 0x01);                                /* enable mem window 0 */
    MS(5);

    read_cis(memseg);
    printf("CIS \"%s\"  MANFID %04X/%04X  cfgbase=%03X\n", g_vers, g_manf, g_card, g_cfg);
    if (g_manf != 0xC00C || g_card != 0x0001) {
        printf("not the Roland SCP-55 (want C00C/0001) - abort\n");
        wr(0x06, 0); wr(0x03, 0); wr(0x02, 0); return 1;
    }

    cor = (unsigned char __far *)MK_FP(memseg, g_cfg);
    *cor = 0x01;                                   /* COR = config index 1 */
    printf("COR@%03X <- 01, reads %02X\n", g_cfg, (unsigned char)*cor);

    wr(0x08, iobase & 0xFF);        wr(0x09, (iobase >> 8) & 0xFF);
    wr(0x0A, (iobase + 0x0F) & 0xFF); wr(0x0B, ((iobase + 0x0F) >> 8) & 0xFF);
    wr(0x07, 0x00);                                /* 8-bit */
    wr(0x03, 0x60);                                /* I/O-card mode (no IRQ for probe) */
    wr(0x06, 0x41);                                /* enable mem win0 + I/O win0 */
    MS(5);

    printf("io %03X-%03X:", iobase, iobase + 15);
    for (i = 0; i < 16; i++) printf(" %02X", (unsigned char)inp(iobase + i));
    printf("\n");

    init_ok = cwait(cb);
    printf("codec@%03X INIT %s  IAR=%02X\n", cb, init_ok ? "clear" : "STUCK", (unsigned char)inp(cb));

    /* scratch R/W test on I0 (left ADC input control) */
    cwrite(cb, 0x00, 0xAA); t1 = cread(cb, 0x00);
    cwrite(cb, 0x00, 0x55); t2 = cread(cb, 0x00);
    printf("I0 wrAA->%02X wr55->%02X\n", t1, t2);

    printf("mode1 I0-15 :");
    for (i = 0; i < 16; i++) printf(" %02X", cread(cb, (unsigned char)i));
    printf("\n");

    i12 = cread(cb, 0x0C);
    cwrite_mce(cb, 0x0C, (unsigned char)(i12 | 0x40));   /* MODE2 enable */
    printf("I12=%02X -> MODE2 -> I12=%02X\n", i12, cread(cb, 0x0C));

    printf("mode2 I16-31:");
    for (i = 16; i < 32; i++) printf(" %02X", cread(cb, (unsigned char)i));
    printf("\n");

    i25 = cread(cb, 0x19);
    printf("I25 version=%02X : ", i25);
    switch (i25 & 0xE0) {
        case 0x80: printf("CS4231\n");  break;
        case 0xA0: printf("CS4231A\n"); break;
        default:   printf("AD1848/other, or MODE2 not honored\n"); break;
    }

    printf("done - card left enabled, I/O base %03X, codec %03X\n", iobase, cb);
    return 0;
}
