/* Sumi OS - kernel heap: boundary-tag allocator with an explicit free list.
   Every block records the thread that allocated it so that all memory of a dead
   application can be reclaimed in one sweep. */
#include "kernel.h"

typedef struct Block {
    u32 size;            /* whole block including header; bit 0 = in use */
    u32 prev_size;       /* size of the block physically before this one (0 for the first) */
    u16 owner;           /* tid of the allocating thread, 0 = kernel */
    u16 magic;
    u32 pad;
    struct Block *next_free, *prev_free;   /* only valid while free (stored in the payload) */
} Block;
#define HDR 16
#define MAGIC 0xB10C
#define MINBLK 32
#define BSIZE(b) ((b)->size & ~1u)
#define USED(b) ((b)->size & 1)

static u8 *heap_lo, *heap_hi;
static Block *free_head;
static u32 used_bytes, block_count;

static void fl_insert(Block *b) {
    b->prev_free = NULL; b->next_free = free_head;
    if (free_head) free_head->prev_free = b;
    free_head = b;
}
static void fl_remove(Block *b) {
    if (b->prev_free) b->prev_free->next_free = b->next_free; else free_head = b->next_free;
    if (b->next_free) b->next_free->prev_free = b->prev_free;
}
static inline Block *next_blk(Block *b) { Block *n = (Block *)((u8 *)b + BSIZE(b)); return (u8 *)n < heap_hi ? n : NULL; }

void heap_init(u32 start, u32 end) {
    start = (start + 15) & ~15u; end &= ~15u;
    heap_lo = (u8 *)start; heap_hi = (u8 *)end;
    Block *b = (Block *)heap_lo;
    b->size = end - start; b->prev_size = 0; b->owner = 0; b->magic = MAGIC;
    free_head = NULL; fl_insert(b);
    used_bytes = 0; block_count = 1;
}

static void *alloc_owner(size_t n, int owner) {
    if (n == 0) n = 1;
    if (n > 0x40000000) return NULL;
    u32 need = (n + HDR + 15) & ~15u;
    if (need < MINBLK) need = MINBLK;
    u32 f = irq_save();
    Block *b = free_head;
    while (b && b->size < need) b = b->next_free;
    if (!b) { irq_restore(f); klog("kmalloc: out of memory (%u bytes)\n", (u32)n); return NULL; }
    if (b->magic != MAGIC) panic("Heap corruption detected in kmalloc (free block %p)", (void *)b);
    fl_remove(b);
    if (b->size - need >= MINBLK) {
        Block *r = (Block *)((u8 *)b + need);
        r->size = b->size - need; r->prev_size = need; r->owner = 0; r->magic = MAGIC;
        Block *nn = next_blk(r);
        if (nn) nn->prev_size = r->size;
        b->size = need;
        fl_insert(r);
        block_count++;
    }
    b->size |= 1; b->owner = (u16)owner;
    used_bytes += BSIZE(b);
    irq_restore(f);
    return (u8 *)b + HDR;
}
void *kmalloc(size_t n) { return alloc_owner(n, cur_thread ? cur_thread->tid : 0); }
void *kmalloc_k(size_t n) { return alloc_owner(n, 0); }
void *kcalloc(size_t n) { void *p = kmalloc(n); if (p) memset(p, 0, n); return p; }

static Block *free_locked(Block *b) {
    used_bytes -= BSIZE(b);
    b->size &= ~1u; b->owner = 0;
    Block *n = next_blk(b);
    if (n && !USED(n)) { fl_remove(n); b->size += n->size; block_count--; }
    if (b->prev_size) {
        Block *p = (Block *)((u8 *)b - b->prev_size);
        if (!USED(p)) { fl_remove(p); p->size += b->size; b = p; block_count--; }
    }
    n = next_blk(b);
    if (n) n->prev_size = b->size;
    fl_insert(b);
    return b;
}
void kfree(void *ptr) {
    if (!ptr) return;
    Block *b = (Block *)((u8 *)ptr - HDR);
    u32 f = irq_save();
    if ((u8 *)b < heap_lo || (u8 *)b >= heap_hi || b->magic != MAGIC || !USED(b))
        panic("Heap corruption or double free detected in kfree(%p)", ptr);
    free_locked(b);
    irq_restore(f);
}
void *krealloc(void *p, size_t n) {
    if (!p) return kmalloc(n);
    Block *b = (Block *)((u8 *)p - HDR);
    u32 old = BSIZE(b) - HDR;
    if (n <= old) return p;
    void *q = alloc_owner(n + n / 2, b->owner);
    if (!q) return NULL;
    memcpy(q, p, old);
    kfree(p);
    return q;
}
void heap_free_owner(int tid) {
    if (tid <= 0) return;
    u32 f = irq_save();
    Block *b = (Block *)heap_lo;
    while (b) {
        if (b->magic != MAGIC) panic("Heap corruption detected while reclaiming thread %d", tid);
        if (USED(b) && b->owner == tid) b = free_locked(b);
        b = next_blk(b);
    }
    irq_restore(f);
}
void heap_stats(u32 *total, u32 *used, u32 *nblocks) {
    if (total) *total = heap_hi - heap_lo;
    if (used) *used = used_bytes;
    if (nblocks) *nblocks = block_count;
}
u32 heap_owner_usage(int tid) {
    u32 sum = 0;
    u32 f = irq_save();
    for (Block *b = (Block *)heap_lo; b; b = next_blk(b)) if (USED(b) && b->owner == tid) sum += BSIZE(b);
    irq_restore(f);
    return sum;
}
