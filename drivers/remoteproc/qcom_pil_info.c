// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2020 Linaro Ltd.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include "qcom_pil_info.h"

/*
 * The PIL relocation information region is used to communicate memory regions
 * occupied by co-processor firmware for post mortem crash analysis.
 *
 * It consists of an array of entries with an 8 byte textual identifier of the
 * region followed by a 64 bit base address and 32 bit size, both little
 * endian.
 */
#define PIL_RELOC_NAME_LEN	8
#define PIL_RELOC_ENTRY_SIZE	(PIL_RELOC_NAME_LEN + sizeof(__le64) + sizeof(__le32))

struct pil_reloc {
	void __iomem *base;
	size_t num_entries;
};

static struct pil_reloc _reloc __read_mostly;
static DEFINE_MUTEX(pil_reloc_lock);
static void __iomem *pil_timeout_base;

static void __iomem *qcom_map_pil_imem_resource(const char *compatible,
						struct resource *imem)
{
	struct device_node *np;
	void __iomem *base;
	int ret;

	np = of_find_compatible_node(NULL, NULL, compatible);
	if (!np) {
		pr_err("failed to find %s\n", compatible);
		return ERR_PTR(-ENOENT);
	}

	ret = of_address_to_resource(np, 0, imem);
	of_node_put(np);
	if (ret < 0)
		return ERR_PTR(ret);

	base = ioremap(imem->start, resource_size(imem));
	if (!base) {
		pr_err("failed to map %s region\n", compatible);
		return ERR_PTR(-ENOMEM);
	}

	return base;
}

/**
 * qcom_pil_timeouts_disabled() - Check if pil timeouts are disabled in imem
 *
 * Return: true if the value 0x53444247 is set in the disable timeout pil
 * imem region, false otherwise.
 */
bool qcom_pil_timeouts_disabled(void)
{
	const char *compatible = "qcom,msm-imem-pil-disable-timeout";
	bool timeouts_disabled;
	struct resource imem;
	void __iomem *base;

	if (!pil_timeout_base) {
		base = qcom_map_pil_imem_resource(compatible, &imem);
		if (IS_ERR(base))
			return false;

		pil_timeout_base = base;
	}

	if (__raw_readl(pil_timeout_base) == 0x53444247) {
		pr_info("pil-imem set to disable pil timeouts\n");
		timeouts_disabled = true;
	} else
		timeouts_disabled = false;

	return timeouts_disabled;
}
EXPORT_SYMBOL_GPL(qcom_pil_timeouts_disabled);

static int qcom_pil_info_init(const char *compatible)
{
	struct resource imem;
	void __iomem *base;

	/* Already initialized? */
	if (_reloc.base)
		return 0;

	base = qcom_map_pil_imem_resource(compatible, &imem);
	if (IS_ERR(base))
		return PTR_ERR(base);

	memset_io(base, 0, resource_size(&imem));

	_reloc.base = base;
	_reloc.num_entries = (u32)resource_size(&imem) / PIL_RELOC_ENTRY_SIZE;

	return 0;
}

/**
 * qcom_pil_info_store() - store PIL information of image in IMEM
 * @image:	name of the image
 * @base:	base address of the loaded image
 * @size:	size of the loaded image
 *
 * Return: 0 on success, negative errno on failure
 */
int qcom_pil_info_store(const char *image, phys_addr_t base, size_t size)
{
	char buf[PIL_RELOC_NAME_LEN];
	void __iomem *entry;
	size_t entry_size;
	int ret;
	int i;

	mutex_lock(&pil_reloc_lock);
	ret = qcom_pil_info_init("qcom,pil-reloc-info");
	if (ret < 0) {
		mutex_unlock(&pil_reloc_lock);
		return ret;
	}

	for (i = 0; i < _reloc.num_entries; i++) {
		entry = _reloc.base + i * PIL_RELOC_ENTRY_SIZE;

		memcpy_fromio(buf, entry, PIL_RELOC_NAME_LEN);

		/*
		 * An empty record means we didn't find it, given that the
		 * records are packed.
		 */
		if (!buf[0])
			goto found_unused;

		if (!strncmp(buf, image, PIL_RELOC_NAME_LEN))
			goto found_existing;
	}

	pr_warn("insufficient PIL info slots\n");
	mutex_unlock(&pil_reloc_lock);
	return -ENOMEM;

found_unused:
	entry_size = min(strlen(image), PIL_RELOC_ENTRY_SIZE - 1);
	memcpy_toio(entry, image, entry_size);
found_existing:
	/* Use two writel() as base is only aligned to 4 bytes on odd entries */
	writel(base, entry + PIL_RELOC_NAME_LEN);
	writel((u64)base >> 32, entry + PIL_RELOC_NAME_LEN + 4);
	writel(size, entry + PIL_RELOC_NAME_LEN + sizeof(__le64));
	mutex_unlock(&pil_reloc_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(qcom_pil_info_store);

static void __exit pil_reloc_exit(void)
{
	mutex_lock(&pil_reloc_lock);
	iounmap(_reloc.base);
	_reloc.base = NULL;
	mutex_unlock(&pil_reloc_lock);
	iounmap(pil_timeout_base);
	pil_timeout_base = NULL;
}
module_exit(pil_reloc_exit);

MODULE_DESCRIPTION("Qualcomm PIL relocation info");
MODULE_LICENSE("GPL v2");
