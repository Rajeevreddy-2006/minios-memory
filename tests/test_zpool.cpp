#include "minitest.h"
extern "C" {
#include "mm.h"
}
#include <vector>
#include <cstring>
#include <algorithm>

static unsigned rng_state = 20261110;
static unsigned rnd32()
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}
static int rnd(int lo, int hi) { return lo + (int)(rnd32() % (unsigned)(hi - lo + 1)); }

static const uint32_t BASE = 0x400000;
static const uint32_t PG = 4096;

// Page contents with very different compressibility. Deterministic in (kind, id).
enum { K_ZERO, K_SAME, K_TEXT, K_RECORDS, K_RANDOM, K_HALF };
static void fill_page(int kind, uint32_t id, uint8_t *buf)
{
    static const char *s = "the kernel pages out cold memory to swap. ";
    const size_t L = std::strlen(s);
    switch (kind) {
    case K_ZERO:  std::memset(buf, 0, PG); break;
    case K_SAME:  std::memset(buf, 0xAB, PG); break;
    case K_TEXT:
    case K_HALF:
        for (uint32_t i = 0; i < PG; i++) buf[i] = (uint8_t)s[i % L];
        buf[0] = (uint8_t)id; buf[1] = (uint8_t)(id >> 8);
        if (kind == K_TEXT) break;
        [[fallthrough]];                                    // K_HALF: second half becomes random
    case K_RANDOM: {
        uint32_t x = id * 2654435761u + 12345u;
        for (uint32_t i = kind == K_HALF ? PG / 2 : 0; i < PG; i++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            buf[i] = (uint8_t)x;
        }
        break;
    }
    case K_RECORDS:
        for (uint32_t i = 0; i < PG; i += 16) {
            std::memcpy(buf + i, "RECORD01", 8);
            uint32_t c = id * 1000 + i / 16;
            std::memcpy(buf + i + 8, &c, 4);
            std::memset(buf + i + 12, 0, 4);
        }
        break;
    }
}

static int write_page(aspace_t *as, uint32_t va, int kind, uint32_t id)
{
    uint8_t buf[PG];
    fill_page(kind, id, buf);
    return vm_write(as, va, buf, PG);
}

static bool page_is(aspace_t *as, uint32_t va, int kind, uint32_t id)
{
    uint8_t want[PG], got[PG];
    fill_page(kind, id, want);
    return vm_read(as, va, got, PG) == VM_OK && std::memcmp(want, got, PG) == 0;
}

static mm_config_t cfg(uint32_t total, uint32_t pool, repl_policy_t p, uint32_t swap = 512,
                       uint32_t max_entries = 0)
{
    mm_config_t c = { total, swap, p, pool, max_entries, 0 };
    return c;
}

static void test_config_validation()
{
    mm_config_t bad = cfg(8, 8, REPL_FIFO);
    CHECK_EQ(mm_init_ex(&bad), VM_EINVAL);                  // pool can't be the whole RAM
    bad = cfg(8, 9, REPL_FIFO);
    CHECK_EQ(mm_init_ex(&bad), VM_EINVAL);
    mm_config_t ok = cfg(8, 7, REPL_FIFO);
    CHECK_EQ(mm_init_ex(&ok), VM_OK);
    CHECK_EQ(pmm_free_frames(), 1u);                        // pool frames are NOT available to user pages
    mm_shutdown();
    ok = cfg(16, 4, REPL_LRU);
    CHECK_EQ(mm_init_ex(&ok), VM_OK);
    CHECK_EQ(pmm_free_frames(), 12u);
    CHECK_EQ(zpool_get_stats().pool_frames_total, 4u);
    mm_shutdown();
}

static void test_roundtrip_all_data_kinds()
{
    for (repl_policy_t pol : { REPL_FIFO, REPL_LRU, REPL_CLOCK }) {
        mm_config_t c = cfg(32, 8, pol);
        CHECK_EQ(mm_init_ex(&c), VM_OK);
        aspace_t *as = vm_create_aspace();
        const int N = 120;
        vm_map(as, BASE, N, PTE_WRITE | PTE_USER);
        for (int p = 0; p < N; p++) CHECK_EQ(write_page(as, BASE + (uint32_t)p * PG, p % 6, (uint32_t)p), VM_OK);

        std::vector<int> order(N);
        for (int i = 0; i < N; i++) order[i] = i;
        for (int i = N - 1; i > 0; i--) std::swap(order[i], order[rnd(0, i)]);
        for (int p : order) CHECK(page_is(as, BASE + (uint32_t)p * PG, p % 6, (uint32_t)p));

        vm_stats_t s = vm_get_stats();
        zpool_stats_t z = zpool_get_stats();
        CHECK(z.stores > 0 && z.loads > 0);
        CHECK(z.samefill_stores > 0);                       // zero / 0xAB pages
        CHECK(z.rejects > 0);                               // random pages don't compress
        CHECK(s.swap_writes > 0);                           // ... so they went to disk
        CHECK(s.zpool_faults > 0 && s.major_faults > 0 && s.minor_faults > 0);
        CHECK_EQ(s.faults, s.minor_faults + s.major_faults + s.zpool_faults);

        vm_destroy_aspace(as);
        z = zpool_get_stats();
        CHECK_EQ(z.live_entries, 0u);                       // nothing leaked
        CHECK_EQ(z.pool_frames_used, 0u);
        CHECK_EQ(pmm_free_frames(), 24u);
        mm_shutdown();
    }
}

// The headline behaviour: more pages than the frame budget, zero disk I/O.
static void test_compressible_pages_avoid_disk()
{
    mm_config_t c = cfg(64, 16, REPL_LRU);
    mm_init_ex(&c);
    aspace_t *as = vm_create_aspace();
    const int N = 100;                                       // 100 pages in a 64-frame budget
    vm_map(as, BASE, N, PTE_WRITE | PTE_USER);
    for (int p = 0; p < N; p++) CHECK_EQ(write_page(as, BASE + (uint32_t)p * PG, K_TEXT, (uint32_t)p), VM_OK);
    for (int p = 0; p < N; p++) CHECK(page_is(as, BASE + (uint32_t)p * PG, K_TEXT, (uint32_t)p));

    vm_stats_t s = vm_get_stats();
    zpool_stats_t z = zpool_get_stats();
    CHECK_EQ(s.swap_writes, 0u);
    CHECK_EQ(s.swap_reads, 0u);
    CHECK(z.stores > 0);
    CHECK(z.live_bytes < (uint64_t)N * 200);                // each page shrank to a tiny blob
    vm_destroy_aspace(as);
    mm_shutdown();
}

static void test_resident_plus_pooled_exceeds_frames()
{
    mm_config_t c = cfg(64, 16, REPL_LRU);                  // 48 user frames
    mm_init_ex(&c);
    aspace_t *as = vm_create_aspace();
    const int N = 100;
    vm_map(as, BASE, N, PTE_WRITE | PTE_USER);
    for (int p = 0; p < N; p++) write_page(as, BASE + (uint32_t)p * PG, K_TEXT, (uint32_t)p);
    zpool_stats_t z = zpool_get_stats();
    CHECK_EQ(vm_resident_pages(as) + z.live_entries, (uint32_t)N);   // every page is still in RAM
    CHECK(vm_resident_pages(as) <= 48u);
    vm_destroy_aspace(as);
    mm_shutdown();
}

static void test_samefill_costs_no_pool_memory()
{
    mm_config_t c = cfg(16, 4, REPL_CLOCK, 512, 1024);
    mm_init_ex(&c);
    aspace_t *as = vm_create_aspace();
    const int N = 600;
    vm_map(as, BASE, N, PTE_WRITE | PTE_USER);
    for (int p = 0; p < N; p++) CHECK_EQ(write_page(as, BASE + (uint32_t)p * PG, p % 2 ? K_SAME : K_ZERO, (uint32_t)p), VM_OK);
    zpool_stats_t z = zpool_get_stats();
    CHECK(z.samefill_stores > 0);
    CHECK_EQ(z.live_samefill, z.live_entries);
    CHECK_EQ(z.pool_frames_used, 0u);                       // not one heap frame consumed
    CHECK_EQ(z.live_bytes, 0ull);
    CHECK_EQ(vm_get_stats().swap_writes, 0u);
    for (int p = 0; p < N; p++) CHECK(page_is(as, BASE + (uint32_t)p * PG, p % 2 ? K_SAME : K_ZERO, (uint32_t)p));
    vm_destroy_aspace(as);
    mm_shutdown();
}

static void test_incompressible_goes_to_disk()
{
    mm_config_t c = cfg(16, 4, REPL_LRU);
    mm_init_ex(&c);
    aspace_t *as = vm_create_aspace();
    const int N = 60;
    vm_map(as, BASE, N, PTE_WRITE | PTE_USER);
    for (int p = 0; p < N; p++) write_page(as, BASE + (uint32_t)p * PG, K_RANDOM, (uint32_t)p);
    for (int p = 0; p < N; p++) CHECK(page_is(as, BASE + (uint32_t)p * PG, K_RANDOM, (uint32_t)p));
    zpool_stats_t z = zpool_get_stats();
    CHECK_EQ(z.stores, 0ull);                               // the pool never kept a random page
    CHECK(z.rejects > 0);
    CHECK(vm_get_stats().swap_writes > 0);
    vm_destroy_aspace(as);
    mm_shutdown();
}

static void test_pool_overflow_writes_back_to_disk()
{
    mm_config_t c = cfg(24, 2, REPL_LRU);                   // a pool of just 2 frames
    mm_init_ex(&c);
    aspace_t *as = vm_create_aspace();
    const int N = 200;
    vm_map(as, BASE, N, PTE_WRITE | PTE_USER);
    for (int p = 0; p < N; p++) CHECK_EQ(write_page(as, BASE + (uint32_t)p * PG, K_RECORDS, (uint32_t)p), VM_OK);
    zpool_stats_t z = zpool_get_stats();
    CHECK(z.stores > 0);
    CHECK(z.writebacks > 0);                                // the pool filled up and spilled to disk
    CHECK(vm_get_stats().swap_writes >= z.writebacks);
    CHECK(z.pool_frames_used <= 2u);                        // never grew past its carve-out

    std::vector<int> order(N);
    for (int i = 0; i < N; i++) order[i] = i;
    for (int i = N - 1; i > 0; i--) std::swap(order[i], order[rnd(0, i)]);
    for (int p : order) CHECK(page_is(as, BASE + (uint32_t)p * PG, K_RECORDS, (uint32_t)p));
    vm_destroy_aspace(as);
    CHECK_EQ(zpool_get_stats().live_entries, 0u);
    mm_shutdown();
}

// A page loaded back from the pool has no backing copy any more, so it must be
// saved again when evicted; losing it here would silently turn it into zeroes.
static void test_exclusive_load_resaves_page()
{
    mm_config_t c = cfg(5, 4, REPL_FIFO, 64);               // exactly one user frame
    mm_init_ex(&c);
    aspace_t *as = vm_create_aspace();
    vm_map(as, BASE, 2, PTE_WRITE | PTE_USER);
    write_page(as, BASE, K_TEXT, 1);
    write_page(as, BASE + PG, K_TEXT, 2);
    for (int round = 0; round < 6; round++) {
        CHECK(page_is(as, BASE, K_TEXT, 1));
        CHECK(page_is(as, BASE + PG, K_TEXT, 2));
    }
    zpool_stats_t z = zpool_get_stats();
    CHECK(z.stores >= 12);                                  // re-stored every time, not just once
    CHECK(z.loads >= 11);
    CHECK_EQ(vm_get_stats().swap_writes, 0u);
    vm_destroy_aspace(as);
    mm_shutdown();
}

static void test_unmap_releases_pool_entries()
{
    mm_config_t c = cfg(16, 4, REPL_LRU);
    mm_init_ex(&c);
    aspace_t *as = vm_create_aspace();
    vm_map(as, BASE, 40, PTE_WRITE | PTE_USER);
    for (int p = 0; p < 40; p++) write_page(as, BASE + (uint32_t)p * PG, K_TEXT, (uint32_t)p);
    CHECK(zpool_get_stats().live_entries > 0);
    CHECK_EQ(vm_unmap(as, BASE, 40), VM_OK);
    zpool_stats_t z = zpool_get_stats();
    CHECK_EQ(z.live_entries, 0u);
    CHECK_EQ(z.pool_frames_used, 0u);                       // empty slabs returned to the buddy allocator
    CHECK_EQ(pmm_free_frames(), 12u);
    vm_destroy_aspace(as);
    mm_shutdown();
}

// Pool and swap both exhausted: writes fail cleanly with ENOMEM, nothing that
// succeeded is corrupted, and freeing memory lets everything be read again.
static void test_total_memory_exhaustion()
{
    mm_config_t c = cfg(8, 2, REPL_LRU, 4);                 // 6 user frames, 2 pool frames, 4 swap slots
    mm_init_ex(&c);
    aspace_t *as = vm_create_aspace();
    const int N = 60;
    vm_map(as, BASE, N, PTE_WRITE | PTE_USER);
    int ok = 0;
    for (; ok < N; ok++) {
        int rc = write_page(as, BASE + (uint32_t)ok * PG, K_RECORDS, (uint32_t)ok);
        if (rc != VM_OK) { CHECK_EQ(rc, VM_ENOMEM); break; }
    }
    CHECK(ok > 6 && ok < N);                                // got past RAM, but not unlimited
    CHECK_EQ(vm_unmap(as, BASE + (uint32_t)(ok / 2) * PG, (uint32_t)(N - ok / 2)), VM_OK);   // give memory back
    for (int p = 0; p < ok / 2; p++) CHECK(page_is(as, BASE + (uint32_t)p * PG, K_RECORDS, (uint32_t)p));
    vm_destroy_aspace(as);
    CHECK_EQ(zpool_get_stats().live_entries, 0u);
    mm_shutdown();
}

// Random reads, writes and unmap/remap across three address spaces with a shadow
// copy of every page. Any corruption in compress / writeback / reload shows up here.
static void stress(const mm_config_t &c, int ops, bool expect_pool)
{
    CHECK_EQ(mm_init_ex(&c), VM_OK);
    const int NA = 3, NP = 40;
    aspace_t *as[NA];
    std::vector<std::vector<uint8_t>> sh((size_t)NA * NP, std::vector<uint8_t>(PG));
    auto va = [&](int p) { return BASE + (uint32_t)p * PG; };

    for (int a = 0; a < NA; a++) {
        as[a] = vm_create_aspace();
        CHECK_EQ(vm_map(as[a], BASE, NP, PTE_WRITE | PTE_USER), VM_OK);
        for (int p = 0; p < NP; p++) {
            auto &s = sh[(size_t)a * NP + p];
            fill_page((a * 7 + p) % 6, (uint32_t)(a * 100 + p), s.data());
            CHECK_EQ(vm_write(as[a], va(p), s.data(), PG), VM_OK);
        }
    }
    for (int op = 0; op < ops; op++) {
        int a = rnd(0, NA - 1), p = rnd(0, NP - 1), r = rnd(0, 99);
        auto &s = sh[(size_t)a * NP + p];
        int len = rnd(1, 120), off = rnd(0, (int)PG - len);
        if (r < 50) {
            uint8_t d[120];
            for (int i = 0; i < len; i++) d[i] = rnd(0, 1) ? 0 : (uint8_t)rnd32();
            CHECK_EQ(vm_write(as[a], va(p) + (uint32_t)off, d, (uint32_t)len), VM_OK);
            std::memcpy(s.data() + off, d, (size_t)len);
        } else if (r < 95) {
            uint8_t d[120];
            CHECK_EQ(vm_read(as[a], va(p) + (uint32_t)off, d, (uint32_t)len), VM_OK);
            CHECK(std::memcmp(d, s.data() + off, (size_t)len) == 0);
        } else {
            CHECK_EQ(vm_unmap(as[a], va(p), 1), VM_OK);
            CHECK_EQ(vm_map(as[a], va(p), 1, PTE_WRITE | PTE_USER), VM_OK);
            uint8_t got[PG], zero[PG] = {};
            CHECK_EQ(vm_read(as[a], va(p), got, PG), VM_OK);
            CHECK(std::memcmp(got, zero, PG) == 0);         // fresh mapping reads as zeroes
            fill_page(rnd(0, 5), (uint32_t)op, s.data());
            CHECK_EQ(vm_write(as[a], va(p), s.data(), PG), VM_OK);
        }
    }
    for (int a = 0; a < NA; a++)
        for (int p = 0; p < NP; p++) {
            uint8_t got[PG];
            CHECK_EQ(vm_read(as[a], va(p), got, PG), VM_OK);
            CHECK(std::memcmp(got, sh[(size_t)a * NP + p].data(), PG) == 0);
        }

    vm_stats_t s = vm_get_stats();
    zpool_stats_t z = zpool_get_stats();
    CHECK_EQ(s.faults, s.minor_faults + s.major_faults + s.zpool_faults);
    if (expect_pool) CHECK(z.stores > 0 && z.loads > 0); else CHECK_EQ(z.stores, 0ull);

    for (int a = 0; a < NA; a++) vm_destroy_aspace(as[a]);
    z = zpool_get_stats();
    CHECK_EQ(z.live_entries, 0u);
    CHECK_EQ(z.pool_frames_used, 0u);
    CHECK_EQ(pmm_free_frames(), c.nframes - c.zpool_frames);
    mm_shutdown();
}

static void test_stress_with_shadow()
{
    stress(cfg(32, 8, REPL_CLOCK), 12000, true);
    stress(cfg(32, 2, REPL_LRU), 12000, true);              // tiny pool: constant writeback
    stress(cfg(16, 4, REPL_FIFO, 512, 64), 12000, true);    // entry table of 64: exhausted constantly
    stress(cfg(24, 0, REPL_CLOCK), 12000, false);           // control: compression off
}

int main()
{
    RUN(test_config_validation);
    RUN(test_roundtrip_all_data_kinds);
    RUN(test_compressible_pages_avoid_disk);
    RUN(test_resident_plus_pooled_exceeds_frames);
    RUN(test_samefill_costs_no_pool_memory);
    RUN(test_incompressible_goes_to_disk);
    RUN(test_pool_overflow_writes_back_to_disk);
    RUN(test_exclusive_load_resaves_page);
    RUN(test_unmap_releases_pool_entries);
    RUN(test_total_memory_exhaustion);
    RUN(test_stress_with_shadow);
    return TEST_SUMMARY();
}
