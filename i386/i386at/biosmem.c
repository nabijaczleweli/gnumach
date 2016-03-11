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
 */

#include <string.h>
#include <i386/model_dep.h>
#include <i386at/biosmem.h>
#include <i386at/elf.h>
#include <kern/assert.h>
#include <kern/debug.h>
#include <kern/macros.h>
#include <kern/printf.h>
#include <mach/vm_param.h>
#include <mach/xen.h>
#include <mach/machine/multiboot.h>
#include <sys/types.h>
#include <vm/vm_page.h>

#define __boot
#define __bootdata
#define __init

#define boot_memmove    memmove
#define boot_panic      panic
#define boot_strlen     strlen

#define BOOT_CGAMEM     phystokv(0xb8000)
#define BOOT_CGACHARS   (80 * 25)
#define BOOT_CGACOLOR   0x7

extern char _start, _end;

/*
 * Maximum number of entries in the BIOS memory map.
 *
 * Because of adjustments of overlapping ranges, the memory map can grow
 * to twice this size.
 */
#define BIOSMEM_MAX_MAP_SIZE 128

/*
 * Memory range types.
 */
#define BIOSMEM_TYPE_AVAILABLE  1
#define BIOSMEM_TYPE_RESERVED   2
#define BIOSMEM_TYPE_ACPI       3
#define BIOSMEM_TYPE_NVS        4
#define BIOSMEM_TYPE_UNUSABLE   5
#define BIOSMEM_TYPE_DISABLED   6

/*
 * Memory map entry.
 */
struct biosmem_map_entry {
    uint64_t base_addr;
    uint64_t length;
    unsigned int type;
};

/*
 * Contiguous block of physical memory.
 *
 * Tha "available" range records what has been passed to the VM system as
 * available inside the segment.
 */
struct biosmem_segment {
    phys_addr_t start;
    phys_addr_t end;
    phys_addr_t avail_start;
    phys_addr_t avail_end;
};

/*
 * Memory map built from the information passed by the boot loader.
 *
 * If the boot loader didn't pass a valid memory map, a simple map is built
 * based on the mem_lower and mem_upper multiboot fields.
 */
static struct biosmem_map_entry biosmem_map[BIOSMEM_MAX_MAP_SIZE * 2]
    __bootdata;
static unsigned int biosmem_map_size __bootdata;

/*
 * Physical segment boundaries.
 */
static struct biosmem_segment biosmem_segments[VM_PAGE_MAX_SEGS] __bootdata;

/*
 * Boundaries of the simple bootstrap heap.
 *
 * This heap is located above BIOS memory.
 */
static uint32_t biosmem_heap_start __bootdata;
static uint32_t biosmem_heap_cur __bootdata;
static uint32_t biosmem_heap_end __bootdata;

static char biosmem_panic_toobig_msg[] __bootdata
    = "biosmem: too many memory map entries";
#ifndef MACH_HYP
static char biosmem_panic_setup_msg[] __bootdata
    = "biosmem: unable to set up the early memory allocator";
#endif /* MACH_HYP */
static char biosmem_panic_noseg_msg[] __bootdata
    = "biosmem: unable to find any memory segment";
static char biosmem_panic_inval_msg[] __bootdata
    = "biosmem: attempt to allocate 0 page";
static char biosmem_panic_nomem_msg[] __bootdata
    = "biosmem: unable to allocate memory";

#ifndef MACH_HYP

static void __boot
biosmem_map_build(const struct multiboot_raw_info *mbi)
{
    struct multiboot_raw_mmap_entry *mb_entry, *mb_end;
    struct biosmem_map_entry *start, *entry, *end;
    unsigned long addr;

    addr = phystokv(mbi->mmap_addr);
    mb_entry = (struct multiboot_raw_mmap_entry *)addr;
    mb_end = (struct multiboot_raw_mmap_entry *)(addr + mbi->mmap_length);
    start = biosmem_map;
    entry = start;
    end = entry + BIOSMEM_MAX_MAP_SIZE;

    while ((mb_entry < mb_end) && (entry < end)) {
        entry->base_addr = mb_entry->base_addr;
        entry->length = mb_entry->length;
        entry->type = mb_entry->type;

        mb_entry = (void *)mb_entry + sizeof(mb_entry->size) + mb_entry->size;
        entry++;
    }

    biosmem_map_size = entry - start;
}

static void __boot
biosmem_map_build_simple(const struct multiboot_raw_info *mbi)
{
    struct biosmem_map_entry *entry;

    entry = biosmem_map;
    entry->base_addr = 0;
    entry->length = mbi->mem_lower << 10;
    entry->type = BIOSMEM_TYPE_AVAILABLE;

    entry++;
    entry->base_addr = BIOSMEM_END;
    entry->length = mbi->mem_upper << 10;
    entry->type = BIOSMEM_TYPE_AVAILABLE;

    biosmem_map_size = 2;
}

#endif /* MACH_HYP */

static int __boot
biosmem_map_entry_is_invalid(const struct biosmem_map_entry *entry)
{
    return (entry->base_addr + entry->length) <= entry->base_addr;
}

static void __boot
biosmem_map_filter(void)
{
    struct biosmem_map_entry *entry;
    unsigned int i;

    i = 0;

    while (i < biosmem_map_size) {
        entry = &biosmem_map[i];

        if (biosmem_map_entry_is_invalid(entry)) {
            biosmem_map_size--;
            boot_memmove(entry, entry + 1,
                         (biosmem_map_size - i) * sizeof(*entry));
            continue;
        }

        i++;
    }
}

static void __boot
biosmem_map_sort(void)
{
    struct biosmem_map_entry tmp;
    unsigned int i, j;

    /*
     * Simple insertion sort.
     */
    for (i = 1; i < biosmem_map_size; i++) {
        tmp = biosmem_map[i];

        for (j = i - 1; j < i; j--) {
            if (biosmem_map[j].base_addr < tmp.base_addr)
                break;

            biosmem_map[j + 1] = biosmem_map[j];
        }

        biosmem_map[j + 1] = tmp;
    }
}

static void __boot
biosmem_map_adjust(void)
{
    struct biosmem_map_entry tmp, *a, *b, *first, *second;
    uint64_t a_end, b_end, last_end;
    unsigned int i, j, last_type;

    biosmem_map_filter();

    /*
     * Resolve overlapping areas, giving priority to most restrictive
     * (i.e. numerically higher) types.
     */
    for (i = 0; i < biosmem_map_size; i++) {
        a = &biosmem_map[i];
        a_end = a->base_addr + a->length;

        j = i + 1;

        while (j < biosmem_map_size) {
            b = &biosmem_map[j];
            b_end = b->base_addr + b->length;

            if ((a->base_addr >= b_end) || (a_end <= b->base_addr)) {
                j++;
                continue;
            }

            if (a->base_addr < b->base_addr) {
                first = a;
                second = b;
            } else {
                first = b;
                second = a;
            }

            if (a_end > b_end) {
                last_end = a_end;
                last_type = a->type;
            } else {
                last_end = b_end;
                last_type = b->type;
            }

            tmp.base_addr = second->base_addr;
            tmp.length = MIN(a_end, b_end) - tmp.base_addr;
            tmp.type = MAX(a->type, b->type);
            first->length = tmp.base_addr - first->base_addr;
            second->base_addr += tmp.length;
            second->length = last_end - second->base_addr;
            second->type = last_type;

            /*
             * Filter out invalid entries.
             */
            if (biosmem_map_entry_is_invalid(a)
                && biosmem_map_entry_is_invalid(b)) {
                *a = tmp;
                biosmem_map_size--;
                memmove(b, b + 1, (biosmem_map_size - j) * sizeof(*b));
                continue;
            } else if (biosmem_map_entry_is_invalid(a)) {
                *a = tmp;
                j++;
                continue;
            } else if (biosmem_map_entry_is_invalid(b)) {
                *b = tmp;
                j++;
                continue;
            }

            if (tmp.type == a->type)
                first = a;
            else if (tmp.type == b->type)
                first = b;
            else {

                /*
                 * If the overlapping area can't be merged with one of its
                 * neighbors, it must be added as a new entry.
                 */

                if (biosmem_map_size >= ARRAY_SIZE(biosmem_map))
                    boot_panic(biosmem_panic_toobig_msg);

                biosmem_map[biosmem_map_size] = tmp;
                biosmem_map_size++;
                j++;
                continue;
            }

            if (first->base_addr > tmp.base_addr)
                first->base_addr = tmp.base_addr;

            first->length += tmp.length;
            j++;
        }
    }

    biosmem_map_sort();
}

static int __boot
biosmem_map_find_avail(phys_addr_t *phys_start, phys_addr_t *phys_end)
{
    const struct biosmem_map_entry *entry, *map_end;
    phys_addr_t seg_start, seg_end;
    uint64_t start, end;

    seg_start = (phys_addr_t)-1;
    seg_end = (phys_addr_t)-1;
    map_end = biosmem_map + biosmem_map_size;

    for (entry = biosmem_map; entry < map_end; entry++) {
        if (entry->type != BIOSMEM_TYPE_AVAILABLE)
            continue;

        start = vm_page_round(entry->base_addr);

        if (start >= *phys_end)
            break;

        end = vm_page_trunc(entry->base_addr + entry->length);

        if ((start < end) && (start < *phys_end) && (end > *phys_start)) {
            if (seg_start == (phys_addr_t)-1)
                seg_start = start;

            seg_end = end;
        }
    }

    if ((seg_start == (phys_addr_t)-1) || (seg_end == (phys_addr_t)-1))
        return -1;

    if (seg_start > *phys_start)
        *phys_start = seg_start;

    if (seg_end < *phys_end)
        *phys_end = seg_end;

    return 0;
}

static void __boot
biosmem_set_segment(unsigned int seg_index, phys_addr_t start, phys_addr_t end)
{
    biosmem_segments[seg_index].start = start;
    biosmem_segments[seg_index].end = end;
}

static phys_addr_t __boot
biosmem_segment_end(unsigned int seg_index)
{
    return biosmem_segments[seg_index].end;
}

static phys_addr_t __boot
biosmem_segment_size(unsigned int seg_index)
{
    return biosmem_segments[seg_index].end - biosmem_segments[seg_index].start;
}

#ifndef MACH_HYP

static void __boot
biosmem_save_cmdline_sizes(struct multiboot_raw_info *mbi)
{
    struct multiboot_raw_module *mod;
    uint32_t i, va;

    if (mbi->flags & MULTIBOOT_LOADER_CMDLINE) {
        va = phystokv(mbi->cmdline);
        mbi->unused0 = boot_strlen((char *)va) + 1;
    }

    if (mbi->flags & MULTIBOOT_LOADER_MODULES) {
        unsigned long addr;

        addr = phystokv(mbi->mods_addr);

        for (i = 0; i < mbi->mods_count; i++) {
            mod = (struct multiboot_raw_module *)addr + i;
            va = phystokv(mod->string);
            mod->reserved = boot_strlen((char *)va) + 1;
        }
    }
}

static void __boot
biosmem_find_boot_data_update(uint32_t min, uint32_t *start, uint32_t *end,
                              uint32_t data_start, uint32_t data_end)
{
    if ((min <= data_start) && (data_start < *start)) {
        *start = data_start;
        *end = data_end;
    }
}

/*
 * Find the first boot data in the given range, and return their containing
 * area (start address is returned directly, end address is returned in end).
 * The following are considered boot data :
 *  - the kernel
 *  - the kernel command line
 *  - the module table
 *  - the modules
 *  - the modules command lines
 *  - the ELF section header table
 *  - the ELF .shstrtab, .symtab and .strtab sections
 *
 * If no boot data was found, 0 is returned, and the end address isn't set.
 */
static uint32_t __boot
biosmem_find_boot_data(const struct multiboot_raw_info *mbi, uint32_t min,
                       uint32_t max, uint32_t *endp)
{
    struct multiboot_raw_module *mod;
    struct elf_shdr *shdr;
    uint32_t i, start, end = end;
    unsigned long tmp;

    start = max;

    biosmem_find_boot_data_update(min, &start, &end, _kvtophys(&_start),
                                  _kvtophys(&_end));

    if ((mbi->flags & MULTIBOOT_LOADER_CMDLINE) && (mbi->cmdline != 0))
        biosmem_find_boot_data_update(min, &start, &end, mbi->cmdline,
                                      mbi->cmdline + mbi->unused0);

    if (mbi->flags & MULTIBOOT_LOADER_MODULES) {
        i = mbi->mods_count * sizeof(struct multiboot_raw_module);
        biosmem_find_boot_data_update(min, &start, &end, mbi->mods_addr,
                                      mbi->mods_addr + i);
        tmp = phystokv(mbi->mods_addr);

        for (i = 0; i < mbi->mods_count; i++) {
            mod = (struct multiboot_raw_module *)tmp + i;
            biosmem_find_boot_data_update(min, &start, &end, mod->mod_start,
                                          mod->mod_end);

            if (mod->string != 0)
                biosmem_find_boot_data_update(min, &start, &end, mod->string,
                                              mod->string + mod->reserved);
        }
    }

    if (mbi->flags & MULTIBOOT_LOADER_SHDR) {
        tmp = mbi->shdr_num * mbi->shdr_size;
        biosmem_find_boot_data_update(min, &start, &end, mbi->shdr_addr,
                                      mbi->shdr_addr + tmp);
        tmp = phystokv(mbi->shdr_addr);

        for (i = 0; i < mbi->shdr_num; i++) {
            shdr = (struct elf_shdr *)(tmp + (i * mbi->shdr_size));

            if ((shdr->type != ELF_SHT_SYMTAB)
                && (shdr->type != ELF_SHT_STRTAB))
                continue;

            biosmem_find_boot_data_update(min, &start, &end, shdr->addr,
                                          shdr->addr + shdr->size);
        }
    }

    if (start == max)
        return 0;

    *endp = end;
    return start;
}

static void __boot
biosmem_setup_allocator(struct multiboot_raw_info *mbi)
{
    uint32_t heap_start, heap_end, max_heap_start, max_heap_end;
    uint32_t mem_end, next;

    /*
     * Find some memory for the heap. Look for the largest unused area in
     * upper memory, carefully avoiding all boot data.
     */
    mem_end = vm_page_trunc((mbi->mem_upper + 1024) << 10);

#ifndef __LP64__
    if (mem_end > VM_PAGE_DIRECTMAP_LIMIT)
        mem_end = VM_PAGE_DIRECTMAP_LIMIT;
#endif /* __LP64__ */

    max_heap_start = 0;
    max_heap_end = 0;
    next = BIOSMEM_END;

    do {
        heap_start = next;
        heap_end = biosmem_find_boot_data(mbi, heap_start, mem_end, &next);

        if (heap_end == 0) {
            heap_end = mem_end;
            next = 0;
        }

        if ((heap_end - heap_start) > (max_heap_end - max_heap_start)) {
            max_heap_start = heap_start;
            max_heap_end = heap_end;
        }
    } while (next != 0);

    max_heap_start = vm_page_round(max_heap_start);
    max_heap_end = vm_page_trunc(max_heap_end);

    if (max_heap_start >= max_heap_end)
        boot_panic(biosmem_panic_setup_msg);

    biosmem_heap_start = max_heap_start;
    biosmem_heap_end = max_heap_end;
    biosmem_heap_cur = biosmem_heap_end;
}

#endif /* MACH_HYP */

static void __boot
biosmem_bootstrap_common(void)
{
    phys_addr_t phys_start, phys_end, last_addr;
    int error;

    biosmem_map_adjust();

    phys_start = BIOSMEM_BASE;
    phys_end = VM_PAGE_DMA_LIMIT;
    error = biosmem_map_find_avail(&phys_start, &phys_end);

    if (error)
        boot_panic(biosmem_panic_noseg_msg);

    biosmem_set_segment(VM_PAGE_SEG_DMA, phys_start, phys_end);
    last_addr = phys_end;

    phys_start = VM_PAGE_DMA_LIMIT;
#ifdef VM_PAGE_DMA32_LIMIT
    phys_end = VM_PAGE_DMA32_LIMIT;
    error = biosmem_map_find_avail(&phys_start, &phys_end);

    if (error)
        goto out;

    biosmem_set_segment(VM_PAGE_SEG_DMA32, phys_start, phys_end);
    last_addr = phys_end;

    phys_start = VM_PAGE_DMA32_LIMIT;
#endif /* VM_PAGE_DMA32_LIMIT */
    phys_end = VM_PAGE_DIRECTMAP_LIMIT;
    error = biosmem_map_find_avail(&phys_start, &phys_end);

    if (error)
        goto out;

    biosmem_set_segment(VM_PAGE_SEG_DIRECTMAP, phys_start, phys_end);
    last_addr = phys_end;

    phys_start = VM_PAGE_DIRECTMAP_LIMIT;
    phys_end = VM_PAGE_HIGHMEM_LIMIT;
    error = biosmem_map_find_avail(&phys_start, &phys_end);

    if (error)
        goto out;

    biosmem_set_segment(VM_PAGE_SEG_HIGHMEM, phys_start, phys_end);

out:
    /* XXX phys_last_addr must be part of the direct physical mapping */
    phys_last_addr = last_addr;
}

#ifdef MACH_HYP

void
biosmem_xen_bootstrap(void)
{
    struct biosmem_map_entry *entry;

    entry = biosmem_map;
    entry->base_addr = 0;
    entry->length = boot_info.nr_pages << PAGE_SHIFT;
    entry->type = BIOSMEM_TYPE_AVAILABLE;

    biosmem_map_size = 1;

    biosmem_bootstrap_common();

    biosmem_heap_start = _kvtophys(boot_info.pt_base)
                         + (boot_info.nr_pt_frames + 3) * 0x1000;
    biosmem_heap_end = boot_info.nr_pages << PAGE_SHIFT;

#ifndef __LP64__
    /* TODO Check that this actually makes sense */
    if (biosmem_heap_end > VM_PAGE_DIRECTMAP_LIMIT)
        biosmem_heap_end = VM_PAGE_DIRECTMAP_LIMIT;
#endif /* __LP64__ */

    /*
     * XXX Allocation on Xen must be bottom-up :
     * At the "start of day", only 512k are available after the boot
     * data. The pmap module then creates a 4g mapping so all physical
     * memory is available, but it uses this allocator to do so.
     * Therefore, it must return pages from this small 512k regions
     * first.
     */
    biosmem_heap_cur = biosmem_heap_start;
}

#else /* MACH_HYP */

void __boot
biosmem_bootstrap(struct multiboot_raw_info *mbi)
{
    if (mbi->flags & MULTIBOOT_LOADER_MMAP)
        biosmem_map_build(mbi);
    else
        biosmem_map_build_simple(mbi);

    biosmem_bootstrap_common();

    /*
     * The kernel and modules command lines will be memory mapped later
     * during initialization. Their respective sizes must be saved.
     */
    biosmem_save_cmdline_sizes(mbi);
    biosmem_setup_allocator(mbi);
}

#endif /* MACH_HYP */

unsigned long __boot
biosmem_bootalloc(unsigned int nr_pages)
{
    unsigned long addr, size;

    assert(!vm_page_ready());

    size = vm_page_ptoa(nr_pages);

    if (size == 0)
        boot_panic(biosmem_panic_inval_msg);

#ifdef MACH_HYP
    addr = biosmem_heap_cur;
#else /* MACH_HYP */
    /* Top-down allocation to avoid unnecessarily filling DMA segments */
    addr = biosmem_heap_cur - size;
#endif /* MACH_HYP */

    if ((addr < biosmem_heap_start) || (addr > biosmem_heap_cur))
        boot_panic(biosmem_panic_nomem_msg);

#ifdef MACH_HYP
    biosmem_heap_cur += size;
#else /* MACH_HYP */
    biosmem_heap_cur = addr;
#endif /* MACH_HYP */

    return addr;
}

phys_addr_t __boot
biosmem_directmap_size(void)
{
    if (biosmem_segment_size(VM_PAGE_SEG_DIRECTMAP) != 0)
        return biosmem_segment_end(VM_PAGE_SEG_DIRECTMAP);
    else if (biosmem_segment_size(VM_PAGE_SEG_DMA32) != 0)
        return biosmem_segment_end(VM_PAGE_SEG_DMA32);
    else
        return biosmem_segment_end(VM_PAGE_SEG_DMA);
}

static const char * __init
biosmem_type_desc(unsigned int type)
{
    switch (type) {
    case BIOSMEM_TYPE_AVAILABLE:
        return "available";
    case BIOSMEM_TYPE_RESERVED:
        return "reserved";
    case BIOSMEM_TYPE_ACPI:
        return "ACPI";
    case BIOSMEM_TYPE_NVS:
        return "ACPI NVS";
    case BIOSMEM_TYPE_UNUSABLE:
        return "unusable";
    default:
        return "unknown (reserved)";
    }
}

static void __init
biosmem_map_show(void)
{
    const struct biosmem_map_entry *entry, *end;

    printf("biosmem: physical memory map:\n");

    for (entry = biosmem_map, end = entry + biosmem_map_size;
         entry < end;
         entry++)
        printf("biosmem: %018llx:%018llx, %s\n", entry->base_addr,
               entry->base_addr + entry->length,
               biosmem_type_desc(entry->type));

    printf("biosmem: heap: %x-%x\n", biosmem_heap_start, biosmem_heap_end);
}

static void __init
biosmem_load_segment(struct biosmem_segment *seg, uint64_t max_phys_end,
                     phys_addr_t phys_start, phys_addr_t phys_end,
                     phys_addr_t avail_start, phys_addr_t avail_end)
{
    unsigned int seg_index;

    seg_index = seg - biosmem_segments;

    if (phys_end > max_phys_end) {
        if (max_phys_end <= phys_start) {
            printf("biosmem: warning: segment %s physically unreachable, "
                   "not loaded\n", vm_page_seg_name(seg_index));
            return;
        }

        printf("biosmem: warning: segment %s truncated to %#llx\n",
               vm_page_seg_name(seg_index), max_phys_end);
        phys_end = max_phys_end;
    }

    if ((avail_start < phys_start) || (avail_start >= phys_end))
        avail_start = phys_start;

    if ((avail_end <= phys_start) || (avail_end > phys_end))
        avail_end = phys_end;

    seg->avail_start = avail_start;
    seg->avail_end = avail_end;
    vm_page_load(seg_index, phys_start, phys_end, avail_start, avail_end);
}

void __init
biosmem_setup(void)
{
    struct biosmem_segment *seg;
    unsigned int i;

    biosmem_map_show();

    for (i = 0; i < ARRAY_SIZE(biosmem_segments); i++) {
        if (biosmem_segment_size(i) == 0)
            break;

        seg = &biosmem_segments[i];
        biosmem_load_segment(seg, VM_PAGE_HIGHMEM_LIMIT, seg->start, seg->end,
                             biosmem_heap_start, biosmem_heap_cur);
    }
}

static void __init
biosmem_free_usable_range(phys_addr_t start, phys_addr_t end)
{
    struct vm_page *page;

    printf("biosmem: release to vm_page: %llx-%llx (%lluk)\n",
           (unsigned long long)start, (unsigned long long)end,
           (unsigned long long)((end - start) >> 10));

    while (start < end) {
        page = vm_page_lookup_pa(start);
        assert(page != NULL);
        vm_page_manage(page);
        start += PAGE_SIZE;
    }
}

static void __init
biosmem_free_usable_update_start(phys_addr_t *start, phys_addr_t res_start,
                                 phys_addr_t res_end)
{
    if ((*start >= res_start) && (*start < res_end))
        *start = res_end;
}

static phys_addr_t __init
biosmem_free_usable_start(phys_addr_t start)
{
    const struct biosmem_segment *seg;
    unsigned int i;

    biosmem_free_usable_update_start(&start, _kvtophys(&_start),
                                     _kvtophys(&_end));
    biosmem_free_usable_update_start(&start, biosmem_heap_start,
                                     biosmem_heap_end);

    for (i = 0; i < ARRAY_SIZE(biosmem_segments); i++) {
        seg = &biosmem_segments[i];
        biosmem_free_usable_update_start(&start, seg->avail_start,
                                         seg->avail_end);
    }

    return start;
}

static int __init
biosmem_free_usable_reserved(phys_addr_t addr)
{
    const struct biosmem_segment *seg;
    unsigned int i;

    if ((addr >= _kvtophys(&_start))
        && (addr < _kvtophys(&_end)))
        return 1;

    if ((addr >= biosmem_heap_start) && (addr < biosmem_heap_end))
        return 1;

    for (i = 0; i < ARRAY_SIZE(biosmem_segments); i++) {
        seg = &biosmem_segments[i];

        if ((addr >= seg->avail_start) && (addr < seg->avail_end))
            return 1;
    }

    return 0;
}

static phys_addr_t __init
biosmem_free_usable_end(phys_addr_t start, phys_addr_t entry_end)
{
    while (start < entry_end) {
        if (biosmem_free_usable_reserved(start))
            break;

        start += PAGE_SIZE;
    }

    return start;
}

static void __init
biosmem_free_usable_entry(phys_addr_t start, phys_addr_t end)
{
    phys_addr_t entry_end;

    entry_end = end;

    for (;;) {
        start = biosmem_free_usable_start(start);

        if (start >= entry_end)
            return;

        end = biosmem_free_usable_end(start, entry_end);
        biosmem_free_usable_range(start, end);
        start = end;
    }
}

void __init
biosmem_free_usable(void)
{
    struct biosmem_map_entry *entry;
    uint64_t start, end;
    unsigned int i;

    for (i = 0; i < biosmem_map_size; i++) {
        entry = &biosmem_map[i];

        if (entry->type != BIOSMEM_TYPE_AVAILABLE)
            continue;

        start = vm_page_round(entry->base_addr);

        if (start >= VM_PAGE_HIGHMEM_LIMIT)
            break;

        end = vm_page_trunc(entry->base_addr + entry->length);

        if (start < BIOSMEM_BASE)
            start = BIOSMEM_BASE;

        biosmem_free_usable_entry(start, end);
    }
}
