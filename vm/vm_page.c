/*
 * Copyright (c) 2010-2014 Richard Braun.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * This implementation uses the binary buddy system to manage its heap.
 * Descriptions of the buddy system can be found in the following works :
 * - "UNIX Internals: The New Frontiers", by Uresh Vahalia.
 * - "Dynamic Storage Allocation: A Survey and Critical Review",
 *    by Paul R. Wilson, Mark S. Johnstone, Michael Neely, and David Boles.
 *
 * In addition, this allocator uses per-CPU pools of pages for order 0
 * (i.e. single page) allocations. These pools act as caches (but are named
 * differently to avoid confusion with CPU caches) that reduce contention on
 * multiprocessor systems. When a pool is empty and cannot provide a page,
 * it is filled by transferring multiple pages from the backend buddy system.
 * The symmetric case is handled likewise.
 */

#include <string.h>
#include <kern/assert.h>
#include <kern/cpu_number.h>
#include <kern/debug.h>
#include <kern/list.h>
#include <kern/lock.h>
#include <kern/macros.h>
#include <kern/printf.h>
#include <kern/thread.h>
#include <mach/vm_param.h>
#include <machine/pmap.h>
#include <sys/types.h>
#include <vm/vm_page.h>

#define __init
#define __initdata
#define __read_mostly

#define thread_pin()
#define thread_unpin()

/*
 * Number of free block lists per segment.
 */
#define VM_PAGE_NR_FREE_LISTS 11

/*
 * The size of a CPU pool is computed by dividing the number of pages in its
 * containing segment by this value.
 */
#define VM_PAGE_CPU_POOL_RATIO 1024

/*
 * Maximum number of pages in a CPU pool.
 */
#define VM_PAGE_CPU_POOL_MAX_SIZE 128

/*
 * The transfer size of a CPU pool is computed by dividing the pool size by
 * this value.
 */
#define VM_PAGE_CPU_POOL_TRANSFER_RATIO 2

/*
 * Per-processor cache of pages.
 */
struct vm_page_cpu_pool {
    simple_lock_data_t lock;
    int size;
    int transfer_size;
    int nr_pages;
    struct list pages;
} __aligned(CPU_L1_SIZE);

/*
 * Special order value for pages that aren't in a free list. Such pages are
 * either allocated, or part of a free block of pages but not the head page.
 */
#define VM_PAGE_ORDER_UNLISTED ((unsigned short)-1)

/*
 * Doubly-linked list of free blocks.
 */
struct vm_page_free_list {
    unsigned long size;
    struct list blocks;
};

/*
 * Segment name buffer size.
 */
#define VM_PAGE_NAME_SIZE 16

/*
 * Segment of contiguous memory.
 */
struct vm_page_seg {
    struct vm_page_cpu_pool cpu_pools[NCPUS];

    phys_addr_t start;
    phys_addr_t end;
    struct vm_page *pages;
    struct vm_page *pages_end;
    simple_lock_data_t lock;
    struct vm_page_free_list free_lists[VM_PAGE_NR_FREE_LISTS];
    unsigned long nr_free_pages;
};

/*
 * Bootstrap information about a segment.
 */
struct vm_page_boot_seg {
    phys_addr_t start;
    phys_addr_t end;
    phys_addr_t avail_start;
    phys_addr_t avail_end;
};

static int vm_page_is_ready __read_mostly;

/*
 * Segment table.
 *
 * The system supports a maximum of 4 segments :
 *  - DMA: suitable for DMA
 *  - DMA32: suitable for DMA when devices support 32-bits addressing
 *  - DIRECTMAP: direct physical mapping, allows direct access from
 *    the kernel with a simple offset translation
 *  - HIGHMEM: must be mapped before it can be accessed
 *
 * Segments are ordered by priority, 0 being the lowest priority. Their
 * relative priorities are DMA < DMA32 < DIRECTMAP < HIGHMEM. Some segments
 * may actually be aliases for others, e.g. if DMA is always possible from
 * the direct physical mapping, DMA and DMA32 are aliases for DIRECTMAP,
 * in which case the segment table contains DIRECTMAP and HIGHMEM only.
 */
static struct vm_page_seg vm_page_segs[VM_PAGE_MAX_SEGS];

/*
 * Bootstrap segment table.
 */
static struct vm_page_boot_seg vm_page_boot_segs[VM_PAGE_MAX_SEGS] __initdata;

/*
 * Number of loaded segments.
 */
static unsigned int vm_page_segs_size __read_mostly;

static void __init
vm_page_init_pa(struct vm_page *page, unsigned short seg_index, phys_addr_t pa)
{
    memset(page, 0, sizeof(*page));
    vm_page_init(page); /* vm_resident members */
    page->type = VM_PT_RESERVED;
    page->seg_index = seg_index;
    page->order = VM_PAGE_ORDER_UNLISTED;
    page->priv = NULL;
    page->phys_addr = pa;
}

void
vm_page_set_type(struct vm_page *page, unsigned int order, unsigned short type)
{
    unsigned int i, nr_pages;

    nr_pages = 1 << order;

    for (i = 0; i < nr_pages; i++)
        page[i].type = type;
}

static void __init
vm_page_free_list_init(struct vm_page_free_list *free_list)
{
    free_list->size = 0;
    list_init(&free_list->blocks);
}

static inline void
vm_page_free_list_insert(struct vm_page_free_list *free_list,
                         struct vm_page *page)
{
    assert(page->order == VM_PAGE_ORDER_UNLISTED);

    free_list->size++;
    list_insert_head(&free_list->blocks, &page->node);
}

static inline void
vm_page_free_list_remove(struct vm_page_free_list *free_list,
                         struct vm_page *page)
{
    assert(page->order != VM_PAGE_ORDER_UNLISTED);

    free_list->size--;
    list_remove(&page->node);
}

static struct vm_page *
vm_page_seg_alloc_from_buddy(struct vm_page_seg *seg, unsigned int order)
{
    struct vm_page_free_list *free_list = free_list;
    struct vm_page *page, *buddy;
    unsigned int i;

    assert(order < VM_PAGE_NR_FREE_LISTS);

    for (i = order; i < VM_PAGE_NR_FREE_LISTS; i++) {
        free_list = &seg->free_lists[i];

        if (free_list->size != 0)
            break;
    }

    if (i == VM_PAGE_NR_FREE_LISTS)
        return NULL;

    page = list_first_entry(&free_list->blocks, struct vm_page, node);
    vm_page_free_list_remove(free_list, page);
    page->order = VM_PAGE_ORDER_UNLISTED;

    while (i > order) {
        i--;
        buddy = &page[1 << i];
        vm_page_free_list_insert(&seg->free_lists[i], buddy);
        buddy->order = i;
    }

    seg->nr_free_pages -= (1 << order);
    return page;
}

static void
vm_page_seg_free_to_buddy(struct vm_page_seg *seg, struct vm_page *page,
                          unsigned int order)
{
    struct vm_page *buddy;
    phys_addr_t pa, buddy_pa;
    unsigned int nr_pages;

    assert(page >= seg->pages);
    assert(page < seg->pages_end);
    assert(page->order == VM_PAGE_ORDER_UNLISTED);
    assert(order < VM_PAGE_NR_FREE_LISTS);

    nr_pages = (1 << order);
    pa = page->phys_addr;

    while (order < (VM_PAGE_NR_FREE_LISTS - 1)) {
        buddy_pa = pa ^ vm_page_ptoa(1 << order);

        if ((buddy_pa < seg->start) || (buddy_pa >= seg->end))
            break;

        buddy = &seg->pages[vm_page_atop(buddy_pa - seg->start)];

        if (buddy->order != order)
            break;

        vm_page_free_list_remove(&seg->free_lists[order], buddy);
        buddy->order = VM_PAGE_ORDER_UNLISTED;
        order++;
        pa &= -vm_page_ptoa(1 << order);
        page = &seg->pages[vm_page_atop(pa - seg->start)];
    }

    vm_page_free_list_insert(&seg->free_lists[order], page);
    page->order = order;
    seg->nr_free_pages += nr_pages;
}

static void __init
vm_page_cpu_pool_init(struct vm_page_cpu_pool *cpu_pool, int size)
{
    simple_lock_init(&cpu_pool->lock);
    cpu_pool->size = size;
    cpu_pool->transfer_size = (size + VM_PAGE_CPU_POOL_TRANSFER_RATIO - 1)
                              / VM_PAGE_CPU_POOL_TRANSFER_RATIO;
    cpu_pool->nr_pages = 0;
    list_init(&cpu_pool->pages);
}

static inline struct vm_page_cpu_pool *
vm_page_cpu_pool_get(struct vm_page_seg *seg)
{
    return &seg->cpu_pools[cpu_number()];
}

static inline struct vm_page *
vm_page_cpu_pool_pop(struct vm_page_cpu_pool *cpu_pool)
{
    struct vm_page *page;

    assert(cpu_pool->nr_pages != 0);
    cpu_pool->nr_pages--;
    page = list_first_entry(&cpu_pool->pages, struct vm_page, node);
    list_remove(&page->node);
    return page;
}

static inline void
vm_page_cpu_pool_push(struct vm_page_cpu_pool *cpu_pool, struct vm_page *page)
{
    assert(cpu_pool->nr_pages < cpu_pool->size);
    cpu_pool->nr_pages++;
    list_insert_head(&cpu_pool->pages, &page->node);
}

static int
vm_page_cpu_pool_fill(struct vm_page_cpu_pool *cpu_pool,
                      struct vm_page_seg *seg)
{
    struct vm_page *page;
    int i;

    assert(cpu_pool->nr_pages == 0);

    simple_lock(&seg->lock);

    for (i = 0; i < cpu_pool->transfer_size; i++) {
        page = vm_page_seg_alloc_from_buddy(seg, 0);

        if (page == NULL)
            break;

        vm_page_cpu_pool_push(cpu_pool, page);
    }

    simple_unlock(&seg->lock);

    return i;
}

static void
vm_page_cpu_pool_drain(struct vm_page_cpu_pool *cpu_pool,
                       struct vm_page_seg *seg)
{
    struct vm_page *page;
    int i;

    assert(cpu_pool->nr_pages == cpu_pool->size);

    simple_lock(&seg->lock);

    for (i = cpu_pool->transfer_size; i > 0; i--) {
        page = vm_page_cpu_pool_pop(cpu_pool);
        vm_page_seg_free_to_buddy(seg, page, 0);
    }

    simple_unlock(&seg->lock);
}

static phys_addr_t __init
vm_page_seg_size(struct vm_page_seg *seg)
{
    return seg->end - seg->start;
}

static int __init
vm_page_seg_compute_pool_size(struct vm_page_seg *seg)
{
    phys_addr_t size;

    size = vm_page_atop(vm_page_seg_size(seg)) / VM_PAGE_CPU_POOL_RATIO;

    if (size == 0)
        size = 1;
    else if (size > VM_PAGE_CPU_POOL_MAX_SIZE)
        size = VM_PAGE_CPU_POOL_MAX_SIZE;

    return size;
}

static void __init
vm_page_seg_init(struct vm_page_seg *seg, phys_addr_t start, phys_addr_t end,
                 struct vm_page *pages)
{
    phys_addr_t pa;
    int pool_size;
    unsigned int i;

    seg->start = start;
    seg->end = end;
    pool_size = vm_page_seg_compute_pool_size(seg);

    for (i = 0; i < ARRAY_SIZE(seg->cpu_pools); i++)
        vm_page_cpu_pool_init(&seg->cpu_pools[i], pool_size);

    seg->pages = pages;
    seg->pages_end = pages + vm_page_atop(vm_page_seg_size(seg));
    simple_lock_init(&seg->lock);

    for (i = 0; i < ARRAY_SIZE(seg->free_lists); i++)
        vm_page_free_list_init(&seg->free_lists[i]);

    seg->nr_free_pages = 0;
    i = seg - vm_page_segs;

    for (pa = seg->start; pa < seg->end; pa += PAGE_SIZE)
        vm_page_init_pa(&pages[vm_page_atop(pa - seg->start)], i, pa);
}

static struct vm_page *
vm_page_seg_alloc(struct vm_page_seg *seg, unsigned int order,
                  unsigned short type)
{
    struct vm_page_cpu_pool *cpu_pool;
    struct vm_page *page;
    int filled;

    assert(order < VM_PAGE_NR_FREE_LISTS);

    if (order == 0) {
        thread_pin();
        cpu_pool = vm_page_cpu_pool_get(seg);
        simple_lock(&cpu_pool->lock);

        if (cpu_pool->nr_pages == 0) {
            filled = vm_page_cpu_pool_fill(cpu_pool, seg);

            if (!filled) {
                simple_unlock(&cpu_pool->lock);
                thread_unpin();
                return NULL;
            }
        }

        page = vm_page_cpu_pool_pop(cpu_pool);
        simple_unlock(&cpu_pool->lock);
        thread_unpin();
    } else {
        simple_lock(&seg->lock);
        page = vm_page_seg_alloc_from_buddy(seg, order);
        simple_unlock(&seg->lock);

        if (page == NULL)
            return NULL;
    }

    assert(page->type == VM_PT_FREE);
    vm_page_set_type(page, order, type);
    return page;
}

static void
vm_page_seg_free(struct vm_page_seg *seg, struct vm_page *page,
                 unsigned int order)
{
    struct vm_page_cpu_pool *cpu_pool;

    assert(page->type != VM_PT_FREE);
    assert(order < VM_PAGE_NR_FREE_LISTS);

    vm_page_set_type(page, order, VM_PT_FREE);

    if (order == 0) {
        thread_pin();
        cpu_pool = vm_page_cpu_pool_get(seg);
        simple_lock(&cpu_pool->lock);

        if (cpu_pool->nr_pages == cpu_pool->size)
            vm_page_cpu_pool_drain(cpu_pool, seg);

        vm_page_cpu_pool_push(cpu_pool, page);
        simple_unlock(&cpu_pool->lock);
        thread_unpin();
    } else {
        simple_lock(&seg->lock);
        vm_page_seg_free_to_buddy(seg, page, order);
        simple_unlock(&seg->lock);
    }
}

void __init
vm_page_load(unsigned int seg_index, phys_addr_t start, phys_addr_t end,
             phys_addr_t avail_start, phys_addr_t avail_end)
{
    struct vm_page_boot_seg *seg;

    assert(seg_index < ARRAY_SIZE(vm_page_boot_segs));
    assert(vm_page_aligned(start));
    assert(vm_page_aligned(end));
    assert(vm_page_aligned(avail_start));
    assert(vm_page_aligned(avail_end));
    assert(start < end);
    assert(start <= avail_start);
    assert(avail_end <= end);
    assert(vm_page_segs_size < ARRAY_SIZE(vm_page_boot_segs));

    seg = &vm_page_boot_segs[seg_index];
    seg->start = start;
    seg->end = end;
    seg->avail_start = avail_start;
    seg->avail_end = avail_end;
    vm_page_segs_size++;
}

int
vm_page_ready(void)
{
    return vm_page_is_ready;
}

static unsigned int
vm_page_select_alloc_seg(unsigned int selector)
{
    unsigned int seg_index;

    switch (selector) {
    case VM_PAGE_SEL_DMA:
        seg_index = VM_PAGE_SEG_DMA;
        break;
    case VM_PAGE_SEL_DMA32:
        seg_index = VM_PAGE_SEG_DMA32;
        break;
    case VM_PAGE_SEL_DIRECTMAP:
        seg_index = VM_PAGE_SEG_DIRECTMAP;
        break;
    case VM_PAGE_SEL_HIGHMEM:
        seg_index = VM_PAGE_SEG_HIGHMEM;
        break;
    default:
        panic("vm_page: invalid selector");
    }

    return MIN(vm_page_segs_size - 1, seg_index);
}

static int __init
vm_page_boot_seg_loaded(const struct vm_page_boot_seg *seg)
{
    return (seg->end != 0);
}

static void __init
vm_page_check_boot_segs(void)
{
    unsigned int i;
    int expect_loaded;

    if (vm_page_segs_size == 0)
        panic("vm_page: no physical memory loaded");

    for (i = 0; i < ARRAY_SIZE(vm_page_boot_segs); i++) {
        expect_loaded = (i < vm_page_segs_size);

        if (vm_page_boot_seg_loaded(&vm_page_boot_segs[i]) == expect_loaded)
            continue;

        panic("vm_page: invalid boot segment table");
    }
}

static phys_addr_t __init
vm_page_boot_seg_size(struct vm_page_boot_seg *seg)
{
    return seg->end - seg->start;
}

static phys_addr_t __init
vm_page_boot_seg_avail_size(struct vm_page_boot_seg *seg)
{
    return seg->avail_end - seg->avail_start;
}

unsigned long __init
vm_page_bootalloc(size_t size)
{
    struct vm_page_boot_seg *seg;
    phys_addr_t pa;
    unsigned int i;

    for (i = vm_page_select_alloc_seg(VM_PAGE_SEL_DIRECTMAP);
         i < vm_page_segs_size;
         i--) {
        seg = &vm_page_boot_segs[i];

        if (size <= vm_page_boot_seg_avail_size(seg)) {
            pa = seg->avail_start;
            seg->avail_start += vm_page_round(size);
            return pa;
        }
    }

    panic("vm_page: no physical memory available");
}

void __init
vm_page_setup(void)
{
    struct vm_page_boot_seg *boot_seg;
    struct vm_page_seg *seg;
    struct vm_page *table, *page, *end;
    size_t nr_pages, table_size;
    unsigned long va;
    unsigned int i;
    phys_addr_t pa;

    vm_page_check_boot_segs();

    /*
     * Compute the page table size.
     */
    nr_pages = 0;

    for (i = 0; i < vm_page_segs_size; i++)
        nr_pages += vm_page_atop(vm_page_boot_seg_size(&vm_page_boot_segs[i]));

    table_size = vm_page_round(nr_pages * sizeof(struct vm_page));
    printf("vm_page: page table size: %lu entries (%luk)\n", nr_pages,
           table_size >> 10);
    table = (struct vm_page *)pmap_steal_memory(table_size);
    va = (unsigned long)table;

    /*
     * Initialize the segments, associating them to the page table. When
     * the segments are initialized, all their pages are set allocated.
     * Pages are then released, which populates the free lists.
     */
    for (i = 0; i < vm_page_segs_size; i++) {
        seg = &vm_page_segs[i];
        boot_seg = &vm_page_boot_segs[i];
        vm_page_seg_init(seg, boot_seg->start, boot_seg->end, table);
        page = seg->pages + vm_page_atop(boot_seg->avail_start
                                         - boot_seg->start);
        end = seg->pages + vm_page_atop(boot_seg->avail_end
                                        - boot_seg->start);

        while (page < end) {
            page->type = VM_PT_FREE;
            vm_page_seg_free_to_buddy(seg, page, 0);
            page++;
        }

        table += vm_page_atop(vm_page_seg_size(seg));
    }

    while (va < (unsigned long)table) {
        pa = pmap_extract(kernel_pmap, va);
        page = vm_page_lookup_pa(pa);
        assert((page != NULL) && (page->type == VM_PT_RESERVED));
        page->type = VM_PT_TABLE;
        va += PAGE_SIZE;
    }

    vm_page_is_ready = 1;
}

void __init
vm_page_manage(struct vm_page *page)
{
    assert(page->seg_index < ARRAY_SIZE(vm_page_segs));
    assert(page->type == VM_PT_RESERVED);

    vm_page_set_type(page, 0, VM_PT_FREE);
    vm_page_seg_free_to_buddy(&vm_page_segs[page->seg_index], page, 0);
}

struct vm_page *
vm_page_lookup_pa(phys_addr_t pa)
{
    struct vm_page_seg *seg;
    unsigned int i;

    for (i = 0; i < vm_page_segs_size; i++) {
        seg = &vm_page_segs[i];

        if ((pa >= seg->start) && (pa < seg->end))
            return &seg->pages[vm_page_atop(pa - seg->start)];
    }

    return NULL;
}

struct vm_page *
vm_page_alloc_pa(unsigned int order, unsigned int selector, unsigned short type)
{
    struct vm_page *page;
    unsigned int i;

    for (i = vm_page_select_alloc_seg(selector); i < vm_page_segs_size; i--) {
        page = vm_page_seg_alloc(&vm_page_segs[i], order, type);

        if (page != NULL)
            return page;
    }

    if (type == VM_PT_PMAP)
        panic("vm_page: unable to allocate pmap page");

    return NULL;
}

void
vm_page_free_pa(struct vm_page *page, unsigned int order)
{
    assert(page != NULL);
    assert(page->seg_index < ARRAY_SIZE(vm_page_segs));

    vm_page_seg_free(&vm_page_segs[page->seg_index], page, order);
}

const char *
vm_page_seg_name(unsigned int seg_index)
{
    /* Don't use a switch statement since segments can be aliased */
    if (seg_index == VM_PAGE_SEG_HIGHMEM)
        return "HIGHMEM";
    else if (seg_index == VM_PAGE_SEG_DIRECTMAP)
        return "DIRECTMAP";
    else if (seg_index == VM_PAGE_SEG_DMA32)
        return "DMA32";
    else if (seg_index == VM_PAGE_SEG_DMA)
        return "DMA";
    else
        panic("vm_page: invalid segment index");
}

void
vm_page_info_all(void)
{
    struct vm_page_seg *seg;
    unsigned long pages;
    unsigned int i;

    for (i = 0; i < vm_page_segs_size; i++) {
        seg = &vm_page_segs[i];
        pages = (unsigned long)(seg->pages_end - seg->pages);
        printf("vm_page: %s: pages: %lu (%luM), free: %lu (%luM)\n",
               vm_page_seg_name(i), pages, pages >> (20 - PAGE_SHIFT),
               seg->nr_free_pages, seg->nr_free_pages >> (20 - PAGE_SHIFT));
    }
}

phys_addr_t
vm_page_mem_size(void)
{
    phys_addr_t total;
    unsigned int i;

    total = 0;

    for (i = 0; i < vm_page_segs_size; i++) {
        /* XXX */
        if (i > VM_PAGE_SEG_DIRECTMAP)
            continue;

        total += vm_page_seg_size(&vm_page_segs[i]);
    }

    return total;
}

unsigned long
vm_page_mem_free(void)
{
    unsigned long total;
    unsigned int i;

    total = 0;

    for (i = 0; i < vm_page_segs_size; i++) {
        /* XXX */
        if (i >  VM_PAGE_SEG_DIRECTMAP)
            continue;

        total += vm_page_segs[i].nr_free_pages;
    }

    return total;
}
