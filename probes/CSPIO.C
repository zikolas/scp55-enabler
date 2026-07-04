/* CSPIO.C - Roland SCP-55 / CS4231A: does DMA-less PIO playback work?
 * The PC110 socket has no DMA path, so normal WSS/CS4231 DMA playback is dead.
 * But the CS4231 has a Programmed-I/O Data Register (PDR = codec base+3 = 0x33B)
 * and a status "playback ready" flag.  This streams a 440Hz square wave to the
 * PDR, paced by the status flag, for ~2s, and reports how many samples actually
 * flowed + which status bits toggled - a definitive PIO-works/doesn't answer.
 *
 * Codec (CS4231A) at 0x338: IAR/IDR/status/PDR = 0x338/0x339/0x33A/0x33B.
 * Build: C:\WATCOM\BLD CSPIO
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

static unsigned g_manf=0, g_card=0, g_cfg=0x400;
static char g_vers[80];
static void read_cis(unsigned seg)
{
    unsigned char __far *p = (unsigned char __far *)MK_FP(seg, 0);
    unsigned off = 0; int g, vi = 0, m;
    for (g = 0; g < 64; g++) {
        unsigned char code = p[off], link;
        if (code == 0xFF) break;
        if (code == 0x00) { off += 2; continue; }
        link = p[off + 2];
        if (code == 0x20) { g_manf=(unsigned)p[off+4]|((unsigned)p[off+6]<<8);
                            g_card=(unsigned)p[off+8]|((unsigned)p[off+10]<<8); }
        else if (code == 0x1A) { int rasz=(p[off+4]&0x03)+1; g_cfg=p[off+8];
                                 if(rasz>=2) g_cfg|=(unsigned)p[off+10]<<8; }
        else if (code == 0x15) { for(m=2;m<link;m++){unsigned char c=p[off+4+2*m];
                                 if(vi<79) g_vers[vi++]=c?(char)c:' ';} g_vers[vi]=0; }
        if (link == 0xFF) break;
        off += ((unsigned)link + 2) * 2;
        if (off >= 0x3000) break;
    }
    while (vi>0 && g_vers[vi-1]==' ') g_vers[--vi]=0;
}

#define IAR 0x338
#define IDR 0x339
#define SR  0x33A
#define PDR 0x33B
static void cwait(void){ int i; for(i=0;i<8000;i++) if(!(inp(IAR)&0x80)) return; }
static unsigned char cget(unsigned char idx){ cwait(); outp(IAR, idx); iod(30); return (unsigned char)inp(IDR); }
static void cput(unsigned char idx, unsigned char v){ cwait(); outp(IAR, idx); iod(30); outp(IDR, v); iod(30); }
/* write a mode-change reg with MCE held */
static void cput_mce(unsigned char idx, unsigned char v)
{ cwait(); outp(IAR,(unsigned char)(0x40|idx)); iod(30); outp(IDR,v); iod(30); }
static void mce_off(void){ cwait(); outp(IAR, 0x00); }   /* index 0, MCE clear */

int main(void)
{
    unsigned memseg = 0xD000, iobase = 0x330;
    unsigned char __far *cor;
    unsigned long __far *bios = (unsigned long __far *)MK_FP(0x40, 0x6C);
    unsigned start, stop, woff;
    unsigned long t0, t1, samples = 0, iters = 0;
    unsigned char sr, srseen = 0, sq = 0x00;
    int i, half = 9;   /* 8000/(440*2) ~= 9 samples per half period */

    pidx = PCIC; soff = 0;
    if ((rd(0x00)&0xC0)!=0x80) { printf("no 82365\n"); return 1; }
    if ((rd(0x01)&0x0C)!=0x0C) { printf("no card\n"); return 1; }
    wr(0x02,0x95); MS(30);
    if (!(rd(0x01)&0x40)) { printf("no power\n"); wr(0x02,0); return 1; }
    wr(0x03,0x40); MS(10);
    start=memseg>>8; stop=(memseg>>8)+3; woff=((unsigned)(0-(memseg>>8))&0x3FFF)|0x4000;
    wr(0x10,start&0xFF); wr(0x11,(start>>8)&0x3F);
    wr(0x12,stop&0xFF);  wr(0x13,(stop>>8)&0x3F);
    wr(0x14,woff&0xFF);  wr(0x15,(woff>>8)&0xFF);
    wr(0x06,0x01); MS(5);
    read_cis(memseg);
    if (g_manf!=0xC00C || g_card!=0x0001) { printf("not SCP-55 (%04X/%04X)\n",g_manf,g_card); wr(0x06,0);wr(0x03,0);wr(0x02,0); return 1; }
    cor = (unsigned char __far *)MK_FP(memseg,g_cfg); *cor = 0x01;
    wr(0x08,iobase&0xFF); wr(0x09,(iobase>>8)&0xFF);
    wr(0x0A,(iobase+0x0F)&0xFF); wr(0x0B,((iobase+0x0F)>>8)&0xFF);
    wr(0x07,0x00); wr(0x03,0x60); wr(0x06,0x41); MS(10);

    /* un-mute output */
    cput(0x06, 0x00); cput(0x07, 0x00);          /* L/R DAC: unmute, 0dB */

    /* set playback format under MCE: I8 = 8-bit unsigned, mono, 8000 Hz (0x00) */
    cput_mce(0x08, 0x00);
    /* interface config I9 = PEN (playback enable), no capture, no autocal */
    cput_mce(0x09, 0x01);
    mce_off();
    MS(50);                                       /* settle */

    /* large playback sample count so it doesn't auto-stop (I15:hi I14:lo) */
    cput(0x0F, 0xFF); cput(0x0E, 0xFF);

    printf("CS4231A PIO test @ %03X: streaming 440Hz square for ~2s...\n", IAR);
    t0 = *bios;
    for (;;) {
        iters++;
        sr = (unsigned char)inp(SR);
        srseen |= sr;
        if (sr & 0x02) {                          /* PRDY: codec wants a sample */
            outp(PDR, sq);
            if (--half <= 0) { half = 9; sq ^= 0xFF; }
            samples++;
        }
        if ((iters & 0x3FFF) == 0) {              /* time check periodically */
            t1 = *bios;
            if ((t1 - t0) > 37UL) break;          /* ~2s (18.2 Hz) */
        }
        if (iters > 20000000UL) break;            /* hard cap */
    }
    t1 = *bios;

    /* stop playback */
    cput_mce(0x09, 0x00); mce_off();
    cput(0x06, 0x80); cput(0x07, 0x80);           /* mute DAC again */

    printf("elapsed %lu ticks (%lu ms), iters=%lu\n", t1-t0, (t1-t0)*55UL, iters);
    printf("samples written to PDR: %lu\n", samples);
    if ((t1-t0) > 0) printf("=> ~%lu samples/sec\n", samples / (((t1-t0)*55UL)/1000UL ? ((t1-t0)*55UL)/1000UL : 1));
    printf("status bits ever seen (OR): %02X  (bit1=PRDY, bit0=INT, bit3=underrun)\n", srseen);
    if (samples > 4000UL) printf("VERDICT: PIO playback FLOWS - DMA-less digital is possible. Did you hear a tone?\n");
    else                  printf("VERDICT: PRDY never paced (samples ~0) - PIO playback does NOT self-feed here.\n");
    return 0;
}
