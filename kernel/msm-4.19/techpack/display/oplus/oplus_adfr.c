/***************************************************************
** Copyright (C) 2018-2020 OPLUS. All rights reserved.
** VENDOR_EDIT
** File : oplus_adfr.h
** Description : ADFR kernel module
** Version : 1.0
** Date : 2020/10/23
******************************************************************/
#include "sde_trace.h"
#include "msm_drv.h"
#include "sde_kms.h"
#include "sde_connector.h"
#include "sde_crtc.h"
#include "sde_encoder_phys.h"

#include "dsi_display.h"
#include "dsi_panel.h"
#include "dsi_parser.h"
#include "dsi_drm.h"
#include "dsi_defs.h"

#include "oplus_adfr.h"

#define OPLUS_ADFR_CONFIG_GLOBAL (1<<0)
#define OPLUS_ADFR_CONFIG_FAKEFRAME (1<<1)
#define OPLUS_ADFR_CONFIG_VSYNC_SWITCH (1<<2)
#define OPLUS_ADFR_CONFIG_VSYNC_SWITCH_MODE (1<<3)

#define OPLUS_ADFR_DEBUG_FAKEFRAME_DISABLE (1<<0)
#define OPLUS_ADFR_DEBUG_VSYNC_SWITCH_DISABLE (1<<1)

#define ADFR_GET_GLOBAL_CONFIG(config) ((config) & OPLUS_ADFR_CONFIG_GLOBAL)
#define ADFR_GET_FAKEFRAME_CONFIG(config) ((config) & OPLUS_ADFR_CONFIG_FAKEFRAME)
#define ADFR_GET_VSYNC_SWITCH_CONFIG(config) ((config) & OPLUS_ADFR_CONFIG_VSYNC_SWITCH)
#define ADFR_GET_VSYNC_SWITCH_MODE(config) ((config) & OPLUS_ADFR_CONFIG_VSYNC_SWITCH_MODE)

static u32 oplus_adfr_config = 0;
static u32 oplus_adfr_debug = 0;
static bool need_deferred_fakeframe = false;

/* --------------- adfr misc ---------------*/

void oplus_adfr_init(void *panel_node)
{
	static bool inited = false;
	u32 config = 0;
	int rc = 0;
	struct device_node *of_node = panel_node;

	pr_info("oplus_adfr_init now.");

	if (!of_node) {
		pr_err("oplus_adfr_init: the param is null.");
		return;
	}

	if (inited) {
		pr_warning("adfr config = %#X already!", oplus_adfr_config);
		return;
	}

	rc = of_property_read_u32(of_node, "oplus,adfr-config", &config);
	if (rc == 0) {
		oplus_adfr_config = config;
	} else {
		oplus_adfr_config = 0;
	}

	inited = true;

	pr_info("adfr config = %#X.", oplus_adfr_config);
}

ssize_t oplus_adfr_get_debug(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	pr_err("get adfr config %#X debug %#X \n", oplus_adfr_config, oplus_adfr_debug);
	return oplus_adfr_debug;
}

ssize_t oplus_adfr_set_debug(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	sscanf(buf, "%u", &oplus_adfr_debug);
	pr_err("get adfr config %#X debug %#X \n", oplus_adfr_config, oplus_adfr_debug);

	return count;
}

static inline bool oplus_adfr_fakeframe_is_enable(void)
{
	return (bool)( ADFR_GET_FAKEFRAME_CONFIG(oplus_adfr_config) &&
		!(oplus_adfr_debug & OPLUS_ADFR_DEBUG_FAKEFRAME_DISABLE) );
}

static inline bool oplus_adfr_vsync_switch_is_enable(void)
{
	return (bool)( ADFR_GET_VSYNC_SWITCH_CONFIG(oplus_adfr_config) &&
		!(oplus_adfr_debug & OPLUS_ADFR_DEBUG_VSYNC_SWITCH_DISABLE) );
}

/*mode 0: soft switch 1:hard switch*/
static inline enum oplus_vsync_mode oplus_adfr_vsync_mode(void)
{
	return (enum oplus_vsync_mode)( ADFR_GET_VSYNC_SWITCH_MODE(oplus_adfr_config) &&
		!(oplus_adfr_debug & OPLUS_ADFR_DEBUG_VSYNC_SWITCH_DISABLE) );
}

inline bool oplus_adfr_is_support(void)
{
	return ADFR_GET_GLOBAL_CONFIG(oplus_adfr_config);
}


/* --------------- msm_drv ---------------*/

int oplus_adfr_thread_create(void *msm_param_ptr,
	void *msm_priv, void *msm_ddev, void *msm_dev)
{
	struct sched_param *param;
	struct msm_drm_private *priv;
	struct drm_device *ddev;
	struct device *dev;
	int i, ret = 0;

	param = msm_param_ptr;
	priv = msm_priv;
	ddev = msm_ddev;
	dev = msm_dev;

	for (i = 0; i < priv->num_crtcs; i++) {

		/* initialize adfr thread */
		priv->adfr_thread[i].crtc_id = priv->crtcs[i]->base.id;
		kthread_init_worker(&priv->adfr_thread[i].worker);
		priv->adfr_thread[i].dev = ddev;
		priv->adfr_thread[i].thread =
			kthread_run(kthread_worker_fn,
				&priv->adfr_thread[i].worker,
				"adfr:%d", priv->adfr_thread[i].crtc_id);
		ret = sched_setscheduler(priv->adfr_thread[i].thread,
							SCHED_FIFO, param);
		if (ret)
			pr_warn("adfr thread priority update failed: %d\n",
									ret);

		if (IS_ERR(priv->adfr_thread[i].thread)) {
			dev_err(dev, "failed to create adfr_commit kthread\n");
			priv->adfr_thread[i].thread = NULL;
		}

		if ((!priv->adfr_thread[i].thread)) {
			/* clean up previously created threads if any */
			for ( ; i >= 0; i--) {
				if (priv->adfr_thread[i].thread) {
					kthread_stop(
						priv->adfr_thread[i].thread);
					priv->adfr_thread[i].thread = NULL;
				}
			}
			return -EINVAL;
		}
	}

	return 0;
}

void oplus_adfr_thread_destroy(void *msm_priv)
{
	struct msm_drm_private *priv;
	int i;

	priv = msm_priv;

	for (i = 0; i < priv->num_crtcs; i++) {
		if (priv->adfr_thread[i].thread) {
			kthread_flush_worker(&priv->adfr_thread[i].worker);
			kthread_stop(priv->adfr_thread[i].thread);
			priv->adfr_thread[i].thread = NULL;
		}
	}
}

/* --------------- sde_crtc ---------------*/

void sde_crtc_adfr_handle_frame_event(void *crt, void* event)
{
	struct drm_crtc *crtc = crt;
	struct sde_crtc *sde_crtc = to_sde_crtc(crtc);
	struct sde_crtc_frame_event *fevent = event;
	struct drm_encoder *encoder;

	// cancel deferred adfr fakeframe timer
	if (oplus_adfr_fakeframe_is_enable() &&
		(fevent->event & SDE_ENCODER_FRAME_EVENT_SIGNAL_RETIRE_FENCE)) {
		mutex_lock(&sde_crtc->crtc_lock);
		list_for_each_entry(encoder, &crtc->dev->mode_config.encoder_list, head) {
			if (encoder->crtc != crtc)
				continue;

			sde_encoder_adfr_cancel_fakeframe(encoder);
		}
		mutex_unlock(&sde_crtc->crtc_lock);
	}
}


/* --------------- sde_encoder ---------------*/

static inline struct dsi_display_mode_priv_info *oplus_get_current_mode_priv_info(struct drm_connector * drm_conn)
{
	struct msm_drm_private *priv;
	struct sde_kms *sde_kms;
	struct dsi_display *dsi_display;
	struct dsi_panel *panel;

	if (!drm_conn) {
		SDE_ERROR("adfr drm_conn is null.\n");
		return NULL;
	}

	priv = drm_conn->dev->dev_private;
	sde_kms = to_sde_kms(priv->kms);

	if (!sde_kms) {
		SDE_ERROR("adfr sde_kms is null.\n");
		return NULL;
	}

	if (sde_kms->dsi_display_count && sde_kms->dsi_displays) {
		// only use primary dsi
		dsi_display = sde_kms->dsi_displays[0];
	} else {
		SDE_ERROR("adfr sde_kms's dsi_display is null.\n");
		return NULL;
	}

	panel = dsi_display->panel;

	if (!panel || !panel->cur_mode) {
		SDE_ERROR("adfr dsi_display's panel is null.\n");
		return NULL;
	}

	return panel->cur_mode->priv_info;
}

void sde_encoder_adfr_prepare_commit(void *crt, void *enc, void *conn) {
	struct dsi_display_mode_priv_info *priv_info;
	struct drm_crtc *crtc = crt;
	struct drm_connector *drm_conn = conn;

	if (!oplus_adfr_fakeframe_is_enable()) {
		return;
	}

	// when power on, disable deferred fakeframe
	// after power on and before first frame flush
	// if panel get a fakeframe then refresh itself (with a dirty buffer), tearing happen
	// so for power on case, set need_deferred_fakeframe false
	// this can avoid deferred fakeframe tearing issue (eg. AOD)
	// power off --> sde_encoder_virt_disable set "sde_enc->cur_master = NULL"
	// power on  --> sde_encoder_virt_enable  set "sde_enc->cur_master = XXX"
	// prepare_commit need cur_master is not null but it is before than sde_encoder_virt_enable
	// so use prepare_commit(NULL, NULL, NULL) to imply this commit is first commit after power on
	if (!crt && !enc && !conn) {
		need_deferred_fakeframe = false;
		// SDE_ATRACE_INT("need_deferred_fakeframe", need_deferred_fakeframe);
		return;
	}
	// after power on, enable deferred fakeframe
	need_deferred_fakeframe = true;

	if (!crt || !enc || !conn) {
		SDE_ERROR("sde_encoder_adfr_prepare_commit error: %p %p %p",
			crt, enc, conn);
		return;
	}

	priv_info = oplus_get_current_mode_priv_info(drm_conn);

	// check 1st bit
	if (!priv_info || !(priv_info->fakeframe_config & 0X00000001)) {
		return;
	}

	// before commit send a fakeframe to triger the panel flush
	// but if pre-frame is pending, ignore this time
	// because pre-frame is a real frame, Not Need fakeframe
	// SDE_ATRACE_INT("frame_pending", sde_crtc_frame_pending(sde_enc->crtc));
	if ((sde_crtc_frame_pending(crtc) == 0)) {
		sde_encoder_adfr_trigger_fakeframe(enc);
	}
}

void sde_encoder_adfr_kickoff(void *crt, void *enc, void *conn) {
	struct dsi_display_mode_priv_info *priv_info;
	struct drm_connector *drm_conn = conn;
	int deferred_ms = -1;

	if (!oplus_adfr_fakeframe_is_enable()) {
		return;
	}

	// SDE_ATRACE_INT("need_deferred_fakeframe", need_deferred_fakeframe);
	if (!need_deferred_fakeframe) {
		SDE_ERROR("sde_encoder_adfr_kickoff skip, need_deferred_fakeframe is false.");
		return;
	}

	if (!crt || !enc || !conn) {
		SDE_ERROR("sde_encoder_adfr_kickoff error:  %p %p %p",
			crt, enc, conn);
		return;
	}

	priv_info = oplus_get_current_mode_priv_info(drm_conn);

	// check 2st bit
	if (!priv_info || !(priv_info->fakeframe_config & 0X00000002)) {
		return;
	}
	deferred_ms = priv_info->deferred_fakeframe_time;

	oplus_adfr_fakeframe_timer_start(enc, deferred_ms);
}

/* --------------- sde_encoder_phys_cmd ---------------*/

int oplus_adfr_adjust_tearcheck_for_dynamic_qsync(void *sde_phys_enc)
{
	struct sde_encoder_phys *phys_enc = sde_phys_enc;
	struct sde_hw_tear_check tc_cfg = {0};
	struct sde_connector *sde_conn = NULL;
	int ret = 0;

	if (sde_connector_get_qsync_mode(phys_enc->connector) == 0 ||
		sde_connector_get_qsync_dynamic_min_fps(phys_enc->connector) == 0) {
		return ret;
	}

	SDE_ATRACE_BEGIN("adjust_tearcheck_for_qsync");
	SDE_ATRACE_INT("frame_state", atomic_read(&phys_enc->frame_state));

	// this time maybe remain in qsync window, so shrink qsync window
	// to avoid tearing and keep qsync enable for this frame
	if (atomic_read(&phys_enc->frame_state) != 0) {
		// 300 is a estimated value
		tc_cfg.sync_threshold_start = 300;
	} else {
		// remain use original qsync window
		tc_cfg.sync_threshold_start = phys_enc->qsync_sync_threshold_start;
	}

	if(phys_enc->current_sync_threshold_start != tc_cfg.sync_threshold_start) {
		SDE_ATRACE_BEGIN("update_qsync");

		if (phys_enc->has_intf_te &&
			phys_enc->hw_intf->ops.update_tearcheck)
			phys_enc->hw_intf->ops.update_tearcheck(
				phys_enc->hw_intf, &tc_cfg);
		else if (phys_enc->hw_pp->ops.update_tearcheck)
			phys_enc->hw_pp->ops.update_tearcheck(
				phys_enc->hw_pp, &tc_cfg);
		SDE_EVT32(DRMID(phys_enc->parent), tc_cfg.sync_threshold_start);

		phys_enc->current_sync_threshold_start = tc_cfg.sync_threshold_start;
		// trigger AP update qsync flush
		sde_conn = to_sde_connector(phys_enc->connector);
		sde_conn->qsync_updated = true;

		SDE_ATRACE_END("update_qsync");
	}

	SDE_ATRACE_INT("threshold_lines", phys_enc->current_sync_threshold_start);
	SDE_ATRACE_END("adjust_tearcheck_for_qsync");

	return ret;

}

/* --------------- dsi_connector ---------------*/

int sde_connector_send_fakeframe(void *conn)
{
	struct drm_connector *connector = conn;
	struct sde_connector *c_conn;
	int rc;

	if (!connector) {
		SDE_ERROR("invalid argument\n");
		return -EINVAL;
	}

	c_conn = to_sde_connector(connector);
	if (!c_conn->display) {
		SDE_ERROR("invalid connector display\n");
		return -EINVAL;
	}

	rc = dsi_display_send_fakeframe(c_conn->display);

	SDE_EVT32(connector->base.id, rc);
	return rc;
}

/* --------------- dsi_display ---------------*/

// update qsync min fps
int dsi_display_qsync_update_min_fps(void *dsi_display, void *dsi_params)
{
	struct dsi_display *display = dsi_display;
	struct msm_display_conn_params *params = dsi_params;
	int i;
	int rc = 0;

	if (!params->qsync_update) {
		return 0;
	}

	/* allow qsync off but update qsync min fps only */
	SDE_ATRACE_INT("qsync_mode", params->qsync_mode);
	SDE_ATRACE_INT("qsync_minfps", params->qsync_dynamic_min_fps);

	SDE_ATRACE_BEGIN("dsi_display_qsync_update_min_fps");

	mutex_lock(&display->display_lock);

	display_for_each_ctrl(i, display) {

		/* send the commands to updaet qsync min fps */
		rc = dsi_panel_send_qsync_min_fps_dcs(display->panel, i, params->qsync_dynamic_min_fps);
		if (rc) {
			DSI_ERR("fail qsync UPDATE cmds rc:%d\n", rc);
			goto exit;
		}
	}

exit:
	SDE_EVT32(params->qsync_mode, params->qsync_dynamic_min_fps, rc);
	mutex_unlock(&display->display_lock);

	SDE_ATRACE_END("dsi_display_qsync_update_min_fps");

	return rc;
}

/* save qsync info, then restore qsync status after panel enable*/
int dsi_display_qsync_restore(void *dsi_display)
{
	struct msm_display_conn_params params;
	struct dsi_display *display = dsi_display;
	int rc = 0;

	if (display->need_qsync_restore) {
		display->need_qsync_restore = false;
	} else {
		return 0;
	}

	params.qsync_update = display->current_qsync_mode ||
						  display->current_qsync_dynamic_min_fps;

	if (!params.qsync_update) {
		DSI_INFO("%s:INFO: qsync status is clean.\n", __func__);
		return 0;
	}

	params.qsync_mode = display->current_qsync_mode;
	params.qsync_dynamic_min_fps = display->current_qsync_dynamic_min_fps;

	SDE_ATRACE_BEGIN("dsi_display_qsync_restore");

	DSI_INFO("qsync restore mode %d minfps %d \n",
	         params.qsync_mode, params.qsync_dynamic_min_fps);
	rc = dsi_display_pre_commit(display, &params);
	SDE_EVT32(params.qsync_mode, params.qsync_dynamic_min_fps, rc);

	SDE_ATRACE_END("dsi_display_qsync_restore");

	return rc;
}

int dsi_display_send_fakeframe(void *disp)
{
	struct dsi_display *display = (struct dsi_display *)disp;
	int i, rc = 0;

	if (!display) {
		pr_err("Invalid params\n");
		return -EINVAL;
	}

	SDE_ATRACE_BEGIN("dsi_display_send_fakeframe");
	display_for_each_ctrl(i, display) {
		/* send the commands to simulate a frame transmission */
		rc = dsi_panel_send_fakeframe_dcs(display->panel, i);
		if (rc) {
			DSI_ERR("fail fake frame cmds rc:%d\n", rc);
			goto exit;
		}
	}

exit:
	SDE_ATRACE_END("dsi_display_send_fakeframe");
	SDE_EVT32(rc);

	return rc;
}

/* --------------- dsi_panel ---------------*/

const char *qsync_min_fps_set_map[DSI_CMD_QSYNC_MIN_FPS_COUNTS] = {
	"qcom,mdss-dsi-qsync-min-fps-0",
	"qcom,mdss-dsi-qsync-min-fps-1",
	"qcom,mdss-dsi-qsync-min-fps-2",
	"qcom,mdss-dsi-qsync-min-fps-3",
	"qcom,mdss-dsi-qsync-min-fps-4",
	"qcom,mdss-dsi-qsync-min-fps-5",
	"qcom,mdss-dsi-qsync-min-fps-6",
	"qcom,mdss-dsi-qsync-min-fps-7",
	"qcom,mdss-dsi-qsync-min-fps-8",
	"qcom,mdss-dsi-qsync-min-fps-9",
};

int dsi_panel_send_qsync_min_fps_dcs(void *dsi_panel,
		int ctrl_idx, uint32_t min_fps)
{
	struct dsi_panel *panel = dsi_panel;
	struct dsi_display_mode_priv_info *priv_info;
	int rc = 0;
	int i = 0;

	if (!panel || !panel->cur_mode) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	priv_info = panel->cur_mode->priv_info;

	mutex_lock(&panel->panel_lock);

	//select a best fps to fit min_fps
	for(i=priv_info->qsync_min_fps_sets_size-1; i>=0; i--) {
		if(priv_info->qsync_min_fps_sets[i] <= min_fps) {
			DSI_DEBUG("ctrl:%d qsync find min fps %d\n", priv_info->qsync_min_fps_sets[i]);
			break;
		}
	}

	if(i >= 0 && i < priv_info->qsync_min_fps_sets_size) {
		DSI_DEBUG("ctrl:%d qsync update min fps %d use \n", ctrl_idx, min_fps);
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_QSYNC_MIN_FPS_0+i);
		if (rc)
			DSI_ERR("[%s] failed to send DSI_CMD_QSYNC_MIN_FPS cmds rc=%d\n",
				panel->name, rc);
	} else {
		DSI_ERR("ctrl:%d failed to sets qsync min fps %u, %d\n", ctrl_idx, min_fps, i);
	}
	mutex_unlock(&panel->panel_lock);
	return rc;
}

int dsi_panel_send_fakeframe_dcs(void *dsi_panel,
		int ctrl_idx)
{
	struct dsi_panel *panel = dsi_panel;
	int rc = 0;

	if (!panel) {
		DSI_ERR("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	DSI_DEBUG("ctrl:%d fake frame\n", ctrl_idx);
	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_FAKEFRAME);
	if (rc)
		DSI_ERR("[%s] failed to send DSI_CMD_FAKEFRAME cmds rc=%d\n",
		       panel->name, rc);

	mutex_unlock(&panel->panel_lock);
	return rc;
}


static int dsi_panel_parse_qsync_min_fps(
		struct dsi_display_mode_priv_info *priv_info,
		struct dsi_parser_utils *utils)
{
	int rc = 0;
	u32 i;

	if (!priv_info) {
		DSI_ERR("dsi_panel_parse_qsync_min_fps err: invalid mode priv info\n");
		return -EINVAL;
	}

	priv_info->qsync_min_fps_sets_size = 0;

	for (i = 0; i < DSI_CMD_QSYNC_MIN_FPS_COUNTS; i++) {
		rc = utils->read_u32(utils->data, qsync_min_fps_set_map[i],
			&priv_info->qsync_min_fps_sets[i]);
		if (rc) {
			DSI_ERR("failed to parse qsync min fps set %u\n", i);
			break;
		}
		else {
			priv_info->qsync_min_fps_sets_size++;
			DSI_INFO("parse qsync min fps set %u = %u\n",
			priv_info->qsync_min_fps_sets_size, priv_info->qsync_min_fps_sets[i]);
		}
	}

	return rc;
}

static int dsi_panel_parse_fakeframe(
		struct dsi_display_mode_priv_info *priv_info,
		struct dsi_parser_utils *utils)
{
	int rc = 0;

	if (!priv_info) {
		DSI_ERR("dsi_panel_parse_fakeframe err: invalid mode priv info\n");
		return -EINVAL;
	}

	priv_info->fakeframe_config = 0;
	priv_info->deferred_fakeframe_time = 0;

	rc = utils->read_u32(utils->data, "oplus,adfr-fakeframe-config",
			&priv_info->fakeframe_config);
	if (rc) {
		DSI_ERR("failed to parse fakeframe.\n");
	}

	rc = utils->read_u32(utils->data, "oplus,adfr-fakeframe-deferred-time",
			&priv_info->deferred_fakeframe_time);
	if (rc) {
		DSI_ERR("failed to parse deferred_fakeframe_time.\n");
	}

	DSI_INFO("adfr fakeframe_config: %u, deferred_fakeframe_time: %u \n",
		priv_info->fakeframe_config, priv_info->deferred_fakeframe_time);

	return rc;
}

int dsi_panel_parse_adfr(void *dsi_mode, void *dsi_utils)
{
	struct dsi_display_mode *mode = dsi_mode;
	struct dsi_parser_utils *utils = dsi_utils;
	struct dsi_display_mode_priv_info *priv_info = mode->priv_info;

	if (dsi_panel_parse_qsync_min_fps(priv_info, utils)) {
		DSI_ERR("adfr failed to parse qsyn min fps\n");
	}
	if (dsi_panel_parse_fakeframe(priv_info, utils)) {
		DSI_ERR("adfr failed to parse fakeframe\n");
	}

	return 0;
}

/* --------------- vsync switch ---------------*/

static int oplus_dsi_display_enable_and_waiting_for_next_te_irq(struct dsi_display *display)
{
	int const switch_te_timeout = msecs_to_jiffies(1100);

	dsi_display_adfr_change_te_irq_status(display, true);
	DSI_INFO("Waiting for the next TE to switch\n");

	display->panel->vsync_switch_pending = true;
	reinit_completion(&display->switch_te_gate);

	if (!wait_for_completion_timeout(&display->switch_te_gate, switch_te_timeout)) {
		DSI_ERR("vsync switch TE check failed\n");
		dsi_display_adfr_change_te_irq_status(display, false);
		return -EINVAL;
	}

	return 0;
}

/*GPIO SWITCH: 0-TP Vsync    1-TE Vsync*/
static int oplus_dsi_display_vsync_switch_check_te(struct dsi_display *display, int level)
{
	int rc = 0;

	if (level == display->panel->vsync_switch_gpio_level) {
		DSI_INFO("vsync_switch_gpio is already %d\n", level);
		return 0;
	}

	if (!gpio_is_valid(display->panel->vsync_switch_gpio)) {
		DSI_ERR("vsync_switch_gpio is invalid\n");
		return -EINVAL;
	}

	oplus_dsi_display_enable_and_waiting_for_next_te_irq(display);
	if (level) {
		rc = gpio_direction_output(display->panel->vsync_switch_gpio, 1);//TE Vsync
		if (rc) {
			DSI_ERR("unable to set dir for vsync_switch_gpio, rc=%d\n", rc);
			return rc;
		} else {
			DSI_INFO("set vsync_switch_gpio to 1\n");
		}
	} else {
		gpio_set_value(display->panel->vsync_switch_gpio, 0);//TP Vsync
		DSI_INFO("set vsync_switch_gpio to 0\n");
	}

	dsi_display_adfr_change_te_irq_status(display, false);

	display->panel->vsync_switch_gpio_level = level;

	return rc;
}

static int oplus_dsi_display_set_vsync_switch_gpio(struct dsi_display *display, int level)
{
	struct dsi_panel *panel = NULL;
	int rc = 0;


	if ((display == NULL) || (display->panel == NULL))
		return -EINVAL;

	panel = display->panel;

	mutex_lock(&display->display_lock);

	if (!panel->panel_initialized) {
		if (gpio_is_valid(panel->vsync_switch_gpio)) {
			if (level) {
				rc = gpio_direction_output(panel->vsync_switch_gpio, 1);//TE Vsync
				DSI_INFO("set vsync_switch_gpio to 1\n");
				if (rc) {
					DSI_ERR("unable to set dir for vsync_switch_gpio gpio rc=%d\n", rc);
				}
			} else {
				gpio_set_value(panel->vsync_switch_gpio, 0);//TP Vsync
				DSI_INFO("set vsync_switch_gpio to 0\n");
			}
			panel->vsync_switch_gpio_level = level;
		}
	} else {
		oplus_dsi_display_vsync_switch_check_te(display, level);
	}

	mutex_unlock(&display->display_lock);
	return rc;
}

static int oplus_dsi_display_get_vsync_switch_gpio(struct dsi_display *display)
{
	if ((display == NULL) || (display->panel == NULL))
		return -EINVAL;

	return display->panel->vsync_switch_gpio_level;
}


/*GPIO SWITCH: 0-TP Vsync    1-TE Vsync*/
ssize_t oplus_set_vsync_switch(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct dsi_display *display = get_main_display();
	int ret = 0;
	int vsync_switch_gpio = 0;

	sscanf(buf, "%du", &vsync_switch_gpio);

	printk(KERN_INFO "%s oplus_set_vsync_switch = %d\n", __func__, vsync_switch_gpio);

	ret = oplus_dsi_display_set_vsync_switch_gpio(display, vsync_switch_gpio);
	if (ret)
		pr_err("oplus_dsi_display_set_vsync_switch_gpio(%d) fail\n", vsync_switch_gpio);

	return count;
}

ssize_t oplus_get_vsync_switch(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct dsi_display *display = get_main_display();
	int vsync_switch_gpio = 0;

	vsync_switch_gpio = oplus_dsi_display_get_vsync_switch_gpio(display);

	return sprintf(buf, "%d\n", vsync_switch_gpio);
}

void oplus_dsi_display_vsync_switch(void *disp)
{
	struct dsi_display *display = disp;
	int level = 0;
	int h_skew = 0;
	int rc = 0;

	if (!oplus_adfr_vsync_switch_is_enable()) {
		SDE_EVT32(0);
		return;
	}

	rc = dsi_panel_tx_cmd_set(display->panel, DSI_CMD_ADFR_PRE_SWITCH);
	if (rc)
		DSI_ERR("[%s] failed to send DSI_CMD_ADFR_PRE_SWITCH cmds rc=%d\n", display->panel, rc);

	if (oplus_adfr_vsync_mode() != OPLUS_HARD_VSYNC) {
		printk(KERN_INFO "%s oplus adfr is not hard vsync\n", __func__);
		return;
	}

	h_skew = display->panel->cur_mode->timing.h_skew;

	if (h_skew == 2 || h_skew == 3) {
		level = 1;//TE Vsync
	} else {
		level = 0;//TP Vsync
	}

	oplus_dsi_display_vsync_switch_check_te(display, level);
}

int oplus_dsi_panel_parse_panel_vsync_source(void *v_mode,
				void *v_utils)
{
	struct dsi_display_mode *mode = v_mode;
	struct dsi_parser_utils *utils = v_utils;
	u32 panel_vsync_source = 0;
	int rc = 0;

	if (!mode || !mode->priv_info) {
		DSI_ERR("invalid arguments\n");
		return -EINVAL;
	}

	if (oplus_adfr_vsync_switch_is_enable() && oplus_adfr_vsync_mode() == OPLUS_SOFT_VSYNC) {
		rc = utils->read_u32(utils->data,
				"qcom,mdss-dsi-panel-vsync-source", &panel_vsync_source);
	}

	mode->vsync_source = rc ? 0xff : panel_vsync_source;

	return rc;
}


