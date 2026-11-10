#include "slab.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PG        BUDDY_FRAME_SIZE
#define NIL16     0xFFFFu
#define MIN_OBJ   8u
#define MAX_OBJS  (PG / MIN_OBJ)          /* 512 */
#define MAX_CACHES 48
#define N_KCLASS  8

typedef struct {
    kmem_cache_t *cache;                  /* NULL: not a slab page */
    uint16_t free_head;                   /* index of first free object, NIL16 if none */
    uint16_t inuse;
    int32_t  next, prev;                  /* links in the cache's partial list */
    uint8_t  alloc_bits[MAX_OBJS / 8];    /* which objects are allocated (double-free check) */
} slab_page_t;

struct kmem_cache {
    slab_ctx_t *ctx;
    char        name[24];
    uint32_t    obj_size, objs_per_slab;
    int32_t     partial_head;             /* slabs with at least one free object */
    uint32_t    slabs, inuse;
};

struct slab_ctx {
    buddy_t      *b;
    uint8_t      *base;
    uint32_t      nframes;
    slab_page_t  *pages;                  /* indexed by frame, like struct page */
    kmem_cache_t *caches[MAX_CACHES];
    int           ncaches;
    kmem_cache_t *kclass[N_KCLASS];       /* 16, 32, ... 2048 */
};

/* free-list links live inside the free objects themselves */
static uint16_t get_link(const uint8_t *p) { uint16_t v; memcpy(&v, p, sizeof v); return v; }
static void     set_link(uint8_t *p, uint16_t v) { memcpy(p, &v, sizeof v); }

static int  bit_get(const slab_page_t *sp, unsigned i) { return (sp->alloc_bits[i >> 3] >> (i & 7)) & 1; }
static void bit_set(slab_page_t *sp, unsigned i)       { sp->alloc_bits[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static void bit_clr(slab_page_t *sp, unsigned i)       { sp->alloc_bits[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

/* ---- partial list ------------------------------------------------------ */

static void partial_push(kmem_cache_t *c, int32_t f)
{
    slab_page_t *pg = c->ctx->pages;
    pg[f].prev = -1;
    pg[f].next = c->partial_head;
    if (c->partial_head >= 0) pg[c->partial_head].prev = f;
    c->partial_head = f;
}

static void partial_remove(kmem_cache_t *c, int32_t f)
{
    slab_page_t *pg = c->ctx->pages;
    if (pg[f].prev >= 0) pg[pg[f].prev].next = pg[f].next; else c->partial_head = pg[f].next;
    if (pg[f].next >= 0) pg[pg[f].next].prev = pg[f].prev;
}

/* ---- context ------------------------------------------------------------ */

slab_ctx_t *slab_ctx_create(buddy_t *b, uint8_t *base)
{
    if (!b || !base) return NULL;
    slab_ctx_t *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->b = b;
    x->base = base;
    x->nframes = buddy_get_stats(b).total_frames;
    x->pages = calloc(x->nframes, sizeof(slab_page_t));
    if (!x->pages) { free(x); return NULL; }
    for (int i = 0; i < N_KCLASS; i++) {
        char nm[24];
        uint32_t sz = 16u << i;
        snprintf(nm, sizeof nm, "kmalloc-%u", sz);
        x->kclass[i] = kmem_cache_create(x, nm, sz);
        if (!x->kclass[i]) { slab_ctx_destroy(x); return NULL; }
    }
    return x;
}

void slab_ctx_destroy(slab_ctx_t *x)
{
    if (!x) return;
    for (int i = 0; i < x->ncaches; i++) free(x->caches[i]);
    free(x->pages);
    free(x);
}

kmem_cache_t *kmem_cache_create(slab_ctx_t *x, const char *name, uint32_t obj_size)
{
    if (!x || x->ncaches >= MAX_CACHES || obj_size == 0 || obj_size > PG) return NULL;
    obj_size = (obj_size + 7u) & ~7u;
    if (obj_size < MIN_OBJ) obj_size = MIN_OBJ;
    kmem_cache_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->ctx = x;
    snprintf(c->name, sizeof c->name, "%s", name ? name : "cache");
    c->obj_size = obj_size;
    c->objs_per_slab = PG / obj_size;
    c->partial_head = -1;
    x->caches[x->ncaches++] = c;
    return c;
}

kmem_cache_stats_t kmem_cache_get_stats(const kmem_cache_t *c)
{
    kmem_cache_stats_t s = { c->obj_size, c->objs_per_slab, c->slabs, c->inuse };
    return s;
}

/* ---- object alloc / free ------------------------------------------------ */

static int32_t new_slab(kmem_cache_t *c)
{
    slab_ctx_t *x = c->ctx;
    int f = buddy_alloc(x->b, 0);
    if (f < 0) return -1;
    slab_page_t *sp = &x->pages[f];
    memset(sp, 0, sizeof *sp);
    sp->cache = c;
    sp->free_head = 0;
    uint8_t *page = x->base + (size_t)f * PG;
    for (uint32_t i = 0; i < c->objs_per_slab; i++)
        set_link(page + (size_t)i * c->obj_size, (uint16_t)(i + 1 < c->objs_per_slab ? i + 1 : NIL16));
    c->slabs++;
    partial_push(c, f);
    return f;
}

void *kmem_cache_alloc(kmem_cache_t *c)
{
    if (!c) return NULL;
    slab_ctx_t *x = c->ctx;
    int32_t f = c->partial_head;
    if (f < 0 && (f = new_slab(c)) < 0) return NULL;

    slab_page_t *sp = &x->pages[f];
    uint16_t idx = sp->free_head;
    uint8_t *p = x->base + (size_t)f * PG + (size_t)idx * c->obj_size;
    sp->free_head = get_link(p);
    sp->inuse++;
    bit_set(sp, idx);
    c->inuse++;
    if (sp->free_head == NIL16) partial_remove(c, f);       /* slab is now full */
    return p;
}

int kmem_cache_free(kmem_cache_t *c, void *ptr)
{
    if (!c || !ptr) return -1;
    slab_ctx_t *x = c->ctx;
    uint8_t *p = ptr;
    if (p < x->base || p >= x->base + (size_t)x->nframes * PG) return -1;

    size_t off = (size_t)(p - x->base);
    int32_t f = (int32_t)(off / PG);
    slab_page_t *sp = &x->pages[f];
    if (sp->cache != c) return -1;                          /* not ours */
    size_t rel = off % PG;
    if (rel % c->obj_size) return -1;                       /* not an object boundary */
    unsigned idx = (unsigned)(rel / c->obj_size);
    if (idx >= c->objs_per_slab || !bit_get(sp, idx)) return -1;   /* double free */

    bit_clr(sp, idx);
    uint16_t old_head = sp->free_head;
    set_link(p, old_head);
    sp->free_head = (uint16_t)idx;
    sp->inuse--;
    c->inuse--;

    int was_full = (old_head == NIL16);                     /* it was not on the partial list */
    if (sp->inuse == 0) {
        if (!was_full) partial_remove(c, f);
        sp->cache = NULL;
        c->slabs--;
        buddy_free(x->b, (uint32_t)f, 0);                   /* empty page goes back to the buddy */
    } else if (was_full) {
        partial_push(c, f);
    }
    return 0;
}

/* ---- kmalloc / kfree ------------------------------------------------------ */

void *kmalloc(slab_ctx_t *x, size_t size)
{
    if (!x) return NULL;
    if (size <= 2048) {
        int i = 0;
        while ((16u << i) < size) i++;                      /* smallest class that fits */
        return kmem_cache_alloc(x->kclass[i]);
    }
    unsigned order = 0;
    while (((size_t)PG << order) < size) {
        if (++order > BUDDY_MAX_ORDER) return NULL;
    }
    int f = buddy_alloc(x->b, order);
    return f < 0 ? NULL : x->base + (size_t)f * PG;
}

int kfree(slab_ctx_t *x, void *ptr)
{
    if (!x || !ptr) return -1;
    uint8_t *p = ptr;
    if (p < x->base || p >= x->base + (size_t)x->nframes * PG) return -1;
    size_t off = (size_t)(p - x->base);
    uint32_t f = (uint32_t)(off / PG);

    if (x->pages[f].cache) return kmem_cache_free(x->pages[f].cache, ptr);

    if (off % PG) return -1;                                /* interior pointer of a large block */
    int order = buddy_block_order(x->b, f);
    if (order < 0) return -1;
    return buddy_free(x->b, f, (unsigned)order);
}
