/* Slab allocator: fixed-size object caches carved out of single buddy pages,
 * plus kmalloc/kfree on top of power-of-two caches.
 *
 * Why slabs: the buddy allocator can only give whole pages. Packing many small
 * objects into one page with an in-page free list makes small allocations fast
 * and avoids wasting a page per object. A page returns to the buddy allocator
 * as soon as its last object is freed.
 */
#ifndef MINIOS_SLAB_H
#define MINIOS_SLAB_H

#include <stddef.h>
#include <stdint.h>
#include "buddy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct slab_ctx    slab_ctx_t;
typedef struct kmem_cache  kmem_cache_t;

typedef struct {
    uint32_t obj_size;        /* after rounding up to a multiple of 8 */
    uint32_t objs_per_slab;
    uint32_t slabs;           /* pages currently held by this cache */
    uint32_t objects_in_use;
} kmem_cache_stats_t;

/* `base` is the address of frame 0 of the buddy range (frame i = base + 4096*i). */
slab_ctx_t   *slab_ctx_create(buddy_t *b, uint8_t *base);
void          slab_ctx_destroy(slab_ctx_t *ctx);

kmem_cache_t *kmem_cache_create(slab_ctx_t *ctx, const char *name, uint32_t obj_size);
void         *kmem_cache_alloc(kmem_cache_t *c);               /* NULL when the heap is full */
int           kmem_cache_free(kmem_cache_t *c, void *p);       /* 0 ok, -1 bad pointer / double free */
kmem_cache_stats_t kmem_cache_get_stats(const kmem_cache_t *c);

/* General purpose allocation: <= 2048 bytes from slab caches, larger from whole buddy blocks. */
void *kmalloc(slab_ctx_t *ctx, size_t size);
int   kfree(slab_ctx_t *ctx, void *p);                         /* 0 ok, -1 invalid pointer */

#ifdef __cplusplus
}
#endif
#endif /* MINIOS_SLAB_H */
