#ifndef __LINUX_CMA_H
#define __LINUX_CMA_H

/*
 * Contiguous Memory Allocator
 * Copyright (c) 2010-2011 by Samsung Electronics.
 * Written by:
 *	Michal Nazarewicz <mina86@mina86.com>
 *	Marek Szyprowski <m.szyprowski@samsung.com>
 */

/*
 * Contiguous Memory Allocator
 *
 *   The Contiguous Memory Allocator (CMA) makes it possible for
 *   device drivers to allocate big contiguous chunks of memory after
 *   the system has booted.
 *
 *   It requires some machine- and/or platform-specific initialisation
 *   code which prepares memory ranges to be used with CMA and later,
 *   device drivers can allocate memory from those ranges.
 *
 * Why is it needed?
 *
 *   Various devices on embedded systems have no scatter-getter and/or
 *   IO map support and require contiguous blocks of memory to
 *   operate.  They include devices such as cameras, hardware video
 *   coders, etc.
 *
 *   Such devices often require big memory buffers (a full HD frame
 *   is, for instance, more then 2 mega pixels large, i.e. more than 6
 *   MB of memory), which makes mechanisms such as kmalloc() or
 *   alloc_page() ineffective.
 *
 *   At the same time, a solution where a big memory region is
 *   reserved for a device is suboptimal since often more memory is
 *   reserved then strictly required and, moreover, the memory is
 *   inaccessible to page system even if device drivers don't use it.
 *
 *   CMA tries to solve this issue by operating on memory regions
 *   where only movable pages can be allocated from.  This way, kernel
 *   can use the memory for pagecache and when device driver requests
 *   it, allocated pages can be migrated.
 *
 * Driver usage
 *
 *   CMA should not be used directly by the device drivers. It should
 *   be considered as helper framework for dma-mapping subsystm and
 *   respective (platform)bus drivers.
 *
 *   The CMA client needs to have a pointer to a CMA context
 *   represented by a struct cma (which is an opaque data type).
 *
 *   Once such pointer is obtained, a caller may allocate contiguous
 *   memory chunk using the following function:
 *
 *     cm_alloc()
 *
 *   This function returns a pointer to the first struct page which
 *   represent a contiguous memory chunk.  This pointer
 *   may be used with the following function:
 *
 *     cm_free()    -- frees allocated contiguous memory
 *
 * Platform/machine integration
 *
 *   CMA context must be created on platform or machine initialisation
 *   and passed to respective subsystem that will be a client for CMA.
 *   The latter may be done by a global variable or some filed in
 *   struct device.  For the former CMA provides the following functions:
 *
 *     cma_init_migratetype()
 *     cma_reserve()
 *     cma_create()
 *
 *   The first one initialises a portion of reserved memory so that it
 *   can be used with CMA.  The second first tries to reserve memory
 *   (using memblock) and then initialise it.
 *
 *   The cma_reserve() function must be called when memblock is still
 *   operational and reserving memory with it is still possible.  On
 *   ARM platform the "reserve" machine callback is a perfect place to
 *   call it.
 *
 *   The last function creates a CMA context on a range of previously
 *   initialised memory addresses.  Because it uses kmalloc() it needs
 *   to be called after SLAB is initialised.
 */

/***************************** Kernel level API *****************************/

#ifdef __KERNEL__

struct cma;
struct page;

#ifdef CONFIG_CMA

/**
 * cma_init_migratetype() - initialises range of physical memory to be used
 *		with CMA context.
 * @start:	start address of the memory range in bytes.
 * @size:	size of the memory range in bytes.
 *
 * The range must be MAX_ORDER_NR_PAGES aligned and it must have been
 * already reserved (eg. with memblock).
 *
 * The actual initialisation is deferred until subsys initcalls are
 * evaluated (unless this has already happened).
 *
 * Returns zero on success or negative error.
 */
int cma_init_migratetype(unsigned long start, unsigned long end);

/**
 * cma_reserve() - reserves memory.
 * @start:	start address of the memory range in bytes hint; if unsure
 *		pass zero.
 * @size:	size of the memory to reserve in bytes.
 *
 * It will use memblock to allocate memory. It will also call
 * cma_init_migratetype() on reserved region so that a CMA context can
 * be created on given range.
 *
 * @start and @size will be aligned to (MAX_ORDER_NR_PAGES << PAGE_SHIFT).
 *
 * Returns reserved's area physical address or value that yields true
 * when checked with IS_ERR_VALUE().
 */
unsigned long cma_reserve(unsigned long start, unsigned long size);

/**
 * cma_create() - creates a CMA context.
 * @start:	start address of the context in bytes.
 * @size:	size of the context in bytes.
 *
 * The range must be page aligned.  Different contexts cannot overlap.
 *
 * The memory range must either lay in ZONE_MOVABLE or must have been
 * initialised with cma_init_migratetype() function.
 *
 * @start and @size must be page aligned.
 *
 * Because this function uses kmalloc() it must be called after SLAB
 * is initialised.  This in particular means that it cannot be called
 * just after cma_reserve() since the former needs to be run way
 * earlier.
 *
 * Returns pointer to CMA context or a pointer-error on error.
 */
struct cma *cma_create(unsigned long start, unsigned long size);

/**
 * cma_destroy() - destroys CMA context.
 * @cma:	context to destroy.
 */
void cma_destroy(struct cma *cma);

/**
 * cm_alloc() - allocates contiguous memory.
 * @cma:	CMA context to use
 * @count:	desired chunk size in pages (must be non-zero)
 * @order:	desired alignment in pages
 *
 * Returns pointer to first page structure representing contiguous memory
 * or a pointer-error on error.
 */
struct page *cm_alloc(struct cma *cma, int count, unsigned int order);

/**
 * cm_free() - frees contiguous memory.
 * @cma:	CMA context to use
 * @pages:	contiguous memory to free
 * @count:	desired chunk size in pages (must be non-zero)
 *
 */
void cm_free(struct cma *cma, struct page *pages, int count);

#else
struct page *cm_alloc(struct cma *cma, int count, unsigned int order)
{
	return NULL;
};
void cm_free(struct cma *cma, struct page *pages, int count) { }
#endif

#endif

#endif
