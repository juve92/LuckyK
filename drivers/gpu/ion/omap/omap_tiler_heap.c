/*
 * drivers/gpu/ion/omap_tiler_heap.c
 *
 * Copyright (C) 2011 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/spinlock.h>

#include <linux/err.h>
#include <linux/genalloc.h>
#include <linux/io.h>
#include <linux/ion.h>
#include <linux/mm.h>
#include <linux/omap_ion.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <mach/tiler.h>
#include <asm/mach/map.h>
#include <asm/page.h>

#include "../ion_priv.h"

static int omap_tiler_heap_allocate(struct ion_heap *heap,
				    struct ion_buffer *buffer,
				    unsigned long size, unsigned long align,
				    unsigned long flags)
{
	if (buffer->flags & OMAP_ION_FLAG_NO_ALLOC_TILER_HEAP)
		return 0;

	pr_err("%s: This should never be called directly -- use the "
	       "OMAP_ION_TILER_ALLOC flag to the ION_IOC_CUSTOM "
	       "instead\n", __func__);
	return -EINVAL;
}

struct omap_tiler_info {
	tiler_blk_handle tiler_handle;	/* handle of the allocation intiler */
	bool lump;			/* true for a single lump allocation */
	u32 n_phys_pages;		/* number of physical pages */
	u32 *phys_addrs;		/* array addrs of pages */
	u32 n_tiler_pages;		/* number of tiler pages */
	u32 *tiler_addrs;		/* array of addrs of tiler pages */
	u32 tiler_start;		/* start addr in tiler -- if not page
					   aligned this may not equal the
					   first entry onf tiler_addrs */
};

static struct sg_table *omap_tiler_map_dma(struct omap_tiler_info *info,
						struct ion_buffer *buffer)
{
	struct sg_table *table;
	int ret, i;

	if (buffer->sg_table) {
		table = buffer->sg_table;
		sg_free_table(table);
	}
	else {
		table = kzalloc(sizeof(struct sg_table), GFP_KERNEL);
		if (!table)
			return ERR_PTR(-ENOMEM);
	}

	ret = sg_alloc_table(table, (info->lump ? 1 : info->n_tiler_pages),
			GFP_KERNEL);
	if (ret) {
		kfree(table);
		buffer->sg_table = NULL;
		return ERR_PTR(ret);
	}

	if (info->lump) {
		sg_set_page(table->sgl, phys_to_page(info->tiler_addrs[0]),
			    info->n_tiler_pages * PAGE_SIZE, 0);
		return table;
	}

	for (i = 0; i < info->n_tiler_pages; i++) {
		sg_set_page(table->sgl, phys_to_page(info->tiler_addrs[i]),
			    PAGE_SIZE, 0);
	}
	return table;
}

static void omap_tiler_unmap_dma(struct ion_buffer *buffer)
{
	if (buffer->sg_table == NULL)
		return;
	sg_free_table(buffer->sg_table);
	kfree(buffer->sg_table);
}

int omap_tiler_alloc(struct ion_heap *heap,
		     struct ion_client *client,
		     struct omap_ion_tiler_alloc_data *data)
{
	struct ion_handle *handle;
	struct ion_buffer *buffer;
	struct sg_table *sg_table;
	struct omap_tiler_info *info;
	u32 n_phys_pages;
	u32 n_tiler_pages;
	ion_phys_addr_t addr;
	int i, ret;

	if (data->fmt == TILER_PIXEL_FMT_PAGE && data->h != 1) {
		pr_err("%s: Page mode (1D) allocations must have a height "
		       "of one\n", __func__);
		return -EINVAL;
	}

	ret = tiler_memsize(data->fmt, data->w, data->h,
			    &n_phys_pages,
			    &n_tiler_pages);

	if (ret) {
		pr_err("%s: invalid tiler request w %u h %u fmt %u\n", __func__,
		       data->w, data->h, data->fmt);
		return ret;
	}

	BUG_ON(!n_phys_pages || !n_tiler_pages);

	info = kzalloc(sizeof(struct omap_tiler_info) +
		       sizeof(u32) * n_phys_pages +
		       sizeof(u32) * n_tiler_pages, GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->n_phys_pages = n_phys_pages;
	info->n_tiler_pages = n_tiler_pages;
	info->phys_addrs = (u32 *)(info + 1);
	info->tiler_addrs = info->phys_addrs + n_phys_pages;

	info->tiler_handle = tiler_alloc_block_area(data->fmt, data->w, data->h,
						    &info->tiler_start,
						    info->tiler_addrs);
	if (IS_ERR_OR_NULL(info->tiler_handle)) {
		ret = PTR_ERR(info->tiler_handle);
		pr_err("%s: failure to allocate address space from tiler\n",
		       __func__);
		goto err_nomem;
	}

	addr = ion_carveout_allocate(heap, n_phys_pages*PAGE_SIZE, 0);
	if (addr == ION_CARVEOUT_ALLOCATE_FAIL) {
		for (i = 0; i < n_phys_pages; i++) {
			addr = ion_carveout_allocate(heap, PAGE_SIZE, 0);

			if (addr == ION_CARVEOUT_ALLOCATE_FAIL) {
				ret = -ENOMEM;
				pr_err("%s: failed to allocate pages to back "
					"tiler address space\n", __func__);
				goto err_alloc;
			}
			info->phys_addrs[i] = addr;
		}
	} else {
		info->lump = true;
		for (i = 0; i < n_phys_pages; i++)
			info->phys_addrs[i] = addr + i*PAGE_SIZE;
	}

	ret = tiler_pin_block(info->tiler_handle, info->phys_addrs,
			      info->n_phys_pages);
	if (ret) {
		pr_err("%s: failure to pin pages to tiler\n", __func__);
		goto err_alloc;
	}

	data->stride = tiler_block_vstride(info->tiler_handle);

	/* This hack is to avoid the call itself from ion_alloc()
		when the buffer and handle are created */
	handle = ion_alloc(client, PAGE_ALIGN(1), 0, 1 << OMAP_ION_HEAP_TILER,
		heap->flags | OMAP_ION_FLAG_NO_ALLOC_TILER_HEAP);
	if (IS_ERR_OR_NULL(handle)) {
		ret = PTR_ERR(handle);
		pr_err("%s: failure to allocate handle to manage tiler"
		       " allocation\n", __func__);
		goto err;
	}

	buffer = ion_handle_buffer(handle);
	buffer->size = info->n_tiler_pages * PAGE_SIZE;
	buffer->priv_virt = info;
	sg_table = omap_tiler_map_dma(info, buffer);
	if (IS_ERR(sg_table))
		goto err;
	buffer->sg_table = sg_table;
	data->handle = handle;
	return 0;

err:
	tiler_unpin_block(info->tiler_handle);
err_alloc:
	tiler_free_block_area(info->tiler_handle);
	if (info->lump)
		ion_carveout_free(heap, addr, n_phys_pages * PAGE_SIZE);
	else
		for (i -= 1; i >= 0; i--)
			ion_carveout_free(heap, info->phys_addrs[i], PAGE_SIZE);
err_nomem:
	kfree(info);
	return ret;
}

void omap_tiler_heap_free(struct ion_buffer *buffer)
{
	struct omap_tiler_info *info = buffer->priv_virt;

	omap_tiler_unmap_dma(buffer);
	tiler_unpin_block(info->tiler_handle);
	tiler_free_block_area(info->tiler_handle);

	if (info->lump) {
		ion_carveout_free(buffer->heap, info->phys_addrs[0],
				  info->n_phys_pages*PAGE_SIZE);
	} else {
		int i;
		for (i = 0; i < info->n_phys_pages; i++)
			ion_carveout_free(buffer->heap,
					  info->phys_addrs[i], PAGE_SIZE);
	}

	kfree(info);
}

static int omap_tiler_phys(struct ion_heap *heap,
			   struct ion_buffer *buffer,
			   ion_phys_addr_t *addr, size_t *len)
{
	struct omap_tiler_info *info = buffer->priv_virt;

	*addr = info->tiler_start;
	*len = buffer->size;
	return 0;
}

static struct sg_table *omap_tiler_map_dma_empty(
					struct ion_buffer *buffer)
{
	struct sg_table *table;
	int ret;

	table = kzalloc(sizeof(struct sg_table), GFP_KERNEL);
	if (!table)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(table, 1, GFP_KERNEL);
	if (ret) {
		kfree(table);
		return ERR_PTR(ret);
	}

	sg_set_page(table->sgl, virt_to_page(buffer->priv_virt), 1, 0);
	return table;
}

struct sg_table *omap_tiler_heap_map_dma(struct ion_heap *heap,
						struct ion_buffer *buffer)
{
	/*
	* In case if called from omap_tiler_alloc() filling sgtable by fake table since
	* we don't have omap_tiler_alloc_info which is required for proper sgtable
	* allocation.
	* This table will filled properly later by omap_tiler_alloc() itself since
	* it has tiler allocation information.
	*/
	if (buffer->flags & OMAP_ION_FLAG_NO_ALLOC_TILER_HEAP) {
		buffer->flags &= ~OMAP_ION_FLAG_NO_ALLOC_TILER_HEAP;
		return omap_tiler_map_dma_empty(buffer);
	}
	if (buffer->sg_table == NULL)
		return ERR_PTR(-EFAULT);
	return buffer->sg_table;
}

void omap_tiler_heap_unmap_dma(struct ion_heap *heap,
				      struct ion_buffer *buffer)
{
}

int omap_tiler_pages(struct ion_client *client, struct ion_handle *handle,
		     int *n, u32 **tiler_addrs)
{
	ion_phys_addr_t addr;
	size_t len;
	int ret;
	struct omap_tiler_info *info = ion_handle_buffer(handle)->priv_virt;

	/* validate that the handle exists in this client */
	ret = ion_phys(client, handle, &addr, &len);
	if (ret)
		return ret;

	*n = info->n_tiler_pages;
	*tiler_addrs = info->tiler_addrs;
	return 0;
}

int omap_tiler_heap_map_user(struct ion_heap *heap, struct ion_buffer *buffer,
			     struct vm_area_struct *vma)
{
	struct omap_tiler_info *info = buffer->priv_virt;
	unsigned long addr = vma->vm_start;
	u32 vma_pages = (vma->vm_end - vma->vm_start) / PAGE_SIZE;
	int n_pages = min(vma_pages, info->n_tiler_pages);
	int i, ret;

	for (i = vma->vm_pgoff; i < n_pages; i++, addr += PAGE_SIZE) {
		ret = remap_pfn_range(vma, addr,
				      __phys_to_pfn(info->tiler_addrs[i]),
				      PAGE_SIZE,
				      pgprot_noncached(vma->vm_page_prot));
		if (ret)
			return ret;
	}
	return 0;
}

static struct ion_heap_ops omap_tiler_ops = {
	.allocate = omap_tiler_heap_allocate,
	.free = omap_tiler_heap_free,
	.phys = omap_tiler_phys,
	.map_dma = omap_tiler_heap_map_dma,
	.unmap_dma = omap_tiler_heap_unmap_dma,
	.map_user = omap_tiler_heap_map_user,
};

struct ion_heap *omap_tiler_heap_create(struct ion_platform_heap *data)
{
	struct ion_heap *heap;

	heap = ion_carveout_heap_create(data);
	if (!heap)
		return ERR_PTR(-ENOMEM);
	heap->ops = &omap_tiler_ops;
	heap->type = OMAP_ION_HEAP_TYPE_TILER;
	heap->name = data->name;
	heap->id = data->id;
	return heap;
}

void omap_tiler_heap_destroy(struct ion_heap *heap)
{
	kfree(heap);
}
