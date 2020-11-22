/*
 * Copyright (c) 2017, The Linux Foundation. All rights reserved.
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

#define pr_fmt(fmt)	"%s:%d: " fmt, __func__, __LINE__
#include <linux/backlight.h>
#include <linux/debugfs.h>
#include <linux/of_gpio.h>
#include <linux/pwm.h>
#include <linux/sysfs.h>
#include <linux/list.h>
#include <linux/list_sort.h>
#include <video/mipi_display.h>

#include <dsi_drm.h>
#include <sde_crtc.h>
#include <sde_encoder.h>

#include "dsi_display.h"
#include "dsi_panel.h"
#include "sde_connector.h"

#ifdef CONFIG_UCI
#include <linux/uci/uci.h>
#endif
#ifdef CONFIG_UCI_NOTIFICATIONS_SCREEN_CALLBACKS
#include <linux/notification/notification.h>
#endif

#define BL_NODE_NAME_SIZE 32
#define BL_BRIGHTNESS_BUF_SIZE 2

#ifdef CONFIG_UCI
static int backlight_min = 3;
static bool backlight_dimmer = false;
static u32 last_brightness;
static bool first_brightness_set = false;
#endif

#ifdef CONFIG_UCI
extern void uci_set_forced_freq(int freq, bool force_mode_change);
extern void uci_release_forced_freq(bool force_mode_change);
#endif

#ifdef CONFIG_UCI
static struct dsi_backlight_config *bl_g;

static bool last_hbm_mode = false;

static int uci_switch_hbm(int on) {
	struct dsi_panel *panel = NULL;
	bool hbm_mode = !!on;

	if (!bl_g->hbm)
		return -ENOTSUPP;

	if (on && bl_g->bl_device->props.state & BL_CORE_FBBLANK) {
		return 0;
	}

	panel = container_of(bl_g, struct dsi_panel, bl_config);
	dsi_panel_try_update_hbm(panel, hbm_mode);

	pr_info("%s %d\n",__func__,on);
	last_hbm_mode = hbm_mode;
	return 0;
}

static int uci_lux_level = -1;
static int uci_lux_level_detailed = -1;
static bool uci_hbm_switch = false;
static bool uci_hbm_use_ambient_light = false;
static bool screen_wake_by_user = false;
static bool screen_on = true;

static bool is_lp_mode_on = false;
extern int kcal_internal_override(int kcal_sat, int kcal_val, int kcal_cont, int r, int g, int b);
extern int kcal_internal_restore(bool forced_update);
extern void kcal_force_update(void);
// user params
static bool lp_kcal_overlay = false;
static bool lp_kcal_overlay_always = false;
static bool lp_kcal_overlay_dynamic = false;
static int lp_kcal_overlay_level = 50;

//
extern void uci_force_sde_update(void);

static void uci_sys_listener(void) {
	if (screen_wake_by_user) {
		int new_lux_level = uci_get_sys_property_int_mm("lux_level", 0, 0, 270000);
		if (!uci_hbm_switch) {  // hbm switch is off..
			if (last_hbm_mode) { // and it's set to hbm already in driver.. switch it off
				uci_switch_hbm(0);
			}
		} else { // hbm switch is on...
			if (new_lux_level==0 && uci_hbm_use_ambient_light) {
				if (last_hbm_mode) {
					uci_switch_hbm(0);
				}
			} else { // new lux level is high...let's switch it on
				if (!last_hbm_mode || uci_lux_level == -1) { //... if it's not yet on...or fresh screen off/on cycle...
					uci_switch_hbm(1);
				}
			}
		}
		uci_lux_level = new_lux_level;
	}
	if (is_lp_mode_on && lp_kcal_overlay_dynamic) {
		int new_lux_level = uci_get_sys_property_int_mm("lux_level_detailed", 0, 0, 270000);
		pr_info("%s [aod_dimmer] is_lp_mode_on - sys - new lux level %d\n",__func__,new_lux_level);
		if (lp_kcal_overlay && new_lux_level <=10) {
			int lvl = lp_kcal_overlay_level + new_lux_level;
			if (kcal_internal_override(254,254,254,lvl,lvl,lvl)>0) {
				pr_info("%s [aod_dimmer] is_lp_mode_on - sys - force_update - lvl %d\n",__func__,lvl);
				kcal_force_update();
				uci_force_sde_update();
			}
		} else {
			kcal_internal_restore(true);
		}
		uci_lux_level_detailed = new_lux_level;
	}
}
static int dsi_backlight_update_status(struct backlight_device *bd);

static void uci_user_listener(void) {

	bool new_hbm_switch = !!uci_get_user_property_int_mm("hbm_switch", 0, 0, 1);
	bool new_hbm_use_ambient_light = !!uci_get_user_property_int_mm("hbm_use_ambient_light", 0, 0, 1);

	lp_kcal_overlay = !!uci_get_user_property_int_mm("lp_kcal_overlay", 0, 0, 1);
	lp_kcal_overlay_always = !!uci_get_user_property_int_mm("lp_kcal_overlay_always", 1, 0, 1);
	lp_kcal_overlay_dynamic = !!uci_get_user_property_int_mm("lp_kcal_overlay_dynamic", 1, 0, 1);
	lp_kcal_overlay_level = uci_get_user_property_int_mm("lp_kcal_overlay_level", 50, 20, 60);
	if (new_hbm_switch!=uci_hbm_switch || new_hbm_use_ambient_light!=uci_hbm_use_ambient_light) {
		uci_hbm_switch = new_hbm_switch;
		uci_hbm_use_ambient_light = new_hbm_use_ambient_light;
		uci_lux_level = -1;
		uci_sys_listener();
	}
	{
		bool change = false;
		int on = backlight_dimmer?1:0;
		int backlight_min_curr = backlight_min;

		backlight_min = uci_get_user_property_int_mm("backlight_min", backlight_min, 2, 128);
		on = !!uci_get_user_property_int_mm("backlight_dimmer", on, 0, 1);

		if (on != backlight_dimmer || backlight_min_curr != backlight_min) change = true;

		backlight_dimmer = on;

		if (first_brightness_set && change) {
			if (!(bl_g->bl_device->props.state & BL_CORE_FBBLANK)) {
				dsi_backlight_update_status(bl_g->bl_device);
			}
		}
	}
}
static void call_uci_sys(struct work_struct * call_uci_sys_work)
{
	uci_sys_listener();
}
static DECLARE_WORK(call_uci_sys_work, call_uci_sys);

static void call_switch_hbm(struct work_struct * call_switch_hbm_work)
{
	uci_switch_hbm(0);
}
static DECLARE_WORK(call_switch_hbm_work, call_switch_hbm);

static void ntf_listener(char* event, int num_param, char* str_param) {
        if (strcmp(event,NTF_EVENT_CHARGE_LEVEL) && strcmp(event, NTF_EVENT_INPUT)) {
                pr_info("%s dsi_backlight ntf listener event %s %d %s\n",__func__,event,num_param,str_param);
        }

        if (!strcmp(event,NTF_EVENT_SLEEP)) {
		uci_lux_level = -1;
		screen_wake_by_user = false;
		screen_on = false;
		//schedule_work(&call_switch_hbm_work); // don't call this, it will switch off by itself

		// after a screen off, last_hbm should be OFF as it turns off by itself
		last_hbm_mode = false;

        }
        if ((!strcmp(event,NTF_EVENT_LOCKED) && !!num_param)) { // locked
		uci_lux_level = -1;
		screen_wake_by_user = false;
		//schedule_work(&call_switch_hbm_work); // don't call this, it will switch off by itself
	}
        if (!strcmp(event,NTF_EVENT_WAKE_BY_USER)) {
		// screen just on...set lux level -1, so HBM will be set again if needed...
		uci_lux_level = -1;
		screen_on = true;
		screen_wake_by_user = true;

		// after a screen off, last_hbm should be OFF as it turns off by itself
		last_hbm_mode = false;
	}
	if (!strcmp(event,NTF_EVENT_WAKE_BY_FRAMEWORK)) {
		uci_lux_level = -1;
		screen_on = true;

		// after a screen off, last_hbm should be OFF as it turns off by itself
		last_hbm_mode = false;
	}
        if (!strcmp(event,NTF_EVENT_INPUT)) {
		//event -> wake by user is sure...trigger sys listener
		if (screen_on) {
			screen_wake_by_user = true;
			schedule_work(&call_uci_sys_work);
		}
	}
}
#endif

struct dsi_backlight_pwm_config {
	struct pwm_device *pwm_bl;
	bool pwm_enabled;
	u32 pwm_period_usecs;
};

static void dsi_panel_bl_hbm_free(struct device *dev,
	struct dsi_backlight_config *bl);

static void dsi_panel_bl_notifier_free(struct device *dev,
	struct dsi_backlight_config *bl);

static int dsi_panel_bl_find_range(struct dsi_backlight_config *bl,
		int brightness, u32 *range);

static inline bool is_lp_mode(unsigned long state)
{
	return (state & (BL_STATE_LP | BL_STATE_LP2)) != 0;
}

static inline bool is_on_mode(unsigned long state)
{
	return (!is_lp_mode(state) && !is_standby_mode(state));
}

static inline unsigned int regulator_mode_from_state(unsigned long state)
{
	if (is_standby_mode(state))
		return REGULATOR_MODE_STANDBY;
	else if (is_lp_mode(state))
		return REGULATOR_MODE_IDLE;
	else
		return REGULATOR_MODE_NORMAL;
}

static int dsi_panel_pwm_bl_register(struct dsi_backlight_config *bl);

static void dsi_panel_bl_free_unregister(struct dsi_backlight_config *bl)
{
	kfree(bl->priv);
}

static int dsi_backlight_update_dcs(struct dsi_backlight_config *bl, u32 bl_lvl)
{
	int rc = 0;
	struct dsi_panel *panel;
	struct mipi_dsi_device *dsi;
	size_t num_params;
	const u32 hbyte = bl->high_byte_offset;

	if (!bl || (bl_lvl > 0xffff)) {
		pr_err("invalid params\n");
		return -EINVAL;
	}

	panel = container_of(bl, struct dsi_panel, bl_config);
	/* if no change in backlight, abort */
	if (bl_lvl == bl->bl_actual)
		return 0;

	dsi = &panel->mipi_device;

	num_params = bl->bl_max_level >= BIT(hbyte) ? 2 : 1;
	if (num_params == 2) {
		u8 payload[2] = { bl_lvl >> hbyte, (BIT(hbyte) - 1) & bl_lvl };

		rc = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
					&payload, sizeof(payload));
	} else {
		u8 payload = bl_lvl;

		rc = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
					&payload, sizeof(payload));
	}

	if (rc < 0)
		pr_err("failed to update dcs backlight:%d\n", bl_lvl);

	return rc;
}

/* Linearly interpolate value x from range [x1, x2] to determine the
 * corresponding value in range [y1, y2].
 */
static int dsi_backlight_lerp(u16 x1, u16 x2, u16 y1, u16 y2, u16 x, u32 *y)
{
	if ((x2 < x1) || (y2 < y1))
		return -EINVAL;

	if (((x2 - x1) == 0) || (x <= x1))
		*y = y1;
	else if (x >= x2)
		*y = y2;
	else
		*y = DIV_ROUND_CLOSEST((x - x1) * (y2 - y1), x2 - x1) + y1;

	return 0;
}

static u32 dsi_backlight_calculate_normal(struct dsi_backlight_config *bl,
		int brightness)
{
	u32 bl_lvl = 0;
	int rc = 0;

	if (bl->lut) {
		/*
		 * look up panel brightness; the first entry in the LUT
		 corresponds to userspace brightness level 1
		 */
		if (WARN_ON(brightness > bl->brightness_max_level))
			bl_lvl = bl->lut[bl->brightness_max_level];
		else
			bl_lvl = bl->lut[brightness];
	} else {
		/* map UI brightness into driver backlight level rounding it */
		rc = dsi_backlight_lerp(
			1, bl->brightness_max_level,
#ifdef CONFIG_UCI
			backlight_dimmer ? backlight_min : (bl->bl_min_level ? : 1), bl->bl_max_level,
#else
			bl->bl_min_level ? : 1, bl->bl_max_level,
#endif
			brightness, &bl_lvl);
		if (unlikely(rc))
			pr_err("failed to linearly interpolate, brightness unmodified\n");
	}

	pr_debug("normal bl: bl_lut %sused\n", bl->lut ? "" : "un");

	return bl_lvl;
}
int dsi_panel_switch_update_hbm(struct dsi_panel *panel)
{
	if (!panel || !panel->funcs || !panel->funcs->update_hbm)
		return -EOPNOTSUPP;

	return panel->funcs->update_hbm(panel);
}

int dsi_backlight_hbm_dimming_start(struct dsi_backlight_config *bl,
	u32 num_frames, struct dsi_panel_cmd_set *stop_cmd)
{
	struct hbm_data *hbm = bl->hbm;

	if (!hbm || !num_frames)
		return 0;

	if (unlikely(!hbm->dimming_workq)) {
		pr_err("hbm: tried to start dimming, but missing worker thread\n");
		return -EINVAL;
	}

	if (!hbm->dimming_active) {
		struct dsi_display *display =
			dev_get_drvdata(hbm->panel->parent);
		int rc;

		if (likely(display->bridge &&
			display->bridge->base.encoder &&
			display->bridge->base.encoder->crtc)) {
			rc = drm_crtc_vblank_get(
				display->bridge->base.encoder->crtc);
		} else {
			pr_err("hbm: missing crtc during dimming start.\n");
			return -EINVAL;
		}

		if (rc) {
			pr_err("hbm: failed drm request to get vblank\n: %d",
				rc);
			return rc;
		}
	}

	hbm->dimming_frames_total = num_frames;
	hbm->dimming_frames_left = num_frames;
	hbm->dimming_stop_cmd = stop_cmd;
	hbm->dimming_active = true;

	pr_debug("hbm dimming starting\n");
	queue_work(hbm->dimming_workq, &hbm->dimming_work);

	return 0;
}

void dsi_backlight_hbm_dimming_stop(struct dsi_backlight_config *bl)
{
	struct dsi_display *display;
	struct hbm_data *hbm = bl->hbm;
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	int rc = 0;

	if (!hbm || !hbm->dimming_active)
		return;

	display = dev_get_drvdata(hbm->panel->parent);
	if (likely(display->bridge &&
		display->bridge->base.encoder &&
		display->bridge->base.encoder->crtc)) {
		drm_crtc_vblank_put(display->bridge->base.encoder->crtc);
	} else {
		pr_err("hbm: missing crtc during dimming end.\n");
	}

	hbm->dimming_frames_total = 0;
	hbm->dimming_frames_left = 0;
	hbm->dimming_active = false;

	if (hbm->dimming_stop_cmd) {
		rc = dsi_panel_switch_update_hbm(panel);
		if (rc == -EOPNOTSUPP)
			rc = dsi_panel_cmd_set_transfer(hbm->panel,
				hbm->dimming_stop_cmd);
		if (rc)
			pr_err("hbm: failed to disable brightness dimming.\n");
	}

	hbm->dimming_stop_cmd = NULL;

	if (panel->hbm_pending_irc_on) {
		rc = dsi_panel_bl_update_irc(bl, true);

		if (rc)
			pr_err("hmb sv: failed to enable IRC.\n");
		panel->hbm_pending_irc_on = false;
	}

	pr_debug("hbm dimming stopped\n");
}

static void dsi_backlight_hbm_dimming_restart(struct dsi_backlight_config *bl)
{
	struct hbm_data *hbm = bl->hbm;

	if (!hbm || !hbm->dimming_active)
		return;

	hbm->dimming_frames_left = hbm->dimming_frames_total;
	pr_debug("hbm: dimming restarted\n");
}

static int dsi_backlight_hbm_wait_frame(struct hbm_data *hbm)
{
	struct dsi_display *display = dev_get_drvdata(hbm->panel->parent);

	if (likely(display->bridge && display->bridge->base.encoder)) {
		int rc = sde_encoder_wait_for_event(
			display->bridge->base.encoder, MSM_ENC_VBLANK);
		if (rc)
			return rc;
	} else {
		pr_err("hbm: missing sde encoder, can't wait for vblank\n");
		return -EINVAL;
	}

	return 0;
}

static void dsi_backlight_hbm_dimming_work(struct work_struct *work)
{
	struct dsi_panel *panel;
	struct hbm_data *hbm =
		container_of(work, struct hbm_data, dimming_work);

	if (!hbm)
		return;

	panel = hbm->panel;
	while (hbm->dimming_active) {
		int rc = dsi_backlight_hbm_wait_frame(hbm);

		/*
		 * it's possible that this thread is running while the driver is
		 * attempting to shut down. if this is the case, the driver
		 * will signal for dimming to stop while holding panel_lock.
		 * so if we fail to acquire the lock, wait a bit, then check the
		 * state of dimming_active again.
		 */
		if (!mutex_trylock(&panel->panel_lock)) {
			usleep_range(1000, 2000);
			continue;
		}

		pr_debug("hbm: dimming waited on frame %d of %d\n",
			hbm->dimming_frames_left, hbm->dimming_frames_total);
		if (!hbm->dimming_active) {
			mutex_unlock(&panel->panel_lock);
			break;
		}

		if (rc) {
			pr_err("hbm: failed to wait for vblank, disabling dimming now\n");
			hbm->dimming_frames_left = 0;
		} else if (hbm->dimming_frames_left > 0) {
			hbm->dimming_frames_left--;
		}

		if (!hbm->dimming_frames_left)
			dsi_backlight_hbm_dimming_stop(&panel->bl_config);

		mutex_unlock(&panel->panel_lock);
	}
}

int dsi_backlight_hbm_find_range(struct dsi_backlight_config *bl,
		int brightness, u32 *range)
{
	u32 i;

	if (!bl || !bl->hbm || !range)
		return -EINVAL;

	for (i = 0; i < bl->hbm->num_ranges; i++) {
		if (brightness <= bl->hbm->ranges[i].user_bri_end) {
			*range = i;
			return 0;
		}
	}

	return -EINVAL;
}

static u32 dsi_backlight_calculate_hbm(struct dsi_backlight_config *bl,
		int brightness)
{
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	struct hbm_data *hbm = bl->hbm;
	struct hbm_range *range = NULL;
	u32 bl_lvl = 0;
	int rc = 0;
	/* It's unlikely that a brightness value of 0 will make it to this
	 * function, but if it does use the dimmest HBM range.
	 */
	u32 target_range = 0;

	if (likely(brightness)) {
		rc = dsi_backlight_hbm_find_range(bl, brightness,
			&target_range);
		if (rc) {
			pr_err("Did not find a matching HBM range for brightness %d\n",
				brightness);
			return bl->bl_actual;
		}
	}

	range = hbm->ranges + target_range;
	if (hbm->cur_range != target_range) {
		dsi_backlight_hbm_dimming_start(bl, range->num_dimming_frames,
			&range->dimming_stop_cmd);
		pr_info("hbm: range %d -> %d\n", hbm->cur_range, target_range);
		hbm->cur_range = target_range;

		rc = dsi_panel_switch_update_hbm(panel);
		if (rc == -EOPNOTSUPP)
			rc = dsi_panel_cmd_set_transfer(panel,
				&range->entry_cmd);
		if (rc) {
			pr_err("Failed to send command for range %d\n",
				target_range);
			return bl->bl_actual;
		}
	}

#ifdef CONFIG_UCI
	{
		int panel_bri_start = (backlight_dimmer && target_range==0) ? backlight_min : range->panel_bri_start; // normal range (0), backlight dimmer can be applied. Otherwise not (HBM).
		rc = dsi_backlight_lerp(
			range->user_bri_start, range->user_bri_end,
			panel_bri_start, range->panel_bri_end,
			brightness, &bl_lvl);
	}
#else
	rc = dsi_backlight_lerp(
		range->user_bri_start, range->user_bri_end,
		range->panel_bri_start, range->panel_bri_end,
		brightness, &bl_lvl);
#endif
	if (unlikely(rc))
		pr_err("hbm: failed to linearly interpolate, brightness unmodified\n");

	pr_debug("hbm: user %d-%d, panel %d-%d\n",
		range->user_bri_start, range->user_bri_end,
		range->panel_bri_start, range->panel_bri_end);

	return bl_lvl;
}

static u32 dsi_backlight_calculate(struct dsi_backlight_config *bl,
				   int brightness)
{
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	u32 bl_lvl = 0;
	u32 bl_temp;

	if (brightness <= 0)
		return 0;

	/* scale backlight */
	bl_temp = mult_frac(brightness, bl->bl_scale,
			MAX_BL_SCALE_LEVEL);

	bl_temp = mult_frac(bl_temp, bl->bl_scale_sv,
			MAX_SV_BL_SCALE_LEVEL);

	if (panel->hbm_mode != HBM_MODE_OFF)
		bl_lvl = dsi_backlight_calculate_hbm(bl, bl_temp);
	else
		bl_lvl = dsi_backlight_calculate_normal(bl, bl_temp);

	pr_info("brightness=%d, bl_scale=%d, sv=%d, bl_lvl=%d, hbm = %d\n",
			brightness, bl->bl_scale, bl->bl_scale_sv, bl_lvl,
			panel->hbm_mode);

	return bl_lvl;
}

static int dsi_backlight_update_status(struct backlight_device *bd)
{
	struct dsi_backlight_config *bl = bl_get_data(bd);
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	struct dsi_display *display;
	int brightness = bd->props.brightness;
	int bl_lvl;
	int rc = 0;
	bool need_notify = false;

	mutex_lock(&panel->panel_lock);
	mutex_lock(&bl->state_lock);
	if ((bd->props.state & (BL_CORE_FBBLANK | BL_CORE_SUSPENDED)) ||
			(bd->props.power != FB_BLANK_UNBLANK))
		brightness = 0;

	bl_lvl = dsi_backlight_calculate(bl, brightness);
	if (bl_lvl == bl->bl_actual && bl->last_state == bd->props.state)
		goto done;

	if (!bl->allow_bl_update) {
		bl->bl_update_pending = true;
		goto done;
	}

	dsi_backlight_hbm_dimming_restart(bl);

	if (dsi_panel_initialized(panel) && bl->update_bl) {
		pr_info("req:%d bl:%d state:0x%x\n",
			bd->props.brightness, bl_lvl, bd->props.state);

		rc = bl->update_bl(bl, bl_lvl);
		if (rc) {
			pr_err("unable to set backlight (%d)\n", rc);
			goto done;
		}
		bl->bl_update_pending = false;
		need_notify = true;
		if (bl->bl_notifier && is_on_mode(bd->props.state)
				&& !(dsi_panel_get_hbm(panel))) {
			u32 target_range = 0;

			rc = dsi_panel_bl_find_range(bl, brightness, &target_range);
			if (rc) {
				pr_err("unable to find range from the backlight table (%d)\n", rc);
			} else if (bl->bl_notifier->cur_range != target_range) {
				bl->bl_notifier->cur_range = target_range;
				sysfs_notify(&bd->dev.kobj, NULL, "brightness");
				pr_debug("cur_range = %d, brightness = %d\n",
						bl->bl_notifier->cur_range, brightness);
			}
		}
	}
	bl->bl_actual = bl_lvl;
	bl->last_state = bd->props.state;
#ifdef CONFIG_UCI
	if (bl_lvl>0) {
		last_brightness = bl_lvl;
	}
	first_brightness_set = true;
#endif

done:
	mutex_unlock(&bl->state_lock);
	mutex_unlock(&panel->panel_lock);

	/* skip notifying user space if bl is 0 */
	if (likely(need_notify && brightness)) {
		display = dev_get_drvdata(panel->parent);
		if (unlikely(!display))
			return rc;

		sde_connector_event_notify(display->drm_conn,
			DRM_EVENT_SYS_BACKLIGHT, sizeof(u32), brightness);
	}

	return rc;
}

static int dsi_backlight_get_brightness(struct backlight_device *bd)
{
	struct dsi_backlight_config *bl = bl_get_data(bd);

	return bl->bl_actual;
}

static const struct backlight_ops dsi_backlight_ops = {
	.update_status = dsi_backlight_update_status,
	.get_brightness = dsi_backlight_get_brightness,
};

static ssize_t alpm_mode_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct backlight_device *bd = to_backlight_device(dev);
	struct dsi_backlight_config *bl = bl_get_data(bd);
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	int rc, alpm_mode;
	const unsigned int lp_state = bl->bl_device->props.state &
			(BL_STATE_LP | BL_STATE_LP2);

	rc = kstrtoint(buf, 0, &alpm_mode);
	if (rc)
		return rc;

	if (bl->bl_device->props.state & BL_CORE_FBBLANK) {
		return -EINVAL;
	} else if ((alpm_mode == 1) && (lp_state != BL_STATE_LP)) {
		pr_info("activating lp1 mode\n");
		dsi_panel_set_lp1(panel);
	} else if ((alpm_mode > 1) && !(lp_state & BL_STATE_LP2)) {
		pr_info("activating lp2 mode\n");
		dsi_panel_set_lp2(panel);
	} else if (!alpm_mode && lp_state) {
		pr_info("activating normal mode\n");
		dsi_panel_set_nolp(panel);
	}

	return count;
}

static ssize_t alpm_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct backlight_device *bd = to_backlight_device(dev);
	int alpm_mode;

	if (bd->props.state & BL_STATE_LP2)
		alpm_mode = 2;
	else
		alpm_mode = (bd->props.state & BL_STATE_LP) != 0;

	return sprintf(buf, "%d\n", alpm_mode);
}
static DEVICE_ATTR_RW(alpm_mode);

static ssize_t hbm_mode_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct backlight_device *bd = NULL;
	struct dsi_backlight_config *bl = NULL;
	struct dsi_panel *panel = NULL;
	int rc = 0;
	int hbm_mode = 0;

	/* dev is non-NULL, enforced by sysfs_create_file_ns */
	bd = to_backlight_device(dev);
	bl = bl_get_data(bd);

	if (!bl->hbm)
		return -ENOTSUPP;

	rc = kstrtoint(buf, 10, &hbm_mode);
	if (rc)
		return rc;

	panel = container_of(bl, struct dsi_panel, bl_config);
	rc = dsi_panel_update_hbm(panel, hbm_mode);
	if (rc) {
		pr_err("hbm_mode store failed: %d\n", rc);
		return rc;
	}
	pr_debug("hbm_mode set to %d\n", panel->hbm_mode);

#ifdef CONFIG_UCI
	last_hbm_mode = hbm_mode;
#endif
	return count;
}

static ssize_t hbm_mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct backlight_device *bd = NULL;
	struct dsi_backlight_config *bl = NULL;
	struct dsi_panel *panel = NULL;
	int hbm_mode = false;

	/* dev is non-NULL, enforced by sysfs_create_file_ns */
	bd = to_backlight_device(dev);
	bl = bl_get_data(bd);

	if (!bl->hbm)
		return snprintf(buf, PAGE_SIZE, "unsupported\n");

	panel = container_of(bl, struct dsi_panel, bl_config);
	hbm_mode = dsi_panel_get_hbm(panel);
#ifdef CONFIG_UCI
	last_hbm_mode = hbm_mode;
#endif

	return snprintf(buf, PAGE_SIZE, "%d\n", hbm_mode);
}

static DEVICE_ATTR_RW(hbm_mode);

static ssize_t hbm_sv_enabled_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct backlight_device *bd;
	struct dsi_backlight_config *bl;
	struct dsi_panel *panel;
	int rc = 0;
	bool hbm_sv_enabled = false;

	/* dev is non-NULL, enforced by sysfs_create_file_ns */
	bd = to_backlight_device(dev);
	bl = bl_get_data(bd);

	if (!bl->hbm)
		return -ENOTSUPP;

	rc = kstrtobool(buf, &hbm_sv_enabled);
	if (rc)
		return rc;

	panel = container_of(bl, struct dsi_panel, bl_config);
	if (!hbm_sv_enabled && panel->hbm_mode == HBM_MODE_SV)
		return -EBUSY;

	panel->hbm_sv_enabled = hbm_sv_enabled;

	return count;
}

static ssize_t hbm_sv_enabled_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct backlight_device *bd;
	struct dsi_backlight_config *bl;
	struct dsi_panel *panel;

	/* dev is non-NULL, enforced by sysfs_create_file_ns */
	bd = to_backlight_device(dev);
	bl = bl_get_data(bd);

	if (!bl->hbm)
		return snprintf(buf, PAGE_SIZE, "unsupported\n");

	panel = container_of(bl, struct dsi_panel, bl_config);
	return snprintf(buf, PAGE_SIZE, "%s\n",
			panel->hbm_sv_enabled ? "true" : "false");
}

static DEVICE_ATTR_RW(hbm_sv_enabled);

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct backlight_device *bd = to_backlight_device(dev);
	struct dsi_backlight_config *bl = bl_get_data(bd);
	struct dsi_panel *panel = container_of(bl,
					struct dsi_panel, bl_config);
	bool show_mode = false;
	char *statestr;
	int rc;

	mutex_lock(&bl->state_lock);
	if (is_standby_mode(bd->props.state)) {
		statestr = "Off";
	} else if (is_lp_mode(bd->props.state)) {
		statestr = "LP";
	} else {
		show_mode = true;
		if (dsi_panel_get_hbm(panel))
			statestr = "HBM";
		else
			statestr = "On";
	}
	mutex_unlock(&bl->state_lock);

	if (show_mode) {
		const struct dsi_display_mode *mode =
				get_panel_display_mode(panel);

		if (unlikely(!mode))
			return -ENODEV;

		rc = snprintf(buf, PAGE_SIZE, "%s: %dx%d@%d\n", statestr,
			 mode->timing.h_active, mode->timing.v_active,
			 mode->timing.refresh_rate);
	} else {
		rc = snprintf(buf, PAGE_SIZE, "%s\n", statestr);
	}

	return rc;
}

static DEVICE_ATTR_RO(state);

int parse_u32_buf(char *src, size_t src_len, u32 *out, size_t out_len)
{
	int rc = 0, cnt = 0;
	char *str;
	const char *delim = " ";

	if (unlikely(!src || !src_len || !out || !out_len))
		return -EINVAL;

	/* src_len is the length of src including null character '\0' */
	if (strnlen(src, src_len) == src_len)
		return -EINVAL;

	for (str = strsep(&src, delim); str != NULL; str = strsep(&src, delim)) {
		rc = kstrtou32(str, 0, out + cnt);
		if (rc)
			return -EINVAL;

		cnt++;

		if (out_len == cnt)
			break;
	}

	return cnt;
}

static ssize_t als_table_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{

	struct backlight_device *bd = to_backlight_device(dev);
	struct dsi_backlight_config *bl = bl_get_data(bd);
	ssize_t als_count, buf_dup_len;
	u8 i;
	u32 ranges[BL_RANGE_MAX] = {0};
	char *buf_dup;

	if (unlikely(!bl || !bl->bl_notifier))
		return -EINVAL;

	if (count == 0)
		return -EINVAL;

	buf_dup = kstrndup(buf, count, GFP_KERNEL);
	if (!buf_dup)
		return -ENOMEM;

	buf_dup_len = strlen(buf_dup) + 1;

	als_count = parse_u32_buf(buf_dup, buf_dup_len, ranges, BL_RANGE_MAX);
	if ((als_count < 0) || (als_count > BL_RANGE_MAX)) {
		kfree(buf_dup);
		pr_warn("als: incorrect parameters from als table node\n");
		return -EINVAL;
	}

	mutex_lock(&bl->state_lock);

	bl->bl_notifier->num_ranges = als_count;
	for (i = 0; i < bl->bl_notifier->num_ranges; i++)
		bl->bl_notifier->ranges[i] = ranges[i];

	mutex_unlock(&bl->state_lock);

	kfree(buf_dup);

	return count;
}

static ssize_t als_table_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct backlight_device *bd = to_backlight_device(dev);
	struct dsi_backlight_config *bl = bl_get_data(bd);
	ssize_t rc = 0;
	size_t len = 0;
	u32 i = 0;

	if (unlikely(!bl || !bl->bl_notifier))
		return -EINVAL;

	mutex_lock(&bl->state_lock);

	for (i = 0; i < bl->bl_notifier->num_ranges; i++) {
		rc = scnprintf((buf + len), PAGE_SIZE - len,
				"%u ", bl->bl_notifier->ranges[i]);
		if (rc < 0) {
			pr_warn("als: incorrect bl_notifier ranges\n");
			mutex_unlock(&bl->state_lock);
			return -EINVAL;
		}
		len = len + rc;
	}

	mutex_unlock(&bl->state_lock);

	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");

	return len;
}
static DEVICE_ATTR_RW(als_table);

static struct attribute *bl_device_attrs[] = {
	&dev_attr_alpm_mode.attr,
	&dev_attr_hbm_mode.attr,
	&dev_attr_hbm_sv_enabled.attr,
	&dev_attr_state.attr,
	&dev_attr_als_table.attr,
	NULL,
};
ATTRIBUTE_GROUPS(bl_device);

static int dsi_backlight_register(struct dsi_backlight_config *bl)
{
	static int display_count;
	char bl_node_name[BL_NODE_NAME_SIZE];
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.power = FB_BLANK_UNBLANK,
	};
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	struct regulator *reg;

	props.max_brightness = bl->brightness_max_level;
	props.brightness = bl->brightness_max_level / 2;

	snprintf(bl_node_name, BL_NODE_NAME_SIZE, "panel%u-backlight",
		 display_count);
	bl->bl_device = devm_backlight_device_register(panel->parent,
				bl_node_name, panel->parent, bl,
				&dsi_backlight_ops, &props);
	if (IS_ERR_OR_NULL(bl->bl_device)) {
		bl->bl_device = NULL;
		return -ENODEV;
	}

	if (sysfs_create_groups(&bl->bl_device->dev.kobj, bl_device_groups))
		pr_warn("unable to create device groups\n");

#ifdef CONFIG_UCI
	bl_g = bl;
	uci_add_sys_listener(uci_sys_listener);
	uci_add_user_listener(uci_user_listener);
	ntf_add_listener(ntf_listener);
#endif

	reg = regulator_get_optional(panel->parent, "lab");
	if (!PTR_ERR_OR_ZERO(reg)) {
		pr_info("LAB regulator found\n");
		panel->bl_config.lab_vreg = reg;
	}

	display_count++;
	return 0;
}

static unsigned long get_state_after_dpms(struct dsi_backlight_config *bl,
				   int power_mode)
{
	struct backlight_device *bd = bl->bl_device;
	unsigned long state = bd->props.state;

	switch (power_mode) {
	case SDE_MODE_DPMS_ON:
		state &= ~(BL_CORE_FBBLANK | BL_STATE_LP | BL_STATE_LP2);
		break;
	case SDE_MODE_DPMS_OFF:
		state &= ~(BL_STATE_LP | BL_STATE_LP2);
		state |= BL_CORE_FBBLANK;
		break;
	case SDE_MODE_DPMS_LP1:
		state |= BL_STATE_LP;
		state &= ~BL_STATE_LP2;
		break;
	case SDE_MODE_DPMS_LP2:
		state |= BL_STATE_LP | BL_STATE_LP2;
		break;
	}

	return state;
}

static int dsi_backlight_update_regulator(struct dsi_backlight_config *bl,
					  unsigned int state)
{
	int rc = 0;

	if (bl->lab_vreg) {
		const unsigned int mode = regulator_mode_from_state(state);
		const unsigned int last_mode =
				regulator_mode_from_state(bl->last_state);

		if (last_mode != mode) {
			pr_debug("set lab vreg mode: 0x%0x\n", mode);
			rc = regulator_set_mode(bl->lab_vreg, mode);
		}
	}

	return rc;
}

int dsi_backlight_early_dpms(struct dsi_backlight_config *bl, int power_mode)
{
	struct backlight_device *bd = bl->bl_device;
	unsigned long state;
	int rc = 0;

	if (!bd)
		return 0;

	pr_info("power_mode:%d state:0x%0x\n", power_mode, bd->props.state);

	mutex_lock(&bl->state_lock);
	state = get_state_after_dpms(bl, power_mode);
#ifdef CONFIG_UCI_NOTIFICATIONS_SCREEN_CALLBACKS
	if (is_lp_mode(state)) {
		pr_info("%s [aod_dimmer] lp_mode - last_brightness %d - lp_kcal_overlay_always %d\n",__func__,last_brightness,lp_kcal_overlay_always);
		if (lp_kcal_overlay && (last_brightness<=7 || lp_kcal_overlay_always)) {
			// last screen-fully-on screen brightness is low, or overlay_always is true, let's do it..
			if (kcal_internal_override(254,254,254,lp_kcal_overlay_level,lp_kcal_overlay_level,lp_kcal_overlay_level)>0) {
				kcal_force_update();
			}
		} else {
			kcal_internal_restore(true);
		}
		is_lp_mode_on = true;
		if (lp_kcal_overlay && lp_kcal_overlay_dynamic) {
			uci_lux_level_detailed = -1;
			write_uci_out("aod_lp_on");
		}
	} else {
		kcal_internal_restore(true);
		is_lp_mode_on = false;
		if (lp_kcal_overlay && lp_kcal_overlay_dynamic) {
			uci_lux_level_detailed = -1;
			write_uci_out("aod_lp_off");
		}
	}
#endif

	if (is_lp_mode(state)) {
		rc = dsi_backlight_update_regulator(bl, state);
		if (rc)
			pr_warn("Error updating regulator state: 0x%x (%d)\n",
				state, rc);
	}
	mutex_unlock(&bl->state_lock);

	return rc;
}

int dsi_backlight_late_dpms(struct dsi_backlight_config *bl, int power_mode)
{
	struct backlight_device *bd = bl->bl_device;
	unsigned long state;

	if (!bd)
		return 0;

	pr_debug("power_mode:%d state:0x%0x\n", power_mode, bd->props.state);

	mutex_lock(&bl->state_lock);
	state = get_state_after_dpms(bl, power_mode);

	if (!is_lp_mode(state)) {
		const int rc = dsi_backlight_update_regulator(bl, state);

		if (rc)
			pr_warn("Error updating regulator state: 0x%x (%d)\n",
				state, rc);
	}

	bd->props.power = state & BL_CORE_FBBLANK ? FB_BLANK_POWERDOWN :
			FB_BLANK_UNBLANK;
	bd->props.state = state;

	mutex_unlock(&bl->state_lock);
	backlight_update_status(bd);
	sysfs_notify(&bd->dev.kobj, NULL, "state");

	pr_info("sysfs_notify state:0x%0x\n", bd->props.state);

	return 0;
}

#define MAX_BINNED_BL_MODES 10

struct binned_lp_node {
	struct list_head head;
	const char *name;
	u32 bl_threshold;
	struct dsi_panel_cmd_set dsi_cmd;
	struct dentry *mode_dir;
};

struct binned_lp_data {
	struct list_head mode_list;
	struct binned_lp_node *last_lp_mode;
	struct binned_lp_node priv_pool[MAX_BINNED_BL_MODES];
};

static int dsi_panel_te2_lp_mode_update(struct dsi_panel *panel,
				struct binned_lp_node *node)
{
	int rc;

	if (unlikely(!panel->funcs || !panel->funcs->update_te2))
		return -EINVAL;

	if (unlikely(!panel->te2_config.te2_ready))
		return -EINVAL;

	if (node->bl_threshold > panel->te2_config.lp_threshold)
		panel->te2_config.current_type = TE2_EDGE_LP_HIGH;
	else
		panel->te2_config.current_type = TE2_EDGE_LP_LOW;

	rc = panel->funcs->update_te2(panel);
	if (rc < 0)
		pr_warn("TE2: LP '%s' mode failed, rc=%d\n", node->name, rc);

	return rc;
}

static int dsi_panel_binned_bl_update(struct dsi_backlight_config *bl,
				      u32 bl_lvl)
{
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	struct binned_lp_data *lp_data = bl->priv;
	struct binned_lp_node *node = NULL;
	struct backlight_properties *props = &bl->bl_device->props;

	if (is_lp_mode(props->state)) {
		struct binned_lp_node *tmp;

		list_for_each_entry(tmp, &lp_data->mode_list, head) {
			if (props->brightness <= tmp->bl_threshold) {
				node = tmp;
				break;
			}
		}
		WARN(!node, "unable to find lp node for bl_lvl: %d\n",
		     props->brightness);
	}

	if (node != lp_data->last_lp_mode) {
		lp_data->last_lp_mode = node;
		if (node) {
			pr_debug("switching display lp mode: %s (%d)\n",
				node->name, props->brightness);
			dsi_panel_cmd_set_transfer(panel, &node->dsi_cmd);

			dsi_panel_te2_lp_mode_update(panel, node);
		} else {
			/* ensure update after lpm */
			bl->bl_actual = -1;
		}
	}

	/* no need to send backlight command if HLPM active */
	if (node)
		return 0;

	return dsi_backlight_update_dcs(bl, bl_lvl);
}

static int _dsi_panel_binned_lp_parse(struct device_node *np,
				      struct binned_lp_node *node)
{
	int rc;
	u32 val = 0;

	rc = of_property_read_u32(np, "google,dsi-lp-brightness-threshold",
				  &val);
	/* treat lack of property as max threshold */
	node->bl_threshold = !rc ? val : UINT_MAX;

	rc = dsi_panel_parse_dt_cmd_set(np, "google,dsi-lp-command",
					"google,dsi-lp-command-state",
					&node->dsi_cmd);
	if (rc) {
		pr_err("Unable to parse dsi-lp-command\n");
		return rc;
	}

	of_property_read_string(np, "label", &node->name);

	pr_debug("Successfully parsed lp mode: %s threshold: %d\n",
		node->name, node->bl_threshold);

	return 0;
}

static int _dsi_panel_binned_bl_cmp(void *priv, struct list_head *lha,
				    struct list_head *lhb)
{
	struct binned_lp_node *a = list_entry(lha, struct binned_lp_node, head);
	struct binned_lp_node *b = list_entry(lhb, struct binned_lp_node, head);

	return a->bl_threshold - b->bl_threshold;
}

void dsi_panel_debugfs_create_binned_bl(struct dentry *parent,
					struct dsi_backlight_config *bl)
{
	struct dentry *r, *file;
	struct binned_lp_data *lp_data;
	struct binned_lp_node *tmp;
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);

	r = debugfs_create_dir("lp_modes", parent);
	if (IS_ERR(r)) {
		pr_err("create debugfs lp_modes failed\n");
		return;
	}

	lp_data = bl->priv;

	list_for_each_entry(tmp, &lp_data->mode_list, head) {
		tmp->mode_dir = debugfs_create_dir(tmp->name, r);
		if (IS_ERR(tmp->mode_dir)) {
			pr_err("create debugfs binned_bl failed\n");
			goto error;
		}

		file = debugfs_create_u32("threshold", 0600,
					  tmp->mode_dir,
					  &tmp->bl_threshold);
		if (IS_ERR_OR_NULL(file)) {
			pr_err("debugfs create threshold file failed\n");
			goto error;
		}

		if (dsi_panel_debugfs_create_cmdset(tmp->mode_dir, "cmd",
						    panel, &tmp->dsi_cmd)) {
			pr_err("debugfs create cmd file failed\n");
			goto error;
		}
	}

	return;

error:
	debugfs_remove_recursive(r);
}

static int dsi_panel_binned_lp_register(struct dsi_backlight_config *bl)
{
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	struct binned_lp_data *lp_data;
	struct device_node *lp_modes_np, *child_np;
	struct binned_lp_node *lp_node;
	int num_modes;
	int rc = -ENOTSUPP;

	lp_data = kzalloc(sizeof(*lp_data), GFP_KERNEL);
	if (!lp_data)
		return -ENOMEM;

	lp_modes_np = of_get_child_by_name(panel->panel_of_node,
					   "google,lp-modes");

	if (!lp_modes_np) {
		kfree(lp_data);
		return rc;
	}

	num_modes = of_get_child_count(lp_modes_np);
	if (!num_modes || (num_modes > MAX_BINNED_BL_MODES)) {
		pr_err("Invalid binned brightness modes: %d\n", num_modes);
		goto exit;
	}

	INIT_LIST_HEAD(&lp_data->mode_list);
	lp_node = lp_data->priv_pool;

	for_each_child_of_node(lp_modes_np, child_np) {
		rc = _dsi_panel_binned_lp_parse(child_np, lp_node);
		if (rc)
			goto exit;

		list_add_tail(&lp_node->head, &lp_data->mode_list);
		lp_node++;
		if (lp_node > &lp_data->priv_pool[MAX_BINNED_BL_MODES - 1]) {
			pr_err("Too many LP modes\n");
			rc = -ENOTSUPP;
			goto exit;
		}
	}
	list_sort(NULL, &lp_data->mode_list, _dsi_panel_binned_bl_cmp);

	bl->update_bl = dsi_panel_binned_bl_update;
	bl->unregister = dsi_panel_bl_free_unregister;
	bl->debugfs_init = dsi_panel_debugfs_create_binned_bl;
	bl->priv = lp_data;

exit:
	of_node_put(lp_modes_np);
	if (rc)
		kfree(lp_data);

	return rc;
}

static const struct of_device_id dsi_backlight_dt_match[] = {
	{
		.compatible = "google,dsi_binned_lp",
		.data = dsi_panel_binned_lp_register,
	},
	{}
};

void dsi_panel_bl_debugfs_init(struct dentry *parent, struct dsi_panel *panel)
{
	struct dsi_backlight_config *bl = &panel->bl_config;

	if (bl->debugfs_init)
		bl->debugfs_init(parent, bl);
}

int dsi_backlight_get_dpms(struct dsi_backlight_config *bl)
{
	struct backlight_device *bd = bl->bl_device;
	int power = 0;
	int state = 0;

	mutex_lock(&bl->state_lock);
	power = bd->props.power;
	state = bd->props.state;
	mutex_unlock(&bl->state_lock);

	if (power == FB_BLANK_POWERDOWN)
		return SDE_MODE_DPMS_OFF;
	else if (state & BL_STATE_LP2)
		return SDE_MODE_DPMS_LP2;
	else if (state & BL_STATE_LP)
		return SDE_MODE_DPMS_LP1;
	else
		return SDE_MODE_DPMS_ON;
}

static int dsi_panel_bl_parse_hbm_node(struct device *parent,
	struct dsi_backlight_config *bl, struct device_node *np,
	struct hbm_range *range)
{
	int rc;
	u32 val = 0;

	rc = of_property_read_u32(np,
		"google,dsi-hbm-range-brightness-threshold", &val);
	if (rc) {
		pr_err("Unable to parse dsi-hbm-range-brightness-threshold");
		return rc;
	}
	if (val > bl->brightness_max_level) {
		pr_err("hbm-brightness-threshold exceeds max userspace brightness\n");
		return rc;
	}
	range->user_bri_start = val;

	rc = of_property_read_u32(np, "google,dsi-hbm-range-bl-min-level",
		&val);
	if (rc) {
		pr_err("dsi-hbm-range-bl-min-level unspecified\n");
		return rc;
	}
	range->panel_bri_start = val;

	rc = of_property_read_u32(np, "google,dsi-hbm-range-bl-max-level",
		&val);
	if (rc) {
		pr_err("dsi-hbm-range-bl-max-level unspecified\n");
		return rc;
	}
	if (val < range->panel_bri_start) {
		pr_err("Invalid HBM panel brightness range: bl-hbm-max-level < bl-hbm-min-level\n");
		return rc;
	}
	range->panel_bri_end = val;

	rc = dsi_panel_parse_dt_cmd_set(np,
		"google,dsi-hbm-range-entry-command",
		"google,dsi-hbm-range-commands-state", &range->entry_cmd);
	if (rc)
		pr_info("Unable to parse optional dsi-hbm-range-entry-command\n");

	rc = of_property_read_u32(np,
		"google,dsi-hbm-range-num-dimming-frames", &val);
	if (rc) {
		pr_debug("Unable to parse optional hbm-range-entry-num-dimming-frames\n");
		range->num_dimming_frames = 0;
	} else {
		range->num_dimming_frames = val;
	}

	rc = dsi_panel_parse_dt_cmd_set(np,
		"google,dsi-hbm-range-dimming-stop-command",
		"google,dsi-hbm-range-commands-state",
		&range->dimming_stop_cmd);
	if (rc)
		pr_debug("Unable to parse optional dsi-hbm-range-dimming-stop-command\n");

	if ((range->dimming_stop_cmd.count && !range->num_dimming_frames) ||
		(!range->dimming_stop_cmd.count && range->num_dimming_frames)) {
		pr_err("HBM dimming requires both stop command and number of frames.\n");
		return -EINVAL;
	}
	return 0;
}

int dsi_panel_bl_register(struct dsi_panel *panel)
{
	int rc = 0;
	struct dsi_backlight_config *bl = &panel->bl_config;
	const struct of_device_id *match;
	int (*register_func)(struct dsi_backlight_config *) = NULL;

	mutex_init(&bl->state_lock);
	match = of_match_node(dsi_backlight_dt_match, panel->panel_of_node);
	if (match && match->data) {
		register_func = match->data;
		rc = register_func(bl);
	}

	if (!register_func || (rc == -ENOTSUPP)) {
		switch (bl->type) {
		case DSI_BACKLIGHT_WLED:
			break;
		case DSI_BACKLIGHT_DCS:
			bl->update_bl = dsi_backlight_update_dcs;
			break;
		case DSI_BACKLIGHT_PWM:
			register_func = dsi_panel_pwm_bl_register;
			break;
		default:
			pr_err("Backlight type(%d) not supported\n", bl->type);
			rc = -ENOTSUPP;
			break;
		}

		if (register_func)
			rc = register_func(bl);
	}

	if (!rc)
		rc = dsi_backlight_register(bl);

	return rc;
}


int dsi_panel_bl_unregister(struct dsi_panel *panel)
{
	struct dsi_backlight_config *bl = &panel->bl_config;

	mutex_destroy(&bl->state_lock);
	if (bl->unregister)
		bl->unregister(bl);

	if (bl->bl_device)
		sysfs_remove_groups(&bl->bl_device->dev.kobj, bl_device_groups);

	dsi_panel_bl_hbm_free(panel->parent, bl);
	dsi_panel_bl_notifier_free(panel->parent, bl);

	return 0;
}

static int dsi_panel_bl_parse_pwm_config(struct dsi_panel *panel,
				struct dsi_backlight_pwm_config *config)
{
	int rc = 0;
	u32 val;
	struct dsi_parser_utils *utils = &panel->utils;

	rc = utils->read_u32(utils->data, "qcom,bl-pmic-pwm-period-usecs",
				  &val);
	if (rc) {
		pr_err("bl-pmic-pwm-period-usecs is not defined, rc=%d\n", rc);
		goto error;
	}
	config->pwm_period_usecs = val;

error:
	return rc;
}

static void dsi_panel_pwm_bl_unregister(struct dsi_backlight_config *bl)
{
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	struct dsi_backlight_pwm_config *pwm_cfg = bl->priv;

	devm_pwm_put(panel->parent, pwm_cfg->pwm_bl);
	kfree(pwm_cfg);
}

static int dsi_panel_pwm_bl_register(struct dsi_backlight_config *bl)
{
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	struct dsi_backlight_pwm_config *pwm_cfg;
	int rc = 0;

	pwm_cfg = kzalloc(sizeof(*pwm_cfg), GFP_KERNEL);
	if (!pwm_cfg)
		return -ENOMEM;

	pwm_cfg->pwm_bl = devm_of_pwm_get(panel->parent, panel->panel_of_node, NULL);
	if (IS_ERR_OR_NULL(pwm_cfg->pwm_bl)) {
		rc = PTR_ERR(pwm_cfg->pwm_bl);
		pr_err("[%s] failed to request pwm, rc=%d\n", panel->name,
			rc);
		kfree(pwm_cfg);
		return rc;
	}

	rc = dsi_panel_bl_parse_pwm_config(panel, pwm_cfg);
	if (rc) {
		pr_err("[%s] failed to parse pwm config, rc=%d\n",
		       panel->name, rc);
		dsi_panel_pwm_bl_unregister(bl);
		return rc;
	}

	bl->priv = pwm_cfg;
	bl->unregister = dsi_panel_pwm_bl_unregister;

	return 0;
}

static int dsi_panel_bl_parse_lut(struct device *parent,
	struct device_node *of_node, const char *bl_lut_prop_name,
	u32 brightness_max_level, u16 **lut)
{
	u32 len = 0;
	u32 i = 0;
	u32 rc = 0;
	const __be32 *val = 0;
	struct property *prop = NULL;
	u32 lut_length = brightness_max_level + 1;
	u16 *bl_lut_tmp = NULL;

	if (!of_node || !bl_lut_prop_name || !lut)
		return -EINVAL;

	if (*lut) {
		pr_warn("LUT for %s already exists, freeing before reparsing\n",
			bl_lut_prop_name);
		devm_kfree(parent, *lut);
		*lut = NULL;
	}

	prop = of_find_property(of_node, bl_lut_prop_name, &len);
	if (!prop)
		goto done; /* LUT is unspecified */

	len /= sizeof(u32);
	if (len != lut_length) {
		pr_warn("%s length %d doesn't match brightness_max_level + 1 %d\n",
			bl_lut_prop_name, len, lut_length);
		goto done;
	}

	pr_debug("%s length %d\n", bl_lut_prop_name, lut_length);
	bl_lut_tmp = devm_kmalloc(parent, sizeof(u16) * lut_length, GFP_KERNEL);
	if (bl_lut_tmp == NULL) {
		rc = -ENOMEM;
		goto done;
	}

	val = prop->value;
	for (i = 0; i < len; i++)
		bl_lut_tmp[i] = (u16)(be32_to_cpup(val++) & 0xffff);

	*lut = bl_lut_tmp;

done:
	return rc;
}

static void dsi_panel_bl_hbm_free(struct device *dev,
	struct dsi_backlight_config *bl)
{
	u32 i = 0;
	struct hbm_data *hbm = bl->hbm;

	if (!hbm)
		return;

	if (hbm->dimming_workq) {
		dsi_backlight_hbm_dimming_stop(bl);
		flush_workqueue(hbm->dimming_workq);
		destroy_workqueue(hbm->dimming_workq);
	}

	dsi_panel_destroy_cmd_packets(&hbm->exit_cmd);
	dsi_panel_dealloc_cmd_packets(&hbm->exit_cmd);
	dsi_panel_destroy_cmd_packets(&hbm->exit_dimming_stop_cmd);
	dsi_panel_dealloc_cmd_packets(&hbm->exit_dimming_stop_cmd);

	dsi_panel_destroy_cmd_packets(&hbm->irc_unlock_cmd);
	dsi_panel_destroy_cmd_packets(&hbm->irc_lock_cmd);
	kfree(hbm->irc_data);

	for (i = 0; i < hbm->num_ranges; i++) {
		dsi_panel_destroy_cmd_packets(&hbm->ranges[i].entry_cmd);
		dsi_panel_dealloc_cmd_packets(&hbm->ranges[i].entry_cmd);

		dsi_panel_destroy_cmd_packets(&hbm->ranges[i].dimming_stop_cmd);
		dsi_panel_dealloc_cmd_packets(&hbm->ranges[i].dimming_stop_cmd);
	}

	devm_kfree(dev, hbm);
	bl->hbm = NULL;
}

static int dsi_panel_bl_parse_hbm(struct device *parent,
		struct dsi_backlight_config *bl, struct device_node *of_node)
{
	struct device_node *hbm_ranges_np;
	struct device_node *child_np;
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	u32 rc = 0;
	u32 i = 0;
	u32 num_ranges = 0;
	u32 val = 0;
	bool dimming_used = false;

	panel->hbm_mode = HBM_MODE_OFF;

	if (bl->hbm) {
		pr_warn("HBM data already parsed, freeing before reparsing\n");
		dsi_panel_bl_hbm_free(parent, bl);
	}

	hbm_ranges_np = of_get_child_by_name(of_node, "google,hbm-ranges");
	if (!hbm_ranges_np) {
		pr_info("HBM modes list not found\n");
		return 0;
	}

	num_ranges = of_get_child_count(hbm_ranges_np);
	if (!num_ranges || (num_ranges > HBM_RANGE_MAX)) {
		pr_err("Invalid number of HBM modes: %d\n", num_ranges);
		return -EINVAL;
	}

	bl->hbm = devm_kzalloc(parent, sizeof(struct hbm_data), GFP_KERNEL);
	if (bl->hbm == NULL) {
		pr_err("Failed to allocate memory for HBM data\n");
		return -ENOMEM;
	}

	rc = dsi_panel_parse_dt_cmd_set(hbm_ranges_np,
		"google,dsi-hbm-exit-command",
		"google,dsi-hbm-commands-state", &bl->hbm->exit_cmd);
	if (rc)
		pr_info("Unable to parse optional dsi-hbm-exit-command\n");

	bl->hbm->num_ranges = num_ranges;

	rc = of_property_read_u32(hbm_ranges_np,
		"google,dsi-hbm-exit-num-dimming-frames", &val);
	if (rc) {
		pr_debug("Unable to parse optional num-dimming-frames\n");
		bl->hbm->exit_num_dimming_frames = 0;
	} else {
		bl->hbm->exit_num_dimming_frames = val;
	}

	rc = dsi_panel_parse_dt_cmd_set(hbm_ranges_np,
		"google,dsi-hbm-exit-dimming-stop-command",
		"google,dsi-hbm-commands-state",
		&bl->hbm->exit_dimming_stop_cmd);
	if (rc)
		pr_debug("Unable to parse optional dsi-hbm-exit-dimming-stop-command\n");

	if ((bl->hbm->exit_dimming_stop_cmd.count &&
		 !bl->hbm->exit_num_dimming_frames) ||
		(!bl->hbm->exit_dimming_stop_cmd.count &&
		 bl->hbm->exit_num_dimming_frames)) {
		pr_err("HBM dimming requires both stop command and number of frames.\n");
		goto exit_free;
	}

	rc = of_property_read_u32(hbm_ranges_np,
		"google,dsi-irc-addr", &val);
	if (rc) {
		pr_debug("Unable to parse dsi-irc-addr\n");
		bl->hbm->irc_addr = 0;
	} else {
		bl->hbm->irc_addr = val;
		rc = of_property_read_u32(hbm_ranges_np,
			"google,dsi-irc-bit-offset", &val);
		if (rc) {
			bl->hbm->irc_bit_offset = 0;
			bl->hbm->irc_addr = 0;
			pr_warn("Unable to parse dsi-irc-bit-offset\n");
		} else {
			bl->hbm->irc_bit_offset = val;
		}

		rc = dsi_panel_parse_dt_cmd_set(hbm_ranges_np,
			"google,dsi-irc-unlock-command",
			"google,dsi-irc-unlock-commands-state",
			&bl->hbm->irc_unlock_cmd);
		if (rc)
			pr_debug("Unable to parse optional dsi-irc-unlock-command\n");

		rc = dsi_panel_parse_dt_cmd_set(hbm_ranges_np,
			"google,dsi-irc-lock-command",
			"google,dsi-irc-lock-commands-state",
			&bl->hbm->irc_lock_cmd);
		if (rc)
			pr_debug("Unable to parse optional dsi-irc-lock-command\n");

		if (!bl->hbm->irc_unlock_cmd.count != !bl->hbm->irc_lock_cmd.count) {
			dsi_panel_destroy_cmd_packets(&bl->hbm->irc_unlock_cmd);
			dsi_panel_destroy_cmd_packets(&bl->hbm->irc_lock_cmd);
			bl->hbm->irc_addr = 0;
			pr_warn("Unable to get a pair of dsi-irc-unlock/lock command\n");
		}
	}

	for_each_child_of_node(hbm_ranges_np, child_np) {
		rc = dsi_panel_bl_parse_hbm_node(parent, bl,
			child_np, bl->hbm->ranges + i);
		if (rc) {
			pr_err("Failed to parse HBM range %d of %d\n",
				i + 1, num_ranges);
			goto exit_free;
		}
		i++;
	}

	for (i = 0; i < num_ranges - 1; i++) {
		/* Make sure ranges are sorted and not overlapping */
		if (bl->hbm->ranges[i].user_bri_start >=
				bl->hbm->ranges[i + 1].user_bri_start) {
			pr_err("HBM ranges must be sorted by hbm-brightness-threshold\n");
			rc = -EINVAL;
			goto exit_free;
		}

		if (bl->hbm->ranges[i].num_dimming_frames)
			dimming_used = true;

		/* Fill in user_bri_end for each range */
		bl->hbm->ranges[i].user_bri_end =
			bl->hbm->ranges[i + 1].user_bri_start - 1;
	}

	if (bl->hbm->ranges[num_ranges - 1].num_dimming_frames ||
		bl->hbm->exit_num_dimming_frames)
		dimming_used = true;


	if (dimming_used) {
		bl->hbm->dimming_workq =
			create_singlethread_workqueue("dsi_dimming_workq");
		if (!bl->hbm->dimming_workq)
			pr_err("failed to create hbm dimming workq!\n");
		else
			INIT_WORK(&bl->hbm->dimming_work,
				dsi_backlight_hbm_dimming_work);
	}

	bl->hbm->ranges[i].user_bri_end = bl->brightness_max_level;
	bl->hbm->cur_range = HBM_RANGE_MAX;
	bl->hbm->dimming_active = false;
	bl->hbm->dimming_frames_total = 0;
	bl->hbm->dimming_frames_left = 0;
	bl->hbm->panel = panel;

	return 0;

exit_free:
	dsi_panel_bl_hbm_free(parent, bl);
	return rc;
}

static int dsi_panel_bl_find_range(struct dsi_backlight_config *bl,
		int brightness, u32 *range)
{
	u32 i;

	if (!bl || !bl->bl_notifier || !range)
		return -EINVAL;

	for (i = 0; i < bl->bl_notifier->num_ranges; i++) {
		if (brightness <= bl->bl_notifier->ranges[i]) {
			*range = i;
			return 0;
		}
	}

	return -EINVAL;
}

static void dsi_panel_bl_notifier_free(struct device *dev,
	struct dsi_backlight_config *bl)
{
	struct bl_notifier_data *bl_notifier = bl->bl_notifier;

	if (!bl_notifier)
		return;

	devm_kfree(dev, bl_notifier);
	bl->bl_notifier = NULL;
}

static int dsi_panel_bl_parse_ranges(struct device *parent,
		struct dsi_backlight_config *bl, struct device_node *of_node)
{
	int num_ranges = 0;

	bl->bl_notifier = devm_kzalloc(parent, sizeof(struct bl_notifier_data), GFP_KERNEL);
	if (bl->bl_notifier == NULL) {
		pr_err("Failed to allocate memory for  bl_notifier_data\n");
		return -ENOMEM;
	}

	num_ranges = of_property_read_variable_u32_array(of_node,
						"qcom,mdss-dsi-bl-notifier-ranges",
						(u32 *)&bl->bl_notifier->ranges,
						0,
						BL_RANGE_MAX);
	if (num_ranges >= 0) {
		bl->bl_notifier->num_ranges = num_ranges;
	} else {
		dsi_panel_bl_notifier_free(parent, bl);
		pr_debug("Unable to parse optional backlight ranges (%d)\n", num_ranges);
		return num_ranges;
	}

	return 0;
}

int dsi_panel_bl_parse_config(struct device *parent, struct dsi_backlight_config *bl)
{
	struct dsi_panel *panel = container_of(bl, struct dsi_panel, bl_config);
	int rc = 0;
	u32 val = 0;
	const char *bl_type;
	const char *data;
	struct dsi_parser_utils *utils = &panel->utils;
	char *bl_name;

	if (!strcmp(panel->type, "primary"))
		bl_name = "qcom,mdss-dsi-bl-pmic-control-type";
	else
		bl_name = "qcom,mdss-dsi-sec-bl-pmic-control-type";

	bl_type = utils->get_property(utils->data, bl_name, NULL);
	if (!bl_type) {
		bl->type = DSI_BACKLIGHT_UNKNOWN;
	} else if (!strcmp(bl_type, "bl_ctrl_pwm")) {
		bl->type = DSI_BACKLIGHT_PWM;
	} else if (!strcmp(bl_type, "bl_ctrl_wled")) {
		bl->type = DSI_BACKLIGHT_WLED;
	} else if (!strcmp(bl_type, "bl_ctrl_dcs")) {
		bl->type = DSI_BACKLIGHT_DCS;
	} else if (!strcmp(bl_type, "bl_ctrl_external")) {
		bl->type = DSI_BACKLIGHT_EXTERNAL;
	} else {
		pr_debug("[%s] bl-pmic-control-type unknown-%s\n",
			 panel->name, bl_type);
		bl->type = DSI_BACKLIGHT_UNKNOWN;
	}
	data = utils->get_property(utils->data, "qcom,bl-update-flag", NULL);
	if (!data) {
		panel->bl_config.bl_update = BL_UPDATE_NONE;
	} else if (!strcmp(data, "delay_until_first_frame")) {
		panel->bl_config.bl_update = BL_UPDATE_DELAY_UNTIL_FIRST_FRAME;
	} else {
		pr_debug("[%s] No valid bl-update-flag: %s\n",
						panel->name, data);
		panel->bl_config.bl_update = BL_UPDATE_NONE;
	}
	panel->bl_config.bl_scale = MAX_BL_SCALE_LEVEL;
	panel->bl_config.bl_scale_sv = MAX_SV_BL_SCALE_LEVEL;
	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-bl-min-level", &val);
	if (rc) {
		pr_debug("[%s] bl-min-level unspecified, defaulting to zero\n",
			 panel->name);
		bl->bl_min_level = 0;
	} else {
		bl->bl_min_level = val;
	}

	rc = utils->read_u32(utils->data, "qcom,mdss-dsi-bl-max-level", &val);
	if (rc) {
		pr_debug("[%s] bl-max-level unspecified, defaulting to max level\n",
			 panel->name);
		bl->bl_max_level = MAX_BL_LEVEL;
	} else {
		bl->bl_max_level = val;
	}

	rc = utils->read_u32(utils->data, "qcom,mdss-brightness-max-level",
		&val);
	if (rc) {
		pr_debug("[%s] brigheness-max-level unspecified, defaulting to 255\n",
			 panel->name);
		bl->brightness_max_level = 255;
	} else {
		bl->brightness_max_level = val;
	}

	rc = dsi_panel_bl_parse_lut(parent, utils->data, "qcom,mdss-dsi-bl-lut",
		bl->brightness_max_level, &bl->lut);
	if (rc) {
		pr_err("[%s] failed to create backlight LUT, rc=%d\n",
			panel->name, rc);
		goto error;
	}
	pr_debug("[%s] bl-lut %sused\n", panel->name, bl->lut ? "" : "un");

	bl->dimming_mode = utils->read_bool(utils->data,
				"google,dsi-panel-dimming-enable");

	rc = dsi_panel_bl_parse_hbm(parent, bl, utils->data);
	if (rc)
		pr_err("[%s] error while parsing high brightness mode (hbm) details, rc=%d\n",
			panel->name, rc);

	rc = dsi_panel_bl_parse_ranges(parent, bl, utils->data);
	if (rc)
		pr_debug("[%s] error while parsing backlight ranges, rc=%d\n",
			panel->name, rc);

	rc = utils->read_u32(utils->data, "google,dsi-bl-cmd-high-byte-offset",
		&val);
	if (rc) {
		pr_debug("[%s] dsi-bl-cmd-high-byte-offset unspecified, defaulting to 8\n",
			 panel->name);
		bl->high_byte_offset = 8;
	} else {
		bl->high_byte_offset = val;
	}

	bl->en_gpio = utils->get_named_gpio(utils->data,
					      "qcom,platform-bklight-en-gpio",
					      0);
	if (!gpio_is_valid(bl->en_gpio)) {
		if (bl->en_gpio == -EPROBE_DEFER) {
			pr_debug("[%s] failed to get bklt gpio, rc=%d\n",
					panel->name, rc);
			rc = -EPROBE_DEFER;
			goto error;
		} else {
			pr_debug("[%s] failed to get bklt gpio, rc=%d\n",
					 panel->name, rc);
			rc = 0;
			goto error;
		}
	}

error:
	return rc;
}

static int dsi_panel_bl_read_brightness(struct dsi_panel *panel,
		struct dsi_backlight_config *bl_cfg, int *lvl)
{
	u32 rc;
	u8 buf[BL_BRIGHTNESS_BUF_SIZE];

	rc = mipi_dsi_dcs_read(&panel->mipi_device,
		MIPI_DCS_GET_DISPLAY_BRIGHTNESS, buf, BL_BRIGHTNESS_BUF_SIZE);

	if (rc <= 0 || rc > BL_BRIGHTNESS_BUF_SIZE) {
		pr_err("mipi_dsi_dcs_read error: %d\n", rc);
		return -EIO;
	}

	if (rc == 1)
		*lvl = buf[0];
	else if (rc == 2)
		*lvl = be16_to_cpu(*(const __be16 *)buf);
	else {
		pr_err("unexpected buffer size: %d\n", rc);
		return -EIO;
	}

	/* Some panels may not clear non-functional bits. */
	*lvl &= (1 << fls(bl_cfg->bl_max_level)) - 1;

	return 0;
}
int dsi_panel_bl_brightness_handoff(struct dsi_panel *panel)
{
	struct dsi_backlight_config *bl_cfg;
	struct backlight_device *bl_device;
	int bl_lvl = 0, brightness, rc;

	if (!panel || !panel->bl_config.bl_device)
		return -EINVAL;

	bl_cfg = &panel->bl_config;
	bl_device = bl_cfg->bl_device;

	rc = dsi_panel_bl_read_brightness(panel, bl_cfg, &bl_lvl);
	if (rc) {
		pr_err("Failed to read brightness from panel.\n");
		return rc;
	}

	rc = dsi_backlight_lerp(bl_cfg->bl_min_level, bl_cfg->bl_max_level, 1,
		bl_cfg->brightness_max_level, bl_lvl, &brightness);
	if (rc) {
		pr_err("Failed to map brightness to user space.\n");
		return rc;
	}

	pr_debug("brightness 0x%x to user space %d\n", bl_lvl, brightness);
	bl_device->props.brightness = brightness;

	return rc;
}

int dsi_panel_bl_update_irc(struct dsi_backlight_config *bl, bool enable)
{
	struct hbm_data *hbm = bl->hbm;
	int rc = 0;
	u32 byte_offset;
	u32 bit_mask;
	u32 irc_data_size;

	if (!hbm || hbm->irc_addr == 0)
		return -EOPNOTSUPP;

	byte_offset = hbm->irc_bit_offset / BITS_PER_BYTE;
	bit_mask = BIT(hbm->irc_bit_offset % BITS_PER_BYTE);
	irc_data_size = byte_offset + 1;

	pr_info("irc update: %d\n", enable);
	dsi_panel_cmd_set_transfer(hbm->panel, &hbm->irc_unlock_cmd);
	if (hbm->irc_data == NULL) {
		hbm->irc_data = kzalloc(irc_data_size, GFP_KERNEL);
		if (hbm->irc_data == NULL) {
			pr_err("failed to alloc irc_data.\n");
			goto done;
		}

		rc = mipi_dsi_dcs_read(&hbm->panel->mipi_device, hbm->irc_addr,
				hbm->irc_data, irc_data_size);
		if (rc != irc_data_size) {
			pr_err("failed to read irc.\n");
			goto done;
		}
		pr_info("Read back irc initial configuration\n");
	}

	if (enable)
		hbm->irc_data[byte_offset] |= bit_mask;
	else
		hbm->irc_data[byte_offset] &= ~bit_mask;

	rc = mipi_dsi_dcs_write(&hbm->panel->mipi_device,
			hbm->irc_addr, hbm->irc_data, irc_data_size);

	if (rc)
		pr_err("failed to send irc cmd.\n");
done:
	dsi_panel_cmd_set_transfer(hbm->panel, &hbm->irc_lock_cmd);
	return rc;
}
