/*
 * Contiguous Memory Allocator
 * Copyright (c) 2010-2011 by Samsung Electronics.
 * Written by:
 *	Michal Nazarewicz <mina86@mina86.com>
 *	Marek Szyprowski <m.szyprowski@samsung.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License or (at your optional) any later version of the license.
 */

/*
 * See include/linux/cma.h for details.
 */

#define pr_fmt(fmt) "cma: " fmt

#ifdef CONFIG_CMA_DEBUG
#  define DEBUG
#endif

#include <asm/page.h>
#include <asm/errno.h>

#include <linux/cma.h>
#include <linux/memblock.h>
#include <linux/err.h>
#include <linux/genalloc.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/page-isolation.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/mm_types.h>

#include "internal.h"

/* XXX Revisit */
#ifdef phys_to_pfn
/* nothing to do */
#elif defined __phys_to_pfn
#  define phys_to_pfn __phys_to_pfn
#else
#  warning correct phys_to_pfn implementation needed
static unsigned long phys_to_pfn(phys_addr_t phys)
{
	return virt_to_pfn(phys_to_virt(phys));
}
#endif


/************************* Initialise CMA *************************/

static struct cma_grabbed {
	unsigned long start;
	unsigned long size;
} cma_grabbed[8] __initdata;
static unsigned cma_grabbed_count __initdata;

#ifdef CONFIG_DEBUG_VM

static int __cma_give_back(unsigned long start, unsigned long size)
{
	unsigned long pfn = phys_to_pfn(start);
	unsigned i = size >> PAGE_SHIFT;
	struct zone *zone;

	pr_debug("%s(%p+%p)\n", __func__, (void *)start, (void *)size);

	VM_BUG_ON(!pfn_valid(pfn));
	zone = page_zone(pfn_to_page(pfn));

	do {
		VM_BUG_ON(!pfn_valid(pfn));
		VM_BUG_ON(page_zone(pfn_to_page(pfn)) != zone);
		if (!(pfn & (pageblock_nr_pages - 1)))
			__free_pageblock_cma(pfn_to_page(pfn));
		++pfn;
		++totalram_pages;
	} while (--i);

	return 0;
}

#else

static int __cma_give_back(unsigned long start, unsigned long size)
{
	unsigned i = size >> (PAGE_SHIFT + pageblock_order);
	struct page *p = phys_to_page(start);

	pr_debug("%s(%p+%p)\n", __func__, (void *)start, (void *)size);

	do {
		__free_pageblock_cma(p);
		p += pageblock_nr_pages;
		totalram_pages += pageblock_nr_pages;
	} while (--i);

	return 0;
}

#endif

static int __init __cma_queue_give_back(unsigned long start, unsigned long size)
{
	if (cma_grabbed_count == ARRAY_SIZE(cma_grabbed))
		return -ENOSPC;

	cma_grabbed[cma_grabbed_count].start = start;
	cma_grabbed[cma_grabbed_count].size  = size;
	++cma_grabbed_count;
	return 0;
}

static int (*cma_give_back)(unsigned long start, unsigned long size) =
	__cma_queue_give_back;

static int __init cma_give_back_queued(void)
{
	struct cma_grabbed *r = cma_grabbed;
	unsigned i = cma_grabbed_count;

	pr_debug("%s(): will give %u range(s)\n", __func__, i);

	cma_give_back = __cma_give_back;

	for (; i; --i, ++r)
		__cma_give_back(r->start, r->size);

	return 0;
}
core_initcall(cma_give_back_queued);

int __ref cma_init_migratetype(unsigned long start, unsigned long size)
{
	pr_debug("%s(%p+%p)\n", __func__, (void *)start, (void *)size);

	if (!size)
		return -EINVAL;
	if ((start | size) & ((MAX_ORDER_NR_PAGES << PAGE_SHIFT) - 1))
		return -EINVAL;
	if (start + size < start)
		return -EOVERFLOW;

	return cma_give_back(start, size);
}

unsigned long cma_reserve(unsigned long start, unsigned long size)
{
	unsigned long alignment;
	int ret;

	pr_debug("%s(%p+%p)\n", __func__, (void *)start, (void *)size);

	/* Sanity checks */
	if (!size)
		return (unsigned long)-EINVAL;

	/* Sanitise input arguments */
	start = ALIGN(start, MAX_ORDER_NR_PAGES << PAGE_SHIFT);
	size  = ALIGN(size , MAX_ORDER_NR_PAGES << PAGE_SHIFT);
	alignment = MAX_ORDER_NR_PAGES << PAGE_SHIFT;

	/* Reserve memory */
	if (start) {
		if (memblock_is_region_reserved(start, size) ||
		    memblock_reserve(start, size) < 0)
			return (unsigned long)-EBUSY;
	} else {
		/*
		 * Use __memblock_alloc_base() since
		 * memblock_alloc_base() panic()s.
		 */
		u64 addr = __memblock_alloc_base(size, alignment, 0);
		if (!addr) {
			return (unsigned long)-ENOMEM;
		} else if (addr + size > ~(unsigned long)0) {
			memblock_free(addr, size);
			return (unsigned long)-EOVERFLOW;
		} else {
			start = addr;
		}
	}

	/* CMA Initialise */
	ret = cma_init_migratetype(start, size);
	if (ret < 0) {
		memblock_free(start, size);
		return ret;
	}

	return start;
}


/************************** CMA context ***************************/

struct cma {
	int migratetype;
	struct gen_pool *pool;
};

static int __cma_check_range(unsigned long start, unsigned long size)
{
	int migratetype = MIGRATE_MOVABLE;
	unsigned long pfn, count;
	struct page *page;
	struct zone *zone;

	start = phys_to_pfn(start);
	if (WARN_ON(!pfn_valid(start)))
		return -EINVAL;

	if (page_zonenum(pfn_to_page(start)) != ZONE_MOVABLE)
		migratetype = MIGRATE_CMA;

	/* First check if all pages are valid and in the same zone */
	zone  = page_zone(pfn_to_page(start));
	count = size >> PAGE_SHIFT;
	pfn   = start;
	while (++pfn, --count) {
		if (WARN_ON(!pfn_valid(pfn)) ||
		    WARN_ON(page_zone(pfn_to_page(pfn)) != zone))
			return -EINVAL;
	}

	/* Now check migratetype of their pageblocks. */
	start = start & ~(pageblock_nr_pages - 1);
	pfn   = ALIGN(pfn, pageblock_nr_pages);
	page  = pfn_to_page(start);
	count = (pfn - start) >> PAGE_SHIFT;
	do {
		if (WARN_ON(get_pageblock_migratetype(page) != migratetype))
			return -EINVAL;
		page += pageblock_nr_pages;
	} while (--count);

	return migratetype;
}

struct cma *cma_create(unsigned long start, unsigned long size)
{
	struct gen_pool *pool;
	int migratetype, ret;
	struct cma *cma;

	pr_debug("%s(%p+%p)\n", __func__, (void *)start, (void *)size);

	if (!size)
		return ERR_PTR(-EINVAL);
	if ((start | size) & (PAGE_SIZE - 1))
		return ERR_PTR(-EINVAL);
	if (start + size < start)
		return ERR_PTR(-EOVERFLOW);

	migratetype = __cma_check_range(start, size);
	if (migratetype < 0)
		return ERR_PTR(migratetype);

	cma = kmalloc(sizeof *cma, GFP_KERNEL);
	if (!cma)
		return ERR_PTR(-ENOMEM);

	pool = gen_pool_create(ffs(PAGE_SIZE) - 1, -1);
	if (!pool) {
		ret = -ENOMEM;
		goto error1;
	}

	ret = gen_pool_add(pool, start, size, -1);
	if (unlikely(ret))
		goto error2;

	cma->migratetype = migratetype;
	cma->pool = pool;

	pr_debug("%s: returning <%p>\n", __func__, (void *)cma);
	return cma;

error2:
	gen_pool_destroy(pool);
error1:
	kfree(cma);
	return ERR_PTR(ret);
}

void cma_destroy(struct cma *cma)
{
	pr_debug("%s(<%p>)\n", __func__, (void *)cma);
	gen_pool_destroy(cma->pool);
}


/************************* Allocate and free *************************/

/* Protects cm_alloc(), cm_free() as well as gen_pools of each cm. */
static DEFINE_MUTEX(cma_mutex);

struct page *cm_alloc(struct cma *cma, int count, unsigned int order)
{
	unsigned long start;
	unsigned long size = count << PAGE_SHIFT;

	if (!cma)
		return NULL;

	pr_debug("%s(<%p>, %lx/%d)\n", __func__, (void *)cma, size, order);

	if (!size)
		return NULL;

	mutex_lock(&cma_mutex);

	start = gen_pool_alloc_aligned(cma->pool, size, order+12);
	if (!start)
		goto error1;

	if (cma->migratetype) {
		unsigned long pfn = phys_to_pfn(start);
		int ret = alloc_contig_range(pfn, pfn + (size >> PAGE_SHIFT),
					     0, cma->migratetype);
		if (ret)
			goto error2;
	}

	mutex_unlock(&cma_mutex);

	pr_debug("%s(): returning [%p]\n", __func__, (void *)phys_to_page(start));
	return phys_to_page(start);
error2:
	gen_pool_free(cma->pool, start, size);
error1:
	mutex_unlock(&cma_mutex);
	return NULL;
}
EXPORT_SYMBOL_GPL(cm_alloc);

void cm_free(struct cma *cma, struct page *pages, int count)
{
	unsigned long size = count << PAGE_SHIFT;
	pr_debug("%s([%p])\n", __func__, (void *)pages);

	if (!cma || !pages)
		return;

	mutex_lock(&cma_mutex);

	gen_pool_free(cma->pool, page_to_phys(pages), size);
	if (cma->migratetype)
		free_contig_pages(pages, count);

	mutex_unlock(&cma_mutex);
}
EXPORT_SYMBOL_GPL(cm_free);
