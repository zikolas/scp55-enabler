/* MPUTEST.C - Roland SCP-55: confirm the 0x330 MPU-401 UART drives the onboard
 * GS Sound Canvas, and play an audible test note.  Also un-mutes the CS4231A
 * mixer (its DAC/aux inputs power up muted) so the synth audio can pass.
 *
 * MPU-401 UART at 0x330 (data) / 0x331 (cmd+status).  Codec (CS4231A) at 0x338.
 * Clean-room, no Card Services.  Build: C:\WATCOM\BLD MPUTEST
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

/* CS4231A codec at 0x338 */
#define CODEC 0x338
static void cwr(unsigned char idx, unsigned char val)
{
    int i; for(i=0;i<4000;i++) if(!(inp(CODEC)&0x80)) break;
    outp(CODEC, idx); iod(50); outp(CODEC+1, val); iod(50);
}

/* MPU-401 UART at 0x330 data / 0x331 cmd+status.
 * status bit7(0x80)=DSR (0=data to read), bit6(0x40)=DRR (0=ok to write) */
#define MPU_DATA 0x330
#define MPU_CMD  0x331
static int mpu_wr_ready(void){ int i; for(i=0;i<20000;i++) if(!(inp(MPU_CMD)&0x40)) return 1; return 0; }
static int mpu_rd_ready(void){ int i; for(i=0;i<20000;i++) if(!(inp(MPU_CMD)&0x80)) return 1; return 0; }
static int mpu_cmd(unsigned char c){ if(!mpu_wr_ready()) return -1; outp(MPU_CMD, c);
    if(!mpu_rd_ready()) return -2; return (unsigned char)inp(MPU_DATA); }
static void midi(unsigned char b){ if(mpu_wr_ready()) outp(MPU_DATA, b); }

int main(void)
{
    unsigned memseg = 0xD000, iobase = 0x330;
    unsigned char __far *cor;
    unsigned start, stop, woff;
    int ackR, ackU, i;
    static unsigned char notes[] = { 60, 64, 67, 72 };   /* C E G C */

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
    printf("CIS \"%s\" %04X/%04X\n", g_vers, g_manf, g_card);
    if (g_manf!=0xC00C || g_card!=0x0001) { printf("not SCP-55\n"); wr(0x06,0);wr(0x03,0);wr(0x02,0); return 1; }
    cor = (unsigned char __far *)MK_FP(memseg,g_cfg); *cor = 0x01;
    wr(0x08,iobase&0xFF); wr(0x09,(iobase>>8)&0xFF);
    wr(0x0A,(iobase+0x0F)&0xFF); wr(0x0B,((iobase+0x0F)>>8)&0xFF);
    wr(0x07,0x00); wr(0x03,0x60); wr(0x06,0x41); MS(10);

    /* un-mute the CS4231A mixer: DAC out + Aux1 + Aux2 (synth likely on an aux) */
    cwr(0x06,0x00); cwr(0x07,0x00);        /* Left/Right DAC: unmute, 0dB */
    cwr(0x02,0x08); cwr(0x03,0x08);        /* Left/Right Aux1: unmute, 0dB */
    cwr(0x04,0x08); cwr(0x05,0x08);        /* Left/Right Aux2: unmute, 0dB */
    printf("CS4231A mixer un-muted (DAC/Aux1/Aux2)\n");

    /* MPU-401 reset + UART mode */
    ackR = mpu_cmd(0xFF);                   /* reset */
    MS(50);
    ackU = mpu_cmd(0x3F);                   /* enter UART mode */
    printf("MPU-401 @330: reset ACK=%d  UART ACK=%d  (0xFE=254 = good)\n", ackR, ackU);

    /* play a GM arpeggio on the Sound Canvas */
    midi(0xC0); midi(0x00);                 /* prog 0 = Acoustic Grand Piano, ch0 */
    for (i = 0; i < 4; i++) {
        midi(0x90); midi(notes[i]); midi(0x64);   /* note on, vel 100 */
        MS(350);
        midi(0x80); midi(notes[i]); midi(0x00);   /* note off */
    }
    /* final chord */
    for (i = 0; i < 4; i++) { midi(0x90); midi(notes[i]); midi(0x64); }
    MS(900);
    for (i = 0; i < 4; i++) { midi(0x90); midi(notes[i]); midi(0x00); }

    printf("Played C-E-G-C + chord on the Sound Canvas. Did you hear it?\n");
    printf("(card left enabled: MPU 330, codec 338)\n");
    return 0;
}
