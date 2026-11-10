#include "minitest.h"
extern "C" {
#include "slab.h"
}
#include <vector>
#include <cstring>
#include <set>

static unsigned rng_state = 777;
static int rnd(int lo, int hi)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return lo + (int)((rng_state >> 8) % (unsigned)(hi - lo + 1));
}

struct Heap {
    uint32_t n;
    std::vector<uint8_t> mem;
    buddy_t *b;
    slab_ctx_t *x;
    explicit Heap(uint32_t frames) : n(frames), mem((size_t)frames * 4096)
    {
        b = buddy_create(frames);
        x = slab_ctx_create(b, mem.data());
    }
    ~Heap() { slab_ctx_destroy(x); buddy_destroy(b); }
    uint32_t free_frames() const { return buddy_get_stats(b).free_frames; }
    bool inside(const void *p) const
    {
        const uint8_t *q = (const uint8_t *)p;
        return q >= mem.data() && q < mem.data() + mem.size();
    }
};

static void test_cache_basics()
{
    Heap h(64);
    kmem_cache_t *c = kmem_cache_create(h.x, "t64", 64);
    CHECK_EQ(kmem_cache_get_stats(c).objs_per_slab, 64u);

    std::vector<uint8_t *> objs;
    std::set<uint8_t *> uniq;
    for (int i = 0; i < 128; i++) {
        uint8_t *p = (uint8_t *)kmem_cache_alloc(c);
        CHECK(p != nullptr && h.inside(p));
        CHECK_EQ((uintptr_t)p % 8, 0u);
        std::memset(p, i, 64);                      // distinct fill per object
        objs.push_back(p);
        uniq.insert(p);
    }
    CHECK_EQ((int)uniq.size(), 128);
    CHECK_EQ(kmem_cache_get_stats(c).slabs, 2u);    // 128 objects of 64 B = exactly 2 pages
    CHECK_EQ(h.free_frames(), 64u - 2);
    for (int i = 0; i < 128; i++) {
        bool ok = true;
        for (int k = 0; k < 64; k++) ok &= objs[i][k] == (uint8_t)i;
        CHECK(ok);                                  // no object overwrote a neighbour
    }
    for (auto p : objs) CHECK_EQ(kmem_cache_free(c, p), 0);
    CHECK_EQ(kmem_cache_get_stats(c).slabs, 0u);
    CHECK_EQ(h.free_frames(), 64u);                 // empty slabs went back to the buddy allocator
}

static void test_size_rounding_and_limits()
{
    Heap h(8);
    kmem_cache_t *c = kmem_cache_create(h.x, "odd", 13);
    CHECK_EQ(kmem_cache_get_stats(c).obj_size, 16u);
    CHECK_EQ(kmem_cache_get_stats(c).objs_per_slab, 256u);
    CHECK(kmem_cache_create(h.x, "big", 5000) == nullptr);
    CHECK(kmem_cache_create(h.x, "zero", 0) == nullptr);
}

static void test_bad_frees_detected()
{
    Heap h(16);
    kmem_cache_t *a = kmem_cache_create(h.x, "a", 64);
    kmem_cache_t *b = kmem_cache_create(h.x, "b", 64);
    uint8_t *p = (uint8_t *)kmem_cache_alloc(a);
    uint8_t *q = (uint8_t *)kmem_cache_alloc(b);
    CHECK_EQ(kmem_cache_free(a, p + 1), -1);        // not an object boundary
    CHECK_EQ(kmem_cache_free(a, q), -1);            // belongs to another cache
    CHECK_EQ(kmem_cache_free(a, nullptr), -1);
    uint8_t outside[8];
    CHECK_EQ(kmem_cache_free(a, outside), -1);      // not heap memory
    CHECK_EQ(kmem_cache_free(a, p), 0);
    CHECK_EQ(kmem_cache_free(a, p), -1);            // double free
    CHECK_EQ(kmem_cache_free(b, q), 0);
    CHECK_EQ(h.free_frames(), 16u);
}

static void test_one_object_per_slab()
{
    Heap h(8);
    kmem_cache_t *c = kmem_cache_create(h.x, "page", 4096);
    CHECK_EQ(kmem_cache_get_stats(c).objs_per_slab, 1u);
    void *a = kmem_cache_alloc(c), *b = kmem_cache_alloc(c), *d = kmem_cache_alloc(c);
    CHECK(a && b && d);
    CHECK_EQ(kmem_cache_get_stats(c).slabs, 3u);
    CHECK_EQ(kmem_cache_free(c, b), 0);
    CHECK_EQ(kmem_cache_get_stats(c).slabs, 2u);
    void *e = kmem_cache_alloc(c);
    CHECK(e != nullptr);
    CHECK_EQ(kmem_cache_free(c, a), 0);
    CHECK_EQ(kmem_cache_free(c, d), 0);
    CHECK_EQ(kmem_cache_free(c, e), 0);
    CHECK_EQ(h.free_frames(), 8u);
}

static void test_random_churn_with_shadow()
{
    Heap h(128);
    kmem_cache_t *cs[2] = { kmem_cache_create(h.x, "s24", 24), kmem_cache_create(h.x, "s200", 200) };
    const uint32_t sz[2] = { 24, 200 };
    struct Obj { uint8_t *p; int cache; uint8_t tag; };
    std::vector<Obj> live;
    uint8_t tag = 1;

    for (int op = 0; op < 20000; op++) {
        if (live.empty() || rnd(0, 9) < 6) {
            int k = rnd(0, 1);
            uint8_t *p = (uint8_t *)kmem_cache_alloc(cs[k]);
            if (!p) continue;                       // heap full is fine
            std::memset(p, tag, sz[k]);
            live.push_back({ p, k, tag });
            tag = (uint8_t)(tag % 250 + 1);
        } else {
            size_t i = (size_t)rnd(0, (int)live.size() - 1);
            Obj o = live[i];
            bool ok = true;
            for (uint32_t b = 0; b < sz[o.cache]; b++) ok &= o.p[b] == o.tag;
            CHECK(ok);                              // contents intact until freed
            CHECK_EQ(kmem_cache_free(cs[o.cache], o.p), 0);
            live.erase(live.begin() + (long)i);
        }
        if (op % 500 == 0) {
            uint32_t n0 = 0, n1 = 0;
            for (auto &o : live) (o.cache ? n1 : n0)++;
            CHECK_EQ(kmem_cache_get_stats(cs[0]).objects_in_use, n0);
            CHECK_EQ(kmem_cache_get_stats(cs[1]).objects_in_use, n1);
        }
    }
    for (auto &o : live) CHECK_EQ(kmem_cache_free(cs[o.cache], o.p), 0);
    CHECK_EQ(h.free_frames(), 128u);
}

static void test_kmalloc_all_size_classes()
{
    Heap h(512);
    const size_t sizes[] = { 1, 16, 17, 100, 2048, 2049, 4096, 5000, 9000, 40000 };
    std::vector<std::pair<uint8_t *, size_t>> live;
    uint8_t tag = 1;
    for (size_t s : sizes) {
        uint8_t *p = (uint8_t *)kmalloc(h.x, s);
        CHECK(p != nullptr && h.inside(p));
        if (s > 2048) CHECK_EQ((uintptr_t)(p - h.mem.data()) % 4096, 0u);   // large = whole pages
        std::memset(p, tag++, s);
        live.push_back({ p, s });
    }
    uint8_t t = 1;
    for (auto &pr : live) {
        bool ok = true;
        for (size_t i = 0; i < pr.second; i++) ok &= pr.first[i] == t;
        CHECK(ok);
        t++;
    }
    CHECK(kmalloc(h.x, (size_t)5 * 1024 * 1024) == nullptr);        // beyond the largest buddy block
    for (auto it = live.rbegin(); it != live.rend(); ++it) CHECK_EQ(kfree(h.x, it->first), 0);
    CHECK_EQ(h.free_frames(), 512u);
}

static void test_heap_exhaustion_and_recovery()
{
    Heap h(8);
    std::vector<void *> v;
    while (void *p = kmalloc(h.x, 4096)) v.push_back(p);
    CHECK_EQ((int)v.size(), 8);                     // every frame used once, then NULL
    for (void *p : v) CHECK_EQ(kfree(h.x, p), 0);
    void *whole = kmalloc(h.x, 8 * 4096);           // order-3 block: only possible if frames merged back
    CHECK(whole != nullptr);
    CHECK_EQ(kfree(h.x, whole), 0);
}

static void test_kfree_rejects_bad_pointers()
{
    Heap h(16);
    uint8_t *big = (uint8_t *)kmalloc(h.x, 3 * 4096);   // order-2 block (4 frames)
    CHECK_EQ(kfree(h.x, big + 4096), -1);               // interior frame
    CHECK_EQ(kfree(h.x, big + 8), -1);                  // unaligned
    CHECK_EQ(kfree(h.x, nullptr), -1);
    CHECK_EQ(kfree(h.x, big), 0);
    CHECK_EQ(kfree(h.x, big), -1);                      // double free
    uint8_t *small = (uint8_t *)kmalloc(h.x, 50);
    CHECK_EQ(kfree(h.x, small), 0);
    CHECK_EQ(kfree(h.x, small), -1);
    CHECK_EQ(h.free_frames(), 16u);
}

int main()
{
    RUN(test_cache_basics);
    RUN(test_size_rounding_and_limits);
    RUN(test_bad_frees_detected);
    RUN(test_one_object_per_slab);
    RUN(test_random_churn_with_shadow);
    RUN(test_kmalloc_all_size_classes);
    RUN(test_heap_exhaustion_and_recovery);
    RUN(test_kfree_rejects_bad_pointers);
    return TEST_SUMMARY();
}
