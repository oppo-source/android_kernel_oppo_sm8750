/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __QCOM_Q6V5_H__
#define __QCOM_Q6V5_H__

#include <linux/kernel.h>
#include <linux/completion.h>
#include <linux/soc/qcom/qcom_aoss.h>

struct icc_path;
struct rproc;
struct qcom_smem_state;
struct qcom_sysmon;

struct qcom_q6v5 {
	struct device *dev;
	struct rproc *rproc;

	struct qcom_smem_state *state;
	struct qmp *qmp;

	struct icc_path *path;

	unsigned stop_bit;

	int wdog_irq;
	int fatal_irq;
	int ready_irq;
	int handover_irq;
	int stop_irq;
	int active_state_ack_irq;

	struct rproc_subdev *ssr_subdev;

	struct work_struct crash_handler;

	bool handover_issued;

	struct completion start_done;
	struct completion stop_done;
	struct completion running_ack;

	int crash_reason;
	int crash_stack;
	unsigned int smem_host_id;

	bool running;

	const char *load_state;
	void (*handover)(struct qcom_q6v5 *q6v5);
	unsigned long long seq;
	unsigned long long crash_seq;
};

int qcom_q6v5_init(struct qcom_q6v5 *q6v5, struct platform_device *pdev,
		   struct rproc *rproc, int crash_reason, int crash_stack,
		   unsigned int smem_host_id, const char *load_state,
		   void (*handover)(struct qcom_q6v5 *q6v5));
void qcom_q6v5_deinit(struct qcom_q6v5 *q6v5);

void qcom_q6v5_register_ssr_subdev(struct qcom_q6v5 *q6v5, struct rproc_subdev *ssr_subdev);
int qcom_q6v5_prepare(struct qcom_q6v5 *q6v5);
int qcom_q6v5_unprepare(struct qcom_q6v5 *q6v5);
int qcom_q6v5_request_stop(struct qcom_q6v5 *q6v5, struct qcom_sysmon *sysmon);
int qcom_q6v5_wait_for_start(struct qcom_q6v5 *q6v5, int timeout);
unsigned long qcom_q6v5_panic(struct qcom_q6v5 *q6v5);

#endif
