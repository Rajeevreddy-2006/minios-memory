# minios-memory

A memory manager for a toy operating system, written in C (about 1,400 lines), with C++ tests and benchmarks.
It does what a real kernel does when RAM runs short: pages memory in on demand, picks victims to evict, and
**compresses cold pages in RAM instead of writing them to disk**, so the same amount of RAM holds more data.

No dependencies beyond `gcc`, `g++` and `make`.

## Result

Fixed budget of 256 frames (1 MiB), working data of 400 pages (1.56x the budget), identical access sequence
each run. A 32-frame compressed pool carved out of that same budget:

| | No compression | With 32-frame pool |
|---|---|---|
| Disk I/O operations | 15,655 | 7,897 (**50% fewer**) |
| Pages held in RAM | 256 | 319 (**1.25x effective capacity**) |
| Modelled time, SSD | 1.57 s | 0.90 s (**1.7x faster**) |
| Modelled time, HDD | 125 s | 63 s (**2.0x faster**) |

It is a trade-off: every pool frame is one fewer resident page, so a pool that is too big hurts (128 frames is
slower than no compression, about 3 s vs 1.6 s on SSD). The sweep is in `docs/bench_output.txt`.
*Times are modelled (disk operations x an assumed 100 us SSD / 8 ms HDD latency, plus measured compression CPU
time). The I/O counts and compression ratios are exact for this workload.*

## Quick start

```
make test     # 5 test suites, ~200,000 checks, built with AddressSanitizer + UBSan
make bench    # heap, compressor, effective-RAM and page-replacement benchmarks
```

## How it works

```
page fault -> free frame?  --no--> pick victim (FIFO / LRU / Clock)
                                      |
              clean page?  --yes--> drop it (or reuse its disk copy)
              dirty page?  ------> every 8-byte word identical? --yes--> store just that word (zero pages)
                                      | no
                                      compress (LZ4-style) -> fits in 2 KiB? --yes--> slab object in the pool
                                                                   | no                  | pool full:
                                                                   v                     v oldest entries
                                                               write to disk swap  <-----  written back to disk
```

Key pieces, bottom up:

- **Paging:** x86-style two-level page tables (10 | 10 | 12 bits) with present / write / dirty / accessed bits,
  demand paging, and swap. Replacement policies: FIFO, LRU, Clock.
- **Buddy allocator:** hands out power-of-two blocks of frames; merges freed blocks with their buddies so large
  contiguous runs reappear.
- **Slab allocator + `kmalloc`:** packs small objects into pages with an in-page free list; detects double frees.
- **LZ4-style compressor:** follows the LZ4 block format; the decompressor bounds-checks everything, since it
  reads data that might be corrupt.
- **Compressed pool:** lives in a kernel heap carved out of the RAM budget. Compressed pages are stored in slab
  objects of 14 size classes (32 B to 2 KiB).

## Code map

| File | What | Lines |
|---|---|---|
| `kernel/mm.[ch]` | frame allocator, page tables, fault handler, swap, replacement, compressed pool | 775 |
| `kernel/buddy.[ch]` | buddy allocator | 169 |
| `kernel/slab.[ch]` | slab caches, kmalloc / kfree | 267 |
| `kernel/lz4.[ch]` | compressor and decompressor | 162 |
| `kernel/pagesim.c` | offline replacement analyser (adds the optimal algorithm, OPT) | 67 |
| `tests/` | five suites with a tiny assertion harness | 1,239 |
| `sim/` | benchmarks | 481 |

Start reading at `kernel/mm.h` (the API), then the "Compressed page pool" section of `kernel/mm.c`.

## Design decisions worth discussing

- **The pool competes with user pages.** Its frames are taken out of the same RAM budget, so the benchmark
  shows a real trade-off and a sweet spot (about 12% of RAM here), not a free lunch.
- **Loads from the pool are exclusive:** the entry is freed when the page returns to RAM, and the page is marked
  dirty so it is saved again if evicted. Without that, a second eviction would silently turn the page into zeros.
  There is a test that fails if this is broken.
- **A subtle race:** making room for a page fault can write back the pool entry of the very page being faulted
  in, changing its page-table entry mid-fault. The fault handler re-reads the entry after allocating a frame.
- **Zero pages are special-cased:** a page of one repeated word is stored as 8 bytes with no pool memory at all.
- **`kmalloc` trade-off:** matches glibc `malloc` on speed (about 25 ns/op) but wastes more memory (1.22x per
  object vs 1.05x) because its size classes are powers of two. The pool uses finer classes for that reason.

## How it is tested

- **Shadow-model stress tests:** random reads, writes and unmap/remap across three address spaces, each page
  mirrored in a shadow copy, over four configurations (including a tiny pool that is constantly written back).
- **Fuzzing:** 33,000 random and corrupted compressed streams fed to the decompressor under the sanitizers.
- **Cross-check:** the live kernel and the independent offline analyser must report identical fault counts.
- **Known answers:** Belady's anomaly, a standard 20-reference string, hand-assembled LZ4 blocks.
- **Failure paths:** segfault, write protection, swap full, pool and swap both exhausted (clean error, no
  corruption), and leak checks.
- **Mutation checks:** the kernel was deliberately broken eight ways (for example, not marking pool loads dirty,
  or not repointing page-table entries on writeback) and the suite failed every time.

## Limitations

- It is a hosted simulation: RAM and swap are host buffers, and `vm_read` / `vm_write` play the MMU. No TLB model,
  no multi-core, no real devices. Heap bookkeeping is not charged against the RAM budget.
- LZ4 compatibility is unverified: validated with hand-built vectors, round trips and fuzzing, but not against
  the reference library.
- Single owner per page: no shared pages, so no deduplication or copy-on-write yet.

## Possible extensions

Page deduplication with copy-on-write `fork` (needs a reverse map from frames to mappers), scan-resistant
replacement (2Q / ARC), a TLB model with huge pages, or a bare-metal x86 boot of the same allocators.

## License

MIT, see `LICENSE`.
