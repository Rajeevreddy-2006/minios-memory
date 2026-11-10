#include "minitest.h"
extern "C" {
#include "buddy.h"
}
#include <vector>
#include <algorithm>
#include <set>

static unsigned rng_state = 424242;
static int rnd(int lo, int hi)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return lo + (int)((rng_state >> 8) % (unsigned)(hi - lo + 1));
}

static uint32_t sum_blocks(const buddy_stats_t &s)
{
    uint32_t t = 0;
    for (int o = 0; o <= BUDDY_MAX_ORDER; o++) t += s.free_blocks[o] << o;
    return t;
}

static void test_full_cycle_coalesces()
{
    buddy_t *b = buddy_create(16);
    std::set<int> seen;
    for (int i = 0; i < 16; i++) {
        int f = buddy_alloc(b, 0);
        CHECK(f >= 0 && f < 16);
        seen.insert(f);
    }
    CHECK_EQ((int)seen.size(), 16);                 // every frame handed out exactly once
    CHECK_EQ(buddy_alloc(b, 0), -1);                // exhausted
    std::vector<int> order(seen.begin(), seen.end());
    for (int i = 15; i > 0; i--) std::swap(order[i], order[rnd(0, i)]);
    for (int f : order) CHECK_EQ(buddy_free(b, (uint32_t)f, 0), 0);

    buddy_stats_t s = buddy_get_stats(b);
    CHECK_EQ(s.free_frames, 16u);
    CHECK_EQ(s.free_blocks[4], 1u);                 // everything merged back into one block
    CHECK_EQ(s.largest_free_order, 4);
    CHECK(s.ext_fragmentation == 0.0);
    CHECK_EQ(buddy_alloc(b, 4), 0);
    buddy_destroy(b);
}

static void test_non_power_of_two_size()
{
    buddy_t *b = buddy_create(13);                  // 8 + 4 + 1
    buddy_stats_t s = buddy_get_stats(b);
    CHECK_EQ(s.free_frames, 13u);
    CHECK_EQ(s.free_blocks[3], 1u);
    CHECK_EQ(s.free_blocks[2], 1u);
    CHECK_EQ(s.free_blocks[0], 1u);
    int a = buddy_alloc(b, 3), c = buddy_alloc(b, 2), d = buddy_alloc(b, 0);
    CHECK(a >= 0 && c >= 0 && d >= 0);
    CHECK_EQ(buddy_alloc(b, 0), -1);
    buddy_free(b, (uint32_t)a, 3);
    buddy_free(b, (uint32_t)c, 2);
    buddy_free(b, (uint32_t)d, 0);
    s = buddy_get_stats(b);
    CHECK_EQ(s.free_frames, 13u);
    CHECK_EQ(s.free_blocks[3], 1u);                 // frame 12 must not merge past the end
    CHECK_EQ(buddy_alloc(b, 4), -1);                // 16 contiguous frames never existed
    buddy_destroy(b);
    CHECK(buddy_create(0) == nullptr);
}

static void test_alignment()
{
    buddy_t *b = buddy_create(64);
    for (unsigned o = 0; o <= 5; o++) {
        int f = buddy_alloc(b, o);
        CHECK(f >= 0);
        CHECK_EQ((unsigned)f & ((1u << o) - 1), 0u);        // order-k blocks are 2^k aligned
        CHECK_EQ(buddy_block_order(b, (uint32_t)f), (int)o);
        buddy_free(b, (uint32_t)f, o);
    }
    buddy_destroy(b);
}

static void test_invalid_frees()
{
    buddy_t *b = buddy_create(32);
    int f = buddy_alloc(b, 2);
    uint32_t before = buddy_get_stats(b).free_frames;
    CHECK_EQ(buddy_free(b, (uint32_t)f, 1), -1);            // wrong order
    CHECK_EQ(buddy_free(b, (uint32_t)f + 1, 2), -1);        // misaligned
    CHECK_EQ(buddy_free(b, 99, 0), -1);                     // out of range
    CHECK_EQ(buddy_free(b, (uint32_t)f, 11), -1);           // bad order
    CHECK_EQ(buddy_get_stats(b).free_frames, before);       // nothing changed
    CHECK_EQ(buddy_free(b, (uint32_t)f, 2), 0);
    CHECK_EQ(buddy_free(b, (uint32_t)f, 2), -1);            // double free
    CHECK_EQ(buddy_get_stats(b).free_frames, 32u);
    buddy_destroy(b);
}

static void test_random_stress()
{
    const uint32_t N = 512;
    buddy_t *b = buddy_create(N);
    struct Blk { uint32_t f; unsigned o; };
    std::vector<Blk> live;
    std::vector<char> used(N, 0);
    uint32_t live_frames = 0;

    for (int op = 0; op < 20000; op++) {
        if (live.empty() || rnd(0, 9) < 6) {
            unsigned o = (unsigned)rnd(0, 5);
            int f = buddy_alloc(b, o);
            if (f >= 0) {
                for (uint32_t i = 0; i < (1u << o); i++) {
                    CHECK(!used[f + i]);                    // blocks never overlap
                    used[f + i] = 1;
                }
                live.push_back({ (uint32_t)f, o });
                live_frames += 1u << o;
            }
        } else {
            size_t i = (size_t)rnd(0, (int)live.size() - 1);
            Blk bl = live[i];
            live.erase(live.begin() + (long)i);
            for (uint32_t k = 0; k < (1u << bl.o); k++) used[bl.f + k] = 0;
            CHECK_EQ(buddy_free(b, bl.f, bl.o), 0);
            live_frames -= 1u << bl.o;
        }
        if (op % 100 == 0) {
            buddy_stats_t s = buddy_get_stats(b);
            CHECK_EQ(s.free_frames, N - live_frames);
            CHECK_EQ(sum_blocks(s), s.free_frames);         // lists agree with the counter
        }
    }
    for (auto &bl : live) CHECK_EQ(buddy_free(b, bl.f, bl.o), 0);
    buddy_stats_t s = buddy_get_stats(b);
    CHECK_EQ(s.free_frames, N);
    CHECK_EQ(s.free_blocks[9], 1u);                         // fully coalesced again
    buddy_destroy(b);
}

static void test_fragmentation_metric()
{
    buddy_t *b = buddy_create(64);
    for (int i = 0; i < 64; i++) buddy_alloc(b, 0);
    for (uint32_t f = 0; f < 64; f += 2) buddy_free(b, f, 0);       // free every other frame
    buddy_stats_t s = buddy_get_stats(b);
    CHECK_EQ(s.free_frames, 32u);
    CHECK_EQ(s.largest_free_order, 0);                      // plenty free, but nothing contiguous
    CHECK(s.ext_fragmentation > 0.95);
    CHECK_EQ(buddy_alloc(b, 1), -1);
    for (uint32_t f = 1; f < 64; f += 2) buddy_free(b, f, 0);
    s = buddy_get_stats(b);
    CHECK(s.ext_fragmentation == 0.0);
    CHECK_EQ(s.free_blocks[6], 1u);
    buddy_destroy(b);
}

int main()
{
    RUN(test_full_cycle_coalesces);
    RUN(test_non_power_of_two_size);
    RUN(test_alignment);
    RUN(test_invalid_frees);
    RUN(test_random_stress);
    RUN(test_fragmentation_metric);
    return TEST_SUMMARY();
}
