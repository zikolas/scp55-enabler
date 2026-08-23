/* MKWAV.C - write C:\LONG.WAV, a long steady tone, so the vendor driver can be
 * caught mid-playback by SCPGLUE /SNAP.  Integer triangle wave only: the PC110
 * is a 486SX with no FPU, so nothing here touches floating point.
 * 11025 Hz, 8-bit unsigned mono, 30 s = ~331 KB.
 */
#include <stdio.h>

#define RATE 11025L
#define SECS 30L

static void w32(FILE *f, unsigned long v)
{ fputc((int)(v & 0xFF), f); fputc((int)((v >> 8) & 0xFF), f);
  fputc((int)((v >> 16) & 0xFF), f); fputc((int)((v >> 24) & 0xFF), f); }
static void w16(FILE *f, unsigned v)
{ fputc((int)(v & 0xFF), f); fputc((int)((v >> 8) & 0xFF), f); }

int main(void)
{
    unsigned long n = RATE * SECS, i;
    int v = 0, d = 10;
    FILE *f = fopen("C:\\LONG.WAV", "wb");

    if (!f) { printf("cannot create C:\\LONG.WAV\n"); return 1; }
    fwrite("RIFF", 1, 4, f); w32(f, 36 + n); fwrite("WAVEfmt ", 1, 8, f);
    w32(f, 16); w16(f, 1); w16(f, 1);          /* PCM, mono */
    w32(f, RATE); w32(f, RATE);                /* rate, bytes/sec */
    w16(f, 1); w16(f, 8);                      /* block align, bits */
    fwrite("data", 1, 4, f); w32(f, n);

    for (i = 0; i < n; i++) {
        v += d;
        if (v >  100) { v =  100; d = -d; }
        if (v < -100) { v = -100; d = -d; }
        fputc(128 + v, f);
    }
    fclose(f);
    printf("wrote C:\\LONG.WAV  %lu Hz 8-bit mono, %lu s, %lu bytes\n",
           RATE, SECS, n);
    return 0;
}
