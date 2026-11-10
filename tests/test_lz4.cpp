#include "minitest.h"
extern "C" {
#include "lz4.h"
}
#include <vector>
#include <string>
#include <cstring>

typedef std::vector<uint8_t> Bytes;

static unsigned rng_state = 31337;
static unsigned rnd32()
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}
static int rnd(int lo, int hi) { return lo + (int)(rnd32() % (unsigned)(hi - lo + 1)); }

static Bytes make(int kind, int n)
{
    Bytes v((size_t)n);
    static const char *words[] = { "memory ", "page ", "frame ", "swap ", "kernel ", "buddy ", "slab " };
    size_t w = 0, pos = 0;
    for (int i = 0; i < n; i++) {
        switch (kind) {
        case 0: v[i] = 0; break;                                   // zeros
        case 1: v[i] = (uint8_t)i; break;                          // counting bytes
        case 2: v[i] = (uint8_t)rnd32(); break;                    // random
        case 3: v[i] = (uint8_t)("abcdefg"[i % 7]); break;         // period 7
        case 4: v[i] = (uint8_t)((i / 300) * 11 + (i % 300) % 251);break;   // long-period repeats
        default: {                                                 // word soup
            const char *s = words[w % 7];
            v[i] = (uint8_t)s[pos++];
            if (!s[pos]) { pos = 0; w += (size_t)rnd(1, 3); }
        }
        }
    }
    return v;
}

static bool roundtrip(const Bytes &in)
{
    Bytes comp((size_t)lz4_compress_bound((int)in.size()) + 1);
    int c = lz4_compress(in.data(), (int)in.size(), comp.data(), (int)comp.size() - 1);
    if (c < 0) return false;
    Bytes out(in.size() + 1, 0xEE);
    int d = lz4_decompress(comp.data(), c, out.data(), (int)in.size());
    if (d != (int)in.size()) return false;
    return in.empty() || std::memcmp(in.data(), out.data(), in.size()) == 0;
}

static void test_roundtrip_all_sizes_and_kinds()
{
    for (int kind = 0; kind < 6; kind++) {
        for (int n = 0; n <= 300; n++) CHECK(roundtrip(make(kind, n)));
        for (int n : { 1000, 4095, 4096, 4097, 20000, 65535 }) CHECK(roundtrip(make(kind, n)));
    }
}

static void test_roundtrip_random_mixed_blocks()
{
    for (int round = 0; round < 300; round++) {
        Bytes in;
        int parts = rnd(1, 6);
        for (int p = 0; p < parts; p++) {
            Bytes seg = make(rnd(0, 5), rnd(1, 900));
            in.insert(in.end(), seg.begin(), seg.end());
        }
        CHECK(roundtrip(in));
    }
}

static void test_compression_effectiveness()
{
    Bytes comp(5000);
    int z = lz4_compress(make(0, 4096).data(), 4096, comp.data(), 5000);
    CHECK(z > 0 && z < 40);                                        // a zero page is a few bytes

    Bytes sentence(4096);
    const std::string s = "the kernel pages out cold memory to swap. ";
    for (int i = 0; i < 4096; i++) sentence[(size_t)i] = (uint8_t)s[(size_t)i % s.size()];
    int t = lz4_compress(sentence.data(), 4096, comp.data(), 5000);
    CHECK(t > 0 && t < 4096 / 20);                                 // repeating text: > 20x

    // randomly ordered words carry real entropy, so a fast matcher only gets ~2-3x
    int w = lz4_compress(make(5, 4096).data(), 4096, comp.data(), 5000);
    CHECK(w > 0 && w < 4096 / 2);

    int r = lz4_compress(make(2, 4096).data(), 4096, comp.data(), 2048);
    CHECK_EQ(r, -1);                                               // random data can't fit in half a page
}

// compress() must never touch memory beyond `cap`, whatever the cap is.
static void test_cap_is_respected()
{
    Bytes in = make(5, 2000);
    Bytes full(4000);
    int need = lz4_compress(in.data(), 2000, full.data(), 4000);
    CHECK(need > 0);
    for (int cap = 0; cap < need; cap++) {
        Bytes buf((size_t)cap + 64, 0xA5);
        int rc = lz4_compress(in.data(), 2000, buf.data(), cap);
        CHECK_EQ(rc, -1);
        bool intact = true;
        for (size_t i = (size_t)cap; i < buf.size(); i++) intact &= buf[i] == 0xA5;
        CHECK(intact);
    }
    Bytes exact((size_t)need + 8, 0xA5);
    CHECK_EQ(lz4_compress(in.data(), 2000, exact.data(), need), need);   // exactly enough is enough
    CHECK(exact[(size_t)need] == 0xA5);
}

// Hand-assembled streams written directly from the LZ4 block-format description.
static void test_known_vectors()
{
    uint8_t out[64];
    {   // 4 literals "abcd", then match offset 4, length 4+4 -> "abcd" "abcdabcd"
        const uint8_t s[] = { 0x44, 'a', 'b', 'c', 'd', 0x04, 0x00 };
        CHECK_EQ(lz4_decompress(s, sizeof s, out, 64), 12);
        CHECK(std::memcmp(out, "abcdabcdabcd", 12) == 0);
    }
    {   // overlapping copy: 1 literal, offset 1, length 6+4 -> run of 11 'a'
        const uint8_t s[] = { 0x16, 'a', 0x01, 0x00 };
        CHECK_EQ(lz4_decompress(s, sizeof s, out, 64), 11);
        CHECK(std::memcmp(out, "aaaaaaaaaaa", 11) == 0);
    }
    {   // literal-length extension: 15 + 3 = 18 literals, no match
        uint8_t s[1 + 1 + 18];
        s[0] = 0xF0; s[1] = 3;
        for (int i = 0; i < 18; i++) s[2 + i] = (uint8_t)('A' + i);
        CHECK_EQ(lz4_decompress(s, sizeof s, out, 64), 18);
        CHECK(out[17] == 'R');
    }
    {   // match-length extension: 1 literal, offset 1, 15 + 255 + 10 + 4 = 284 bytes
        uint8_t s[] = { 0x1F, 'x', 0x01, 0x00, 255, 10 };
        Bytes big(400);
        CHECK_EQ(lz4_decompress(s, sizeof s, big.data(), 400), 285);
        bool all = true;
        for (int i = 0; i < 285; i++) all &= big[i] == 'x';
        CHECK(all);
    }
    {   // empty block is a single zero token
        const uint8_t s[] = { 0x00 };
        CHECK_EQ(lz4_decompress(s, 1, out, 64), 0);
        uint8_t c[8];
        CHECK_EQ(lz4_compress(out, 0, c, 8), 1);
        CHECK_EQ(c[0], 0);
    }
}

static void test_malformed_input_rejected()
{
    uint8_t out[64];
    { const uint8_t s[] = { 0x14, 'a', 0x00, 0x00 };           CHECK_EQ(lz4_decompress(s, 4, out, 64), -1); }   // offset 0
    { const uint8_t s[] = { 0x14, 'a', 0x05, 0x00 };           CHECK_EQ(lz4_decompress(s, 4, out, 64), -1); }   // offset before start
    { const uint8_t s[] = { 0x50, 'a', 'b' };                  CHECK_EQ(lz4_decompress(s, 3, out, 64), -1); }   // truncated literals
    { const uint8_t s[] = { 0x14, 'a', 0x01 };                 CHECK_EQ(lz4_decompress(s, 3, out, 64), -1); }   // truncated offset
    { const uint8_t s[] = { 0xF0 };                            CHECK_EQ(lz4_decompress(s, 1, out, 64), -1); }   // length ext missing
    { const uint8_t s[] = { 0x1F, 'a', 0x01, 0x00, 255 };      CHECK_EQ(lz4_decompress(s, 5, out, 64), -1); }   // match ext runs off end
    { const uint8_t s[] = { 0x1F, 'a', 0x01, 0x00, 255, 255, 3 };
      CHECK_EQ(lz4_decompress(s, 7, out, 64), -1); }                                                           // output overflow
    CHECK_EQ(lz4_decompress(nullptr, 3, out, 64), -1);
}

// Garbage and corrupted streams must never crash or write out of bounds
// (the sanitizers are watching); the result is either an error or a size within cap.
static void test_fuzz_decompressor()
{
    Bytes out(4096);
    for (int i = 0; i < 30000; i++) {
        Bytes junk((size_t)rnd(0, 80));
        for (auto &b : junk) b = (uint8_t)rnd32();
        int rc = lz4_decompress(junk.data(), (int)junk.size(), out.data(), 4096);
        CHECK(rc == -1 || (rc >= 0 && rc <= 4096));
    }
    for (int i = 0; i < 3000; i++) {
        Bytes in = make(rnd(0, 5), rnd(50, 3000));
        Bytes comp((size_t)lz4_compress_bound((int)in.size()));
        int c = lz4_compress(in.data(), (int)in.size(), comp.data(), (int)comp.size());
        comp.resize((size_t)c);
        int mode = rnd(0, 2);
        if (mode == 0) comp[(size_t)rnd(0, c - 1)] ^= (uint8_t)(1u << rnd(0, 7));
        else if (mode == 1) comp.resize((size_t)rnd(0, c - 1));
        else for (int k = 0; k < 4; k++) comp[(size_t)rnd(0, c - 1)] = (uint8_t)rnd32();
        Bytes dst(in.size());
        int rc = lz4_decompress(comp.data(), (int)comp.size(), dst.data(), (int)dst.size());
        CHECK(rc == -1 || (rc >= 0 && rc <= (int)in.size()));
    }
}

int main()
{
    RUN(test_roundtrip_all_sizes_and_kinds);
    RUN(test_roundtrip_random_mixed_blocks);
    RUN(test_compression_effectiveness);
    RUN(test_cap_is_respected);
    RUN(test_known_vectors);
    RUN(test_malformed_input_rejected);
    RUN(test_fuzz_decompressor);
    return TEST_SUMMARY();
}
