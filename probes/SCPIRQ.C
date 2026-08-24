/* SCPIRQ.C - does the SCP-55's IREQ actually reach the host PIC?
 *
 * This is the blocker test for "pump off the card's IRQ instead of the RTC".
 * Every result to date was taken with the card IRQ OFF (/I=0), so the whole
 * idea rests on an assumption nobody has measured: that a PCMCIA sound card's
 * interrupt is routed to an ISA line at all on these hosts.  The REX-5571 work
 * hit exactly this wall - the ES1688's FIFO-half-empty interrupt never reached
 * an ISA line on the PC110.
 *
 * The probe deliberately splits the question in two, because "no interrupts"
 * has two very different causes and only one of them kills the idea:
 *
 *   PHASE A - timer running?   IEN OFF, no ISR.  Program the CS4231A's own
 *             timer and POLL I24 for TI (D6) rolling over.  This proves the
 *             timer works without involving the PIC at all.
 *   PHASE B - interrupt arriving?  Hook the vector, unmask the PIC, set IEN
 *             (I10 D1) and count ISR entries.
 *
 *   A yes / B no  => the timer is fine, the IREQ does not reach the PIC.
 *                    Idea is dead on this host; stay on the RTC.
 *   A yes / B yes => GO.  Size the period to the FIFO drain time.
 *   A no          => the timer itself is misprogrammed; B tells you nothing.
 *
 * Run SCP55GO WITH an IRQ first - the enabler defaults to /I=5 but the bench
 * launchers pass /I=0, so the line is normally NOT routed:
 *     SCP55GO /PCIC /I=5 /W=DC00
 *     SCPIRQ /IRQ=5
 *
 * Register map (proven 2026-08-24, NOT the datasheet defaults):
 *   R0 IAR = base+8   R1 IDR = base+9   R2 STATUS = base+6   R3 PDR = base+7
 * Vendor Windows driver captured mid-playback: I16=C0 (TE+OLB), I20=42
 * (base 66 ~ 660us), I24=40 (TI rolling), I10=02 (IEN) - so this is the
 * configuration the card is known to run in, not a guess.
 *
 * SAFETY: restores I10/I16/I20/I21, the interrupt vector and the PIC mask on
 * every exit path including the keypress abort.  Touches nothing but the codec
 * registers at base+6..+9 - no PCIC access, no I/O window programming.
 *
 * Usage: SCPIRQ [/IRQ=5] [/IO=330] [/TB=66] [/SEC=3]
 */
#include <stdio.h>
#include <conio.h>
#include <string.h>
#include <stdlib.h>
#include <dos.h>
#include <i86.h>

static unsigned BASE = 0x330;
#define IAR (BASE+8)
#define IDR (BASE+9)
#define SRP (BASE+6)
#define PDR (BASE+7)

/* Alternate Feature Enable I (I16) */
#define I16_TE   0x40           /* timer enable            */
#define I16_OLB  0x80           /* output level bit        */
/* Pin Control (I10) */
#define I10_IEN  0x02           /* interrupt enable        */
/* Alternate Feature Status (I24) */
#define I24_PI   0x10           /* playback interrupt      */
#define I24_CI   0x20           /* capture interrupt       */
#define I24_TI   0x40           /* timer interrupt         */

static volatile unsigned long isr_count = 0;
static volatile unsigned char isr_last24 = 0;
static volatile unsigned char isr_or24   = 0;   /* OR of every I24 seen */

static void (__interrupt __far *old_vec)(void);
static unsigned g_irq = 5, g_vec = 0x0D, g_tb = 66, g_sec = 3;
static unsigned g_maskport = 0x21, g_maskbit = 0x20;
static unsigned char old_mask, old_m1;
static int casc = 0;
static unsigned char sv10, sv16, sv20, sv21;
static int hooked = 0, armed = 0;
static unsigned g_iar, g_idr, g_srp;            /* ISR-local copies */

static void iod(unsigned long n){ while (n--) (void)inp(0x80); }
#define MS(x) iod((unsigned long)(x) * 1000UL)

/* ---- ExCA/82365 readback ------------------------------------------------
 * Read-only.  We never WRITE the PCIC here - the enabler owns that - but we
 * do need to see what the chip actually LATCHED, because a bridge that
 * refuses the IRQ nibble looks identical from the card side to a card whose
 * IREQ is not wired.  Index 3E0h / data 3E1h; socket n lives at bank n*40h.
 * Safe only because no Card Services is resident to race the index register.
 */
static int g_sock = -1;
static unsigned char exca_rd(unsigned char bank, unsigned char idx)
{ outp(0x3E0, (unsigned char)(bank + idx)); iod(50); return (unsigned char)inp(0x3E1); }

static void ci_wait(void)
{ unsigned long i; for(i=0;i<400000UL;i++) if(!(inp(IAR)&0x80)) return; }
static void ci_put(unsigned char idx, unsigned char v)
{ ci_wait(); outp(IAR, idx); iod(200); outp(IDR, v); iod(200); }
static unsigned char ci_get(unsigned char idx)
{ ci_wait(); outp(IAR, idx); iod(200); return (unsigned char)inp(IDR); }

/* BIOS tick at 0040:006C, 18.2065 Hz */
static unsigned long bios_ticks(void)
{ unsigned long far *t = (unsigned long far *)0x0040006CUL; return *t; }

/* ---- the ISR ------------------------------------------------------------
 * Kept as short as possible: read the status, clear it, EOI.  Uses only the
 * ISR-local port copies so it never touches the BASE global.
 */
static void __interrupt __far irq_isr(void)
{
    unsigned char s24;
    outp(g_iar, 24);
    s24 = (unsigned char)inp(g_idr);
    isr_last24 = s24;
    isr_or24 = (unsigned char)(isr_or24 | s24);
    /* clear the card's interrupt: status register write, then I24 = 0 */
    outp(g_srp, 0);
    outp(g_iar, 24);
    outp(g_idr, 0);
    isr_count++;
    if (g_irq >= 8) outp(0xA0, 0x20);
    outp(0x20, 0x20);
}

/* ---- PHASE C: the MPU-401's own interrupt -------------------------------
 * Same question, different source.  The SCP-55 is FUNCID 02 (serial port),
 * so if the card wires IREQ# to the UART rather than to the codec, the MPU
 * will interrupt where the codec cannot.  An MPU-401 raises IREQ whenever a
 * byte is waiting to be read - no enable register - and every command is
 * answered with a 0xFE ACK, which is exactly such a byte.
 * MPU data = BASE+0, status/command = BASE+1.
 *   status D7 (80h) = NO data to read     D6 (40h) = NOT ready to write
 */
#define MPU_DA (BASE+0)
#define MPU_ST (BASE+1)

static volatile unsigned long mpu_isr_count = 0;
static volatile unsigned char mpu_isr_byte  = 0;
static volatile int           mpu_isr_got   = 0;
static unsigned g_mda, g_mst;                   /* ISR-local copies */

static void __interrupt __far mpu_isr(void)
{
    int guard = 32;
    while (guard-- && !(inp(g_mst) & 0x80)) {
        mpu_isr_byte = (unsigned char)inp(g_mda);
        mpu_isr_got  = 1;
    }
    mpu_isr_count++;
    if (g_irq >= 8) outp(0xA0, 0x20);
    outp(0x20, 0x20);
}

static void mpu_drain(void)
{ int g = 64; while (g-- && !(inp(MPU_ST) & 0x80)) (void)inp(MPU_DA); }

static int mpu_cmd(unsigned char c)             /* 0 = timed out writing */
{ long i; for (i = 0; i < 100000L; i++) if (!(inp(MPU_ST) & 0x40))
                                        { outp(MPU_ST, c); return 1; }
  return 0; }

static void unprogram(void)
{
    if (armed) {
        ci_put(10, sv10);                    /* IEN off first */
        ci_put(16, sv16);                    /* TE off        */
        ci_put(20, sv20);
        ci_put(21, sv21);
        armed = 0;
    }
}

static void unhook(void)
{
    unsigned char m;
    if (!hooked) return;
    _disable();
    m = (unsigned char)inp(g_maskport);
    outp(g_maskport, (unsigned char)((m & ~g_maskbit) | (old_mask & g_maskbit)));
    if (casc) { m = (unsigned char)inp(0x21);
                outp(0x21, (unsigned char)((m & ~0x04) | (old_m1 & 0x04)));
                casc = 0; }
    _dos_setvect(g_vec, old_vec);
    _enable();
    hooked = 0;
}

static void bail(const char *why)
{
    unprogram();
    unhook();
    printf("\n%s\n", why);
    exit(1);
}

int main(int argc, char **argv)
{
    int i;
    unsigned long t0, t1, polls = 0, ti_seen = 0;
    unsigned char s24, id, r2_or = 0, poll_ack = 0;
    int poll_got = 0;

    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '/' && a[0] != '-') continue;
        if      (!strnicmp(a+1, "IRQ=", 4)) g_irq = (unsigned)atoi(a+5);
        else if (!strnicmp(a+1, "IO=",  3)) BASE  = (unsigned)strtol(a+4, NULL, 16);
        else if (!strnicmp(a+1, "TB=",  3)) g_tb  = (unsigned)atoi(a+4);
        else if (!strnicmp(a+1, "SEC=", 4)) g_sec = (unsigned)atoi(a+5);
        else if (!strnicmp(a+1, "SOCK=",5)) g_sock = atoi(a+6);
        else { printf("Usage: SCPIRQ [/IRQ=5] [/IO=330] [/TB=66] [/SEC=3] [/SOCK=n]\n"); return 1; }
    }
    if (g_irq > 15 || g_irq == 2) { printf("Bad /IRQ (2 is the cascade)\n"); return 1; }
    if (g_sec < 1) g_sec = 1;
    if (g_tb < 1)  g_tb  = 1;

    g_iar = IAR; g_idr = IDR; g_srp = SRP;
    if (g_irq < 8) { g_vec = 0x08 + g_irq; g_maskport = 0x21; g_maskbit = (unsigned)(1 << g_irq); }
    else           { g_vec = 0x70 + (g_irq - 8); g_maskport = 0xA1; g_maskbit = (unsigned)(1 << (g_irq - 8)); }

    printf("SCPIRQ - does the SCP-55 IREQ reach the PIC?\n");
    printf("  codec %03Xh (IAR %03Xh)  IRQ %u (vector %02Xh)  timer base %u  %us/phase\n",
           BASE, IAR, g_irq, g_vec, g_tb, g_sec);

    if ((unsigned char)inp(IAR) == 0xFF)
        { printf("\nNothing at %03Xh - run SCP55GO first.\n", IAR); return 1; }
    id = ci_get(25);
    printf("  codec ID (I25) = %02X\n", id);

    sv10 = ci_get(10); sv16 = ci_get(16); sv20 = ci_get(20); sv21 = ci_get(21);
    printf("  saved I10=%02X I16=%02X I20=%02X I21=%02X\n\n", sv10, sv16, sv20, sv21);

    /* ---------------- PHASE A: timer, polled, IEN OFF -------------------- */
    printf("PHASE A  timer running? (IEN off, polling I24 for TI)\n");
    ci_put(10, (unsigned char)(sv10 & ~I10_IEN));       /* make sure IEN is OFF */
    ci_put(20, (unsigned char)(g_tb & 0xFF));
    ci_put(21, (unsigned char)((g_tb >> 8) & 0xFF));
    ci_put(24, 0);                                       /* clear status */
    ci_put(16, (unsigned char)(sv16 | I16_TE));          /* timer ENABLE */
    armed = 1;

    t0 = bios_ticks();
    t1 = t0 + (unsigned long)(g_sec * 18UL);
    while (bios_ticks() < t1) {
        s24 = ci_get(24);
        polls++;
        if (s24 & I24_TI) { ti_seen++; ci_put(24, 0); }
        if (kbhit()) { (void)getch(); bail("aborted"); }
    }
    printf("  TI seen on %lu of %lu polls   (last I24 = %02X)\n",
           ti_seen, polls, (unsigned char)ci_get(24));
    if (!ti_seen)
        printf("  => TIMER NOT ROLLING OVER. Phase B cannot be interpreted.\n");
    else
        printf("  => timer is running.\n");

    /* ---------------- PHASE B: same timer, but via the PIC --------------- */
    printf("\nPHASE B  interrupt arriving? (vector %02Xh hooked, PIC unmasked)\n", g_vec);
    isr_count = 0; isr_or24 = 0; isr_last24 = 0;

    _disable();
    old_vec  = _dos_getvect(g_vec);
    old_mask = (unsigned char)inp(g_maskport);
    _dos_setvect(g_vec, irq_isr);
    outp(g_maskport, (unsigned char)(old_mask & ~g_maskbit));   /* unmask */
    if (g_irq >= 8) {                       /* slave delivers via IRQ2 cascade */
        old_m1 = (unsigned char)inp(0x21);
        outp(0x21, (unsigned char)(old_m1 & ~0x04));
        casc = 1;
    }
    hooked = 1;
    _enable();

    ci_put(24, 0);
    ci_put(10, (unsigned char)(sv10 | I10_IEN));         /* IEN ON */

    t0 = bios_ticks();
    t1 = t0 + (unsigned long)(g_sec * 18UL);
    while (bios_ticks() < t1) {
        r2_or = (unsigned char)(r2_or | (unsigned char)inp(SRP));
        if (kbhit()) { (void)getch(); bail("aborted"); }
    }

    ci_put(10, (unsigned char)(sv10 & ~I10_IEN));        /* IEN OFF */

    printf("  ISR entries: %lu in ~%us  (%lu/s)\n",
           isr_count, g_sec, isr_count / (unsigned long)g_sec);
    printf("  R2 (status) bits seen while IEN on: %02X   INT(D0)=%c\n",
           r2_or, (r2_or & 0x01) ? 'y' : 'n');
    printf("  I24 bits seen in ISR: %02X  (TI=%c PI=%c CI=%c)   last=%02X\n",
           isr_or24,
           (isr_or24 & I24_TI) ? 'y' : 'n',
           (isr_or24 & I24_PI) ? 'y' : 'n',
           (isr_or24 & I24_CI) ? 'y' : 'n',
           isr_last24);

    unprogram();
    unhook();

    /* ---------------- PHASE C: MPU-401 receive interrupt ----------------- */
    printf("\nPHASE C  does the UART interrupt instead? (MPU-401 at %03Xh)\n", MPU_DA);
    g_mda = MPU_DA; g_mst = MPU_ST;
    mpu_drain();
    mpu_isr_count = 0; mpu_isr_got = 0;

    _disable();
    old_vec  = _dos_getvect(g_vec);
    old_mask = (unsigned char)inp(g_maskport);
    _dos_setvect(g_vec, mpu_isr);
    outp(g_maskport, (unsigned char)(old_mask & ~g_maskbit));
    if (g_irq >= 8) { old_m1 = (unsigned char)inp(0x21);
                      outp(0x21, (unsigned char)(old_m1 & ~0x04)); casc = 1; }
    hooked = 1;
    _enable();

    if (!mpu_cmd(0x3F))                          /* enter UART mode -> ACK */
        printf("  MPU never became write-ready - no UART here?\n");
    t0 = bios_ticks(); t1 = t0 + 18UL;           /* ~1s */
    while (bios_ticks() < t1) {
        if (!(inp(MPU_ST) & 0x80)) { poll_ack = (unsigned char)inp(MPU_DA); poll_got = 1; }
        if (kbhit()) { (void)getch(); bail("aborted"); }
    }
    mpu_cmd(0xFF);                               /* reset back out of UART  */
    t0 = bios_ticks(); t1 = t0 + 18UL;
    while (bios_ticks() < t1) {
        if (!(inp(MPU_ST) & 0x80)) { poll_ack = (unsigned char)inp(MPU_DA); poll_got = 1; }
        if (kbhit()) { (void)getch(); bail("aborted"); }
    }
    unhook();
    mpu_drain();

    printf("  ACK byte seen: %s", (mpu_isr_got || poll_got) ? "yes" : "NO");
    if (mpu_isr_got) printf("  (in ISR, %02X)", mpu_isr_byte);
    if (poll_got)    printf("  (polled, %02X)", poll_ack);
    printf("\n  ISR entries: %lu\n", mpu_isr_count);

    if (g_sock >= 0) {
        unsigned char bank = (unsigned char)(g_sock * 0x40), r;
        printf("\nExCA socket %d (bank %02Xh) readback:\n", g_sock, bank);
        printf("  00 ident=%02X  01 status=%02X  02 power=%02X  03 intgen=%02X\n",
               exca_rd(bank,0), exca_rd(bank,1), exca_rd(bank,2), exca_rd(bank,3));
        printf("  04 cscevt=%02X  05 csccfg=%02X  06 winena=%02X  07 ioctl =%02X\n",
               exca_rd(bank,4), exca_rd(bank,5), exca_rd(bank,6), exca_rd(bank,7));
        r = exca_rd(bank,3);
        printf("  intgen: IRQ nibble=%u  D4=%u  cardtype=%s  reset=%s\n",
               (unsigned)(r & 0x0F), (unsigned)((r >> 4) & 1),
               (r & 0x20) ? "I/O" : "memory", (r & 0x40) ? "released" : "ASSERTED");
        if ((unsigned)(r & 0x0F) != g_irq)
            printf("  => THE BRIDGE DID NOT LATCH IRQ %u (reads %u).\n", g_irq, (unsigned)(r & 0x0F));
        else
            printf("  => IRQ %u latched in the bridge; the card side is not driving it.\n", g_irq);
    }

    printf("\nVERDICT: ");
    if (!ti_seen)          printf("INCONCLUSIVE - timer never rolled over.\n");
    else if (isr_count)    printf("IREQ REACHES THE PIC on IRQ %u. Card-IRQ pump is viable.\n", g_irq);
    else if (!(r2_or & 0x01))
                           printf("timer rolls over but the CODEC never asserts INT (R2 D0).\n"
                                  "         This is codec-side config, NOT the bridge and NOT\n"
                                  "         card wiring - the /INT pin is never driven.\n");
    else if (mpu_isr_count)
                           printf("codec asserts INT but only the UART reaches IRQ %u.\n"
                                  "         => the card wires IREQ# to the MPU-401, NOT to the codec.\n"
                                  "         Card-IRQ pump is dead on the SCP-55 specifically.\n", g_irq);
    else if (mpu_isr_got || poll_got)
                           printf("codec asserts INT, MPU answers, but NEITHER reaches IRQ %u.\n"
                                  "         IREQ# does not get through on this host at all.\n", g_irq);
    else                   printf("codec asserts INT but nothing reaches IRQ %u, and the MPU\n"
                                  "         never answered - phase C inconclusive.\n", g_irq);
    return 0;
}
