// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright (c) 2022 Imagination Technologies Ltd. */

#include "pvr_vm.h"

#include "drm/pvr_drm.h"
#include "linux/gfp_types.h"
#include "pvr_device.h"
#include "pvr_drv.h"
#include "pvr_gem.h"
#include "pvr_rogue_heap_config.h"
#include "pvr_rogue_mmu_defs.h"

#include <drm/drm_gem.h>
#include <drm/drm_gpuva_mgr.h>

#include <linux/container_of.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/stddef.h>

/*
 * The value of the device page size (%PVR_DEVICE_PAGE_SIZE) is currently
 * pegged to the host page size (%PAGE_SIZE). This chunk of macro goodness both
 * ensures that the selected host page size corresponds to a valid device page
 * size and sets up values needed by the MMU code below.
 */
#if (PVR_DEVICE_PAGE_SIZE == SZ_4K)
# define ROGUE_MMUCTRL_PAGE_SIZE_X ROGUE_MMUCTRL_PAGE_SIZE_4KB
# define ROGUE_MMUCTRL_PAGE_X_RANGE_SHIFT ROGUE_MMUCTRL_PAGE_4KB_RANGE_SHIFT
# define ROGUE_MMUCTRL_PAGE_X_RANGE_CLRMSK ROGUE_MMUCTRL_PAGE_4KB_RANGE_CLRMSK
#elif (PVR_DEVICE_PAGE_SIZE == SZ_16K)
# define ROGUE_MMUCTRL_PAGE_SIZE_X ROGUE_MMUCTRL_PAGE_SIZE_16KB
# define ROGUE_MMUCTRL_PAGE_X_RANGE_SHIFT ROGUE_MMUCTRL_PAGE_16KB_RANGE_SHIFT
# define ROGUE_MMUCTRL_PAGE_X_RANGE_CLRMSK ROGUE_MMUCTRL_PAGE_16KB_RANGE_CLRMSK
#elif (PVR_DEVICE_PAGE_SIZE == SZ_64K)
# define ROGUE_MMUCTRL_PAGE_SIZE_X ROGUE_MMUCTRL_PAGE_SIZE_64KB
# define ROGUE_MMUCTRL_PAGE_X_RANGE_SHIFT ROGUE_MMUCTRL_PAGE_64KB_RANGE_SHIFT
# define ROGUE_MMUCTRL_PAGE_X_RANGE_CLRMSK ROGUE_MMUCTRL_PAGE_64KB_RANGE_CLRMSK
#elif (PVR_DEVICE_PAGE_SIZE == SZ_256K)
# define ROGUE_MMUCTRL_PAGE_SIZE_X ROGUE_MMUCTRL_PAGE_SIZE_256KB
# define ROGUE_MMUCTRL_PAGE_X_RANGE_SHIFT ROGUE_MMUCTRL_PAGE_256KB_RANGE_SHIFT
# define ROGUE_MMUCTRL_PAGE_X_RANGE_CLRMSK ROGUE_MMUCTRL_PAGE_256KB_RANGE_CLRMSK
#elif (PVR_DEVICE_PAGE_SIZE == SZ_1M)
# define ROGUE_MMUCTRL_PAGE_SIZE_X ROGUE_MMUCTRL_PAGE_SIZE_1MB
# define ROGUE_MMUCTRL_PAGE_X_RANGE_SHIFT ROGUE_MMUCTRL_PAGE_1MB_RANGE_SHIFT
# define ROGUE_MMUCTRL_PAGE_X_RANGE_CLRMSK ROGUE_MMUCTRL_PAGE_1MB_RANGE_CLRMSK
#elif (PVR_DEVICE_PAGE_SIZE == SZ_2M)
# define ROGUE_MMUCTRL_PAGE_SIZE_X ROGUE_MMUCTRL_PAGE_SIZE_2MB
# define ROGUE_MMUCTRL_PAGE_X_RANGE_SHIFT ROGUE_MMUCTRL_PAGE_2MB_RANGE_SHIFT
# define ROGUE_MMUCTRL_PAGE_X_RANGE_CLRMSK ROGUE_MMUCTRL_PAGE_2MB_RANGE_CLRMSK
#else
# error Unsupported device page size PVR_DEVICE_PAGE_SIZE
#endif

#define ROGUE_MMUCTRL_ENTRIES_PT_VALUE_X   \
	(ROGUE_MMUCTRL_ENTRIES_PT_VALUE >> \
	 (PVR_DEVICE_PAGE_SHIFT - PVR_SHIFT_FROM_SIZE(SZ_4K)))

/**
 * pvr_vm_mmu_flush() - Request flush of all MMU caches.
 * @pvr_dev: Target PowerVR device.
 *
 * This function must be called following any possible change to the MMU page
 * tables.
 *
 * Returns:
 *  * 0 on success, or
 *  * Any error encountered while submitting the flush command via the KCCB.
 */
int
pvr_vm_mmu_flush(struct pvr_device *pvr_dev)
{
	struct rogue_fwif_kccb_cmd cmd_mmu_cache;
	struct rogue_fwif_mmucachedata *cmd_mmu_cache_data =
		&cmd_mmu_cache.cmd_data.mmu_cache_data;
	u32 slot;
	int err;

	/* Can't flush MMU if the firmware hasn't booted yet. */
	if (!pvr_dev->fw_dev.booted) {
		err = 0;
		goto err_out;
	}

	cmd_mmu_cache.cmd_type = ROGUE_FWIF_KCCB_CMD_MMUCACHE;
	/* Request a complete MMU flush, across all pagetable levels, TLBs and contexts. */
	cmd_mmu_cache_data->cache_flags = ROGUE_FWIF_MMUCACHEDATA_FLAGS_PT |
					  ROGUE_FWIF_MMUCACHEDATA_FLAGS_PD |
					  ROGUE_FWIF_MMUCACHEDATA_FLAGS_PC |
					  ROGUE_FWIF_MMUCACHEDATA_FLAGS_TLB |
					  ROGUE_FWIF_MMUCACHEDATA_FLAGS_INTERRUPT;
	pvr_fw_object_get_fw_addr(pvr_dev->fw_dev.mem.mmucache_sync_obj,
				  &cmd_mmu_cache_data->mmu_cache_sync_fw_addr);
	cmd_mmu_cache_data->mmu_cache_sync_update_value = 0;

	err = pvr_kccb_send_cmd(pvr_dev, &cmd_mmu_cache, &slot);
	if (err)
		goto err_out;

	err = pvr_kccb_wait_for_completion(pvr_dev, slot, HZ, NULL);
	if (err)
		goto err_out;

err_out:
	return err;
}

/**
 * DOC: PowerVR Virtual Memory Handling
 */
/**
 * DOC: PowerVR Virtual Memory Handling (constants)
 *
 * .. c:macro:: PVR_IDX_INVALID
 *
 *    Default value for a u16-based index.
 *
 *    This value cannot be zero, since zero is a valid index value.
 */
#define PVR_IDX_INVALID ((u16)(-1))

/**
 * DOC: VM backing pages
 */
/**
 * DOC: VM backing pages (constants)
 *
 * .. c:macro:: PVR_VM_BACKING_PAGE_SIZE
 *
 *    Page size of a PowerVR device's integrated MMU. The CPU page size must be
 *    at least as large as this value for the current implementation; this is
 *    checked at compile-time.
 */
#define PVR_VM_BACKING_PAGE_SIZE SZ_4K
static_assert(PAGE_SIZE >= PVR_VM_BACKING_PAGE_SIZE);

/**
 * struct pvr_vm_backing_page - Represents a single page used to back a page
 *                              table of any level.
 * @dma_addr: DMA address of this page.
 * @host_ptr: CPU address of this page.
 * @pvr_dev: The PowerVR device to which this page is associated. **For
 *           internal use only.**
 */
struct pvr_vm_backing_page {
	dma_addr_t dma_addr;
	void *host_ptr;
/* private: internal use only */
	struct pvr_device *pvr_dev;
};

/**
 * pvr_vm_backing_page_init() - Initialize a VM backing page.
 * @page: Target backing page.
 * @pvr_dev: Target PowerVR device.
 *
 * This function performs three distinct operations:
 *
 * 1. Allocate a single page,
 * 2. Map the page to the CPU, and
 * 3. Map the page to DMA-space.
 *
 * It is expected that @page be zeroed (e.g. from kzalloc()) before calling
 * this function.
 *
 * Return:
 *  * 0 on success, or
 *  * -%ENOMEM if allocation of the backing page or mapping of the backing
 *    page to DMA fails.
 */
static int
pvr_vm_backing_page_init(struct pvr_vm_backing_page *page,
			 struct pvr_device *pvr_dev)
{
	struct device *dev = from_pvr_device(pvr_dev)->dev;

	struct page *raw_page;
	int err;

	dma_addr_t dma_addr;
	void *host_ptr;

	raw_page = alloc_page(__GFP_ZERO | GFP_KERNEL);
	if (!raw_page) {
		err = -ENOMEM;
		goto err_out;
	}

	host_ptr = kmap(raw_page);

	dma_addr = dma_map_page(dev, raw_page, 0, PVR_VM_BACKING_PAGE_SIZE,
				DMA_TO_DEVICE);
	if (dma_mapping_error(dev, dma_addr)) {
		err = -ENOMEM;
		goto err_unmap_free_page;
	}

	page->dma_addr = dma_addr;
	page->host_ptr = host_ptr;
	page->pvr_dev = pvr_dev;

	return 0;

err_unmap_free_page:
	kunmap(raw_page);
	__free_page(raw_page);

err_out:
	return err;
}

/**
 * pvr_vm_backing_page_fini() - Teardown a VM backing page.
 * @page: Target backing page.
 *
 * This function performs the mirror operations to pvr_vm_backing_page_init(),
 * in reverse order:
 *
 * 1. Unmap the page from DMA-space,
 * 2. Unmap the page from the CPU, and
 * 3. Free the page.
 *
 * It also zeros @page.
 *
 * It is a no-op to call this function a second (or further) time on any @page.
 */
static void
pvr_vm_backing_page_fini(struct pvr_vm_backing_page *page)
{
	struct device *dev = from_pvr_device(page->pvr_dev)->dev;
	struct page *raw_page = kmap_to_page(page->host_ptr);

	/* Do nothing if no allocation is present. */
	if (!page->pvr_dev)
		return;

	dma_unmap_page(dev, page->dma_addr, PVR_VM_BACKING_PAGE_SIZE,
		       DMA_TO_DEVICE);

	kunmap(raw_page);

	__free_page(raw_page);

	memset(page, 0, sizeof(*page));
}

/**
 * pvr_vm_backing_page_sync() - Flush a VM backing page from the CPU to the
 *                              device.
 * @page: Target backing page.
 *
 * .. caution::
 *
 *    **This is potentially an expensive function call.** Only call
 *    pvr_vm_backing_page_sync() once you're sure you have no more changes to
 *    make to the backing page in the immediate future.
 */
static void
pvr_vm_backing_page_sync(struct pvr_vm_backing_page *page)
{
	struct device *dev;

	/*
	 * Do nothing if no allocation is present. This may be the case if
	 * we are unmapping pages.
	 */
	if (!page->pvr_dev)
		return;

	dev = from_pvr_device(page->pvr_dev)->dev;

	dma_sync_single_for_device(dev, page->dma_addr,
				   PVR_VM_BACKING_PAGE_SIZE, DMA_TO_DEVICE);
}

/**
 * DOC: Raw page tables
 */

#define PVR_PAGE_TABLE_TYPEOF_ENTRY(level_) \
	typeof_member(struct pvr_page_table_l##level_##_entry_raw, val)

#define PVR_PAGE_TABLE_FIELD_GET(level_, name_, field_, entry_)           \
	(((entry_).val &                                           \
	  ~ROGUE_MMUCTRL_##name_##_DATA_##field_##_CLRMSK) >> \
	 ROGUE_MMUCTRL_##name_##_DATA_##field_##_SHIFT)

#define PVR_PAGE_TABLE_FIELD_PREP(level_, name_, field_, val_)            \
	((((PVR_PAGE_TABLE_TYPEOF_ENTRY(level_))(val_))            \
	  << ROGUE_MMUCTRL_##name_##_DATA_##field_##_SHIFT) & \
	 ~ROGUE_MMUCTRL_##name_##_DATA_##field_##_CLRMSK)

/**
 * struct pvr_page_table_l2_entry_raw - A single entry in a level 2 page table.
 * @val: The raw value of this entry.
 *
 * This type is a structure for type-checking purposes. At compile-time, its
 * size is checked against %ROGUE_MMUCTRL_ENTRY_SIZE_PC_VALUE.
 *
 * The value stored in this structure can be decoded using the following bitmap:
 *
 * .. flat-table::
 *    :widths: 1 5
 *    :stub-columns: 1
 *
 *    * - 31..4
 *      - **Level 1 Page Table Base Address:** Bits 39..12 of the L1
 *        page table base address, which is 4KiB aligned.
 *
 *    * - 3..2
 *      - *(reserved)*
 *
 *    * - 1
 *      - **Pending:** When valid bit is not set, indicates that a valid
 *        entry is pending and the MMU should wait for the driver to map
 *        the entry. This is used to support page demand mapping of
 *        memory.
 *
 *    * - 0
 *      - **Valid:** Indicates that the entry contains a valid L1 page
 *        table. If the valid bit is not set, then an attempted use of
 *        the page would result in a page fault.
 */
struct pvr_page_table_l2_entry_raw {
	u32 val;
} __packed;
static_assert(sizeof(struct pvr_page_table_l2_entry_raw) * 8 ==
	      ROGUE_MMUCTRL_ENTRY_SIZE_PC_VALUE);

static __always_inline bool
pvr_page_table_l2_entry_raw_is_valid(struct pvr_page_table_l2_entry_raw entry)
{
	return PVR_PAGE_TABLE_FIELD_GET(2, PC, VALID, entry);
}

/**
 * pvr_page_table_l2_entry_raw_set() - Write a valid entry into a raw level 2
 *                                     page table.
 * @entry: Target raw level 2 page table entry.
 * @child_table_dma_addr: DMA address of the level 1 page table to be
 *                        associated with @entry.
 *
 * When calling this function, @child_table_dma_addr must be a valid DMA
 * address and a multiple of %ROGUE_MMUCTRL_PC_DATA_PD_BASE_ALIGNSIZE.
 */
static __always_inline void
pvr_page_table_l2_entry_raw_set(struct pvr_page_table_l2_entry_raw *entry,
				dma_addr_t child_table_dma_addr)
{
	child_table_dma_addr >>= ROGUE_MMUCTRL_PC_DATA_PD_BASE_ALIGNSHIFT;

	entry->val =
		PVR_PAGE_TABLE_FIELD_PREP(2, PC, VALID, true) |
		PVR_PAGE_TABLE_FIELD_PREP(2, PC, ENTRY_PENDING, false) |
		PVR_PAGE_TABLE_FIELD_PREP(2, PC, PD_BASE, child_table_dma_addr);
}

static __always_inline void
pvr_page_table_l2_entry_raw_clear(struct pvr_page_table_l2_entry_raw *entry)
{
	entry->val = 0;
}

/**
 * struct pvr_page_table_l1_entry_raw - A single entry in a level 1 page table.
 * @val: The raw value of this entry.
 *
 * This type is a structure for type-checking purposes. At compile-time, its
 * size is checked against %ROGUE_MMUCTRL_ENTRY_SIZE_PD_VALUE.
 *
 * The value stored in this structure can be decoded using the following bitmap:
 *
 * .. flat-table::
 *    :widths: 1 5
 *    :stub-columns: 1
 *
 *    * - 63..41
 *      - *(reserved)*
 *
 *    * - 40
 *      - **Pending:** When valid bit is not set, indicates that a valid entry
 *        is pending and the MMU should wait for the driver to map the entry.
 *        This is used to support page demand mapping of memory.
 *
 *    * - 39..5
 *      - **Level 0 Page Table Base Address:** The way this value is
 *        interpreted depends on the page size. Bits not specified in the
 *        table below (e.g. bits 11..5 for page size 4KiB) should be
 *        considered reserved.
 *
 *        This table shows the bits used in an L1 page table entry to
 *        represent the Physical Table Base Address for a given Page Size.
 *        Since each L1 page table entry covers 2MiB of address space, the
 *        maximum page size is 2MiB.
 *
 *        .. flat-table::
 *           :widths: 1 1 1 1
 *           :header-rows: 1
 *           :stub-columns: 1
 *
 *           * - Page size
 *             - L0 page table base address bits
 *             - Number of L0 page table entries
 *             - Size of L0 page table
 *
 *           * - 4KiB
 *             - 39..12
 *             - 512
 *             - 4KiB
 *
 *           * - 16KiB
 *             - 39..10
 *             - 128
 *             - 1KiB
 *
 *           * - 64KiB
 *             - 39..8
 *             - 32
 *             - 256B
 *
 *           * - 256KiB
 *             - 39..6
 *             - 8
 *             - 64B
 *
 *           * - 1MiB
 *             - 39..5 (4 = '0')
 *             - 2
 *             - 16B
 *
 *           * - 2MiB
 *             - 39..5 (4..3 = '00')
 *             - 1
 *             - 8B
 *
 *    * - 4
 *      - *(reserved)*
 *
 *    * - 3..1
 *      - **Page Size:** Sets the page size, from 4KiB to 2MiB.
 *
 *    * - 0
 *      - **Valid:** Indicates that the entry contains a valid L0 page table.
 *        If the valid bit is not set, then an attempted use of the page would
 *        result in a page fault.
 */
struct pvr_page_table_l1_entry_raw {
	u64 val;
} __packed;
static_assert(sizeof(struct pvr_page_table_l1_entry_raw) * 8 ==
	      ROGUE_MMUCTRL_ENTRY_SIZE_PD_VALUE);

static __always_inline bool
pvr_page_table_l1_entry_raw_is_valid(struct pvr_page_table_l1_entry_raw entry)
{
	return PVR_PAGE_TABLE_FIELD_GET(1, PD, VALID, entry);
}

/**
 * pvr_page_table_l1_entry_raw_set() - Write a valid entry into a raw level 1
 *                                     page table.
 * @entry: Target raw level 1 page table entry.
 * @child_table_dma_addr: DMA address of the level 0 page table to be
 *                        associated with @entry.
 *
 * When calling this function, @child_table_dma_addr must be a valid DMA
 * address and a multiple of 4 KiB.
 */
static void
pvr_page_table_l1_entry_raw_set(struct pvr_page_table_l1_entry_raw *entry,
				dma_addr_t child_table_dma_addr)
{
	entry->val = PVR_PAGE_TABLE_FIELD_PREP(1, PD, VALID, true) |
		     PVR_PAGE_TABLE_FIELD_PREP(1, PD, ENTRY_PENDING, false) |
		     PVR_PAGE_TABLE_FIELD_PREP(1, PD, PAGE_SIZE,
					       ROGUE_MMUCTRL_PAGE_SIZE_X) |
		     /*
		      * The use of a 4K-specific macro here is correct. It is
		      * a future optimization to allocate sub-host-page-sized
		      * blocks for individual tables, so the condition that any
		      * page table address is aligned to the size of the
		      * largest (a 4KB) table currently holds.
		      */
		     (child_table_dma_addr &
		      ~ROGUE_MMUCTRL_PT_BASE_4KB_RANGE_CLRMSK);
}

static __always_inline void
pvr_page_table_l1_entry_raw_clear(struct pvr_page_table_l1_entry_raw *entry)
{
	entry->val = 0;
}

/**
 * struct pvr_page_table_l0_entry_raw - A single entry in a level 0 page table.
 * @val: The raw value of this entry.
 *
 * This type is a structure for type-checking purposes. At compile-time, its
 * size is checked against %ROGUE_MMUCTRL_ENTRY_SIZE_PT_VALUE.
 *
 * The value stored in this structure can be decoded using the following bitmap:
 *
 * .. flat-table::
 *    :widths: 1 5
 *    :stub-columns: 1
 *
 *    * - 63
 *      - *(reserved)*
 *
 *    * - 62
 *      - **PM/FW Protect:** Indicates a protected region which only the
 *        Parameter Manager (PM) or firmware processor can write to.
 *
 *    * - 61..40
 *      - **VP Page (High):** Virtual-physical page used for Parameter Manager
 *        (PM) memory. This field is only used if the additional level of PB
 *        virtualization is enabled. The VP Page field is needed by the PM in
 *        order to correctly reconstitute the free lists after render
 *        completion. This (High) field holds bits 39..18 of the value; the
 *        Low field holds bits 17..12. Bits 11..0 are always zero because the
 *        value is always aligned to the 4KiB page size.
 *
 *    * - 39..12
 *      - **Physical Page Address:** The way this value is interpreted depends
 *        on the page size. Bits not specified in the table below (e.g. bits
 *        20..12 for page size 2MiB) should be considered reserved.
 *
 *        This table shows the bits used in an L0 page table entry to represent
 *        the Physical Page Address for a given page size (as defined in the
 *        associated L1 page table entry).
 *
 *        .. flat-table::
 *           :widths: 1 1
 *           :header-rows: 1
 *           :stub-columns: 1
 *
 *           * - Page size
 *             - Physical address bits
 *
 *           * - 4KiB
 *             - 39..12
 *
 *           * - 16KiB
 *             - 39..14
 *
 *           * - 64KiB
 *             - 39..16
 *
 *           * - 256KiB
 *             - 39..18
 *
 *           * - 1MiB
 *             - 39..20
 *
 *           * - 2MiB
 *             - 39..21
 *
 *    * - 11..6
 *      - **VP Page (Low):** Continuation of VP Page (High).
 *
 *    * - 5
 *      - **Pending:** When valid bit is not set, indicates that a valid entry
 *        is pending and the MMU should wait for the driver to map the entry.
 *        This is used to support page demand mapping of memory.
 *
 *    * - 4
 *      - **PM Src:** Set on Parameter Manager (PM) allocated page table
 *        entries when indicated by the PM. Note that this bit will only be set
 *        by the PM, not by the device driver.
 *
 *    * - 3
 *      - **SLC Bypass Control:** Specifies requests to this page should bypass
 *        the System Level Cache (SLC), if enabled in SLC configuration.
 *
 *    * - 2
 *      - **Cache Coherency:** Indicates that the page is coherent (i.e. it
 *        does not require a cache flush between operations on the CPU and the
 *        device).
 *
 *    * - 1
 *      - **Read Only:** If set, this bit indicates that the page is read only.
 *        An attempted write to this page would result in a write-protection
 *        fault.
 *
 *    * - 0
 *      - **Valid:** Indicates that the entry contains a valid page. If the
 *        valid bit is not set, then an attempted use of the page would result
 *        in a page fault.
 */
struct pvr_page_table_l0_entry_raw {
	u64 val;
} __packed;
static_assert(sizeof(struct pvr_page_table_l0_entry_raw) * 8 ==
	      ROGUE_MMUCTRL_ENTRY_SIZE_PT_VALUE);

/**
 * struct pvr_page_flags_raw - The configurable flags from a single entry in a
 *                             level 0 page table.
 * @val: The raw value of these flags. Since these are a strict subset of
 *       &struct pvr_page_table_l0_entry_raw; use that type for our member here.
 *
 * The flags stored in this type are: PM/FW Protect; SLC Bypass Control; Cache
 * Coherency, and Read Only (bits 62, 3, 2 and 1 respectively).
 *
 * This type should never be instantiated directly; instead use
 * pvr_page_flags_raw_create() to ensure only valid bits of @val are set.
 */
struct pvr_page_flags_raw {
	struct pvr_page_table_l0_entry_raw val;
} __packed;
static_assert(sizeof(struct pvr_page_flags_raw) ==
	      sizeof(struct pvr_page_table_l0_entry_raw));

static __always_inline bool
pvr_page_table_l0_entry_raw_is_valid(struct pvr_page_table_l0_entry_raw entry)
{
	return PVR_PAGE_TABLE_FIELD_GET(0, PT, VALID, entry);
}

/**
 * pvr_page_table_l0_entry_raw_set() - Write a valid entry into a raw level 0
 *                                     page table.
 * @entry: Target raw level 0 page table entry.
 * @dma_addr: DMA address of the physical page to be associated with @entry.
 * @flags: Options to be set on @entry.
 *
 * When calling this function, @child_table_dma_addr must be a valid DMA
 * address and a multiple of %PVR_DEVICE_PAGE_SIZE.
 *
 * The @flags parameter is directly assigned into @entry. It is the callers
 * responsibility to ensure that only bits specified in
 * &struct pvr_page_flags_raw are set in @flags.
 */
static void
pvr_page_table_l0_entry_raw_set(struct pvr_page_table_l0_entry_raw *entry,
				dma_addr_t dma_addr,
				struct pvr_page_flags_raw flags)
{
	entry->val = PVR_PAGE_TABLE_FIELD_PREP(0, PT, VALID, true) |
		     PVR_PAGE_TABLE_FIELD_PREP(0, PT, ENTRY_PENDING, false) |
		     (dma_addr & ~ROGUE_MMUCTRL_PAGE_X_RANGE_CLRMSK) |
		     flags.val.val;
}

static __always_inline void
pvr_page_table_l0_entry_raw_clear(struct pvr_page_table_l0_entry_raw *entry)
{
	entry->val = 0;
}

/**
 * pvr_page_flags_raw_create() - Initialize the flag bits of a raw level 0 page
 *                               table entry.
 * @read_only: This page is read-only (see: Read Only).
 * @cache_coherent: This page does not require cache flushes (see: Cache
 *                  Coherency).
 * @slc_bypass: This page bypasses the device cache (see: SLC Bypass Control).
 * @pm_fw_protect: This page is only for use by the firmware or Parameter
 *                 Manager (see PM/FW Protect).
 *
 * For more details on the use of these four options, see their respective
 * entries in the table under &struct pvr_page_table_l0_entry_raw.
 *
 * Return:
 * A new &struct pvr_page_flags_raw instance which can be passed directly to
 * pvr_page_table_l0_entry_raw_set() or pvr_page_table_l0_insert().
 */
static struct pvr_page_flags_raw
pvr_page_flags_raw_create(bool read_only, bool cache_coherent, bool slc_bypass,
			  bool pm_fw_protect)
{
	struct pvr_page_flags_raw flags;

	flags.val.val =
		PVR_PAGE_TABLE_FIELD_PREP(0, PT, READ_ONLY, read_only) |
		PVR_PAGE_TABLE_FIELD_PREP(0, PT, CC, cache_coherent) |
		PVR_PAGE_TABLE_FIELD_PREP(0, PT, SLC_BYPASS_CTRL, slc_bypass) |
		PVR_PAGE_TABLE_FIELD_PREP(0, PT, PM_META_PROTECT, pm_fw_protect);

	return flags;
}

/**
 * struct pvr_page_table_l2_raw - The raw data of a level 2 page table.
 *
 * This type is a structure for type-checking purposes. At compile-time, its
 * size is checked against %PVR_VM_BACKING_PAGE_SIZE.
 */
struct pvr_page_table_l2_raw {
	/** @entries: The raw values of this table. */
	struct pvr_page_table_l2_entry_raw
		entries[ROGUE_MMUCTRL_ENTRIES_PC_VALUE];
} __packed;
static_assert(sizeof(struct pvr_page_table_l2_raw) == PVR_VM_BACKING_PAGE_SIZE);

/**
 * struct pvr_page_table_l1_raw - The raw data of a level 1 page table.
 *
 * This type is a structure for type-checking purposes. At compile-time, its
 * size is checked against %PVR_VM_BACKING_PAGE_SIZE.
 */
struct pvr_page_table_l1_raw {
	/** @entries: The raw values of this table. */
	struct pvr_page_table_l1_entry_raw
		entries[ROGUE_MMUCTRL_ENTRIES_PD_VALUE];
} __packed;
static_assert(sizeof(struct pvr_page_table_l1_raw) == PVR_VM_BACKING_PAGE_SIZE);

/**
 * struct pvr_page_table_l0_raw - The raw data of a level 0 page table.
 *
 * This type is a structure for type-checking purposes. At compile-time, its
 * size is checked against %PVR_VM_BACKING_PAGE_SIZE.
 *
 * .. caution::
 *
 *    The size of level 0 page tables is variable depending on the page size
 *    specified in the associated level 1 page table entry. Since the device
 *    page size in use is pegged to the host page size, it cannot vary at
 *    runtime. This structure is therefore only defined to contain the required
 *    number of entries for the current device page size. **You should never
 *    read or write beyond the last supported entry.**
 */
struct pvr_page_table_l0_raw {
	/** @entries: The raw values of this table. */
	struct pvr_page_table_l0_entry_raw
		entries[ROGUE_MMUCTRL_ENTRIES_PT_VALUE_X];
} __packed;
static_assert(sizeof(struct pvr_page_table_l0_raw) <= PVR_VM_BACKING_PAGE_SIZE);

/**
 * DOC: Mirror page tables
 */

/*
 * We pre-declare these types because they cross-depend on pointers to each
 * other.
 */
struct pvr_page_table_l2;
struct pvr_page_table_l1;
struct pvr_page_table_l0;

/**
 * struct pvr_page_table_ptr - A reference to a single physical page as indexed
 *                             by the page table structure.
 * @pvr_dev: The PowerVR device associated with the VM context the
 *           pointer is traversing.
 * @l1_free_list: List of level 1 page tables to free when the pointer is destroyed.
 * @l0_free_list: List of level 0 page tables to free when the pointer is destroyed.
 * @l2_table: A cached handle to the level 2 page table the pointer is
 *            currently traversing.
 * @l1_table: A cached handle to the level 1 page table the pointer is
 *            currently traversing.
 * @l0_table: A cached handle to the level 0 page table the pointer is
 *            currently traversing.
 * @l2_idx: Index into the level 2 page table the pointer is currently
 *          referencing.
 * @l1_idx: Index into the level 1 page table the pointer is currently
 *          referencing.
 * @l0_idx: Index into the level 0 page table the pointer is currently
 *          referencing.
 * @sync_level_required: The maximum level of the page table tree structure
 *                       which has (possibly) been modified since it was last
 *                       flushed to the device.
 *
 *                       This field should only be set with
 *                       pvr_page_table_ptr_require_sync() or indirectly by
 *                       pvr_page_table_ptr_sync_partial().
 */
struct pvr_page_table_ptr {
	struct pvr_device *pvr_dev;
	struct pvr_page_table_l1 *l1_free_list;
	struct pvr_page_table_l0 *l0_free_list;
	struct pvr_page_table_l2 *l2_table;
	struct pvr_page_table_l1 *l1_table;
	struct pvr_page_table_l0 *l0_table;
	u16 l2_idx;
	u16 l1_idx;
	u16 l0_idx;
	s8 sync_level_required;
};

/**
 * struct pvr_page_table_l2 - A wrapped level 2 page table.
 *
 * To access the raw part of this table, use pvr_page_table_l2_get_raw().
 * Alternatively to access a raw entry directly, use
 * pvr_page_table_l2_get_entry_raw().
 *
 * A level 2 page table forms the root of the page table tree structure, so
 * this type has no &parent or &parent_idx members.
 */
struct pvr_page_table_l2 {
	/**
	 * @entries: The children of this node in the page table tree
	 * structure. These are also mirror tables. The indexing of this array
	 * is identical to that of the raw equivalent
	 * (&pvr_page_table_l1_raw.entries).
	 */
	struct pvr_page_table_l1 *entries[ROGUE_MMUCTRL_ENTRIES_PC_VALUE];

	/**
	 * @backing_page: A handle to the memory which holds the raw
	 * equivalent of this table. **For internal use only.**
	 */
	struct pvr_vm_backing_page backing_page;

	/**
	 * @entry_count: The current number of valid entries (that we know of)
	 * in this table. This value is essentially a refcount - the table is
	 * destroyed when this value is decremented to zero by
	 * pvr_page_table_l2_remove().
	 */
	u16 entry_count;
};

/**
 * pvr_page_table_l2_init() - Initialize a level 2 page table.
 * @table: Target level 2 page table.
 * @pvr_dev: Target PowerVR device
 *
 * It is expected that @table be zeroed (e.g. from kzalloc()) before calling
 * this function.
 *
 * Return:
 *  * 0 on success, or
 *  * Any error encountered while intializing &table->backing_page using
 *    pvr_vm_backing_page_init().
 */
static __always_inline int
pvr_page_table_l2_init(struct pvr_page_table_l2 *table,
		       struct pvr_device *pvr_dev)
{
	return pvr_vm_backing_page_init(&table->backing_page, pvr_dev);
}

/**
 * pvr_page_table_l2_fini() - Teardown a level 2 page table.
 * @table: Target level 2 page table.
 *
 * It is an error to attempt to use @table after calling this function.
 */
static __always_inline void
pvr_page_table_l2_fini(struct pvr_page_table_l2 *table)
{
	pvr_vm_backing_page_fini(&table->backing_page);
}

/**
 * pvr_page_table_l2_sync() - Flush a level 2 page table from the CPU to the
 *                            device.
 * @table: Target level 2 page table.
 *
 * This is just a thin wrapper around pvr_vm_backing_page_sync(), so the
 * warning there applies here too: **Only call pvr_page_table_l2_sync() once
 * you're sure you have no more changes to make to** @table **in the immediate
 * future.**
 *
 * If child level 1 page tables of @table also need to be flushed, this should
 * be done first using pvr_page_table_l1_sync() *before* calling this function.
 */
static __always_inline void
pvr_page_table_l2_sync(struct pvr_page_table_l2 *table)
{
	pvr_vm_backing_page_sync(&table->backing_page);
}

/**
 * pvr_page_table_l2_get_raw() - Access the raw equivalent of a mirror level 2
 *                               page table.
 * @table: Target level 2 page table.
 *
 * Essentially returns the CPU address of the raw equivalent of @table, cast to
 * a &struct pvr_page_table_l2_raw pointer.
 *
 * You probably want to call pvr_page_table_l2_get_entry_raw() instead.
 *
 * Return:
 * The raw equivalent of @table.
 */
static __always_inline struct pvr_page_table_l2_raw *
pvr_page_table_l2_get_raw(struct pvr_page_table_l2 *table)
{
	return table->backing_page.host_ptr;
}

/**
 * pvr_page_table_l2_get_entry_raw() - Access an entry from the raw equivalent
 *                                     of a mirror level 2 page table.
 * @table: Target level 2 page table.
 * @idx: Index of the entry to access.
 *
 * Technically this function returns a pointer to a slot in a raw level 2 page
 * table, since the returned "entry" is not guaranteed to be valid. The caller
 * must verify the validity of the entry at the returned address (perhaps using
 * pvr_page_table_l2_entry_raw_is_valid()) before reading or overwriting it.
 *
 * The value of @idx is not checked here; it is the callers responsibility to
 * ensure @idx refers to a valid index within @table before dereferencing the
 * returned pointer.
 *
 * Return:
 * A pointer to the requested raw level 2 page table entry.
 */
static __always_inline struct pvr_page_table_l2_entry_raw *
pvr_page_table_l2_get_entry_raw(struct pvr_page_table_l2 *table, u16 idx)
{
	return &pvr_page_table_l2_get_raw(table)->entries[idx];
}

/**
 * pvr_page_table_l2_entry_is_valid() - Check if a level 2 page table entry is
 *                                      marked as valid.
 * @table: Target level 2 page table.
 * @idx: Index of the entry to check.
 *
 * The value of @idx is not checked here; it is the callers responsibility to
 * ensure @idx refers to a valid index within @table before calling this
 * function.
 */
static __always_inline bool
pvr_page_table_l2_entry_is_valid(struct pvr_page_table_l2 *table, u16 idx)
{
	struct pvr_page_table_l2_entry_raw entry_raw =
		*pvr_page_table_l2_get_entry_raw(table, idx);

	return pvr_page_table_l2_entry_raw_is_valid(entry_raw);
}

/**
 * struct pvr_page_table_l1 - A wrapped level 1 page table.
 *
 * To access the raw part of this table, use pvr_page_table_l1_get_raw().
 * Alternatively to access a raw entry directly, use
 * pvr_page_table_l1_get_entry_raw().
 */
struct pvr_page_table_l1 {
	/**
	 * @entries: The children of this node in the page table tree
	 * structure. These are also mirror tables. The indexing of this array
	 * is identical to that of the raw equivalent
	 * (&pvr_page_table_l0_raw.entries).
	 */
	struct pvr_page_table_l0 *entries[ROGUE_MMUCTRL_ENTRIES_PD_VALUE];

	/**
	 * @backing_page: A handle to the memory which holds the raw
	 * equivalent of this table. **For internal use only.**
	 */
	struct pvr_vm_backing_page backing_page;

	union {
		/**
		 * @parent: The parent of this node in the page table tree structure.
		 *
		 * This is also a mirror table.
		 *
		 * Only valid when the L1 page table is active. When the L1 page table
		 * has been removed and queued for destruction, the next_free field
		 * should be used instead.
		 */
		struct pvr_page_table_l2 *parent;

		/**
		 * @next_free: Pointer to the next L1 page table to free.
		 *
		 * Used to form a linked list of L1 page tables queued for destruction.
		 * Only valid when the page table has been removed and queued for
		 * destruction.
		 */
		struct pvr_page_table_l1 *next_free;
	};

	/**
	 * @parent_idx: The index of the entry in the parent table (see
	 * @parent) which corresponds to this table.
	 */
	u16 parent_idx;

	/**
	 * @entry_count: The current number of valid entries (that we know of)
	 * in this table. This value is essentially a refcount - the table is
	 * destroyed when this value is decremented to zero by
	 * pvr_page_table_l1_remove().
	 */
	u16 entry_count;
};

/**
 * pvr_page_table_l1_init() - Initialize a level 1 page table.
 * @table: Target level 1 page table.
 * @pvr_dev: Target PowerVR device
 *
 * When this function returns successfully, @table is still not considered
 * valid. It must be inserted into the page table tree structure with
 * pvr_page_table_l2_insert() before it is ready for use.
 *
 * It is expected that @table be zeroed (e.g. from kzalloc()) before calling
 * this function.
 *
 * Return:
 *  * 0 on success, or
 *  * Any error encountered while intializing &table->backing_page using
 *    pvr_vm_backing_page_init().
 */
static __always_inline int
pvr_page_table_l1_init(struct pvr_page_table_l1 *table,
		       struct pvr_device *pvr_dev)
{
	table->parent_idx = PVR_IDX_INVALID;

	return pvr_vm_backing_page_init(&table->backing_page, pvr_dev);
}

/**
 * pvr_page_table_l1_fini() - Teardown a level 1 page table.
 * @table: Target level 1 page table.
 *
 * It is an error to attempt to use @table after calling this function, even
 * indirectly. This includes calling pvr_page_table_l2_remove(), which must
 * be called *before* pvr_page_table_l1_fini().
 */
static __always_inline void
pvr_page_table_l1_fini(struct pvr_page_table_l1 *table)
{
	pvr_vm_backing_page_fini(&table->backing_page);
}

/**
 * pvr_page_table_l1_sync() - Flush a level 1 page table from the CPU to the
 *                            device.
 * @table: Target level 1 page table.
 *
 * This is just a thin wrapper around pvr_vm_backing_page_sync(), so the
 * warning there applies here too: **Only call pvr_page_table_l1_sync() once
 * you're sure you have no more changes to make to** @table **in the immediate
 * future.**
 *
 * If child level 0 page tables of @table also need to be flushed, this should
 * be done first using pvr_page_table_l0_sync() *before* calling this function.
 */
static __always_inline void
pvr_page_table_l1_sync(struct pvr_page_table_l1 *table)
{
	pvr_vm_backing_page_sync(&table->backing_page);
}

/**
 * pvr_page_table_l1_get_raw() - Access the raw equivalent of a mirror level 1
 *                               page table.
 * @table: Target level 1 page table.
 *
 * Essentially returns the CPU address of the raw equivalent of @table, cast to
 * a &struct pvr_page_table_l1_raw pointer.
 *
 * You probably want to call pvr_page_table_l1_get_entry_raw() instead.
 *
 * Return:
 * The raw equivalent of @table.
 */
static __always_inline struct pvr_page_table_l1_raw *
pvr_page_table_l1_get_raw(struct pvr_page_table_l1 *table)
{
	return table->backing_page.host_ptr;
}

/**
 * pvr_page_table_l1_get_entry_raw() - Access an entry from the raw equivalent
 *                                     of a mirror level 1 page table.
 * @table: Target level 1 page table.
 * @idx: Index of the entry to access.
 *
 * Technically this function returns a pointer to a slot in a raw level 1 page
 * table, since the returned "entry" is not guaranteed to be valid. The caller
 * must verify the validity of the entry at the returned address (perhaps using
 * pvr_page_table_l1_entry_raw_is_valid()) before reading or overwriting it.
 *
 * The value of @idx is not checked here; it is the callers responsibility to
 * ensure @idx refers to a valid index within @table before dereferencing the
 * returned pointer.
 *
 * Return:
 * A pointer to the requested raw level 1 page table entry.
 */
static __always_inline struct pvr_page_table_l1_entry_raw *
pvr_page_table_l1_get_entry_raw(struct pvr_page_table_l1 *table, u16 idx)
{
	return &pvr_page_table_l1_get_raw(table)->entries[idx];
}

/**
 * pvr_page_table_l1_entry_is_valid() - Check if a level 1 page table entry is
 *                                      marked as valid.
 * @table: Target level 1 page table.
 * @idx: Index of the entry to check.
 *
 * The value of @idx is not checked here; it is the callers responsibility to
 * ensure @idx refers to a valid index within @table before calling this
 * function.
 */
static __always_inline bool
pvr_page_table_l1_entry_is_valid(struct pvr_page_table_l1 *table, u16 idx)
{
	struct pvr_page_table_l1_entry_raw entry_raw =
		*pvr_page_table_l1_get_entry_raw(table, idx);

	return pvr_page_table_l1_entry_raw_is_valid(entry_raw);
}

/**
 * struct pvr_page_table_l0 - A wrapped level 0 page table.
 *
 * To access the raw part of this table, use pvr_page_table_l0_get_raw().
 * Alternatively to access a raw entry directly, use
 * pvr_page_table_l0_get_entry_raw().
 *
 * There is no mirror representation of an individual page, so this type has no
 * &entries member.
 */
struct pvr_page_table_l0 {
	/**
	 * @backing_page: A handle to the memory which holds the raw
	 * equivalent of this table. **For internal use only.**
	 */
	struct pvr_vm_backing_page backing_page;

	union {
		/**
		 * @parent: The parent of this node in the page table tree structure.
		 *
		 * This is also a mirror table.
		 *
		 * Only valid when the L0 page table is active. When the L0 page table
		 * has been removed and queued for destruction, the next_free field
		 * should be used instead.
		 */
		struct pvr_page_table_l1 *parent;

		/**
		 * @next_free: Pointer to the next L0 page table to free.
		 *
		 * Used to form a linked list of L0 page tables queued for destruction.
		 *
		 * Only valid when the page table has been removed and queued for
		 * destruction.
		 */
		struct pvr_page_table_l0 *next_free;
	};

	/**
	 * @parent_idx: The index of the entry in the parent table (see
	 * @parent) which corresponds to this table.
	 */
	u16 parent_idx;

	/**
	 * @entry_count: The current number of valid entries (that we know of)
	 * in this table. This value is essentially a refcount - the table is
	 * destroyed when this value is decremented to zero by
	 * pvr_page_table_l0_remove().
	 */
	u16 entry_count;
};

/**
 * pvr_page_table_l0_init() - Initialize a level 0 page table.
 * @table: Target level 0 page table.
 * @pvr_dev: Target PowerVR device
 *
 * When this function returns successfully, @table is still not considered
 * valid. It must be inserted into the page table tree structure with
 * pvr_page_table_l1_insert() before it is ready for use.
 *
 * It is expected that @table be zeroed (e.g. from kzalloc()) before calling
 * this function.
 *
 * Return:
 *  * 0 on success, or
 *  * Any error encountered while intializing &table->backing_page using
 *    pvr_vm_backing_page_init().
 */
static __always_inline int
pvr_page_table_l0_init(struct pvr_page_table_l0 *table,
		       struct pvr_device *pvr_dev)
{
	table->parent_idx = PVR_IDX_INVALID;

	return pvr_vm_backing_page_init(&table->backing_page, pvr_dev);
}

/**
 * pvr_page_table_l0_fini() - Teardown a level 0 page table.
 * @table: Target level 0 page table.
 *
 * It is an error to attempt to use @table after calling this function, even
 * indirectly. This includes calling pvr_page_table_l1_remove(), which must
 * be called *before* pvr_page_table_l0_fini().
 */
static __always_inline void
pvr_page_table_l0_fini(struct pvr_page_table_l0 *table)
{
	pvr_vm_backing_page_fini(&table->backing_page);
}

/**
 * pvr_page_table_l0_sync() - Flush a level 0 page table from the CPU to the
 *                            device.
 * @table: Target level 0 page table.
 *
 * This is just a thin wrapper around pvr_vm_backing_page_sync(), so the
 * warning there applies here too: **Only call pvr_page_table_l0_sync() once
 * you're sure you have no more changes to make to** @table **in the immediate
 * future.**
 *
 * If child pages of @table also need to be flushed, this should be done first
 * using a DMA sync function (e.g. dma_sync_sg_for_device()) *before* calling
 * this function.
 */
static __always_inline void
pvr_page_table_l0_sync(struct pvr_page_table_l0 *table)
{
	pvr_vm_backing_page_sync(&table->backing_page);
}

/**
 * pvr_page_table_l0_get_raw() - Access the raw equivalent of a mirror level 0
 *                               page table.
 * @table: Target level 0 page table.
 *
 * Essentially returns the CPU address of the raw equivalent of @table, cast to
 * a &struct pvr_page_table_l0_raw pointer.
 *
 * You probably want to call pvr_page_table_l0_get_entry_raw() instead.
 *
 * Return:
 * The raw equivalent of @table.
 */
static __always_inline struct pvr_page_table_l0_raw *
pvr_page_table_l0_get_raw(struct pvr_page_table_l0 *table)
{
	return table->backing_page.host_ptr;
}

/**
 * pvr_page_table_l0_get_entry_raw() - Access an entry from the raw equivalent
 *                                     of a mirror level 0 page table.
 * @table: Target level 0 page table.
 * @idx: Index of the entry to access.
 *
 * Technically this function returns a pointer to a slot in a raw level 0 page
 * table, since the returned "entry" is not guaranteed to be valid. The caller
 * must verify the validity of the entry at the returned address (perhaps using
 * pvr_page_table_l0_entry_raw_is_valid()) before reading or overwriting it.
 *
 * The value of @idx is not checked here; it is the callers responsibility to
 * ensure @idx refers to a valid index within @table before dereferencing the
 * returned pointer. This is espcially important for level 0 page tables, which
 * can have a variable number of entries.
 *
 * Return:
 * A pointer to the requested raw level 0 page table entry.
 */
static __always_inline struct pvr_page_table_l0_entry_raw *
pvr_page_table_l0_get_entry_raw(struct pvr_page_table_l0 *table, u16 idx)
{
	return &pvr_page_table_l0_get_raw(table)->entries[idx];
}

/**
 * pvr_page_table_l0_entry_is_valid() - Check if a level 0 page table entry is
 *                                      marked as valid.
 * @table: Target level 0 page table.
 * @idx: Index of the entry to check.
 *
 * The value of @idx is not checked here; it is the callers responsibility to
 * ensure @idx refers to a valid index within @table before calling this
 * function.
 */
static __always_inline bool
pvr_page_table_l0_entry_is_valid(struct pvr_page_table_l0 *table, u16 idx)
{
	struct pvr_page_table_l0_entry_raw entry_raw =
		*pvr_page_table_l0_get_entry_raw(table, idx);

	return pvr_page_table_l0_entry_raw_is_valid(entry_raw);
}

/**
 * pvr_page_table_l2_insert() - Insert an entry referring to a level 1 page
 *                              table into a level 2 page table.
 * @ptr: Page table pointer pointing to the entry to insert the L1 page table into.
 * @child_table: Target level 1 page table to be referenced by the new entry.
 *
 * The values of @ptr are not checked here; it is the callers responsibility to
 * ensure @ptr points to a valid L2 entry.
 *
 * This function is unchecked. Do not call it unless you're absolutely sure
 * there is not already a valid entry at @ptr.
 */
static void
pvr_page_table_l2_insert(struct pvr_page_table_ptr *ptr,
			 struct pvr_page_table_l1 *child_table)
{
	struct pvr_page_table_l2_entry_raw *entry_raw =
		pvr_page_table_l2_get_entry_raw(ptr->l2_table, ptr->l2_idx);

	pvr_page_table_l2_entry_raw_set(entry_raw,
					child_table->backing_page.dma_addr);

	child_table->parent = ptr->l2_table;
	child_table->parent_idx = ptr->l2_idx;
	ptr->l2_table->entries[ptr->l2_idx] = child_table;
	++ptr->l2_table->entry_count;
	ptr->l1_table = child_table;
}

/**
 * pvr_page_table_l2_remove() - Remove a level 1 page table from a level 2 page
 *                              table.
 * @ptr: Page table pointer pointing to the L2 entry to remove.
 *
 * The values of @ptr are not checked here; it is the callers responsibility to
 * ensure @ptr points to a valid L2 entry before calling this function.
 *
 * This function is unchecked. Do not call it unless you're absolutely sure
 * there is a valid entry pointed by @ptr. It is **not** a no-op to call this
 * function twice, and subsequent calls **will** place @table into an invalid
 * state.
 */
static void
pvr_page_table_l2_remove(struct pvr_page_table_ptr *ptr)
{
	struct pvr_page_table_l2_entry_raw *entry_raw =
		pvr_page_table_l2_get_entry_raw(ptr->l2_table, ptr->l1_table->parent_idx);

	WARN_ON(ptr->l1_table->parent != ptr->l2_table);

	pvr_page_table_l2_entry_raw_clear(entry_raw);

	ptr->l2_table->entries[ptr->l1_table->parent_idx] = NULL;
	ptr->l1_table->parent_idx = PVR_IDX_INVALID;
	ptr->l1_table->next_free = ptr->l1_free_list;
	ptr->l1_free_list = ptr->l1_table;
	ptr->l1_table = NULL;

	--ptr->l2_table->entry_count;
}

/**
 * pvr_page_table_l1_insert() - Insert an entry referring to a level 0 page
 *                              table into a level 1 page table.
 * @ptr: Page table pointer pointing to the entry to insert the L0 page table into.
 * @child_table: L0 page table to insert.
 *
 * The value of @ptr is not checked here; it is the callers responsibility to
 * ensure @ptr points to valid L1 entry before calling this function.
 *
 * This function is unchecked. Do not call it unless you're absolutely sure
 * there is not already a valid entry at @ptr.
 */
static void
pvr_page_table_l1_insert(struct pvr_page_table_ptr *ptr,
			 struct pvr_page_table_l0 *child_table)
{
	struct pvr_page_table_l1_entry_raw *entry_raw =
		pvr_page_table_l1_get_entry_raw(ptr->l1_table, ptr->l1_idx);

	pvr_page_table_l1_entry_raw_set(entry_raw,
					child_table->backing_page.dma_addr);

	child_table->parent = ptr->l1_table;
	child_table->parent_idx = ptr->l1_idx;
	ptr->l1_table->entries[ptr->l1_idx] = child_table;
	++ptr->l1_table->entry_count;
	ptr->l0_table = child_table;
}

/**
 * pvr_page_table_l1_remove() - Remove a level 0 page table from a level 1 page
 *                              table.
 * @ptr: Page table pointer pointing to the L1 entry to remove.
 *
 * If this function results in the L1 table becoming empty, it will be removed
 * from its parent level 2 page table and destroyed.
 *
 * The values of @ptr are not checked here; it is the callers responsibility to
 * ensure @ptr points to a valid L1 entry before calling this function.
 *
 * This function is unchecked. Do not call it unless you're absolutely sure
 * there is a valid entry pointed by @ptr. It is **not** a no-op to call this
 * function twice, and subsequent calls **will** place @table into an invalid
 * state.
 */
static void
pvr_page_table_l1_remove(struct pvr_page_table_ptr *ptr)
{
	struct pvr_page_table_l1_entry_raw *entry_raw =
		pvr_page_table_l1_get_entry_raw(ptr->l0_table->parent,
						ptr->l0_table->parent_idx);

	WARN_ON(ptr->l0_table->parent != ptr->l1_table);

	pvr_page_table_l1_entry_raw_clear(entry_raw);

	ptr->l1_table->entries[ptr->l0_table->parent_idx] = NULL;
	ptr->l0_table->parent_idx = PVR_IDX_INVALID;
	ptr->l0_table->next_free = ptr->l0_free_list;
	ptr->l0_free_list = ptr->l0_table;
	ptr->l0_table = NULL;

	if (--ptr->l1_table->entry_count == 0) {
		/* Clear the parent L2 page table entry. */
		if (ptr->l1_table->parent_idx != PVR_IDX_INVALID)
			pvr_page_table_l2_remove(ptr);
	}
}

/**
 * pvr_page_table_l0_insert() - Insert an entry referring to a physical page
 *                              into a level 0 page table.
 * @ptr: Page table pointer pointing to the L0 entry to insert.
 * @dma_addr: Target DMA address to be referenced by the new entry.
 * @flags: Page options to be stored in the new entry.
 *
 * The values of @ptr are not checked here; it is the callers responsibility to
 * ensure @ptr points to a valid L0 entry before calling this function.
 *
 * This function is unchecked. Do not call it unless you're absolutely sure
 * there is not already a valid entry in the L0 table @ptr points to.
 */
static void
pvr_page_table_l0_insert(struct pvr_page_table_ptr *ptr,
			 dma_addr_t dma_addr, struct pvr_page_flags_raw flags)
{
	struct pvr_page_table_l0_entry_raw *entry_raw =
		pvr_page_table_l0_get_entry_raw(ptr->l0_table, ptr->l0_idx);

	pvr_page_table_l0_entry_raw_set(entry_raw, dma_addr, flags);

	/*
	 * There is no entry to set here - we don't keep a mirror of
	 * individual pages.
	 */

	++ptr->l0_table->entry_count;
}

/**
 * pvr_page_table_l0_remove() - Remove a physical page from a level 0 page
 *                              table.
 * @ptr: Page table pointer pointing to the L0 entry to remove.
 *
 * If this function results in the L0 table becoming empty, it will be removed
 * from its parent L1 page table and destroyed.
 *
 * The values of @ptr are not checked here; it is the callers responsibility to
 * ensure @ptr points to a valid L0 entry before calling this function.
 *
 * This function is unchecked. Do not call it unless you're absolutely sure
 * there is a valid entry pointed by @ptr. It is **not** a no-op to call this
 * function twice, and subsequent calls **will** place @table into an invalid
 * state.
 */
static void
pvr_page_table_l0_remove(struct pvr_page_table_ptr *ptr)
{
	struct pvr_page_table_l0_entry_raw *entry_raw =
		pvr_page_table_l0_get_entry_raw(ptr->l0_table, ptr->l0_idx);

	pvr_page_table_l0_entry_raw_clear(entry_raw);

	/*
	 * There is no entry to clear here - we don't keep a mirror of
	 * individual pages.
	 */

	if (--ptr->l0_table->entry_count == 0) {
		/* Clear the parent L1 page table entry. */
		if (ptr->l0_table->parent_idx != PVR_IDX_INVALID)
			pvr_page_table_l1_remove(ptr);
	}
}

/**
 * DOC: Page table index utilities
 */
/**
 * DOC: Page table index utilities (constants)
 *
 * .. c:macro:: PVR_PAGE_TABLE_ADDR_SPACE_SIZE
 *
 *    Size of device-virtual address space which can be represented in the page
 *    table structure.
 *
 *    This value is checked at runtime against
 *    &pvr_device_features.virtual_address_space_bits by
 *    pvr_vm_create_context(), which will return an error if the feature value
 *    does not match this constant.
 *
 *    .. admonition:: Future work
 *
 *       It should be possible to support other values of
 *       &pvr_device_features.virtual_address_space_bits, but so far no
 *       hardware has been created which advertises an unsupported value.
 *
 * .. c:macro:: PVR_PAGE_TABLE_ADDR_BITS
 *
 *    Number of bits needed to represent any value less than
 *    %PVR_PAGE_TABLE_ADDR_SPACE_SIZE exactly.
 *
 * .. c:macro:: PVR_PAGE_TABLE_ADDR_MASK
 *
 *    Bitmask of device-virtual addresses which are valid in the page table
 *    structure.
 *
 *    This value is derived from %PVR_PAGE_TABLE_ADDR_SPACE_SIZE, so the same
 *    notes on that constant apply here.
 */
#define PVR_PAGE_TABLE_ADDR_SPACE_SIZE SZ_1T
#define PVR_PAGE_TABLE_ADDR_BITS __ffs(PVR_PAGE_TABLE_ADDR_SPACE_SIZE)
#define PVR_PAGE_TABLE_ADDR_MASK (PVR_PAGE_TABLE_ADDR_SPACE_SIZE - 1)

/**
 * pvr_page_table_l2_idx() - Calculate the level 2 page table index for a
 *                           device-virtual address.
 * @device_addr: Target device-virtual address.
 *
 * This function does not perform any bounds checking - it is the caller's
 * responsibility to ensure that @device_addr is valid before interpreting
 * the result.
 *
 * Return:
 * The index into a level 2 page table corresponding to @device_addr.
 */
static __always_inline u16
pvr_page_table_l2_idx(u64 device_addr)
{
	return (device_addr & ~ROGUE_MMUCTRL_VADDR_PC_INDEX_CLRMSK) >>
	       ROGUE_MMUCTRL_VADDR_PC_INDEX_SHIFT;
}

/**
 * pvr_page_table_l1_idx() - Calculate the level 1 page table index for a
 *                           device-virtual address.
 * @device_addr: Target device-virtual address.
 *
 * This function does not perform any bounds checking - it is the caller's
 * responsibility to ensure that @device_addr is valid before interpreting
 * the result.
 *
 * Return:
 * The index into a level 1 page table corresponding to @device_addr.
 */
static __always_inline u16
pvr_page_table_l1_idx(u64 device_addr)
{
	return (device_addr & ~ROGUE_MMUCTRL_VADDR_PD_INDEX_CLRMSK) >>
	       ROGUE_MMUCTRL_VADDR_PD_INDEX_SHIFT;
}

/**
 * pvr_page_table_l0_idx() - Calculate the level 0 page table index for a
 *                           device-virtual address.
 * @device_addr: Target device-virtual address.
 *
 * This function does not perform any bounds checking - it is the caller's
 * responsibility to ensure that @device_addr is valid before interpreting
 * the result.
 *
 * Return:
 * The index into a level 0 page table corresponding to @device_addr.
 */
static __always_inline u16
pvr_page_table_l0_idx(u64 device_addr)
{
	return (device_addr & ~ROGUE_MMUCTRL_VADDR_PT_INDEX_CLRMSK) >>
	       ROGUE_MMUCTRL_PAGE_X_RANGE_SHIFT;
}

/**
 * DOC: High-level page table operations
 */

/**
 * pvr_page_table_l1_create_unchecked() - Create a level 1 page table and
 *                                        insert it into a level 2 page table.
 * @ptr: Page table pointer pointing to the entry to insert the L1 page table into.
 *
 * This function is unchecked. By using it, the caller is asserting that @ptr
 * points to a valid L2 slot, and that this slot does not contain a valid entry.
 *
 * Return:
 *  * 0 on success,
 *  * -%ENOMEM if allocation of a &struct pvr_page_table_l1 fails, or
 *  * Any error encountered while initializing the new level 1 page table with
 *    pvr_page_table_l1_init().
 */
static int
pvr_page_table_l1_create_unchecked(struct pvr_page_table_ptr *ptr)
{
	struct pvr_page_table_l1 *table;
	int err;

	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return -ENOMEM;

	err = pvr_page_table_l1_init(table, ptr->pvr_dev);
	if (err)
		goto err_free_table;

	pvr_page_table_l2_insert(ptr, table);
	return 0;

err_free_table:
	kfree(table);
	return err;
}

/**
 * pvr_page_table_l1_get_or_create() - Retrieves (optionally creating if
 *                                     necessary) a level 1 page table from the
 *                                     specified level 2 page table entry.
 * @ptr: [IN] Page table pointer.
 * @should_create: [IN] Specifies whether new page tables should be created
 *                 when empty page table entries are encountered during
 *                 traversal.
 * @did_create: [OUT] Optional pointer to a flag which is set when
 *              @should_create is %true and new page table entries are created.
 *              In any other case, the value will not be modified.
 *
 * Return:
 *  * 0 on success, or
 *
 *    If @should_create is %false:
 *     * -%ENXIO if a level 1 page table would have been created.
 *
 *    If @should_create is %true:
 *     * Any error encountered while creating the level 1 page table with
 *       pvr_page_table_l1_create_unchecked() if one needs to be created.
 */
static int
pvr_page_table_l1_get_or_create(struct pvr_page_table_ptr *ptr,
				bool should_create, bool *did_create)
{
	int err;

	if (pvr_page_table_l2_entry_is_valid(ptr->l2_table, ptr->l2_idx)) {
		ptr->l1_table = ptr->l2_table->entries[ptr->l2_idx];
		return 0;
	}

	if (!should_create)
		return -ENXIO;

	/* Safe, because we just verified the entry does not exist yet. */
	err = pvr_page_table_l1_create_unchecked(ptr);
	if (!err && did_create)
		*did_create = true;

	return err;
}

/**
 * pvr_page_table_l0_create_unchecked() - Create a level 0 page table and
 *                                        insert it into a level 1 page table.
 * @ptr: Page table pointer pointing to the L1 entry to insert the L0 page table into.
 *
 * This function is unchecked. By using it, the caller is asserting that @ptr
 * points to a valid L1 slot, and that slot does not contain a valid entry.
 *
 * Return:
 *  * 0 on success,
 *  * -%ENOMEM if allocation of a &struct pvr_page_table_l0 fails, or
 *  * Any error encountered while initializing the new level 0 page table with
 *    pvr_page_table_l1_init().
 */
static int
pvr_page_table_l0_create_unchecked(struct pvr_page_table_ptr *ptr)
{
	struct pvr_page_table_l0 *table;
	int err;

	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return -ENOMEM;

	err = pvr_page_table_l0_init(table, ptr->pvr_dev);
	if (err)
		goto err_free_table;

	pvr_page_table_l1_insert(ptr, table);
	return 0;

err_free_table:
	kfree(table);
	return err;
}

/**
 * pvr_page_table_l0_get_or_create() - Retrieves (optionally creating if
 *                                     necessary) a level 0 page table from the
 *                                     specified level 1 page table entry.
 * @ptr: [IN] Page table pointer.
 * @should_create: [IN] Specifies whether new page tables should be created
 *                 when empty page table entries are encountered during
 *                 traversal.
 * @did_create: [OUT] Optional pointer to a flag which is set when
 *              @should_create is %true and new page table entries are created.
 *              In any other case, the value will not be modified.
 *
 * Return:
 *  * 0 on success,
 *  * -%ENXIO if @should_create is %false and a level 0 page table would have
 *    been created, or
 *  * Any error returned by pvr_page_table_l1_create_unchecked() if
 *    @should_create is %true and a new level 0 page table needs to be created.
 */
static int
pvr_page_table_l0_get_or_create(struct pvr_page_table_ptr *ptr,
				bool should_create, bool *did_create)
{
	int err;

	if (pvr_page_table_l1_entry_is_valid(ptr->l1_table, ptr->l1_idx)) {
		ptr->l0_table = ptr->l1_table->entries[ptr->l1_idx];
		return 0;
	}

	if (!should_create)
		return -ENXIO;

	/* Safe, because we just verified the entry does not exist yet. */
	err = pvr_page_table_l0_create_unchecked(ptr);
	if (!err && did_create)
		*did_create = true;

	return err;
}

/**
 * DOC: Page table pointer
 */
/**
 * DOC: Page table pointer (constants)
 *
 * .. c:macro:: PVR_PAGE_TABLE_PTR_IN_SYNC
 *
 *    Negative value to indicate that a page table pointer is fully in sync
 *    when assigned to &pvr_page_table_ptr->sync_level_required.
 */
#define PVR_PAGE_TABLE_PTR_IN_SYNC ((s8)(-1))

/**
 * pvr_page_table_ptr_require_sync() - Mark a page table pointer as requiring a
 *                                     sync operation for the referenced page
 *                                     tables up to a specified level.
 * @ptr: Target page table pointer.
 * @level: Maximum page table level for which a sync is required.
 */
static __always_inline void
pvr_page_table_ptr_require_sync(struct pvr_page_table_ptr *ptr, s8 level)
{
	if (ptr->sync_level_required < level)
		ptr->sync_level_required = level;
}

/**
 * pvr_page_table_ptr_sync_manual() - Trigger a sync of some or all of the
 *                                    page tables referenced by a page table
 *                                    pointer.
 * @ptr: Target page table pointer.
 * @level: Maximum page table level to sync.
 *
 * Do not call this function directly. Instead use
 * pvr_page_table_ptr_sync_partial() which is checked against the current
 * value of &ptr->sync_level_required as set by
 * pvr_page_table_ptr_require_sync().
 */
static void
pvr_page_table_ptr_sync_manual(struct pvr_page_table_ptr *ptr, s8 level)
{
	/*
	 * We sync the page table levels in ascending order (starting from the
	 * leaf node) to ensure consistency.
	 */

	if (level < 0)
		return;

	if (ptr->l0_table)
		pvr_page_table_l0_sync(ptr->l0_table);

	if (level < 1)
		return;

	if (ptr->l1_table)
		pvr_page_table_l1_sync(ptr->l1_table);

	if (level < 2)
		return;

	pvr_page_table_l2_sync(ptr->l2_table);
}

/**
 * pvr_page_table_ptr_sync_partial() - Trigger a sync of some or all of the
 *                                     page tables referenced by a page table
 *                                     pointer.
 * @ptr: Target page table pointer.
 * @level: Requested page table level to sync up to (inclusive).
 *
 * If @level is greater than the maximum level recorded by @ptr as requiring
 * a sync operation, only the previously recorded maximum will be used.
 *
 * Additionally, if @level is greater than or equal to the maximum level
 * recorded by @ptr as requiring a sync operation, that maximum level will be
 * reset as a full sync will be performed. This is equivalent to calling
 * pvr_page_table_ptr_sync().
 */
static void
pvr_page_table_ptr_sync_partial(struct pvr_page_table_ptr *ptr, s8 level)
{
	/*
	 * If the requested sync level is greater than or equal to the
	 * currently required sync level, we do two things:
	 *  * Don't waste time syncing levels we haven't previously marked as
	 *    requiring a sync, and
	 *  * Reset the required sync level since we are about to sync
	 *    everything that was previously marked as requiring a sync.
	 */
	if (level >= ptr->sync_level_required) {
		level = ptr->sync_level_required;
		ptr->sync_level_required = PVR_PAGE_TABLE_PTR_IN_SYNC;
	}

	pvr_page_table_ptr_sync_manual(ptr, level);
}

/**
 * pvr_page_table_ptr_sync() - Trigger a sync of every page table referenced by
 *                             a page table pointer.
 * @ptr: Target page table pointer.
 *
 * The maximum level marked internally as requiring a sync will be reset so
 * that subsequent calls to this function will be no-ops unless @ptr is
 * otherwise updated.
 */
static __always_inline void
pvr_page_table_ptr_sync(struct pvr_page_table_ptr *ptr)
{
	pvr_page_table_ptr_sync_manual(ptr, ptr->sync_level_required);

	ptr->sync_level_required = PVR_PAGE_TABLE_PTR_IN_SYNC;
}

/**
 * pvr_page_table_ptr_load_tables() - Load pointers to tables in each level of
 *                                    the page table tree structure needed to
 *                                    reference the physical page referenced by
 *                                    a page table pointer.
 * @ptr: Target page table pointer.
 * @should_create: Specifies whether new page tables should be created when
 *                 empty page table entries are encountered during traversal.
 * @load_level_required: Maximum page table level to load.
 *
 * If @should_create is %true, this function may modify the stored required
 * sync level of @ptr as new page tables are created and inserted into their
 * respective parents.
 *
 * Since there is only one root page table, it is technically incorrect to call
 * this function with a value of @load_level_required greater than or equal to
 * the root level number. However, this is not explicitly disallowed here.
 *
 * Return:
 *  * 0 on success,
 *  * Any error returned by pvr_page_table_l1_get_or_create() if
 *    @load_level_required >= 1 except -%ENXIO, or
 *  * Any error returned by pvr_page_table_l0_get_or_create() if
 *    @load_level_required >= 0 except -%ENXIO.
 */
static int
pvr_page_table_ptr_load_tables(struct pvr_page_table_ptr *ptr,
			       bool should_create, s8 load_level_required)
{
	bool did_create_l1 = false;
	bool did_create_l0 = false;
	int err;

	/* Clear tables we're about to fetch in case of error states. */
	if (load_level_required >= 1)
		ptr->l1_table = NULL;

	if (load_level_required >= 0)
		ptr->l0_table = NULL;

	/* Get or create L1 page table. */
	if (load_level_required >= 1) {
		err = pvr_page_table_l1_get_or_create(ptr, should_create, &did_create_l1);
		if (err) {
			/*
			 * If @should_create is %false and no L1 page table was
			 * found, return early but without an error. Since
			 * pvr_page_table_l1_get_or_create() can only return
			 * -%ENXIO if @should_create is %false, there is no
			 * need to check it here.
			 */
			if (err == -ENXIO)
				err = 0;

			goto err_out;
		}
	}

	/* Get or create L0 page table. */
	if (load_level_required >= 0) {
		err = pvr_page_table_l0_get_or_create(ptr, should_create, &did_create_l0);
		if (err) {
			/*
			 * If @should_create is %false and no L0 page table was
			 * found, return early but without an error. Since
			 * pvr_page_table_l0_get_or_create() can only return
			 * -%ENXIO if @should_create is %false, there is no
			 * need to check it here.
			 */
			if (err == -ENXIO)
				err = 0;

			/*
			 * At this point, an L1 page table could have been
			 * created but is now empty due to the failed attempt
			 * at creating an L0 page table. In this instance, we
			 * must remove the empty L1 page table ourselves as
			 * pvr_page_table_l1_remove() is never called as part
			 * of the error path in
			 * pvr_page_table_l0_get_or_create().
			 */
			if (did_create_l1) {
				pvr_page_table_l2_remove(ptr);
				pvr_page_table_ptr_require_sync(ptr, 2);
			}

			goto err_out;
		}
	}

	if (did_create_l1)
		pvr_page_table_ptr_require_sync(ptr, 2);
	else if (did_create_l0)
		pvr_page_table_ptr_require_sync(ptr, 1);

	return 0;

err_out:
	return err;
}

/**
 * pvr_page_table_ptr_set() - Reassign a page table pointer, syncing any
 *                            page tables previously assigned to it which are
 *                            no longer relevant.
 * @ptr: Target page table pointer.
 * @device_addr: New pointer target.
 * @should_create: Specify whether new page tables should be created when
 *                 empty page table entries are encountered during traversal.
 *
 * This function performs a full sync on the pointer, regardless of which
 * levels are modified.
 *
 * Return:
 *  * 0 on success, or
 *  * Any error returned by pvr_page_table_ptr_load_tables().
 */
static int
pvr_page_table_ptr_set(struct pvr_page_table_ptr *ptr, u64 device_addr,
		       bool should_create)
{
	pvr_page_table_ptr_sync(ptr);

	ptr->l2_idx = pvr_page_table_l2_idx(device_addr);
	ptr->l1_idx = pvr_page_table_l1_idx(device_addr);
	ptr->l0_idx = pvr_page_table_l0_idx(device_addr);

	return pvr_page_table_ptr_load_tables(ptr, should_create, 1);
}

/**
 * pvr_page_table_ptr_init() - Initialize a page table pointer.
 * @ptr: Target page table pointer.
 * @pvr_dev: Target PowerVR device.
 * @root_table: Root of the target page table tree structure.
 * @device_addr: Pointer target.
 * @should_create: Specify whether new page tables should be created when
 *                 empty page table entries are encountered during traversal.
 *
 * This function zeroes @ptr; it must not be a valid page table pointer when it
 * is called.
 *
 * Return:
 *  * 0 on success, or
 *  * Any error returned by pvr_page_table_ptr_set().
 */
static int
pvr_page_table_ptr_init(struct pvr_page_table_ptr *ptr,
			struct pvr_device *pvr_dev,
			struct pvr_page_table_l2 *root_table, u64 device_addr,
			bool should_create)
{
	memset(ptr, 0, sizeof(*ptr));

	ptr->pvr_dev = pvr_dev;
	ptr->l2_table = root_table;
	ptr->sync_level_required = PVR_PAGE_TABLE_PTR_IN_SYNC;

	return pvr_page_table_ptr_set(ptr, device_addr, should_create);
}

/**
 * pvr_page_table_ptr_fini() - Teardown a page table pointer.
 * @ptr: Target page table pointer.
 */
static void
pvr_page_table_ptr_fini(struct pvr_page_table_ptr *ptr)
{
	bool flush_caches = ptr->sync_level_required != PVR_PAGE_TABLE_PTR_IN_SYNC;

	if (WARN_ON(!flush_caches && (ptr->l0_free_list || ptr->l1_free_list)))
		flush_caches = true;

	pvr_page_table_ptr_sync(ptr);

	if (flush_caches)
		WARN_ON(pvr_vm_mmu_flush(ptr->pvr_dev));

	while (ptr->l0_free_list) {
		struct pvr_page_table_l0 *l0 = ptr->l0_free_list;

		ptr->l0_free_list = l0->next_free;
		pvr_page_table_l0_fini(l0);
		kfree(l0);
	}

	while (ptr->l1_free_list) {
		struct pvr_page_table_l1 *l1 = ptr->l1_free_list;

		ptr->l1_free_list = l1->next_free;
		pvr_page_table_l1_fini(l1);
		kfree(l1);
	}
}

/**
 * pvr_page_table_ptr_next_page() - Advance a page table pointer.
 * @ptr: Target page table pointer.
 * @should_create: Specify whether new page tables should be created when
 *                 empty page table entries are encountered during traversal.
 *
 * If @should_create is %false, it is the caller's responsibility to verify that
 * the state of the table references in @ptr is valid on return. If -%ENXIO is
 * returned, at least one of the table references is invalid. It should be
 * noted that @ptr as a whole will be left in a valid state if -%ENXIO is
 * returned, unlike other error codes. The caller should check which references
 * are invalid by comparing them to %NULL. Only &@ptr->l2_table is guaranteed
 * to be valid, since it represents the root of the page table tree structure.
 *
 * Return:
 *  * 0 on success,
 *  * -%EPERM if the operation would wrap at the top of the page table
 *    hierarchy,
 *  * -%ENXIO if @should_create is %false and a page table of any level would
 *    have otherwise been created, or
 *  * Any error returned while attempting to create missing page tables if
 *    @should_create is %true.
 */
static int
pvr_page_table_ptr_next_page(struct pvr_page_table_ptr *ptr, bool should_create)
{
	s8 load_level_required = PVR_PAGE_TABLE_PTR_IN_SYNC;

	if (++ptr->l0_idx != ROGUE_MMUCTRL_ENTRIES_PT_VALUE_X)
		goto load_tables;

	ptr->l0_idx = 0;
	load_level_required = 0;

	if (++ptr->l1_idx != ROGUE_MMUCTRL_ENTRIES_PD_VALUE)
		goto load_tables;

	ptr->l1_idx = 0;
	load_level_required = 1;

	if (++ptr->l2_idx != ROGUE_MMUCTRL_ENTRIES_PC_VALUE)
		goto load_tables;

	/*
	 * If the pattern continued, we would set &ptr->l2_idx to zero here.
	 * However, that would wrap the top layer of the page table hierarchy
	 * which is not a valid operation. Instead, we warn and return an
	 * error.
	 */
	WARN(true,
	     "%s(%p) attempted to loop the top of the page table hierarchy",
	     __func__, ptr);
	return -EPERM;

	/* If indices have wrapped, we need to load new tables. */
load_tables:
	/* First, flush tables which will be unloaded. */
	pvr_page_table_ptr_sync_partial(ptr, load_level_required);

	/* Then load tables from the required level down. */
	return pvr_page_table_ptr_load_tables(ptr, should_create,
					      load_level_required);
}

/**
 * pvr_page_table_ptr_copy() - Duplicate a page table pointer.
 * @dst: [OUT] New page table pointer.
 * @src: [IN] Original page table pointer.
 *
 * The pointer at @dst will be marked as "synced" so that any sync operations
 * required on @src are not duplicated.
 */
static void
pvr_page_table_ptr_copy(struct pvr_page_table_ptr *dst,
			const struct pvr_page_table_ptr *src)
{
	memcpy(dst, src, sizeof(*dst));

	/*
	 * Nothing currently in the clone requires a sync later on, since the
	 * original will handle it either when advancing or during teardown.
	 */
	dst->sync_level_required = PVR_PAGE_TABLE_PTR_IN_SYNC;

	/* The clone starts with an empty list of L1/L0 tables to free. */
	dst->l1_free_list = NULL;
	dst->l0_free_list = NULL;
}

/**
 * DOC: Single page operations
 */

/**
 * pvr_page_create() - Create a device-virtual memory page and insert it into
 * a level 0 page table.
 * @ptr: Page table pointer to the device-virtual address of the target page.
 * @dma_addr: DMA address of the physical page backing the created page.
 * @flags: Page options saved on the level 0 page table entry for reading by
 *         the device.
 *
 * Return:
 *  * 0 on success, or
 *  * -%EEXIST if the requested page already exists.
 */
static int
pvr_page_create(struct pvr_page_table_ptr *ptr, dma_addr_t dma_addr,
		struct pvr_page_flags_raw flags)
{
	/* Do not create a new page if one already exists. */
	if (pvr_page_table_l0_entry_is_valid(ptr->l0_table, ptr->l0_idx))
		return -EEXIST;

	pvr_page_table_l0_insert(ptr, dma_addr, flags);

	pvr_page_table_ptr_require_sync(ptr, 0);

	return 0;
}

/**
 * pvr_page_destroy() - Destroy a device page after removing it from its
 *                      parent level 0 page table.
 * @ptr: Page table pointer to the device-virtual address of the target page.
 */
static void
pvr_page_destroy(struct pvr_page_table_ptr *ptr)
{
	/* Do nothing if the page does not exist. */
	if (!pvr_page_table_l0_entry_is_valid(ptr->l0_table, ptr->l0_idx))
		return;

	/* Clear the parent L0 page table entry. */
	pvr_page_table_l0_remove(ptr);

	pvr_page_table_ptr_require_sync(ptr, 0);
}

/**
 * DOC: Memory context
 *
 * This is the "top level" datatype in the VM code. It's exposed in the public
 * API as an opaque handle.
 */

/**
 * struct pvr_vm_context - Context type which encapsulates an entire page table
 * tree structure.
 * @pvr_dev: The PowerVR device to which this context is bound.
 *
 * This binding is immutable for the life of the context.
 * @root_table: The root of the page table tree structure.
 *
 * This embedded struct is our "mirror" version of the top level
 * page table. By definition, there can only be one of these. The
 * device requires this top level table to always exist, so there
 * is no need for it to be a pointer here.
 * @gpuva_mgr: GPUVA manager object associated with this context.
 * @lock: Global lock on this entire structure of page tables.
 * @fw_mem_ctx_obj: Firmware object representing firmware memory context.
 * @ref_count: Reference count of object.
 */
struct pvr_vm_context {
	struct pvr_device *pvr_dev;
	struct pvr_page_table_l2 root_table;
	struct drm_gpuva_manager gpuva_mgr;
	struct mutex lock;
	struct pvr_fw_object *fw_mem_ctx_obj;
	struct kref ref_count;
};

/**
 * pvr_vm_get_page_table_root_addr() - Get the DMA address of the root of the
 *                                     page table structure behind a VM context.
 * @vm_ctx: Target VM context.
 */
dma_addr_t pvr_vm_get_page_table_root_addr(struct pvr_vm_context *vm_ctx)
{
	return vm_ctx->root_table.backing_page.dma_addr;
}

/**
 * pvr_vm_context_unmap_from_ptr() - Unmap pages from a memory context starting
 *                                   from the entry addressed by a page table
 *                                   pointer.
 * @ptr: Page table pointer to the first page to unmap.
 * @nr_pages: Number of pages to unmap.
 *
 * Return:
 *  * 0 on success, or
 *  * Any error encountered while advancing @ptr with
 *    pvr_page_table_ptr_next_page() (except -%ENXIO).
 */
static int
pvr_vm_context_unmap_from_ptr(struct pvr_page_table_ptr *ptr, u64 nr_pages)
{
	u64 page;
	int err;

	if (nr_pages == 0)
		return 0;

	/*
	 * Destroy first page outside loop, as it doesn't require a pointer
	 * increment beforehand. If the L0 page table reference in @ptr is
	 * %NULL, there cannot be a mapped page at @ptr (so skip ahead).
	 */
	if (ptr->l0_table)
		pvr_page_destroy(ptr);

	for (page = 1; page < nr_pages; ++page) {
		err = pvr_page_table_ptr_next_page(ptr, false);
		/*
		 * If the page table tree structure at @ptr is incomplete,
		 * skip ahead. We don't care about unmapping pages that
		 * cannot exist.
		 *
		 * FIXME: This could be made more efficient by jumping ahead
		 * using pvr_page_table_ptr_set().
		 */
		if (err == -ENXIO)
			continue;

		if (err)
			goto err_out;

		pvr_page_destroy(ptr);
	}

	return 0;

err_out:
	return err;
}

/**
 * pvr_vm_context_unmap() - Unmap pages from a memory context.
 * @vm_ctx: Target memory context.
 * @device_addr: First device-virtual address to unmap.
 * @nr_pages: Number of pages to unmap.
 *
 * The total amount of device-virtual memory unmapped by pvr_vm_context_unmap()
 * is @nr_pages * %PVR_DEVICE_PAGE_SIZE.
 */
static int
pvr_vm_context_unmap(struct pvr_vm_context *vm_ctx, u64 device_addr,
		     u64 nr_pages)
{
	struct pvr_page_table_ptr ptr;
	int err;

	err = pvr_page_table_ptr_init(&ptr, vm_ctx->pvr_dev,
				      &vm_ctx->root_table, device_addr, false);
	if (err && err != -ENXIO)
		goto err_out;

	err = pvr_vm_context_unmap_from_ptr(&ptr, nr_pages);
	if (err)
		goto err_ptr_fini;

	err = 0;
	goto err_ptr_fini;

err_ptr_fini:
	pvr_page_table_ptr_fini(&ptr);

err_out:
	return err;
}

/**
 * pvr_vm_context_map_direct() - Map pages to a memory context starting from
 *                               the entry addressed by a page table pointer.
 * @vm_ctx: Target memory context.
 * @dma_addr: Initial DMA address of the memory to be mapped.
 * @size: Size of mapping.
 * @ptr: Page table pointer to the first page to map.
 * @flags: Flags to be set on every device-virtual page created.
 *
 * Return:
 *  * 0 on success,
 *  * Any error encountered while creating a page with pvr_page_create(), or
 *  * Any error encountered while advancing @ptr with
 *    pvr_page_table_ptr_next_page().
 */
static int
pvr_vm_context_map_direct(struct pvr_vm_context *vm_ctx, dma_addr_t dma_addr,
			  u64 size, struct pvr_page_table_ptr *ptr,
			  struct pvr_page_flags_raw flags)
{
	unsigned int pages = size >> PVR_DEVICE_PAGE_SHIFT;
	unsigned int page;

	struct pvr_page_table_ptr ptr_copy;

	int err;

	/*
	 * Before progressing, save a copy of the start pointer so we can use
	 * it again if we enter an error state and have to destroy pages.
	 */
	pvr_page_table_ptr_copy(&ptr_copy, ptr);

	/*
	 * Create first page outside loop, as it doesn't require a pointer
	 * increment beforehand.
	 */
	err = pvr_page_create(ptr, dma_addr, flags);
	if (err)
		goto err_fini_ptr_copy;

	for (page = 1; page < pages; ++page) {
		err = pvr_page_table_ptr_next_page(ptr, true);
		if (err)
			goto err_destroy_pages;

		dma_addr += PVR_DEVICE_PAGE_SIZE;

		err = pvr_page_create(ptr, dma_addr, flags);
		if (err)
			goto err_destroy_pages;
	}

	err = 0;
	goto err_fini_ptr_copy;

err_destroy_pages:
	err = pvr_vm_context_unmap_from_ptr(&ptr_copy, page);

err_fini_ptr_copy:
	pvr_page_table_ptr_fini(&ptr_copy);

	return err;
}

/**
 * pvr_vm_context_map_sgl() - Map part of a scatter-gather table entry to device-virtual memory.
 *
 * @vm_ctx: Target VM context.
 * @sgl: Target scatter-gather table entry.
 * @offset: Offset into @sgl to map from. Must result in a starting address
 *          from @sgl which is CPU page-aligned.
 * @size: Size of the memory to be mapped in bytes. Must be a non-zero multiple
 *        of the device page size.
 * @ptr: Page table pointer which points to the first page that should be
 *       mapped to. This will point to the last page mapped to on return.
 * @page_flags: Page options to be applied to every device-virtual memory page
 *              in the created mapping.
 *
 * If you need to map all of @sgl, use pvr_vm_context_map_sgl() instead which
 * derives the values of @offset and @size from @sgl directly.
 *
 * Return:
 *  * 0 on success,
 *  * -%EINVAL if the range specified by @offset and @size is not completely
 *    within @sgl, or
 *  * Any error returned by pvr_vm_context_map_direct().
 */
static int
pvr_vm_context_map_sgl(struct pvr_vm_context *vm_ctx,
		       struct scatterlist *sgl, u64 offset, u64 size,
		       struct pvr_page_table_ptr *ptr,
		       struct pvr_page_flags_raw page_flags)
{
	dma_addr_t dma_addr = sg_dma_address(sgl);
	unsigned int dma_len = sg_dma_len(sgl);

	if (size > dma_len || offset > dma_len - size)
		return -EINVAL;

	return pvr_vm_context_map_direct(vm_ctx, dma_addr + offset, size, ptr,
					 page_flags);
}

/**
 * pvr_vm_context_map_sgt() - Map part of a scatter-gather table into device-virtual memory.
 * @vm_ctx: Target VM context.
 * @sgt: Target scatter-gather table.
 * @sgt_offset: Offset into @sgt to map from. Must result in a starting
 * address from @sgt which is CPU page-aligned.
 * @device_addr: Virtual device address to map to. Must be device page-aligned.
 * @size: Size of memory to be mapped in bytes. Must be a non-zero multiple
 * of the device page size.
 * @page_flags: Page options to be applied to every device-virtual memory page
 * in the created mapping.
 *
 * Return:
 *  * 0 on success, or
 *  * ...
 */
static int
pvr_vm_context_map_sgt(struct pvr_vm_context *vm_ctx,
		       struct sg_table *sgt, u64 sgt_offset,
		       u64 device_addr, u64 size,
		       struct pvr_page_flags_raw page_flags)
{
	struct pvr_page_table_ptr ptr, ptr_copy;
	struct scatterlist *sgl;
	u64 mapped_size = 0;
	unsigned int count;
	int err;

	if (!size)
		return 0;

	if ((sgt_offset | size) & ~PVR_DEVICE_PAGE_MASK)
		return -EINVAL;

	err = pvr_page_table_ptr_init(&ptr, vm_ctx->pvr_dev,
				      &vm_ctx->root_table, device_addr, true);
	if (err)
		return -EINVAL;

	pvr_page_table_ptr_copy(&ptr_copy, &ptr);

	for_each_sgtable_dma_sg(sgt, sgl, count) {
		size_t sgl_len = sg_dma_len(sgl);
		u64 sgl_offset, map_sgl_len;

		if (sgl_len <= sgt_offset) {
			sgt_offset -= sgl_len;
			continue;
		}

		sgl_offset = sgt_offset;
		map_sgl_len = min_t(u64, sgl_len - sgl_offset, size - mapped_size);

		err = pvr_vm_context_map_sgl(vm_ctx, sgl,
					     sgl_offset,
					     map_sgl_len, &ptr,
					     page_flags);
		if (err)
			break;


		/*
		 * Flag the L0 page table as requiring a flush when the page
		 * table pointer is destroyed.
		 */
		pvr_page_table_ptr_require_sync(&ptr, 0);

		sgt_offset = 0;
		mapped_size += map_sgl_len;

		if (mapped_size >= size)
			break;

		err = pvr_page_table_ptr_next_page(&ptr, true);
		if (err)
			break;
	}

	if (err && mapped_size) {
		pvr_vm_context_unmap_from_ptr(&ptr_copy,
					      mapped_size >> PVR_DEVICE_PAGE_SHIFT);
	}

	pvr_page_table_ptr_fini(&ptr_copy);
	pvr_page_table_ptr_fini(&ptr);
	return err;
}

/**
 * DOC: Memory mappings
 */

/**
 * pvr_vm_gpuva_mapping_page_flags_raw() - Generate raw page flags required for
 * applying a mapping.
 * @va: Pointer to drm_gpuva_op_map.
 *
 * Return: A raw page flags instance for use with pvr_vm_context_map_sgt().
 */
static __always_inline struct pvr_page_flags_raw
pvr_vm_gpuva_mapping_page_flags_raw(const struct drm_gpuva_op_map *va)
{
	const u64 flags = gem_to_pvr_gem(va->gem.obj)->flags;

	/*
	 * FIXME: There is currently no way to mark a mapping as read-only or
	 * cache-coherent. Should there be?
	 */
	return pvr_page_flags_raw_create(false, false,
					 flags & DRM_PVR_BO_DEVICE_BYPASS_CACHE,
					 flags & DRM_PVR_BO_DEVICE_PM_FW_PROTECT);
}

/**
 * pvr_vm_gpuva_mapping_init() - Setup a mapping object with the specified
 * parameters ready for mapping using pvr_vm_gpuva_mapping_map().
 * @va: Pointer to drm_gpuva mapping object.
 * @device_addr: Device-virtual address at the start of the mapping.
 * @size: Size of the desired mapping.
 * @pvr_obj: Target PowerVR memory object.
 * @pvr_obj_offset: Offset into @pvr_obj to begin mapping from.
 *
 * Some parameters of this function are unchecked. It is therefore the callers
 * responsibility to ensure certain constraints are met. Specifically:
 *
 * * @pvr_obj_offset must be less than the size of @pvr_obj,
 * * The sum of @pvr_obj_offset and @size must be less than or equal to the
 *   size of @pvr_obj,
 * * The range specified by @pvr_obj_offset and @size (the "CPU range") must be
 *   CPU page-aligned both in start position and size, and
 * * The range specified by @device_addr and @size (the "device range") must be
 *   device page-aligned both in start position and size.
 *
 * Furthermore, it is up to the caller to make sure that a reference to @pvr_obj
 * is taken prior to mapping @va with the drm_gpuva_manager.
 */
static void
pvr_vm_gpuva_mapping_init(struct drm_gpuva *va, u64 device_addr, u64 size,
			  struct pvr_gem_object *pvr_obj, u64 pvr_obj_offset)
{
	va->va.addr = device_addr;
	va->va.range = size;
	va->gem.obj = gem_from_pvr_gem(pvr_obj);
	va->gem.offset = pvr_obj_offset;
}

struct pvr_vm_gpuva_op_ctx {
	struct pvr_vm_context *vm_ctx;
	struct drm_gpuva_prealloc *pa;
	struct drm_gpuva *new_va, *prev_va, *next_va;
};

/**
 * pvr_vm_gpuva_mapping_fini() - Teardown a mapping.
 * @va: Pointer to drm_gpuva mapping object.
 *
 * This function may not be called on a mapping which is currently active. The
 * caller must call pvr_vm_gpuva_mapping_unmap() on @va (or otherwise ensure
 * @va is not currently mapped) before calling this function.
 */
static void
pvr_vm_gpuva_mapping_fini(struct drm_gpuva *va)
{
	pvr_gem_object_put(gem_to_pvr_gem(va->gem.obj));
}

/**
 * pvr_vm_gpuva_map() - Insert a mapping into a memory context.
 * @op: gpuva op containing the remap details.
 * @op_ctx: Operation context.
 *
 * Context: Called by drm_gpuva_sm_map following a successful mapping while
 * @op_ctx.vm_ctx mutex is held.
 *
 * Return:
 *  * 0 on success, or
 *  * Any error encountered while attempting to obtain a reference to the
 *    buffer bound to @op (see pvr_gem_object_get_pages()), or
 *  * Any error returned by pvr_vm_context_map_sgt().
 */
static int
pvr_vm_gpuva_map(struct drm_gpuva_op *op, void *op_ctx)
{
	struct pvr_vm_gpuva_op_ctx *ctx = op_ctx;
	struct pvr_gem_object *pvr_gem = gem_to_pvr_gem(op->map.gem.obj);
	struct sg_table *sgt;
	int err;

	if (!gem_from_pvr_gem(pvr_gem)->import_attach) {
		err = pvr_gem_object_get_pages(pvr_gem);
		if (err)
			return err;
	}

	sgt = pvr_gem_object_get_pages_sgt(pvr_gem);
	if (IS_ERR(sgt)) {
		err = PTR_ERR(sgt);
		goto err_put_pages;
	}

	err = pvr_vm_context_map_sgt(ctx->vm_ctx, sgt, op->map.gem.offset,
				     op->map.va.addr, op->map.va.range,
				     pvr_vm_gpuva_mapping_page_flags_raw(&op->map));
	if (err)
		goto err_put_pages;

	pvr_vm_gpuva_mapping_init(ctx->new_va, op->map.va.addr,
				  op->map.va.range, pvr_gem, op->map.gem.offset);

	err = drm_gpuva_map(&ctx->vm_ctx->gpuva_mgr, ctx->pa, ctx->new_va);
	if (err)
		goto err_unmap;

	/*
	 * Increment the refcount on the underlying physical memory resource
	 * to prevent de-allocation while the mapping exists.
	 */
	pvr_gem_object_get(pvr_gem);

	drm_gpuva_link(ctx->new_va);
	ctx->new_va = NULL;
	return 0;

err_unmap:
	pvr_vm_context_unmap(ctx->vm_ctx, op->map.va.addr,
			     op->map.va.range >> PVR_DEVICE_PAGE_SHIFT);

err_put_pages:
	if (!gem_from_pvr_gem(pvr_gem)->import_attach)
		pvr_gem_object_put_pages(pvr_gem);

	return err;
}

/**
 * pvr_vm_gpuva_unmap() - Remove a mapping from a memory context.
 * @op: gpuva op containing the unmap details.
 * @state: Iterator.
 * @op_ctx: Operation context.
 *
 * Context: Called by drm_gpuva_sm_unmap following a successful unmapping while
 * @op_ctx.vm_ctx mutex is held.
 *
 * Return:
 *  * 0 on success, or
 *  * Any error returned by pvr_vm_context_unmap().
 */
static int
pvr_vm_gpuva_unmap(struct drm_gpuva_op *op, drm_gpuva_state_t state, void *op_ctx)
{
	struct pvr_gem_object *pvr_gem = gem_to_pvr_gem(op->unmap.va->gem.obj);
	struct pvr_vm_gpuva_op_ctx *ctx = op_ctx;

	int err = pvr_vm_context_unmap(ctx->vm_ctx, op->unmap.va->va.addr,
				       op->unmap.va->va.range >> PVR_DEVICE_PAGE_SHIFT);
	if (err)
		return err;

	drm_gpuva_unmap(state);
	drm_gpuva_unlink(op->unmap.va);

	if (!gem_from_pvr_gem(pvr_gem)->import_attach)
		pvr_gem_object_put_pages(pvr_gem);

	pvr_gem_object_put(pvr_gem);
	kfree(op->unmap.va);
	return 0;
}

/**
 * pvr_vm_gpuva_remap() - Remap a mapping within a memory context.
 * @op: gpuva op containing the remap details.
 * @state: Iterator.
 * @op_ctx: Operation context.
 *
 * Context: Called by either drm_gpuva_sm_map or drm_gpuva_sm_unmap when a
 * mapping or unmapping operation causes a region to be split. The
 * @op_ctx.vm_ctx mutex is held.
 *
 * Return:
 *  * 0 on success, or
 *  * Any error returned by pvr_vm_gpuva_unmap() or pvr_vm_gpuva_unmap().
 */
static int
pvr_vm_gpuva_remap(struct drm_gpuva_op *op, drm_gpuva_state_t state, void *op_ctx)
{
	struct pvr_vm_gpuva_op_ctx *ctx = op_ctx;
	int err;

	if (op->remap.unmap) {
		const u64 va_start = op->remap.prev ?
				     op->remap.prev->va.addr + op->remap.prev->va.range :
				     op->remap.unmap->va->va.addr;
		const u64 va_end = op->remap.next ?
				   op->remap.next->va.addr :
				   op->remap.unmap->va->va.addr + op->remap.unmap->va->va.range;

		err = pvr_vm_context_unmap(ctx->vm_ctx, va_start,
					   (va_end - va_start) >> PVR_DEVICE_PAGE_SHIFT);
		if (err)
			return err;
	}

	if (op->remap.prev)
		pvr_vm_gpuva_mapping_init(ctx->prev_va, op->remap.prev->va.addr,
					  op->remap.prev->va.range,
					  gem_to_pvr_gem(op->remap.prev->gem.obj),
					  op->remap.prev->gem.offset);

	if (op->remap.next)
		pvr_vm_gpuva_mapping_init(ctx->next_va, op->remap.next->va.addr,
					  op->remap.next->va.range,
					  gem_to_pvr_gem(op->remap.next->gem.obj),
					  op->remap.next->gem.offset);

	/* No actual remap required: the page table tree depth is fixed to 3, and we use 4k
	 * page table entries only for now.
	 */
	err = drm_gpuva_remap(state, ctx->prev_va, ctx->next_va);
	if (err)
		return err;

	if (op->remap.prev) {
		pvr_gem_object_get(gem_to_pvr_gem(ctx->prev_va->gem.obj));
		drm_gpuva_link(ctx->prev_va);
		ctx->prev_va = NULL;
	}

	if (op->remap.next) {
		pvr_gem_object_get(gem_to_pvr_gem(ctx->next_va->gem.obj));
		drm_gpuva_link(ctx->next_va);
		ctx->next_va = NULL;
	}

	if (op->remap.unmap) {
		struct pvr_gem_object *pvr_gem = gem_to_pvr_gem(op->remap.unmap->va->gem.obj);

		drm_gpuva_unlink(op->unmap.va);
		if (!gem_from_pvr_gem(pvr_gem)->import_attach)
			pvr_gem_object_put_pages(pvr_gem);

		pvr_gem_object_put(pvr_gem);
	}

	return 0;
}

/*
 * Public API
 *
 * For an overview of these functions, see *DOC: Public API* in "pvr_vm.h".
 */

/**
 * pvr_device_addr_is_valid() - Tests whether a device-virtual address
 *                              is valid.
 * @device_addr: Virtual device address to test.
 *
 * Return:
 *  * %true if @device_addr is within the valid range for a device page
 *    table and is aligned to the device page size, or
 *  * %false otherwise.
 */
bool
pvr_device_addr_is_valid(u64 device_addr)
{
	return (device_addr & ~PVR_PAGE_TABLE_ADDR_MASK) == 0 &&
	       (device_addr & ~PVR_DEVICE_PAGE_MASK) == 0;
}

/**
 * pvr_device_addr_and_size_are_valid() - Tests whether a device-virtual
 * address and associated size are both valid.
 * @device_addr: Virtual device address to test.
 * @size: Size of the range based at @device_addr to test.
 *
 * Calling pvr_device_addr_is_valid() twice (once on @size, and again on
 * @device_addr + @size) to verify a device-virtual address range initially
 * seems intuitive, but it produces a false-negative when the address range
 * is right at the end of device-virtual address space.
 *
 * This function catches that corner case, as well as checking that
 * @size is non-zero.
 *
 * Return:
 *  * %true if @device_addr is device page aligned; @size is device page
 *    aligned; the range specified by @device_addr and @size is within the
 *    bounds of the device-virtual address space, and @size is non-zero, or
 *  * %false otherwise.
 */
bool
pvr_device_addr_and_size_are_valid(u64 device_addr, u64 size)
{
	return pvr_device_addr_is_valid(device_addr) &&
	       size != 0 && (size & ~PVR_DEVICE_PAGE_MASK) == 0 &&
	       (device_addr + size <= PVR_PAGE_TABLE_ADDR_SPACE_SIZE);
}

static const struct drm_gpuva_fn_ops pvr_vm_gpuva_ops = {
	.sm_step_map = pvr_vm_gpuva_map,
	.sm_step_remap = pvr_vm_gpuva_remap,
	.sm_step_unmap = pvr_vm_gpuva_unmap,
};

/**
 * pvr_vm_create_context() - Create a new VM context.
 * @pvr_dev: Target PowerVR device.
 * @is_userspace_context: %true if this context is for userspace. This will
 *                        create a firmware memory context for the VM context
 *                        and disable warnings when tearing down mappings.
 *
 * Return:
 *  * A handle to the newly-minted VM context on success,
 *  * -%EINVAL if the feature "virtual address space bits" on @pvr_dev is
 *    missing or has an unsupported value,
 *  * -%ENOMEM if allocation of the structure behind the opaque handle fails,
 *    or
 *  * Any error encountered while setting up internal structures.
 */
struct pvr_vm_context *
pvr_vm_create_context(struct pvr_device *pvr_dev, bool is_userspace_context)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);

	struct pvr_vm_context *vm_ctx;
	u16 device_addr_bits;

	int err;

	err = PVR_FEATURE_VALUE(pvr_dev, virtual_address_space_bits,
				&device_addr_bits);
	if (err) {
		drm_err(drm_dev,
			"Failed to get device virtual address space bits\n");
		goto err_out;
	}

	if (device_addr_bits != PVR_PAGE_TABLE_ADDR_BITS) {
		drm_err(drm_dev,
			"Device has unsupported virtual address space size\n");
		err = -EINVAL;
		goto err_out;
	}

	vm_ctx = kzalloc(sizeof(*vm_ctx), GFP_KERNEL);
	if (!vm_ctx) {
		err = -ENOMEM;
		goto err_out;
	}

	vm_ctx->pvr_dev = pvr_dev;
	kref_init(&vm_ctx->ref_count);
	mutex_init(&vm_ctx->lock);

	drm_gpuva_manager_init(&vm_ctx->gpuva_mgr,
			       is_userspace_context ? "PowerVR-user-VM" : "PowerVR-FW-VM",
			       0, 1ULL << device_addr_bits, 0, 0, &pvr_vm_gpuva_ops);

	err = pvr_page_table_l2_init(&vm_ctx->root_table, pvr_dev);
	if (err)
		goto err_put_ctx;

	if (is_userspace_context) {
		err = pvr_fw_mem_context_create(pvr_dev, vm_ctx, &vm_ctx->fw_mem_ctx_obj);
		if (err)
			goto err_put_ctx;
	}

	return vm_ctx;

err_put_ctx:
	pvr_vm_context_put(vm_ctx);

err_out:
	return ERR_PTR(err);
}

/**
 * pvr_vm_context_release() - Teardown a VM context.
 * @ref_count: Pointer to reference counter of the VM context.
 *
 * This function ensures that no mappings are left dangling by unmapping them
 * all in order of ascending device-virtual address.
 */
static void
pvr_vm_context_release(struct kref *ref_count)
{
	struct pvr_vm_context *vm_ctx =
		container_of(ref_count, struct pvr_vm_context, ref_count);
	DRM_GPUVA_ITER(it, &vm_ctx->gpuva_mgr, 0);
	struct drm_gpuva *va;

	if (vm_ctx->fw_mem_ctx_obj)
		pvr_fw_mem_context_destroy(vm_ctx->fw_mem_ctx_obj);

	drm_gpuva_iter_for_each(va, it) {
		bool can_release_gem_obj;

		if (WARN_ON(unlikely(va == &vm_ctx->gpuva_mgr.kernel_alloc_node)))
			continue;

		drm_gpuva_iter_remove(&it);

		drm_gem_gpuva_lock(va->gem.obj);
		can_release_gem_obj = !WARN_ON(pvr_vm_context_unmap(vm_ctx, va->va.addr,
					       va->va.range >> PVR_DEVICE_PAGE_SHIFT));
		if (can_release_gem_obj && !va->gem.obj->import_attach)
			pvr_gem_object_put_pages(gem_to_pvr_gem(va->gem.obj));
		drm_gpuva_unlink(va);
		drm_gem_gpuva_unlock(va->gem.obj);

		if (can_release_gem_obj)
			pvr_vm_gpuva_mapping_fini(va);
		kfree(va);
	}

	drm_gpuva_manager_destroy(&vm_ctx->gpuva_mgr);
	pvr_page_table_l2_fini(&vm_ctx->root_table);
	mutex_destroy(&vm_ctx->lock);

	kfree(vm_ctx);
}

/**
 * pvr_vm_context_lookup() - Look up VM context from handle
 * @pvr_file: Pointer to pvr_file structure.
 * @handle: Object handle.
 *
 * Takes reference on VM context object. Call pvr_vm_context_put() to release.
 *
 * Returns:
 *  * The requested object on success, or
 *  * %NULL on failure (object does not exist in list, or is not a VM context)
 */
struct pvr_vm_context *
pvr_vm_context_lookup(struct pvr_file *pvr_file, u32 handle)
{
	struct pvr_vm_context *vm_ctx;

	xa_lock(&pvr_file->vm_ctx_handles);
	vm_ctx = xa_load(&pvr_file->vm_ctx_handles, handle);
	if (vm_ctx)
		kref_get(&vm_ctx->ref_count);

	xa_unlock(&pvr_file->vm_ctx_handles);

	return vm_ctx;
}

/**
 * pvr_vm_context_put() - Release a reference on a VM context
 * @vm_ctx: Target VM context.
 *
 * Returns:
 *  * %true if the VM context was destroyed, or
 *  * %false if there are any references still remaining.
 */
bool
pvr_vm_context_put(struct pvr_vm_context *vm_ctx)
{
	WARN_ON(!vm_ctx);

	if (vm_ctx)
		return kref_put(&vm_ctx->ref_count, pvr_vm_context_release);

	return true;
}

/**
 * pvr_destroy_vm_contexts_for_file: Destroy any VM contexts associated with the
 * given file.
 * @pvr_file: Pointer to pvr_file structure.
 *
 * Removes all vm_contexts associated with @pvr_file from the device VM context
 * list and drops initial references. vm_contexts will then be destroyed once
 * all outstanding references are dropped.
 */
void pvr_destroy_vm_contexts_for_file(struct pvr_file *pvr_file)
{
	struct pvr_vm_context *vm_ctx;
	unsigned long handle;

	xa_for_each(&pvr_file->vm_ctx_handles, handle, vm_ctx) {
		/* vm_ctx is not used here because that would create a race with xa_erase */
		pvr_vm_context_put(xa_erase(&pvr_file->vm_ctx_handles, handle));
	}
}

/**
 * pvr_vm_map() - Map a section of physical memory into a section of device-virtual memory.
 * @vm_ctx: Target VM context.
 * @pvr_obj: Target PowerVR memory object.
 * @pvr_obj_offset: Offset into @pvr_obj to map from.
 * @device_addr: Virtual device address at the start of the requested mapping.
 * @size: Size of the requested mapping.
 *
 * No handle is returned to represent the mapping. Instead, callers should
 * remember @device_addr and use that as a handle.
 *
 * Return:
 *  * 0 on success,
 *  * -%EINVAL if @device_addr is not a valid page-aligned device-virtual
 *    address; the region specified by @pvr_obj_offset and @size does not fall
 *    entirely within @pvr_obj, or any part of the specified region of @pvr_obj
 *    is not device-virtual page-aligned,
 *  * Any error encountered while performing internal operations required to
 *    destroy the mapping (returned from pvr_vm_gpuva_map or
 *    pvr_vm_gpuva_remap).
 */
int
pvr_vm_map(struct pvr_vm_context *vm_ctx,
	   struct pvr_gem_object *pvr_obj, u64 pvr_obj_offset,
	   u64 device_addr, u64 size)
{
	const size_t pvr_obj_size = pvr_gem_object_size(pvr_obj);
	struct pvr_vm_gpuva_op_ctx op_ctx = { .vm_ctx = vm_ctx };
	int err;

	if (!pvr_device_addr_and_size_are_valid(device_addr, size) ||
	    pvr_obj_offset & ~PAGE_MASK || size & ~PAGE_MASK ||
	    pvr_obj_offset + size > pvr_obj_size ||
	    pvr_obj_offset > pvr_obj_size) {
		return -EINVAL;
	}

	op_ctx.pa = drm_gpuva_prealloc_create(&vm_ctx->gpuva_mgr);
	if (!op_ctx.pa)
		return -ENOMEM;

	op_ctx.new_va = kzalloc(sizeof(*op_ctx.new_va), GFP_KERNEL);
	op_ctx.prev_va = kzalloc(sizeof(*op_ctx.prev_va), GFP_KERNEL);
	op_ctx.next_va = kzalloc(sizeof(*op_ctx.next_va), GFP_KERNEL);
	if (!op_ctx.new_va || !op_ctx.prev_va || !op_ctx.next_va) {
		err = -ENOMEM;
		goto out;
	}

	mutex_lock(&vm_ctx->lock);
	err = drm_gpuva_sm_map(&vm_ctx->gpuva_mgr, &op_ctx, device_addr, size,
			       gem_from_pvr_gem(pvr_obj), pvr_obj_offset);
	mutex_unlock(&vm_ctx->lock);

out:
	kfree(op_ctx.next_va);
	kfree(op_ctx.prev_va);
	kfree(op_ctx.new_va);
	drm_gpuva_prealloc_destroy(op_ctx.pa);
	return err;
}

/**
 * pvr_vm_unmap() - Unmap an already mapped section of device-virtual memory.
 * @vm_ctx: Target VM context.
 * @device_addr: Virtual device address at the start of the target mapping.
 * @size: Size of the target mapping.
 *
 * Return:
 *  * 0 on success,
 *  * -%EINVAL if @device_addr is not a valid page-aligned device-virtual
 *    address,
 *  * Any error encountered while performing internal operations required to
 *    destroy the mapping (returned from pvr_vm_gpuva_unmap or
 *    pvr_vm_gpuva_remap).
 */
int
pvr_vm_unmap(struct pvr_vm_context *vm_ctx, u64 device_addr, u64 size)
{
	struct pvr_vm_gpuva_op_ctx op_ctx = { .vm_ctx = vm_ctx };
	int err;

	if (!pvr_device_addr_and_size_are_valid(device_addr, size))
		return -EINVAL;

	op_ctx.prev_va = kzalloc(sizeof(*op_ctx.prev_va), GFP_KERNEL);
	op_ctx.next_va = kzalloc(sizeof(*op_ctx.next_va), GFP_KERNEL);
	if (!op_ctx.prev_va || !op_ctx.next_va) {
		err = -ENOMEM;
		goto out;
	}

	mutex_lock(&vm_ctx->lock);
	err = drm_gpuva_sm_unmap(&vm_ctx->gpuva_mgr, &op_ctx, device_addr, size);
	mutex_unlock(&vm_ctx->lock);

out:
	kfree(op_ctx.next_va);
	kfree(op_ctx.prev_va);
	return err;
}

/*
 * Static data areas are determined by firmware.
 *
 * When adding a new static data area you will also need to update the reserved_size field for the
 * heap in pvr_heaps[].
 */
static const struct drm_pvr_static_data_area static_data_areas[] = {
	{
		.area_usage = DRM_PVR_STATIC_DATA_AREA_FENCE,
		.location_heap_id = DRM_PVR_HEAP_GENERAL,
		.offset = 0,
		.size = 128,
	},
	{
		.area_usage = DRM_PVR_STATIC_DATA_AREA_YUV_CSC,
		.location_heap_id = DRM_PVR_HEAP_GENERAL,
		.offset = 128,
		.size = 1024,
	},
	{
		.area_usage = DRM_PVR_STATIC_DATA_AREA_VDM_SYNC,
		.location_heap_id = DRM_PVR_HEAP_PDS_CODE_DATA,
		.offset = 0,
		.size = 128,
	},
	{
		.area_usage = DRM_PVR_STATIC_DATA_AREA_EOT,
		.location_heap_id = DRM_PVR_HEAP_PDS_CODE_DATA,
		.offset = 128,
		.size = 128,
	},
	{
		.area_usage = DRM_PVR_STATIC_DATA_AREA_VDM_SYNC,
		.location_heap_id = DRM_PVR_HEAP_USC_CODE,
		.offset = 0,
		.size = 128,
	},
};

#define GET_RESERVED_SIZE(last_offset, last_size) round_up((last_offset) + (last_size), PAGE_SIZE)

/*
 * The values given to GET_RESERVED_SIZE() are taken from the last entry in the corresponding
 * static data area for each heap.
 */
static const struct drm_pvr_heap pvr_heaps[] = {
	[DRM_PVR_HEAP_GENERAL] = {
		.base = ROGUE_GENERAL_HEAP_BASE,
		.size = ROGUE_GENERAL_HEAP_SIZE,
		.flags = 0,
		.page_size_log2 = PVR_DEVICE_PAGE_SHIFT,
	},
	[DRM_PVR_HEAP_PDS_CODE_DATA] = {
		.base = ROGUE_PDSCODEDATA_HEAP_BASE,
		.size = ROGUE_PDSCODEDATA_HEAP_SIZE,
		.flags = 0,
		.page_size_log2 = PVR_DEVICE_PAGE_SHIFT,
	},
	[DRM_PVR_HEAP_USC_CODE] = {
		.base = ROGUE_USCCODE_HEAP_BASE,
		.size = ROGUE_USCCODE_HEAP_SIZE,
		.flags = 0,
		.page_size_log2 = PVR_DEVICE_PAGE_SHIFT,
	},
	[DRM_PVR_HEAP_RGNHDR] = {
		.base = ROGUE_RGNHDR_HEAP_BASE,
		.size = ROGUE_RGNHDR_HEAP_SIZE,
		.flags = 0,
		.page_size_log2 = PVR_DEVICE_PAGE_SHIFT,
	},
	[DRM_PVR_HEAP_VIS_TEST] = {
		.base = ROGUE_VISTEST_HEAP_BASE,
		.size = ROGUE_VISTEST_HEAP_SIZE,
		.flags = 0,
		.page_size_log2 = PVR_DEVICE_PAGE_SHIFT,
	},
	[DRM_PVR_HEAP_TRANSFER_FRAG] = {
		.base = ROGUE_TRANSFER_FRAG_HEAP_BASE,
		.size = ROGUE_TRANSFER_FRAG_HEAP_SIZE,
		.flags = 0,
		.page_size_log2 = PVR_DEVICE_PAGE_SHIFT,
	},
};

int
pvr_static_data_areas_get(const struct pvr_device *pvr_dev,
			  struct drm_pvr_ioctl_dev_query_args *args)
{
	struct drm_pvr_dev_query_static_data_areas query = {0};
	int err;

	if (!args->pointer) {
		args->size = sizeof(struct drm_pvr_dev_query_static_data_areas);
		return 0;
	}

	err = PVR_UOBJ_GET(query, args->size, args->pointer);
	if (err < 0)
		return err;

	if (!query.static_data_areas.array) {
		query.static_data_areas.count = ARRAY_SIZE(static_data_areas);
		query.static_data_areas.stride = sizeof(struct drm_pvr_static_data_area);
		goto copy_out;
	}

	if (query.static_data_areas.count > ARRAY_SIZE(static_data_areas))
		query.static_data_areas.count = ARRAY_SIZE(static_data_areas);

	err = PVR_UOBJ_SET_ARRAY(&query.static_data_areas, static_data_areas);
	if (err < 0)
		return err;

copy_out:
	err = PVR_UOBJ_SET(args->pointer, args->size, query);
	if (err < 0)
		return err;

	args->size = sizeof(query);
	return 0;
}

int
pvr_heap_info_get(const struct pvr_device *pvr_dev,
		  struct drm_pvr_ioctl_dev_query_args *args)
{
	struct drm_pvr_dev_query_heap_info query = {0};
	u64 dest;
	int err;

	if (!args->pointer) {
		args->size = sizeof(struct drm_pvr_dev_query_heap_info);
		return 0;
	}

	err = PVR_UOBJ_GET(query, args->size, args->pointer);
	if (err < 0)
		return err;

	if (!query.heaps.array) {
		query.heaps.count = ARRAY_SIZE(pvr_heaps);
		query.heaps.stride = sizeof(struct drm_pvr_heap);
		goto copy_out;
	}

	if (query.heaps.count > ARRAY_SIZE(pvr_heaps))
		query.heaps.count = ARRAY_SIZE(pvr_heaps);

	/* Region header heap is only present if BRN63142 is present. */
	dest = query.heaps.array;
	for (size_t i = 0; i < query.heaps.count; i++) {
		struct drm_pvr_heap heap = pvr_heaps[i];

		if (i == DRM_PVR_HEAP_RGNHDR && !PVR_HAS_QUIRK(pvr_dev, 63142))
			heap.size = 0;

		err = PVR_UOBJ_SET(dest, query.heaps.stride, heap);
		if (err < 0)
			return err;

		dest += query.heaps.stride;
	}

copy_out:
	err = PVR_UOBJ_SET(args->pointer, args->size, query);
	if (err < 0)
		return err;

	args->size = sizeof(query);
	return 0;
}

/**
 * pvr_heap_contains_range() - Determine if a given heap contains the specified
 *                             device-virtual address range.
 * @pvr_heap: Target heap.
 * @start: Inclusive start of the target range.
 * @end: Inclusive end of the target range.
 *
 * It is an error to call this function with values of @start and @end that do
 * not satisfy the condition @start <= @end.
 */
static __always_inline bool
pvr_heap_contains_range(const struct drm_pvr_heap *pvr_heap, u64 start, u64 end)
{
	return pvr_heap->base <= start && end < pvr_heap->base + pvr_heap->size;
}

/**
 * pvr_find_heap_containing() - Find a heap which contains the specified
 *                              device-virtual address range.
 * @pvr_dev: Target PowerVR device.
 * @start: Start of the target range.
 * @size: Size of the target range.
 *
 * Return:
 *  * A pointer to a constant instance of struct drm_pvr_heap representing the
 *    heap containing the entire range specified by @start and @size on
 *    success, or
 *  * %NULL if no such heap exists.
 */
const struct drm_pvr_heap *
pvr_find_heap_containing(struct pvr_device *pvr_dev, u64 start, u64 size)
{
	u64 end;

	if (check_add_overflow(start, size - 1, &end))
		return NULL;

	/*
	 * There are no guarantees about the order of address ranges in
	 * &pvr_heaps, so iterate over the entire array for a heap whose
	 * range completely encompasses the given range.
	 */
	for (u32 heap_id = 0; heap_id < ARRAY_SIZE(pvr_heaps); heap_id++) {
		/* Filter heaps that present only with an associated quirk */
		if (heap_id == DRM_PVR_HEAP_RGNHDR &&
		    !PVR_HAS_QUIRK(pvr_dev, 63142)) {
			continue;
		}

		if (pvr_heap_contains_range(&pvr_heaps[heap_id], start, end))
			return &pvr_heaps[heap_id];
	}

	return NULL;
}

/**
 * pvr_vm_find_gem_object() - Look up a buffer object from a given
 *                            device-virtual address.
 * @vm_ctx: [IN] Target VM context.
 * @device_addr: [IN] Virtual device address at the start of the required
 *               object.
 * @mapped_offset_out: [OUT] Pointer to location to write offset of the start
 *                     of the mapped region within the buffer object. May be
 *                     %NULL if this information is not required.
 * @mapped_size_out: [OUT] Pointer to location to write size of the mapped
 *                   region. May be %NULL if this information is not required.
 *
 * If successful, a reference will be taken on the buffer object. The caller
 * must drop the reference with pvr_gem_object_put().
 *
 * Return:
 *  * The PowerVR buffer object mapped at @device_addr if one exists, or
 *  * %NULL otherwise.
 */
struct pvr_gem_object *
pvr_vm_find_gem_object(struct pvr_vm_context *vm_ctx, u64 device_addr,
		       u64 *mapped_offset_out, u64 *mapped_size_out)
{
	struct pvr_gem_object *pvr_obj;
	struct drm_gpuva *va;

	mutex_lock(&vm_ctx->lock);

	va = drm_gpuva_find_first(&vm_ctx->gpuva_mgr, device_addr,
				  vm_ctx->gpuva_mgr.mm_range - device_addr);
	if (!va)
		goto err_unlock;

	pvr_obj = gem_to_pvr_gem(va->gem.obj);
	pvr_gem_object_get(pvr_obj);

	if (mapped_offset_out)
		*mapped_offset_out = va->gem.offset;
	if (mapped_size_out)
		*mapped_size_out = va->va.range;

	mutex_unlock(&vm_ctx->lock);

	return pvr_obj;

err_unlock:
	mutex_unlock(&vm_ctx->lock);

	return NULL;
}

/**
 * pvr_vm_get_fw_mem_context: Get object representing firmware memory context
 * @vm_ctx: Target VM context.
 *
 * Returns:
 *  * FW object representing firmware memory context, or
 *  * %NULL if this VM context does not have a firmware memory context.
 */
struct pvr_fw_object *
pvr_vm_get_fw_mem_context(struct pvr_vm_context *vm_ctx)
{
	return vm_ctx->fw_mem_ctx_obj;
}
