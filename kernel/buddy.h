/* Buddy allocator: hands out power-of-two blocks of 4 KiB frames.
 *
 * Free blocks of each order live on per-order lists. Allocation takes the
 * smallest free block that fits and splits it; freeing merges a block with its
 * "buddy" (the neighbour at address ^ size) whenever both are free, so large
 * contiguous runs reappear once their users are gone.
 *
 * The allocator only manages frame *indices* [0, nframes); per-frame metadata
 * is kept in host arrays (the equivalent of Linux's struct page array).
 */
#ifndef MINIOS_BUDDY_H
#define MINIOS_BUDDY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BUDDY_MAX_ORDER 10          /* largest block: 2^10 frames = 4 MiB */
#define BUDDY_FRAME_SIZE 4096u

typedef struct buddy buddy_t;

typedef struct {
    uint32_t total_frames;
    uint32_t free_frames;
    uint32_t free_blocks[BUDDY_MAX_ORDER + 1];  /* free blocks per order */
    int      largest_free_order;                /* -1 if nothing is free */
    double   ext_fragmentation;                 /* 1 - largest_free_block / free_frames */
} buddy_stats_t;

buddy_t *buddy_create(uint32_t nframes);        /* NULL on bad size / out of memory */
void     buddy_destroy(buddy_t *b);

int      buddy_alloc(buddy_t *b, unsigned order);               /* first frame index, or -1 */
int      buddy_free(buddy_t *b, uint32_t frame, unsigned order);/* 0 ok, -1 invalid / double free */
int      buddy_block_order(const buddy_t *b, uint32_t frame);   /* order of the allocated block
                                                                   starting at frame, or -1 */
buddy_stats_t buddy_get_stats(const buddy_t *b);

#ifdef __cplusplus
}
#endif
#endif /* MINIOS_BUDDY_H */
