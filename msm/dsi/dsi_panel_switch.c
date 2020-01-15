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
#define DSI_PANEL_SWITCH_NAME "google,dsi_panel_switch"

#define TE_TIMEOUT_MS	50

#define for_each_display_mode(i, mode, panel) \
	for (i = 0, mode = dsi_panel_to_display(panel)->modes; \
		i < panel->num_timing_nodes; i++, mode++)


#define DSI_WRITE_CMD_BUF(dsi, cmd) \
	(IS_ERR_VALUE(mipi_dsi_dcs_write_buffer(dsi, cmd, ARRAY_SIZE(cmd))))

#define S6E3HC2_DEFAULT_FPS 60

static const u8 unlock_cmd[] = { 0xF0, 0x5A, 0x5A };
static const u8 lock_cmd[]   = { 0xF0, 0xA5, 0xA5 };

struct panel_switch_funcs {
	struct panel_switch_data *(*create)(struct dsi_panel *panel);
	void (*destroy)(struct panel_switch_data *pdata);
	void (*put_mode)(struct dsi_display_mode *mode);
	void (*perform_switch)(struct panel_switch_data *pdata,
			       const struct dsi_display_mode *mode);
	int (*post_enable)(struct panel_switch_data *pdata);
	int (*send_nolp_cmds)(struct dsi_panel *panel);
};

struct panel_switch_data {
	struct dsi_panel *panel;
	struct dentry *debug_root;

	struct kthread_work switch_work;
	struct kthread_worker worker;
	struct task_struct *thread;

	const struct dsi_display_mode *display_mode;
	const struct dsi_display_mode *idle_mode;
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

static inline bool is_display_mode_same(const struct dsi_display_mode *m1,
					const struct dsi_display_mode *m2)
{
	return m1 && m2 && m1->timing.refresh_rate == m2->timing.refresh_rate;
}

static inline void sde_atrace_mode_fps(const struct panel_switch_data *pdata,
				       const struct dsi_display_mode *mode)
{
	sde_atrace('C', pdata->thread, "FPS", mode->timing.refresh_rate);
}

ssize_t panel_dsi_write_buf(struct dsi_panel *panel,
			    const void *data, size_t len, bool send_last)
{
	const struct mipi_dsi_device *dsi = &panel->mipi_device;
	const struct mipi_dsi_host_ops *ops = panel->host->ops;
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.tx_buf = data,
		.tx_len = len,
		.flags = 0,
	};

	switch (len) {
	case 0:
		return -EINVAL;

	case 1:
		msg.type = MIPI_DSI_DCS_SHORT_WRITE;
		break;

	case 2:
		msg.type = MIPI_DSI_DCS_SHORT_WRITE_PARAM;
		break;

	default:
		msg.type = MIPI_DSI_DCS_LONG_WRITE;
		break;
	}

	if (send_last)
		msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;

	return ops->transfer(panel->host, &msg);
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

enum s6e3hc2_wrctrld_flags {
	S6E3HC2_WRCTRLD_DIMMING_BIT	= BIT(3),
	S6E3HC2_WRCTRLD_FRAME_RATE_BIT	= BIT(4),
	S6E3HC2_WRCTRLD_BCTRL_BIT	= BIT(5),
	S6E3HC2_WRCTRLD_HBM_BIT		= BIT(7) | BIT(6),
};

struct s6e3hc2_wrctrl_data {
	bool hbm_enable;
	bool dimming_active;
	u32 refresh_rate;
};

static int s6e3hc2_write_ctrld_reg(struct dsi_panel *panel,
				   const struct s6e3hc2_wrctrl_data *data,
				   bool send_last)
{
	u8 wrctrl_reg = S6E3HC2_WRCTRLD_BCTRL_BIT;
	u8 payload[2] = { MIPI_DCS_WRITE_CONTROL_DISPLAY, 0 };

	if (data->hbm_enable)
		wrctrl_reg |= S6E3HC2_WRCTRLD_HBM_BIT;

	if (data->dimming_active)
		wrctrl_reg |= S6E3HC2_WRCTRLD_DIMMING_BIT;

	if (data->refresh_rate == 90)
		wrctrl_reg |= S6E3HC2_WRCTRLD_FRAME_RATE_BIT;

	pr_debug("hbm_enable: %d dimming_active: %d refresh_rate: %d hz\n",
		data->hbm_enable, data->dimming_active, data->refresh_rate);

	payload[1] = wrctrl_reg;

	return panel_dsi_write_buf(panel, &payload, sizeof(payload), send_last);
}

static int s6e3hc2_switch_mode_update(struct dsi_panel *panel,
				      const struct dsi_display_mode *mode,
				      bool send_last)
{
	struct s6e3hc2_wrctrl_data data = {0};

	if (unlikely(!mode))
		return -EINVAL;

	data.refresh_rate = mode->timing.refresh_rate;

	return s6e3hc2_write_ctrld_reg(panel, &data, send_last);
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
	struct dsi_panel *panel = pdata->panel;

	SDE_ATRACE_BEGIN(__func__);
	if (pdata->funcs && pdata->funcs->perform_switch)
		pdata->funcs->perform_switch(pdata, mode);

	if (pdata->switch_pending) {
		pdata->switch_pending = false;
		wake_up_all(&pdata->switch_wq);
	}

	if (panel->bl_config.bl_device)
		sysfs_notify(&panel->bl_config.bl_device->dev.kobj, NULL,
					"state");

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

	sde_atrace_mode_fps(pdata, mode);
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

static bool dsi_mode_matches_cmdline(const struct dsi_display_mode *dm,
				     const struct drm_cmdline_mode *cm)
{

	if (!cm->refresh_specified && !cm->specified)
		return false;
	if (cm->refresh_specified && cm->refresh != dm->timing.refresh_rate)
		return false;
	if (cm->specified && (cm->xres != dm->timing.h_active ||
			      cm->yres != dm->timing.v_active))
		return false;
	return true;
}

static const struct dsi_display_mode *
display_mode_from_cmdline(const struct dsi_panel *panel,
			  const char *modestr)
{
	const struct dsi_display *display;
	const struct dsi_display_mode *mode;
	struct drm_cmdline_mode cm = {0};
	int i;

	display = dsi_panel_to_display(panel);
	if (!display)
		return ERR_PTR(-ENODEV);

	if (!drm_mode_parse_command_line_for_connector(modestr,
						       display->drm_conn,
						       &cm))
		return ERR_PTR(-EINVAL);

	for_each_display_mode(i, mode, panel) {
		if (dsi_mode_matches_cmdline(mode, &cm))
			return mode;
	}

	return NULL;
}

static const struct dsi_display_mode *
display_mode_from_user(const struct dsi_panel *panel,
		       const char __user *user_buf, size_t user_len)
{
	char modestr[40];
	size_t len = min(user_len, sizeof(modestr) - 1);
	int rc;

	rc = copy_from_user(modestr, user_buf, len);
	if (rc)
		return ERR_PTR(-EFAULT);

	modestr[len] = '\0';

	return display_mode_from_cmdline(panel, strim(modestr));
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

static int panel_idle(struct dsi_panel *panel)
{
	struct panel_switch_data *pdata = panel->private_data;
	const struct dsi_display_mode *idle_mode;

	if (unlikely(!pdata))
		return -EINVAL;

	kthread_flush_work(&pdata->switch_work);

	mutex_lock(&panel->panel_lock);
	idle_mode = pdata->idle_mode;
	if (idle_mode && !is_display_mode_same(idle_mode, panel->cur_mode)) {
		/*
		 * clocks are going to be turned off right after this call, so
		 * switch needs to happen synchronously
		 */
		pdata->display_mode = idle_mode;
		panel_switch_to_mode(pdata, idle_mode);

		sde_atrace_mode_fps(pdata, idle_mode);
	}
	mutex_unlock(&panel->panel_lock);

	SDE_ATRACE_INT("display_idle", 1);

	return 0;
}

static int panel_wakeup(struct dsi_panel *panel)
{
	struct panel_switch_data *pdata = panel->private_data;
	const struct dsi_display_mode *mode = NULL;

	if (unlikely(!pdata))
		return -EINVAL;

	mutex_lock(&panel->panel_lock);
	if (!is_display_mode_same(pdata->display_mode, panel->cur_mode))
		mode = panel->cur_mode;
	mutex_unlock(&panel->panel_lock);

	if (mode)
		panel_queue_switch(pdata, mode);

	SDE_ATRACE_INT("display_idle", 0);

	return 0;
}

static int panel_send_nolp(struct dsi_panel *panel)
{
	struct panel_switch_data *pdata = panel->private_data;

	if (unlikely(!pdata || !pdata->funcs))
		return -EINVAL;

	if (!pdata->funcs->send_nolp_cmds)
		return -EOPNOTSUPP;

	return pdata->funcs->send_nolp_cmds(panel);
}

static ssize_t debugfs_panel_switch_mode_write(struct file *file,
					       const char __user *user_buf,
					       size_t user_len,
					       loff_t *ppos)
{
	struct seq_file *seq = file->private_data;
	struct panel_switch_data *pdata = seq->private;
	const struct dsi_display_mode *mode;

	if (!pdata->panel || !dsi_panel_initialized(pdata->panel))
		return -ENOENT;

	mode = display_mode_from_user(pdata->panel, user_buf, user_len);
	if (IS_ERR(mode))
		return PTR_ERR(mode);
	else if (!mode)
		return -ENOENT;

	panel_queue_switch(pdata, mode);

	return user_len;
}

static int debugfs_panel_switch_mode_read(struct seq_file *seq, void *data)
{
	struct panel_switch_data *pdata = seq->private;
	const struct dsi_display_mode *mode;

	if (unlikely(!pdata->panel))
		return -ENOENT;

	mutex_lock(&pdata->panel->panel_lock);
	mode = pdata->display_mode;
	mutex_unlock(&pdata->panel->panel_lock);

	if (mode)
		seq_printf(seq, "%dx%d@%d\n", mode->timing.h_active,
			   mode->timing.v_active, mode->timing.refresh_rate);
	else
		seq_puts(seq, "unknown");

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

static ssize_t sysfs_idle_mode_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	const struct dsi_display *display;
	struct panel_switch_data *pdata;
	const struct dsi_display_mode *mode = NULL;
	ssize_t rc;

	display = dev_get_drvdata(dev);
	if (unlikely(!display || !display->panel ||
		     !display->panel->private_data))
		return -EINVAL;

	pdata = display->panel->private_data;

	mutex_lock(&pdata->panel->panel_lock);
	mode = pdata->idle_mode;
	mutex_unlock(&pdata->panel->panel_lock);

	if (mode)
		rc = snprintf(buf, PAGE_SIZE, "%dx%d@%d\n",
			      mode->timing.h_active, mode->timing.v_active,
			      mode->timing.refresh_rate);
	else
		rc = snprintf(buf, PAGE_SIZE, "none\n");

	return rc;
}
static ssize_t sysfs_idle_mode_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	const struct dsi_display *display;
	struct panel_switch_data *pdata;
	const struct dsi_display_mode *mode = NULL;

	display = dev_get_drvdata(dev);
	if (unlikely(!display || !display->panel ||
		     !display->panel->private_data))
		return -EINVAL;

	pdata = display->panel->private_data;
	if (count > 1 && strncmp(buf, "none", 4)) {
		char *modestr = kstrndup(buf, count, GFP_KERNEL);

		/* remove any trailing lf at end of sysfs input */
		mode = display_mode_from_cmdline(display->panel,
						 strim(modestr));
		kfree(modestr);

		if (IS_ERR(mode))
			return PTR_ERR(mode);
	}
	mutex_lock(&display->panel->panel_lock);
	pdata->idle_mode = mode;
	mutex_unlock(&display->panel->panel_lock);

	return count;
}

static DEVICE_ATTR(idle_mode, 0644,
		   sysfs_idle_mode_show,
		   sysfs_idle_mode_store);

static struct attribute *panel_switch_sysfs_attrs[] = {
	&dev_attr_idle_mode.attr,
	NULL,
};

static struct attribute_group panel_switch_sysfs_attrs_group = {
	.attrs = panel_switch_sysfs_attrs,
};

static const struct dsi_panel_funcs panel_funcs = {
	.mode_switch = panel_switch,
	.pre_disable = panel_flush_switch_queue,
	.pre_kickoff = panel_pre_kickoff,
	.idle        = panel_idle,
	.wakeup      = panel_wakeup,
	.post_enable = panel_post_enable,
	.pre_lp1     = panel_flush_switch_queue,
	.send_nolp   = panel_send_nolp,
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

	sysfs_create_group(&panel->parent->kobj,
			   &panel_switch_sysfs_attrs_group);

	return 0;
}

static void panel_switch_data_deinit(struct panel_switch_data *pdata)
{
	kthread_flush_worker(&pdata->worker);
	kthread_stop(pdata->thread);
	sysfs_remove_group(&pdata->panel->parent->kobj,
			   &panel_switch_sysfs_attrs_group);
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

#define S6E3HC2_GAMMA_BAND_LEN 45

/**
 * s6e3hc2_gamma_info - Information used to access gamma data on s6e3hc2.
 * @cmd: Command to use when writing/reading gamma from the DDIC.
 * @len: Total number of bytes to write/read from DDIC, including prefix_len.
 * @prefix_len: Number of bytes that precede gamma data when writing/reading
 *     from the DDIC. This is a subset of len.
 * @flash_offset: Address offset to use when reading from flash.
 * @cmd_group_with_next: Allow sending gamma tables in groups by setting this
 *     flag the gamma set will be sent together with the next set on the list.
 *     Order of commands matter when using this flag.
 */
const struct s6e3hc2_gamma_info {
	u8 cmd;
	u32 len;
	u32 prefix_len;
	u32 flash_offset;
	bool cmd_group_with_next;
} s6e3hc2_gamma_tables[] = {
	/* order of commands matter due to use of cmds grouping */
	{ 0xC8, S6E3HC2_GAMMA_BAND_LEN * 3, 0, 0x0000, false },
	{ 0xC9, S6E3HC2_GAMMA_BAND_LEN * 4, 0, 0x0087, true },
	{ 0xB3, 2 + S6E3HC2_GAMMA_BAND_LEN, 2, 0x013B, false },
};

#define S6E3HC2_NUM_GAMMA_TABLES ARRAY_SIZE(s6e3hc2_gamma_tables)

struct s6e3hc2_panel_data {
	u8 *gamma_data[S6E3HC2_NUM_GAMMA_TABLES];
};

/*
 * s6e3hc2_gamma_update() expects DD-IC to be in unlocked state, so
 * to make sure there are unlock/lock commands when calling this func.
 */
static void s6e3hc2_gamma_update(struct panel_switch_data *pdata,
				 const struct dsi_display_mode *mode)
{
	struct s6e3hc2_panel_data *priv_data;
	int i;

	if (unlikely(!mode || !mode->priv_info))
		return;

	priv_data = mode->priv_info->switch_data;
	if (unlikely(!priv_data))
		return;

	for (i = 0; i < S6E3HC2_NUM_GAMMA_TABLES; i++) {
		const struct s6e3hc2_gamma_info *info =
				&s6e3hc2_gamma_tables[i];
		/* extra byte for the dsi command */
		const size_t len = info->len + 1;
		const void *data = priv_data->gamma_data[i];
		const bool send_last =
				!info->cmd_group_with_next;

		if (WARN(!data, "Gamma table #%d not read\n", i))
			continue;

		if (IS_ERR_VALUE(panel_dsi_write_buf(pdata->panel, data, len,
					send_last)))
			pr_warn("failed sending gamma cmd 0x%02x\n",
				s6e3hc2_gamma_tables[i].cmd);
	}
}

static void s6e3hc2_gamma_update_reg_locked(struct panel_switch_data *pdata,
				 const struct dsi_display_mode *mode)
{
	struct dsi_panel *panel = pdata->panel;
	struct mipi_dsi_device *dsi = &panel->mipi_device;

	if (unlikely(!mode))
		return;

	DSI_WRITE_CMD_BUF(dsi, unlock_cmd);
	s6e3hc2_gamma_update(pdata, mode);
	DSI_WRITE_CMD_BUF(dsi, lock_cmd);
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
		const struct s6e3hc2_gamma_info *info =
			&s6e3hc2_gamma_tables[i];
		u8 *buf = priv_data->gamma_data[i];

		/* store cmd on first byte to send payload as is */
		*buf = info->cmd;
		buf++;

		rc = mipi_dsi_dcs_read(dsi, info->cmd, buf, info->len);
		if (rc != info->len)
			pr_warn("Only got %zd / %d bytes\n", rc, info->len);
	}

	SDE_ATRACE_END(__func__);

	return 0;
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

		for (j = info->prefix_len; j < info->len; j++, offset++) {
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

static int find_gamma_data_for_refresh_rate(struct dsi_panel *panel,
	u32 refresh_rate, u8 ***gamma_data)
{
	struct dsi_display_mode *mode;
	int i;

	if (!gamma_data)
		return -EINVAL;

	for_each_display_mode(i, mode, panel)
		if (mode->timing.refresh_rate == refresh_rate) {
			struct s6e3hc2_panel_data *priv_data =
				mode->priv_info->switch_data;

			if (unlikely(!priv_data))
				return -ENODATA;

			*gamma_data = priv_data->gamma_data;
			return 0;
		}

	return -ENODATA;
}

/*
 * For some modes, gamma curves are located in registers addresses that require
 * an offset to read/write. Because we cannot access a register offset directly,
 * we must read the portion of the data that precedes the gamma curve data
 * itself ("prefix") as well. In such cases, we read the prefix + gamma curve
 * data from DDIC registers, and only gamma curve data from flash.
 *
 * This function looks for such gamma curves, and adjusts gamma data read from
 * flash to include the prefix read from registers. The result is that, for all
 * modes, wherever the gamma curves were read from (registers or flash), when
 * that gamma data is written back to registers the write includes the original
 * prefix.
 * In other words, when we write gamma data to registers, we do not modify
 * prefix data; we only modify gamma data.
 */
static int s6e3hc2_gamma_set_prefixes(struct panel_switch_data *pdata)
{
	int i;
	int rc = 0;
	u8 **gamma_data_otp;
	u8 **gamma_data_flash;

	/*
	 * For s6e3hc2, 60Hz gamma curves are read from OTP and 90Hz
	 * gamma curves are read from flash.
	 */
	rc = find_gamma_data_for_refresh_rate(pdata->panel, 60,
		&gamma_data_otp);
	if (rc) {
		pr_err("Error setting gamma prefix: no matching OTP mode, err %d\n",
			rc);
		return rc;
	}

	rc = find_gamma_data_for_refresh_rate(pdata->panel, 90,
		&gamma_data_flash);
	if (rc) {
		pr_err("Error setting gamma prefix: no matching flash mode, err %d\n",
			rc);
		return rc;
	}

	for (i = 0; i < S6E3HC2_NUM_GAMMA_TABLES; i++) {
		const struct s6e3hc2_gamma_info *gamma_info =
			&s6e3hc2_gamma_tables[i];
		u8 *gamma_curve_otp = gamma_data_otp[i];
		u8 *gamma_curve_flash = gamma_data_flash[i];

		if (!gamma_info->prefix_len)
			continue;

		/* skip command byte */
		gamma_curve_otp++;
		gamma_curve_flash++;

		memcpy(gamma_curve_flash, gamma_curve_otp,
			gamma_info->prefix_len);
	}

	return rc;
}

static int s6e3hc2_gamma_read_tables(struct panel_switch_data *pdata)
{
	struct s6e3hc2_switch_data *sdata;
	const struct dsi_display_mode *mode;
	struct mipi_dsi_device *dsi;
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

	rc = s6e3hc2_gamma_set_prefixes(pdata);
	if (rc) {
		pr_err("Unable to set gamma prefix\n");
		goto abort;
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

static int s6e3hc2_check_gamma_infos(const struct s6e3hc2_gamma_info *infos,
		size_t num_infos)
{
	int i;

	if (!infos) {
		pr_err("Null gamma infos\n");
		return -EINVAL;
	}

	for (i = 0; i < num_infos; i++) {
		const struct s6e3hc2_gamma_info *info = &infos[i];

		if (unlikely(info->prefix_len >= info->len)) {
			pr_err("Gamma prefix length (%u) >= total length length (%u)\n",
				info->prefix_len, info->len);
			return -EINVAL;
		}
	}

	return 0;
}

static struct panel_switch_data *s6e3hc2_switch_create(struct dsi_panel *panel)
{
	struct s6e3hc2_switch_data *sdata;
	int rc;

	rc = s6e3hc2_check_gamma_infos(s6e3hc2_gamma_tables,
		S6E3HC2_NUM_GAMMA_TABLES);
	if (rc)
		return ERR_PTR(rc);

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
	struct dsi_panel *panel = pdata->panel;
	struct mipi_dsi_device *dsi = &panel->mipi_device;

	if (!mode)
		return;

	if (DSI_WRITE_CMD_BUF(dsi, unlock_cmd))
		return;

	s6e3hc2_switch_mode_update(panel, mode, false);
	s6e3hc2_gamma_update(pdata, mode);

	DSI_WRITE_CMD_BUF(dsi, lock_cmd);
}

int s6e3hc2_send_nolp_cmds(struct dsi_panel *panel)
{
	struct panel_switch_data *pdata;
	struct dsi_display_mode *cur_mode;
	struct dsi_panel_cmd_set *cmd;
	int rc = 0;

	if (unlikely(!panel || !panel->cur_mode))
		return -EINVAL;

	pdata = panel->private_data;
	cur_mode = panel->cur_mode;

	cmd = &cur_mode->priv_info->cmd_sets[DSI_CMD_SET_NOLP];
	rc = dsi_panel_cmd_set_transfer(panel, cmd);
	if (rc) {
		pr_debug("[%s] failed to send DSI_CMD_SET_NOLP cmd, rc=%d\n",
		       panel->name, rc);
		return rc;
	}

	s6e3hc2_gamma_update(pdata, cur_mode);

	cmd = &cur_mode->priv_info->cmd_sets[DSI_CMD_SET_POST_NOLP];
	rc = dsi_panel_cmd_set_transfer(panel, cmd);
	if (rc)
		pr_debug("[%s] failed to send DSI_CMD_SET_POST_NOLP cmd, rc=%d\n",
		       panel->name, rc);
	return rc;
}

static inline bool s6e3hc2_need_update_gamma(
			const struct panel_switch_data *pdata,
			const struct dsi_display_mode *mode)
{
	return mode && pdata && pdata->panel &&
		(mode->timing.refresh_rate != S6E3HC2_DEFAULT_FPS) &&
		!(mode->dsi_mode_flags & DSI_MODE_FLAG_DMS) &&
		(pdata->panel->power_mode == SDE_MODE_DPMS_ON);
}

static int s6e3hc2_post_enable(struct panel_switch_data *pdata)
{
	struct s6e3hc2_switch_data *sdata;
	const struct dsi_display_mode *mode;

	if (unlikely(!pdata || !pdata->panel))
		return -ENOENT;

	sdata = container_of(pdata, struct s6e3hc2_switch_data, base);
	mode = pdata->panel->cur_mode;

	kthread_flush_work(&sdata->gamma_work);
	if (!sdata->gamma_ready) {
		kthread_queue_work(&pdata->worker, &sdata->gamma_work);
	} else if (s6e3hc2_need_update_gamma(pdata, mode)) {
		s6e3hc2_gamma_update_reg_locked(pdata, mode);
		pr_debug("Updated gamma for %dhz\n", mode->timing.refresh_rate);
	}

	return 0;
}

const struct panel_switch_funcs s6e3hc2_switch_funcs = {
	.create = s6e3hc2_switch_create,
	.destroy = s6e3hc2_switch_data_destroy,
	.perform_switch = s6e3hc2_perform_switch,
	.post_enable = s6e3hc2_post_enable,
	.send_nolp_cmds = s6e3hc2_send_nolp_cmds,
};

static const struct of_device_id panel_switch_dt_match[] = {
	{
		.compatible = DSI_PANEL_GAMMA_NAME,
		.data = &s6e3hc2_switch_funcs,
	},
	{
		.compatible = DSI_PANEL_SWITCH_NAME,
		.data = &panel_switch_default_funcs,
	},
	{},
};

int dsi_panel_switch_init(struct dsi_panel *panel)
{
	struct panel_switch_data *pdata = NULL;
	const struct panel_switch_funcs *funcs = NULL;
	const struct of_device_id *match;

	match = of_match_node(panel_switch_dt_match, panel->panel_of_node);
	if (match && match->data)
		funcs = match->data;

	if (!funcs) {
		pr_info("Panel switch is not supported\n");
		return 0;
	}

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
