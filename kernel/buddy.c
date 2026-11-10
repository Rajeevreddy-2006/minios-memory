#include "buddy.h"
#include <stdlib.h>
#include <string.h>

struct buddy {
    uint32_t n;
    uint32_t free_frames;
    int32_t  head[BUDDY_MAX_ORDER + 1];
    int32_t *next, *prev;     /* doubly linked free lists, linked by frame index */
    uint8_t *free_order;      /* order+1 if this frame heads a free block, else 0 */
    uint8_t *alloc_order;     /* order+1 if this frame heads an allocated block, else 0 */
};

static void list_add(buddy_t *b, uint32_t f, unsigned o)
{
    b->next[f] = b->head[o];
    b->prev[f] = -1;
    if (b->head[o] >= 0) b->prev[b->head[o]] = (int32_t)f;
    b->head[o] = (int32_t)f;
    b->free_order[f] = (uint8_t)(o + 1);
}

static void list_del(buddy_t *b, uint32_t f, unsigned o)
{
    int32_t nx = b->next[f], pv = b->prev[f];
    if (pv >= 0) b->next[pv] = nx; else b->head[o] = nx;
    if (nx >= 0) b->prev[nx] = pv;
    b->free_order[f] = 0;
}

buddy_t *buddy_create(uint32_t n)
{
    if (n == 0 || n > (1u << 24)) return NULL;
    buddy_t *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->n = n;
    b->next = malloc(n * sizeof(int32_t));
    b->prev = malloc(n * sizeof(int32_t));
    b->free_order = calloc(n, 1);
    b->alloc_order = calloc(n, 1);
    if (!b->next || !b->prev || !b->free_order || !b->alloc_order) { buddy_destroy(b); return NULL; }
    for (int o = 0; o <= BUDDY_MAX_ORDER; o++) b->head[o] = -1;

    /* Cover [0, n) with the largest aligned blocks that fit (n need not be a power of two). */
    uint32_t i = 0;
    while (i < n) {
        unsigned o = 0;
        while (o < BUDDY_MAX_ORDER && (i & ((1u << (o + 1)) - 1)) == 0 && i + (1u << (o + 1)) <= n)
            o++;
        list_add(b, i, o);
        b->free_frames += 1u << o;
        i += 1u << o;
    }
    return b;
}

void buddy_destroy(buddy_t *b)
{
    if (!b) return;
    free(b->next); free(b->prev); free(b->free_order); free(b->alloc_order);
    free(b);
}

int buddy_alloc(buddy_t *b, unsigned order)
{
    if (!b || order > BUDDY_MAX_ORDER) return -1;
    unsigned o = order;
    while (o <= BUDDY_MAX_ORDER && b->head[o] < 0) o++;
    if (o > BUDDY_MAX_ORDER) return -1;

    uint32_t f = (uint32_t)b->head[o];
    list_del(b, f, o);
    while (o > order) {                 /* split: keep the low half, free the high half */
        o--;
        list_add(b, f + (1u << o), o);
    }
    b->alloc_order[f] = (uint8_t)(order + 1);
    b->free_frames -= 1u << order;
    return (int)f;
}

int buddy_free(buddy_t *b, uint32_t frame, unsigned order)
{
    if (!b || order > BUDDY_MAX_ORDER || frame >= b->n) return -1;
    if (frame & ((1u << order) - 1)) return -1;                 /* misaligned */
    if (b->alloc_order[frame] != order + 1) return -1;          /* double free / wrong order */

    b->alloc_order[frame] = 0;
    b->free_frames += 1u << order;

    while (order < BUDDY_MAX_ORDER) {
        uint32_t bud = frame ^ (1u << order);
        if (bud >= b->n || b->free_order[bud] != order + 1) break;
        list_del(b, bud, order);        /* merge with the free buddy */
        if (bud < frame) frame = bud;
        order++;
    }
    list_add(b, frame, order);
    return 0;
}

int buddy_block_order(const buddy_t *b, uint32_t frame)
{
    if (!b || frame >= b->n || !b->alloc_order[frame]) return -1;
    return (int)b->alloc_order[frame] - 1;
}

buddy_stats_t buddy_get_stats(const buddy_t *b)
{
    buddy_stats_t s;
    memset(&s, 0, sizeof s);
    s.largest_free_order = -1;
    if (!b) return s;
    s.total_frames = b->n;
    s.free_frames = b->free_frames;
    for (int o = 0; o <= BUDDY_MAX_ORDER; o++)
        for (int32_t f = b->head[o]; f >= 0; f = b->next[f]) {
            s.free_blocks[o]++;
            s.largest_free_order = o;
        }
    if (s.free_frames && s.largest_free_order >= 0)
        s.ext_fragmentation = 1.0 - (double)(1u << s.largest_free_order) / s.free_frames;
    return s;
}
