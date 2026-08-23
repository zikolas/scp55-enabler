/* SCPTRACE.C - arm the SCPTRACE.VXD port tracer on the Roland SCP-55's
 * window, capture the vendor driver's I/O traffic, dump to C:\SCPTRACE.BIN
 * (raw event dwords) and C:\SCPTRACE.TXT (decoded, one event per line,
 * plus a per-port histogram).
 *
 * Event dword: [7:0]=value [15:8]=port low byte (0x3x => 0x33x,
 * 0xEx => 0x3Ex) [16]=dir 1=OUT [31:17]=ms timestamp (15-bit, wraps 32s).
 *
 * Usage:  SCPTRACE          hook card window + PCIC, trace until keypress
 *         SCPTRACE /PCIC    hook only 0x3E0/0x3E1 (card-init trace: arm,
 *                           then eject/reinsert or Device Manager
 *                           disable->enable so the driver re-runs init)
 *         SCPTRACE /CARD    hook only 0x330-0x33F
 *
 * The ARM hook mask is itself a measurement - see SCPTRACE.ASM.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <conio.h>

#define ST_ARM    1
#define ST_DISARM 2
#define ST_STATUS 3
#define ST_READ   4
#define ST_RESET  5
#define MAXEV     4096

#define M_CARD    0x0000FFFFUL          /* portlist bits 0..15  = 330-33F */
#define M_PCIC    0x00030000UL          /* portlist bits 16,17  = 3E0/3E1 */
#define M_ALL     (M_CARD | M_PCIC)

static HANDLE hVxd = INVALID_HANDLE_VALUE;
static DWORD  evbuf[MAXEV];
static DWORD  hist[0x120];              /* per-port event counts */

static int ioctl_dw(DWORD code, DWORD *out)
{
    DWORD n = 0, val = 0;
    if (!DeviceIoControl(hVxd, code, NULL, 0, &val, 4, &n, NULL))
        return -1;
    if (out) *out = val;
    return 0;
}

static int ioctl_arm(DWORD req, DWORD *out)
{
    DWORD n = 0, val = 0;
    if (!DeviceIoControl(hVxd, ST_ARM, &req, 4, &val, 4, &n, NULL))
        return -1;
    if (out) *out = val;
    return 0;
}

static HANDLE load_vxd(void)
{
    char exe[MAX_PATH], path[MAX_PATH + 16];
    char *p;
    HANDLE h;

    GetModuleFileName(NULL, exe, sizeof exe);
    p = strrchr(exe, '\\');
    if (p) p[1] = 0;

    sprintf(path, "\\\\.\\%sSCPTRACE.VXD", exe);
    h = CreateFile(path, 0, 0, NULL, 0, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h != INVALID_HANDLE_VALUE) return h;
    printf("  load try 1 (%s): err %lu\n", path, GetLastError());

    h = CreateFile("\\\\.\\SCPTRACE.VXD", 0, 0, NULL, 0,
                   FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h != INVALID_HANDLE_VALUE) return h;
    printf("  load try 2 (\\\\.\\SCPTRACE.VXD): err %lu\n", GetLastError());
    return INVALID_HANDLE_VALUE;
}

/* ---- what lives at each card port (from SCPROBE2 + the CIS) ---------- */
static const char *portname(unsigned port)
{
    switch (port) {
    case 0x330: return "MPU data ";
    case 0x331: return "MPU stat ";
    case 0x332: return "GLUE +2  ";
    case 0x333: return "GLUE +3  ";
    case 0x334: return "GLUE +4  ";
    case 0x335: return "GLUE +5  ";
    case 0x336: return "GLUE +6  ";
    case 0x337: return "GLUE +7  ";
    case 0x338: return "cs IAR   ";
    case 0x339: return "cs IDR   ";
    case 0x33A: return "cs STATUS";
    case 0x33B: return "cs PDR   ";
    case 0x3E0: return "PCIC idx ";
    case 0x3E1: return "PCIC dat ";
    }
    if (port >= 0x33C && port <= 0x33F) return "(past win)";
    return "?        ";
}

/* ---- 82365SL register names; memory-window regs are flagged loudly --- */
static const char *pcicname(unsigned idx, int *ismem)
{
    static char buf[32];
    unsigned r = idx & 0x3F;
    const char *nm;
    unsigned win, fld;

    *ismem = 0;
    switch (r) {
    case 0x00: nm = "IDENT";        break;
    case 0x01: nm = "IF-STATUS";    break;
    case 0x02: nm = "POWER";        break;
    case 0x03: nm = "INT-GEN";      break;
    case 0x04: nm = "CSC-STATUS";   break;
    case 0x05: nm = "CSC-CFG";      break;
    case 0x06: nm = "WIN-ENABLE";   break;
    case 0x07: nm = "IO-CTL";       break;
    case 0x16: nm = "CD/GEN-CTL";   break;
    case 0x1E: nm = "GLOBAL-CTL";   break;
    default:   nm = NULL;           break;
    }
    if (nm) {
        sprintf(buf, "%s", nm);
        return buf;
    }
    if (r >= 0x08 && r <= 0x0F) {
        static const char *iof[8] = { "IO0-START-LO", "IO0-START-HI",
                                      "IO0-STOP-LO",  "IO0-STOP-HI",
                                      "IO1-START-LO", "IO1-START-HI",
                                      "IO1-STOP-LO",  "IO1-STOP-HI" };
        sprintf(buf, "%s", iof[r - 0x08]);
        return buf;
    }
    if (r >= 0x10 && r <= 0x35 && (r & 7) < 6) {
        static const char *mf[6] = { "START-LO", "START-HI", "STOP-LO",
                                     "STOP-HI",  "OFF-LO",   "OFF-HI" };
        win = (r - 0x10) >> 3;
        fld = r & 7;
        *ismem = 1;
        sprintf(buf, "MEMWIN%u-%s", win, mf[fld]);
        return buf;
    }
    sprintf(buf, "idx%02X", r);
    return buf;
}

int main(int argc, char **argv)
{
    DWORD req = M_ALL, mask = 0, stat = 0, n = 0, count, i;
    int   memwin_seen = 0;
    const char *what = "card window 330-33F + PCIC 3E0/3E1";
    FILE *f;

    for (i = 1; (int)i < argc; i++) {
        if (!stricmp(argv[i], "/PCIC")) {
            req = M_PCIC; what = "PCIC 3E0/3E1 only (card-init trace)";
        } else if (!stricmp(argv[i], "/CARD")) {
            req = M_CARD; what = "card window 330-33F only";
        } else {
            printf("usage: SCPTRACE [/PCIC | /CARD]\n");
            return 1;
        }
    }

    printf("SCPTRACE - Roland SCP-55 port trace: %s\n", what);
    hVxd = load_vxd();
    if (hVxd == INVALID_HANDLE_VALUE) {
        printf("cannot load SCPTRACE.VXD\n");
        return 1;
    }

    ioctl_dw(ST_RESET, NULL);
    if (ioctl_arm(req, &mask) != 0) {
        printf("ARM ioctl failed\n");
        return 1;
    }
    printf("armed. requested %05lX  hooked %05lX\n", req, mask);
    printf("  (bits 0..15 = ports 330..33F, bit16 = 3E0, bit17 = 3E1)\n");

    if ((req & M_CARD) && (mask & M_CARD) != M_CARD) {
        printf("\n*** card-window ports NOT all hooked (%04lX of FFFF) ***\n",
               mask & M_CARD);
        printf("    another VxD owns them - on this bench that means the\n"
               "    vendor driver does the wave I/O at ring 0 and ring-3\n"
               "    trapping cannot see it.  Escape hatches: install the\n"
               "    Win3.1 scp55.drv (no VxD) as the Win98 wave driver, or\n"
               "    fall back to CR4.DE debug-register I/O breakpoints.\n");
    }
    if ((req & M_PCIC) && (mask & M_PCIC) != M_PCIC)
        printf("NOTE: 3E0/3E1 not hooked - the PCMCIA stack owns them.\n");

    /* Persist the mask immediately: it is a result in its own right, and
     * it survives even if nothing else happens. */
    f = fopen("C:\\SCPTRACE.TXT", "w");
    if (f) {
        fprintf(f, "requested   = %05lX\nhooked      = %05lX\n", req, mask);
        fprintf(f, "cardports   = %s\n",
                ((mask & M_CARD) == M_CARD) ? "ALL HOOKED"
                                            : "PARTIAL - owned elsewhere");
        fclose(f);
    }

    if (req == M_PCIC)
        printf("\n>>> now make the driver re-run card init: eject+reinsert\n"
               "    the SCP-55, or Device Manager disable -> enable it.\n"
               "    Then press any key. <<<\n\n");
    else
        printf("\n>>> now play a short wave file (a few seconds),\n"
               "    then press any key to stop capture <<<\n\n");
    getch();

    ioctl_dw(ST_STATUS, &stat);
    count = stat & 0x7FFFFFFFUL;
    printf("events captured: %lu%s\n", count,
           (stat & 0x80000000UL) ? "  (buffer full - first N kept)" : "");

    if (!DeviceIoControl(hVxd, ST_READ, NULL, 0, evbuf, sizeof evbuf, &n, NULL))
        printf("READ ioctl failed\n");
    else {
        unsigned lastidx = 0xFFFF;
        count = n / 4;
        f = fopen("C:\\SCPTRACE.BIN", "wb");
        if (f) { fwrite(evbuf, 4, count, f); fclose(f); }

        memset(hist, 0, sizeof hist);
        f = fopen("C:\\SCPTRACE.TXT", "w");
        if (f) {
            fprintf(f, "requested   = %05lX\nhooked      = %05lX\n", req, mask);
            fprintf(f, "cardports   = %s\n",
                    ((mask & M_CARD) == M_CARD) ? "ALL HOOKED"
                                                : "PARTIAL - owned elsewhere");
            fprintf(f, "events      = %lu%s\n", count,
                    (stat & 0x80000000UL) ? "  (BUFFER FULL)" : "");
            fprintf(f, "--- idx port name      dir val t(ms)  note ---\n");
            for (i = 0; i < count; i++) {
                DWORD e = evbuf[i];
                unsigned port = 0x300 | ((e >> 8) & 0xFF);
                unsigned val  = (unsigned)(e & 0xFF);
                int isout = (e & 0x10000UL) != 0;
                char note[64];

                note[0] = 0;
                if (port == 0x3E0 && isout) lastidx = val;
                else if (port == 0x3E1 && lastidx != 0xFFFF) {
                    int ismem = 0;
                    const char *nm = pcicname(lastidx, &ismem);
                    sprintf(note, "sock%u %s%s", (lastidx >> 6) & 1, nm,
                            ismem ? "  <<< MEM WINDOW" : "");
                    if (ismem && isout) memwin_seen = 1;
                }
                if (port <= 0x33F || port >= 0x3E0)
                    hist[port - 0x300]++;

                fprintf(f, "%5lu %04X %s %s %02X t=%lu  %s\n",
                        i, port, portname(port), isout ? "OUT" : "IN ",
                        val, (e >> 17) & 0x7FFF, note);
            }
            fprintf(f, "\n--- per-port totals ---\n");
            for (i = 0; i < 0x120; i++)
                if (hist[i])
                    fprintf(f, "  %04X %s %lu\n", (unsigned)(0x300 + i),
                            portname(0x300 + (unsigned)i), hist[i]);
            if (memwin_seen)
                fprintf(f, "\n*** PCIC MEMORY-WINDOW writes seen - the driver "
                           "maps card memory.\n    Cross-check against the "
                           "512-byte function-specific common\n    memory the "
                           "CIS declares (CISTPL_DEVICE = D9 00).\n");
            fclose(f);
        }
        printf("wrote C:\\SCPTRACE.BIN + C:\\SCPTRACE.TXT (%lu events)\n",
               count);
        if (memwin_seen)
            printf("*** PCIC memory-window writes seen - see the TXT ***\n");
    }

    ioctl_dw(ST_DISARM, NULL);
    CloseHandle(hVxd);
    printf("disarmed. done.\n");
    return 0;
}
