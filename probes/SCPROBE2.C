/* SCPROBE2.C - Roland SCP-55 codec identification, take 2.
 * Adds I/O settling delays (the CIS declares MWAIT) and diagnoses the codec
 * index/data path: IAR read-back sweep, timed register R/W, and a MODE2 /
 * I25-version read done with MCE + delays.  Assumes the card is a Roland
 * SCP-55; brings it up the same way SCPROBE does.
 *
 * Build: C:\WATCOM\BLD SCPROBE2
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
        if (code == 0x20) { g_manf = (unsigned)p[off+4] | ((unsigned)p[off+6]<<8);
                            g_card = (unsigned)p[off+8] | ((unsigned)p[off+10]<<8); }
        else if (code == 0x1A) { int rasz=(p[off+4]&0x03)+1; g_cfg=p[off+8];
                                 if(rasz>=2) g_cfg|=(unsigned)p[off+10]<<8; }
        else if (code == 0x15) { for(m=2;m<link;m++){ unsigned char c=p[off+4+2*m];
                                 if(vi<79) g_vers[vi++]=c?(char)c:' '; } g_vers[vi]=0; }
        if (link == 0xFF) break;
        off += ((unsigned)link + 2) * 2;
        if (off >= 0x3000) break;
    }
    while (vi > 0 && g_vers[vi-1]==' ') g_vers[--vi]=0;
}

/* codec at cb.  settle after every IAR/IDR touch; poll INIT AND spin a fixed
 * delay because this card's IAR read-back does not reflect INIT reliably. */
static unsigned CB;
static void csettle(void){ iod(200); }
static unsigned char ciar(void){ return (unsigned char)inp(CB); }
static unsigned char cread(unsigned char idx)
{
    int i; for(i=0;i<4000;i++) if(!(inp(CB)&0x80)) break;
    outp(CB, idx); csettle();
    return (unsigned char)inp(CB+1);
}
static void cwrite(unsigned char idx, unsigned char val)
{
    int i; for(i=0;i<4000;i++) if(!(inp(CB)&0x80)) break;
    outp(CB, idx); csettle();
    outp(CB+1, val); csettle();
    MS(2);                                   /* let any auto-cal settle */
}
static void cwrite_mce(unsigned char idx, unsigned char val)
{
    int i; for(i=0;i<4000;i++) if(!(inp(CB)&0x80)) break;
    outp(CB, (unsigned char)(0x40 | idx)); csettle();   /* MCE set */
    outp(CB+1, val); csettle();
    outp(CB, idx); csettle();                            /* MCE clear */
    MS(2);
}

int main(void)
{
    unsigned memseg = 0xD000, iobase = 0x330;
    unsigned char __far *cor;
    unsigned char i12a, i12b, i25, a, b;
    unsigned start, stop, woff;
    int i;

    pidx = PCIC; soff = 0;
    if ((rd(0x00) & 0xC0) != 0x80) { printf("no 82365 - abort\n"); return 1; }
    if ((rd(0x01) & 0x0C) != 0x0C) { printf("no card - abort\n"); return 1; }
    wr(0x02, 0x95); MS(30);
    if (!(rd(0x01) & 0x40)) { printf("no power - abort\n"); wr(0x02,0); return 1; }
    wr(0x03, 0x40); MS(10);
    start = memseg>>8; stop = (memseg>>8)+3; woff = ((unsigned)(0-(memseg>>8))&0x3FFF)|0x4000;
    wr(0x10,start&0xFF); wr(0x11,(start>>8)&0x3F);
    wr(0x12,stop&0xFF);  wr(0x13,(stop>>8)&0x3F);
    wr(0x14,woff&0xFF);  wr(0x15,(woff>>8)&0xFF);
    wr(0x06,0x01); MS(5);
    read_cis(memseg);
    printf("CIS \"%s\" %04X/%04X cfg=%03X\n", g_vers, g_manf, g_card, g_cfg);
    if (g_manf!=0xC00C || g_card!=0x0001) { printf("not SCP-55 - abort\n");
        wr(0x06,0);wr(0x03,0);wr(0x02,0); return 1; }
    cor = (unsigned char __far *)MK_FP(memseg,g_cfg); *cor = 0x01;
    wr(0x08,iobase&0xFF); wr(0x09,(iobase>>8)&0xFF);
    wr(0x0A,(iobase+0x0F)&0xFF); wr(0x0B,((iobase+0x0F)>>8)&0xFF);
    wr(0x07,0x00); wr(0x03,0x60); wr(0x06,0x41); MS(10);

    /* --- test index/data at both candidate codec bases: +4 and +8 --- */
    for (CB = iobase+4; CB <= iobase+8; CB += 4) {
        printf("\n[codec base %03X]\n", CB);
        printf(" IAR sweep (write idx -> read IAR):");
        for (i = 0; i < 8; i++) { outp(CB, (unsigned char)i); csettle();
            printf(" %d:%02X", i, ciar()); }
        printf("\n");
        /* timed scratch R/W on I0 and I1 */
        cwrite(0x00, 0xAA); a = cread(0x00);
        cwrite(0x00, 0x55); b = cread(0x00);
        printf(" I0: wrAA->%02X wr55->%02X\n", a, b);
        cwrite(0x01, 0xAA); a = cread(0x01);
        cwrite(0x01, 0x55); b = cread(0x01);
        printf(" I1: wrAA->%02X wr55->%02X\n", a, b);
        printf(" I0-15:");
        for (i = 0; i < 16; i++) printf(" %02X", cread((unsigned char)i));
        printf("\n");
        i12a = cread(0x0C);
        cwrite_mce(0x0C, (unsigned char)(i12a | 0x40));
        i12b = cread(0x0C);
        i25 = cread(0x19);
        printf(" I12 %02X ->MODE2-> %02X   I25=%02X ", i12a, i12b, i25);
        if      ((i25 & 0xE0) == 0x80) printf("=> CS4231\n");
        else if ((i25 & 0xE0) == 0xA0) printf("=> CS4231A\n");
        else if ((i12b & 0x40) == 0)   printf("=> AD1848 (no MODE2) or dead\n");
        else                            printf("=> unknown\n");
    }
    printf("\ndone.\n");
    return 0;
}
