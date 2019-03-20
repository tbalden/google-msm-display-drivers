/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt)	"%s: " fmt, __func__

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <uapi/linux/sched/types.h>
#include <video/mipi_display.h>

#include "dsi_display.h"
#include "dsi_panel.h"
#include "sde_trace.h"
#include "sde_connector.h"

#define DSI_PANEL_GAMMA_NAME "google,dsi_s6e3hc2_gamma"

#define TE_TIMEOUT_MS	50

#define for_each_display_mode(i, mode, panel) \
	for (i = 0, mode = dsi_panel_to_display(panel)->modes; \
		i < panel->num_timing_nodes; i++, mode++)


#define DSI_WRITE_CMD_BUF(dsi, cmd) \
	(IS_ERR_VALUE(mipi_dsi_dcs_write_buffer(dsi, cmd, ARRAY_SIZE(cmd))))

struct panel_switch_funcs {
	struct panel_switch_data *(*create)(struct dsi_panel *panel);
	void (*destroy)(struct panel_switch_data *pdata);
	void (*put_mode)(struct dsi_display_mode *mode);
	void (*perform_switch)(struct panel_switch_data *pdata,
			       const struct dsi_display_mode *mode);
	int (*post_enable)(struct panel_switch_data *pdata);
};

struct panel_switch_data {
	struct dsi_panel *panel;
	struct dentry *debug_root;

	struct kthread_work switch_work;
	struct kthread_worker worker;
	struct task_struct *thread;

	const struct dsi_display_mode *display_mode;
	wait_queue_head_t switch_wq;
	bool switch_pending;
	int switch_te_listen_count;

	atomic_t te_counter;
	struct dsi_display_te_listener te_listener;
	struct completion te_completion;

	const struct panel_switch_funcs *funcs;
};

static inline
struct dsi_display *dsi_panel_to_display(const struct dsi_panel *panel)
{
	return dev_get_drvdata(panel->parent);
}

static void panel_handle_te(struct dsi_display_te_listener *tl)
{
	struct panel_switch_data *pdata;

	if (unlikely(!tl))
		return;

	pdata = container_of(tl, struct panel_switch_data, te_listener);

	complete(&pdata->te_completion);

	if (likely(pdata->thread)) {
		/* 1-bit counter that shows up in panel thread timeline */
		sde_atrace('C', pdata->thread, "TE_VSYNC",
			   atomic_inc_return(&pdata->te_counter) & 1);
	}
}

static void panel_switch_cmd_set_transfer(struct panel_switch_data *pdata,
					  const struct dsi_display_mode *mode)
{
	struct dsi_panel *panel = pdata->panel;
	struct dsi_panel_cmd_set *cmd;
	int rc;

	cmd = &mode->priv_info->cmd_sets[DSI_CMD_SET_TIMING_SWITCH];

	rc = dsi_panel_cmd_set_transfer(panel, cmd);
	if (rc)
		pr_warn("failed to send TIMING switch cmd, rc=%d\n", rc);
}

static void panel_switch_to_mode(struct panel_switch_data *pdata,
				 const struct dsi_display_mode *mode)
{
	SDE_ATRACE_BEGIN(__func__);
	if (pdata->funcs && pdata->funcs->perform_switch)
		pdata->funcs->perform_switch(pdata, mode);

	if (pdata->switch_pending) {
		pdata->switch_pending = false;
		wake_up_all(&pdata->switch_wq);
	}

	SDE_ATRACE_END(__func__);
}

static void panel_switch_worker(struct kthread_work *work)
{
	struct panel_switch_data *pdata;
	struct dsi_panel *panel;
	struct dsi_display *display;
	const struct dsi_display_mode *mode;
	const unsigned long timeout = msecs_to_jiffies(TE_TIMEOUT_MS);
	int rc;
	u32 te_listen_cnt;

	if (unlikely(!work))
		return;

	pdata = container_of(work, struct panel_switch_data, switch_work);
	panel = pdata->panel;
	if (unlikely(!panel))
		return;

	display = dsi_panel_to_display(panel);
	if (unlikely(!display))
		return;

	mutex_lock(&panel->panel_lock);
	mode = pdata->display_mode;
	if (unlikely(!mode)) {
		mutex_unlock(&panel->panel_lock);
		return;
	}

	SDE_ATRACE_BEGIN(__func__);

	pr_debug("switching mode to %dhz\n", mode->timing.refresh_rate);

	te_listen_cnt = pdata->switch_te_listen_count;
	if (te_listen_cnt) {
		reinit_completion(&pdata->te_completion);
		dsi_display_add_te_listener(display, &pdata->te_listener);
	}

	/* switch is shadowed by vsync so this can be done ahead of TE */
	panel_switch_to_mode(pdata, mode);
	mutex_unlock(&panel->panel_lock);

	if (te_listen_cnt) {
		rc = wait_for_completion_timeout(&pdata->te_completion,
						 timeout);
		if (!rc)
			pr_warn("Timed out waiting for TE while switching!\n");
		else
			pr_debug("TE received after %dus\n",
				jiffies_to_usecs(timeout - rc));
	}

	SDE_ATRACE_INT("FPS", mode->timing.refresh_rate);
	SDE_ATRACE_END(__func__);

	/*
	 * this is meant only for debugging purposes, keep TE enabled for a few
	 * extra frames to see how they align after switch
	 */
	if (te_listen_cnt) {
		te_listen_cnt--;
		pr_debug("waiting for %d extra te\n", te_listen_cnt);
		while (rc && te_listen_cnt) {
			rc = wait_for_completion_timeout(&pdata->te_completion,
							timeout);
			te_listen_cnt--;
		}
		dsi_display_remove_te_listener(display, &pdata->te_listener);
	}
}

static const struct dsi_display_mode *
panel_get_display_mode(const struct dsi_panel *panel, u32 refresh_rate)
{
	const struct dsi_display_mode *mode;
	int i;

	for_each_display_mode(i, mode, panel) {
		if (refresh_rate == mode->timing.refresh_rate)
			return mode;
	}

	return NULL;
}

static void panel_queue_switch(struct panel_switch_data *pdata,
			       const struct dsi_display_mode *new_mode)
{
	if (unlikely(!pdata || !pdata->panel || !new_mode))
		return;

	kthread_flush_work(&pdata->switch_work);

	mutex_lock(&pdata->panel->panel_lock);
	pdata->display_mode = new_mode;
	pdata->switch_pending = true;
	mutex_unlock(&pdata->panel->panel_lock);

	kthread_queue_work(&pdata->worker, &pdata->switch_work);
}

static int panel_switch(struct dsi_panel *panel)
{
	struct panel_switch_data *pdata = panel->private_data;

	if (!panel->cur_mode)
		return -EINVAL;

	SDE_ATRACE_BEGIN(__func__);
	panel_queue_switch(pdata, panel->cur_mode);
	SDE_ATRACE_END(__func__);

	return 0;
}

static int panel_pre_kickoff(struct dsi_panel *panel)
{
	struct panel_switch_data *pdata = panel->private_data;
	const unsigned long timeout = msecs_to_jiffies(TE_TIMEOUT_MS);

	if (!wait_event_timeout(pdata->switch_wq,
				!pdata->switch_pending, timeout))
		pr_warn("Timed out waiting for panel switch\n");

	return 0;
}

static int panel_flush_switch_queue(struct dsi_panel *panel)
{
	struct panel_switch_data *pdata = panel->private_data;

	kthread_flush_worker(&pdata->worker);

	return 0;
}

static int panel_post_enable(struct dsi_panel *panel)
{
	struct panel_switch_data *pdata = panel->private_data;
	int rc = 0;

	if (!pdata)
		return -EINVAL;

	if (pdata->funcs && pdata->funcs->post_enable)
		rc = pdata->funcs->post_enable(pdata);

	return rc;
}

static ssize_t debugfs_panel_switch_mode_write(struct file *file,
					       const char __user *user_buf,
					       size_t user_len,
					       loff_t *ppos)
{
	struct seq_file *seq = file->private_data;
	struct panel_switch_data *pdata = seq->private;
	const struct dsi_display_mode *mode;
	u32 refresh_rate;
	int rc;

	if (!pdata->panel || !dsi_panel_initialized(pdata->panel))
		return -EINVAL;

	rc = kstrtou32_from_user(user_buf, user_len, 0, &refresh_rate);
	if (rc)
		return rc;

	mode = panel_get_display_mode(pdata->panel, refresh_rate);
	if (!mode)
		return -EINVAL;

	panel_queue_switch(pdata, mode);

	return user_len;
}

static int debugfs_panel_switch_mode_read(struct seq_file *seq, void *data)
{
	struct panel_switch_data *pdata = seq->private;
	const struct dsi_display_mode *mode;
	u32 refresh_rate = 0;

	if (unlikely(!pdata->panel))
		return -ENOENT;

	mutex_lock(&pdata->panel->panel_lock);
	mode = pdata->panel->cur_mode;
	if (likely(mode))
		refresh_rate = mode->timing.refresh_rate;
	mutex_unlock(&pdata->panel->panel_lock);

	seq_printf(seq, "%d\n", refresh_rate);

	return 0;
}

static int debugfs_panel_switch_mode_open(struct inode *inode, struct file *f)
{
	return single_open(f, debugfs_panel_switch_mode_read, inode->i_private);
}

static const struct file_operations panel_switch_fops = {
	.owner = THIS_MODULE,
	.open = debugfs_panel_switch_mode_open,
	.write = debugfs_panel_switch_mode_write,
	.read = seq_read,
	.release = single_release,
};

static const struct dsi_panel_funcs panel_funcs = {
	.mode_switch = panel_switch,
	.pre_disable = panel_flush_switch_queue,
	.pre_kickoff = panel_pre_kickoff,
	.post_enable = panel_post_enable,
	.pre_lp1     = panel_flush_switch_queue,
};

static int panel_switch_data_init(struct dsi_panel *panel,
				  struct panel_switch_data *pdata)
{
	struct sched_param param = {
		.sched_priority = 16,
	};
	const struct dsi_display *display;

	display = dsi_panel_to_display(panel);
	if (unlikely(!display))
		return -ENOENT;

	kthread_init_work(&pdata->switch_work, panel_switch_worker);
	kthread_init_worker(&pdata->worker);
	pdata->thread = kthread_run(kthread_worker_fn, &pdata->worker, "panel");
	if (IS_ERR_OR_NULL(pdata->thread))
		return -EFAULT;

	pdata->panel = panel;
	pdata->te_listener.handler = panel_handle_te;
	pdata->display_mode = panel->cur_mode;

	sched_setscheduler(pdata->thread, SCHED_FIFO, &param);
	init_completion(&pdata->te_completion);
	init_waitqueue_head(&pdata->switch_wq);
	atomic_set(&pdata->te_counter, 0);

	panel->private_data = pdata;
	panel->funcs = &panel_funcs;

	pdata->debug_root = debugfs_create_dir("switch", display->root);
	debugfs_create_file("mode", 0600, pdata->debug_root, pdata,
			    &panel_switch_fops);
	debugfs_create_u32("te_listen_count", 0600, pdata->debug_root,
			    &pdata->switch_te_listen_count);
	debugfs_create_atomic_t("te_counter", 0600, pdata->debug_root,
				&pdata->te_counter);

	return 0;
}

static void panel_switch_data_deinit(struct panel_switch_data *pdata)
{
	kthread_flush_worker(&pdata->worker);
	kthread_stop(pdata->thread);
}

static void panel_switch_data_destroy(struct panel_switch_data *pdata)
{
	panel_switch_data_deinit(pdata);
	if (pdata->panel && pdata->panel->parent)
		devm_kfree(pdata->panel->parent, pdata);
}

static struct panel_switch_data *panel_switch_create(struct dsi_panel *panel)
{
	struct panel_switch_data *pdata;
	int rc;

	pdata = devm_kzalloc(panel->parent, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	rc = panel_switch_data_init(panel, pdata);
	if (rc)
		return ERR_PTR(rc);

	return pdata;
}

const struct panel_switch_funcs panel_switch_default_funcs = {
	.create = panel_switch_create,
	.destroy = panel_switch_data_destroy,
	.perform_switch = panel_switch_cmd_set_transfer,
};

struct s6e3hc2_switch_data {
	struct panel_switch_data base;

	bool gamma_ready;
	struct kthread_work gamma_work;
};

const struct s6e3hc2_gamma_info {
	u8 cmd;
	u32 len;
	u32 flash_offset;
	u8 par_offset;
} s6e3hc2_gamma_tables[] = {
	{ 0xC8, 135, 0x0000 },
	{ 0xC9, 180, 0x0087 },
	{ 0xB3,  45, 0x013B, 0x02 },
};

#define S6E3HC2_NUM_GAMMA_TABLES ARRAY_SIZE(s6e3hc2_gamma_tables)

struct s6e3hc2_panel_data {
	u8 *gamma_data[S6E3HC2_NUM_GAMMA_TABLES];
};

static void s6e3hc2_gamma_update(struct panel_switch_data *pdata,
				 const struct dsi_display_mode *mode)
{
	struct mipi_dsi_device *dsi = &pdata->panel->mipi_device;
	struct s6e3hc2_panel_data *priv_data;
	int i;

	if (unlikely(!mode || !mode->priv_info))
		return;

	priv_data = mode->priv_info->switch_data;
	if (unlikely(!priv_data))
		return;

	for (i = 0; i < S6E3HC2_NUM_GAMMA_TABLES; i++) {
		/* extra byte for the dsi command */
		const size_t len = s6e3hc2_gamma_tables[i].len + 1;
		const void *data = priv_data->gamma_data[i];

		if (WARN(!data, "Gamma table #%d not read\n", i))
			continue;

		if (IS_ERR_VALUE(mipi_dsi_dcs_write_buffer(dsi, data, len)))
			pr_warn("failed sending gamma cmd 0x%02x\n",
				s6e3hc2_gamma_tables[i].cmd);
	}
}

static int s6e3hc2_gamma_read_otp(struct panel_switch_data *pdata,
				  const struct s6e3hc2_panel_data *priv_data)
{
	struct mipi_dsi_device *dsi;
	ssize_t rc;
	int i;

	SDE_ATRACE_BEGIN(__func__);

	dsi = &pdata->panel->mipi_device;

	for (i = 0; i < S6E3HC2_NUM_GAMMA_TABLES; i++) {
		const struct s6e3hc2_gamma_info *info;
		u8 gpar_cmd[2] = { 0xB0 };
		u8 *buf = priv_data->gamma_data[i];

		info = &s6e3hc2_gamma_tables[i];

		if (info->par_offset) {
			gpar_cmd[1] = info->par_offset;
			if (DSI_WRITE_CMD_BUF(dsi, gpar_cmd))
				goto error;
		}

		/* store cmd on first byte to send payload as is */
		*buf = info->cmd;
		buf++;

		rc = mipi_dsi_dcs_read(dsi, info->cmd, buf, info->len);
		if (rc != info->len)
			pr_warn("Only got %zd / %d bytes\n", rc, info->len);
	}

	SDE_ATRACE_END(__func__);

	return 0;

error:
	SDE_ATRACE_END(__func__);

	pr_err("Failed to read gamma from OTP\n");
	return -EFAULT;
}

static int s6e3hc2_gamma_read_flash(struct panel_switch_data *pdata,
				    const struct s6e3hc2_panel_data *priv_data)
{
	struct mipi_dsi_device *dsi;
	const u8 flash_mode_en[]  = { 0xF1, 0xF1, 0xA2 };
	const u8 flash_mode_dis[] = { 0xF1, 0xA5, 0xA5 };
	const u8 pgm_dis[]        = { 0xC0, 0x00 };
	const u8 pgm_en[]         = { 0xC0, 0x02 };
	const u8 exe_inst[]	  = { 0xC0, 0x03 };
	const u8 write_en[]       = { 0xC1,
		0x00, 0x00, 0x00, 0x06,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x05 };
	const u8 quad_en[]        = { 0xC1,
		0x00, 0x00, 0x00, 0x01,
		0x40, 0x02, 0x00, 0x00,
		0x00, 0x00, 0x10 };
	ssize_t rc;
	int i, j;

	SDE_ATRACE_BEGIN(__func__);

	dsi = &pdata->panel->mipi_device;

	if (DSI_WRITE_CMD_BUF(dsi, flash_mode_en) ||
	    DSI_WRITE_CMD_BUF(dsi, pgm_en) ||
	    DSI_WRITE_CMD_BUF(dsi, write_en) ||
	    DSI_WRITE_CMD_BUF(dsi, exe_inst))
		goto error;
	usleep_range(950, 1000);

	if (DSI_WRITE_CMD_BUF(dsi, quad_en) ||
	    DSI_WRITE_CMD_BUF(dsi, exe_inst))
		goto error;
	msleep(30);

	for (i = 0; i < S6E3HC2_NUM_GAMMA_TABLES; i++) {
		const struct s6e3hc2_gamma_info *info;
		const u8 gpar_cmd[] = { 0xB0, 0x0B };
		u8 flash_rd[] = { 0xC1,
			0x00, 0x00, 0x00, 0x6B, 0x00, 0x00, 0x00, /*Read Inst*/
			0x0A, 0x00, 0x00,    /* Flash data Address : 0A0000h */
			0x00, 0x05,          /* Bit rate setting */
			0x01 };
		u32 offset;
		u8 *buf;

		info = &s6e3hc2_gamma_tables[i];
		offset = info->flash_offset;
		buf = priv_data->gamma_data[i];
		/* store cmd on first byte to send payload as is */
		*buf = info->cmd;
		buf++;

		for (j = 0; j < info->len; j++, offset++) {
			u8 tmp[2];

			flash_rd[9] = (offset >> 8) & 0xFF;
			flash_rd[10] = offset & 0xFF;

			if (DSI_WRITE_CMD_BUF(dsi, flash_rd) ||
			    DSI_WRITE_CMD_BUF(dsi, exe_inst))
				goto error;
			usleep_range(200, 250);

			if (DSI_WRITE_CMD_BUF(dsi, gpar_cmd))
				goto error;

			rc = mipi_dsi_dcs_read(dsi, 0xFB, tmp, sizeof(tmp));
			if (rc != 2)
				pr_warn("Only got %zd / 2 bytes\n", rc);

			pr_debug("read flash offset %04x: %02X %02X\n",
				 offset, tmp[0], tmp[1]);
			buf[j] = tmp[1];
		}
	}

	if (DSI_WRITE_CMD_BUF(dsi, pgm_dis) ||
	    DSI_WRITE_CMD_BUF(dsi, flash_mode_dis))
		goto error;

	SDE_ATRACE_END(__func__);

	return 0;

error:
	SDE_ATRACE_END(__func__);

	pr_err("Failed to read gamma from flash\n");
	return -EFAULT;
}

static int s6e3hc2_gamma_alloc_mode_memory(const struct dsi_display_mode *mode)
{
	struct s6e3hc2_panel_data *priv_data;
	size_t offset, total_size;
	int i;
	u8 *buf;

	if (unlikely(!mode || !mode->priv_info))
		return -EINVAL;

	if (mode->priv_info->switch_data)
		return 0;

	total_size = sizeof(*priv_data);

	for (i = 0; i < S6E3HC2_NUM_GAMMA_TABLES; i++)
		total_size += s6e3hc2_gamma_tables[i].len;
	/* add an extra byte for cmd */
	total_size += S6E3HC2_NUM_GAMMA_TABLES;

	priv_data = kmalloc(total_size, GFP_KERNEL);
	if (!priv_data)
		return -ENOMEM;

	/* use remaining data at the end of buffer */
	buf = (u8 *)(priv_data);
	offset = sizeof(*priv_data);

	for (i = 0; i < S6E3HC2_NUM_GAMMA_TABLES; i++) {
		const size_t len = s6e3hc2_gamma_tables[i].len;

		priv_data->gamma_data[i] = buf + offset;
		/* reserve extra byte to hold cmd */
		offset += len + 1;
	}

	mode->priv_info->switch_data = priv_data;

	return 0;
}

static int s6e3hc2_gamma_read_mode(struct panel_switch_data *pdata,
				   const struct dsi_display_mode *mode)
{
	const struct s6e3hc2_panel_data *priv_data;
	int rc;

	rc = s6e3hc2_gamma_alloc_mode_memory(mode);
	if (rc)
		return rc;

	priv_data = mode->priv_info->switch_data;

	switch (mode->timing.refresh_rate) {
	case 60:
		rc = s6e3hc2_gamma_read_otp(pdata, priv_data);
		break;
	case 90:
		rc = s6e3hc2_gamma_read_flash(pdata, priv_data);
		break;
	default:
		pr_warn("Unknown refresh rate!\n");
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int s6e3hc2_gamma_read_tables(struct panel_switch_data *pdata)
{
	struct s6e3hc2_switch_data *sdata;
	const struct dsi_display_mode *mode;
	struct mipi_dsi_device *dsi;
	const u8 unlock_cmd[] = { 0xF0, 0x5A, 0x5A };
	const u8 lock_cmd[]   = { 0xF0, 0xA5, 0xA5 };
	int i, rc = 0;

	if (unlikely(!pdata || !pdata->panel))
		return -ENOENT;

	sdata = container_of(pdata, struct s6e3hc2_switch_data, base);
	if (sdata->gamma_ready)
		return 0;

	dsi = &pdata->panel->mipi_device;
	if (DSI_WRITE_CMD_BUF(dsi, unlock_cmd))
		return -EFAULT;

	for_each_display_mode(i, mode, pdata->panel) {
		rc = s6e3hc2_gamma_read_mode(pdata, mode);
		if (rc) {
			pr_err("Unable to read gamma for mode #%d\n", i);
			goto abort;
		}
	}

	sdata->gamma_ready = true;

abort:
	if (DSI_WRITE_CMD_BUF(dsi, lock_cmd))
		return -EFAULT;

	return rc;
}

static void s6e3hc2_gamma_work(struct kthread_work *work)
{
	struct s6e3hc2_switch_data *sdata;
	struct dsi_panel *panel;

	sdata = container_of(work, struct s6e3hc2_switch_data, gamma_work);
	panel = sdata->base.panel;

	if (!panel)
		return;

	mutex_lock(&panel->panel_lock);
	s6e3hc2_gamma_read_tables(&sdata->base);
	mutex_unlock(&panel->panel_lock);
}

static void s6e3hc2_gamma_print(struct seq_file *seq,
				const struct dsi_display_mode *mode)
{
	const struct s6e3hc2_panel_data *priv_data;
	int i, j;

	if (!mode || !mode->priv_info)
		return;

	seq_printf(seq, "\n=== %dhz Mode Gamma ===\n",
		   mode->timing.refresh_rate);

	priv_data = mode->priv_info->switch_data;

	if (!priv_data) {
		seq_puts(seq, "No data available!\n");
		return;
	}

	for (i = 0; i < S6E3HC2_NUM_GAMMA_TABLES; i++) {
		const size_t len = s6e3hc2_gamma_tables[i].len;
		const u8 cmd = s6e3hc2_gamma_tables[i].cmd;
		const u8 *buf = priv_data->gamma_data[i] + 1;

		seq_printf(seq, "0x%02X:", cmd);
		for (j = 0; j < len; j++) {
			if (j && (j % 8) == 0)
				seq_puts(seq, "\n     ");
			seq_printf(seq, " %02X", buf[j]);
		}
		seq_puts(seq, "\n");
	}
}

static int debugfs_s6e3hc2_gamma_read(struct seq_file *seq, void *data)
{
	struct panel_switch_data *pdata = seq->private;
	int i, rc;

	if (unlikely(!pdata || !pdata->panel))
		return -EINVAL;

	if (!dsi_panel_initialized(pdata->panel))
		return -EPIPE;

	mutex_lock(&pdata->panel->panel_lock);
	rc = s6e3hc2_gamma_read_tables(pdata);
	if (!rc) {
		const struct dsi_display_mode *mode;

		for_each_display_mode(i, mode, pdata->panel)
			s6e3hc2_gamma_print(seq, mode);
	}
	mutex_unlock(&pdata->panel->panel_lock);

	return rc;
}

ssize_t debugfs_s6e3hc2_gamma_write(struct file *file,
				    const char __user *user_buf,
				    size_t count, loff_t *ppos)
{
	struct seq_file *seq = file->private_data;
	struct panel_switch_data *pdata = seq->private;
	struct s6e3hc2_switch_data *sdata;

	if (!pdata || !pdata->panel)
		return -EINVAL;

	sdata = container_of(pdata, struct s6e3hc2_switch_data, base);

	mutex_lock(&pdata->panel->panel_lock);
	sdata->gamma_ready = false;
	mutex_unlock(&pdata->panel->panel_lock);

	return count;
}

static int debugfs_s6e3hc2_gamma_open(struct inode *inode, struct file *f)
{
	return single_open(f, debugfs_s6e3hc2_gamma_read, inode->i_private);
}

static const struct file_operations s6e3hc2_read_gamma_fops = {
	.owner   = THIS_MODULE,
	.open    = debugfs_s6e3hc2_gamma_open,
	.write   = debugfs_s6e3hc2_gamma_write,
	.read    = seq_read,
	.release = single_release,
};

static struct panel_switch_data *s6e3hc2_switch_create(struct dsi_panel *panel)
{
	struct s6e3hc2_switch_data *sdata;
	int rc;

	sdata = devm_kzalloc(panel->parent, sizeof(*sdata), GFP_KERNEL);
	if (!sdata)
		return ERR_PTR(-ENOMEM);

	rc = panel_switch_data_init(panel, &sdata->base);
	if (rc)
		return ERR_PTR(rc);

	kthread_init_work(&sdata->gamma_work, s6e3hc2_gamma_work);
	debugfs_create_file("gamma", 0600, sdata->base.debug_root,
			    &sdata->base, &s6e3hc2_read_gamma_fops);

	return &sdata->base;
}

static void s6e3hc2_switch_data_destroy(struct panel_switch_data *pdata)
{
	struct s6e3hc2_switch_data *sdata;

	sdata = container_of(pdata, struct s6e3hc2_switch_data, base);

	panel_switch_data_deinit(pdata);
	if (pdata->panel && pdata->panel->parent)
		devm_kfree(pdata->panel->parent, sdata);
}

static void s6e3hc2_perform_switch(struct panel_switch_data *pdata,
				   const struct dsi_display_mode *mode)
{
	struct mipi_dsi_device *dsi = &pdata->panel->mipi_device;
	const u8 unlock_cmd[] = { 0xF0, 0x5A, 0x5A };
	const u8 lock_cmd[]   = { 0xF0, 0xA5, 0xA5 };

	if (!mode)
		return;

	if (DSI_WRITE_CMD_BUF(dsi, unlock_cmd))
		return;

	panel_switch_cmd_set_transfer(pdata, mode);
	s6e3hc2_gamma_update(pdata, mode);

	DSI_WRITE_CMD_BUF(dsi, lock_cmd);
}

static int s6e3hc2_post_enable(struct panel_switch_data *pdata)
{
	struct s6e3hc2_switch_data *sdata;

	if (unlikely(!pdata || !pdata->panel))
		return -ENOENT;

	sdata = container_of(pdata, struct s6e3hc2_switch_data, base);

	kthread_flush_work(&sdata->gamma_work);
	if (!sdata->gamma_ready)
		kthread_queue_work(&pdata->worker, &sdata->gamma_work);

	return 0;
}

const struct panel_switch_funcs s6e3hc2_switch_funcs = {
	.create = s6e3hc2_switch_create,
	.destroy = s6e3hc2_switch_data_destroy,
	.perform_switch = s6e3hc2_perform_switch,
	.post_enable = s6e3hc2_post_enable,
};

static const struct of_device_id panel_switch_dt_match[] = {
	{
		.compatible = DSI_PANEL_GAMMA_NAME,
		.data = &s6e3hc2_switch_funcs,
	},
	{},
};

int dsi_panel_switch_init(struct dsi_panel *panel)
{
	struct panel_switch_data *pdata = NULL;
	const struct panel_switch_funcs *funcs;
	const struct of_device_id *match;

	match = of_match_node(panel_switch_dt_match, panel->panel_of_node);
	if (match && match->data)
		funcs = match->data;
	else
		funcs = &panel_switch_default_funcs;

	if (funcs->create)
		pdata = funcs->create(panel);

	if (IS_ERR_OR_NULL(pdata))
		return -ENOENT;

	pdata->funcs = funcs;

	return 0;
}

void dsi_panel_switch_put_mode(struct dsi_display_mode *mode)
{
	if (likely(mode->priv_info)) {
		kfree(mode->priv_info->switch_data);
		mode->priv_info->switch_data = NULL;
	}
}

/*
 * This should be called without panel_lock as flush/wait on worker can
 * deadlock while holding it
 */
void dsi_panel_switch_destroy(struct dsi_panel *panel)
{
	struct panel_switch_data *pdata = panel->private_data;
	struct dsi_display_mode *mode;
	int i;

	for_each_display_mode(i, mode, pdata->panel)
		dsi_panel_switch_put_mode(mode);

	if (pdata && pdata->funcs && pdata->funcs->destroy)
		pdata->funcs->destroy(pdata);
}