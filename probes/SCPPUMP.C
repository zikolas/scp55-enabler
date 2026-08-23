/* SCPPUMP.C - gapless WAV playback on the SCP-55 via an RTC-paced ISR pump.
 *
 * This is the engine sc_scp55.c will need, prototyped standalone so it can be
 * debugged in isolation rather than inside the VSBPCMCIA framework.
 *
 *   RTC (IRQ 8) tick -> ISR tops up the codec FIFO from a ring buffer
 *   main loop        -> refills the ring from disk
 *
 * Feeding happens in the interrupt, so a disk read never stalls the DAC - which
 * is exactly the click SCPWAV produces at every buffer refill.
 *
 * Register map (proven 2026-08-24, NOT the datasheet defaults):
 *   R0 IAR = base+8   R1 IDR = base+9   R2 STATUS = base+6   R3 PDR = base+7
 *
 * TICK RATE MATTERS: the codec's playback FIFO is 16 samples, which at 22.05 kHz
 * is only ~0.7 ms, so it must be serviced faster than ~1400 Hz or it drains
 * between visits.  Hence 2048 Hz for rates above 12 kHz and 1024 Hz below - at
 * those rates each tick moves ~11 bytes, which the FIFO absorbs without the ISR
 * having to spin.  Spinning inside the ISR at 22 kHz would occupy the entire
 * tick period.
 *
 * Why the RTC and not the card's IRQ: SCPENA assigns one through Card Services
 * and we never determined which.  The RTC needs no PCIC access and is the same
 * clock VSBPCMCIA already pumps from.
 *
 * SAFETY: restores the RTC registers, both PIC masks and the INT 70h vector on
 * every exit path including the keypress abort.  Codec registers only otherwise.
 *
 * Usage: SCPPUMP file.wav [/BASE=330] [/CFS=n] [/RS=5] [/NPER=n]
 */
#include <stdio.h>
#include <conio.h>
#include <string.h>
#include <dos.h>
#include <i86.h>

static unsigned BASE = 0x330;
#define IAR (BASE+8)
#define IDR (BASE+9)
#define SRP (BASE+6)
#define PDR (BASE+7)

#define SR_PRDY 0x02
#define SR_SER  0x10

#define RING 32768U
#define RMASK (RING-1)

static unsigned char ring[RING];
static volatile unsigned rhead = 0, rtail = 0;   /* head=main, tail=ISR */
static volatile unsigned long isr_ticks = 0, isr_bytes = 0;
static volatile unsigned long isr_dry = 0, isr_ser = 0;
static volatile unsigned nper = 12;
static unsigned g_srp, g_pdr;                    /* ISR-local copies    */

static void (__interrupt __far *old70)(void);
static unsigned char old_a, old_b, old_m1, old_m2;
static int rtc_on = 0;

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

static unsigned long bios_ticks_stub(void)
{ unsigned long far *t = (unsigned long far *)MK_FP(0x40,0x6C); return *t; }

static unsigned char cmos_rd(unsigned char r)
{ outp(0x70, r); iod(4); return (unsigned char)inp(0x71); }
static void cmos_wr(unsigned char r, unsigned char v)
{ outp(0x70, r); iod(4); outp(0x71, v); iod(4); }

/* ---- the pump ------------------------------------------------------------
 * Short and non-spinning: move what the FIFO will take right now, then leave.
 * If the ring is dry we count it - that is the signal the main loop is losing
 * the race, and it is the number that matters when this becomes a driver.
 */
static void __interrupt __far __loadds rtc_isr(void)
{
    unsigned n = nper;
    unsigned char sr;

    isr_ticks++;
    while (n--) {
        if (rtail == rhead) { isr_dry++; break; }        /* ring empty */
        sr = (unsigned char)inp(g_srp);
        if (sr & SR_SER) isr_ser++;
        if (!(sr & SR_PRDY)) break;                      /* FIFO full  */
        outp(g_pdr, ring[rtail]);
        rtail = (rtail + 1) & RMASK;
        isr_bytes++;
    }

    (void)cmos_rd(0x0C);            /* clear the RTC interrupt or it stops */
    outp(0xA0, 0x20);               /* EOI slave  */
    outp(0x20, 0x20);               /* EOI master */
}

static void rtc_start(unsigned char rs)
{
    old70 = _dos_getvect(0x70);
    _dos_setvect(0x70, rtc_isr);

    old_a = cmos_rd(0x0A);
    old_b = cmos_rd(0x0B);
    cmos_wr(0x0A, (unsigned char)((old_a & 0xF0) | (rs & 0x0F)));
    cmos_wr(0x0B, (unsigned char)(old_b | 0x40));        /* PIE */
    (void)cmos_rd(0x0C);

    old_m1 = (unsigned char)inp(0x21);
    old_m2 = (unsigned char)inp(0xA1);
    outp(0xA1, (unsigned char)(old_m2 & 0xFE));          /* unmask IRQ8    */
    outp(0x21, (unsigned char)(old_m1 & 0xFB));          /* unmask cascade */
    rtc_on = 1;
}

static void rtc_stop(void)
{
    if (!rtc_on) return;
    cmos_wr(0x0B, old_b);
    cmos_wr(0x0A, old_a);
    (void)cmos_rd(0x0C);
    outp(0x21, old_m1);
    outp(0xA1, old_m2);
    _dos_setvect(0x70, old70);
    rtc_on = 0;
}

static unsigned long rate_of[8] =
    { 5512UL, 11025UL, 18900UL, 22050UL, 37800UL, 44100UL, 33075UL, 6620UL };
static unsigned long rd32(unsigned char *p)
{ return (unsigned long)p[0]|((unsigned long)p[1]<<8)
       |((unsigned long)p[2]<<16)|((unsigned long)p[3]<<24); }
static unsigned rd16(unsigned char *p)
{ return (unsigned)p[0]|((unsigned)p[1]<<8); }

int main(int argc, char **argv)
{
    char *fname = NULL;
    int i, cfs = -1, rs = -1, best = 1;
    unsigned chan = 0, bits = 0, space, want;
    unsigned long srate = 0, dlen = 0, t0, t1;
    unsigned char hdr[64], i8, i11;
    FILE *f;

    for (i = 1; i < argc; i++) {
        if      (!strnicmp(argv[i],"/BASE=",6)) sscanf(argv[i]+6,"%x",&BASE);
        else if (!strnicmp(argv[i],"/CFS=", 5)) sscanf(argv[i]+5,"%d",&cfs);
        else if (!strnicmp(argv[i],"/RS=",  4)) sscanf(argv[i]+4,"%d",&rs);
        else if (!strnicmp(argv[i],"/NPER=",6)) { int t; sscanf(argv[i]+6,"%d",&t);
                                                  nper = (unsigned)t; }
        else if (argv[i][0] != '/') fname = argv[i];
    }
    if (!fname) { printf("usage: SCPPUMP file.wav [/CFS=n] [/RS=5] [/NPER=n]\n");
                  return 1; }

    f = fopen(fname, "rb");
    if (!f) { printf("cannot open %s\n", fname); return 1; }
    if (fread(hdr,1,12,f)!=12 || memcmp(hdr,"RIFF",4) || memcmp(hdr+8,"WAVE",4)) {
        printf("not a RIFF/WAVE file\n"); fclose(f); return 1; }
    for (;;) {
        unsigned long clen;
        if (fread(hdr,1,8,f)!=8) { printf("no data chunk\n"); fclose(f); return 1; }
        clen = rd32(hdr+4);
        if (!memcmp(hdr,"fmt ",4)) {
            unsigned char fm[32]; unsigned w = (unsigned)(clen>32?32:clen);
            if (fread(fm,1,w,f)!=w) { fclose(f); return 1; }
            if (rd16(fm)!=1) { printf("not PCM\n"); fclose(f); return 1; }
            chan=rd16(fm+2); srate=rd32(fm+4); bits=rd16(fm+14);
            if (clen>w) fseek(f,(long)(clen-w),SEEK_CUR);
        } else if (!memcmp(hdr,"data",4)) { dlen = clen; break; }
        else fseek(f,(long)(clen+(clen&1)),SEEK_CUR);
    }
    if (!chan || (bits!=8 && bits!=16) || chan>2) {
        printf("unsupported format\n"); fclose(f); return 1; }

    if (cfs < 0) {
        unsigned long bd = 0xFFFFFFFFUL;
        for (i=0;i<8;i++) {
            unsigned long d = rate_of[i]>srate ? rate_of[i]-srate : srate-rate_of[i];
            if (d<bd) { bd=d; best=i; }
        }
        cfs = best;
    }
    /* FIFO is 16 samples: service faster than the time it takes to drain */
    if (rs < 0) rs = (rate_of[cfs] > 12000UL) ? 5 : 6;   /* 2048 Hz : 1024 Hz */

    printf("SCPPUMP - %s: %lu Hz %uch %ub -> CFS=%d (%lu Hz)\n",
           fname, srate, chan, bits, cfs, rate_of[cfs]);
    printf("RTC RS=%d (%u Hz), %u bytes/tick, ring %u\n",
           rs, (unsigned)(32768U >> (rs-1)), nper, RING);

    i8 = (unsigned char)((cfs & 7) << 1);
    if (chan==2)  i8 |= 0x10;
    if (bits==16) i8 |= 0x40;

    for (i=0;i<8;i++) {
        outp(IAR,(unsigned char)i); iod(200);
        if ((inp(IAR)&0x1F)!=i) { printf("codec not responding\n");
                                  fclose(f); return 1; }
    }
    g_srp = SRP; g_pdr = PDR;

    ci_put(0x0C,(unsigned char)(ci_get(0x0C)|0x40));
    ci_put_mce(0x08, i8);
    ci_put_mce(0x09, 0xC8);
    ci_put(0x00,0xC0); ci_put(0x01,0xC0);
    ci_put(0x02,0x0A); ci_put(0x03,0x0A);
    ci_put(0x06,0x03); ci_put(0x07,0x03);
    ci_put(0x0A,0x02); ci_put(0x1A,0xC0);
    ci_put(0x15,0x00); ci_put(0x14,0x42);
    ci_put(0x10,0xC0);

    /* prime the ring before the DAC is enabled */
    want = (unsigned)(dlen > (unsigned long)(RING-1) ? (RING-1) : dlen);
    rhead = (unsigned)fread(ring, 1, want, f);
    dlen -= rhead;

    ci_put(0x09,0xC9);                       /* PEN - go */
    MS(5);
    (void)inp(SRP);
    rtc_start((unsigned char)rs);

    printf("playing - any key to stop\n");
    t0 = bios_ticks_stub();
    while (dlen || rtail != rhead) {
        space = (unsigned)((rtail - rhead - 1) & RMASK);
        if (dlen && space > 512) {
            unsigned n, chunk = space;
            if (chunk > 4096) chunk = 4096;
            if ((unsigned long)chunk > dlen) chunk = (unsigned)dlen;
            if ((unsigned)(rhead + chunk) > RING) chunk = RING - rhead;
            n = (unsigned)fread(ring + rhead, 1, chunk, f);
            if (!n) dlen = 0;
            else { rhead = (unsigned)((rhead + n) & RMASK); dlen -= n; }
        }
        if (kbhit()) { (void)getch(); break; }
    }
    t1 = bios_ticks_stub();

    rtc_stop();
    i11 = ci_get(0x0B);
    ci_put(0x09,0xC8); ci_put(0x10,0x00);
    ci_put(0x06,0x80); ci_put(0x07,0x80);
    fclose(f);

    printf("\nticks=%lu  bytes=%lu  ring-dry=%lu  SER=%lu\n",
           isr_ticks, isr_bytes, isr_dry, isr_ser);
    printf("elapsed %lu ticks (%lu.%lu s)   final I11=%02X (PUR=%d)\n",
           t1-t0, (t1-t0)/18UL, ((t1-t0)%18UL)*10UL/18UL, i11, (i11>>6)&1);
    printf("ring-dry counts ISR visits that found no data: that is the number\n"
           "that matters when this becomes the backend's pump.\n");
    return 0;
}
