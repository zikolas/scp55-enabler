/* GSGO.C - Roland SCP-55 DOS point enabler for GS MIDI.
 * Copyright (c) 2026 zikolas. MIT License.
 *
 * Clean-room: built from the card's own CIS, the public Intel 82365SL PCIC
 * register set, and the public AD1848/Crystal CS4231 codec model. No Card
 * Services / Socket Services. Does NOT disassemble the Roland drivers.
 *
 * The SCP-55 carries a Roland GS Sound Canvas synth reached over an MPU-401
 * UART, plus a Crystal CS4231A codec (its mixer routes the synth to the audio
 * out). This platform (PC110, 82365SL, no DMA) can't do the card's WSS/SB
 * digital audio - but the Sound Canvas needs no DMA, so GSGO sets up exactly
 * that: MPU-401 for MIDI + the CS4231A mixer un-muted so you hear it.
 *
 * There is NO gameport wired on this unit, so GSGO never touches 0x201.
 *
 * Layout at the chosen base B (default 0x330): B/B+1 = MPU-401 UART,
 * B+8..B+11 = CS4231A codec (IAR/IDR/status/PIO).
 *
 * Build (Open Watcom, 16-bit real mode):  C:\WATCOM\BLD GSGO
 *
 * Usage: GSGO [/MPU=330] [/I=5] [/MT32] [/MIDI=file] [/SYSEX=hex] [/RESET]
 *             [/LINE[=gain]] [/S=0..7] [/W=D000] [/OFF]
 *   /MPU=hex   MPU-401 base: 330 (idx1) / 370 (idx2) / 3B0 (idx3) / 3F0 (idx4)
 *   /I=dec     IRQ to route (default 5; /I=0 = none)
 *   /MT32      send MT32EMUL.MID from the current folder (put Roland's file there)
 *              to switch the Sound Canvas into MT-32 emulation mode
 *   /MIDI=file send any Standard MIDI File once (e.g. a synth setup .MID)
 *   /SYSEX=hex send a raw MIDI/SysEx message straight from the command line, e.g.
 *              /SYSEX=F04110421240007F0041F7 (non-hex chars ignored; = GS reset)
 *   /RESET     GS reset the Sound Canvas (built-in standard GS Reset SysEx)
 *   /LINE     pass the line-in jack through to the output (CS4231A Line input);
 *             /LINE=0..8 sets the gain: 0 = 0dB, up to 8 = max (+12dB, 1.5dB/step).
 *             /LINE alone = max gain.
 *   /S=dec    socket 0..7 (default: auto-scan for the SCP-55)
 *   /W=hex    attribute-window segment for the CIS/COR (default D000)
 *   /OFF      power the SCP-55's socket down and exit
 */
#include <stdio.h>
#include <conio.h>
#include <stdlib.h>
#include <i86.h>

#define GSGO_VER "1.2"
#define PCIC_BASE 0x3E0
#define MAX_SOCKET 7

static unsigned pcic_idx = PCIC_BASE;
static unsigned sockoff  = 0;

static void select_socket(unsigned s)
{
    pcic_idx = PCIC_BASE + (s >> 1) * 2;
    sockoff  = (s & 1) ? 0x40 : 0x00;
}
static unsigned char rd(unsigned r){ outp(pcic_idx, sockoff + r); return (unsigned char)inp(pcic_idx + 1); }
static void          wr(unsigned r, unsigned v){ outp(pcic_idx, sockoff + r); outp(pcic_idx + 1, v); }
static int  controller_present(void){ return (rd(0x00) & 0xC0) == 0x80; }
static void iod(unsigned long n){ while (n--) (void)inp(0x80); }
#define MS(x) iod((unsigned long)(x) * 1000UL)

/* ---- CIS ---- */
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
        else if (code == 0x1A) { int rasz = (p[off+4]&0x03)+1; g_cfg = p[off+8];
                                 if (rasz >= 2) g_cfg |= (unsigned)p[off+10]<<8; }
        else if (code == 0x15) { for (m=2;m<link;m++){ unsigned char c=p[off+4+2*m];
                                 if (vi<79) g_vers[vi++]=c?(char)c:' '; } g_vers[vi]=0; }
        if (link == 0xFF) break;
        off += ((unsigned)link + 2) * 2;
        if (off >= 0x3000) break;
    }
    while (vi > 0 && g_vers[vi-1]==' ') g_vers[--vi]=0;
}
static int is_scp55(void){ return g_manf == 0xC00C && g_card == 0x0001; }

/* Does host segment cseg..cseg+3 pages overlap an ENABLED memory window on any
 * present socket? (i.e. is another card already mapped there?)  Reads only, so
 * it disturbs nothing. Memory windows 0..4 = bits 0..4 of the Window-Enable reg
 * (0x06); each window's start/stop host page is in regs 0x10+w*8 .. +3. */
static int mem_win_overlaps(unsigned cseg)
{
    unsigned cs = cseg >> 8, ce = (cseg >> 8) + 3;      /* our probe's start/stop page (A19-A12) */
    unsigned chip, half, w;
    for (chip = 0; chip < 4; chip++) {
        pcic_idx = PCIC_BASE + chip * 2; sockoff = 0;
        if (!controller_present()) continue;
        for (half = 0; half < 2; half++) {
            unsigned char en;
            sockoff = half ? 0x40 : 0x00;
            en = rd(0x06);
            for (w = 0; w < 5; w++) {
                unsigned base = 0x10 + w * 8, ws, we;
                if (!(en & (1 << w))) continue;         /* window w not enabled */
                ws = ((unsigned)(rd(base+1) & 0x0F) << 8) | rd(base);
                we = ((unsigned)(rd(base+3) & 0x0F) << 8) | rd(base+2);
                if (cs <= we && ws <= ce) return 1;     /* ranges overlap */
            }
        }
    }
    return 0;
}

/* Pick a 16 KB host segment for the CIS probe that no other card has mapped, so
 * the scan won't collide with an in-use card's window. Prefers 'want' (the D000
 * default or a /W value); falls back through a list of commonly-free segments. */
static unsigned find_free_window(unsigned want)
{
    static unsigned cand[] = { 0xD000, 0xCC00, 0xD400, 0xD800, 0xDC00,
                               0xE000, 0xE400, 0xE800, 0xC800, 0 };
    int i;
    if (!mem_win_overlaps(want)) return want;
    for (i = 0; cand[i]; i++) if (!mem_win_overlaps(cand[i])) return cand[i];
    return want;                                        /* nothing free - use the default anyway */
}

/* Power a socket, map its attribute window, read the CIS. Returns 1 if the card
 * there is the SCP-55 (left powered+mapped). If it's not ours, the socket is put
 * back the way we found it: a socket we powered up is powered back down, but a
 * card that was ALREADY enabled/in use is restored, not disturbed or powered off
 * (so scanning doesn't clobber another card the user has running). */
static int probe_socket(unsigned memseg)
{
    unsigned start, stop, woff;
    unsigned char s03, s06, s10, s11, s12, s13, s14, s15;
    int was_on;
    if ((rd(0x01) & 0x0C) != 0x0C) return 0;             /* no card */
    was_on = (rd(0x01) & 0x40) != 0;                     /* power already active => a card here is already up/in use */
    /* save the socket regs we borrow for the CIS read, so an already-enabled
     * card can be put back exactly as we found it */
    s03 = rd(0x03); s06 = rd(0x06);
    s10 = rd(0x10); s11 = rd(0x11); s12 = rd(0x12);
    s13 = rd(0x13); s14 = rd(0x14); s15 = rd(0x15);
    if (!was_on) {                                       /* only power a socket we found off */
        wr(0x02, 0x95); MS(20);                          /* 5V */
        if (!(rd(0x01) & 0x40)) { wr(0x02, 0x00); return 0; }
    }
    wr(0x03, 0x40); MS(10);                              /* reset off, mem mode */
    start = memseg >> 8; stop = (memseg >> 8) + 3;
    woff  = ((unsigned)(0 - (memseg >> 8)) & 0x3FFF) | 0x4000;
    wr(0x10, start & 0xFF); wr(0x11, (start >> 8) & 0x3F);
    wr(0x12, stop  & 0xFF); wr(0x13, (stop  >> 8) & 0x3F);
    wr(0x14, woff  & 0xFF); wr(0x15, (woff  >> 8) & 0xFF);
    wr(0x06, rd(0x06) | 0x01);                           /* enable mem win0 (keep other windows) */
    read_cis(memseg);
    if (is_scp55()) return 1;                            /* our card: leave it powered + mapped */
    /* not our card - undo the probe without harming an in-use card */
    if (was_on) {                                        /* already-enabled card: restore exactly, DON'T power off */
        wr(0x10, s10); wr(0x11, s11); wr(0x12, s12); wr(0x13, s13);
        wr(0x14, s14); wr(0x15, s15); wr(0x06, s06); wr(0x03, s03);
    } else {                                             /* we powered it up just to peek: power it back down */
        wr(0x06, 0x00); wr(0x03, 0x00); wr(0x02, 0x00);
    }
    return 0;
}

/* ---- CS4231A codec at cbase (generous settles - the codec is MWAIT-timed) ---- */
static unsigned CBASE;
/* wait while the codec is busy (IAR reads 0x80 during power-on calibration /
 * resync). Generous: a cold-booted card is still calibrating for ~20ms. */
static void ccwait(void){ unsigned long i; for (i=0;i<400000UL;i++) if (!(inp(CBASE)&0x80)) return; }
static void ccput(unsigned char idx, unsigned char v){ ccwait(); outp(CBASE, idx); iod(200); outp(CBASE+1, v); iod(200); }
static unsigned char ccget(unsigned char idx){ ccwait(); outp(CBASE, idx); iod(200); return (unsigned char)inp(CBASE+1); }
/* enable MODE2 (I12 bit6) so the Line-input regs (I18/I19) exist; verify+retry */
static int cc_mode2(void)
{
    int t;
    for (t = 0; t < 4; t++) {
        unsigned char i12 = ccget(0x0C);
        ccwait(); outp(CBASE, (unsigned char)(0x40 | 0x0C)); iod(200);   /* MCE */
        outp(CBASE+1, (unsigned char)(i12 | 0x40)); iod(200);
        ccwait(); outp(CBASE, 0x0C); iod(200); MS(2);                    /* drop MCE */
        if (ccget(0x0C) & 0x40) return 1;
    }
    return 0;
}

/* ---- MPU-401 UART: base = data, base+1 = cmd+status ---- */
static unsigned MBASE;
static int mpu_wr_ready(void){ int i; for (i=0;i<20000;i++) if (!(inp(MBASE+1)&0x40)) return 1; return 0; }
static int mpu_rd_ready(void){ int i; for (i=0;i<20000;i++) if (!(inp(MBASE+1)&0x80)) return 1; return 0; }
static int mpu_cmd(unsigned char c){ if (!mpu_wr_ready()) return -1; outp(MBASE+1, c);
    if (!mpu_rd_ready()) return -2; return (unsigned char)inp(MBASE); }

/* The GS Reset is an 11-byte *published standard* command from the Roland GS
 * spec (a functional protocol command, like the MPU 0xFF reset - not creative
 * content), so we embed it. MT-32 mode and any other setup come from a MIDI file
 * the USER supplies (see midi_send_file), so GSGO ships no vendor content. */
static unsigned char gs_reset[] = {
    0xF0,0x41,0x10,0x42,0x12,0x40,0x00,0x7F,0x00,0x41,0xF7
};

static void midi_out(unsigned char b){ if (mpu_wr_ready()) outp(MBASE, b); }

/* Before a mode change, silence any hanging notes on all 16 channels (All Sound
 * Off, CC120) - instant insurance ahead of any GS-reset settle. */
static void midi_panic(void)
{
    int ch;
    for (ch = 0; ch < 16; ch++) { midi_out((unsigned char)(0xB0 | ch)); midi_out(0x78); midi_out(0x00); }
}

/* send a raw MIDI byte buffer (the embedded GS reset, or /SYSEX), settle after F7 */
static void midi_send_raw(unsigned char *b, unsigned len)
{
    unsigned i;
    midi_panic();
    for (i = 0; i < len; i++) { midi_out(b[i]); if (b[i] == 0xF7) MS(50); }
}

/* parse a hex string ("F0411042...F7") into bytes; non-hex chars are skipped so
 * separators are fine. Used by /SYSEX to send a raw MIDI/SysEx message. */
static unsigned char sysexbuf[128];
static unsigned parse_hex(char *s, unsigned char *out, unsigned max)
{
    unsigned n = 0; int hi = -1;
    while (*s && n < max) {
        char c = *s++; int d = -1;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else continue;                                  /* skip spaces/commas/etc. */
        if (hi < 0) hi = d;
        else { out[n++] = (unsigned char)((hi << 4) | d); hi = -1; }
    }
    return n;
}

/* --- fire-once Standard MIDI File sender ------------------------------------
 * Reads a user-supplied .MID, walks the first track, and streams its SysEx +
 * channel messages to the MPU UART, honoring the file's own delta-time delays.
 * This is how /MT32 and /MIDI=file work - the setup content is the user's file,
 * not ours.  Returns 1 = sent, 0 = file not found, -1 = not a valid SMF. */
static unsigned char smfbuf[16384];
static unsigned char *smf_p, *smf_end;
static unsigned long smf_vlq(void)
{
    unsigned long v = 0;
    while (smf_p < smf_end) { unsigned char c = *smf_p++; v = (v << 7) | (c & 0x7F); if (!(c & 0x80)) break; }
    return v;
}
static int midi_send_file(char *fname)
{
    FILE *f; unsigned n, div, status = 0; unsigned char *p; unsigned long tempo = 500000UL, clen;
    f = fopen(fname, "rb");
    if (!f) return 0;
    n = (unsigned)fread(smfbuf, 1, sizeof(smfbuf), f);
    fclose(f);
    if (n < 22 || smfbuf[0]!='M' || smfbuf[1]!='T' || smfbuf[2]!='h' || smfbuf[3]!='d') return -1;
    div = ((unsigned)smfbuf[12] << 8) | smfbuf[13];
    if (div == 0 || (div & 0x8000)) div = 96;               /* metric division; punt on SMPTE */
    p = smfbuf + 14;                                        /* find first MTrk */
    while (p + 8 <= smfbuf + n && !(p[0]=='M'&&p[1]=='T'&&p[2]=='r'&&p[3]=='k')) {
        clen = ((unsigned long)p[4]<<24)|((unsigned long)p[5]<<16)|((unsigned long)p[6]<<8)|p[7];
        p += 8 + clen;
    }
    if (p + 8 > smfbuf + n) return -1;
    clen = ((unsigned long)p[4]<<24)|((unsigned long)p[5]<<16)|((unsigned long)p[6]<<8)|p[7];
    smf_p = p + 8; smf_end = smf_p + clen;
    if (smf_end > smfbuf + n) smf_end = smfbuf + n;
    midi_panic();
    while (smf_p < smf_end) {
        unsigned long dt = smf_vlq();
        if (dt) { unsigned long ms = dt * (tempo / div) / 1000UL;
                  if (ms > 500UL) ms = 500UL;      /* cap: setup files need <=~500ms settles, and this clips pointless trailing gaps */
                  if (ms) MS(ms); }
        if (smf_p >= smf_end) break;
        if (*smf_p == 0xFF) {                              /* meta - skip (track tempo) */
            unsigned char mt; unsigned long ln;
            smf_p++; mt = *smf_p++; ln = smf_vlq();
            if (mt == 0x51 && ln >= 3)
                tempo = ((unsigned long)smf_p[0]<<16)|((unsigned long)smf_p[1]<<8)|smf_p[2];
            smf_p += ln;
        } else if (*smf_p == 0xF0 || *smf_p == 0xF7) {     /* SysEx (F0) or escape (F7) */
            unsigned char lead = *smf_p++; unsigned long ln = smf_vlq(), k;
            if (lead == 0xF0) midi_out(0xF0);
            for (k = 0; k < ln && smf_p < smf_end; k++) midi_out(*smf_p++);
        } else {                                           /* channel voice (+/- running status) */
            int nb;
            if (*smf_p >= 0x80) status = *smf_p++;
            nb = ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) ? 1 : 2;
            midi_out((unsigned char)status);
            while (nb-- && smf_p < smf_end) midi_out(*smf_p++);
        }
    }
    return 1;
}

/* match "/NAME" or "/NAME=val" case-insensitively */
static int sw(const char *a, const char *name, char **val)
{
    int i;
    if (a[0] != '/' && a[0] != '-') return 0;
    for (i = 0; name[i]; i++) { char c = a[1+i]; if (c>='a'&&c<='z') c-=32; if (c != name[i]) return 0; }
    if (a[1+i] == '\0') { *val = 0; return 1; }
    if (a[1+i] == '=') { *val = (char *)a + 1 + i + 1; return 1; }
    return 0;
}

int main(int argc, char **argv)
{
    unsigned mpu = 0x330, irq = 5, sock = 0, memseg = 0xD000, linelvl = 8;
    int off = 0, sgiven = 0, found = 0, any = 0, i, en_line = 0, line_ok = 0;
    int en_reset = 0, en_mt32 = 0, midi_did = -9, wgiven = 0;
    unsigned sysex_len = 0;
    char *midi_file = 0, *midi_name = 0;
    unsigned char lreg;
    unsigned char coridx;
    int ackU, codec_ok;
    unsigned char __far *cor;

    for (i = 1; i < argc; i++) {
        char *a = argv[i], *vp;
        if (a[0] != '/' && a[0] != '-') continue;
        if      (sw(a, "OFF", &vp)) off = 1;
        else if (sw(a, "MT32", &vp)) en_mt32 = 1;
        else if (sw(a, "MIDI", &vp)) { if (vp) midi_file = vp; }
        else if (sw(a, "SYSEX",&vp)) { if (vp) sysex_len = parse_hex(vp, sysexbuf, sizeof(sysexbuf)); }
        else if (sw(a, "RESET",&vp)) en_reset = 1;
        else if (sw(a, "LINE",&vp)) { en_line = 1; if (vp) { linelvl = (unsigned)strtol(vp, 0, 10); if (linelvl > 8) linelvl = 8; } }
        else if (sw(a, "MPU", &vp)) { if (vp) mpu = (unsigned)strtol(vp, 0, 16); }
        else if (sw(a, "I",   &vp)) { if (vp) irq = (unsigned)strtol(vp, 0, 10); }
        else if (sw(a, "S",   &vp)) { if (vp) { sock = (unsigned)strtol(vp, 0, 10); sgiven = 1; } }
        else if (sw(a, "W",   &vp)) { if (vp) { memseg = (unsigned)strtol(vp, 0, 16); wgiven = 1; } }
        else printf("Ignoring unknown switch: %s\n", a);
    }

    /* MPU base <-> COR config index */
    switch (mpu) {
        case 0x330: coridx = 0x01; break;
        case 0x370: coridx = 0x02; break;
        case 0x3B0: coridx = 0x03; break;
        case 0x3F0: coridx = 0x04; break;
        default: printf("Bad /MPU=%03X : use 330 / 370 / 3B0 / 3F0\n", mpu); return 2;
    }
    if (sgiven && sock > MAX_SOCKET) { printf("Bad /S=%u : 0..%u\n", sock, MAX_SOCKET); return 2; }
    if (irq > 15) { printf("Bad /I=%u : 0..15\n", irq); return 2; }

    /* Unless the window was pinned with /W, pick a probe segment no other card
     * has mapped - so the CIS read won't collide with a card already using D000. */
    if (!wgiven) {
        unsigned free = find_free_window(memseg);
        if (free != memseg) { printf("Probe window %04X in use by another card; using %04X.\n", memseg, free); memseg = free; }
    }

    /* locate the SCP-55 */
    if (sgiven) {
        select_socket(sock);
        if (!controller_present()) { printf("No PCIC for socket %u (port %03X)\n", sock, pcic_idx); return 1; }
        found = probe_socket(memseg);
    } else {
        unsigned chip, half;
        for (chip = 0; chip < 4 && !found; chip++) {
            pcic_idx = PCIC_BASE + chip * 2; sockoff = 0;
            if (!controller_present()) continue;
            any = 1;
            for (half = 0; half < 2; half++) {
                sock = chip * 2 + half; select_socket(sock);
                if (probe_socket(memseg)) { found = 1; break; }
            }
        }
        if (!any) { printf("No 82365-class PCIC found (3E0/3E2/3E4/3E6).\n"); return 1; }
    }
    if (!found) {
        if (g_manf) printf("Not the SCP-55 (found MANFID %04X/%04X \"%s\").\n", g_manf, g_card, g_vers);
        else        printf("No Roland SCP-55 found in any socket.\n");
        return 3;
    }

    if (off) {
        wr(0x06, 0x00); wr(0x03, 0x00); wr(0x02, 0x00);
        printf("SCP-55 socket %u powered off.\n", sock);
        return 0;
    }

    MBASE = mpu; CBASE = mpu + 8;

    /* COR (attribute window probe_socket left mapped) */
    cor = (unsigned char __far *)MK_FP(memseg, g_cfg);
    *cor = coridx;

    /* I/O window 0 = base..base+0x0F (MPU at +0/+1, codec at +8..+11).
     * Deliberately does NOT reach 0x201 - no gameport on this unit. */
    wr(0x08, mpu & 0xFF);          wr(0x09, (mpu >> 8) & 0xFF);
    wr(0x0A, (mpu + 0x0F) & 0xFF); wr(0x0B, ((mpu + 0x0F) >> 8) & 0xFF);
    wr(0x07, 0x00);                                    /* 8-bit */
    wr(0x03, irq ? (0x60 | 0x10 | (irq & 0x0F)) : 0x60);   /* I/O mode (+IRQ) */
    wr(0x06, 0x40);                                     /* enable I/O win0; mem win off */

    /* A cold-booted card was only just powered, so the CS4231 is still running
     * its ~20ms power-on calibration and reads 0x80 (busy). Wait it out before
     * touching the mixer, or the un-mute writes get dropped (synth stays muted,
     * codec reads "not responding" until a second run). Poll IAR until != 0x80. */
    { unsigned long w; for (w = 0; w < 600000UL; w++) if (!(inp(CBASE) & 0x80)) break; }

    /* un-mute the CS4231A so the Sound Canvas is audible (powers up muted) */
    ccput(0x06, 0x00); ccput(0x07, 0x00);              /* L/R DAC out: unmute 0dB */
    ccput(0x02, 0x08); ccput(0x03, 0x08);              /* L/R Aux1: unmute 0dB */
    ccput(0x04, 0x08); ccput(0x05, 0x08);              /* L/R Aux2: unmute 0dB */

    /* verify the codec responds by reading back a mixer reg we just wrote
     * (Aux1-L = I2 = 0x08). The IAR index read-back is unreliable on this card,
     * but the register file itself reads back cleanly. Exact rev = CS4231A,
     * pinned via SCPROBE2's MODE2 I25=0xA0. */
    codec_ok = ((ccget(0x02) & 0x1F) == 0x08);

    /* /LINE: pass the analog line-in jack straight through to the output. The
     * CS4231A Line input (I18/I19, MODE2 regs) mixes to the DAC output with no
     * DMA/capture - so a laptop plugged into line-in feeds the card's speakers. */
    if (en_line) {
        /* User level 0..8 (0 = 0dB, 8 = max +12dB). The CS4231 field is inverted
         * (0x00 = +12dB, 0x08 = 0dB, 1.5dB/step), so reg = 0x08 - level. */
        lreg = (unsigned char)(0x08 - linelvl);
        if (cc_mode2()) {
            ccput(0x12, lreg);                         /* I18 Left Line In: unmute */
            ccput(0x13, lreg);                         /* I19 Right Line In: unmute */
            line_ok = ((ccget(0x12) & 0x1F) == lreg) && !(ccget(0x12) & 0x80);
        }
    }

    /* Reset to a known state (its ACK timing varies and a re-run may already be
     * in UART mode, so best-effort), flush any pending bytes, then enter UART
     * mode - the 0x3F ACK is the reliable readiness signal. */
    if (mpu_wr_ready()) outp(MBASE + 1, 0xFF);
    MS(40);
    for (i = 0; i < 16; i++) { if (inp(MBASE + 1) & 0x80) break; (void)inp(MBASE); }
    ackU = mpu_cmd(0x3F);

    /* Set the Sound Canvas mode: /MIDI=file streams a user-supplied setup .MID;
     * /SYSEX=hex sends a raw MIDI/SysEx message; /MT32 reads MT32EMUL.MID;
     * /RESET sends the built-in standard GS reset. */
    if (ackU == 0xFE) {
        if (midi_file || en_mt32) {              /* send a user-supplied setup .MID */
            midi_name = midi_file ? midi_file : "MT32EMUL.MID";
            midi_did = midi_send_file(midi_name);
        } else if (sysex_len) {                  /* raw MIDI/SysEx bytes from /SYSEX */
            midi_send_raw(sysexbuf, sysex_len); midi_did = 4;
        } else if (en_reset) {                   /* embedded standard GS reset */
            midi_send_raw(gs_reset, sizeof(gs_reset)); midi_did = 2;
        }
    }

    printf("GSGO %s - Roland SCP-55: socket %u%s\n", GSGO_VER, sock, sgiven ? "" : " (auto)");
    printf("   CIS \"%s\"  MANFID %04X/%04X  COR @%03X idx %02X\n",
           g_vers, g_manf, g_card, g_cfg, coridx);
    printf("   GS Sound Canvas via MPU-401 @ %03X", mpu);
    if (irq) printf("  IRQ %u", irq); else printf("  (no IRQ)");
    printf("\n   codec CS4231A @ %03X (%s), mixer un-muted\n",
           CBASE, codec_ok ? "responding" : "NOT responding");
    if (ackU == 0xFE)
        printf("   MPU-401 UART ready (ACK 0xFE)\n");
    else
        printf("   WARNING: MPU-401 UART did not ACK (got %d) - MIDI may not respond.\n", ackU);
    if (en_line)
        printf("   line-in pass-through %s (gain %u/8, ~%+d dB)\n",
               line_ok ? "ON" : "requested but NOT confirmed", linelvl, (int)linelvl * 3 / 2);
    if (midi_did == 4)       printf("   Sound Canvas: sent %u-byte SysEx\n", sysex_len);
    else if (midi_did == 2)  printf("   Sound Canvas: GS reset\n");
    else if (midi_did == 1)  printf("   Sound Canvas: sent %s%s\n", midi_name,
                                    (en_mt32 && !midi_file) ? " (MT-32 mode)" : "");
    else if (midi_did == 0)  printf("   %s not found - put it in this folder, or use /MIDI=PATH\n", midi_name);
    else if (midi_did == -1) printf("   %s is not a valid MIDI file\n", midi_name);
    return 0;
}
