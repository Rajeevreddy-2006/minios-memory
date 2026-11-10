/* Offline page-replacement analyser: replays a reference string against N frames.
 * Semantics deliberately mirror the live VMM in mm.c (lowest free frame first,
 * reference bit set on load, same Clock hand rules) so the two can be
 * cross-checked in tests. */
#include "mm.h"

#define MAX_SIM_FRAMES 1024

const char *page_algo_name(page_algo_t a)
{
    static const char *n[] = { "FIFO", "LRU", "Clock", "OPT" };
    return (a >= 0 && a <= ALGO_OPT) ? n[a] : "?";
}

int pagesim_faults(page_algo_t algo, int nframes, const int *refs, int n)
{
    if (nframes <= 0 || nframes > MAX_SIM_FRAMES || n < 0 || (!refs && n)) return -1;

    int  page[MAX_SIM_FRAMES], refbit[MAX_SIM_FRAMES];
    long loaded[MAX_SIM_FRAMES], used[MAX_SIM_FRAMES];
    for (int i = 0; i < nframes; i++) page[i] = -1;

    int hand = 0, faults = 0;
    for (int i = 0; i < n; i++) {
        int hit = -1;
        for (int j = 0; j < nframes; j++)
            if (page[j] == refs[i]) { hit = j; break; }
        if (hit >= 0) { used[hit] = i; refbit[hit] = 1; continue; }

        faults++;
        int slot = -1;
        for (int j = 0; j < nframes; j++)
            if (page[j] == -1) { slot = j; break; }

        if (slot < 0) {
            switch (algo) {
            case ALGO_FIFO:
                slot = 0;
                for (int j = 1; j < nframes; j++) if (loaded[j] < loaded[slot]) slot = j;
                break;
            case ALGO_LRU:
                slot = 0;
                for (int j = 1; j < nframes; j++) if (used[j] < used[slot]) slot = j;
                break;
            case ALGO_CLOCK:
                while (refbit[hand]) { refbit[hand] = 0; hand = (hand + 1) % nframes; }
                slot = hand;
                hand = (hand + 1) % nframes;
                break;
            case ALGO_OPT: {
                int farthest = -1;
                for (int j = 0; j < nframes; j++) {
                    int next = n + 1;                       /* never used again */
                    for (int k = i + 1; k < n; k++)
                        if (refs[k] == page[j]) { next = k; break; }
                    if (next > farthest) { farthest = next; slot = j; }
                }
                break;
            }
            }
        }
        page[slot] = refs[i];
        loaded[slot] = used[slot] = i;
        refbit[slot] = 1;
    }
    return faults;
}
