#include "lz4.h"
#include <string.h>

#define HASH_BITS    12
#define MIN_MATCH    4
#define MF_LIMIT     12   /* a match may not start in the last 12 bytes (LZ4 format rule) */
#define LAST_LITERALS 5   /* the last 5 bytes are always literals (LZ4 format rule) */

int lz4_compress_bound(int n) { return n + n / 255 + 16; }

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }
static uint32_t hash4(uint32_t v)      { return (v * 2654435761u) >> (32 - HASH_BITS); }

/* Append one sequence: `litlen` literals, then (unless matchlen == 0) a match.
 * Returns the new output position, or -1 if it would exceed cap. */
static int emit_seq(uint8_t *dst, int op, int cap, const uint8_t *lit, int litlen,
                    int offset, int matchlen)
{
    int ml = matchlen ? matchlen - MIN_MATCH : 0;
    int need = 1 + litlen + (litlen >= 15 ? (litlen - 15) / 255 + 1 : 0);
    if (matchlen) need += 2 + (ml >= 15 ? (ml - 15) / 255 + 1 : 0);
    if (need > cap - op) return -1;

    int tp = op++;
    int token = (litlen >= 15 ? 15 : litlen) << 4;
    if (litlen >= 15) {
        int r = litlen - 15;
        while (r >= 255) { dst[op++] = 255; r -= 255; }
        dst[op++] = (uint8_t)r;
    }
    if (litlen) memcpy(dst + op, lit, (size_t)litlen);
    op += litlen;

    if (matchlen) {
        dst[op++] = (uint8_t)(offset & 0xFF);
        dst[op++] = (uint8_t)(offset >> 8);
        token |= ml >= 15 ? 15 : ml;
        if (ml >= 15) {
            int r = ml - 15;
            while (r >= 255) { dst[op++] = 255; r -= 255; }
            dst[op++] = (uint8_t)r;
        }
    }
    dst[tp] = (uint8_t)token;
    return op;
}

int lz4_compress(const uint8_t *src, int n, uint8_t *dst, int cap)
{
    if (n < 0 || n > LZ4_MAX_INPUT || cap < 0 || (n > 0 && !src) || (cap > 0 && !dst)) return -1;

    uint16_t table[1 << HASH_BITS];          /* position + 1; 0 = empty */
    memset(table, 0, sizeof table);

    int ip = 0, anchor = 0, op = 0;
    if (n >= MF_LIMIT + 1) {
        const int last_start = n - MF_LIMIT;
        while (ip <= last_start) {
            uint32_t seq = rd32(src + ip);
            uint32_t h = hash4(seq);
            int ref = (int)table[h] - 1;
            table[h] = (uint16_t)(ip + 1);

            if (ref >= 0 && rd32(src + ref) == seq) {
                int mlen = MIN_MATCH;
                while (ip + mlen < n - LAST_LITERALS && src[ref + mlen] == src[ip + mlen]) mlen++;
                op = emit_seq(dst, op, cap, src + anchor, ip - anchor, ip - ref, mlen);
                if (op < 0) return -1;
                ip += mlen;
                anchor = ip;
                if (ip + 2 <= n)             /* seed the table just behind the cursor */
                    table[hash4(rd32(src + ip - 2))] = (uint16_t)(ip - 2 + 1);
            } else {
                ip++;
            }
        }
    }
    return emit_seq(dst, op, cap, src + anchor, n - anchor, 0, 0);
}

int lz4_decompress(const uint8_t *src, int n, uint8_t *dst, int cap)
{
    if (n < 0 || cap < 0 || (n > 0 && !src) || (cap > 0 && !dst)) return -1;
    long ip = 0, op = 0;

    while (ip < n) {
        int token = src[ip++];

        long lit = token >> 4;
        if (lit == 15) {
            int b;
            do {
                if (ip >= n) return -1;
                b = src[ip++];
                lit += b;
            } while (b == 255);
        }
        if (lit > n - ip || lit > cap - op) return -1;
        if (lit) memcpy(dst + op, src + ip, (size_t)lit);
        ip += lit;
        op += lit;
        if (ip == n) break;                  /* last sequence has no match part */

        if (n - ip < 2) return -1;
        long offset = src[ip] | (src[ip + 1] << 8);
        ip += 2;
        if (offset == 0 || offset > op) return -1;

        long ml = token & 15;
        if (ml == 15) {
            int b;
            do {
                if (ip >= n) return -1;
                b = src[ip++];
                ml += b;
            } while (b == 255);
        }
        ml += MIN_MATCH;
        if (ml > cap - op) return -1;
        for (long i = 0; i < ml; i++)        /* byte-wise: source and destination may overlap */
            dst[op + i] = dst[op + i - offset];
        op += ml;
    }
    return (int)op;
}
