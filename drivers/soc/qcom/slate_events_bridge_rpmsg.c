// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include "slate_events_bridge_rpmsg.h"

static struct seb_rpmsg_dev *pdev;
struct seb_channel_ops seb_ops;

void seb_channel_init(void (*fn1)(bool), void (*fn2)(void *data, int len))
{
	seb_ops.glink_channel_state = fn1;
	seb_ops.rx_msg = fn2;
}
EXPORT_SYMBOL_GPL(seb_channel_init);

int seb_rpmsg_tx_msg(void  *msg, size_t len)
{
	int ret = 0;

	if (pdev == NULL || !pdev->chnl_state) {
		pr_err("pmsg_device is null, channel is closed\n");
		return -ENETRESET;
	}
	pdev->message = msg;
	pdev->message_length = len;
	if (pdev->message != NULL) {
		ret = rpmsg_send(pdev->channel,
			pdev->message, pdev->message_length);
		if (ret)
			pr_err("rpmsg_send failed: %d\n", ret);

	}
	return ret;
}
EXPORT_SYMBOL_GPL(seb_rpmsg_tx_msg);

static int seb_rpmsg_probe(struct rpmsg_device  *rpdev)
{
	int ret = 0;
	void *msg = NULL;

	pdev = devm_kzalloc(&rpdev->dev, sizeof(*pdev), GFP_KERNEL);
	if (!pdev)
		return -ENOMEM;

	pdev->channel = rpdev->ept;
	pdev->dev = &rpdev->dev;
	if (pdev->channel == NULL)
		return -ENOMEM;

	pdev->chnl_state = true;
	dev_set_drvdata(&rpdev->dev, pdev);

	/* send a callback to slate event bridge driver*/
	seb_ops.glink_channel_state(true);
	if (pdev->message == NULL)
		ret = seb_rpmsg_tx_msg(msg, 0);
	return 0;
}

static void seb_rpmsg_remove(struct rpmsg_device *rpdev)
{
	pdev->chnl_state = false;
	pdev->message = NULL;
	dev_dbg(&rpdev->dev, "rpmsg client driver is removed\n");
	seb_ops.glink_channel_state(false);
	dev_set_drvdata(&rpdev->dev, NULL);
}

static int seb_rpmsg_cb(struct rpmsg_device *rpdev,
				void *data, int len, void *priv, u32 src)
{
	struct seb_rpmsg_dev *dev =
			dev_get_drvdata(&rpdev->dev);

	if (!dev)
		return -ENODEV;
	seb_ops.rx_msg(data, len);
	return 0;
}

static const struct rpmsg_device_id rpmsg_driver_seb_id_table[] = {
	{ "slate-event" },
	{},
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_driver_seb_id_table);

static const struct of_device_id rpmsg_driver_seb_of_match[] = {
	{ .compatible = "qcom,slate-events-bridge-rpmsg" },
	{},
};

static struct rpmsg_driver rpmsg_seb_client = {
	.id_table = rpmsg_driver_seb_id_table,
	.probe = seb_rpmsg_probe,
	.callback = seb_rpmsg_cb,
	.remove = seb_rpmsg_remove,
	.drv = {
		.name = "qcom,seb_rpmsg",
		.of_match_table = rpmsg_driver_seb_of_match,
	},
};
module_rpmsg_driver(rpmsg_seb_client);

MODULE_DESCRIPTION("Interface Driver for Slate events bridge RPMSG");
MODULE_LICENSE("GPL");
