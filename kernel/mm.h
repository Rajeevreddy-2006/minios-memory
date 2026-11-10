/* MiniOS memory manager.
 *
 *  - Physical memory: a bitmap frame allocator (PMM) over a fixed pool of 4 KiB frames.
 *  - Virtual memory: x86-style 2-level page tables (10 | 10 | 12 bit split) with
 *    PRESENT / WRITE / USER / ACCESSED / DIRTY bits, one address space per process.
 *  - Demand paging: vm_map() only reserves; frames are allocated on first touch.
 *  - Swap: when frames run out a victim is chosen (FIFO, LRU or Clock), written
 *    to a swap area if it holds data, and its PTE becomes SWAPPED.
 *
 * In the hosted build "physical memory" and swap are host buffers and vm_read /
 * vm_write play the role of the MMU. On bare metal the same page-table layout
 * is loaded into CR3 and the fault path below becomes the #PF handler.
 */
#ifndef MINIOS_MM_H
#define MINIOS_MM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAGE_SIZE  4096u
#define PAGE_SHIFT 12

#define PTE_PRESENT  (1u << 0)
#define PTE_WRITE    (1u << 1)
#define PTE_USER     (1u << 2)
#define PTE_ACCESSED (1u << 5)
#define PTE_DIRTY    (1u << 6)
#define PTE_RESERVED (1u << 9)   /* software: mapped, not yet backed (demand-zero) */
#define PTE_SWAPPED  (1u << 10)  /* software: contents in swap, slot in bits 31:12 */
#define PTE_ZSWAPPED (1u << 11)  /* software: contents in the compressed pool, entry id in bits 31:12 */

#define VM_OK      0
#define VM_EFAULT -1   /* unmapped address or protection violation (SIGSEGV) */
#define VM_ENOMEM -2   /* no frame can be freed, or swap is full */
#define VM_EINVAL -3   /* bad argument (alignment, range already mapped, ...) */

typedef enum { REPL_FIFO, REPL_LRU, REPL_CLOCK } repl_policy_t;

typedef struct {
    uint64_t accesses;
    uint64_t faults;        /* minor + major + zpool */
    uint64_t minor_faults;  /* demand-zero fills (no disk I/O) */
    uint64_t major_faults;  /* page read back from disk swap */
    uint64_t zpool_faults;  /* page decompressed from the in-RAM pool (no disk I/O) */
    uint64_t evictions;
    uint64_t swap_reads;
    uint64_t swap_writes;
} vm_stats_t;

typedef struct aspace aspace_t;

/* Configuration. `nframes` is the TOTAL RAM budget. If zpool_frames > 0, that many
 * frames are carved out of it for the kernel heap (buddy + slab) that stores
 * compressed pages, leaving nframes - zpool_frames frames for resident user pages.
 * So compression is never "free memory": the pool competes with user pages. */
typedef struct {
    uint32_t      nframes;
    uint32_t      nswap_slots;
    repl_policy_t policy;
    uint32_t      zpool_frames;       /* 0 = compressed pool disabled */
    uint32_t      zpool_max_entries;  /* max pages held in the pool; 0 = 16384 */
    uint32_t      zpool_max_obj;      /* reject pages that compress to more bytes than this;
                                         0 = 2048 (must compress at least 2:1) */
} mm_config_t;

/* lifecycle */
int      mm_init_ex(const mm_config_t *cfg);
int      mm_init(uint32_t nframes, uint32_t nswap_slots, repl_policy_t policy);  /* pool disabled */
void     mm_shutdown(void);

/* physical frame allocator; frames taken here are pinned (never evicted) */
int      pmm_alloc_frame(void);          /* frame index or -1 */
void     pmm_free_frame(int frame);
uint32_t pmm_free_frames(void);

/* address spaces */
aspace_t *vm_create_aspace(void);
void      vm_destroy_aspace(aspace_t *as);
int       vm_map(aspace_t *as, uint32_t va, uint32_t npages, uint32_t flags); /* flags: PTE_WRITE|PTE_USER */
int       vm_unmap(aspace_t *as, uint32_t va, uint32_t npages);

/* memory access through the "MMU" (may fault and page in) */
int       vm_read(aspace_t *as, uint32_t va, void *buf, uint32_t len);
int       vm_write(aspace_t *as, uint32_t va, const void *buf, uint32_t len);
int       vm_touch(aspace_t *as, uint32_t va, int is_write);

/* Compressed pool statistics. Counters accumulate until vm_reset_stats();
 * the "live" / "pool_frames" fields are gauges describing the current state. */
typedef struct {
    uint64_t stores;            /* pages placed in the pool */
    uint64_t samefill_stores;   /* of which all-same-word pages (stored as 8 bytes, no compression) */
    uint64_t rejects;           /* pages that compressed too poorly and went to disk instead */
    uint64_t loads;             /* pages brought back from the pool */
    uint64_t writebacks;        /* pool entries pushed out to disk swap to make room */
    uint64_t bytes_in;          /* uncompressed bytes of compressed stores (excl. same-fill) */
    uint64_t bytes_out;         /* their compressed size */
    uint64_t compress_ns, decompress_ns;   /* measured CPU time */
    uint32_t live_entries;      /* pages currently held (including same-fill) */
    uint32_t live_samefill;
    uint64_t live_bytes;        /* compressed bytes currently held */
    uint32_t pool_frames_total; /* heap frames reserved for the pool */
    uint32_t pool_frames_used;  /* heap frames currently holding slabs */
} zpool_stats_t;

/* introspection */
vm_stats_t   vm_get_stats(void);
zpool_stats_t zpool_get_stats(void);
void         vm_reset_stats(void);      /* resets VM and pool counters */
uint32_t   vm_resident_pages(const aspace_t *as);

/* ---- offline page-replacement analyser (pagesim.c) ---------------------
 * Replays a reference string against N frames and returns the fault count.
 * OPT needs future knowledge, so it is only available here, not in the kernel. */
typedef enum { ALGO_FIFO, ALGO_LRU, ALGO_CLOCK, ALGO_OPT } page_algo_t;
int pagesim_faults(page_algo_t algo, int nframes, const int *refs, int n);
const char *page_algo_name(page_algo_t a);

#ifdef __cplusplus
}
#endif
#endif /* MINIOS_MM_H */
