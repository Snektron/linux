// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Red Hat.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *     Danilo Krummrich <dakr@redhat.com>
 *
 */

#include <drm/drm_gem.h>
#include <drm/drm_gpuva_mgr.h>

/**
 * DOC: Overview
 *
 * The DRM GPU VA Manager, represented by struct drm_gpuva_manager keeps track
 * of a GPU's virtual address (VA) space and manages the corresponding virtual
 * mappings represented by &drm_gpuva objects. It also keeps track of the
 * mapping's backing &drm_gem_object buffers.
 *
 * &drm_gem_object buffers maintain a list (and a corresponding list lock) of
 * &drm_gpuva objects representing all existent GPU VA mappings using this
 * &drm_gem_object as backing buffer.
 *
 * GPU VAs can be flagged as sparse, such that drivers may use GPU VAs to also
 * keep track of sparse PTEs in order to support Vulkan 'Sparse Resources'.
 *
 * The GPU VA manager internally uses a &maple_tree to manage the
 * &drm_gpuva mappings within a GPU's virtual address space.
 *
 * The &drm_gpuva_manager contains a special &drm_gpuva representing the
 * portion of VA space reserved by the kernel. This node is initialized together
 * with the GPU VA manager instance and removed when the GPU VA manager is
 * destroyed.
 *
 * In a typical application drivers would embed struct drm_gpuva_manager and
 * struct drm_gpuva within their own driver specific structures, there won't be
 * any memory allocations of it's own nor memory allocations of &drm_gpuva
 * entries.
 *
 * However, the &drm_gpuva_manager needs to allocate nodes for it's internal
 * tree structures when &drm_gpuva entries are inserted. In order to support
 * inserting &drm_gpuva entries from dma-fence signalling critical sections the
 * &drm_gpuva_manager provides struct drm_gpuva_prealloc. Drivers may create
 * pre-allocated nodes which drm_gpuva_prealloc_create() and subsequently insert
 * a new &drm_gpuva entry with drm_gpuva_insert_prealloc().
 */

/**
 * DOC: Split and Merge
 *
 * The DRM GPU VA manager also provides an algorithm implementing splitting and
 * merging of existent GPU VA mappings with the ones that are requested to be
 * mapped or unmapped. This feature is required by the Vulkan API to implement
 * Vulkan 'Sparse Memory Bindings' - drivers UAPIs often refer to this as
 * VM BIND.
 *
 * Drivers can call drm_gpuva_sm_map() to receive a sequence of callbacks
 * containing map, unmap and remap operations for a given newly requested
 * mapping. The sequence of callbacks represents the set of operations to
 * execute in order to integrate the new mapping cleanly into the current state
 * of the GPU VA space.
 *
 * Depending on how the new GPU VA mapping intersects with the existent mappings
 * of the GPU VA space the &drm_gpuva_fn_ops callbacks contain an arbitrary
 * amount of unmap operations, a maximum of two remap operations and a single
 * map operation. The caller might receive no callback at all if no operation is
 * required, e.g. if the requested mapping already exists in the exact same way.
 *
 * The single map operation represents the original map operation requested by
 * the caller.
 *
 * &drm_gpuva_op_unmap contains a 'keep' field, which indicates whether the
 * &drm_gpuva to unmap is physically contiguous with the original mapping
 * request. Optionally, if 'keep' is set, drivers may keep the actual page table
 * entries for this &drm_gpuva, adding the missing page table entries only and
 * update the &drm_gpuva_manager's view of things accordingly.
 *
 * Drivers may do the same optimization, namely delta page table updates, also
 * for remap operations. This is possible since &drm_gpuva_op_remap consists of
 * one unmap operation and one or two map operations, such that drivers can
 * derive the page table update delta accordingly.
 *
 * Note that there can't be more than two existent mappings to split up, one at
 * the beginning and one at the end of the new mapping, hence there is a
 * maximum of two remap operations.
 *
 * Analogous to drm_gpuva_sm_map() drm_gpuva_sm_unmap() uses &drm_gpuva_fn_ops
 * to call back into the driver in order to unmap a range of GPU VA space. The
 * logic behind this function is way simpler though: For all existent mappings
 * enclosed by the given range unmap operations are created. For mappings which
 * are only partically located within the given range, remap operations are
 * created such that those mappings are split up and re-mapped partically.
 *
 * To update the &drm_gpuva_manager's view of the GPU VA space
 * drm_gpuva_insert(), drm_gpuva_insert_prealloc(), and drm_gpuva_remove() may
 * be used. Please note that these functions are not safe to be called from a
 * &drm_gpuva_fn_ops callback originating from drm_gpuva_sm_map() or
 * drm_gpuva_sm_unmap(). The drm_gpuva_map(), drm_gpuva_remap() and
 * drm_gpuva_unmap() helpers should be used instead.
 *
 * The following diagram depicts the basic relationships of existent GPU VA
 * mappings, a newly requested mapping and the resulting mappings as implemented
 * by drm_gpuva_sm_map() - it doesn't cover any arbitrary combinations of these.
 *
 * 1) Requested mapping is identical. Replace it, but indicate the backing PTEs
 *    could be kept.
 *
 *    ::
 *
 *	     0     a     1
 *	old: |-----------| (bo_offset=n)
 *
 *	     0     a     1
 *	req: |-----------| (bo_offset=n)
 *
 *	     0     a     1
 *	new: |-----------| (bo_offset=n)
 *
 *
 * 2) Requested mapping is identical, except for the BO offset, hence replace
 *    the mapping.
 *
 *    ::
 *
 *	     0     a     1
 *	old: |-----------| (bo_offset=n)
 *
 *	     0     a     1
 *	req: |-----------| (bo_offset=m)
 *
 *	     0     a     1
 *	new: |-----------| (bo_offset=m)
 *
 *
 * 3) Requested mapping is identical, except for the backing BO, hence replace
 *    the mapping.
 *
 *    ::
 *
 *	     0     a     1
 *	old: |-----------| (bo_offset=n)
 *
 *	     0     b     1
 *	req: |-----------| (bo_offset=n)
 *
 *	     0     b     1
 *	new: |-----------| (bo_offset=n)
 *
 *
 * 4) Existent mapping is a left aligned subset of the requested one, hence
 *    replace the existent one.
 *
 *    ::
 *
 *	     0  a  1
 *	old: |-----|       (bo_offset=n)
 *
 *	     0     a     2
 *	req: |-----------| (bo_offset=n)
 *
 *	     0     a     2
 *	new: |-----------| (bo_offset=n)
 *
 *    .. note::
 *       We expect to see the same result for a request with a different BO
 *       and/or non-contiguous BO offset.
 *
 *
 * 5) Requested mapping's range is a left aligned subset of the existent one,
 *    but backed by a different BO. Hence, map the requested mapping and split
 *    the existent one adjusting it's BO offset.
 *
 *    ::
 *
 *	     0     a     2
 *	old: |-----------| (bo_offset=n)
 *
 *	     0  b  1
 *	req: |-----|       (bo_offset=n)
 *
 *	     0  b  1  a' 2
 *	new: |-----|-----| (b.bo_offset=n, a.bo_offset=n+1)
 *
 *    .. note::
 *       We expect to see the same result for a request with a different BO
 *       and/or non-contiguous BO offset.
 *
 *
 * 6) Existent mapping is a superset of the requested mapping. Split it up, but
 *    indicate that the backing PTEs could be kept.
 *
 *    ::
 *
 *	     0     a     2
 *	old: |-----------| (bo_offset=n)
 *
 *	     0  a  1
 *	req: |-----|       (bo_offset=n)
 *
 *	     0  a  1  a' 2
 *	new: |-----|-----| (a.bo_offset=n, a'.bo_offset=n+1)
 *
 *
 * 7) Requested mapping's range is a right aligned subset of the existent one,
 *    but backed by a different BO. Hence, map the requested mapping and split
 *    the existent one, without adjusting the BO offset.
 *
 *    ::
 *
 *	     0     a     2
 *	old: |-----------| (bo_offset=n)
 *
 *	           1  b  2
 *	req:       |-----| (bo_offset=m)
 *
 *	     0  a  1  b  2
 *	new: |-----|-----| (a.bo_offset=n,b.bo_offset=m)
 *
 *
 * 8) Existent mapping is a superset of the requested mapping. Split it up, but
 *    indicate that the backing PTEs could be kept.
 *
 *    ::
 *
 *	      0     a     2
 *	old: |-----------| (bo_offset=n)
 *
 *	           1  a  2
 *	req:       |-----| (bo_offset=n+1)
 *
 *	     0  a' 1  a  2
 *	new: |-----|-----| (a'.bo_offset=n, a.bo_offset=n+1)
 *
 *
 * 9) Existent mapping is overlapped at the end by the requested mapping backed
 *    by a different BO. Hence, map the requested mapping and split up the
 *    existent one, without adjusting the BO offset.
 *
 *    ::
 *
 *	     0     a     2
 *	old: |-----------|       (bo_offset=n)
 *
 *	           1     b     3
 *	req:       |-----------| (bo_offset=m)
 *
 *	     0  a  1     b     3
 *	new: |-----|-----------| (a.bo_offset=n,b.bo_offset=m)
 *
 *
 * 10) Existent mapping is overlapped by the requested mapping, both having the
 *     same backing BO with a contiguous offset. Indicate the backing PTEs of
 *     the old mapping could be kept.
 *
 *     ::
 *
 *	      0     a     2
 *	 old: |-----------|       (bo_offset=n)
 *
 *	            1     a     3
 *	 req:       |-----------| (bo_offset=n+1)
 *
 *	      0  a' 1     a     3
 *	 new: |-----|-----------| (a'.bo_offset=n, a.bo_offset=n+1)
 *
 *
 * 11) Requested mapping's range is a centered subset of the existent one
 *     having a different backing BO. Hence, map the requested mapping and split
 *     up the existent one in two mappings, adjusting the BO offset of the right
 *     one accordingly.
 *
 *     ::
 *
 *	      0        a        3
 *	 old: |-----------------| (bo_offset=n)
 *
 *	            1  b  2
 *	 req:       |-----|       (bo_offset=m)
 *
 *	      0  a  1  b  2  a' 3
 *	 new: |-----|-----|-----| (a.bo_offset=n,b.bo_offset=m,a'.bo_offset=n+2)
 *
 *
 * 12) Requested mapping is a contiguous subset of the existent one. Split it
 *     up, but indicate that the backing PTEs could be kept.
 *
 *     ::
 *
 *	      0        a        3
 *	 old: |-----------------| (bo_offset=n)
 *
 *	            1  a  2
 *	 req:       |-----|       (bo_offset=n+1)
 *
 *	      0  a' 1  a  2 a'' 3
 *	 old: |-----|-----|-----| (a'.bo_offset=n, a.bo_offset=n+1, a''.bo_offset=n+2)
 *
 *
 * 13) Existent mapping is a right aligned subset of the requested one, hence
 *     replace the existent one.
 *
 *     ::
 *
 *	            1  a  2
 *	 old:       |-----| (bo_offset=n+1)
 *
 *	      0     a     2
 *	 req: |-----------| (bo_offset=n)
 *
 *	      0     a     2
 *	 new: |-----------| (bo_offset=n)
 *
 *     .. note::
 *        We expect to see the same result for a request with a different bo
 *        and/or non-contiguous bo_offset.
 *
 *
 * 14) Existent mapping is a centered subset of the requested one, hence
 *     replace the existent one.
 *
 *     ::
 *
 *	            1  a  2
 *	 old:       |-----| (bo_offset=n+1)
 *
 *	      0        a       3
 *	 req: |----------------| (bo_offset=n)
 *
 *	      0        a       3
 *	 new: |----------------| (bo_offset=n)
 *
 *     .. note::
 *        We expect to see the same result for a request with a different bo
 *        and/or non-contiguous bo_offset.
 *
 *
 * 15) Existent mappings is overlapped at the beginning by the requested mapping
 *     backed by a different BO. Hence, map the requested mapping and split up
 *     the existent one, adjusting it's BO offset accordingly.
 *
 *     ::
 *
 *	            1     a     3
 *	 old:       |-----------| (bo_offset=n)
 *
 *	      0     b     2
 *	 req: |-----------|       (bo_offset=m)
 *
 *	      0     b     2  a' 3
 *	 new: |-----------|-----| (b.bo_offset=m,a.bo_offset=n+2)
 */

/**
 * DOC: Locking
 *
 * Generally, the GPU VA manager does not take care of locking itself, it is
 * the drivers responsibility to take care about locking. Drivers might want to
 * protect the following operations: inserting, removing and iterating
 * &drm_gpuva objects as well as generating all kinds of operations, such as
 * split / merge or prefetch.
 *
 * The GPU VA manager also does not take care of the locking of the backing
 * &drm_gem_object buffers GPU VA lists by itself; drivers are responsible to
 * enforce mutual exclusion.
 */

 /*
  * Maple Tree Locking
  *
  * The maple tree's advanced API requires the user of the API to protect
  * certain tree operations with a lock (either the external or internal tree
  * lock) for tree internal reasons.
  *
  * The actual rules (when to aquire/release the lock) are enforced by lockdep
  * through the maple tree implementation.
  *
  * For this reason the DRM GPUVA manager takes the maple tree's internal
  * spinlock according to the lockdep enforced rules.
  *
  * Please note, that this lock is *only* meant to fulfill the maple trees
  * requirements and does not intentionally protect the DRM GPUVA manager
  * against concurrent access.
  *
  * The following mail thread provides more details on why the maple tree
  * has this requirement.
  *
  * https://lore.kernel.org/lkml/20230217134422.14116-5-dakr@redhat.com/
  */

static int __drm_gpuva_insert(struct drm_gpuva_manager *mgr,
			      struct drm_gpuva *va);
static void __drm_gpuva_remove(struct drm_gpuva *va);

/**
 * drm_gpuva_manager_init - initialize a &drm_gpuva_manager
 * @mgr: pointer to the &drm_gpuva_manager to initialize
 * @name: the name of the GPU VA space
 * @start_offset: the start offset of the GPU VA space
 * @range: the size of the GPU VA space
 * @reserve_offset: the start of the kernel reserved GPU VA area
 * @reserve_range: the size of the kernel reserved GPU VA area
 * @ops: &drm_gpuva_fn_ops called on &drm_gpuva_sm_map / &drm_gpuva_sm_unmap
 *
 * The &drm_gpuva_manager must be initialized with this function before use.
 *
 * Note that @mgr must be cleared to 0 before calling this function. The given
 * &name is expected to be managed by the surrounding driver structures.
 */
void
drm_gpuva_manager_init(struct drm_gpuva_manager *mgr,
		       const char *name,
		       u64 start_offset, u64 range,
		       u64 reserve_offset, u64 reserve_range,
		       struct drm_gpuva_fn_ops *ops)
{
	mt_init(&mgr->mtree);

	mgr->mm_start = start_offset;
	mgr->mm_range = range;

	mgr->name = name ? name : "unknown";
	mgr->ops = ops;

	memset(&mgr->kernel_alloc_node, 0, sizeof(struct drm_gpuva));

	if (reserve_range) {
		mgr->kernel_alloc_node.va.addr = reserve_offset;
		mgr->kernel_alloc_node.va.range = reserve_range;

		__drm_gpuva_insert(mgr, &mgr->kernel_alloc_node);
	}

}
EXPORT_SYMBOL(drm_gpuva_manager_init);

/**
 * drm_gpuva_manager_destroy - cleanup a &drm_gpuva_manager
 * @mgr: pointer to the &drm_gpuva_manager to clean up
 *
 * Note that it is a bug to call this function on a manager that still
 * holds GPU VA mappings.
 */
void
drm_gpuva_manager_destroy(struct drm_gpuva_manager *mgr)
{
	mgr->name = NULL;

	if (mgr->kernel_alloc_node.va.range)
		__drm_gpuva_remove(&mgr->kernel_alloc_node);

	mtree_lock(&mgr->mtree);
	WARN(!mtree_empty(&mgr->mtree),
	     "GPUVA tree is not empty, potentially leaking memory.");
	__mt_destroy(&mgr->mtree);
	mtree_unlock(&mgr->mtree);
}
EXPORT_SYMBOL(drm_gpuva_manager_destroy);

static inline bool
drm_gpuva_in_mm_range(struct drm_gpuva_manager *mgr, u64 addr, u64 range)
{
	u64 end = addr + range;
	u64 mm_start = mgr->mm_start;
	u64 mm_end = mm_start + mgr->mm_range;

	return addr < mm_end && mm_start < end;
}

static inline bool
drm_gpuva_in_kernel_node(struct drm_gpuva_manager *mgr, u64 addr, u64 range)
{
	u64 end = addr + range;
	u64 kstart = mgr->kernel_alloc_node.va.addr;
	u64 krange = mgr->kernel_alloc_node.va.range;
	u64 kend = kstart + krange;

	return krange && addr < kend && kstart < end;
}

static inline bool
drm_gpuva_range_valid(struct drm_gpuva_manager *mgr,
		      u64 addr, u64 range)
{
	return drm_gpuva_in_mm_range(mgr, addr, range) &&
	       !drm_gpuva_in_kernel_node(mgr, addr, range);
}

/**
 * drm_gpuva_iter_remove - removes the iterators current element
 * @it: the &drm_gpuva_iterator
 *
 * This removes the element the iterator currently points to.
 */
void
drm_gpuva_iter_remove(struct drm_gpuva_iterator *it)
{
	mas_lock(&it->mas);
	mas_erase(&it->mas);
	mas_unlock(&it->mas);
}
EXPORT_SYMBOL(drm_gpuva_iter_remove);

/**
 * drm_gpuva_prealloc_create - creates a preallocated node to store a
 * &drm_gpuva entry.
 *
 * Returns: the &drm_gpuva_prealloc object on success, NULL on failure
 */
struct drm_gpuva_prealloc *
drm_gpuva_prealloc_create(void)
{
	struct drm_gpuva_prealloc *pa;

	pa = kzalloc(sizeof(*pa), GFP_KERNEL);
	if (!pa)
		return NULL;

	if (mas_preallocate(&pa->mas, GFP_KERNEL)) {
		kfree(pa);
		return NULL;
	}

	return pa;
}
EXPORT_SYMBOL(drm_gpuva_prealloc_create);

/**
 * drm_gpuva_prealloc_destroy - destroyes a preallocated node and frees the
 * &drm_gpuva_prealloc
 *
 * @pa: the &drm_gpuva_prealloc to destroy
 */
void
drm_gpuva_prealloc_destroy(struct drm_gpuva_prealloc *pa)
{
	mas_destroy(&pa->mas);
	kfree(pa);
}
EXPORT_SYMBOL(drm_gpuva_prealloc_destroy);

static int
drm_gpuva_insert_state(struct drm_gpuva_manager *mgr,
		       struct ma_state *mas,
		       struct drm_gpuva *va)
{
	u64 addr = va->va.addr;
	u64 range = va->va.range;
	u64 last = addr + range - 1;

	mas_set(mas, addr);

	mas_lock(mas);
	if (unlikely(mas_walk(mas))) {
		mas_unlock(mas);
		return -EEXIST;
	}

	if (unlikely(mas->last < last)) {
		mas_unlock(mas);
		return -EEXIST;
	}

	mas->index = addr;
	mas->last = last;

	mas_store_prealloc(mas, va);
	mas_unlock(mas);

	va->mgr = mgr;

	return 0;
}

static int
__drm_gpuva_insert(struct drm_gpuva_manager *mgr,
		   struct drm_gpuva *va)
{
	MA_STATE(mas, &mgr->mtree, 0, 0);
	int ret;

	ret = mas_preallocate(&mas, GFP_KERNEL);
	if (ret)
		return ret;

	return drm_gpuva_insert_state(mgr, &mas, va);
}

/**
 * drm_gpuva_insert - insert a &drm_gpuva
 * @mgr: the &drm_gpuva_manager to insert the &drm_gpuva in
 * @va: the &drm_gpuva to insert
 *
 * Insert a &drm_gpuva with a given address and range into a
 * &drm_gpuva_manager.
 *
 * It is not allowed to use this function while iterating this GPU VA space,
 * e.g via drm_gpuva_iter_for_each().
 *
 * Returns: 0 on success, negative error code on failure.
 */
int
drm_gpuva_insert(struct drm_gpuva_manager *mgr,
		 struct drm_gpuva *va)
{
	u64 addr = va->va.addr;
	u64 range = va->va.range;

	if (unlikely(!drm_gpuva_range_valid(mgr, addr, range)))
		return -EINVAL;

	return __drm_gpuva_insert(mgr, va);
}
EXPORT_SYMBOL(drm_gpuva_insert);

/**
 * drm_gpuva_insert_prealloc - insert a &drm_gpuva with a preallocated node
 * @mgr: the &drm_gpuva_manager to insert the &drm_gpuva in
 * @va: the &drm_gpuva to insert
 * @pa: the &drm_gpuva_prealloc node
 *
 * Insert a &drm_gpuva with a given address and range into a
 * &drm_gpuva_manager.
 *
 * It is not allowed to use this function while iterating this GPU VA space,
 * e.g via drm_gpuva_iter_for_each().
 *
 * Returns: 0 on success, negative error code on failure.
 */
int
drm_gpuva_insert_prealloc(struct drm_gpuva_manager *mgr,
			  struct drm_gpuva_prealloc *pa,
			  struct drm_gpuva *va)
{
	struct ma_state *mas = &pa->mas;
	u64 addr = va->va.addr;
	u64 range = va->va.range;

	if (unlikely(!drm_gpuva_range_valid(mgr, addr, range)))
		return -EINVAL;

	mas->tree = &mgr->mtree;
	return drm_gpuva_insert_state(mgr, mas, va);
}
EXPORT_SYMBOL(drm_gpuva_insert_prealloc);

static void
__drm_gpuva_remove(struct drm_gpuva *va)
{
	MA_STATE(mas, &va->mgr->mtree, va->va.addr, 0);

	mas_lock(&mas);
	mas_erase(&mas);
	mas_unlock(&mas);
}

/**
 * drm_gpuva_remove - remove a &drm_gpuva
 * @va: the &drm_gpuva to remove
 *
 * This removes the given &va from the underlaying tree.
 *
 * It is not allowed to use this function while iterating this GPU VA space,
 * e.g via drm_gpuva_iter_for_each(). Please use drm_gpuva_iter_remove()
 * instead.
 */
void
drm_gpuva_remove(struct drm_gpuva *va)
{
	struct drm_gpuva_manager *mgr = va->mgr;

	if (unlikely(va == &mgr->kernel_alloc_node)) {
		WARN(1, "Can't destroy kernel reserved node.\n");
		return;
	}

	__drm_gpuva_remove(va);
}
EXPORT_SYMBOL(drm_gpuva_remove);

/**
 * drm_gpuva_link - link a &drm_gpuva
 * @va: the &drm_gpuva to link
 *
 * This adds the given &va to the GPU VA list of the &drm_gem_object it is
 * associated with.
 *
 * This function expects the caller to protect the GEM's GPUVA list against
 * concurrent access.
 */
void
drm_gpuva_link(struct drm_gpuva *va)
{
	if (likely(va->gem.obj))
		list_add_tail(&va->gem.entry, &va->gem.obj->gpuva.list);
}
EXPORT_SYMBOL(drm_gpuva_link);

/**
 * drm_gpuva_unlink - unlink a &drm_gpuva
 * @va: the &drm_gpuva to unlink
 *
 * This removes the given &va from the GPU VA list of the &drm_gem_object it is
 * associated with.
 *
 * This function expects the caller to protect the GEM's GPUVA list against
 * concurrent access.
 */
void
drm_gpuva_unlink(struct drm_gpuva *va)
{
	if (likely(va->gem.obj))
		list_del_init(&va->gem.entry);
}
EXPORT_SYMBOL(drm_gpuva_unlink);

/**
 * drm_gpuva_find_first - find the first &drm_gpuva in the given range
 * @mgr: the &drm_gpuva_manager to search in
 * @addr: the &drm_gpuvas address
 * @range: the &drm_gpuvas range
 *
 * Returns: the first &drm_gpuva within the given range
 */
struct drm_gpuva *
drm_gpuva_find_first(struct drm_gpuva_manager *mgr,
		     u64 addr, u64 range)
{
	MA_STATE(mas, &mgr->mtree, addr, 0);
	struct drm_gpuva *va;

	mas_lock(&mas);
	va = mas_find(&mas, addr + range - 1);
	mas_unlock(&mas);

	return va;
}
EXPORT_SYMBOL(drm_gpuva_find_first);

/**
 * drm_gpuva_find - find a &drm_gpuva
 * @mgr: the &drm_gpuva_manager to search in
 * @addr: the &drm_gpuvas address
 * @range: the &drm_gpuvas range
 *
 * Returns: the &drm_gpuva at a given &addr and with a given &range
 */
struct drm_gpuva *
drm_gpuva_find(struct drm_gpuva_manager *mgr,
	       u64 addr, u64 range)
{
	struct drm_gpuva *va;

	va = drm_gpuva_find_first(mgr, addr, range);
	if (!va)
		goto out;

	if (va->va.addr != addr ||
	    va->va.range != range)
		goto out;

	return va;

out:
	return NULL;
}
EXPORT_SYMBOL(drm_gpuva_find);

/**
 * drm_gpuva_find_prev - find the &drm_gpuva before the given address
 * @mgr: the &drm_gpuva_manager to search in
 * @start: the given GPU VA's start address
 *
 * Find the adjacent &drm_gpuva before the GPU VA with given &start address.
 *
 * Note that if there is any free space between the GPU VA mappings no mapping
 * is returned.
 *
 * Returns: a pointer to the found &drm_gpuva or NULL if none was found
 */
struct drm_gpuva *
drm_gpuva_find_prev(struct drm_gpuva_manager *mgr, u64 start)
{
	MA_STATE(mas, &mgr->mtree, start - 1, 0);
	struct drm_gpuva *va;

	if (start <= mgr->mm_start ||
	    start > (mgr->mm_start + mgr->mm_range))
		return NULL;

	mas_lock(&mas);
	va = mas_walk(&mas);
	mas_unlock(&mas);

	return va;
}
EXPORT_SYMBOL(drm_gpuva_find_prev);

/**
 * drm_gpuva_find_next - find the &drm_gpuva after the given address
 * @mgr: the &drm_gpuva_manager to search in
 * @end: the given GPU VA's end address
 *
 * Find the adjacent &drm_gpuva after the GPU VA with given &end address.
 *
 * Note that if there is any free space between the GPU VA mappings no mapping
 * is returned.
 *
 * Returns: a pointer to the found &drm_gpuva or NULL if none was found
 */
struct drm_gpuva *
drm_gpuva_find_next(struct drm_gpuva_manager *mgr, u64 end)
{
	MA_STATE(mas, &mgr->mtree, end, 0);
	struct drm_gpuva *va;

	if (end < mgr->mm_start ||
	    end >= (mgr->mm_start + mgr->mm_range))
		return NULL;

	mas_lock(&mas);
	va = mas_walk(&mas);
	mas_unlock(&mas);

	return va;
}
EXPORT_SYMBOL(drm_gpuva_find_next);

/**
 * drm_gpuva_interval_empty - indicate whether a given interval of the VA space
 * is empty
 * @mgr: the &drm_gpuva_manager to check the range for
 * @addr: the start address of the range
 * @range: the range of the interval
 *
 * Returns: true if the interval is empty, false otherwise
 */
bool
drm_gpuva_interval_empty(struct drm_gpuva_manager *mgr, u64 addr, u64 range)
{
	DRM_GPUVA_ITER(it, mgr, addr);
	struct drm_gpuva *va;

	drm_gpuva_iter_for_each_range(va, it, addr + range)
		return false;

	return true;
}
EXPORT_SYMBOL(drm_gpuva_interval_empty);

/**
 * drm_gpuva_map - helper to insert a &drm_gpuva from &drm_gpuva_fn_ops
 * callbacks
 *
 * @mgr: the &drm_gpuva_manager
 * @pa: the &drm_gpuva_prealloc
 * @va: the &drm_gpuva to inser
 */
int
drm_gpuva_map(struct drm_gpuva_manager *mgr,
	      struct drm_gpuva_prealloc *pa,
	      struct drm_gpuva *va)
{
	return drm_gpuva_insert_prealloc(mgr, pa, va);
}
EXPORT_SYMBOL(drm_gpuva_map);

/**
 * drm_gpuva_remap - helper to insert a &drm_gpuva from &drm_gpuva_fn_ops
 * callbacks
 *
 * @state: the current &drm_gpuva_state
 * @prev: the &drm_gpuva to remap when keeping the start of a mapping,
 * may be NULL
 * @next: the &drm_gpuva to remap when keeping the end of a mapping,
 * may be NULL
 */
int
drm_gpuva_remap(drm_gpuva_state_t state,
		struct drm_gpuva *prev,
		struct drm_gpuva *next)
{
	struct ma_state *mas = &state->mas;
	u64 max = mas->last;

	if (unlikely(!prev && !next))
		return -EINVAL;

	if (prev) {
		u64 addr = prev->va.addr;
		u64 last = addr + prev->va.range - 1;

		if (unlikely(addr != mas->index))
			return -EINVAL;

		if (unlikely(last >= mas->last))
			return -EINVAL;
	}

	if (next) {
		u64 addr = next->va.addr;
		u64 last = addr + next->va.range - 1;

		if (unlikely(last != mas->last))
			return -EINVAL;

		if (unlikely(addr <= mas->index))
			return -EINVAL;
	}

	if (prev && next) {
		u64 p_last = prev->va.addr + prev->va.range - 1;
		u64 n_addr = next->va.addr;

		if (unlikely(p_last > n_addr))
			return -EINVAL;

		if (unlikely(n_addr - p_last <= 1))
			return -EINVAL;
	}

	mas_lock(mas);
	if (prev) {
		mas_store(mas, prev);
		mas_next(mas, max);
		if (!next)
			mas_store(mas, NULL);
	}

	if (next) {
		mas->last = next->va.addr - 1;
		mas_store(mas, NULL);
		mas_next(mas, max);
		mas_store(mas, next);
	}
	mas_unlock(mas);

	return 0;
}
EXPORT_SYMBOL(drm_gpuva_remap);

/**
 * drm_gpuva_unmap - helper to remove a &drm_gpuva from &drm_gpuva_fn_ops
 * callbacks
 *
 * @state: the current &drm_gpuva_state
 *
 * The entry associated with the current state is removed.
 */
void
drm_gpuva_unmap(drm_gpuva_state_t state)
{
	drm_gpuva_iter_remove(state);
}
EXPORT_SYMBOL(drm_gpuva_unmap);

static int
op_map_cb(struct drm_gpuva_fn_ops *fn, void *priv,
	  u64 addr, u64 range,
	  struct drm_gem_object *obj, u64 offset)
{
	struct drm_gpuva_op op = {};

	op.op = DRM_GPUVA_OP_MAP;
	op.map.va.addr = addr;
	op.map.va.range = range;
	op.map.gem.obj = obj;
	op.map.gem.offset = offset;

	return fn->sm_step_map(&op, priv);
}

static int
op_remap_cb(struct drm_gpuva_fn_ops *fn,
	    drm_gpuva_state_t state, void *priv,
	    struct drm_gpuva_op_map *prev,
	    struct drm_gpuva_op_map *next,
	    struct drm_gpuva_op_unmap *unmap)
{
	struct drm_gpuva_op op = {};
	struct drm_gpuva_op_remap *r;

	op.op = DRM_GPUVA_OP_REMAP;
	r = &op.remap;
	r->prev = prev;
	r->next = next;
	r->unmap = unmap;

	return fn->sm_step_remap(&op, state, priv);
}

static int
op_unmap_cb(struct drm_gpuva_fn_ops *fn,
	    drm_gpuva_state_t state, void *priv,
	    struct drm_gpuva *va, bool merge)
{
	struct drm_gpuva_op op = {};

	op.op = DRM_GPUVA_OP_UNMAP;
	op.unmap.va = va;
	op.unmap.keep = merge;

	return fn->sm_step_unmap(&op, state, priv);
}

static int
__drm_gpuva_sm_map(struct drm_gpuva_manager *mgr,
		   struct drm_gpuva_fn_ops *ops, void *priv,
		   u64 req_addr, u64 req_range,
		   struct drm_gem_object *req_obj, u64 req_offset)
{
	DRM_GPUVA_ITER(it, mgr, req_addr);
	struct drm_gpuva *va, *prev = NULL;
	u64 req_end = req_addr + req_range;
	int ret;

	if (unlikely(!drm_gpuva_in_mm_range(mgr, req_addr, req_range)))
		return -EINVAL;

	if (unlikely(drm_gpuva_in_kernel_node(mgr, req_addr, req_range)))
		return -EINVAL;

	drm_gpuva_iter_for_each_range(va, it, req_end) {
		struct drm_gem_object *obj = va->gem.obj;
		u64 offset = va->gem.offset;
		u64 addr = va->va.addr;
		u64 range = va->va.range;
		u64 end = addr + range;
		bool merge = !!va->gem.obj;

		if (addr == req_addr) {
			merge &= obj == req_obj &&
				 offset == req_offset;

			if (end == req_end) {
				ret = op_unmap_cb(ops, &it, priv, va, merge);
				if (ret)
					return ret;
				break;
			}

			if (end < req_end) {
				ret = op_unmap_cb(ops, &it, priv, va, merge);
				if (ret)
					return ret;
				goto next;
			}

			if (end > req_end) {
				struct drm_gpuva_op_map n = {
					.va.addr = req_end,
					.va.range = range - req_range,
					.gem.obj = obj,
					.gem.offset = offset + req_range,
				};
				struct drm_gpuva_op_unmap u = {
					.va = va,
					.keep = merge,
				};

				ret = op_remap_cb(ops, &it, priv, NULL, &n, &u);
				if (ret)
					return ret;
				break;
			}
		} else if (addr < req_addr) {
			u64 ls_range = req_addr - addr;
			struct drm_gpuva_op_map p = {
				.va.addr = addr,
				.va.range = ls_range,
				.gem.obj = obj,
				.gem.offset = offset,
			};
			struct drm_gpuva_op_unmap u = { .va = va };

			merge &= obj == req_obj &&
				 offset + ls_range == req_offset;
			u.keep = merge;

			if (end == req_end) {
				ret = op_remap_cb(ops, &it, priv, &p, NULL, &u);
				if (ret)
					return ret;
				break;
			}

			if (end < req_end) {
				ret = op_remap_cb(ops, &it, priv, &p, NULL, &u);
				if (ret)
					return ret;
				goto next;
			}

			if (end > req_end) {
				struct drm_gpuva_op_map n = {
					.va.addr = req_end,
					.va.range = end - req_end,
					.gem.obj = obj,
					.gem.offset = offset + ls_range +
						      req_range,
				};

				ret = op_remap_cb(ops, &it, priv, &p, &n, &u);
				if (ret)
					return ret;
				break;
			}
		} else if (addr > req_addr) {
			merge &= obj == req_obj &&
				 offset == req_offset +
					   (addr - req_addr);

			if (end == req_end) {
				ret = op_unmap_cb(ops, &it, priv, va, merge);
				if (ret)
					return ret;
				break;
			}

			if (end < req_end) {
				ret = op_unmap_cb(ops, &it, priv, va, merge);
				if (ret)
					return ret;
				goto next;
			}

			if (end > req_end) {
				struct drm_gpuva_op_map n = {
					.va.addr = req_end,
					.va.range = end - req_end,
					.gem.obj = obj,
					.gem.offset = offset + req_end - addr,
				};
				struct drm_gpuva_op_unmap u = {
					.va = va,
					.keep = merge,
				};

				ret = op_remap_cb(ops, &it, priv, NULL, &n, &u);
				if (ret)
					return ret;
				break;
			}
		}
next:
		prev = va;
	}

	return op_map_cb(ops, priv,
			 req_addr, req_range,
			 req_obj, req_offset);
}

static int
__drm_gpuva_sm_unmap(struct drm_gpuva_manager *mgr,
		     struct drm_gpuva_fn_ops *ops, void *priv,
		     u64 req_addr, u64 req_range)
{
	DRM_GPUVA_ITER(it, mgr, req_addr);
	struct drm_gpuva *va;
	u64 req_end = req_addr + req_range;
	int ret;

	if (unlikely(drm_gpuva_in_kernel_node(mgr, req_addr, req_range)))
		return -EINVAL;

	drm_gpuva_iter_for_each_range(va, it, req_end) {
		struct drm_gpuva_op_map prev = {}, next = {};
		bool prev_split = false, next_split = false;
		struct drm_gem_object *obj = va->gem.obj;
		u64 offset = va->gem.offset;
		u64 addr = va->va.addr;
		u64 range = va->va.range;
		u64 end = addr + range;

		if (addr < req_addr) {
			prev.va.addr = addr;
			prev.va.range = req_addr - addr;
			prev.gem.obj = obj;
			prev.gem.offset = offset;

			prev_split = true;
		}

		if (end > req_end) {
			next.va.addr = req_end;
			next.va.range = end - req_end;
			next.gem.obj = obj;
			next.gem.offset = offset + (req_end - addr);

			next_split = true;
		}

		if (prev_split || next_split) {
			struct drm_gpuva_op_unmap unmap = { .va = va };

			ret = op_remap_cb(ops, &it, priv,
					  prev_split ? &prev : NULL,
					  next_split ? &next : NULL,
					  &unmap);
			if (ret)
				return ret;
		} else {
			ret = op_unmap_cb(ops, &it, priv, va, false);
			if (ret)
				return ret;
		}
	}

	return 0;
}

/**
 * drm_gpuva_sm_map - creates the &drm_gpuva_op split/merge steps
 * @mgr: the &drm_gpuva_manager representing the GPU VA space
 * @req_addr: the start address of the new mapping
 * @req_range: the range of the new mapping
 * @req_obj: the &drm_gem_object to map
 * @req_offset: the offset within the &drm_gem_object
 * @priv: pointer to a driver private data structure
 *
 * This function iterates the given range of the GPU VA space. It utilizes the
 * &drm_gpuva_fn_ops to call back into the driver providing the split and merge
 * steps.
 *
 * Drivers may use these callbacks to update the GPU VA space right away within
 * the callback. In case the driver decides to copy and store the operations for
 * later processing neither this function nor &drm_gpuva_sm_unmap is allowed to
 * be called before the &drm_gpuva_manager's view of the GPU VA space was
 * updated with the previous set of operations. To update the
 * &drm_gpuva_manager's view of the GPU VA space drm_gpuva_insert(),
 * drm_gpuva_destroy_locked() and/or drm_gpuva_destroy_unlocked() should be
 * used.
 *
 * A sequence of callbacks can contain map, unmap and remap operations, but
 * the sequence of callbacks might also be empty if no operation is required,
 * e.g. if the requested mapping already exists in the exact same way.
 *
 * There can be an arbitrary amount of unmap operations, a maximum of two remap
 * operations and a single map operation. The latter one represents the original
 * map operation requested by the caller.
 *
 * Returns: 0 on success or a negative error code
 */
int
drm_gpuva_sm_map(struct drm_gpuva_manager *mgr, void *priv,
		 u64 req_addr, u64 req_range,
		 struct drm_gem_object *req_obj, u64 req_offset)
{
	struct drm_gpuva_fn_ops *ops = mgr->ops;

	if (unlikely(!(ops && ops->sm_step_map &&
		       ops->sm_step_remap &&
		       ops->sm_step_unmap)))
		return -EINVAL;

	return __drm_gpuva_sm_map(mgr, ops, priv,
				  req_addr, req_range,
				  req_obj, req_offset);
}
EXPORT_SYMBOL(drm_gpuva_sm_map);

/**
 * drm_gpuva_sm_unmap - creates the &drm_gpuva_ops to split on unmap
 * @mgr: the &drm_gpuva_manager representing the GPU VA space
 * @priv: pointer to a driver private data structure
 * @req_addr: the start address of the range to unmap
 * @req_range: the range of the mappings to unmap
 *
 * This function iterates the given range of the GPU VA space. It utilizes the
 * &drm_gpuva_fn_ops to call back into the driver providing the operations to
 * unmap and, if required, split existent mappings.
 *
 * Drivers may use these callbacks to update the GPU VA space right away within
 * the callback. In case the driver decides to copy and store the operations for
 * later processing neither this function nor &drm_gpuva_sm_map is allowed to be
 * called before the &drm_gpuva_manager's view of the GPU VA space was updated
 * with the previous set of operations. To update the &drm_gpuva_manager's view
 * of the GPU VA space drm_gpuva_insert(), drm_gpuva_destroy_locked() and/or
 * drm_gpuva_destroy_unlocked() should be used.
 *
 * A sequence of callbacks can contain unmap and remap operations, depending on
 * whether there are actual overlapping mappings to split.
 *
 * There can be an arbitrary amount of unmap operations and a maximum of two
 * remap operations.
 *
 * Returns: 0 on success or a negative error code
 */
int
drm_gpuva_sm_unmap(struct drm_gpuva_manager *mgr, void *priv,
		   u64 req_addr, u64 req_range)
{
	struct drm_gpuva_fn_ops *ops = mgr->ops;

	if (unlikely(!(ops && ops->sm_step_remap &&
		       ops->sm_step_unmap)))
		return -EINVAL;

	return __drm_gpuva_sm_unmap(mgr, ops, priv,
				    req_addr, req_range);
}
EXPORT_SYMBOL(drm_gpuva_sm_unmap);

static struct drm_gpuva_op *
gpuva_op_alloc(struct drm_gpuva_manager *mgr)
{
	struct drm_gpuva_fn_ops *fn = mgr->ops;
	struct drm_gpuva_op *op;

	if (fn && fn->op_alloc)
		op = fn->op_alloc();
	else
		op = kzalloc(sizeof(*op), GFP_KERNEL);

	if (unlikely(!op))
		return NULL;

	return op;
}

static void
gpuva_op_free(struct drm_gpuva_manager *mgr,
	      struct drm_gpuva_op *op)
{
	struct drm_gpuva_fn_ops *fn = mgr->ops;

	if (fn && fn->op_free)
		fn->op_free(op);
	else
		kfree(op);
}

static int
drm_gpuva_sm_step(struct drm_gpuva_op *__op,
		  drm_gpuva_state_t state,
		  void *priv)
{
	struct {
		struct drm_gpuva_manager *mgr;
		struct drm_gpuva_ops *ops;
	} *args = priv;
	struct drm_gpuva_manager *mgr = args->mgr;
	struct drm_gpuva_ops *ops = args->ops;
	struct drm_gpuva_op *op;

	op = gpuva_op_alloc(mgr);
	if (unlikely(!op))
		goto err;

	memcpy(op, __op, sizeof(*op));

	if (op->op == DRM_GPUVA_OP_REMAP) {
		struct drm_gpuva_op_remap *__r = &__op->remap;
		struct drm_gpuva_op_remap *r = &op->remap;

		r->unmap = kmemdup(__r->unmap, sizeof(*r->unmap),
				   GFP_KERNEL);
		if (unlikely(!r->unmap))
			goto err_free_op;

		if (__r->prev) {
			r->prev = kmemdup(__r->prev, sizeof(*r->prev),
					  GFP_KERNEL);
			if (unlikely(!r->prev))
				goto err_free_unmap;
		}

		if (__r->next) {
			r->next = kmemdup(__r->next, sizeof(*r->next),
					  GFP_KERNEL);
			if (unlikely(!r->next))
				goto err_free_prev;
		}
	}

	list_add_tail(&op->entry, &ops->list);

	return 0;

err_free_unmap:
	kfree(op->remap.unmap);
err_free_prev:
	kfree(op->remap.prev);
err_free_op:
	gpuva_op_free(mgr, op);
err:
	return -ENOMEM;
}

static int
drm_gpuva_sm_step_map(struct drm_gpuva_op *__op, void *priv)
{
	return drm_gpuva_sm_step(__op, NULL, priv);
}

static struct drm_gpuva_fn_ops gpuva_list_ops = {
	.sm_step_map = drm_gpuva_sm_step_map,
	.sm_step_remap = drm_gpuva_sm_step,
	.sm_step_unmap = drm_gpuva_sm_step,
};

/**
 * drm_gpuva_sm_map_ops_create - creates the &drm_gpuva_ops to split and merge
 * @mgr: the &drm_gpuva_manager representing the GPU VA space
 * @req_addr: the start address of the new mapping
 * @req_range: the range of the new mapping
 * @req_obj: the &drm_gem_object to map
 * @req_offset: the offset within the &drm_gem_object
 *
 * This function creates a list of operations to perform splitting and merging
 * of existent mapping(s) with the newly requested one.
 *
 * The list can be iterated with &drm_gpuva_for_each_op and must be processed
 * in the given order. It can contain map, unmap and remap operations, but it
 * also can be empty if no operation is required, e.g. if the requested mapping
 * already exists is the exact same way.
 *
 * There can be an arbitrary amount of unmap operations, a maximum of two remap
 * operations and a single map operation. The latter one represents the original
 * map operation requested by the caller.
 *
 * Note that before calling this function again with another mapping request it
 * is necessary to update the &drm_gpuva_manager's view of the GPU VA space. The
 * previously obtained operations must be either processed or abandoned. To
 * update the &drm_gpuva_manager's view of the GPU VA space drm_gpuva_insert(),
 * drm_gpuva_destroy_locked() and/or drm_gpuva_destroy_unlocked() should be
 * used.
 *
 * After the caller finished processing the returned &drm_gpuva_ops, they must
 * be freed with &drm_gpuva_ops_free.
 *
 * Returns: a pointer to the &drm_gpuva_ops on success, an ERR_PTR on failure
 */
struct drm_gpuva_ops *
drm_gpuva_sm_map_ops_create(struct drm_gpuva_manager *mgr,
			    u64 req_addr, u64 req_range,
			    struct drm_gem_object *req_obj, u64 req_offset)
{
	struct drm_gpuva_ops *ops;
	struct {
		struct drm_gpuva_manager *mgr;
		struct drm_gpuva_ops *ops;
	} args;
	int ret;

	ops = kzalloc(sizeof(*ops), GFP_KERNEL);
	if (unlikely(!ops))
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&ops->list);

	args.mgr = mgr;
	args.ops = ops;

	ret = __drm_gpuva_sm_map(mgr, &gpuva_list_ops, &args,
				 req_addr, req_range,
				 req_obj, req_offset);
	if (ret)
		goto err_free_ops;

	return ops;

err_free_ops:
	drm_gpuva_ops_free(mgr, ops);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(drm_gpuva_sm_map_ops_create);

/**
 * drm_gpuva_sm_unmap_ops_create - creates the &drm_gpuva_ops to split on unmap
 * @mgr: the &drm_gpuva_manager representing the GPU VA space
 * @req_addr: the start address of the range to unmap
 * @req_range: the range of the mappings to unmap
 *
 * This function creates a list of operations to perform unmapping and, if
 * required, splitting of the mappings overlapping the unmap range.
 *
 * The list can be iterated with &drm_gpuva_for_each_op and must be processed
 * in the given order. It can contain unmap and remap operations, depending on
 * whether there are actual overlapping mappings to split.
 *
 * There can be an arbitrary amount of unmap operations and a maximum of two
 * remap operations.
 *
 * Note that before calling this function again with another range to unmap it
 * is necessary to update the &drm_gpuva_manager's view of the GPU VA space. The
 * previously obtained operations must be processed or abandoned. To update the
 * &drm_gpuva_manager's view of the GPU VA space drm_gpuva_insert(),
 * drm_gpuva_destroy_locked() and/or drm_gpuva_destroy_unlocked() should be
 * used.
 *
 * After the caller finished processing the returned &drm_gpuva_ops, they must
 * be freed with &drm_gpuva_ops_free.
 *
 * Returns: a pointer to the &drm_gpuva_ops on success, an ERR_PTR on failure
 */
struct drm_gpuva_ops *
drm_gpuva_sm_unmap_ops_create(struct drm_gpuva_manager *mgr,
			      u64 req_addr, u64 req_range)
{
	struct drm_gpuva_ops *ops;
	struct {
		struct drm_gpuva_manager *mgr;
		struct drm_gpuva_ops *ops;
	} args;
	int ret;

	ops = kzalloc(sizeof(*ops), GFP_KERNEL);
	if (unlikely(!ops))
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&ops->list);

	args.mgr = mgr;
	args.ops = ops;

	ret = __drm_gpuva_sm_unmap(mgr, &gpuva_list_ops, &args,
				   req_addr, req_range);
	if (ret)
		goto err_free_ops;

	return ops;

err_free_ops:
	drm_gpuva_ops_free(mgr, ops);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(drm_gpuva_sm_unmap_ops_create);

/**
 * drm_gpuva_prefetch_ops_create - creates the &drm_gpuva_ops to prefetch
 * @mgr: the &drm_gpuva_manager representing the GPU VA space
 * @addr: the start address of the range to prefetch
 * @range: the range of the mappings to prefetch
 *
 * This function creates a list of operations to perform prefetching.
 *
 * The list can be iterated with &drm_gpuva_for_each_op and must be processed
 * in the given order. It can contain prefetch operations.
 *
 * There can be an arbitrary amount of prefetch operations.
 *
 * After the caller finished processing the returned &drm_gpuva_ops, they must
 * be freed with &drm_gpuva_ops_free.
 *
 * Returns: a pointer to the &drm_gpuva_ops on success, an ERR_PTR on failure
 */
struct drm_gpuva_ops *
drm_gpuva_prefetch_ops_create(struct drm_gpuva_manager *mgr,
			      u64 addr, u64 range)
{
	DRM_GPUVA_ITER(it, mgr, addr);
	struct drm_gpuva_ops *ops;
	struct drm_gpuva_op *op;
	struct drm_gpuva *va;
	int ret;

	ops = kzalloc(sizeof(*ops), GFP_KERNEL);
	if (!ops)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&ops->list);

	drm_gpuva_iter_for_each_range(va, it, addr + range) {
		op = gpuva_op_alloc(mgr);
		if (!op) {
			ret = -ENOMEM;
			goto err_free_ops;
		}

		op->op = DRM_GPUVA_OP_PREFETCH;
		op->prefetch.va = va;
		list_add_tail(&op->entry, &ops->list);
	}

	return ops;

err_free_ops:
	drm_gpuva_ops_free(mgr, ops);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(drm_gpuva_prefetch_ops_create);

/**
 * drm_gpuva_gem_unmap_ops_create - creates the &drm_gpuva_ops to unmap a GEM
 * @mgr: the &drm_gpuva_manager representing the GPU VA space
 * @obj: the &drm_gem_object to unmap
 *
 * This function creates a list of operations to perform unmapping for every
 * GPUVA attached to a GEM.
 *
 * The list can be iterated with &drm_gpuva_for_each_op and consists out of an
 * arbitrary amount of unmap operations.
 *
 * After the caller finished processing the returned &drm_gpuva_ops, they must
 * be freed with &drm_gpuva_ops_free.
 *
 * It is the callers responsibility to protect the GEMs GPUVA list against
 * concurrent access.
 *
 * Returns: a pointer to the &drm_gpuva_ops on success, an ERR_PTR on failure
 */
struct drm_gpuva_ops *
drm_gpuva_gem_unmap_ops_create(struct drm_gpuva_manager *mgr,
			       struct drm_gem_object *obj)
{
	struct drm_gpuva_ops *ops;
	struct drm_gpuva_op *op;
	struct drm_gpuva *va;
	int ret;

	ops = kzalloc(sizeof(*ops), GFP_KERNEL);
	if (!ops)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&ops->list);

	drm_gem_for_each_gpuva(va, obj) {
		op = gpuva_op_alloc(mgr);
		if (!op) {
			ret = -ENOMEM;
			goto err_free_ops;
		}

		op->op = DRM_GPUVA_OP_UNMAP;
		op->unmap.va = va;
		list_add_tail(&op->entry, &ops->list);
	}

	return ops;

err_free_ops:
	drm_gpuva_ops_free(mgr, ops);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(drm_gpuva_gem_unmap_ops_create);


/**
 * drm_gpuva_ops_free - free the given &drm_gpuva_ops
 * @mgr: the &drm_gpuva_manager the ops were created for
 * @ops: the &drm_gpuva_ops to free
 *
 * Frees the given &drm_gpuva_ops structure including all the ops associated
 * with it.
 */
void
drm_gpuva_ops_free(struct drm_gpuva_manager *mgr,
		   struct drm_gpuva_ops *ops)
{
	struct drm_gpuva_op *op, *next;

	drm_gpuva_for_each_op_safe(op, next, ops) {
		list_del(&op->entry);

		if (op->op == DRM_GPUVA_OP_REMAP) {
			kfree(op->remap.prev);
			kfree(op->remap.next);
			kfree(op->remap.unmap);
		}

		gpuva_op_free(mgr, op);
	}

	kfree(ops);
}
EXPORT_SYMBOL(drm_gpuva_ops_free);
