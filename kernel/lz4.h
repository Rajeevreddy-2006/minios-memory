/* LZ4-style block compressor, written from scratch for MiniOS.
 *
 * It follows the public LZ4 *block* format (token byte with literal/match
 * nibbles, 2-byte little-endian offsets, 255-run length extensions, final
 * literals-only sequence) with a single-probe greedy matcher over a 4096-entry
 * hash table. Chosen because it is fast and simple, which is exactly what you
 * want on the page-eviction path.
 *
 * Inputs are limited to 65535 bytes so match offsets always fit in 16 bits and
 * the hash table can hold uint16 positions (a page is 4096).
 */
#ifndef MINIOS_LZ4_H
#define MINIOS_LZ4_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LZ4_MAX_INPUT 65535

/* Worst-case compressed size for n input bytes. */
int lz4_compress_bound(int n);

/* Returns the compressed size, or -1 if it would not fit in `cap` bytes
 * (or the arguments are invalid). Never writes more than `cap` bytes. */
int lz4_compress(const uint8_t *src, int n, uint8_t *dst, int cap);

/* Returns the decompressed size, or -1 on malformed input or if the output
 * would exceed `cap`. Safe on untrusted input: every read and write is bounds-checked. */
int lz4_decompress(const uint8_t *src, int n, uint8_t *dst, int cap);

#ifdef __cplusplus
}
#endif
#endif /* MINIOS_LZ4_H */
