#include "minitest.h"
extern "C" {
#include "mm.h"
}
#include <vector>
#include <algorithm>
#include <cstring>

static unsigned rng_state = 987654321;
static int rnd(int lo, int hi)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return lo + (int)((rng_state >> 8) % (unsigned)(hi - lo + 1));
}

static const repl_policy_t POLICIES[] = { REPL_FIFO, REPL_LRU, REPL_CLOCK };
static const uint32_t BASE = 0x400000;   // arbitrary user region (second page-directory entry)

static void test_pmm()
{
    CHECK_EQ(mm_init(8, 0, REPL_FIFO), VM_OK);
    CHECK_EQ(mm_init(8, 0, REPL_FIFO), VM_EINVAL);        // double init rejected
    for (int i = 0; i < 8; i++) CHECK_EQ(pmm_alloc_frame(), i);
    CHECK_EQ(pmm_alloc_frame(), -1);                       // exhausted
    CHECK_EQ(pmm_free_frames(), 0u);
    pmm_free_frame(3);
    pmm_free_frame(5);
    CHECK_EQ(pmm_free_frames(), 2u);
    CHECK_EQ(pmm_alloc_frame(), 3);                        // lowest free first
    CHECK_EQ(pmm_alloc_frame(), 5);
    mm_shutdown();
}

static void test_demand_paging()
{
    mm_init(8, 16, REPL_LRU);
    aspace_t *as = vm_create_aspace();
    CHECK_EQ(vm_map(as, BASE, 4, PTE_WRITE | PTE_USER), VM_OK);
    CHECK_EQ(pmm_free_frames(), 8u);                       // map() allocates nothing
    CHECK_EQ(vm_resident_pages(as), 0u);

    uint8_t b = 0xAA;
    CHECK_EQ(vm_read(as, BASE + 10, &b, 1), VM_OK);
    CHECK_EQ(b, 0);                                        // demand-zero
    CHECK_EQ(vm_get_stats().faults, 1u);
    CHECK_EQ(vm_get_stats().minor_faults, 1u);
    CHECK_EQ(pmm_free_frames(), 7u);

    const char msg[] = "hello, paging";
    CHECK_EQ(vm_write(as, BASE + 20, msg, sizeof msg), VM_OK);   // same page: no new fault
    CHECK_EQ(vm_get_stats().faults, 1u);
    char back[sizeof msg] = {};
    CHECK_EQ(vm_read(as, BASE + 20, back, sizeof msg), VM_OK);
    CHECK(std::strcmp(back, msg) == 0);

    // a buffer that straddles a page boundary
    std::vector<uint8_t> in(64), out(64);
    for (int i = 0; i < 64; i++) in[i] = (uint8_t)(i * 3 + 1);
    uint32_t va = BASE + 2 * PAGE_SIZE - 32;
    CHECK_EQ(vm_write(as, va, in.data(), 64), VM_OK);
    CHECK_EQ(vm_read(as, va, out.data(), 64), VM_OK);
    CHECK(in == out);
    CHECK_EQ(vm_resident_pages(as), 3u);

    vm_destroy_aspace(as);
    CHECK_EQ(pmm_free_frames(), 8u);                       // everything returned
    mm_shutdown();
}

static void test_faults_and_protection()
{
    mm_init(8, 16, REPL_FIFO);
    aspace_t *as = vm_create_aspace();
    uint8_t b;
    CHECK_EQ(vm_read(as, 0x1000, &b, 1), VM_EFAULT);       // unmapped
    CHECK_EQ(vm_map(as, BASE + 1, 1, PTE_WRITE), VM_EINVAL);    // misaligned
    CHECK_EQ(vm_map(as, BASE, 0, PTE_WRITE), VM_EINVAL);        // empty range
    CHECK_EQ(vm_map(as, BASE, 2, 0), VM_OK);               // read-only region
    CHECK_EQ(vm_map(as, BASE + PAGE_SIZE, 2, PTE_WRITE), VM_EINVAL);  // overlaps
    CHECK_EQ(vm_read(as, BASE, &b, 1), VM_OK);
    CHECK_EQ(vm_write(as, BASE, &b, 1), VM_EFAULT);        // write to resident RO page
    CHECK_EQ(vm_touch(as, BASE + PAGE_SIZE, 1), VM_EFAULT);// write fault on non-resident RO page
    CHECK_EQ(vm_read(as, BASE + 2 * PAGE_SIZE, &b, 1), VM_EFAULT);   // past the end
    vm_destroy_aspace(as);
    mm_shutdown();
}

static void test_isolation()
{
    mm_init(8, 16, REPL_LRU);
    aspace_t *a = vm_create_aspace(), *b = vm_create_aspace();
    vm_map(a, BASE, 1, PTE_WRITE);
    vm_map(b, BASE, 1, PTE_WRITE);
    uint32_t x = 0x11111111, y = 0x22222222, rx = 0, ry = 0;
    vm_write(a, BASE, &x, 4);
    vm_write(b, BASE, &y, 4);
    vm_read(a, BASE, &rx, 4);
    vm_read(b, BASE, &ry, 4);
    CHECK_EQ(rx, x);
    CHECK_EQ(ry, y);                                       // same VA, different memory
    vm_destroy_aspace(a);
    vm_destroy_aspace(b);
    mm_shutdown();
}

static uint8_t pat(uint32_t page, uint32_t off) { return (uint8_t)(page * 31 + off * 7 + 5); }

// Many more virtual pages than frames: data must survive swapping, and swap
// slots must be recycled (4 rounds x 200 pages > 512 slots would fail on a leak).
static void test_swap_integrity()
{
    for (repl_policy_t pol : POLICIES) {
        const uint32_t NFR = 16, NPG = 200;
        mm_init(NFR, 512, pol);
        for (int round = 0; round < 4; round++) {
            aspace_t *as = vm_create_aspace();
            CHECK_EQ(vm_map(as, BASE, NPG, PTE_WRITE | PTE_USER), VM_OK);
            for (uint32_t p = 0; p < NPG; p++) {
                uint8_t head[8], tail[8];
                for (int i = 0; i < 8; i++) { head[i] = pat(p, i); tail[i] = pat(p, 4088 + i); }
                CHECK_EQ(vm_write(as, BASE + p * PAGE_SIZE, head, 8), VM_OK);
                CHECK_EQ(vm_write(as, BASE + p * PAGE_SIZE + 4088, tail, 8), VM_OK);
            }
            std::vector<int> order(NPG);
            for (uint32_t i = 0; i < NPG; i++) order[i] = (int)i;
            for (int i = (int)NPG - 1; i > 0; i--) std::swap(order[i], order[rnd(0, i)]);
            for (int p : order) {
                uint8_t head[8], tail[8];
                CHECK_EQ(vm_read(as, BASE + p * PAGE_SIZE, head, 8), VM_OK);
                CHECK_EQ(vm_read(as, BASE + p * PAGE_SIZE + 4088, tail, 8), VM_OK);
                bool ok = true;
                for (int i = 0; i < 8; i++)
                    ok &= head[i] == pat(p, i) && tail[i] == pat(p, 4088 + i);
                CHECK(ok);
            }
            CHECK(vm_resident_pages(as) <= NFR);
            vm_destroy_aspace(as);
            CHECK_EQ(pmm_free_frames(), NFR);
        }
        vm_stats_t s = vm_get_stats();
        CHECK(s.evictions > 0);
        CHECK(s.swap_writes > 0);
        CHECK(s.major_faults > 0);
        CHECK_EQ(s.faults, s.minor_faults + s.major_faults);
        mm_shutdown();
    }
}

static void test_clean_pages_need_no_swap_io()
{
    mm_init(4, 16, REPL_LRU);
    aspace_t *as = vm_create_aspace();
    vm_map(as, BASE, 20, PTE_WRITE);
    for (int round = 0; round < 3; round++)
        for (uint32_t p = 0; p < 20; p++) vm_touch(as, BASE + p * PAGE_SIZE, 0);
    CHECK(vm_get_stats().evictions > 0);
    CHECK_EQ(vm_get_stats().swap_writes, 0u);              // never written -> dropped, not saved
    vm_destroy_aspace(as);
    mm_shutdown();
}

static void test_swap_copy_reuse()
{
    mm_init(1, 4, REPL_FIFO);                              // one frame forces constant paging
    aspace_t *as = vm_create_aspace();
    vm_map(as, BASE, 2, PTE_WRITE);
    uint32_t v = 0xCAFEBABE, r = 0;
    vm_write(as, BASE, &v, 4);                             // page0 dirty
    vm_touch(as, BASE + PAGE_SIZE, 0);                     // evicts page0 -> 1 swap write
    CHECK_EQ(vm_get_stats().swap_writes, 1u);
    vm_read(as, BASE, &r, 4);                              // page0 comes back (major fault)
    CHECK_EQ(r, v);
    vm_touch(as, BASE + PAGE_SIZE, 0);                     // evicts clean page0: copy still valid
    CHECK_EQ(vm_get_stats().swap_writes, 1u);              // no second write
    r = 0;
    vm_read(as, BASE, &r, 4);
    CHECK_EQ(r, v);
    CHECK_EQ(vm_get_stats().swap_reads, 2u);
    vm_destroy_aspace(as);
    mm_shutdown();
}

static void test_out_of_memory_paths()
{
    // swap full: dirty evictions run out of slots
    mm_init(4, 2, REPL_FIFO);
    aspace_t *as = vm_create_aspace();
    vm_map(as, BASE, 10, PTE_WRITE);
    uint8_t one = 1;
    int rc[10];
    for (int p = 0; p < 10; p++) rc[p] = vm_write(as, BASE + p * PAGE_SIZE, &one, 1);
    for (int p = 0; p < 6; p++) CHECK_EQ(rc[p], VM_OK);    // 4 frames + 2 swap slots
    CHECK_EQ(rc[6], VM_ENOMEM);
    vm_destroy_aspace(as);
    mm_shutdown();

    // every frame pinned by the "kernel": nothing can be evicted
    mm_init(2, 8, REPL_LRU);
    pmm_alloc_frame();
    pmm_alloc_frame();
    as = vm_create_aspace();
    vm_map(as, BASE, 1, PTE_WRITE);
    CHECK_EQ(vm_touch(as, BASE, 0), VM_ENOMEM);
    vm_destroy_aspace(as);
    mm_shutdown();
}

static void test_unmap_releases_resources()
{
    mm_init(4, 16, REPL_LRU);
    aspace_t *as = vm_create_aspace();
    vm_map(as, BASE, 10, PTE_WRITE);
    uint8_t one = 1;
    for (int p = 0; p < 10; p++) vm_write(as, BASE + p * PAGE_SIZE, &one, 1);
    CHECK_EQ(pmm_free_frames(), 0u);
    CHECK_EQ(vm_unmap(as, BASE, 10), VM_OK);
    CHECK_EQ(pmm_free_frames(), 4u);
    CHECK_EQ(vm_touch(as, BASE, 0), VM_EFAULT);            // gone
    CHECK_EQ(vm_map(as, BASE, 10, PTE_WRITE), VM_OK);      // address range reusable
    vm_destroy_aspace(as);
    mm_shutdown();
}

static void test_textbook_reference_strings()
{
    // Belady's anomaly: FIFO gets *worse* with more frames
    const int belady[] = { 1, 2, 3, 4, 1, 2, 5, 1, 2, 3, 4, 5 };
    CHECK_EQ(pagesim_faults(ALGO_FIFO, 3, belady, 12), 9);
    CHECK_EQ(pagesim_faults(ALGO_FIFO, 4, belady, 12), 10);
    CHECK(pagesim_faults(ALGO_LRU, 4, belady, 12) <= pagesim_faults(ALGO_LRU, 3, belady, 12));

    // classic 20-reference string, 3 frames
    const int s[] = { 7, 0, 1, 2, 0, 3, 0, 4, 2, 3, 0, 3, 2, 1, 2, 0, 1, 7, 0, 1 };
    CHECK_EQ(pagesim_faults(ALGO_FIFO, 3, s, 20), 15);
    CHECK_EQ(pagesim_faults(ALGO_LRU,  3, s, 20), 12);
    CHECK_EQ(pagesim_faults(ALGO_OPT,  3, s, 20), 9);
}

static void test_opt_is_a_lower_bound()
{
    for (int round = 0; round < 100; round++) {
        int n = rnd(20, 120), pages = rnd(4, 15), frames = rnd(2, 8);
        std::vector<int> refs(n);
        for (int &r : refs) r = rnd(0, pages - 1);
        int opt = pagesim_faults(ALGO_OPT, frames, refs.data(), n);
        for (page_algo_t a : { ALGO_FIFO, ALGO_LRU, ALGO_CLOCK })
            CHECK(opt <= pagesim_faults(a, frames, refs.data(), n));
        // every distinct page must fault at least once
        std::vector<int> u = refs;
        std::sort(u.begin(), u.end());
        int distinct = (int)(std::unique(u.begin(), u.end()) - u.begin());
        CHECK(opt >= distinct);
    }
}

// The live kernel and the offline analyser implement the same policies, so
// fault counts on the same reference string must match exactly.
static void test_live_vmm_matches_analyser()
{
    struct { repl_policy_t live; page_algo_t sim; } pairs[] = {
        { REPL_FIFO, ALGO_FIFO }, { REPL_LRU, ALGO_LRU }, { REPL_CLOCK, ALGO_CLOCK }
    };
    for (auto &pr : pairs) {
        for (int round = 0; round < 40; round++) {
            int n = rnd(30, 200), pages = rnd(4, 14), frames = rnd(2, 8);
            std::vector<int> refs(n);
            for (int &r : refs) r = rnd(0, pages - 1);

            mm_init((uint32_t)frames, 64, pr.live);
            aspace_t *as = vm_create_aspace();
            vm_map(as, BASE, (uint32_t)pages, PTE_WRITE);
            for (int r : refs) vm_touch(as, BASE + (uint32_t)r * PAGE_SIZE, 0);
            int live = (int)vm_get_stats().faults;
            vm_destroy_aspace(as);
            mm_shutdown();

            CHECK_EQ(live, pagesim_faults(pr.sim, frames, refs.data(), n));
        }
    }
}

int main()
{
    RUN(test_pmm);
    RUN(test_demand_paging);
    RUN(test_faults_and_protection);
    RUN(test_isolation);
    RUN(test_swap_integrity);
    RUN(test_clean_pages_need_no_swap_io);
    RUN(test_swap_copy_reuse);
    RUN(test_out_of_memory_paths);
    RUN(test_unmap_releases_resources);
    RUN(test_textbook_reference_strings);
    RUN(test_opt_is_a_lower_bound);
    RUN(test_live_vmm_matches_analyser);
    return TEST_SUMMARY();
}
