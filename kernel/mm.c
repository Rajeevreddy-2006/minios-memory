#define _POSIX_C_SOURCE 200809L   /* clock_gettime */
#include "mm.h"
#include "buddy.h"
#include "slab.h"
#include "lz4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

_Static_assert(PAGE_SIZE == BUDDY_FRAME_SIZE, "VM page size must match the allocator frame size");

/* ---- global memory-manager state --------------------------------------- */

typedef struct {
    int       used;        /* holds a user page (evictable)? pinned frames have used = 0 */
    aspace_t *as;          /* owner */
    uint32_t  vpn;         /* virtual page number in the owner */
    uint64_t  loaded_at;   /* FIFO key */
    uint64_t  last_used;   /* LRU key */
    int       swap_slot;   /* disk-swap slot holding a still-valid copy, or -1 */
} frame_t;

struct aspace {
    uint32_t *dir[1024];   /* page tables; the "page directory" */
};

static struct {
    int           ready;
    uint8_t      *mem;           /* all RAM: user frames first, then the kernel-heap frames */
    uint32_t      nframes;       /* USER frames (evictable pool) */
    uint32_t      free_frames;
    uint32_t     *fbitmap;       /* 1 = allocated */
    frame_t      *frames;
    uint8_t      *swap;
    uint32_t      nswap;
    uint32_t     *sbitmap;
    repl_policy_t policy;
    uint32_t      clock_hand;
    uint64_t      clock;         /* logical time, bumped on every access */
    vm_stats_t    stats;
} M;

/* ---- bitmap helpers ------------------------------------------------------ */

static int bm_alloc(uint32_t *bm, uint32_t n)
{
    for (uint32_t w = 0; w < (n + 31) / 32; w++) {
        if (bm[w] == 0xFFFFFFFFu) continue;
        for (uint32_t b = 0; b < 32; b++) {
            uint32_t i = w * 32 + b;
            if (i >= n) return -1;
            if (!(bm[w] & (1u << b))) { bm[w] |= 1u << b; return (int)i; }
        }
    }
    return -1;
}

static void bm_free(uint32_t *bm, uint32_t i) { bm[i / 32] &= ~(1u << (i % 32)); }

/* ---- physical frame allocator (user frames) ------------------------------ */

int pmm_alloc_frame(void)
{
    if (!M.ready) return -1;
    int f = bm_alloc(M.fbitmap, M.nframes);
    if (f >= 0) M.free_frames--;
    return f;
}

void pmm_free_frame(int f)
{
    if (!M.ready || f < 0 || (uint32_t)f >= M.nframes) return;
    bm_free(M.fbitmap, (uint32_t)f);
    M.free_frames++;
}

uint32_t pmm_free_frames(void) { return M.ready ? M.free_frames : 0; }

/* ---- page tables ----------------------------------------------------------- */

static uint32_t *pte_lookup(aspace_t *as, uint32_t va, int create)
{
    uint32_t di = va >> 22, ti = (va >> 12) & 0x3FF;
    if (!as->dir[di]) {
        if (!create) return NULL;
        as->dir[di] = calloc(1024, sizeof(uint32_t));
        if (!as->dir[di]) return NULL;
    }
    return &as->dir[di][ti];
}

/* ========================================================================== *
 *  Compressed page pool (zswap-style)
 *
 *  On eviction a dirty page is compressed into an object from the kernel heap
 *  (slab size classes over the buddy allocator) instead of being written to disk.
 *  Pages whose every 8-byte word is identical are stored as just that word.
 *  Loads are exclusive: the pool entry is freed when the page is faulted back in,
 *  and the page is marked dirty so it is saved again if evicted later.
 *  When the pool is full the oldest entries are written back to disk swap.
 * ========================================================================== */

enum { ZE_SAMEFILL = 1, ZE_COMPRESSED = 2 };

typedef struct {
    int       used;
    int       kind;
    aspace_t *as;          /* owner, needed to repoint the PTE on writeback */
    uint32_t  vpn;
    uint32_t  off;         /* byte offset of the compressed blob within the heap */
    uint16_t  csize;
    uint16_t  cls;         /* slab size class holding the blob */
    uint64_t  fill;        /* SAMEFILL pattern */
    int32_t   older, newer;/* FIFO links; Z.oldest is written back first */
} zentry_t;

static const uint32_t ZP_SIZES[] = { 32, 64, 96, 128, 192, 256, 384, 512, 640, 768,
                                     1024, 1280, 1536, 2048 };
#define ZP_NCLASSES   ((int)(sizeof ZP_SIZES / sizeof ZP_SIZES[0]))
#define ZP_MAX_OBJ    2048
#define ZP_DEFAULT_ENTRIES 16384
#define ZP_MAX_WRITEBACKS_PER_STORE 8

static struct {
    int           enabled;
    buddy_t      *buddy;
    slab_ctx_t   *slab;
    uint8_t      *heap;                     /* base of the heap frames inside M.mem */
    kmem_cache_t *cls[ZP_NCLASSES];
    zentry_t     *e;
    uint32_t      max_entries;
    uint32_t     *free_ids;
    uint32_t      nfree;
    int32_t       oldest, newest;
    uint32_t      max_obj;
    uint32_t      live, live_same;
    uint64_t      live_bytes;
    zpool_stats_t st;                       /* counters only; gauges are filled on read */
} Z;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int is_samefill(const uint8_t *page, uint64_t *fill)
{
    uint64_t first, w;
    memcpy(&first, page, sizeof first);
    for (size_t i = sizeof first; i < PAGE_SIZE; i += sizeof w) {
        memcpy(&w, page + i, sizeof w);
        if (w != first) return 0;
    }
    *fill = first;
    return 1;
}

static int class_for(uint32_t size)
{
    for (int i = 0; i < ZP_NCLASSES; i++)
        if (ZP_SIZES[i] >= size) return i;
    return -1;
}

static void ze_push_newest(int32_t id)
{
    zentry_t *e = &Z.e[id];
    e->older = Z.newest;
    e->newer = -1;
    if (Z.newest >= 0) Z.e[Z.newest].newer = id; else Z.oldest = id;
    Z.newest = id;
}

static void ze_unlink(int32_t id)
{
    zentry_t *e = &Z.e[id];
    if (e->older >= 0) Z.e[e->older].newer = e->newer; else Z.oldest = e->newer;
    if (e->newer >= 0) Z.e[e->newer].older = e->older; else Z.newest = e->older;
}

static int32_t ze_take_id(void)
{
    return Z.nfree ? (int32_t)Z.free_ids[--Z.nfree] : -1;
}

static void ze_put_id(int32_t id) { Z.free_ids[Z.nfree++] = (uint32_t)id; }

/* Drop an entry and free its heap object. */
static void ze_release(int32_t id)
{
    zentry_t *e = &Z.e[id];
    ze_unlink(id);
    if (e->kind == ZE_COMPRESSED) {
        kmem_cache_free(Z.cls[e->cls], Z.heap + e->off);
        Z.live_bytes -= e->csize;
    } else {
        Z.live_same--;
    }
    e->used = 0;
    Z.live--;
    ze_put_id(id);
}

/* Reconstruct the page held by an entry. 0 on success. */
static int zp_read_into(const zentry_t *e, uint8_t *dst)
{
    if (e->kind == ZE_SAMEFILL) {
        for (size_t i = 0; i < PAGE_SIZE; i += sizeof e->fill) memcpy(dst + i, &e->fill, sizeof e->fill);
        return 0;
    }
    uint64_t t0 = now_ns();
    int rc = lz4_decompress(Z.heap + e->off, e->csize, dst, (int)PAGE_SIZE);
    Z.st.decompress_ns += now_ns() - t0;
    return rc == (int)PAGE_SIZE ? 0 : -1;
}

/* Push the oldest pool entry out to disk swap. Returns 1 if one was written back. */
static int zpool_writeback_oldest(void)
{
    int32_t id = Z.oldest;
    if (id < 0) return 0;
    zentry_t *e = &Z.e[id];

    int slot = bm_alloc(M.sbitmap, M.nswap);
    if (slot < 0) return 0;                                   /* disk swap is full too */
    if (zp_read_into(e, M.swap + (size_t)slot * PAGE_SIZE) != 0) { bm_free(M.sbitmap, (uint32_t)slot); return 0; }
    M.stats.swap_writes++;
    Z.st.writebacks++;

    uint32_t *pte = pte_lookup(e->as, e->vpn << PAGE_SHIFT, 0);
    *pte = (*pte & (PTE_WRITE | PTE_USER)) | PTE_SWAPPED | ((uint32_t)slot << PAGE_SHIFT);
    ze_release(id);
    return 1;
}

/* Try to place a page in the pool. Returns an entry id, or -1 if the page
 * should go to disk instead (incompressible, or no room even after writeback). */
static int zpool_store(aspace_t *as, uint32_t vpn, const uint8_t *page)
{
    uint8_t tmp[ZP_MAX_OBJ];
    uint64_t fill = 0;
    int same = is_samefill(page, &fill);
    int clen = 0, cls = 0;

    if (!same) {
        uint64_t t0 = now_ns();
        clen = lz4_compress(page, (int)PAGE_SIZE, tmp, (int)Z.max_obj);
        Z.st.compress_ns += now_ns() - t0;
        if (clen < 0) { Z.st.rejects++; return -1; }
        cls = class_for((uint32_t)clen);
    }

    int writebacks = 0;
    int32_t id;
    while ((id = ze_take_id()) < 0)
        if (writebacks++ >= ZP_MAX_WRITEBACKS_PER_STORE || !zpool_writeback_oldest()) return -1;

    uint8_t *obj = NULL;
    if (!same) {
        while (!(obj = kmem_cache_alloc(Z.cls[cls]))) {
            if (writebacks++ >= ZP_MAX_WRITEBACKS_PER_STORE || !zpool_writeback_oldest()) {
                ze_put_id(id);
                return -1;
            }
        }
        memcpy(obj, tmp, (size_t)clen);
    }

    zentry_t *e = &Z.e[id];
    e->used = 1;
    e->kind = same ? ZE_SAMEFILL : ZE_COMPRESSED;
    e->as = as;
    e->vpn = vpn;
    e->off = same ? 0 : (uint32_t)(obj - Z.heap);
    e->csize = (uint16_t)clen;
    e->cls = (uint16_t)cls;
    e->fill = fill;
    ze_push_newest(id);

    Z.live++;
    Z.st.stores++;
    if (same) {
        Z.live_same++;
        Z.st.samefill_stores++;
    } else {
        Z.live_bytes += (uint64_t)clen;
        Z.st.bytes_in += PAGE_SIZE;
        Z.st.bytes_out += (uint64_t)clen;
    }
    return id;
}

static void zp_shutdown(void)
{
    slab_ctx_destroy(Z.slab);
    buddy_destroy(Z.buddy);
    free(Z.e);
    free(Z.free_ids);
    memset(&Z, 0, sizeof Z);
}

static int zp_init(uint32_t heap_frames, uint32_t max_entries, uint32_t max_obj, uint8_t *heap)
{
    memset(&Z, 0, sizeof Z);
    if (heap_frames == 0) return VM_OK;                       /* feature off */

    Z.heap = heap;
    Z.oldest = Z.newest = -1;
    Z.max_obj = max_obj ? max_obj : ZP_MAX_OBJ;
    if (Z.max_obj > ZP_MAX_OBJ) Z.max_obj = ZP_MAX_OBJ;
    Z.max_entries = max_entries ? max_entries : ZP_DEFAULT_ENTRIES;
    Z.buddy = buddy_create(heap_frames);
    Z.slab = Z.buddy ? slab_ctx_create(Z.buddy, heap) : NULL;
    Z.e = calloc(Z.max_entries, sizeof(zentry_t));
    Z.free_ids = malloc(Z.max_entries * sizeof(uint32_t));
    int ok = Z.slab && Z.e && Z.free_ids;
    for (int i = 0; ok && i < ZP_NCLASSES; i++) {
        char nm[24];
        snprintf(nm, sizeof nm, "zpool-%u", ZP_SIZES[i]);
        ok = (Z.cls[i] = kmem_cache_create(Z.slab, nm, ZP_SIZES[i])) != NULL;
    }
    if (!ok) { zp_shutdown(); return VM_ENOMEM; }

    for (uint32_t i = 0; i < Z.max_entries; i++) Z.free_ids[i] = Z.max_entries - 1 - i;
    Z.nfree = Z.max_entries;
    Z.enabled = 1;
    return VM_OK;
}

zpool_stats_t zpool_get_stats(void)
{
    zpool_stats_t s = Z.st;
    s.live_entries = Z.live;
    s.live_samefill = Z.live_same;
    s.live_bytes = Z.live_bytes;
    if (Z.enabled) {
        buddy_stats_t b = buddy_get_stats(Z.buddy);
        s.pool_frames_total = b.total_frames;
        s.pool_frames_used = b.total_frames - b.free_frames;
    }
    return s;
}

/* ---- address spaces ------------------------------------------------------- */

/* Drop whatever backs this PTE (frame, swap slot or pool entry) and clear it. */
static void release_pte(uint32_t *pte)
{
    if (*pte & PTE_PRESENT) {
        uint32_t f = *pte >> PAGE_SHIFT;
        frame_t *fr = &M.frames[f];
        if (fr->swap_slot >= 0) bm_free(M.sbitmap, (uint32_t)fr->swap_slot);
        memset(fr, 0, sizeof *fr);
        fr->swap_slot = -1;
        pmm_free_frame((int)f);
    } else if (*pte & PTE_SWAPPED) {
        bm_free(M.sbitmap, *pte >> PAGE_SHIFT);
    } else if (*pte & PTE_ZSWAPPED) {
        ze_release((int32_t)(*pte >> PAGE_SHIFT));
    }
    *pte = 0;
}

aspace_t *vm_create_aspace(void)
{
    return M.ready ? calloc(1, sizeof(aspace_t)) : NULL;
}

void vm_destroy_aspace(aspace_t *as)
{
    if (!as) return;
    for (int d = 0; d < 1024; d++) {
        if (!as->dir[d]) continue;
        for (int t = 0; t < 1024; t++) release_pte(&as->dir[d][t]);
        free(as->dir[d]);
    }
    free(as);
}

int vm_map(aspace_t *as, uint32_t va, uint32_t npages, uint32_t flags)
{
    if (!M.ready || !as || (va & (PAGE_SIZE - 1)) || npages == 0) return VM_EINVAL;
    if ((uint64_t)va + (uint64_t)npages * PAGE_SIZE > 0x100000000ull) return VM_EINVAL;
    for (uint32_t i = 0; i < npages; i++) {          /* check first: all-or-nothing */
        uint32_t *pte = pte_lookup(as, va + i * PAGE_SIZE, 0);
        if (pte && *pte) return VM_EINVAL;
    }
    for (uint32_t i = 0; i < npages; i++) {
        uint32_t *pte = pte_lookup(as, va + i * PAGE_SIZE, 1);
        if (!pte) { vm_unmap(as, va, i); return VM_ENOMEM; }
        *pte = PTE_RESERVED | (flags & (PTE_WRITE | PTE_USER));
    }
    return VM_OK;
}

int vm_unmap(aspace_t *as, uint32_t va, uint32_t npages)
{
    if (!M.ready || !as || (va & (PAGE_SIZE - 1))) return VM_EINVAL;
    for (uint32_t i = 0; i < npages; i++) {
        uint32_t *pte = pte_lookup(as, va + i * PAGE_SIZE, 0);
        if (pte) release_pte(pte);
    }
    return VM_OK;
}

/* ---- page replacement ----------------------------------------------------- */

static int pick_victim(void)
{
    int v = -1;
    switch (M.policy) {
    case REPL_FIFO:
        for (uint32_t f = 0; f < M.nframes; f++)
            if (M.frames[f].used && (v < 0 || M.frames[f].loaded_at < M.frames[v].loaded_at))
                v = (int)f;
        return v;
    case REPL_LRU:
        for (uint32_t f = 0; f < M.nframes; f++)
            if (M.frames[f].used && (v < 0 || M.frames[f].last_used < M.frames[v].last_used))
                v = (int)f;
        return v;
    case REPL_CLOCK: {
        /* second chance: skip (and clear) recently-referenced pages */
        for (uint32_t step = 0; step < 2 * M.nframes; step++) {
            uint32_t f = M.clock_hand;
            if (!M.frames[f].used) { M.clock_hand = (f + 1) % M.nframes; continue; }
            uint32_t *pte = pte_lookup(M.frames[f].as, M.frames[f].vpn << PAGE_SHIFT, 0);
            if (*pte & PTE_ACCESSED) {
                *pte &= ~PTE_ACCESSED;
                M.clock_hand = (f + 1) % M.nframes;
            } else {
                M.clock_hand = (f + 1) % M.nframes;
                return (int)f;
            }
        }
        return -1;
    }
    }
    return -1;
}

/* Save the victim (compressed pool, then disk) if it holds data, and rewrite its PTE. */
static int evict(int f)
{
    frame_t *fr = &M.frames[f];
    uint32_t *pte = pte_lookup(fr->as, fr->vpn << PAGE_SHIFT, 0);
    uint32_t keep = *pte & (PTE_WRITE | PTE_USER);
    const uint8_t *data = M.mem + (size_t)f * PAGE_SIZE;

    if (*pte & PTE_DIRTY) {
        int zid = Z.enabled ? zpool_store(fr->as, fr->vpn, data) : -1;
        if (zid >= 0) {
            if (fr->swap_slot >= 0) bm_free(M.sbitmap, (uint32_t)fr->swap_slot);   /* stale copy */
            *pte = keep | PTE_ZSWAPPED | ((uint32_t)zid << PAGE_SHIFT);
        } else {
            int slot = fr->swap_slot;
            if (slot < 0) slot = bm_alloc(M.sbitmap, M.nswap);
            if (slot < 0) return VM_ENOMEM;                 /* swap full */
            memcpy(M.swap + (size_t)slot * PAGE_SIZE, data, PAGE_SIZE);
            M.stats.swap_writes++;
            *pte = keep | PTE_SWAPPED | ((uint32_t)slot << PAGE_SHIFT);
        }
    } else if (fr->swap_slot >= 0) {
        /* clean and the disk copy is still valid: no I/O needed */
        *pte = keep | PTE_SWAPPED | ((uint32_t)fr->swap_slot << PAGE_SHIFT);
    } else {
        /* never written: contents are all zeroes, just forget it */
        *pte = keep | PTE_RESERVED;
    }
    M.stats.evictions++;
    memset(fr, 0, sizeof *fr);
    fr->swap_slot = -1;
    return VM_OK;
}

/* Obtain a free frame, evicting one if memory is full. */
static int frame_get(void)
{
    int f = pmm_alloc_frame();
    if (f >= 0) return f;
    int v = pick_victim();
    if (v < 0) return VM_ENOMEM;
    int rc = evict(v);
    if (rc) return rc;
    return v;                       /* still marked allocated in the bitmap: reuse it */
}

/* ---- the page-fault path ---------------------------------------------------- */

static int handle_fault(aspace_t *as, uint32_t va, int is_write)
{
    uint32_t *pte = pte_lookup(as, va, 0);
    if (!pte || !(*pte & (PTE_RESERVED | PTE_SWAPPED | PTE_ZSWAPPED))) return VM_EFAULT;  /* SIGSEGV */
    if (is_write && !(*pte & PTE_WRITE)) return VM_EFAULT;                                /* protection */

    int f = frame_get();
    if (f < 0) return f;

    /* Re-read the PTE: making room may have written this page's pool entry back to disk. */
    uint32_t cur = *pte;
    uint8_t *dst = M.mem + (size_t)f * PAGE_SIZE;
    frame_t *fr = &M.frames[f];
    uint32_t flags = cur & (PTE_WRITE | PTE_USER);
    uint32_t extra = 0;

    if (cur & PTE_ZSWAPPED) {
        int32_t id = (int32_t)(cur >> PAGE_SHIFT);
        if (zp_read_into(&Z.e[id], dst) != 0) { pmm_free_frame(f); return VM_EFAULT; }
        ze_release(id);                     /* exclusive load: the pool no longer holds it ... */
        fr->swap_slot = -1;
        extra = PTE_DIRTY;                  /* ... so it must be saved again on the next eviction */
        Z.st.loads++;
        M.stats.zpool_faults++;
    } else if (cur & PTE_SWAPPED) {
        uint32_t slot = cur >> PAGE_SHIFT;
        memcpy(dst, M.swap + (size_t)slot * PAGE_SIZE, PAGE_SIZE);
        fr->swap_slot = (int)slot;          /* keep the copy: a clean re-eviction is free */
        M.stats.swap_reads++;
        M.stats.major_faults++;
    } else {
        memset(dst, 0, PAGE_SIZE);
        fr->swap_slot = -1;
        M.stats.minor_faults++;
    }
    M.stats.faults++;

    fr->used = 1;
    fr->as = as;
    fr->vpn = va >> PAGE_SHIFT;
    fr->loaded_at = ++M.clock;
    fr->last_used = M.clock;
    *pte = ((uint32_t)f << PAGE_SHIFT) | PTE_PRESENT | flags | extra;
    return VM_OK;
}

/* One memory access as the MMU would perform it. */
static int access_byte(aspace_t *as, uint32_t va, int is_write, uint8_t *rd, uint8_t wr)
{
    uint32_t *pte = pte_lookup(as, va, 0);
    M.stats.accesses++;
    if (!pte || !(*pte & PTE_PRESENT)) {
        int rc = handle_fault(as, va, is_write);
        if (rc) return rc;
        pte = pte_lookup(as, va, 0);
    } else if (is_write && !(*pte & PTE_WRITE)) {
        return VM_EFAULT;
    }
    *pte |= PTE_ACCESSED | (is_write ? PTE_DIRTY : 0);
    uint32_t f = *pte >> PAGE_SHIFT;
    M.frames[f].last_used = ++M.clock;
    uint8_t *p = M.mem + (size_t)f * PAGE_SIZE + (va & (PAGE_SIZE - 1));
    if (is_write) *p = wr; else if (rd) *rd = *p;
    return VM_OK;
}

int vm_touch(aspace_t *as, uint32_t va, int is_write)
{
    if (!M.ready || !as) return VM_EINVAL;
    uint8_t dummy;
    return access_byte(as, va, is_write, &dummy, 0);
}

int vm_read(aspace_t *as, uint32_t va, void *buf, uint32_t len)
{
    if (!M.ready || !as || (!buf && len)) return VM_EINVAL;
    uint8_t *out = buf;
    for (uint32_t i = 0; i < len; i++) {
        int rc = access_byte(as, va + i, 0, &out[i], 0);
        if (rc) return rc;
    }
    return VM_OK;
}

int vm_write(aspace_t *as, uint32_t va, const void *buf, uint32_t len)
{
    if (!M.ready || !as || (!buf && len)) return VM_EINVAL;
    const uint8_t *in = buf;
    for (uint32_t i = 0; i < len; i++) {
        int rc = access_byte(as, va + i, 1, NULL, in[i]);
        if (rc) return rc;
    }
    return VM_OK;
}

/* ---- introspection --------------------------------------------------------- */

vm_stats_t vm_get_stats(void) { return M.stats; }

void vm_reset_stats(void)
{
    memset(&M.stats, 0, sizeof M.stats);
    memset(&Z.st, 0, sizeof Z.st);
}

uint32_t vm_resident_pages(const aspace_t *as)
{
    uint32_t n = 0;
    for (uint32_t f = 0; M.ready && f < M.nframes; f++)
        if (M.frames[f].used && M.frames[f].as == as) n++;
    return n;
}

/* ---- lifecycle --------------------------------------------------------------- */

int mm_init_ex(const mm_config_t *cfg)
{
    if (M.ready || !cfg) return VM_EINVAL;
    uint32_t total = cfg->nframes, zf = cfg->zpool_frames, nswap = cfg->nswap_slots;
    if (total == 0 || total > (1u << 20) || nswap > (1u << 20) || zf >= total) return VM_EINVAL;
    uint32_t user = total - zf;

    memset(&M, 0, sizeof M);
    M.mem     = calloc((size_t)total, PAGE_SIZE);
    M.fbitmap = calloc((user + 31) / 32, sizeof(uint32_t));
    M.frames  = calloc(user, sizeof(frame_t));
    M.swap    = calloc(nswap ? nswap : 1, PAGE_SIZE);
    M.sbitmap = calloc((nswap + 31) / 32 + 1, sizeof(uint32_t));
    int ok = M.mem && M.fbitmap && M.frames && M.swap && M.sbitmap;
    if (ok && zp_init(zf, cfg->zpool_max_entries, cfg->zpool_max_obj,
                      M.mem + (size_t)user * PAGE_SIZE) != VM_OK)
        ok = 0;
    if (!ok) {
        free(M.mem); free(M.fbitmap); free(M.frames); free(M.swap); free(M.sbitmap);
        memset(&M, 0, sizeof M);
        return VM_ENOMEM;
    }
    for (uint32_t i = 0; i < user; i++) M.frames[i].swap_slot = -1;
    M.nframes = M.free_frames = user;
    M.nswap = nswap;
    M.policy = cfg->policy;
    M.ready = 1;
    return VM_OK;
}

int mm_init(uint32_t nframes, uint32_t nswap, repl_policy_t policy)
{
    mm_config_t cfg = { nframes, nswap, policy, 0, 0, 0 };
    return mm_init_ex(&cfg);
}

void mm_shutdown(void)
{
    if (!M.ready) return;
    zp_shutdown();
    free(M.mem); free(M.fbitmap); free(M.frames); free(M.swap); free(M.sbitmap);
    memset(&M, 0, sizeof M);
}
