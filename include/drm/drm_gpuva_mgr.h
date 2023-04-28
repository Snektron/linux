/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __DRM_GPUVA_MGR_H__
#define __DRM_GPUVA_MGR_H__

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
 */

#include <linux/maple_tree.h>
#include <linux/mm.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct drm_gpuva_manager;
struct drm_gpuva_fn_ops;
struct drm_gpuva_prealloc;

/**
 * enum drm_gpuva_flags - flags for struct drm_gpuva
 */
enum drm_gpuva_flags {
	/**
	 * @DRM_GPUVA_EVICTED:
	 *
	 * Flag indicating that the &drm_gpuva's backing GEM is evicted.
	 */
	DRM_GPUVA_EVICTED = (1 << 0),

	/**
	 * @DRM_GPUVA_SPARSE:
	 *
	 * Flag indicating that the &drm_gpuva is a sparse mapping.
	 */
	DRM_GPUVA_SPARSE = (1 << 1),

	/**
	 * @DRM_GPUVA_USERBITS: user defined bits
	 */
	DRM_GPUVA_USERBITS = (1 << 2),
};

/**
 * struct drm_gpuva - structure to track a GPU VA mapping
 *
 * This structure represents a GPU VA mapping and is associated with a
 * &drm_gpuva_manager.
 *
 * Typically, this structure is embedded in bigger driver structures.
 */
struct drm_gpuva {
	/**
	 * @mgr: the &drm_gpuva_manager this object is associated with
	 */
	struct drm_gpuva_manager *mgr;

	/**
	 * @flags: the &drm_gpuva_flags for this mapping
	 */
	enum drm_gpuva_flags flags;

	/**
	 * @va: structure containing the address and range of the &drm_gpuva
	 */
	struct {
		/**
		 * @addr: the start address
		 */
		u64 addr;

		/*
		 * @range: the range
		 */
		u64 range;
	} va;

	/**
	 * @gem: structure containing the &drm_gem_object and it's offset
	 */
	struct {
		/**
		 * @offset: the offset within the &drm_gem_object
		 */
		u64 offset;

		/**
		 * @obj: the mapped &drm_gem_object
		 */
		struct drm_gem_object *obj;

		/**
		 * @entry: the &list_head to attach this object to a &drm_gem_object
		 */
		struct list_head entry;
	} gem;
};

void drm_gpuva_link(struct drm_gpuva *va);
void drm_gpuva_unlink(struct drm_gpuva *va);

int drm_gpuva_insert(struct drm_gpuva_manager *mgr,
		     struct drm_gpuva *va);
int drm_gpuva_insert_prealloc(struct drm_gpuva_manager *mgr,
			      struct drm_gpuva_prealloc *pa,
			      struct drm_gpuva *va);
void drm_gpuva_remove(struct drm_gpuva *va);

struct drm_gpuva *drm_gpuva_find(struct drm_gpuva_manager *mgr,
				 u64 addr, u64 range);
struct drm_gpuva *drm_gpuva_find_first(struct drm_gpuva_manager *mgr,
				       u64 addr, u64 range);
struct drm_gpuva *drm_gpuva_find_prev(struct drm_gpuva_manager *mgr, u64 start);
struct drm_gpuva *drm_gpuva_find_next(struct drm_gpuva_manager *mgr, u64 end);

bool drm_gpuva_interval_empty(struct drm_gpuva_manager *mgr, u64 addr, u64 range);

/**
 * drm_gpuva_evict - sets whether the backing GEM of this &drm_gpuva is evicted
 * @va: the &drm_gpuva to set the evict flag for
 * @evict: indicates whether the &drm_gpuva is evicted
 */
static inline void drm_gpuva_evict(struct drm_gpuva *va, bool evict)
{
	if (evict)
		va->flags |= DRM_GPUVA_EVICTED;
	else
		va->flags &= ~DRM_GPUVA_EVICTED;
}

/**
 * drm_gpuva_evicted - indicates whether the backing BO of this &drm_gpuva
 * is evicted
 * @va: the &drm_gpuva to check
 */
static inline bool drm_gpuva_evicted(struct drm_gpuva *va)
{
	return va->flags & DRM_GPUVA_EVICTED;
}

/**
 * struct drm_gpuva_manager - DRM GPU VA Manager
 *
 * The DRM GPU VA Manager keeps track of a GPU's virtual address space by using
 * &maple_tree structures. Typically, this structure is embedded in bigger
 * driver structures.
 *
 * Drivers can pass addresses and ranges in an arbitrary unit, e.g. bytes or
 * pages.
 *
 * There should be one manager instance per GPU virtual address space.
 */
struct drm_gpuva_manager {
	/**
	 * @name: the name of the DRM GPU VA space
	 */
	const char *name;

	/**
	 * @mm_start: start of the VA space
	 */
	u64 mm_start;

	/**
	 * @mm_range: length of the VA space
	 */
	u64 mm_range;

	/**
	 * @mtree: the &maple_tree to track GPU VA mappings
	 */
	struct maple_tree mtree;

	/**
	 * @kernel_alloc_node:
	 *
	 * &drm_gpuva representing the address space cutout reserved for
	 * the kernel
	 */
	struct drm_gpuva kernel_alloc_node;

	/**
	 * @ops: &drm_gpuva_fn_ops providing the split/merge steps to drivers
	 */
	const struct drm_gpuva_fn_ops *ops;
};

void drm_gpuva_manager_init(struct drm_gpuva_manager *mgr,
			    const char *name,
			    u64 start_offset, u64 range,
			    u64 reserve_offset, u64 reserve_range,
			    const struct drm_gpuva_fn_ops *ops);
void drm_gpuva_manager_destroy(struct drm_gpuva_manager *mgr);

/**
 * struct drm_gpuva_prealloc - holds a preallocated node for the
 * &drm_gpuva_manager to insert a single new entry
 */
struct drm_gpuva_prealloc {
	/**
	 * @mas: the maple tree advanced state
	 */
	struct ma_state mas;
};

struct drm_gpuva_prealloc * drm_gpuva_prealloc_create(struct drm_gpuva_manager *mgr);
void drm_gpuva_prealloc_destroy(struct drm_gpuva_prealloc *pa);

/**
 * struct drm_gpuva_iterator - iterator for walking the internal (maple) tree
 */
struct drm_gpuva_iterator {
	/**
	 * @mas: the maple tree advanced state
	 */
	struct ma_state mas;

	/**
	 * @mgr: the &drm_gpuva_manager to iterate
	 */
	struct drm_gpuva_manager *mgr;
};
typedef struct drm_gpuva_iterator * drm_gpuva_state_t;

void drm_gpuva_iter_remove(struct drm_gpuva_iterator *it);
int drm_gpuva_iter_va_replace(struct drm_gpuva_iterator *it,
			      struct drm_gpuva *va);

static inline struct drm_gpuva *
drm_gpuva_iter_find(struct drm_gpuva_iterator *it, unsigned long max)
{
	struct drm_gpuva *va;

	mas_lock(&it->mas);
	va = mas_find(&it->mas, max);
	mas_unlock(&it->mas);

	return va;
}

/**
 * DRM_GPUVA_ITER - create an iterator structure to iterate the &drm_gpuva tree
 * @name: the name of the &drm_gpuva_iterator to create
 * @mgr__: the &drm_gpuva_manager to iterate
 * @start: starting offset, the first entry will overlap this
 */
#define DRM_GPUVA_ITER(name, mgr__, start)				\
	struct drm_gpuva_iterator name = {				\
		.mas = MA_STATE_INIT(&(mgr__)->mtree, start, 0),	\
		.mgr = mgr__,						\
	}

/**
 * drm_gpuva_iter_for_each_range - iternator to walk over a range of entries
 * @va__: the &drm_gpuva found for the current iteration
 * @it__: &drm_gpuva_iterator structure to assign to in each iteration step
 * @end__: ending offset, the last entry will start before this (but may overlap)
 *
 * This function can be used to iterate &drm_gpuva objects.
 *
 * It is safe against the removal of elements using &drm_gpuva_iter_remove,
 * however it is not safe against the removal of elements using
 * &drm_gpuva_remove.
 */
#define drm_gpuva_iter_for_each_range(va__, it__, end__) \
	while (((va__) = drm_gpuva_iter_find(&(it__), (end__) - 1)))

/**
 * drm_gpuva_iter_for_each - iternator to walk over all existing entries
 * @va__: the &drm_gpuva found for the current iteration
 * @it__: &drm_gpuva_iterator structure to assign to in each iteration step
 *
 * This function can be used to iterate &drm_gpuva objects.
 *
 * In order to walk over all potentially existing entries, the
 * &drm_gpuva_iterator must be initialized to start at
 * &drm_gpuva_manager->mm_start or simply 0.
 *
 * It is safe against the removal of elements using &drm_gpuva_iter_remove,
 * however it is not safe against the removal of elements using
 * &drm_gpuva_remove.
 */
#define drm_gpuva_iter_for_each(va__, it__) \
	drm_gpuva_iter_for_each_range(va__, it__, (it__).mgr->mm_start + (it__).mgr->mm_range)

/**
 * enum drm_gpuva_op_type - GPU VA operation type
 *
 * Operations to alter the GPU VA mappings tracked by the &drm_gpuva_manager.
 */
enum drm_gpuva_op_type {
	/**
	 * @DRM_GPUVA_OP_MAP: the map op type
	 */
	DRM_GPUVA_OP_MAP,

	/**
	 * @DRM_GPUVA_OP_REMAP: the remap op type
	 */
	DRM_GPUVA_OP_REMAP,

	/**
	 * @DRM_GPUVA_OP_UNMAP: the unmap op type
	 */
	DRM_GPUVA_OP_UNMAP,

	/**
	 * @DRM_GPUVA_OP_PREFETCH: the prefetch op type
	 */
	DRM_GPUVA_OP_PREFETCH,
};

/**
 * struct drm_gpuva_op_map - GPU VA map operation
 *
 * This structure represents a single map operation generated by the
 * DRM GPU VA manager.
 */
struct drm_gpuva_op_map {
	/**
	 * @va: structure containing address and range of a map
	 * operation
	 */
	struct {
		/**
		 * @addr: the base address of the new mapping
		 */
		u64 addr;

		/**
		 * @range: the range of the new mapping
		 */
		u64 range;
	} va;

	/**
	 * @gem: structure containing the &drm_gem_object and it's offset
	 */
	struct {
		/**
		 * @offset: the offset within the &drm_gem_object
		 */
		u64 offset;

		/**
		 * @obj: the &drm_gem_object to map
		 */
		struct drm_gem_object *obj;
	} gem;
};

/**
 * struct drm_gpuva_op_unmap - GPU VA unmap operation
 *
 * This structure represents a single unmap operation generated by the
 * DRM GPU VA manager.
 */
struct drm_gpuva_op_unmap {
	/**
	 * @va: the &drm_gpuva to unmap
	 */
	struct drm_gpuva *va;

	/**
	 * @keep:
	 *
	 * Indicates whether this &drm_gpuva is physically contiguous with the
	 * original mapping request.
	 *
	 * Optionally, if &keep is set, drivers may keep the actual page table
	 * mappings for this &drm_gpuva, adding the missing page table entries
	 * only and update the &drm_gpuva_manager accordingly.
	 */
	bool keep;
};

/**
 * struct drm_gpuva_op_remap - GPU VA remap operation
 *
 * This represents a single remap operation generated by the DRM GPU VA manager.
 *
 * A remap operation is generated when an existing GPU VA mmapping is split up
 * by inserting a new GPU VA mapping or by partially unmapping existent
 * mapping(s), hence it consists of a maximum of two map and one unmap
 * operation.
 *
 * The @unmap operation takes care of removing the original existing mapping.
 * @prev is used to remap the preceding part, @next the subsequent part.
 *
 * If either a new mapping's start address is aligned with the start address
 * of the old mapping or the new mapping's end address is aligned with the
 * end address of the old mapping, either @prev or @next is NULL.
 *
 * Note, the reason for a dedicated remap operation, rather than arbitrary
 * unmap and map operations, is to give drivers the chance of extracting driver
 * specific data for creating the new mappings from the unmap operations's
 * &drm_gpuva structure which typically is embedded in larger driver specific
 * structures.
 */
struct drm_gpuva_op_remap {
	/**
	 * @prev: the preceding part of a split mapping
	 */
	struct drm_gpuva_op_map *prev;

	/**
	 * @next: the subsequent part of a split mapping
	 */
	struct drm_gpuva_op_map *next;

	/**
	 * @unmap: the unmap operation for the original existing mapping
	 */
	struct drm_gpuva_op_unmap *unmap;
};

/**
 * struct drm_gpuva_op_prefetch - GPU VA prefetch operation
 *
 * This structure represents a single prefetch operation generated by the
 * DRM GPU VA manager.
 */
struct drm_gpuva_op_prefetch {
	/**
	 * @va: the &drm_gpuva to prefetch
	 */
	struct drm_gpuva *va;
};

/**
 * struct drm_gpuva_op - GPU VA operation
 *
 * This structure represents a single generic operation.
 *
 * The particular type of the operation is defined by @op.
 */
struct drm_gpuva_op {
	/**
	 * @entry:
	 *
	 * The &list_head used to distribute instances of this struct within
	 * &drm_gpuva_ops.
	 */
	struct list_head entry;

	/**
	 * @op: the type of the operation
	 */
	enum drm_gpuva_op_type op;

	union {
		/**
		 * @map: the map operation
		 */
		struct drm_gpuva_op_map map;

		/**
		 * @remap: the remap operation
		 */
		struct drm_gpuva_op_remap remap;

		/**
		 * @unmap: the unmap operation
		 */
		struct drm_gpuva_op_unmap unmap;

		/**
		 * @prefetch: the prefetch operation
		 */
		struct drm_gpuva_op_prefetch prefetch;
	};
};

/**
 * struct drm_gpuva_ops - wraps a list of &drm_gpuva_op
 */
struct drm_gpuva_ops {
	/**
	 * @list: the &list_head
	 */
	struct list_head list;
};

/**
 * drm_gpuva_for_each_op - iterator to walk over &drm_gpuva_ops
 * @op: &drm_gpuva_op to assign in each iteration step
 * @ops: &drm_gpuva_ops to walk
 *
 * This iterator walks over all ops within a given list of operations.
 */
#define drm_gpuva_for_each_op(op, ops) list_for_each_entry(op, &(ops)->list, entry)

/**
 * drm_gpuva_for_each_op_safe - iterator to safely walk over &drm_gpuva_ops
 * @op: &drm_gpuva_op to assign in each iteration step
 * @next: &next &drm_gpuva_op to store the next step
 * @ops: &drm_gpuva_ops to walk
 *
 * This iterator walks over all ops within a given list of operations. It is
 * implemented with list_for_each_safe(), so save against removal of elements.
 */
#define drm_gpuva_for_each_op_safe(op, next, ops) \
	list_for_each_entry_safe(op, next, &(ops)->list, entry)

/**
 * drm_gpuva_for_each_op_from_reverse - iterate backwards from the given point
 * @op: &drm_gpuva_op to assign in each iteration step
 * @ops: &drm_gpuva_ops to walk
 *
 * This iterator walks over all ops within a given list of operations beginning
 * from the given operation in reverse order.
 */
#define drm_gpuva_for_each_op_from_reverse(op, ops) \
	list_for_each_entry_from_reverse(op, &(ops)->list, entry)

/**
 * drm_gpuva_first_op - returns the first &drm_gpuva_op from &drm_gpuva_ops
 * @ops: the &drm_gpuva_ops to get the fist &drm_gpuva_op from
 */
#define drm_gpuva_first_op(ops) \
	list_first_entry(&(ops)->list, struct drm_gpuva_op, entry)

/**
 * drm_gpuva_last_op - returns the last &drm_gpuva_op from &drm_gpuva_ops
 * @ops: the &drm_gpuva_ops to get the last &drm_gpuva_op from
 */
#define drm_gpuva_last_op(ops) \
	list_last_entry(&(ops)->list, struct drm_gpuva_op, entry)

/**
 * drm_gpuva_prev_op - previous &drm_gpuva_op in the list
 * @op: the current &drm_gpuva_op
 */
#define drm_gpuva_prev_op(op) list_prev_entry(op, entry)

/**
 * drm_gpuva_next_op - next &drm_gpuva_op in the list
 * @op: the current &drm_gpuva_op
 */
#define drm_gpuva_next_op(op) list_next_entry(op, entry)

struct drm_gpuva_ops *
drm_gpuva_sm_map_ops_create(struct drm_gpuva_manager *mgr,
			    u64 addr, u64 range,
			    struct drm_gem_object *obj, u64 offset);
struct drm_gpuva_ops *
drm_gpuva_sm_unmap_ops_create(struct drm_gpuva_manager *mgr,
			      u64 addr, u64 range);

struct drm_gpuva_ops *
drm_gpuva_prefetch_ops_create(struct drm_gpuva_manager *mgr,
				 u64 addr, u64 range);

struct drm_gpuva_ops *
drm_gpuva_gem_unmap_ops_create(struct drm_gpuva_manager *mgr,
			       struct drm_gem_object *obj);

void drm_gpuva_ops_free(struct drm_gpuva_manager *mgr,
			struct drm_gpuva_ops *ops);

/**
 * struct drm_gpuva_fn_ops - callbacks for split/merge steps
 *
 * This structure defines the callbacks used by &drm_gpuva_sm_map and
 * &drm_gpuva_sm_unmap to provide the split/merge steps for map and unmap
 * operations to drivers.
 */
struct drm_gpuva_fn_ops {
	/**
	 * @op_alloc: called when the &drm_gpuva_manager allocates
	 * a struct drm_gpuva_op
	 *
	 * Some drivers may want to embed struct drm_gpuva_op into driver
	 * specific structures. By implementing this callback drivers can
	 * allocate memory accordingly.
	 *
	 * This callback is optional.
	 */
	struct drm_gpuva_op *(*op_alloc)(void);

	/**
	 * @op_free: called when the &drm_gpuva_manager frees a
	 * struct drm_gpuva_op
	 *
	 * Some drivers may want to embed struct drm_gpuva_op into driver
	 * specific structures. By implementing this callback drivers can
	 * free the previously allocated memory accordingly.
	 *
	 * This callback is optional.
	 */
	void (*op_free)(struct drm_gpuva_op *op);

	/**
	 * @sm_step_map: called from &drm_gpuva_sm_map to finally insert the
	 * mapping once all previous steps were completed
	 *
	 * The &priv pointer matches the one the driver passed to
	 * &drm_gpuva_sm_map or &drm_gpuva_sm_unmap, respectively.
	 *
	 * Can be NULL if &drm_gpuva_sm_map is used.
	 */
	int (*sm_step_map)(struct drm_gpuva_op *op, void *priv);

	/**
	 * @sm_step_remap: called from &drm_gpuva_sm_map and
	 * &drm_gpuva_sm_unmap to split up an existent mapping
	 *
	 * This callback is called when existent mapping needs to be split up.
	 * This is the case when either a newly requested mapping overlaps or
	 * is enclosed by an existent mapping or a partial unmap of an existent
	 * mapping is requested.
	 *
	 * Drivers must not modify the GPUVA space with accessors that do not
	 * take a &drm_gpuva_state as argument from this callback.
	 *
	 * The &priv pointer matches the one the driver passed to
	 * &drm_gpuva_sm_map or &drm_gpuva_sm_unmap, respectively.
	 *
	 * Can be NULL if neither &drm_gpuva_sm_map nor &drm_gpuva_sm_unmap is
	 * used.
	 */
	int (*sm_step_remap)(struct drm_gpuva_op *op,
			     drm_gpuva_state_t state,
			     void *priv);

	/**
	 * @sm_step_unmap: called from &drm_gpuva_sm_map and
	 * &drm_gpuva_sm_unmap to unmap an existent mapping
	 *
	 * This callback is called when existent mapping needs to be unmapped.
	 * This is the case when either a newly requested mapping encloses an
	 * existent mapping or an unmap of an existent mapping is requested.
	 *
	 * Drivers must not modify the GPUVA space with accessors that do not
	 * take a &drm_gpuva_state as argument from this callback.
	 *
	 * The &priv pointer matches the one the driver passed to
	 * &drm_gpuva_sm_map or &drm_gpuva_sm_unmap, respectively.
	 *
	 * Can be NULL if neither &drm_gpuva_sm_map nor &drm_gpuva_sm_unmap is
	 * used.
	 */
	int (*sm_step_unmap)(struct drm_gpuva_op *op,
			     drm_gpuva_state_t state,
			     void *priv);
};

int drm_gpuva_sm_map(struct drm_gpuva_manager *mgr, void *priv,
		     u64 addr, u64 range,
		     struct drm_gem_object *obj, u64 offset);

int drm_gpuva_sm_unmap(struct drm_gpuva_manager *mgr, void *priv,
		       u64 addr, u64 range);

int drm_gpuva_map(struct drm_gpuva_manager *mgr,
		  struct drm_gpuva_prealloc *pa,
		  struct drm_gpuva *va);
int drm_gpuva_remap(drm_gpuva_state_t state,
		    struct drm_gpuva *prev,
		    struct drm_gpuva *next);
void drm_gpuva_unmap(drm_gpuva_state_t state);

#endif /* __DRM_GPUVA_MGR_H__ */
