// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "VendorHooks: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/sched/debug.h>
#include <linux/io.h>
#include <linux/syscore_ops.h>

#include <soc/qcom/watchdog.h>

#include <trace/hooks/debug.h>
#include <trace/hooks/printk.h>
#include <trace/hooks/timer.h>

static DEFINE_PER_CPU(struct pt_regs, regs_before_stop);
static DEFINE_RAW_SPINLOCK(stop_lock);

static void printk_hotplug(void *unused, int *flag)
{
	*flag = 1;
}

static void trace_ipi_stop(void *unused, struct pt_regs *regs)
{
	unsigned int cpu = smp_processor_id();
	unsigned long flags;

	per_cpu(regs_before_stop, cpu) = *regs;
	raw_spin_lock_irqsave(&stop_lock, flags);
	pr_crit("CPU%u: stopping\n", cpu);
	show_regs(regs);
	raw_spin_unlock_irqrestore(&stop_lock, flags);
}

static void timer_recalc_index(void *unused,
			unsigned int lvl, unsigned long *expires)
{
	*expires -= 1;
}

#if IS_ENABLED(CONFIG_DEBUG_SPINLOCK) && \
	(IS_ENABLED(CONFIG_DEBUG_SPINLOCK_BITE_ON_BUG) || IS_ENABLED(CONFIG_DEBUG_SPINLOCK_PANIC_ON_BUG))
static int entry_spin_bug(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	raw_spinlock_t *lock = (raw_spinlock_t *)regs->regs[0];
	const char *msg = (const char *)regs->regs[1];
	struct task_struct *owner = READ_ONCE(lock->owner);

	if (!debug_locks_off())
		return 0;

	/* Dup of spin_bug in kernel/locking/spinlock_debug.c */
	if (owner == SPINLOCK_OWNER_INIT)
		owner = NULL;
	printk(KERN_EMERG "BUG: spinlock %s on CPU#%d, %s/%d\n",
		msg, raw_smp_processor_id(),
		current->comm, task_pid_nr(current));
	printk(KERN_EMERG " lock: %pS, .magic: %08x, .owner: %s/%d, "
			".owner_cpu: %d\n",
		lock, READ_ONCE(lock->magic),
		owner ? owner->comm : "<none>",
		owner ? task_pid_nr(owner) : -1,
		READ_ONCE(lock->owner_cpu));

#if IS_ENABLED(CONFIG_DEBUG_SPINLOCK_BITE_ON_BUG)
	qcom_wdt_trigger_bite();
#elif IS_ENABLED(CONFIG_DEBUG_SPINLOCK_PANIC_ON_BUG)
	BUG();
#else
# error "Neither CONFIG_DEBUG_SPINLOCK_BITE_ON_BUG nor CONFIG_DEBUG_SPINLOCK_PANIC_ON_BUG is enabled yet trying to enable spin_bug hook"
#endif
	return 0;
}

struct kretprobe spin_bug_probe = {
	.entry_handler = entry_spin_bug,
	.maxactive = 1,
	.kp.symbol_name = "spin_bug",
};

static void register_spinlock_bug_hook(void)
{
	int ret;

	ret = register_kretprobe(&spin_bug_probe);
	if (ret)
		pr_err("Failed to register spin_bug_probe: %x\n", ret);
}
#else
static inline void register_spinlock_bug_hook(void) { }
#endif

#ifdef CONFIG_RANDOMIZE_BASE
#define KASLR_OFFSET_MASK	0x00000000FFFFFFFF
static void __iomem *map_prop_mem(const char *propname)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, propname);
	void __iomem *addr;

	if (!np) {
		pr_err("Unable to find DT property: %s\n", propname);
		return NULL;
	}

	addr = of_iomap(np, 0);
	if (!addr)
		pr_err("Unable to map memory for DT property: %s\n", propname);
	return addr;
}

static void store_kaslr_offset(void)
{
	void __iomem *mem = map_prop_mem("qcom,msm-imem-kaslr_offset");

	if (!mem)
		return;

	__raw_writel(0xdead4ead, mem);
	__raw_writel((kimage_vaddr - KIMAGE_VADDR) & KASLR_OFFSET_MASK,
		     mem + 4);
	__raw_writel(((kimage_vaddr - KIMAGE_VADDR) >> 32) & KASLR_OFFSET_MASK,
		     mem + 8);

	iounmap(mem);
}

#if defined(CONFIG_HIBERNATION)
static struct syscore_ops kaslr_offset_restore_syscore_ops = {
	.resume = store_kaslr_offset,
};
#endif /* CONFIG_HIBERNATION */

#else
static void store_kaslr_offset(void) {}
#endif /* CONFIG_RANDOMIZE_BASE */

static int __init qcom_vendor_hook_driver_init(void)
{
	int ret;

	store_kaslr_offset();
#ifdef CONFIG_HIBERNATION
	register_syscore_ops(&kaslr_offset_restore_syscore_ops);
#endif

	ret = register_trace_android_vh_ipi_stop(trace_ipi_stop, NULL);
	if (ret) {
		pr_err("Failed to register android_vh_ipi_stop hook\n");
		return ret;
	}

	ret = register_trace_android_vh_printk_hotplug(printk_hotplug, NULL);
	if (ret) {
		pr_err("Failed to android_vh_printk_hotplug hook\n");
		unregister_trace_android_vh_ipi_stop(trace_ipi_stop, NULL);
		return ret;
	}

	ret = register_trace_android_vh_timer_calc_index(timer_recalc_index, NULL);
	if (ret) {
		pr_err("Failed to android_vh_timer_calc_index hook\n");
		unregister_trace_android_vh_ipi_stop(trace_ipi_stop, NULL);
		unregister_trace_android_vh_printk_hotplug(printk_hotplug, NULL);
		return ret;
	}

	register_spinlock_bug_hook();

	return ret;
}

static void __exit qcom_vendor_hook_driver_exit(void)
{
	/* Reset all initialized global variables and unregister callbacks. */
	unregister_trace_android_vh_ipi_stop(trace_ipi_stop, NULL);
	unregister_trace_android_vh_printk_hotplug(printk_hotplug, NULL);
	unregister_trace_android_vh_timer_calc_index(timer_recalc_index, NULL);
}

#if IS_MODULE(CONFIG_QCOM_CPU_VENDOR_HOOKS)
module_init(qcom_vendor_hook_driver_init);
#else
pure_initcall(qcom_vendor_hook_driver_init);
#endif
module_exit(qcom_vendor_hook_driver_exit);
MODULE_DESCRIPTION("QCOM CPU Vendor Hooks Driver");
MODULE_LICENSE("GPL");
