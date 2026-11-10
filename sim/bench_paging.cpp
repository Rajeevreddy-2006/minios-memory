// Page-replacement benchmark: FIFO vs LRU vs Clock vs OPT on reference strings,
// plus a live run through the real page-fault path. Run with `make bench`.
extern "C" {
#include "mm.h"
}
#include <cstdio>
#include <vector>

static unsigned rng_state = 2026;
static int rnd(int lo, int hi)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return lo + (int)((rng_state >> 8) % (unsigned)(hi - lo + 1));
}

static std::vector<int> make_refs(int kind, int n)
{
    std::vector<int> r;
    for (int i = 0; i < n; i++) {
        switch (kind) {
        case 0:  r.push_back(i % 40); break;                                   // loop over 40 pages
        case 1:  r.push_back(rnd(0, 9) < 8 ? rnd(0, 7) : rnd(8, 63)); break;   // 80% to 8 hot pages
        default: r.push_back(rnd(0, 63)); break;                               // uniform random
        }
    }
    return r;
}

int main()
{
    std::printf("=============== PAGE REPLACEMENT ===============\n");

    const int belady[] = { 1, 2, 3, 4, 1, 2, 5, 1, 2, 3, 4, 5 };
    std::printf("\nBelady's anomaly, reference string 1 2 3 4 1 2 5 1 2 3 4 5 (page faults)\n");
    std::printf("  frames:  ");
    for (int f = 1; f <= 7; f++) std::printf("%4d", f);
    for (page_algo_t a : { ALGO_FIFO, ALGO_LRU, ALGO_OPT }) {
        std::printf("\n  %-8s ", page_algo_name(a));
        for (int f = 1; f <= 7; f++) std::printf("%4d", pagesim_faults(a, f, belady, 12));
    }
    std::printf("\n  (FIFO goes from 9 faults at 3 frames to 10 at 4: more memory, more faults)\n");

    const char *names[] = { "Loop over 40 pages (sequential scan)",
                            "Locality: 80% of references hit 8 hot pages (of 64)",
                            "Uniform random over 64 pages" };
    const int frames[] = { 4, 8, 16, 32, 48 };
    for (int kind = 0; kind < 3; kind++) {
        auto refs = make_refs(kind, 4000);
        std::printf("\n%s, 4000 references, fault rate %%\n", names[kind]);
        std::printf("  %-8s", "frames:");
        for (int f : frames) std::printf("%8d", f);
        std::printf("\n");
        for (page_algo_t a : { ALGO_FIFO, ALGO_LRU, ALGO_CLOCK, ALGO_OPT }) {
            std::printf("  %-8s", page_algo_name(a));
            for (int f : frames)
                std::printf("%7.1f%%", 100.0 * pagesim_faults(a, f, refs.data(), (int)refs.size()) / refs.size());
            std::printf("\n");
        }
    }

    // live kernel: real page tables, swap I/O, 256 virtual pages on 32 frames
    std::printf("\nLive kernel run: 256 virtual pages on 32 frames, locality workload, 20000 accesses\n");
    std::printf("  %-8s %8s %8s %8s %10s %11s\n",
                "policy", "faults", "minor", "major", "evictions", "swap writes");
    struct { repl_policy_t p; const char *n; } pol[] = {
        { REPL_FIFO, "FIFO" }, { REPL_LRU, "LRU" }, { REPL_CLOCK, "Clock" } };
    for (auto &pr : pol) {
        rng_state = 7;
        mm_init(32, 1024, pr.p);
        aspace_t *as = vm_create_aspace();
        const uint32_t base = 0x10000000;
        vm_map(as, base, 256, PTE_WRITE | PTE_USER);
        for (int i = 0; i < 20000; i++) {
            int page = rnd(0, 9) < 8 ? rnd(0, 15) : rnd(16, 255);
            vm_touch(as, base + (uint32_t)page * PAGE_SIZE, rnd(0, 3) == 0);
        }
        vm_stats_t s = vm_get_stats();
        std::printf("  %-8s %8llu %8llu %8llu %10llu %11llu\n", pr.n,
                    (unsigned long long)s.faults, (unsigned long long)s.minor_faults,
                    (unsigned long long)s.major_faults, (unsigned long long)s.evictions,
                    (unsigned long long)s.swap_writes);
        vm_destroy_aspace(as);
        mm_shutdown();
    }
    std::printf("\n");
    return 0;
}
