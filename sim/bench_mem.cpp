// MiniOS memory benchmark (build with `make bench_mem`; no sanitizers, so timings are real).
//   1. kernel heap: kmalloc/kfree (buddy + slab) vs glibc malloc, and buddy fragmentation
//      (runs first so glibc's heap is pristine and its footprint can be measured)
//   2. compressor: ratio and speed on different kinds of page data
//   3. "effective RAM": same workload, same RAM budget, with and without a compressed pool
extern "C" {
#include "mm.h"
#include "lz4.h"
#include "slab.h"
#include "buddy.h"
}
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <malloc.h>

typedef std::chrono::steady_clock Clock;
static double secs(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double>(b - a).count();
}

static unsigned rng_state = 1;
static unsigned rnd32()
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}
static int rnd(int lo, int hi) { return lo + (int)(rnd32() % (unsigned)(hi - lo + 1)); }

static const uint32_t PG = 4096;

// ------------------------------------------------------------- page data ----

enum Kind { K_ZERO, K_SAME, K_SPARSE, K_RECORDS, K_PROSE, K_POINTERS, K_RANDOM, K_COUNT };
static const char *kind_name[K_COUNT] = {
    "zero page", "same byte (0xAB)", "sparse (~5% data)", "16-byte records",
    "prose (64-word vocab)", "pointer array", "random bytes"
};

static void gen(int kind, uint32_t seed, uint8_t *buf)
{
    unsigned save = rng_state;
    rng_state = seed * 2654435761u + 99u;
    switch (kind) {
    case K_ZERO: std::memset(buf, 0, PG); break;
    case K_SAME: std::memset(buf, 0xAB, PG); break;
    case K_SPARSE:
        std::memset(buf, 0, PG);
        for (int i = 0; i < 200; i++) buf[rnd32() % PG] = (uint8_t)rnd32();
        break;
    case K_RECORDS:
        for (uint32_t i = 0; i < PG; i += 16) {
            std::memcpy(buf + i, "RECORD01", 8);
            uint32_t c = seed * 1000 + i / 16;
            std::memcpy(buf + i + 8, &c, 4);
            std::memset(buf + i + 12, 0, 4);
        }
        break;
    case K_PROSE: {
        static char vocab[64][9];
        static bool init = false;
        if (!init) {
            unsigned sv = rng_state; rng_state = 777;
            for (auto &w : vocab) {
                int len = rnd(2, 8);
                for (int k = 0; k < len; k++) w[k] = (char)('a' + rnd(0, 25));
                w[len] = 0;
            }
            rng_state = sv; init = true;
        }
        uint32_t pos = 0;
        while (pos < PG) {
            const char *w = vocab[rnd32() % 64];
            for (; *w && pos < PG; w++) buf[pos++] = (uint8_t)*w;
            if (pos < PG) buf[pos++] = ' ';
        }
        break;
    }
    case K_POINTERS:
        for (uint32_t i = 0; i < PG; i += 8) {
            uint64_t p = 0x00007f3a00000000ull + ((uint64_t)(rnd32() & 0x3FFFF) << 3);
            std::memcpy(buf + i, &p, 8);
        }
        break;
    default:
        for (uint32_t i = 0; i < PG; i++) buf[i] = (uint8_t)rnd32();
    }
    rng_state = save;
}

// ---------------------------------------------------- 1. compressor ----------

static void section_compression()
{
    std::printf("\n\n=============== 2. COMPRESSOR ON 4 KiB PAGES ===============\n");
    std::printf("  %-22s %9s %7s %9s %12s %12s\n",
                "page contents", "avg bytes", "ratio", "pool ok", "compress", "decompress");
    const int NP = 256, REPS = 20;
    std::vector<uint8_t> in((size_t)NP * PG), scratch(PG);
    volatile size_t sink = 0;

    for (int k = 0; k < K_COUNT; k++) {
        for (int p = 0; p < NP; p++) gen(k, (uint32_t)p, &in[(size_t)p * PG]);

        if (k == K_ZERO || k == K_SAME) {
            std::printf("  %-22s %9s %7s %9s %12s %12s\n", kind_name[k], "8 (same-fill)",
                        "512x", "100%", "no compress", "memset");
            continue;
        }
        std::vector<std::vector<uint8_t>> blobs((size_t)NP);
        size_t total = 0;
        int accepted = 0;
        for (int p = 0; p < NP; p++) {
            blobs[p].resize((size_t)lz4_compress_bound((int)PG));
            int c = lz4_compress(&in[(size_t)p * PG], (int)PG, blobs[p].data(), (int)blobs[p].size());
            blobs[p].resize((size_t)c);
            total += (size_t)c;
            accepted += c <= 2048;
        }
        auto t0 = Clock::now();
        for (int r = 0; r < REPS; r++)
            for (int p = 0; p < NP; p++)
                sink += (size_t)lz4_compress(&in[(size_t)p * PG], (int)PG, scratch.data(), (int)PG);
        auto t1 = Clock::now();
        for (int r = 0; r < REPS; r++)
            for (int p = 0; p < NP; p++)
                sink += (size_t)lz4_decompress(blobs[p].data(), (int)blobs[p].size(), scratch.data(), (int)PG);
        auto t2 = Clock::now();

        double mb = (double)NP * REPS * PG / 1e6;
        double avg = (double)total / NP;
        if (avg > 4000)       // incompressible: stored as literals, so "decompression" is just a memcpy
            std::printf("  %-22s %9.0f %6.2fx %8.0f%% %8.0f MB/s %12s\n", kind_name[k], avg,
                        PG / avg, 100.0 * accepted / NP, mb / secs(t0, t1), "(stored raw)");
        else
            std::printf("  %-22s %9.0f %6.2fx %8.0f%% %8.0f MB/s %8.0f MB/s\n", kind_name[k], avg,
                        PG / avg, 100.0 * accepted / NP, mb / secs(t0, t1), mb / secs(t1, t2));
    }
    std::printf("  ('pool ok' = compresses to <= 2048 B, the pool's acceptance limit)\n");
    (void)sink;
}

// ---------------------------------------------- 2. effective RAM experiment ----

static const uint32_t TOTAL_FRAMES = 256;     // the fixed RAM budget (1 MiB of 4 KiB frames)
static const uint32_t VPAGES = 400;           // working data: 1.56x the RAM budget
static const uint32_t HOT = 96;
static const int OPS = 150000;
static const uint32_t BASE = 0x10000000;

static int kind_of_page(uint32_t p)
{
    unsigned h = (p * 2654435761u) >> 16;
    unsigned r = h % 100;
    if (r < 20) return K_ZERO;
    if (r < 35) return K_SPARSE;
    if (r < 50) return K_RECORDS;
    if (r < 65) return K_PROSE;
    if (r < 75) return K_POINTERS;
    if (r < 80) return K_SAME;
    return K_RANDOM;
}

struct Result {
    uint32_t pool, user, resident;
    vm_stats_t s;
    zpool_stats_t z;
    double ssd_s, hdd_s, cpu_ms;
};

static Result run_config(uint32_t pool)
{
    Result r;
    std::memset(&r, 0, sizeof r);
    mm_config_t c = { TOTAL_FRAMES, 4096, REPL_CLOCK, pool, 0, 0 };
    if (mm_init_ex(&c) != VM_OK) { std::printf("init failed\n"); std::exit(1); }
    aspace_t *as = vm_create_aspace();
    vm_map(as, BASE, VPAGES, PTE_WRITE | PTE_USER);

    uint8_t buf[PG];
    for (uint32_t p = 0; p < VPAGES; p++) {
        gen(kind_of_page(p), p, buf);
        vm_write(as, BASE + p * PG, buf, PG);
    }
    std::vector<uint32_t> perm(VPAGES);
    for (uint32_t i = 0; i < VPAGES; i++) perm[i] = i;
    rng_state = 4242;
    for (uint32_t i = VPAGES - 1; i > 0; i--) std::swap(perm[i], perm[rnd32() % (i + 1)]);

    vm_reset_stats();                         // measure only the steady-state access phase
    rng_state = 99;
    for (int op = 0; op < OPS; op++) {
        uint32_t page = rnd(0, 99) < 85 ? perm[rnd32() % HOT] : perm[HOT + rnd32() % (VPAGES - HOT)];
        uint32_t va = BASE + page * PG + (rnd32() % 4000);
        if (rnd(0, 3) == 0) { uint64_t v = (uint64_t)op; vm_write(as, va, &v, 8); }
        else vm_touch(as, va, 0);
    }

    r.pool = pool;
    r.user = TOTAL_FRAMES - pool;
    r.s = vm_get_stats();
    r.z = zpool_get_stats();
    r.resident = vm_resident_pages(as);
    r.cpu_ms = (double)(r.z.compress_ns + r.z.decompress_ns) / 1e6;
    double disk = (double)(r.s.swap_reads + r.s.swap_writes);
    r.ssd_s = disk * 100e-6 + r.cpu_ms / 1e3;          // modelled: 100 us per disk I/O
    r.hdd_s = disk * 8e-3   + r.cpu_ms / 1e3;          // modelled: 8 ms per disk I/O
    vm_destroy_aspace(as);
    mm_shutdown();
    return r;
}

static void section_effective_ram()
{
    std::printf("\n\n=============== 3. EFFECTIVE RAM: SAME BUDGET, WITH AND WITHOUT A COMPRESSED POOL ===============\n");
    std::printf("RAM budget: %u frames (%u KiB). Data: %u pages (%.2fx the budget): 20%% zero, 5%% same-byte, 15%% sparse,\n"
                "15%% records, 15%% prose, 10%% pointer arrays, 20%% random. %d accesses, 85%% to a %u-page hot set, 25%% writes.\n"
                "Pool frames are carved OUT of the budget, so every pool frame is one fewer resident page.\n\n",
                TOTAL_FRAMES, TOTAL_FRAMES * 4, VPAGES, (double)VPAGES / TOTAL_FRAMES, OPS, HOT);

    std::printf("  %-5s %-6s %9s %9s %9s %9s %9s %10s %9s %9s\n", "pool", "user", "disk rd", "disk wr",
                "pool hit", "writeback", "held pgs", "capacity", "SSD sec", "HDD sec");
    std::vector<Result> res;
    for (uint32_t pool : { 0u, 16u, 32u, 48u, 64u, 96u, 128u }) res.push_back(run_config(pool));

    for (auto &r : res) {
        uint32_t held = r.resident + r.z.live_entries;
        std::printf("  %-5u %-6u %9llu %9llu %9llu %9llu %9u %9.2fx %9.2f %9.1f\n", r.pool, r.user,
                    (unsigned long long)r.s.swap_reads, (unsigned long long)r.s.swap_writes,
                    (unsigned long long)r.s.zpool_faults, (unsigned long long)r.z.writebacks,
                    held, (double)held / TOTAL_FRAMES, r.ssd_s, r.hdd_s);
    }

    size_t bs = 0, bh = 0;
    for (size_t i = 1; i < res.size(); i++) {
        if (res[i].ssd_s < res[bs].ssd_s || bs == 0) bs = i;
        if (res[i].hdd_s < res[bh].hdd_s || bh == 0) bh = i;
    }
    const Result &b = res[0], &s = res[bs], &h = res[bh];
    std::printf("\n  Best pool size on SSD: %u frames -> %.2f s vs %.2f s without compression (%.1fx faster)\n",
                s.pool, s.ssd_s, b.ssd_s, b.ssd_s / s.ssd_s);
    std::printf("  Best pool size on HDD: %u frames -> %.1f s vs %.1f s without compression (%.1fx faster)\n",
                h.pool, h.hdd_s, b.hdd_s, b.hdd_s / h.hdd_s);
    std::printf("  Disk I/O at that SSD optimum: %llu ops vs %llu (%.0f%% fewer); pool compression ratio %.2fx\n",
                (unsigned long long)(s.s.swap_reads + s.s.swap_writes),
                (unsigned long long)(b.s.swap_reads + b.s.swap_writes),
                100.0 * (1.0 - (double)(s.s.swap_reads + s.s.swap_writes) /
                               (double)(b.s.swap_reads + b.s.swap_writes)),
                s.z.bytes_out ? (double)s.z.bytes_in / (double)s.z.bytes_out : 0.0);
    std::printf("  CPU spent compressing + decompressing at that point: %.1f ms (measured)\n", s.cpu_ms);
    std::printf("  Note: times are MODELLED as disk_ops x latency (100 us SSD, 8 ms HDD) + measured\n"
                "  compression CPU time. 'capacity' = (resident + pooled pages) / RAM budget.\n");
}

// ------------------------------------------------- 3. kernel heap benchmark ----

static void section_allocators()
{
    std::printf("=============== 1. KERNEL HEAP: kmalloc (buddy + slab) vs glibc malloc ===============\n");
    const int SLOTS = 40000, NOPS = 3000000;
    const uint16_t sizes[] = { 24, 40, 64, 100, 160, 256, 400, 700, 1000 };
    struct Op { uint32_t slot; uint16_t size; };
    std::vector<Op> ops((size_t)NOPS);
    std::vector<char> live(SLOTS, 0);
    rng_state = 31;
    for (auto &o : ops) {
        o.slot = rnd32() % SLOTS;
        o.size = sizes[rnd32() % 9];
    }

    // --- glibc (measured first: any earlier large alloc/free would leave slack in its arena)
    std::vector<void *> mp(SLOTS, nullptr);
    std::vector<uint16_t> msz(SLOTS, 0);
    size_t mlive = 0;
    struct mallinfo2 m0 = mallinfo2();                   // baseline, so only this loop's growth is counted
    auto g0 = Clock::now();
    for (auto &o : ops) {
        if (!mp[o.slot]) {
            void *p = std::malloc(o.size);
            *(uint8_t *)p = 1;
            mp[o.slot] = p; msz[o.slot] = o.size; mlive += o.size;
        } else {
            std::free(mp[o.slot]);
            mlive -= msz[o.slot];
            mp[o.slot] = nullptr;
        }
    }
    auto g1 = Clock::now();
    struct mallinfo2 mi = mallinfo2();
    double glibc_ns = secs(g0, g1) / NOPS * 1e9;
    double glibc_over = (double)((mi.arena + mi.hblkhd) - (m0.arena + m0.hblkhd)) / (double)mlive;
    double glibc_obj  = (double)(mi.uordblks - m0.uordblks) / (double)mlive;   // chunk headers + rounding
    for (int i = 0; i < SLOTS; i++) std::free(mp[i]);

    // --- ours (own scope, so the 64 MiB simulated heap is released before glibc is measured)
    double ours_ns = 0, ours_over = 0, ours_obj = 0;
    size_t failed = 0;
    {
    const uint32_t HEAP_FRAMES = 16384;                  // 64 MiB simulated kernel heap
    std::vector<uint8_t> heap((size_t)HEAP_FRAMES * PG);
    buddy_t *b = buddy_create(HEAP_FRAMES);
    slab_ctx_t *x = slab_ctx_create(b, heap.data());
    std::vector<void *> ptr(SLOTS, nullptr);
    std::vector<uint16_t> psz(SLOTS, 0);
    size_t live_bytes = 0;
    auto t0 = Clock::now();
    for (auto &o : ops) {
        if (!ptr[o.slot]) {
            void *p = kmalloc(x, o.size);
            if (!p) { failed++; continue; }
            *(uint8_t *)p = 1;
            ptr[o.slot] = p; psz[o.slot] = o.size; live_bytes += o.size;
        } else {
            kfree(x, ptr[o.slot]);
            live_bytes -= psz[o.slot];
            ptr[o.slot] = nullptr;
        }
    }
    auto t1 = Clock::now();
    buddy_stats_t bs = buddy_get_stats(b);
    double used_bytes = (double)(bs.total_frames - bs.free_frames) * PG;
    ours_ns = secs(t0, t1) / NOPS * 1e9;
    ours_over = used_bytes / (double)live_bytes;
    size_t class_bytes = 0;                              // what the live objects occupy after size-class rounding
    for (int i = 0; i < SLOTS; i++)
        if (ptr[i]) { size_t c = 16; while (c < psz[i]) c <<= 1; class_bytes += c; }
    ours_obj = (double)class_bytes / (double)live_bytes;
    for (int i = 0; i < SLOTS; i++) if (ptr[i]) kfree(x, ptr[i]);
    slab_ctx_destroy(x);
    buddy_destroy(b);
    }

    std::printf("  %d alloc/free operations, ~%d live objects, sizes 24..1000 B\n\n", NOPS, SLOTS / 2);
    std::printf("  %-26s %10s %22s %24s\n", "", "ns / op", "per-object overhead", "footprint / live bytes");
    std::printf("  %-26s %10.1f %21.2fx %23.2fx\n", "MiniOS kmalloc (slab)", ours_ns, ours_obj, ours_over);
    std::printf("  %-26s %10.1f %21.2fx %23.2fx\n", "glibc malloc", glibc_ns, glibc_obj, glibc_over);
    std::printf("  (failed MiniOS allocations: %zu)\n", failed);
    std::printf("  per-object overhead = bytes the live objects occupy after header / size-class rounding;\n"
                "  footprint = total memory the allocator holds, which also counts partly-filled slab pages / free chunks.\n"
                "  Caveat: both run in user space on the same op sequence. glibc is a different design\n"
                "  (arenas, tcache) tuned for threads; this shows trade-offs, not a claim to beat it.\n"
                "  MiniOS's kmalloc rounds up to power-of-two classes, which is where its extra overhead comes from\n"
                "  (the compressed pool uses finer-grained classes for exactly this reason).\n");

    // --- buddy fragmentation under churn
    std::printf("\n  Buddy allocator fragmentation: random order-0..3 alloc/free held near 70%% utilization\n");
    const uint32_t N = 4096;
    buddy_t *bb = buddy_create(N);
    struct Blk { uint32_t f; unsigned o; };
    std::vector<Blk> lv;
    uint32_t used = 0;
    rng_state = 5;
    for (int i = 0; i < 400000; i++) {
        bool want_alloc = used < N * 70 / 100 ? rnd(0, 9) < 7 : rnd(0, 9) < 3;
        if (want_alloc || lv.empty()) {
            unsigned o = (unsigned)rnd(0, 3);
            int f = buddy_alloc(bb, o);
            if (f >= 0) { lv.push_back({ (uint32_t)f, o }); used += 1u << o; }
        } else {
            size_t k = (size_t)rnd32() % lv.size();
            buddy_free(bb, lv[k].f, lv[k].o);
            used -= 1u << lv[k].o;
            lv[k] = lv.back();
            lv.pop_back();
        }
    }
    buddy_stats_t fs = buddy_get_stats(bb);
    uint32_t nblocks = 0;
    for (int o = 0; o <= BUDDY_MAX_ORDER; o++) nblocks += fs.free_blocks[o];
    std::printf("    %u of %u frames in use; %u free, split across %u blocks; largest free block: order %d (%u KiB)\n",
                N - fs.free_frames, N, fs.free_frames, nblocks,
                fs.largest_free_order, (1u << fs.largest_free_order) * 4);
    std::printf("    external fragmentation index %.2f (0 = one big block, 1 = all free memory in single frames)\n",
                fs.ext_fragmentation);
    for (auto &bl : lv) buddy_free(bb, bl.f, bl.o);
    buddy_stats_t after = buddy_get_stats(bb);
    bool merged = after.free_frames == N && after.free_blocks[BUDDY_MAX_ORDER] == N >> BUDDY_MAX_ORDER;
    std::printf("    after freeing everything: %u free in %u maximum-size blocks (fully coalesced: %s)\n",
                after.free_frames, after.free_blocks[BUDDY_MAX_ORDER], merged ? "yes" : "no");
    buddy_destroy(bb);
}

int main()
{
    section_allocators();       // first, so glibc's heap is untouched when it is measured
    section_compression();
    section_effective_ram();
    std::printf("\n");
    return 0;
}
