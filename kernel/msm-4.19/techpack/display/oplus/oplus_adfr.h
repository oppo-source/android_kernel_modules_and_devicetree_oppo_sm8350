/***************************************************************
** Copyright (C),  2020,  OPPO Mobile Comm Corp.,  Ltd
** VENDOR_EDIT
** File : oplus_adfr.h
** Description : ADFR kernel module
** Version : 1.0
** Date : 2020/10/23
******************************************************************/

#ifndef _OPLUS_ADFR_H_
#define _OPLUS_ADFR_H_

// please just only include linux common head file to keep me pure
#include <linux/device.h>
#include <linux/hrtimer.h>


enum oplus_vsync_mode {
	OPLUS_SOFT_VSYNC = 0,
	OPLUS_HARD_VSYNC,
};

/* --------------- adfr misc ---------------*/
void oplus_adfr_init(void *dsi_panel);
inline bool oplus_adfr_is_support(void);
ssize_t oplus_adfr_get_debug(struct device *dev,
	struct device_attribute *attr, char *buf);
ssize_t oplus_adfr_set_debug(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count);
ssize_t oplus_set_vsync_switch(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count);
ssize_t oplus_get_vsync_switch(struct device *dev,
		struct device_attribute *attr, char *buf);

/* --------------- msm_drv ---------------*/
int oplus_adfr_thread_create(void *msm_param,
	void *msm_priv, void *msm_ddev, void *msm_dev);
void oplus_adfr_thread_destroy(void *msm_priv);

/* --------------- sde_crtc ---------------*/
void sde_crtc_adfr_handle_frame_event(void *crtc, void* event);

/* --------------- sde_encoder ---------------*/
// if frame start commit, cancel the second fake frame
int sde_encoder_adfr_cancel_fakeframe(void *enc);
// timer handler for second fake frame
enum hrtimer_restart sde_encoder_fakeframe_timer_handler(struct hrtimer *timer);
// the fake frame cmd send function
void sde_encoder_fakeframe_work_handler(struct kthread_work *work);
void oplus_adfr_fakeframe_timer_start(void *enc, int deferred_ms);
int sde_encoder_adfr_trigger_fakeframe(void *enc);
// trigger first fake frame
void sde_encoder_adfr_prepare_commit(void *crtc, void *enc, void *conn);
// trigger second fake frame
void sde_encoder_adfr_kickoff(void *crtc, void *enc, void *conn);

/* --------------- sde_encoder_phys_cmd ---------------*/
int oplus_adfr_adjust_tearcheck_for_dynamic_qsync(void *sde_phys_enc);

/* --------------- dsi_connector ---------------*/
int sde_connector_send_fakeframe(void *conn);

/* --------------- dsi_display ---------------*/
int dsi_display_qsync_update_min_fps(void *dsi_display, void *dsi_params);
int dsi_display_qsync_restore(void *dsi_display);
/*
 * dsi_display_send_fakeframe - send 2C/3C dcs to Panel
 * @display: Pointer to private display structure
 * Returns: Zero on success
 */
int dsi_display_send_fakeframe(void *disp);
void dsi_display_adfr_change_te_irq_status(void *display, bool enable);

/* --------------- dsi_panel ---------------*/
int dsi_panel_parse_adfr(void *dsi_mode, void *dsi_utils);
int dsi_panel_send_qsync_min_fps_dcs(void *dsi_panel,
				int ctrl_idx, uint32_t min_fps);
int dsi_panel_send_fakeframe_dcs(void *dsi_panel,
				int ctrl_idx);


/* --------------- vsync switch ---------------*/
void oplus_dsi_display_vsync_switch(void *display);

int oplus_dsi_panel_parse_panel_vsync_source(void *mode,
				void *utils);

#endif /* _OPLUS_ADFR_H_ */
