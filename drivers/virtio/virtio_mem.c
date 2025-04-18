// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Virtio-mem device driver.
 *
 * Copyright Red Hat, Inc. 2020
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Author(s): David Hildenbrand <david@redhat.com>
 */

#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/mem-buf.h>
#include <soc/qcom/secure_buffer.h>
#include <linux/xarray.h>
#include <linux/virtio.h>
#include <linux/virtio_mem.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/memory_hotplug.h>
#include <linux/memory.h>
#include <linux/hrtimer.h>
#include <linux/crash_dump.h>
#include <linux/mutex.h>
#include <linux/bitmap.h>
#include <linux/lockdep.h>
#include <linux/log2.h>
#include <linux/sched/mm.h>
#include "qti_virtio_mem.h"

#include <acpi/acpi_numa.h>

static bool unplug_online = true;
module_param(unplug_online, bool, 0644);
MODULE_PARM_DESC(unplug_online, "Try to unplug online memory");

static bool force_bbm;
module_param(force_bbm, bool, 0444);
MODULE_PARM_DESC(force_bbm,
		"Force Big Block Mode. Default is 0 (auto-selection)");

static unsigned long bbm_block_size;
module_param(bbm_block_size, ulong, 0444);
MODULE_PARM_DESC(bbm_block_size,
		 "Big Block size in bytes. Default is 0 (auto-detection).");

/*
 * virtio-mem currently supports the following modes of operation:
 *
 * * Sub Block Mode (SBM): A Linux memory block spans 2..X subblocks (SB). The
 *   size of a Sub Block (SB) is determined based on the device block size, the
 *   pageblock size, and the maximum allocation granularity of the buddy.
 *   Subblocks within a Linux memory block might either be plugged or unplugged.
 *   Memory is added/removed to Linux MM in Linux memory block granularity.
 *
 * * Big Block Mode (BBM): A Big Block (BB) spans 1..X Linux memory blocks.
 *   Memory is added/removed to Linux MM in Big Block granularity.
 *
 * The mode is determined automatically based on the Linux memory block size
 * and the device block size.
 *
 * User space / core MM (auto onlining) is responsible for onlining added
 * Linux memory blocks - and for selecting a zone. Linux Memory Blocks are
 * always onlined separately, and all memory within a Linux memory block is
 * onlined to the same zone - virtio-mem relies on this behavior.
 */

/* For now, only allow one virtio-mem device */
struct virtio_mem *virtio_mem_dev;
static DEFINE_XARRAY(xa_membuf);

/*
 * We have to share a single online_page callback among all virtio-mem
 * devices. We use RCU to iterate the list in the callback.
 */
static DEFINE_MUTEX(virtio_mem_mutex);
static LIST_HEAD(virtio_mem_devices);

static void virtio_mem_online_page_cb(struct page *page, unsigned int order);
static void virtio_mem_fake_offline_going_offline(unsigned long pfn,
						  unsigned long nr_pages);
static void virtio_mem_fake_offline_cancel_offline(unsigned long pfn,
						   unsigned long nr_pages);
static void virtio_mem_retry(struct virtio_mem *vm);
static int virtio_mem_create_resource(struct virtio_mem *vm);
static void virtio_mem_delete_resource(struct virtio_mem *vm);
static int virtio_mem_send_plug_request(struct virtio_mem *vm, uint64_t addr,
					uint64_t size, bool memmap);
static int virtio_mem_send_unplug_request(struct virtio_mem *vm, uint64_t addr,
					  uint64_t size, bool memmap);
static void virtio_mem_deinit_hotplug(struct virtio_mem *vm);

/*
 * Register a virtio-mem device so it will be considered for the online_page
 * callback.
 */
static int register_virtio_mem_device(struct virtio_mem *vm)
{
	int rc = 0;

	/* First device registers the callback. */
	mutex_lock(&virtio_mem_mutex);
	if (list_empty(&virtio_mem_devices))
		rc = set_online_page_callback(&virtio_mem_online_page_cb);
	if (!rc)
		list_add_rcu(&vm->next, &virtio_mem_devices);
	mutex_unlock(&virtio_mem_mutex);

	return rc;
}

/*
 * Unregister a virtio-mem device so it will no longer be considered for the
 * online_page callback.
 */
static void unregister_virtio_mem_device(struct virtio_mem *vm)
{
	/* Last device unregisters the callback. */
	mutex_lock(&virtio_mem_mutex);
	list_del_rcu(&vm->next);
	if (list_empty(&virtio_mem_devices))
		restore_online_page_callback(&virtio_mem_online_page_cb);
	mutex_unlock(&virtio_mem_mutex);

	synchronize_rcu();
}

/*
 * Calculate the memory block id of a given address.
 */
static unsigned long virtio_mem_phys_to_mb_id(unsigned long addr)
{
	return addr / memory_block_size_bytes();
}

/*
 * Calculate the physical start address of a given memory block id.
 */
static unsigned long virtio_mem_mb_id_to_phys(unsigned long mb_id)
{
	return mb_id * memory_block_size_bytes();
}

/*
 * Calculate the physical start address of a given sb memory block id,
 */
static unsigned long virtio_mem_sb_id_to_phys(struct virtio_mem *vm, unsigned long mb_id, int sb_id)
{
	if (vm->memmap_on_memory)
		sb_id += 1;
	return mb_id * memory_block_size_bytes() + sb_id * vm->sbm.sb_size;
}

/*
 * Calculate the big block id of a given address.
 */
static unsigned long virtio_mem_phys_to_bb_id(struct virtio_mem *vm,
					      uint64_t addr)
{
	return addr / vm->bbm.bb_size;
}

/*
 * Calculate the physical start address of a given big block id.
 */
static uint64_t virtio_mem_bb_id_to_phys(struct virtio_mem *vm,
					 unsigned long bb_id)
{
	return bb_id * vm->bbm.bb_size;
}

/*
 * Calculate the subblock id of a given address.
 */
static unsigned long virtio_mem_phys_to_sb_id(struct virtio_mem *vm,
					      unsigned long addr)
{
	const unsigned long mb_id = virtio_mem_phys_to_mb_id(addr);
	const unsigned long mb_addr = virtio_mem_mb_id_to_phys(mb_id);
	unsigned long sb_id;

	sb_id = (addr - mb_addr) / vm->sbm.sb_size;
	if (vm->memmap_on_memory)
		sb_id -= 1;
	return sb_id;
}

/*
 * Set the state of a big block, taking care of the state counter.
 */
static void virtio_mem_bbm_set_bb_state(struct virtio_mem *vm,
					unsigned long bb_id,
					enum virtio_mem_bbm_bb_state state)
{
	const unsigned long idx = bb_id - vm->bbm.first_bb_id;
	enum virtio_mem_bbm_bb_state old_state;

	old_state = vm->bbm.bb_states[idx];
	vm->bbm.bb_states[idx] = state;

	BUG_ON(vm->bbm.bb_count[old_state] == 0);
	vm->bbm.bb_count[old_state]--;
	vm->bbm.bb_count[state]++;
}

/*
 * Get the state of a big block.
 */
static enum virtio_mem_bbm_bb_state virtio_mem_bbm_get_bb_state(struct virtio_mem *vm,
								unsigned long bb_id)
{
	return vm->bbm.bb_states[bb_id - vm->bbm.first_bb_id];
}

/*
 * Prepare the big block state array for the next big block.
 */
static int virtio_mem_bbm_bb_states_prepare_next_bb(struct virtio_mem *vm)
{
	unsigned long old_bytes = vm->bbm.next_bb_id - vm->bbm.first_bb_id;
	unsigned long new_bytes = old_bytes + 1;
	int old_pages = PFN_UP(old_bytes);
	int new_pages = PFN_UP(new_bytes);
	uint8_t *new_array;

	if (vm->bbm.bb_states && old_pages == new_pages)
		return 0;

	new_array = vzalloc(new_pages * PAGE_SIZE);
	if (!new_array)
		return -ENOMEM;

	mutex_lock(&vm->hotplug_mutex);
	if (vm->bbm.bb_states)
		memcpy(new_array, vm->bbm.bb_states, old_pages * PAGE_SIZE);
	vfree(vm->bbm.bb_states);
	vm->bbm.bb_states = new_array;
	mutex_unlock(&vm->hotplug_mutex);

	return 0;
}

#define virtio_mem_bbm_for_each_bb(_vm, _bb_id, _state) \
	for (_bb_id = vm->bbm.first_bb_id; \
	     _bb_id < vm->bbm.next_bb_id && _vm->bbm.bb_count[_state]; \
	     _bb_id++) \
		if (virtio_mem_bbm_get_bb_state(_vm, _bb_id) == _state)

#define virtio_mem_bbm_for_each_bb_rev(_vm, _bb_id, _state) \
	for (_bb_id = vm->bbm.next_bb_id - 1; \
	     _bb_id >= vm->bbm.first_bb_id && _vm->bbm.bb_count[_state]; \
	     _bb_id--) \
		if (virtio_mem_bbm_get_bb_state(_vm, _bb_id) == _state)

/*
 * Set the state of a memory block, taking care of the state counter.
 */
static void virtio_mem_sbm_set_mb_state(struct virtio_mem *vm,
					unsigned long mb_id, uint8_t state)
{
	const unsigned long idx = mb_id - vm->sbm.first_mb_id;
	uint8_t old_state;

	old_state = vm->sbm.mb_states[idx];
	vm->sbm.mb_states[idx] = state;

	BUG_ON(vm->sbm.mb_count[old_state] == 0);
	vm->sbm.mb_count[old_state]--;
	vm->sbm.mb_count[state]++;
}

/*
 * Get the state of a memory block.
 */
static uint8_t virtio_mem_sbm_get_mb_state(struct virtio_mem *vm,
					   unsigned long mb_id)
{
	const unsigned long idx = mb_id - vm->sbm.first_mb_id;

	return vm->sbm.mb_states[idx];
}

/*
 * Prepare the state array for the next memory block.
 */
static int virtio_mem_sbm_mb_states_prepare_next_mb(struct virtio_mem *vm)
{
	int old_pages = PFN_UP(vm->sbm.next_mb_id - vm->sbm.first_mb_id);
	int new_pages = PFN_UP(vm->sbm.next_mb_id - vm->sbm.first_mb_id + 1);
	uint8_t *new_array;

	if (vm->sbm.mb_states && old_pages == new_pages)
		return 0;

	new_array = vzalloc(new_pages * PAGE_SIZE);
	if (!new_array)
		return -ENOMEM;

	mutex_lock(&vm->hotplug_mutex);
	if (vm->sbm.mb_states)
		memcpy(new_array, vm->sbm.mb_states, old_pages * PAGE_SIZE);
	vfree(vm->sbm.mb_states);
	vm->sbm.mb_states = new_array;
	mutex_unlock(&vm->hotplug_mutex);

	return 0;
}

#define virtio_mem_sbm_for_each_mb(_vm, _mb_id, _state) \
	for (_mb_id = _vm->sbm.first_mb_id; \
	     _mb_id < _vm->sbm.next_mb_id && _vm->sbm.mb_count[_state]; \
	     _mb_id++) \
		if (virtio_mem_sbm_get_mb_state(_vm, _mb_id) == _state)

#define virtio_mem_sbm_for_each_mb_rev(_vm, _mb_id, _state) \
	for (_mb_id = _vm->sbm.next_mb_id - 1; \
	     _mb_id >= _vm->sbm.first_mb_id && _vm->sbm.mb_count[_state]; \
	     _mb_id--) \
		if (virtio_mem_sbm_get_mb_state(_vm, _mb_id) == _state)

/*
 * Calculate the bit number in the subblock bitmap for the given subblock
 * inside the given memory block.
 */
static int virtio_mem_sbm_sb_state_bit_nr(struct virtio_mem *vm,
					  unsigned long mb_id, int sb_id)
{
	return (mb_id - vm->sbm.first_mb_id) * vm->sbm.sbs_per_mb + sb_id;
}

/*
 * Mark all selected subblocks plugged.
 *
 * Will not modify the state of the memory block.
 */
static void virtio_mem_sbm_set_sb_plugged(struct virtio_mem *vm,
					  unsigned long mb_id, int sb_id,
					  int count)
{
	const int bit = virtio_mem_sbm_sb_state_bit_nr(vm, mb_id, sb_id);

	__bitmap_set(vm->sbm.sb_states, bit, count);
}

/*
 * Mark all selected subblocks unplugged.
 *
 * Will not modify the state of the memory block.
 */
static void virtio_mem_sbm_set_sb_unplugged(struct virtio_mem *vm,
					    unsigned long mb_id, int sb_id,
					    int count)
{
	const int bit = virtio_mem_sbm_sb_state_bit_nr(vm, mb_id, sb_id);

	__bitmap_clear(vm->sbm.sb_states, bit, count);
}

/*
 * Test if all selected subblocks are plugged.
 */
static bool virtio_mem_sbm_test_sb_plugged(struct virtio_mem *vm,
					   unsigned long mb_id, int sb_id,
					   int count)
{
	const int bit = virtio_mem_sbm_sb_state_bit_nr(vm, mb_id, sb_id);

	if (count == 1)
		return test_bit(bit, vm->sbm.sb_states);

	/* TODO: Helper similar to bitmap_set() */
	return find_next_zero_bit(vm->sbm.sb_states, bit + count, bit) >=
	       bit + count;
}

/*
 * Test if all selected subblocks are unplugged.
 */
static bool virtio_mem_sbm_test_sb_unplugged(struct virtio_mem *vm,
					     unsigned long mb_id, int sb_id,
					     int count)
{
	const int bit = virtio_mem_sbm_sb_state_bit_nr(vm, mb_id, sb_id);

	/* TODO: Helper similar to bitmap_set() */
	return find_next_bit(vm->sbm.sb_states, bit + count, bit) >=
	       bit + count;
}

/*
 * Find the first unplugged subblock. Returns vm->sbm.sbs_per_mb in case there is
 * none.
 */
static int virtio_mem_sbm_first_unplugged_sb(struct virtio_mem *vm,
					    unsigned long mb_id)
{
	const int bit = virtio_mem_sbm_sb_state_bit_nr(vm, mb_id, 0);

	return find_next_zero_bit(vm->sbm.sb_states,
				  bit + vm->sbm.sbs_per_mb, bit) - bit;
}

/*
 * Prepare the subblock bitmap for the next memory block.
 */
static int virtio_mem_sbm_sb_states_prepare_next_mb(struct virtio_mem *vm)
{
	const unsigned long old_nb_mb = vm->sbm.next_mb_id - vm->sbm.first_mb_id;
	const unsigned long old_nb_bits = old_nb_mb * vm->sbm.sbs_per_mb;
	const unsigned long new_nb_bits = (old_nb_mb + 1) * vm->sbm.sbs_per_mb;
	int old_pages = PFN_UP(BITS_TO_LONGS(old_nb_bits) * sizeof(long));
	int new_pages = PFN_UP(BITS_TO_LONGS(new_nb_bits) * sizeof(long));
	unsigned long *new_bitmap, *old_bitmap;

	if (vm->sbm.sb_states && old_pages == new_pages)
		return 0;

	new_bitmap = vzalloc(new_pages * PAGE_SIZE);
	if (!new_bitmap)
		return -ENOMEM;

	mutex_lock(&vm->hotplug_mutex);
	if (vm->sbm.sb_states)
		memcpy(new_bitmap, vm->sbm.sb_states, old_pages * PAGE_SIZE);

	old_bitmap = vm->sbm.sb_states;
	vm->sbm.sb_states = new_bitmap;
	mutex_unlock(&vm->hotplug_mutex);

	vfree(old_bitmap);
	return 0;
}

/*
 * See memory_block_memmap_on_memory_pages() in mm/memory_hotplug.c
 * memory_hotplug.memmap_on_memory must be set to "force"
 */
static unsigned long virtio_mem_memory_block_vmemmap_size(void)
{
	unsigned long vmemmap_size = PAGES_PER_SECTION * sizeof(struct page);
	unsigned long nr_pages = PFN_UP(vmemmap_size);

	return pageblock_align(nr_pages) * PAGE_SIZE;
}

static int virtio_mem_plug_memmap(struct virtio_mem *vm, uint64_t addr)
{
	unsigned long vmemmap_size = virtio_mem_memory_block_vmemmap_size();

	if (!vm->memmap_on_memory)
		return 0;

	dev_dbg(&vm->vdev->dev, "plugging memmap: 0x%llx - 0x%llx\n", addr,
		addr + vmemmap_size - 1);
	return virtio_mem_send_plug_request(vm, addr, vmemmap_size, true);
}

static void virtio_mem_unplug_memmap(struct virtio_mem *vm, uint64_t addr)
{
	unsigned long vmemmap_size = virtio_mem_memory_block_vmemmap_size();

	if (!vm->memmap_on_memory)
		return;

	dev_dbg(&vm->vdev->dev, "unplugging memmap: 0x%llx - 0x%llx\n", addr,
		addr + vmemmap_size - 1);
	virtio_mem_send_unplug_request(vm, addr, vmemmap_size, true);
}

/*
 * Test if we could add memory without creating too much offline memory -
 * to avoid running OOM if memory is getting onlined deferred.
 */
static bool virtio_mem_could_add_memory(struct virtio_mem *vm, uint64_t size)
{
	if (WARN_ON_ONCE(size > vm->offline_threshold))
		return false;

	return atomic64_read(&vm->offline_size) + size <= vm->offline_threshold;
}

/*
 * Try adding memory to Linux. Will usually only fail if out of memory.
 *
 * Must not be called with the vm->hotplug_mutex held (possible deadlock with
 * onlining code).
 *
 * Will not modify the state of memory blocks in virtio-mem.
 */
static int virtio_mem_add_memory(struct virtio_mem *vm, uint64_t addr,
				 uint64_t size)
{
	int rc;
	mhp_t mhp_flags = MHP_MERGE_RESOURCE | MHP_NID_IS_MGID;

	/*
	 * When force-unloading the driver and we still have memory added to
	 * Linux, the resource name has to stay.
	 */
	if (!vm->resource_name) {
		vm->resource_name = kstrdup_const("System RAM (virtio_mem)",
						  GFP_KERNEL);
		if (!vm->resource_name)
			return -ENOMEM;
	}

	dev_dbg(&vm->vdev->dev, "adding memory: 0x%llx - 0x%llx\n", addr,
		addr + size - 1);

	if (vm->memmap_on_memory)
		mhp_flags |= MHP_MEMMAP_ON_MEMORY;

	/*
	 * We must bring in the memory backing the struct page array because
	 * add_memory_driver_managed() may write to it in sparse_add_section().
	 */
	rc = virtio_mem_plug_memmap(vm, addr);
	if (rc)
		return rc;

	/* Memory might get onlined immediately. */
	atomic64_add(size, &vm->offline_size);
	rc = add_memory_driver_managed(vm->mgid, addr, size, vm->resource_name,
				       mhp_flags);
	if (rc) {
		atomic64_sub(size, &vm->offline_size);
		dev_warn(&vm->vdev->dev, "adding memory failed: %d\n", rc);
		/*
		 * TODO: Linux MM does not properly clean up yet in all cases
		 * where adding of memory failed - especially on -ENOMEM.
		 */
		virtio_mem_unplug_memmap(vm, addr);
	}
	return rc;
}

/*
 * See virtio_mem_add_memory(): Try adding a single Linux memory block.
 */
static int virtio_mem_sbm_add_mb(struct virtio_mem *vm, unsigned long mb_id)
{
	const uint64_t addr = virtio_mem_mb_id_to_phys(mb_id);
	const uint64_t size = memory_block_size_bytes();

	return virtio_mem_add_memory(vm, addr, size);
}

/*
 * See virtio_mem_add_memory(): Try adding a big block.
 */
static int virtio_mem_bbm_add_bb(struct virtio_mem *vm, unsigned long bb_id)
{
	const uint64_t addr = virtio_mem_bb_id_to_phys(vm, bb_id);
	const uint64_t size = vm->bbm.bb_size;

	return virtio_mem_add_memory(vm, addr, size);
}

/*
 * Try removing memory from Linux. Will only fail if memory blocks aren't
 * offline.
 *
 * Must not be called with the vm->hotplug_mutex held (possible deadlock with
 * onlining code).
 *
 * Will not modify the state of memory blocks in virtio-mem.
 */
static int virtio_mem_remove_memory(struct virtio_mem *vm, uint64_t addr,
				    uint64_t size)
{
	int rc;

	dev_dbg(&vm->vdev->dev, "removing memory: 0x%llx - 0x%llx\n", addr,
		addr + size - 1);
	rc = remove_memory(addr, size);
	if (!rc) {
		atomic64_sub(size, &vm->offline_size);
		/*
		 * We might have freed up memory we can now unplug, retry
		 * immediately instead of waiting.
		 */
		virtio_mem_retry(vm);
	} else {
		dev_dbg(&vm->vdev->dev, "removing memory failed: %d\n", rc);
	}

	/*
	 * mhp_deinit_memmap_on_memory() will try to access memmap during hotremove,
	 * so only remove the memmap after hot-removing the memory.
	 */
	if (!rc)
		virtio_mem_unplug_memmap(vm, addr);
	return rc;
}

/*
 * See virtio_mem_remove_memory(): Try removing a single Linux memory block.
 */
static int virtio_mem_sbm_remove_mb(struct virtio_mem *vm, unsigned long mb_id)
{
	const uint64_t addr = virtio_mem_mb_id_to_phys(mb_id);
	const uint64_t size = memory_block_size_bytes();

	return virtio_mem_remove_memory(vm, addr, size);
}

/*
 * Try offlining and removing memory from Linux.
 *
 * Must not be called with the vm->hotplug_mutex held (possible deadlock with
 * onlining code).
 *
 * Will not modify the state of memory blocks in virtio-mem.
 */
static int virtio_mem_offline_and_remove_memory(struct virtio_mem *vm,
						uint64_t addr,
						uint64_t size)
{
	int rc;

	dev_dbg(&vm->vdev->dev,
		"offlining and removing memory: 0x%llx - 0x%llx\n", addr,
		addr + size - 1);

	rc = offline_and_remove_memory(addr, size);
	if (!rc) {
		atomic64_sub(size, &vm->offline_size);

		/*
		 * mhp_deinit_memmap_on_memory() will try to access memmap
		 * during hotremove, so only remove the memmap after
		 * hot-removing the memory.
		 */
		virtio_mem_unplug_memmap(vm, addr);

		/*
		 * We might have freed up memory we can now unplug, retry
		 * immediately instead of waiting.
		 */
		virtio_mem_retry(vm);
		return 0;
	}
	dev_dbg(&vm->vdev->dev, "offlining and removing memory failed: %d\n", rc);
	/*
	 * We don't really expect this to fail, because we fake-offlined all
	 * memory already. But it could fail in corner cases.
	 */
	WARN_ON_ONCE(rc != -ENOMEM && rc != -EBUSY);
	return rc == -ENOMEM ? -ENOMEM : -EBUSY;
}

/*
 * See virtio_mem_offline_and_remove_memory(): Try offlining and removing
 * a single Linux memory block.
 */
static int virtio_mem_sbm_offline_and_remove_mb(struct virtio_mem *vm,
						unsigned long mb_id)
{
	const uint64_t addr = virtio_mem_mb_id_to_phys(mb_id);
	const uint64_t size = memory_block_size_bytes();

	return virtio_mem_offline_and_remove_memory(vm, addr, size);
}

/*
 * Try (offlining and) removing memory from Linux in case all subblocks are
 * unplugged. Can be called on online and offline memory blocks.
 *
 * May modify the state of memory blocks in virtio-mem.
 */
static int virtio_mem_sbm_try_remove_unplugged_mb(struct virtio_mem *vm,
						  unsigned long mb_id)
{
	int rc;

	/*
	 * Once all subblocks of a memory block were unplugged, offline and
	 * remove it.
	 */
	if (!virtio_mem_sbm_test_sb_unplugged(vm, mb_id, 0, vm->sbm.sbs_per_mb))
		return 0;

	/* offline_and_remove_memory() works for online and offline memory. */
	mutex_unlock(&vm->hotplug_mutex);
	rc = virtio_mem_sbm_offline_and_remove_mb(vm, mb_id);
	mutex_lock(&vm->hotplug_mutex);
	if (!rc)
		virtio_mem_sbm_set_mb_state(vm, mb_id,
					    VIRTIO_MEM_SBM_MB_UNUSED);
	return rc;
}

/*
 * See virtio_mem_offline_and_remove_memory(): Try to offline and remove a
 * all Linux memory blocks covered by the big block.
 */
static int virtio_mem_bbm_offline_and_remove_bb(struct virtio_mem *vm,
						unsigned long bb_id)
{
	const uint64_t addr = virtio_mem_bb_id_to_phys(vm, bb_id);
	const uint64_t size = vm->bbm.bb_size;

	return virtio_mem_offline_and_remove_memory(vm, addr, size);
}

/*
 * Trigger the workqueue so the device can perform its magic.
 */
static void virtio_mem_retry(struct virtio_mem *vm)
{
	unsigned long flags;

	spin_lock_irqsave(&vm->removal_lock, flags);
	if (!vm->removing)
		queue_work(system_freezable_wq, &vm->wq);
	spin_unlock_irqrestore(&vm->removal_lock, flags);
}

static int virtio_mem_translate_node_id(struct virtio_mem *vm, uint16_t node_id)
{
	int node = NUMA_NO_NODE;

#if defined(CONFIG_ACPI_NUMA)
	if (virtio_has_feature(vm->vdev, VIRTIO_MEM_F_ACPI_PXM))
		node = pxm_to_node(node_id);
#endif
	return node;
}

/*
 * Test if a virtio-mem device overlaps with the given range. Can be called
 * from (notifier) callbacks lockless.
 */
static bool virtio_mem_overlaps_range(struct virtio_mem *vm, uint64_t start,
				      uint64_t size)
{
	return start < vm->addr + vm->region_size && vm->addr < start + size;
}

/*
 * Test if a virtio-mem device contains a given range. Can be called from
 * (notifier) callbacks lockless.
 */
static bool virtio_mem_contains_range(struct virtio_mem *vm, uint64_t start,
				      uint64_t size)
{
	return start >= vm->addr && start + size <= vm->addr + vm->region_size;
}

static int virtio_mem_sbm_notify_going_online(struct virtio_mem *vm,
					      unsigned long mb_id)
{
	switch (virtio_mem_sbm_get_mb_state(vm, mb_id)) {
	case VIRTIO_MEM_SBM_MB_OFFLINE_PARTIAL:
	case VIRTIO_MEM_SBM_MB_OFFLINE:
		return NOTIFY_OK;
	default:
		break;
	}
	dev_warn_ratelimited(&vm->vdev->dev,
			     "memory block onlining denied\n");
	return NOTIFY_BAD;
}

static void virtio_mem_sbm_notify_offline(struct virtio_mem *vm,
					  unsigned long mb_id)
{
	switch (virtio_mem_sbm_get_mb_state(vm, mb_id)) {
	case VIRTIO_MEM_SBM_MB_KERNEL_PARTIAL:
	case VIRTIO_MEM_SBM_MB_MOVABLE_PARTIAL:
		virtio_mem_sbm_set_mb_state(vm, mb_id,
					    VIRTIO_MEM_SBM_MB_OFFLINE_PARTIAL);
		break;
	case VIRTIO_MEM_SBM_MB_KERNEL:
	case VIRTIO_MEM_SBM_MB_MOVABLE:
		virtio_mem_sbm_set_mb_state(vm, mb_id,
					    VIRTIO_MEM_SBM_MB_OFFLINE);
		break;
	default:
		BUG();
		break;
	}
}

static void virtio_mem_sbm_notify_online(struct virtio_mem *vm,
					 unsigned long mb_id,
					 unsigned long start_pfn)
{
	const bool is_movable = is_zone_movable_page(pfn_to_page(start_pfn));
	int new_state;

	switch (virtio_mem_sbm_get_mb_state(vm, mb_id)) {
	case VIRTIO_MEM_SBM_MB_OFFLINE_PARTIAL:
		new_state = VIRTIO_MEM_SBM_MB_KERNEL_PARTIAL;
		if (is_movable)
			new_state = VIRTIO_MEM_SBM_MB_MOVABLE_PARTIAL;
		break;
	case VIRTIO_MEM_SBM_MB_OFFLINE:
		new_state = VIRTIO_MEM_SBM_MB_KERNEL;
		if (is_movable)
			new_state = VIRTIO_MEM_SBM_MB_MOVABLE;
		break;
	default:
		BUG();
		break;
	}
	virtio_mem_sbm_set_mb_state(vm, mb_id, new_state);
}

static void virtio_mem_sbm_notify_going_offline(struct virtio_mem *vm,
						unsigned long mb_id)
{
	const unsigned long nr_pages = PFN_DOWN(vm->sbm.sb_size);
	unsigned long pfn;
	int sb_id;

	for (sb_id = 0; sb_id < vm->sbm.sbs_per_mb; sb_id++) {
		if (virtio_mem_sbm_test_sb_plugged(vm, mb_id, sb_id, 1))
			continue;
		pfn = PFN_DOWN(virtio_mem_sb_id_to_phys(vm, mb_id, sb_id));
		virtio_mem_fake_offline_going_offline(pfn, nr_pages);
	}
}

static void virtio_mem_sbm_notify_cancel_offline(struct virtio_mem *vm,
						 unsigned long mb_id)
{
	const unsigned long nr_pages = PFN_DOWN(vm->sbm.sb_size);
	unsigned long pfn;
	int sb_id;

	for (sb_id = 0; sb_id < vm->sbm.sbs_per_mb; sb_id++) {
		if (virtio_mem_sbm_test_sb_plugged(vm, mb_id, sb_id, 1))
			continue;
		pfn = PFN_DOWN(virtio_mem_sb_id_to_phys(vm, mb_id, sb_id));
		virtio_mem_fake_offline_cancel_offline(pfn, nr_pages);
	}
}

static void virtio_mem_bbm_notify_going_offline(struct virtio_mem *vm,
						unsigned long bb_id,
						unsigned long pfn,
						unsigned long nr_pages)
{
	/*
	 * When marked as "fake-offline", all online memory of this device block
	 * is allocated by us. Otherwise, we don't have any memory allocated.
	 */
	if (virtio_mem_bbm_get_bb_state(vm, bb_id) !=
	    VIRTIO_MEM_BBM_BB_FAKE_OFFLINE)
		return;
	virtio_mem_fake_offline_going_offline(pfn, nr_pages);
}

static void virtio_mem_bbm_notify_cancel_offline(struct virtio_mem *vm,
						 unsigned long bb_id,
						 unsigned long pfn,
						 unsigned long nr_pages)
{
	if (virtio_mem_bbm_get_bb_state(vm, bb_id) !=
	    VIRTIO_MEM_BBM_BB_FAKE_OFFLINE)
		return;
	virtio_mem_fake_offline_cancel_offline(pfn, nr_pages);
}

/*
 * This callback will either be called synchronously from add_memory() or
 * asynchronously (e.g., triggered via user space). We have to be careful
 * with locking when calling add_memory().
 */
static int virtio_mem_memory_notifier_cb(struct notifier_block *nb,
					 unsigned long action, void *arg)
{
	struct virtio_mem *vm = container_of(nb, struct virtio_mem,
					     memory_notifier);
	struct memory_notify *mhp = arg;
	const unsigned long start = PFN_PHYS(mhp->start_pfn);
	const unsigned long size = PFN_PHYS(mhp->nr_pages);
	int rc = NOTIFY_OK;
	unsigned long id;

	if (!virtio_mem_overlaps_range(vm, start, size))
		return NOTIFY_DONE;

	if (vm->in_sbm) {
		unsigned long expected_size = memory_block_size_bytes();
		unsigned long expected_offset = 0;

		/*
		 * online_pages will exclude pages reserved for memmap. Adjust accordingly.
		 */
		if (vm->memmap_on_memory) {
			expected_size -= vm->sbm.sb_size;
			expected_offset += vm->sbm.sb_size;
		}

		id = virtio_mem_phys_to_mb_id(start);
		/*
		 * In SBM, we add memory in separate memory blocks - we expect
		 * it to be onlined/offlined in the same granularity. Bail out
		 * if this ever changes.
		 */
		if (WARN_ON_ONCE(size != expected_size ||
				 !IS_ALIGNED(start - expected_offset,
					     memory_block_size_bytes())))
			return NOTIFY_BAD;
	} else {
		id = virtio_mem_phys_to_bb_id(vm, start);
		/*
		 * In BBM, we only care about onlining/offlining happening
		 * within a single big block, we don't care about the
		 * actual granularity as we don't track individual Linux
		 * memory blocks.
		 */
		if (WARN_ON_ONCE(id != virtio_mem_phys_to_bb_id(vm, start + size - 1)))
			return NOTIFY_BAD;
	}

	/*
	 * Avoid circular locking lockdep warnings. We lock the mutex
	 * e.g., in MEM_GOING_ONLINE and unlock it in MEM_ONLINE. The
	 * blocking_notifier_call_chain() has it's own lock, which gets unlocked
	 * between both notifier calls and will bail out. False positive.
	 */
	lockdep_off();

	switch (action) {
	case MEM_GOING_OFFLINE:
		mutex_lock(&vm->hotplug_mutex);
		if (vm->removing) {
			rc = notifier_from_errno(-EBUSY);
			mutex_unlock(&vm->hotplug_mutex);
			break;
		}
		vm->hotplug_active = true;
		if (vm->in_sbm)
			virtio_mem_sbm_notify_going_offline(vm, id);
		else
			virtio_mem_bbm_notify_going_offline(vm, id,
							    mhp->start_pfn,
							    mhp->nr_pages);
		break;
	case MEM_GOING_ONLINE:
		mutex_lock(&vm->hotplug_mutex);
		if (vm->removing) {
			rc = notifier_from_errno(-EBUSY);
			mutex_unlock(&vm->hotplug_mutex);
			break;
		}
		vm->hotplug_active = true;
		if (vm->in_sbm)
			rc = virtio_mem_sbm_notify_going_online(vm, id);
		break;
	case MEM_OFFLINE:
		if (vm->in_sbm)
			virtio_mem_sbm_notify_offline(vm, id);

		atomic64_add(size, &vm->offline_size);
		/*
		 * Trigger the workqueue. Now that we have some offline memory,
		 * maybe we can handle pending unplug requests.
		 */
		if (!unplug_online)
			virtio_mem_retry(vm);

		vm->hotplug_active = false;
		mutex_unlock(&vm->hotplug_mutex);
		break;
	case MEM_ONLINE:
		if (vm->in_sbm)
			virtio_mem_sbm_notify_online(vm, id, mhp->start_pfn);

		atomic64_sub(size, &vm->offline_size);
		/*
		 * Start adding more memory once we onlined half of our
		 * threshold. Don't trigger if it's possibly due to our actipn
		 * (e.g., us adding memory which gets onlined immediately from
		 * the core).
		 */
		if (!atomic_read(&vm->wq_active) &&
		    virtio_mem_could_add_memory(vm, vm->offline_threshold / 2))
			virtio_mem_retry(vm);

		vm->hotplug_active = false;
		mutex_unlock(&vm->hotplug_mutex);
		break;
	case MEM_CANCEL_OFFLINE:
		if (!vm->hotplug_active)
			break;
		if (vm->in_sbm)
			virtio_mem_sbm_notify_cancel_offline(vm, id);
		else
			virtio_mem_bbm_notify_cancel_offline(vm, id,
							     mhp->start_pfn,
							     mhp->nr_pages);
		vm->hotplug_active = false;
		mutex_unlock(&vm->hotplug_mutex);
		break;
	case MEM_CANCEL_ONLINE:
		if (!vm->hotplug_active)
			break;
		vm->hotplug_active = false;
		mutex_unlock(&vm->hotplug_mutex);
		break;
	default:
		break;
	}

	lockdep_on();

	return rc;
}

/*
 * Set a range of pages PG_offline. Remember pages that were never onlined
 * (via generic_online_page()) using PageDirty().
 */
static void virtio_mem_set_fake_offline(unsigned long pfn,
					unsigned long nr_pages, bool onlined)
{
	page_offline_begin();
	for (; nr_pages--; pfn++) {
		struct page *page = pfn_to_page(pfn);

		__SetPageOffline(page);
		if (!onlined) {
			SetPageDirty(page);
			/* FIXME: remove after cleanups */
			ClearPageReserved(page);
		}
	}
	page_offline_end();
}

/*
 * Clear PG_offline from a range of pages. If the pages were never onlined,
 * (via generic_online_page()), clear PageDirty().
 */
static void virtio_mem_clear_fake_offline(unsigned long pfn,
					  unsigned long nr_pages, bool onlined)
{
	for (; nr_pages--; pfn++) {
		struct page *page = pfn_to_page(pfn);

		__ClearPageOffline(page);
		if (!onlined)
			ClearPageDirty(page);
	}
}

/*
 * Release a range of fake-offline pages to the buddy, effectively
 * fake-onlining them.
 */
static void virtio_mem_fake_online(unsigned long pfn, unsigned long nr_pages)
{
	unsigned long order = MAX_ORDER;
	unsigned long i;

	/*
	 * We might get called for ranges that don't cover properly aligned
	 * MAX_ORDER pages; however, we can only online properly aligned
	 * pages with an order of MAX_ORDER at maximum.
	 */
	while (!IS_ALIGNED(pfn | nr_pages, 1 << order))
		order--;

	for (i = 0; i < nr_pages; i += 1 << order) {
		struct page *page = pfn_to_page(pfn + i);

		/*
		 * If the page is PageDirty(), it was kept fake-offline when
		 * onlining the memory block. Otherwise, it was allocated
		 * using alloc_contig_range(). All pages in a subblock are
		 * alike.
		 */
		if (PageDirty(page)) {
			virtio_mem_clear_fake_offline(pfn + i, 1 << order, false);
			generic_online_page(page, order);
		} else {
			virtio_mem_clear_fake_offline(pfn + i, 1 << order, true);
			free_contig_range(pfn + i, 1 << order);
			adjust_managed_page_count(page, 1 << order);
		}
	}
}

/*
 * Try to allocate a range, marking pages fake-offline, effectively
 * fake-offlining them.
 */
static int virtio_mem_fake_offline(struct virtio_mem *vm, unsigned long pfn,
				   unsigned long nr_pages)
{
	const bool is_movable = is_zone_movable_page(pfn_to_page(pfn));
	int rc, retry_count;

	/*
	 * TODO: We want an alloc_contig_range() mode that tries to allocate
	 * harder (e.g., dealing with temporarily pinned pages, PCP), especially
	 * with ZONE_MOVABLE. So for now, retry a couple of times with
	 * ZONE_MOVABLE before giving up - because that zone is supposed to give
	 * some guarantees.
	 */
	for (retry_count = 0; retry_count < 5; retry_count++) {
		/*
		 * If the config changed, stop immediately and go back to the
		 * main loop: avoid trying to keep unplugging if the device
		 * might have decided to not remove any more memory.
		 */
		if (atomic_read(&vm->config_changed))
			return -EAGAIN;

		rc = alloc_contig_range(pfn, pfn + nr_pages, MIGRATE_MOVABLE,
					GFP_KERNEL);
		if (rc == -ENOMEM)
			/* whoops, out of memory */
			return rc;
		else if (rc && !is_movable)
			break;
		else if (rc)
			continue;

		virtio_mem_set_fake_offline(pfn, nr_pages, true);
		adjust_managed_page_count(pfn_to_page(pfn), -nr_pages);
		return 0;
	}

	return -EBUSY;
}

/*
 * Handle fake-offline pages when memory is going offline - such that the
 * pages can be skipped by mm-core when offlining.
 */
static void virtio_mem_fake_offline_going_offline(unsigned long pfn,
						  unsigned long nr_pages)
{
	struct page *page;
	unsigned long i;

	/*
	 * Drop our reference to the pages so the memory can get offlined
	 * and add the unplugged pages to the managed page counters (so
	 * offlining code can correctly subtract them again).
	 */
	adjust_managed_page_count(pfn_to_page(pfn), nr_pages);
	/* Drop our reference to the pages so the memory can get offlined. */
	for (i = 0; i < nr_pages; i++) {
		page = pfn_to_page(pfn + i);
		if (WARN_ON(!page_ref_dec_and_test(page)))
			dump_page(page, "fake-offline page referenced");
	}
}

/*
 * Handle fake-offline pages when memory offlining is canceled - to undo
 * what we did in virtio_mem_fake_offline_going_offline().
 */
static void virtio_mem_fake_offline_cancel_offline(unsigned long pfn,
						   unsigned long nr_pages)
{
	unsigned long i;

	/*
	 * Get the reference we dropped when going offline and subtract the
	 * unplugged pages from the managed page counters.
	 */
	adjust_managed_page_count(pfn_to_page(pfn), -nr_pages);
	for (i = 0; i < nr_pages; i++)
		page_ref_inc(pfn_to_page(pfn + i));
}

static void virtio_mem_online_page(struct virtio_mem *vm,
				   struct page *page, unsigned int order)
{
	const unsigned long start = page_to_phys(page);
	const unsigned long end = start + PFN_PHYS(1 << order);
	unsigned long addr, next, id, sb_id, count;
	bool do_online;

	/*
	 * We can get called with any order up to MAX_ORDER. If our subblock
	 * size is smaller than that and we have a mixture of plugged and
	 * unplugged subblocks within such a page, we have to process in
	 * smaller granularity. In that case we'll adjust the order exactly once
	 * within the loop.
	 */
	for (addr = start; addr < end; ) {
		next = addr + PFN_PHYS(1 << order);

		if (vm->in_sbm) {
			id = virtio_mem_phys_to_mb_id(addr);
			sb_id = virtio_mem_phys_to_sb_id(vm, addr);
			count = virtio_mem_phys_to_sb_id(vm, next - 1) - sb_id + 1;

			if (virtio_mem_sbm_test_sb_plugged(vm, id, sb_id, count)) {
				/* Fully plugged. */
				do_online = true;
			} else if (count == 1 ||
				   virtio_mem_sbm_test_sb_unplugged(vm, id, sb_id, count)) {
				/* Fully unplugged. */
				do_online = false;
			} else {
				/*
				 * Mixture, process sub-blocks instead. This
				 * will be at least the size of a pageblock.
				 * We'll run into this case exactly once.
				 */
				order = ilog2(vm->sbm.sb_size) - PAGE_SHIFT;
				do_online = virtio_mem_sbm_test_sb_plugged(vm, id, sb_id, 1);
				continue;
			}
		} else {
			/*
			 * If the whole block is marked fake offline, keep
			 * everything that way.
			 */
			id = virtio_mem_phys_to_bb_id(vm, addr);
			do_online = virtio_mem_bbm_get_bb_state(vm, id) !=
				    VIRTIO_MEM_BBM_BB_FAKE_OFFLINE;
		}

		if (do_online)
			generic_online_page(pfn_to_page(PFN_DOWN(addr)), order);
		else
			virtio_mem_set_fake_offline(PFN_DOWN(addr), 1 << order,
						    false);
		addr = next;
	}
}

static void virtio_mem_online_page_cb(struct page *page, unsigned int order)
{
	const unsigned long addr = page_to_phys(page);
	struct virtio_mem *vm;

	rcu_read_lock();
	list_for_each_entry_rcu(vm, &virtio_mem_devices, next) {
		/*
		 * Pages we're onlining will never cross memory blocks and,
		 * therefore, not virtio-mem devices.
		 */
		if (!virtio_mem_contains_range(vm, addr, PFN_PHYS(1 << order)))
			continue;

		/*
		 * virtio_mem_set_fake_offline() might sleep. We can safely
		 * drop the RCU lock at this point because the device
		 * cannot go away. See virtio_mem_remove() how races
		 * between memory onlining and device removal are handled.
		 */
		rcu_read_unlock();

		virtio_mem_online_page(vm, page, order);
		return;
	}
	rcu_read_unlock();

	/* not virtio-mem memory, but e.g., a DIMM. online it */
	generic_online_page(page, order);
}

/* Default error values to -ENOMEM - virtio_mem_run_wq expects certain rc only */
static int virtio_mem_convert_error_code(int rc)
{
	if (rc == -ENOSPC || rc == -ETXTBSY || rc == -EBUSY || rc == -EAGAIN)
		return rc;
	return -ENOMEM;
}

/*
 * mem-buf currently is handle based. This means we must break up requests into
 * the common unit size(device_block_size). GH_RM_MEM_DONATE does not actually require
 * tracking the handle, so this could be optimized further.
 *
 * This function must return one of ENOSPC, ETXTBSY, EBUSY, ENOMEM, EAGAIN
 */
static int virtio_mem_send_plug_request(struct virtio_mem *vm, uint64_t addr,
					uint64_t size, bool memmap)
{
	void *membuf;
	struct mem_buf_allocation_data alloc_data;
	u32 vmids[1];
	u32 perms[1] = {PERM_READ | PERM_WRITE | PERM_EXEC};
	struct gh_sgl_desc *gh_sgl;
	uint64_t orig_addr = addr;
	int ret;
	u64 block_size = vm->device_block_size;

	dev_dbg(&vm->vdev->dev, "plugging memory: 0x%llx - 0x%llx\n", addr,
		addr + size - 1);

	vmids[0] = mem_buf_current_vmid();

	alloc_data.size = block_size;
	alloc_data.nr_acl_entries = ARRAY_SIZE(vmids);
	alloc_data.vmids = vmids;
	alloc_data.perms = perms;
	alloc_data.trans_type = GH_RM_TRANS_TYPE_DONATE;
	gh_sgl = kzalloc(offsetof(struct gh_sgl_desc, sgl_entries[1]), GFP_KERNEL);
	if (!gh_sgl)
		return -ENOMEM;
	/* ipa_base/size configured below */
	gh_sgl->n_sgl_entries = 1;

	alloc_data.sgl_desc = gh_sgl;
	alloc_data.src_mem_type = MEM_BUF_BUDDY_MEM_TYPE;
	alloc_data.src_data = NULL;
	alloc_data.dst_mem_type = MEM_BUF_BUDDY_MEM_TYPE;
	alloc_data.dst_data = NULL;

	while (size) {
		gh_sgl->sgl_entries[0].ipa_base = addr;
		gh_sgl->sgl_entries[0].size = block_size;

		membuf = mem_buf_alloc(&alloc_data);
		if (IS_ERR(membuf)) {
			dev_err(&vm->vdev->dev, "mem_buf_alloc failed with %ld\n", PTR_ERR(membuf));
			ret = virtio_mem_convert_error_code(PTR_ERR(membuf));
			goto err_mem_buf_alloc;
		}

		xa_store(&xa_membuf, addr, membuf, GFP_KERNEL);
		if (!memmap)
			vm->plugged_size += block_size;

		size -= block_size;
		addr += block_size;
	}

	kfree(gh_sgl);
	return 0;

err_mem_buf_alloc:
	if (addr > orig_addr)
		virtio_mem_send_unplug_request(vm, orig_addr, addr - orig_addr, memmap);
	kfree(gh_sgl);
	return ret;
}

static int virtio_mem_send_unplug_request(struct virtio_mem *vm, uint64_t addr,
					  uint64_t size, bool memmap)
{
	void *membuf;
	u64 block_size = vm->device_block_size;
	uint64_t saved_size = size;

	dev_dbg(&vm->vdev->dev, "unplugging memory: 0x%llx - 0x%llx\n", addr,
		addr + size - 1);

	while (size) {
		membuf = xa_load(&xa_membuf, addr);
		if (WARN(!membuf, "No membuf for %llx\n", addr))
			return -EINVAL;

		mem_buf_free(membuf);

		size -= block_size;
		addr += block_size;
	}

	/*
	 * Only update if all successful to be in-line with how errors
	 * are handled by this function's callers
	 */
	if (!memmap)
		vm->plugged_size -= saved_size;
	return 0;
}

static int virtio_mem_send_unplug_all_request(struct virtio_mem *vm)
{
	dev_dbg(&vm->vdev->dev, "unplugging all memory");
	WARN_ON(1);
	return -EINVAL;
}

/*
 * Plug selected subblocks. Updates the plugged state, but not the state
 * of the memory block.
 */
static int virtio_mem_sbm_plug_sb(struct virtio_mem *vm, unsigned long mb_id,
				  int sb_id, int count)
{
	const uint64_t addr = virtio_mem_sb_id_to_phys(vm, mb_id, sb_id);
	const uint64_t size = count * vm->sbm.sb_size;
	int rc;

	rc = virtio_mem_send_plug_request(vm, addr, size, false);
	if (!rc)
		virtio_mem_sbm_set_sb_plugged(vm, mb_id, sb_id, count);
	return rc;
}

/*
 * Unplug selected subblocks. Updates the plugged state, but not the state
 * of the memory block.
 */
static int virtio_mem_sbm_unplug_sb(struct virtio_mem *vm, unsigned long mb_id,
				    int sb_id, int count)
{
	const uint64_t addr = virtio_mem_sb_id_to_phys(vm, mb_id, sb_id);
	const uint64_t size = count * vm->sbm.sb_size;
	int rc;

	rc = virtio_mem_send_unplug_request(vm, addr, size, false);
	if (!rc)
		virtio_mem_sbm_set_sb_unplugged(vm, mb_id, sb_id, count);
	return rc;
}

/*
 * Request to unplug a big block.
 *
 * Will not modify the state of the big block.
 */
static int virtio_mem_bbm_unplug_bb(struct virtio_mem *vm, unsigned long bb_id)
{
	const uint64_t addr = virtio_mem_bb_id_to_phys(vm, bb_id);
	const uint64_t size = vm->bbm.bb_size;

	return virtio_mem_send_unplug_request(vm, addr, size, false);
}

/*
 * Request to plug a big block.
 *
 * Will not modify the state of the big block.
 */
static int virtio_mem_bbm_plug_bb(struct virtio_mem *vm, unsigned long bb_id)
{
	const uint64_t addr = virtio_mem_bb_id_to_phys(vm, bb_id);
	const uint64_t size = vm->bbm.bb_size;

	return virtio_mem_send_plug_request(vm, addr, size, false);
}

/*
 * Unplug the desired number of plugged subblocks of a offline or not-added
 * memory block. Will fail if any subblock cannot get unplugged (instead of
 * skipping it).
 *
 * Will not modify the state of the memory block.
 *
 * Note: can fail after some subblocks were unplugged.
 */
static int virtio_mem_sbm_unplug_any_sb_raw(struct virtio_mem *vm,
					    unsigned long mb_id, uint64_t *nb_sb)
{
	int sb_id, count;
	int rc;

	sb_id = vm->sbm.sbs_per_mb - 1;
	while (*nb_sb) {
		/* Find the next candidate subblock */
		while (sb_id >= 0 &&
		       virtio_mem_sbm_test_sb_unplugged(vm, mb_id, sb_id, 1))
			sb_id--;
		if (sb_id < 0)
			break;
		/* Try to unplug multiple subblocks at a time */
		count = 1;
		while (count < *nb_sb && sb_id > 0 &&
		       virtio_mem_sbm_test_sb_plugged(vm, mb_id, sb_id - 1, 1)) {
			count++;
			sb_id--;
		}

		rc = virtio_mem_sbm_unplug_sb(vm, mb_id, sb_id, count);
		if (rc)
			return rc;
		*nb_sb -= count;
		sb_id--;
	}

	return 0;
}

/*
 * Unplug all plugged subblocks of an offline or not-added memory block.
 *
 * Will not modify the state of the memory block.
 *
 * Note: can fail after some subblocks were unplugged.
 */
static int virtio_mem_sbm_unplug_mb(struct virtio_mem *vm, unsigned long mb_id)
{
	uint64_t nb_sb = vm->sbm.sbs_per_mb;

	return virtio_mem_sbm_unplug_any_sb_raw(vm, mb_id, &nb_sb);
}

/*
 * Prepare tracking data for the next memory block.
 */
static int virtio_mem_sbm_prepare_next_mb(struct virtio_mem *vm,
					  unsigned long *mb_id)
{
	int rc;

	if (vm->sbm.next_mb_id > vm->sbm.last_usable_mb_id)
		return -ENOSPC;

	/* Resize the state array if required. */
	rc = virtio_mem_sbm_mb_states_prepare_next_mb(vm);
	if (rc)
		return rc;

	/* Resize the subblock bitmap if required. */
	rc = virtio_mem_sbm_sb_states_prepare_next_mb(vm);
	if (rc)
		return rc;

	vm->sbm.mb_count[VIRTIO_MEM_SBM_MB_UNUSED]++;
	*mb_id = vm->sbm.next_mb_id++;
	return 0;
}

/*
 * Try to plug the desired number of subblocks and add the memory block
 * to Linux.
 *
 * Will modify the state of the memory block.
 */
static int virtio_mem_sbm_plug_and_add_mb(struct virtio_mem *vm,
					  unsigned long mb_id, uint64_t *nb_sb)
{
	const int count = min_t(int, *nb_sb, vm->sbm.sbs_per_mb);
	int rc;

	if (WARN_ON_ONCE(!count))
		return -EINVAL;

	/*
	 * Plug the requested number of subblocks before adding it to linux,
	 * so that onlining will directly online all plugged subblocks.
	 */
	rc = virtio_mem_sbm_plug_sb(vm, mb_id, 0, count);
	if (rc)
		return rc;

	/*
	 * Mark the block properly offline before adding it to Linux,
	 * so the memory notifiers will find the block in the right state.
	 */
	if (count == vm->sbm.sbs_per_mb)
		virtio_mem_sbm_set_mb_state(vm, mb_id,
					    VIRTIO_MEM_SBM_MB_OFFLINE);
	else
		virtio_mem_sbm_set_mb_state(vm, mb_id,
					    VIRTIO_MEM_SBM_MB_OFFLINE_PARTIAL);

	/* Add the memory block to linux - if that fails, try to unplug. */
	rc = virtio_mem_sbm_add_mb(vm, mb_id);
	if (rc) {
		int new_state = VIRTIO_MEM_SBM_MB_UNUSED;

		if (virtio_mem_sbm_unplug_sb(vm, mb_id, 0, count))
			new_state = VIRTIO_MEM_SBM_MB_PLUGGED;
		virtio_mem_sbm_set_mb_state(vm, mb_id, new_state);
		return rc;
	}

	*nb_sb -= count;
	return 0;
}

/*
 * Try to plug the desired number of subblocks of a memory block that
 * is already added to Linux.
 *
 * Will modify the state of the memory block.
 *
 * Note: Can fail after some subblocks were successfully plugged.
 */
static int virtio_mem_sbm_plug_any_sb(struct virtio_mem *vm,
				      unsigned long mb_id, uint64_t *nb_sb)
{
	const int old_state = virtio_mem_sbm_get_mb_state(vm, mb_id);
	unsigned long pfn, nr_pages;
	int sb_id, count;
	int rc;

	if (WARN_ON_ONCE(!*nb_sb))
		return -EINVAL;

	while (*nb_sb) {
		sb_id = virtio_mem_sbm_first_unplugged_sb(vm, mb_id);
		if (sb_id >= vm->sbm.sbs_per_mb)
			break;
		count = 1;
		while (count < *nb_sb &&
		       sb_id + count < vm->sbm.sbs_per_mb &&
		       !virtio_mem_sbm_test_sb_plugged(vm, mb_id, sb_id + count, 1))
			count++;

		rc = virtio_mem_sbm_plug_sb(vm, mb_id, sb_id, count);
		if (rc)
			return rc;
		*nb_sb -= count;
		if (old_state == VIRTIO_MEM_SBM_MB_OFFLINE_PARTIAL)
			continue;

		/* fake-online the pages if the memory block is online */
		pfn = PFN_DOWN(virtio_mem_sb_id_to_phys(vm, mb_id, sb_id));
		nr_pages = PFN_DOWN(count * vm->sbm.sb_size);
		virtio_mem_fake_online(pfn, nr_pages);
	}

	if (virtio_mem_sbm_test_sb_plugged(vm, mb_id, 0, vm->sbm.sbs_per_mb))
		virtio_mem_sbm_set_mb_state(vm, mb_id, old_state - 1);

	return 0;
}

static int virtio_mem_sbm_plug_request(struct virtio_mem *vm, uint64_t diff)
{
	const int mb_states[] = {
		VIRTIO_MEM_SBM_MB_KERNEL_PARTIAL,
		VIRTIO_MEM_SBM_MB_MOVABLE_PARTIAL,
		VIRTIO_MEM_SBM_MB_OFFLINE_PARTIAL,
	};
	uint64_t nb_sb = diff / vm->sbm.sb_size;
	unsigned long mb_id;
	int rc, i;

	if (!nb_sb)
		return 0;

	/* Don't race with onlining/offlining */
	mutex_lock(&vm->hotplug_mutex);

	for (i = 0; i < ARRAY_SIZE(mb_states); i++) {
		virtio_mem_sbm_for_each_mb(vm, mb_id, mb_states[i]) {
			rc = virtio_mem_sbm_plug_any_sb(vm, mb_id, &nb_sb);
			if (rc || !nb_sb)
				goto out_unlock;
			cond_resched();
		}
	}

	/*
	 * We won't be working on online/offline memory blocks from this point,
	 * so we can't race with memory onlining/offlining. Drop the mutex.
	 */
	mutex_unlock(&vm->hotplug_mutex);

	/* Try to plug and add unused blocks */
	virtio_mem_sbm_for_each_mb(vm, mb_id, VIRTIO_MEM_SBM_MB_UNUSED) {
		if (!virtio_mem_could_add_memory(vm, memory_block_size_bytes()))
			return -ENOSPC;

		rc = virtio_mem_sbm_plug_and_add_mb(vm, mb_id, &nb_sb);
		if (rc || !nb_sb)
			return rc;
		cond_resched();
	}

	/* Try to prepare, plug and add new blocks */
	while (nb_sb) {
		if (!virtio_mem_could_add_memory(vm, memory_block_size_bytes()))
			return -ENOSPC;

		rc = virtio_mem_sbm_prepare_next_mb(vm, &mb_id);
		if (rc)
			return rc;
		rc = virtio_mem_sbm_plug_and_add_mb(vm, mb_id, &nb_sb);
		if (rc)
			return rc;
		cond_resched();
	}

	return 0;
out_unlock:
	mutex_unlock(&vm->hotplug_mutex);
	return rc;
}

/*
 * Plug a big block and add it to Linux.
 *
 * Will modify the state of the big block.
 */
static int virtio_mem_bbm_plug_and_add_bb(struct virtio_mem *vm,
					  unsigned long bb_id)
{
	int rc;

	if (WARN_ON_ONCE(virtio_mem_bbm_get_bb_state(vm, bb_id) !=
			 VIRTIO_MEM_BBM_BB_UNUSED))
		return -EINVAL;

	rc = virtio_mem_bbm_plug_bb(vm, bb_id);
	if (rc)
		return rc;
	virtio_mem_bbm_set_bb_state(vm, bb_id, VIRTIO_MEM_BBM_BB_ADDED);

	rc = virtio_mem_bbm_add_bb(vm, bb_id);
	if (rc) {
		if (!virtio_mem_bbm_unplug_bb(vm, bb_id))
			virtio_mem_bbm_set_bb_state(vm, bb_id,
						    VIRTIO_MEM_BBM_BB_UNUSED);
		else
			/* Retry from the main loop. */
			virtio_mem_bbm_set_bb_state(vm, bb_id,
						    VIRTIO_MEM_BBM_BB_PLUGGED);
		return rc;
	}
	return 0;
}

/*
 * Prepare tracking data for the next big block.
 */
static int virtio_mem_bbm_prepare_next_bb(struct virtio_mem *vm,
					  unsigned long *bb_id)
{
	int rc;

	if (vm->bbm.next_bb_id > vm->bbm.last_usable_bb_id)
		return -ENOSPC;

	/* Resize the big block state array if required. */
	rc = virtio_mem_bbm_bb_states_prepare_next_bb(vm);
	if (rc)
		return rc;

	vm->bbm.bb_count[VIRTIO_MEM_BBM_BB_UNUSED]++;
	*bb_id = vm->bbm.next_bb_id;
	vm->bbm.next_bb_id++;
	return 0;
}

static int virtio_mem_bbm_plug_request(struct virtio_mem *vm, uint64_t diff)
{
	uint64_t nb_bb = diff / vm->bbm.bb_size;
	unsigned long bb_id;
	int rc;

	if (!nb_bb)
		return 0;

	/* Try to plug and add unused big blocks */
	virtio_mem_bbm_for_each_bb(vm, bb_id, VIRTIO_MEM_BBM_BB_UNUSED) {
		if (!virtio_mem_could_add_memory(vm, vm->bbm.bb_size))
			return -ENOSPC;

		rc = virtio_mem_bbm_plug_and_add_bb(vm, bb_id);
		if (!rc)
			nb_bb--;
		if (rc || !nb_bb)
			return rc;
		cond_resched();
	}

	/* Try to prepare, plug and add new big blocks */
	while (nb_bb) {
		if (!virtio_mem_could_add_memory(vm, vm->bbm.bb_size))
			return -ENOSPC;

		rc = virtio_mem_bbm_prepare_next_bb(vm, &bb_id);
		if (rc)
			return rc;
		rc = virtio_mem_bbm_plug_and_add_bb(vm, bb_id);
		if (!rc)
			nb_bb--;
		if (rc)
			return rc;
		cond_resched();
	}

	return 0;
}

/*
 * Try to plug the requested amount of memory.
 */
static int virtio_mem_plug_request(struct virtio_mem *vm, uint64_t diff)
{
	if (vm->in_sbm)
		return virtio_mem_sbm_plug_request(vm, diff);
	return virtio_mem_bbm_plug_request(vm, diff);
}

/*
 * Unplug the desired number of plugged subblocks of an offline memory block.
 * Will fail if any subblock cannot get unplugged (instead of skipping it).
 *
 * Will modify the state of the memory block. Might temporarily drop the
 * hotplug_mutex.
 *
 * Note: Can fail after some subblocks were successfully unplugged.
 */
static int virtio_mem_sbm_unplug_any_sb_offline(struct virtio_mem *vm,
						unsigned long mb_id,
						uint64_t *nb_sb)
{
	int rc;

	rc = virtio_mem_sbm_unplug_any_sb_raw(vm, mb_id, nb_sb);

	/* some subblocks might have been unplugged even on failure */
	if (!virtio_mem_sbm_test_sb_plugged(vm, mb_id, 0, vm->sbm.sbs_per_mb))
		virtio_mem_sbm_set_mb_state(vm, mb_id,
					    VIRTIO_MEM_SBM_MB_OFFLINE_PARTIAL);
	if (rc)
		return rc;

	if (virtio_mem_sbm_test_sb_unplugged(vm, mb_id, 0, vm->sbm.sbs_per_mb)) {
		/*
		 * Remove the block from Linux - this should never fail.
		 * Hinder the block from getting onlined by marking it
		 * unplugged. Temporarily drop the mutex, so
		 * any pending GOING_ONLINE requests can be serviced/rejected.
		 */
		virtio_mem_sbm_set_mb_state(vm, mb_id,
					    VIRTIO_MEM_SBM_MB_UNUSED);

		mutex_unlock(&vm->hotplug_mutex);
		rc = virtio_mem_sbm_remove_mb(vm, mb_id);
		BUG_ON(rc);
		mutex_lock(&vm->hotplug_mutex);
	}
	return 0;
}

/*
 * Unplug the given plugged subblocks of an online memory block.
 *
 * Will modify the state of the memory block.
 */
static int virtio_mem_sbm_unplug_sb_online(struct virtio_mem *vm,
					   unsigned long mb_id, int sb_id,
					   int count)
{
	const unsigned long nr_pages = PFN_DOWN(vm->sbm.sb_size) * count;
	const int old_state = virtio_mem_sbm_get_mb_state(vm, mb_id);
	unsigned long start_pfn;
	int rc;

	start_pfn = PFN_DOWN(virtio_mem_sb_id_to_phys(vm, mb_id, sb_id));

	rc = virtio_mem_fake_offline(vm, start_pfn, nr_pages);
	if (rc)
		return rc;

	/* Try to unplug the allocated memory */
	rc = virtio_mem_sbm_unplug_sb(vm, mb_id, sb_id, count);
	if (rc) {
		/* Return the memory to the buddy. */
		virtio_mem_fake_online(start_pfn, nr_pages);
		return rc;
	}

	switch (old_state) {
	case VIRTIO_MEM_SBM_MB_KERNEL:
		virtio_mem_sbm_set_mb_state(vm, mb_id,
					    VIRTIO_MEM_SBM_MB_KERNEL_PARTIAL);
		break;
	case VIRTIO_MEM_SBM_MB_MOVABLE:
		virtio_mem_sbm_set_mb_state(vm, mb_id,
					    VIRTIO_MEM_SBM_MB_MOVABLE_PARTIAL);
		break;
	}

	return 0;
}

/*
 * Unplug the desired number of plugged subblocks of an online memory block.
 * Will skip subblock that are busy.
 *
 * Will modify the state of the memory block. Might temporarily drop the
 * hotplug_mutex.
 *
 * Note: Can fail after some subblocks were successfully unplugged. Can
 *       return 0 even if subblocks were busy and could not get unplugged.
 */
static int virtio_mem_sbm_unplug_any_sb_online(struct virtio_mem *vm,
					       unsigned long mb_id,
					       uint64_t *nb_sb)
{
	int rc, sb_id;

	/* If possible, try to unplug the complete block in one shot. */
	if (*nb_sb >= vm->sbm.sbs_per_mb &&
	    virtio_mem_sbm_test_sb_plugged(vm, mb_id, 0, vm->sbm.sbs_per_mb)) {
		rc = virtio_mem_sbm_unplug_sb_online(vm, mb_id, 0,
						     vm->sbm.sbs_per_mb);
		if (!rc) {
			*nb_sb -= vm->sbm.sbs_per_mb;
			goto unplugged;
		} else if (rc != -EBUSY && rc != -ENOMEM)
			return rc;
	}

	/* Fallback to single subblocks. */
	for (sb_id = vm->sbm.sbs_per_mb - 1; sb_id >= 0 && *nb_sb; sb_id--) {
		/* Find the next candidate subblock */
		while (sb_id >= 0 &&
		       !virtio_mem_sbm_test_sb_plugged(vm, mb_id, sb_id, 1))
			sb_id--;
		if (sb_id < 0)
			break;

		rc = virtio_mem_sbm_unplug_sb_online(vm, mb_id, sb_id, 1);
		if (rc == -EBUSY)
			continue;
		else if (rc)
			return rc;
		*nb_sb -= 1;
	}

unplugged:
	rc = virtio_mem_sbm_try_remove_unplugged_mb(vm, mb_id);
	if (rc)
		vm->sbm.have_unplugged_mb = 1;
	/* Ignore errors, this is not critical. We'll retry later. */
	return 0;
}

/*
 * Unplug the desired number of plugged subblocks of a memory block that is
 * already added to Linux. Will skip subblock of online memory blocks that are
 * busy (by the OS). Will fail if any subblock that's not busy cannot get
 * unplugged.
 *
 * Will modify the state of the memory block. Might temporarily drop the
 * hotplug_mutex.
 *
 * Note: Can fail after some subblocks were successfully unplugged. Can
 *       return 0 even if subblocks were busy and could not get unplugged.
 */
static int virtio_mem_sbm_unplug_any_sb(struct virtio_mem *vm,
					unsigned long mb_id,
					uint64_t *nb_sb)
{
	const int old_state = virtio_mem_sbm_get_mb_state(vm, mb_id);

	switch (old_state) {
	case VIRTIO_MEM_SBM_MB_KERNEL_PARTIAL:
	case VIRTIO_MEM_SBM_MB_KERNEL:
	case VIRTIO_MEM_SBM_MB_MOVABLE_PARTIAL:
	case VIRTIO_MEM_SBM_MB_MOVABLE:
		return virtio_mem_sbm_unplug_any_sb_online(vm, mb_id, nb_sb);
	case VIRTIO_MEM_SBM_MB_OFFLINE_PARTIAL:
	case VIRTIO_MEM_SBM_MB_OFFLINE:
		return virtio_mem_sbm_unplug_any_sb_offline(vm, mb_id, nb_sb);
	}
	return -EINVAL;
}

static int virtio_mem_sbm_unplug_request(struct virtio_mem *vm, uint64_t diff)
{
	const int mb_states[] = {
		VIRTIO_MEM_SBM_MB_OFFLINE_PARTIAL,
		VIRTIO_MEM_SBM_MB_OFFLINE,
		VIRTIO_MEM_SBM_MB_MOVABLE_PARTIAL,
		VIRTIO_MEM_SBM_MB_KERNEL_PARTIAL,
		VIRTIO_MEM_SBM_MB_MOVABLE,
		VIRTIO_MEM_SBM_MB_KERNEL,
	};
	uint64_t nb_sb = diff / vm->sbm.sb_size;
	unsigned long mb_id;
	int rc, i;

	if (!nb_sb)
		return 0;

	/*
	 * We'll drop the mutex a couple of times when it is safe to do so.
	 * This might result in some blocks switching the state (online/offline)
	 * and we could miss them in this run - we will retry again later.
	 */
	mutex_lock(&vm->hotplug_mutex);

	/*
	 * We try unplug from partially plugged blocks first, to try removing
	 * whole memory blocks along with metadata. We prioritize ZONE_MOVABLE
	 * as it's more reliable to unplug memory and remove whole memory
	 * blocks, and we don't want to trigger a zone imbalances by
	 * accidentially removing too much kernel memory.
	 */
	for (i = 0; i < ARRAY_SIZE(mb_states); i++) {
		virtio_mem_sbm_for_each_mb_rev(vm, mb_id, mb_states[i]) {
			rc = virtio_mem_sbm_unplug_any_sb(vm, mb_id, &nb_sb);
			if (rc || !nb_sb)
				goto out_unlock;
			mutex_unlock(&vm->hotplug_mutex);
			cond_resched();
			mutex_lock(&vm->hotplug_mutex);
		}
		if (!unplug_online && i == 1) {
			mutex_unlock(&vm->hotplug_mutex);
			return 0;
		}
	}

	mutex_unlock(&vm->hotplug_mutex);
	return nb_sb ? -EBUSY : 0;
out_unlock:
	mutex_unlock(&vm->hotplug_mutex);
	return rc;
}

/*
 * Try to offline and remove a big block from Linux and unplug it. Will fail
 * with -EBUSY if some memory is busy and cannot get unplugged.
 *
 * Will modify the state of the memory block. Might temporarily drop the
 * hotplug_mutex.
 */
static int virtio_mem_bbm_offline_remove_and_unplug_bb(struct virtio_mem *vm,
						       unsigned long bb_id)
{
	const unsigned long start_pfn = PFN_DOWN(virtio_mem_bb_id_to_phys(vm, bb_id));
	const unsigned long nr_pages = PFN_DOWN(vm->bbm.bb_size);
	unsigned long end_pfn = start_pfn + nr_pages;
	unsigned long pfn;
	struct page *page;
	int rc;

	if (WARN_ON_ONCE(virtio_mem_bbm_get_bb_state(vm, bb_id) !=
			 VIRTIO_MEM_BBM_BB_ADDED))
		return -EINVAL;

	/*
	 * Start by fake-offlining all memory. Once we marked the device
	 * block as fake-offline, all newly onlined memory will
	 * automatically be kept fake-offline. Protect from concurrent
	 * onlining/offlining until we have a consistent state.
	 */
	mutex_lock(&vm->hotplug_mutex);
	virtio_mem_bbm_set_bb_state(vm, bb_id, VIRTIO_MEM_BBM_BB_FAKE_OFFLINE);

	for (pfn = start_pfn; pfn < end_pfn; pfn += PAGES_PER_SECTION) {
		page = pfn_to_online_page(pfn);
		if (!page)
			continue;

		rc = virtio_mem_fake_offline(vm, pfn, PAGES_PER_SECTION);
		if (rc) {
			end_pfn = pfn;
			goto rollback;
		}
	}
	mutex_unlock(&vm->hotplug_mutex);

	rc = virtio_mem_bbm_offline_and_remove_bb(vm, bb_id);
	if (rc) {
		mutex_lock(&vm->hotplug_mutex);
		goto rollback;
	}

	rc = virtio_mem_bbm_unplug_bb(vm, bb_id);
	if (rc)
		virtio_mem_bbm_set_bb_state(vm, bb_id,
					    VIRTIO_MEM_BBM_BB_PLUGGED);
	else
		virtio_mem_bbm_set_bb_state(vm, bb_id,
					    VIRTIO_MEM_BBM_BB_UNUSED);
	return rc;

rollback:
	for (pfn = start_pfn; pfn < end_pfn; pfn += PAGES_PER_SECTION) {
		page = pfn_to_online_page(pfn);
		if (!page)
			continue;
		virtio_mem_fake_online(pfn, PAGES_PER_SECTION);
	}
	virtio_mem_bbm_set_bb_state(vm, bb_id, VIRTIO_MEM_BBM_BB_ADDED);
	mutex_unlock(&vm->hotplug_mutex);
	return rc;
}

/*
 * Test if a big block is completely offline.
 */
static bool virtio_mem_bbm_bb_is_offline(struct virtio_mem *vm,
					 unsigned long bb_id)
{
	const unsigned long start_pfn = PFN_DOWN(virtio_mem_bb_id_to_phys(vm, bb_id));
	const unsigned long nr_pages = PFN_DOWN(vm->bbm.bb_size);
	unsigned long pfn;

	for (pfn = start_pfn; pfn < start_pfn + nr_pages;
	     pfn += PAGES_PER_SECTION) {
		if (pfn_to_online_page(pfn))
			return false;
	}

	return true;
}

/*
 * Test if a big block is completely onlined to ZONE_MOVABLE (or offline).
 */
static bool virtio_mem_bbm_bb_is_movable(struct virtio_mem *vm,
					 unsigned long bb_id)
{
	const unsigned long start_pfn = PFN_DOWN(virtio_mem_bb_id_to_phys(vm, bb_id));
	const unsigned long nr_pages = PFN_DOWN(vm->bbm.bb_size);
	struct page *page;
	unsigned long pfn;

	for (pfn = start_pfn; pfn < start_pfn + nr_pages;
	     pfn += PAGES_PER_SECTION) {
		page = pfn_to_online_page(pfn);
		if (!page)
			continue;
		if (!is_zone_movable_page(page))
			return false;
	}

	return true;
}

static int virtio_mem_bbm_unplug_request(struct virtio_mem *vm, uint64_t diff)
{
	uint64_t nb_bb = diff / vm->bbm.bb_size;
	uint64_t bb_id;
	int rc, i;

	if (!nb_bb)
		return 0;

	/*
	 * Try to unplug big blocks. Similar to SBM, start with offline
	 * big blocks.
	 */
	for (i = 0; i < 3; i++) {
		virtio_mem_bbm_for_each_bb_rev(vm, bb_id, VIRTIO_MEM_BBM_BB_ADDED) {
			cond_resched();

			/*
			 * As we're holding no locks, these checks are racy,
			 * but we don't care.
			 */
			if (i == 0 && !virtio_mem_bbm_bb_is_offline(vm, bb_id))
				continue;
			if (i == 1 && !virtio_mem_bbm_bb_is_movable(vm, bb_id))
				continue;
			rc = virtio_mem_bbm_offline_remove_and_unplug_bb(vm, bb_id);
			if (rc == -EBUSY)
				continue;
			if (!rc)
				nb_bb--;
			if (rc || !nb_bb)
				return rc;
		}
		if (i == 0 && !unplug_online)
			return 0;
	}

	return nb_bb ? -EBUSY : 0;
}

/*
 * Try to unplug the requested amount of memory.
 */
static int virtio_mem_unplug_request(struct virtio_mem *vm, uint64_t diff)
{
	if (vm->in_sbm)
		return virtio_mem_sbm_unplug_request(vm, diff);
	return virtio_mem_bbm_unplug_request(vm, diff);
}

/*
 * Try to unplug all blocks that couldn't be unplugged before, for example,
 * because the hypervisor was busy. Further, offline and remove any memory
 * blocks where we previously failed.
 */
static int virtio_mem_cleanup_pending_mb(struct virtio_mem *vm)
{
	unsigned long id;
	int rc = 0;

	if (!vm->in_sbm) {
		virtio_mem_bbm_for_each_bb(vm, id,
					   VIRTIO_MEM_BBM_BB_PLUGGED) {
			rc = virtio_mem_bbm_unplug_bb(vm, id);
			if (rc)
				return rc;
			virtio_mem_bbm_set_bb_state(vm, id,
						    VIRTIO_MEM_BBM_BB_UNUSED);
		}
		return 0;
	}

	virtio_mem_sbm_for_each_mb(vm, id, VIRTIO_MEM_SBM_MB_PLUGGED) {
		rc = virtio_mem_sbm_unplug_mb(vm, id);
		if (rc)
			return rc;
		virtio_mem_sbm_set_mb_state(vm, id,
					    VIRTIO_MEM_SBM_MB_UNUSED);
	}

	if (!vm->sbm.have_unplugged_mb)
		return 0;

	/*
	 * Let's retry (offlining and) removing completely unplugged Linux
	 * memory blocks.
	 */
	vm->sbm.have_unplugged_mb = false;

	mutex_lock(&vm->hotplug_mutex);
	virtio_mem_sbm_for_each_mb(vm, id, VIRTIO_MEM_SBM_MB_MOVABLE_PARTIAL)
		rc |= virtio_mem_sbm_try_remove_unplugged_mb(vm, id);
	virtio_mem_sbm_for_each_mb(vm, id, VIRTIO_MEM_SBM_MB_KERNEL_PARTIAL)
		rc |= virtio_mem_sbm_try_remove_unplugged_mb(vm, id);
	virtio_mem_sbm_for_each_mb(vm, id, VIRTIO_MEM_SBM_MB_OFFLINE_PARTIAL)
		rc |= virtio_mem_sbm_try_remove_unplugged_mb(vm, id);
	mutex_unlock(&vm->hotplug_mutex);

	if (rc)
		vm->sbm.have_unplugged_mb = true;
	/* Ignore errors, this is not critical. We'll retry later. */
	return 0;
}

/*
 * Update all parts of the config that could have changed.
 */
static void virtio_mem_refresh_config(struct virtio_mem *vm)
{
	const struct range pluggable_range = mhp_get_pluggable_range(true);
	uint64_t end_addr;

	/* calculate the last usable memory block id */
	/*
	 * Although the end address never changes with virtio-mem platform device
	 * this is the only place with the previous code flow where last_usable_mb_id
	 * is set. So, keep it here for now to minimize diff.
	 */
	end_addr = min(vm->addr + vm->region_size - 1,
		       pluggable_range.end);

	if (vm->in_sbm) {
		vm->sbm.last_usable_mb_id = virtio_mem_phys_to_mb_id(end_addr);
		if (!IS_ALIGNED(end_addr + 1, memory_block_size_bytes()))
			vm->sbm.last_usable_mb_id--;
	} else {
		vm->bbm.last_usable_bb_id = virtio_mem_phys_to_bb_id(vm,
								     end_addr);
		if (!IS_ALIGNED(end_addr + 1, vm->bbm.bb_size))
			vm->bbm.last_usable_bb_id--;
	}
	/*
	 * If we cannot plug any of our device memory (e.g., nothing in the
	 * usable region is addressable), the last usable memory block id will
	 * be smaller than the first usable memory block id. We'll stop
	 * attempting to add memory with -ENOSPC from our main loop.
	 */

	/* see if there is a request to change the size */
	/*
	 * vm->requested_size is set by caller of virtio_mem_config_changed()
	 * with virtio platform dev
	 */
	dev_info(&vm->vdev->dev, "plugged size: 0x%llx", vm->plugged_size);
	dev_info(&vm->vdev->dev, "requested size: 0x%llx", vm->requested_size);
}

/*
 * Workqueue function for handling plug/unplug requests and config updates.
 */
static void virtio_mem_run_wq(struct work_struct *work)
{
	struct virtio_mem *vm = container_of(work, struct virtio_mem, wq);
	uint64_t diff;
	int rc;
	unsigned int noreclaim_flag;

	if (unlikely(vm->in_kdump)) {
		dev_warn_once(&vm->vdev->dev,
			     "unexpected workqueue run in kdump kernel\n");
		return;
	}

	hrtimer_cancel(&vm->retry_timer);

	if (vm->broken)
		return;

	atomic_set(&vm->wq_active, 1);

retry:
	rc = 0;

	/* Make sure we start with a clean state if there are leftovers. */
	if (unlikely(vm->unplug_all_required))
		rc = virtio_mem_send_unplug_all_request(vm);

	if (atomic_read(&vm->config_changed)) {
		atomic_set(&vm->config_changed, 0);
		virtio_mem_refresh_config(vm);
	}

	/* Cleanup any leftovers from previous runs */
	if (!rc)
		rc = virtio_mem_cleanup_pending_mb(vm);

	if (!rc && vm->requested_size != vm->plugged_size) {
		if (vm->requested_size > vm->plugged_size) {
			diff = vm->requested_size - vm->plugged_size;
			noreclaim_flag = memalloc_noreclaim_save();
			rc = virtio_mem_plug_request(vm, diff);
			memalloc_noreclaim_restore(noreclaim_flag);
		} else {
			diff = vm->plugged_size - vm->requested_size;
			rc = virtio_mem_unplug_request(vm, diff);
		}
	}

	/*
	 * Keep retrying to offline and remove completely unplugged Linux
	 * memory blocks.
	 */
	if (!rc && vm->in_sbm && vm->sbm.have_unplugged_mb)
		rc = -EBUSY;

	switch (rc) {
	case 0:
		vm->retry_timer_ms = VIRTIO_MEM_RETRY_TIMER_MIN_MS;
		break;
	case -ENOSPC:
		/*
		 * We cannot add any more memory (alignment, physical limit)
		 * or we have too many offline memory blocks.
		 */
		break;
	case -ETXTBSY:
		/*
		 * The hypervisor cannot process our request right now
		 * (e.g., out of memory, migrating);
		 */
	case -EBUSY:
		/*
		 * We cannot free up any memory to unplug it (all plugged memory
		 * is busy).
		 */
	case -ENOMEM:
		/* Out of memory, try again later. */
		hrtimer_start(&vm->retry_timer, ms_to_ktime(vm->retry_timer_ms),
			      HRTIMER_MODE_REL);
		break;
	case -EAGAIN:
		/* Retry immediately (e.g., the config changed). */
		goto retry;
	default:
		/* Unknown error, mark as broken */
		dev_err(&vm->vdev->dev,
			"unknown error, marking device broken: %d\n", rc);
		vm->broken = true;
	}

	atomic_set(&vm->wq_active, 0);
}

static enum hrtimer_restart virtio_mem_timer_expired(struct hrtimer *timer)
{
	struct virtio_mem *vm = container_of(timer, struct virtio_mem,
					     retry_timer);

	virtio_mem_retry(vm);
	vm->retry_timer_ms = min_t(unsigned int, vm->retry_timer_ms * 2,
				   VIRTIO_MEM_RETRY_TIMER_MAX_MS);
	return HRTIMER_NORESTART;
}

static int virtio_mem_init_hotplug(struct virtio_mem *vm)
{
	const struct range pluggable_range = mhp_get_pluggable_range(true);
	uint64_t unit_pages, sb_size, addr;
	int rc;

	/* bad device setup - warn only */
	if (!IS_ALIGNED(vm->addr, memory_block_size_bytes()))
		dev_warn(&vm->vdev->dev,
			 "The alignment of the physical start address can make some memory unusable.\n");
	if (!IS_ALIGNED(vm->addr + vm->region_size, memory_block_size_bytes()))
		dev_warn(&vm->vdev->dev,
			 "The alignment of the physical end address can make some memory unusable.\n");
	if (vm->addr < pluggable_range.start ||
	    vm->addr + vm->region_size - 1 > pluggable_range.end)
		dev_warn(&vm->vdev->dev,
			 "Some device memory is not addressable/pluggable. This can make some memory unusable.\n");

	/* Prepare the offline threshold - make sure we can add two blocks. */
	vm->offline_threshold = max_t(uint64_t, 2 * memory_block_size_bytes(),
				      VIRTIO_MEM_DEFAULT_OFFLINE_THRESHOLD);

	/*
	 * alloc_contig_range() works reliably with pageblock
	 * granularity on ZONE_NORMAL, use pageblock_nr_pages.
	 */
	sb_size = PAGE_SIZE * pageblock_nr_pages;
	sb_size = max_t(uint64_t, vm->device_block_size, sb_size);

	if (sb_size < memory_block_size_bytes() && !force_bbm) {
		/* SBM: At least two subblocks per Linux memory block. */
		vm->in_sbm = true;
		vm->sbm.sb_size = sb_size;
		vm->sbm.sbs_per_mb = memory_block_size_bytes() /
				     vm->sbm.sb_size;

		/* Round up to the next full memory block */
		addr = max_t(uint64_t, vm->addr, pluggable_range.start) +
		       memory_block_size_bytes() - 1;
		vm->sbm.first_mb_id = virtio_mem_phys_to_mb_id(addr);
		vm->sbm.next_mb_id = vm->sbm.first_mb_id;
	} else {
		/* BBM: At least one Linux memory block. */
		vm->bbm.bb_size = max_t(uint64_t, vm->device_block_size,
					memory_block_size_bytes());

		if (bbm_block_size) {
			if (!is_power_of_2(bbm_block_size)) {
				dev_warn(&vm->vdev->dev,
					 "bbm_block_size is not a power of 2");
			} else if (bbm_block_size < vm->bbm.bb_size) {
				dev_warn(&vm->vdev->dev,
					 "bbm_block_size is too small");
			} else {
				vm->bbm.bb_size = bbm_block_size;
			}
		}

		/* Round up to the next aligned big block */
		addr = max_t(uint64_t, vm->addr, pluggable_range.start) +
		       vm->bbm.bb_size - 1;
		vm->bbm.first_bb_id = virtio_mem_phys_to_bb_id(vm, addr);
		vm->bbm.next_bb_id = vm->bbm.first_bb_id;

		/* Make sure we can add two big blocks. */
		vm->offline_threshold = max_t(uint64_t, 2 * vm->bbm.bb_size,
					      vm->offline_threshold);
	}

	if (IS_ENABLED(CONFIG_MHP_MEMMAP_ON_MEMORY) && vm->in_sbm) {
		unsigned long vmemmap_size = virtio_mem_memory_block_vmemmap_size();
		uint64_t nr_mbs = PHYS_PFN(vm->region_size) >> PFN_SECTION_SHIFT;

		if (vmemmap_size != vm->sbm.sb_size) {
			dev_info(&vm->vdev->dev, "memmap_on_memory is not enabled because sb_size=%llx bytes differs from vmemmap_size=%lx bytes\n",
				vm->sbm.sb_size, vmemmap_size);
		} else {
			/* First sb_size block used for memmap */
			vm->sbm.sbs_per_mb -= 1;
			vm->memmap_on_memory = true;
			dev_info(&vm->vdev->dev, "memmap_on_memory is enabled\n");
			vm->max_pluggable_size -= nr_mbs * vmemmap_size;
			dev_info(&vm->vdev->dev, "max_pluggable_size is limited to %llx out of %llx\n",
				 vm->max_pluggable_size, vm->region_size);
		}
	}

	/*
	 * virtio_mem_sbm_plug_sb() & virtio_mem_bbm_plug_bb() call
	 * virtio_mem_send_plug_request() with count * sb_size and
	 * bb_size respectively. Check whether vm->device_block_size
	 * fits evenly.
	 */
	if (vm->in_sbm && vm->sbm.sb_size % vm->device_block_size) {
		dev_err(&vm->vdev->dev, "Device block size %llx doesn't fit in %llx\n",
			vm->device_block_size, vm->sbm.sb_size);
		return -EINVAL;
	} else if (!vm->in_sbm && vm->bbm.bb_size % vm->device_block_size) {
		dev_err(&vm->vdev->dev, "Device block size %llx doesn't fit in %llx\n",
			vm->device_block_size, vm->bbm.bb_size);
		return -EINVAL;
	}

	dev_info(&vm->vdev->dev, "memory block size: 0x%lx",
		 memory_block_size_bytes());
	if (vm->in_sbm)
		dev_info(&vm->vdev->dev, "subblock size: 0x%llx",
			 (unsigned long long)vm->sbm.sb_size);
	else
		dev_info(&vm->vdev->dev, "big block size: 0x%llx",
			 (unsigned long long)vm->bbm.bb_size);

	/* create the parent resource for all memory */
	rc = virtio_mem_create_resource(vm);
	if (rc)
		return rc;

	/* use a single dynamic memory group to cover the whole memory device */
	if (vm->in_sbm)
		unit_pages = PHYS_PFN(memory_block_size_bytes());
	else
		unit_pages = PHYS_PFN(vm->bbm.bb_size);
	rc = memory_group_register_dynamic(vm->nid, unit_pages);
	if (rc < 0)
		goto out_del_resource;
	vm->mgid = rc;

	/*
	 * If we still have memory plugged, we have to unplug all memory first.
	 * Registering our parent resource makes sure that this memory isn't
	 * actually in use (e.g., trying to reload the driver).
	 */
	if (vm->plugged_size) {
		vm->unplug_all_required = true;
		dev_info(&vm->vdev->dev, "unplugging all memory is required\n");
	}

	/* register callbacks */
	vm->memory_notifier.notifier_call = virtio_mem_memory_notifier_cb;
	rc = register_memory_notifier(&vm->memory_notifier);
	if (rc)
		goto out_unreg_group;
	rc = register_virtio_mem_device(vm);
	if (rc)
		goto out_unreg_mem;

	return 0;
out_unreg_mem:
	unregister_memory_notifier(&vm->memory_notifier);
out_unreg_group:
	memory_group_unregister(vm->mgid);
out_del_resource:
	virtio_mem_delete_resource(vm);
	return rc;
}

#ifdef CONFIG_PROC_VMCORE
static int virtio_mem_send_state_request(struct virtio_mem *vm, uint64_t addr,
					 uint64_t size)
{
	const uint64_t nb_vm_blocks = size / vm->device_block_size;
	const struct virtio_mem_req req = {
		.type = cpu_to_virtio16(vm->vdev, VIRTIO_MEM_REQ_STATE),
		.u.state.addr = cpu_to_virtio64(vm->vdev, addr),
		.u.state.nb_blocks = cpu_to_virtio16(vm->vdev, nb_vm_blocks),
	};
	int rc = -ENOMEM;

	dev_dbg(&vm->vdev->dev, "requesting state: 0x%llx - 0x%llx\n", addr,
		addr + size - 1);

	switch (virtio_mem_send_request(vm, &req)) {
	case VIRTIO_MEM_RESP_ACK:
		return virtio16_to_cpu(vm->vdev, vm->resp.u.state.state);
	case VIRTIO_MEM_RESP_ERROR:
		rc = -EINVAL;
		break;
	default:
		break;
	}

	dev_dbg(&vm->vdev->dev, "requesting state failed: %d\n", rc);
	return rc;
}

static bool virtio_mem_vmcore_pfn_is_ram(struct vmcore_cb *cb,
					 unsigned long pfn)
{
	struct virtio_mem *vm = container_of(cb, struct virtio_mem,
					     vmcore_cb);
	uint64_t addr = PFN_PHYS(pfn);
	bool is_ram;
	int rc;

	if (!virtio_mem_contains_range(vm, addr, PAGE_SIZE))
		return true;
	if (!vm->plugged_size)
		return false;

	/*
	 * We have to serialize device requests and access to the information
	 * about the block queried last.
	 */
	mutex_lock(&vm->hotplug_mutex);

	addr = ALIGN_DOWN(addr, vm->device_block_size);
	if (addr != vm->last_block_addr) {
		rc = virtio_mem_send_state_request(vm, addr,
						   vm->device_block_size);
		/* On any kind of error, we're going to signal !ram. */
		if (rc == VIRTIO_MEM_STATE_PLUGGED)
			vm->last_block_plugged = true;
		else
			vm->last_block_plugged = false;
		vm->last_block_addr = addr;
	}

	is_ram = vm->last_block_plugged;
	mutex_unlock(&vm->hotplug_mutex);
	return is_ram;
}
#endif /* CONFIG_PROC_VMCORE */

static int virtio_mem_init_kdump(struct virtio_mem *vm)
{
#ifdef CONFIG_PROC_VMCORE
	dev_info(&vm->vdev->dev, "memory hot(un)plug disabled in kdump kernel\n");
	vm->vmcore_cb.pfn_is_ram = virtio_mem_vmcore_pfn_is_ram;
	register_vmcore_cb(&vm->vmcore_cb);
	return 0;
#else /* CONFIG_PROC_VMCORE */
	dev_warn(&vm->vdev->dev, "disabled in kdump kernel without vmcore\n");
	return -EBUSY;
#endif /* CONFIG_PROC_VMCORE */
}

static int virtio_mem_encryption_setup(struct virtio_mem *vm)
{
	char *propname;
	struct device_node *np = vm->vdev->dev.of_node;
	u32 flags;
	u64 size, ipa_base;
	const struct range pluggable_range = mhp_get_pluggable_range(true);
	struct range range;
	int ret;

	propname = "qcom,memory-encryption";
	vm->use_memory_encryption = of_property_read_bool(np, propname);

	propname = "qcom,max-size";
	ret = of_property_read_u64(np, propname, &size);
	if (ret) {
		dev_err(&vm->vdev->dev, "Missing %s\n", propname);
		return -EINVAL;
	}
	if (!IS_ALIGNED(size, memory_block_size_bytes())) {
		dev_err(&vm->vdev->dev, "%s must be aligned to %lx\n",
			propname, memory_block_size_bytes());
		return -EINVAL;
	}

	/* qcom,ipa-range includes range.start & range.end */
	propname = "qcom,ipa-range";
	ret = of_property_read_u64_index(np, propname, 0, &range.start);
	ret |= of_property_read_u64_index(np, propname, 1, &range.end);
	if (ret) {
		dev_err(&vm->vdev->dev, "Missing %s\n", propname);
		return -EINVAL;
	}

	range.start = max(range.start, pluggable_range.start);
	range.end = min(range.end, pluggable_range.end);

	/*
	 * Using the DEFAULT flag will request the same encryption level
	 * as the base kernel memory.
	 */
	if (vm->use_memory_encryption)
		flags = GH_RM_IPA_RESERVE_DEFAULT;
	else
		flags = GH_RM_IPA_RESERVE_NORMAL;

	ret = gh_rm_ipa_reserve(size, memory_block_size_bytes(),
				range, flags, 0,
				&ipa_base);
	if (ret) {
		if (ret == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		dev_err(&vm->vdev->dev, "Hypervisor ipa reserve not supported\n");
		return ret;
	}

	vm->addr = ipa_base;
	vm->region_size = size;
	vm->max_pluggable_size = size;
	return 0;
}

static int virtio_mem_init(struct virtio_mem *vm)
{
	uint16_t node_id;
	int ret;
	u32 device_block_size;

	/* Fetch all properties that can't change. */
	ret = of_property_read_u32(vm->vdev->dev.of_node, "qcom,block-size",
				   &device_block_size);
	if (ret) {
		dev_err(&vm->vdev->dev, "Failed to parse qcom,block-size property\n");
		return -EINVAL;
	}
	vm->device_block_size = device_block_size;

	node_id = NUMA_NO_NODE;
	vm->nid = virtio_mem_translate_node_id(vm, node_id);

	/* Also determines the ipa_address and size */
	ret = virtio_mem_encryption_setup(vm);
	if (ret)
		return ret;

	/* Determine the nid for the device based on the lowest address. */
	if (vm->nid == NUMA_NO_NODE)
		vm->nid = memory_add_physaddr_to_nid(vm->addr);

	dev_info(&vm->vdev->dev, "start address: 0x%llx", vm->addr);
	dev_info(&vm->vdev->dev, "region size: 0x%llx", vm->region_size);
	dev_info(&vm->vdev->dev, "device block size: 0x%llx",
		 (unsigned long long)vm->device_block_size);
	if (vm->nid != NUMA_NO_NODE && IS_ENABLED(CONFIG_NUMA))
		dev_info(&vm->vdev->dev, "nid: %d", vm->nid);

	/*
	 * We don't want to (un)plug or reuse any memory when in kdump. The
	 * memory is still accessible (but not exposed to Linux).
	 */
	if (vm->in_kdump)
		return virtio_mem_init_kdump(vm);
	return virtio_mem_init_hotplug(vm);
}

static int virtio_mem_create_resource(struct virtio_mem *vm)
{
	/*
	 * When force-unloading the driver and removing the device, we
	 * could have a garbage pointer. Duplicate the string.
	 */
	const char *name = kstrdup(dev_name(&vm->vdev->dev), GFP_KERNEL);

	if (!name)
		return -ENOMEM;

	/* Disallow mapping device memory via /dev/mem completely. */
	vm->parent_resource = __request_mem_region(vm->addr, vm->region_size,
						   name, IORESOURCE_SYSTEM_RAM |
						   IORESOURCE_EXCLUSIVE);
	if (!vm->parent_resource) {
		kfree(name);
		dev_warn(&vm->vdev->dev, "could not reserve device region\n");
		dev_info(&vm->vdev->dev,
			 "reloading the driver is not supported\n");
		return -EBUSY;
	}

	/* The memory is not actually busy - make add_memory() work. */
	vm->parent_resource->flags &= ~IORESOURCE_BUSY;
	return 0;
}

static void virtio_mem_delete_resource(struct virtio_mem *vm)
{
	const char *name;

	if (!vm->parent_resource)
		return;

	name = vm->parent_resource->name;
	release_resource(vm->parent_resource);
	kfree(vm->parent_resource);
	kfree(name);
	vm->parent_resource = NULL;
}

static int virtio_mem_range_has_system_ram(struct resource *res, void *arg)
{
	return 1;
}

static bool virtio_mem_has_memory_added(struct virtio_mem *vm)
{
	const unsigned long flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;

	return walk_iomem_res_desc(IORES_DESC_NONE, flags, vm->addr,
				   vm->addr + vm->region_size, NULL,
				   virtio_mem_range_has_system_ram) == 1;
}

static int virtio_mem_probe(struct platform_device *vdev)
{
	struct virtio_mem *vm;
	int rc;

	BUILD_BUG_ON(sizeof(struct virtio_mem_req) != 24);
	BUILD_BUG_ON(sizeof(struct virtio_mem_resp) != 10);

	vm = kzalloc(sizeof(*vm), GFP_KERNEL);
	if (!vm)
		return -ENOMEM;
	platform_set_drvdata(vdev, vm);

	init_waitqueue_head(&vm->host_resp);
	vm->vdev = vdev;
	INIT_WORK(&vm->wq, virtio_mem_run_wq);
	mutex_init(&vm->hotplug_mutex);
	INIT_LIST_HEAD(&vm->next);
	spin_lock_init(&vm->removal_lock);
	hrtimer_init(&vm->retry_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	vm->retry_timer.function = virtio_mem_timer_expired;
	vm->retry_timer_ms = VIRTIO_MEM_RETRY_TIMER_MIN_MS;
	vm->in_kdump = is_kdump_kernel();

	/* initialize the device by querying the config */
	rc = virtio_mem_init(vm);
	if (rc)
		goto out_free_vm;

	virtio_mem_dev = vm;

	rc = qti_virtio_mem_init(vdev);
	if (rc)
		goto out_deinit_hotplug;

	/* trigger a config update to start processing the requested_size */
	if (!vm->in_kdump) {
		atomic_set(&vm->config_changed, 1);
		queue_work(system_freezable_wq, &vm->wq);
	}

	return 0;

out_deinit_hotplug:
	virtio_mem_deinit_hotplug(vm);
out_free_vm:
	kfree(vm);
	platform_set_drvdata(vdev, NULL);

	return rc;
}

static void virtio_mem_deinit_hotplug(struct virtio_mem *vm)
{
	unsigned long mb_id;
	int rc;

	/*
	 * Make sure the workqueue won't be triggered anymore and no memory
	 * blocks can be onlined/offlined until we're finished here.
	 */
	mutex_lock(&vm->hotplug_mutex);
	spin_lock_irq(&vm->removal_lock);
	vm->removing = true;
	spin_unlock_irq(&vm->removal_lock);
	mutex_unlock(&vm->hotplug_mutex);

	/* wait until the workqueue stopped */
	cancel_work_sync(&vm->wq);
	hrtimer_cancel(&vm->retry_timer);

	if (vm->in_sbm) {
		/*
		 * After we unregistered our callbacks, user space can online
		 * partially plugged offline blocks. Make sure to remove them.
		 */
		virtio_mem_sbm_for_each_mb(vm, mb_id,
					   VIRTIO_MEM_SBM_MB_OFFLINE_PARTIAL) {
			rc = virtio_mem_sbm_remove_mb(vm, mb_id);
			BUG_ON(rc);
			virtio_mem_sbm_set_mb_state(vm, mb_id,
						    VIRTIO_MEM_SBM_MB_UNUSED);
		}
		/*
		 * After we unregistered our callbacks, user space can no longer
		 * offline partially plugged online memory blocks. No need to
		 * worry about them.
		 */
	}

	/* unregister callbacks */
	unregister_virtio_mem_device(vm);
	unregister_memory_notifier(&vm->memory_notifier);

	/*
	 * There is no way we could reliably remove all memory we have added to
	 * the system. And there is no way to stop the driver/device from going
	 * away. Warn at least.
	 */
	if (virtio_mem_has_memory_added(vm)) {
		dev_warn(&vm->vdev->dev,
			 "device still has system memory added\n");
	} else {
		virtio_mem_delete_resource(vm);
		kfree_const(vm->resource_name);
		memory_group_unregister(vm->mgid);
	}

	/* remove all tracking data - no locking needed */
	if (vm->in_sbm) {
		vfree(vm->sbm.mb_states);
		vfree(vm->sbm.sb_states);
	} else {
		vfree(vm->bbm.bb_states);
	}
}

static void virtio_mem_deinit_kdump(struct virtio_mem *vm)
{
#ifdef CONFIG_PROC_VMCORE
	unregister_vmcore_cb(&vm->vmcore_cb);
#endif /* CONFIG_PROC_VMCORE */
}

static int virtio_mem_remove(struct platform_device *vdev)
{
	struct virtio_mem *vm = platform_get_drvdata(vdev);

	qti_virtio_mem_exit(vdev);

	if (vm->in_kdump)
		virtio_mem_deinit_kdump(vm);
	else
		virtio_mem_deinit_hotplug(vm);

	kfree(vm);
	platform_set_drvdata(vdev, NULL);

	return 0;
}

void virtio_mem_config_changed(struct platform_device *vdev)
{
	struct virtio_mem *vm = platform_get_drvdata(vdev);

	if (unlikely(vm->in_kdump))
		return;

	atomic_set(&vm->config_changed, 1);
	virtio_mem_retry(vm);
}

static const struct of_device_id virtio_mem_id_table[] = {
	{ .compatible = "qcom,virtio-mem" },
	{ },
};

static struct platform_driver virtio_mem_driver = {
	.driver	= {
		.name			= "virtio_mem",
		.of_match_table		= virtio_mem_id_table,
	},
	.probe	= virtio_mem_probe,
	.remove	= virtio_mem_remove,
};

module_platform_driver(virtio_mem_driver);
MODULE_DEVICE_TABLE(of, virtio_mem_id_table);
MODULE_AUTHOR("David Hildenbrand <david@redhat.com>");
MODULE_DESCRIPTION("Virtio-mem driver");
MODULE_LICENSE("GPL");
