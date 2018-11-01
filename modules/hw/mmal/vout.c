/*****************************************************************************
 * mmal.c: MMAL-based vout plugin for Raspberry Pi
 *****************************************************************************
 * Copyright © 2014 jusst technologies GmbH
 * $Id$
 *
 * Authors: Dennis Hamester <dennis.hamester@gmail.com>
 *          Julian Scheel <julian@jusst.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include <stdatomic.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_threads.h>
#include <vlc_vout_display.h>
#include <vlc_modules.h>

#include "mmal_picture.h"
#include "subpic.h"

#include <bcm_host.h>
#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_default_components.h>
#include <interface/vmcs_host/vc_tvservice.h>
#include <interface/vmcs_host/vc_dispmanx.h>

#define TRACE_ALL 0

#define MAX_BUFFERS_IN_TRANSIT 1
#define VC_TV_MAX_MODE_IDS 127

#define MMAL_LAYER_NAME "mmal-layer"
#define MMAL_LAYER_TEXT N_("VideoCore layer where the video is displayed.")
#define MMAL_LAYER_LONGTEXT N_("VideoCore layer where the video is displayed. Subpictures are displayed directly above and a black background directly below.")

#define MMAL_BLANK_BACKGROUND_NAME "mmal-blank-background"
#define MMAL_BLANK_BACKGROUND_TEXT N_("Blank screen below video.")
#define MMAL_BLANK_BACKGROUND_LONGTEXT N_("Render blank screen below video. " \
        "Increases VideoCore load.")

#define MMAL_ADJUST_REFRESHRATE_NAME "mmal-adjust-refreshrate"
#define MMAL_ADJUST_REFRESHRATE_TEXT N_("Adjust HDMI refresh rate to the video.")
#define MMAL_ADJUST_REFRESHRATE_LONGTEXT N_("Adjust HDMI refresh rate to the video.")

#define MMAL_NATIVE_INTERLACED "mmal-native-interlaced"
#define MMAL_NATIVE_INTERLACE_TEXT N_("Force interlaced video mode.")
#define MMAL_NATIVE_INTERLACE_LONGTEXT N_("Force the HDMI output into an " \
        "interlaced video mode for interlaced video content.")

/* Ideal rendering phase target is at rough 25% of frame duration */
#define PHASE_OFFSET_TARGET ((double)0.25)
#define PHASE_CHECK_INTERVAL 100

struct dmx_region_t {
    struct dmx_region_t *next;
    picture_t *picture;
    MMAL_BUFFER_HEADER_T * buf;
    VC_RECT_T bmp_rect;
    VC_RECT_T src_rect;
    VC_RECT_T dst_rect;
    VC_DISPMANX_ALPHA_T alpha;
    DISPMANX_ELEMENT_HANDLE_T element;
    DISPMANX_RESOURCE_HANDLE_T resource;
    int32_t pos_x;
    int32_t pos_y;
};

#define SUBS_MAX 4

typedef struct vout_subpic_s {
    MMAL_COMPONENT_T *component;
    subpic_reg_stash_t sub;
} vout_subpic_t;

struct vout_display_sys_t {
    vlc_mutex_t manage_mutex;

    picture_t **pictures; /* Actual list of alloced pictures passed into picture_pool */
    picture_pool_t *picture_pool;

    MMAL_COMPONENT_T *component;
    MMAL_PORT_T *input;
    MMAL_POOL_T *pool; /* mmal buffer headers, used for pushing pictures to component*/
    int i_planes; /* Number of actually used planes, 1 for opaque, 3 for i420 */

    uint32_t buffer_size; /* size of actual mmal buffers */
    int buffers_in_transit; /* number of buffers currently pushed to mmal component */
    unsigned num_buffers; /* number of buffers allocated at mmal port */

    DISPMANX_DISPLAY_HANDLE_T dmx_handle;
    DISPMANX_ELEMENT_HANDLE_T bkg_element;
    DISPMANX_RESOURCE_HANDLE_T bkg_resource;
    unsigned display_width;
    unsigned display_height;

    unsigned int i_frame_rate_base; /* cached framerate to detect changes for rate adjustment */
    unsigned int i_frame_rate;

    int next_phase_check; /* lowpass for phase check frequency */
    int phase_offset; /* currently applied offset to presentation time in ns */
    int layer; /* the dispman layer (z-index) used for video rendering */

    bool need_configure_display; /* indicates a required display reconfigure to main thread */
    bool adjust_refresh_rate;
    bool native_interlaced;
    bool b_top_field_first; /* cached interlaced settings to detect changes for native mode */
    bool b_progressive;
    bool force_config;

    vout_subpic_t subs[SUBS_MAX];
};


/* Utility functions */
static int configure_display(vout_display_t *vd, const vout_display_cfg_t *cfg,
                const video_format_t *fmt);

/* TV service */
static int query_resolution(vout_display_t *vd, unsigned *width, unsigned *height);
static void tvservice_cb(void *callback_data, uint32_t reason, uint32_t param1,
                uint32_t param2);
static void adjust_refresh_rate(vout_display_t *vd, const video_format_t *fmt);
static int set_latency_target(vout_display_t *vd, bool enable);

/* DispManX */
static void close_dmx(vout_display_t *vd);
static void show_background(vout_display_t *vd, bool enable);
static void maintain_phase_sync(vout_display_t *vd);


static void vd_input_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buf)
{
#if TRACE_ALL
    vout_display_t * const vd = (vout_display_t *)port->userdata;
    pic_ctx_mmal_t * ctx = buf->user_data;
    msg_Dbg(vd, "<<< %s[%d] cmd=%d, ctx=%p, buf=%p, flags=%#x, pts=%lld", __func__, buf->cmd, ctx, buf,
            buf->flags, (long long)buf->pts);
#else
    VLC_UNUSED(port);
#endif

    mmal_buffer_header_release(buf);

#if TRACE_ALL
    msg_Dbg(vd, ">>> %s", __func__);
#endif
}

static int query_resolution(vout_display_t *vd, unsigned *width, unsigned *height)
{
    TV_DISPLAY_STATE_T display_state;
    int ret = 0;

    if (vc_tv_get_display_state(&display_state) == 0) {
        if (display_state.state & 0xFF) {
            *width = display_state.display.hdmi.width;
            *height = display_state.display.hdmi.height;
        } else if (display_state.state & 0xFF00) {
            *width = display_state.display.sdtv.width;
            *height = display_state.display.sdtv.height;
        } else {
            msg_Warn(vd, "Invalid display state %"PRIx32, display_state.state);
            ret = -1;
        }
    } else {
        msg_Warn(vd, "Failed to query display resolution");
        ret = -1;
    }

    return ret;
}

static int configure_display(vout_display_t *vd, const vout_display_cfg_t *cfg,
                const video_format_t *fmt)
{
    vout_display_sys_t *sys = vd->sys;
    vout_display_place_t place;
    MMAL_DISPLAYREGION_T display_region;
    MMAL_STATUS_T status;

    if (!cfg && !fmt)
        return -EINVAL;

    if (fmt) {
        sys->input->format->es->video.par.num = fmt->i_sar_num;
        sys->input->format->es->video.par.den = fmt->i_sar_den;

        status = mmal_port_format_commit(sys->input);
        if (status != MMAL_SUCCESS) {
            msg_Err(vd, "Failed to commit format for input port %s (status=%"PRIx32" %s)",
                            sys->input->name, status, mmal_status_to_string(status));
            return -EINVAL;
        }
    } else {
        fmt = &vd->source;
    }

    if (!cfg)
        cfg = vd->cfg;

    vout_display_PlacePicture(&place, fmt, cfg, false);

    display_region.hdr.id = MMAL_PARAMETER_DISPLAYREGION;
    display_region.hdr.size = sizeof(MMAL_DISPLAYREGION_T);
    display_region.fullscreen = MMAL_FALSE;
    display_region.src_rect.x = fmt->i_x_offset;
    display_region.src_rect.y = fmt->i_y_offset;
    display_region.src_rect.width = fmt->i_visible_width;
    display_region.src_rect.height = fmt->i_visible_height;
    display_region.dest_rect.x = place.x;
    display_region.dest_rect.y = place.y;
    display_region.dest_rect.width = place.width;
    display_region.dest_rect.height = place.height;
    display_region.layer = sys->layer;
    display_region.set = MMAL_DISPLAY_SET_FULLSCREEN | MMAL_DISPLAY_SET_SRC_RECT |
            MMAL_DISPLAY_SET_DEST_RECT | MMAL_DISPLAY_SET_LAYER;
    status = mmal_port_parameter_set(sys->input, &display_region.hdr);
    if (status != MMAL_SUCCESS) {
        msg_Err(vd, "Failed to set display region (status=%"PRIx32" %s)",
                        status, mmal_status_to_string(status));
        return -EINVAL;
    }

    show_background(vd, var_InheritBool(vd, MMAL_BLANK_BACKGROUND_NAME));
    sys->adjust_refresh_rate = var_InheritBool(vd, MMAL_ADJUST_REFRESHRATE_NAME);
    sys->native_interlaced = var_InheritBool(vd, MMAL_NATIVE_INTERLACED);
    if (sys->adjust_refresh_rate) {
        adjust_refresh_rate(vd, fmt);
        set_latency_target(vd, true);
    }

    return 0;
}

// Actual picture pool for MMAL opaques is just a set of trivial containers
static picture_pool_t *vd_pool(vout_display_t *vd, unsigned count)
{
    msg_Dbg(vd, "%s: fmt:%dx%d, source:%dx%d", __func__, vd->fmt.i_width, vd->fmt.i_height, vd->source.i_width, vd->source.i_height);
    return picture_pool_NewFromFormat(&vd->fmt, count);
}

static void vd_display(vout_display_t *vd, picture_t *p_pic,
                subpicture_t *subpicture)
{
    vout_display_sys_t * const sys = vd->sys;
    MMAL_STATUS_T err;

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s", __func__);
#endif

    // Not expecting subpictures in the current setup
    // Subpics should be attached to the main pic
    if (subpicture != NULL) {
        subpicture_Delete(subpicture);
    }

    if (sys->force_config ||
        p_pic->format.i_frame_rate != sys->i_frame_rate ||
        p_pic->format.i_frame_rate_base != sys->i_frame_rate_base ||
        p_pic->b_progressive != sys->b_progressive ||
        p_pic->b_top_field_first != sys->b_top_field_first)
    {
        sys->force_config = false;
        sys->b_top_field_first = p_pic->b_top_field_first;
        sys->b_progressive = p_pic->b_progressive;
        sys->i_frame_rate = p_pic->format.i_frame_rate;
        sys->i_frame_rate_base = p_pic->format.i_frame_rate_base;
        configure_display(vd, NULL, &p_pic->format);
    }


    if (!sys->input->is_enabled &&
        (err = mmal_port_enable(sys->input, vd_input_port_cb)) != MMAL_SUCCESS)
    {
        msg_Err(vd, "Input port enable failed");
        goto fail;
    }

    // Stuff into input
    // We assume the BH is already set up with values reflecting pic date etc.
    {
        MMAL_BUFFER_HEADER_T * const pic_buf = pic_mmal_buffer(p_pic);
        if ((err = port_send_replicated(sys->input, sys->pool, pic_buf, pic_buf->pts)) != MMAL_SUCCESS)
        {
            msg_Err(vd, "Send buffer to input failed");
            goto fail;
        }
    }

    if (p_pic->context == NULL) {
        msg_Dbg(vd, "%s: No context", __func__);
    }
    else
    {
        unsigned int sub_no = 0;

        for (sub_no = 0; sub_no != SUBS_MAX; ++sub_no) {
            int rv;
            if ((rv = hw_mmal_subpic_update(VLC_OBJECT(vd), p_pic, sub_no, &sys->subs[sub_no].sub,
                                            &(MMAL_RECT_T){.width = sys->display_width, .height = sys->display_height},
                                            p_pic->date)) == 0)
                break;
            else if (rv < 0)
                goto fail;
        }
    }

    picture_Release(p_pic);

    if (sys->next_phase_check == 0 && sys->adjust_refresh_rate)
        maintain_phase_sync(vd);
    sys->next_phase_check = (sys->next_phase_check + 1) % PHASE_CHECK_INTERVAL;

fail:
    /* NOP */;
}

static int vd_control(vout_display_t *vd, int query, va_list args)
{
    vout_display_sys_t *sys = vd->sys;
    vout_display_cfg_t cfg;
    const vout_display_cfg_t *tmp_cfg;
    int ret = VLC_EGENERIC;

    switch (query) {
        case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
            tmp_cfg = va_arg(args, const vout_display_cfg_t *);
            if (tmp_cfg->display.width == sys->display_width &&
                            tmp_cfg->display.height == sys->display_height) {
                cfg = *vd->cfg;
                cfg.display.width = sys->display_width;
                cfg.display.height = sys->display_height;
                if (configure_display(vd, &cfg, NULL) >= 0)
                    ret = VLC_SUCCESS;
            }
            break;

        case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
        case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
            if (configure_display(vd, NULL, &vd->source) >= 0)
                ret = VLC_SUCCESS;
            break;

        case VOUT_DISPLAY_RESET_PICTURES:
            msg_Warn(vd, "Reset Pictures");
            vd->fmt = vd->source; // Take whatever source wants to give us
            ret = VLC_SUCCESS;
            break;

        case VOUT_DISPLAY_CHANGE_ZOOM:
            msg_Warn(vd, "Unsupported control query %d", query);
            break;

        case VOUT_DISPLAY_CHANGE_MMAL_HIDE:
        {
            MMAL_STATUS_T err;
            unsigned int i;

            msg_Dbg(vd, "Hide display");

            for (i = 0; i != SUBS_MAX; ++i)
                hw_mmal_subpic_flush(VLC_OBJECT(vd), &sys->subs[i].sub);

            if (sys->input->is_enabled &&
                (err = mmal_port_disable(sys->input)) != MMAL_SUCCESS)
            {
                msg_Err(vd, "Unable to disable port: err=%d", err);
                ret = VLC_EGENERIC;
                break;
            }
            show_background(vd, false);
            sys->force_config = true;

            ret = VLC_SUCCESS;
            break;
        }

        default:
            msg_Warn(vd, "Unknown control query %d", query);
            break;
    }

    return ret;
}

static void vd_manage(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;
    unsigned width, height;

    vlc_mutex_lock(&sys->manage_mutex);

    if (sys->need_configure_display) {
        close_dmx(vd);
        sys->dmx_handle = vc_dispmanx_display_open(0);

        if (query_resolution(vd, &width, &height) >= 0) {
            sys->display_width = width;
            sys->display_height = height;
//            msg_Dbg(vd, "%s: %dx%d", __func__, width, height);
//            vout_window_ReportSize(vd->cfg->window, width, height);
        }

        sys->need_configure_display = false;
    }

    vlc_mutex_unlock(&sys->manage_mutex);
}

static void vd_prepare(vout_display_t *vd, picture_t *picture,
#if VLC_VER_3
                       subpicture_t *subpicture
#else
                       subpicture_t *subpicture, vlc_tick_t date
#endif
                       )
{
    VLC_UNUSED(picture);
    VLC_UNUSED(subpicture);
//    VLC_UNUSED(date);

    vd_manage(vd);
#if 0
    VLC_UNUSED(date);
    vout_display_sys_t *sys = vd->sys;
    picture_sys_t *pic_sys = picture->p_sys;

    if (!sys->adjust_refresh_rate || pic_sys->displayed)
        return;

    /* Apply the required phase_offset to the picture, so that vd_display()
     * will be called at the corrected time from the core */
    picture->date += sys->phase_offset;
#endif
}


static void vd_control_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    vout_display_t *vd = (vout_display_t *)port->userdata;
    MMAL_STATUS_T status;

    if (buffer->cmd == MMAL_EVENT_ERROR) {
        status = *(uint32_t *)buffer->data;
        msg_Err(vd, "MMAL error %"PRIx32" \"%s\"", status, mmal_status_to_string(status));
    }

    mmal_buffer_header_release(buffer);
}

static void tvservice_cb(void *callback_data, uint32_t reason, uint32_t param1, uint32_t param2)
{
    VLC_UNUSED(reason);
    VLC_UNUSED(param1);
    VLC_UNUSED(param2);

    vout_display_t *vd = (vout_display_t *)callback_data;
    vout_display_sys_t *sys = vd->sys;

    vlc_mutex_lock(&sys->manage_mutex);
    sys->need_configure_display = true;
    vlc_mutex_unlock(&sys->manage_mutex);
}

static int set_latency_target(vout_display_t *vd, bool enable)
{
    vout_display_sys_t *sys = vd->sys;
    MMAL_STATUS_T status;

    MMAL_PARAMETER_AUDIO_LATENCY_TARGET_T latency_target = {
        .hdr = { MMAL_PARAMETER_AUDIO_LATENCY_TARGET, sizeof(latency_target) },
        .enable = enable ? MMAL_TRUE : MMAL_FALSE,
        .filter = 2,
        .target = 4000,
        .shift = 3,
        .speed_factor = -135,
        .inter_factor = 500,
        .adj_cap = 20
    };

    status = mmal_port_parameter_set(sys->input, &latency_target.hdr);
    if (status != MMAL_SUCCESS) {
        msg_Err(vd, "Failed to configure latency target on input port %s (status=%"PRIx32" %s)",
                        sys->input->name, status, mmal_status_to_string(status));
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

static void adjust_refresh_rate(vout_display_t *vd, const video_format_t *fmt)
{
    vout_display_sys_t *sys = vd->sys;
    TV_DISPLAY_STATE_T display_state;
    TV_SUPPORTED_MODE_NEW_T supported_modes[VC_TV_MAX_MODE_IDS];
    char response[20]; /* answer is hvs_update_fields=%1d */
    int num_modes;
    double frame_rate = (double)fmt->i_frame_rate / fmt->i_frame_rate_base;
    int best_id = -1;
    double best_score, score;
    int i;

    vc_tv_get_display_state(&display_state);
    if(display_state.display.hdmi.mode != HDMI_MODE_OFF) {
        num_modes = vc_tv_hdmi_get_supported_modes_new(display_state.display.hdmi.group,
                        supported_modes, VC_TV_MAX_MODE_IDS, NULL, NULL);

        for (i = 0; i < num_modes; ++i) {
            TV_SUPPORTED_MODE_NEW_T *mode = &supported_modes[i];
            if (!sys->native_interlaced) {
                if (mode->width != display_state.display.hdmi.width ||
                                mode->height != display_state.display.hdmi.height ||
                                mode->scan_mode == HDMI_INTERLACED)
                    continue;
            } else {
                if (mode->width != vd->fmt.i_visible_width ||
                        mode->height != vd->fmt.i_visible_height)
                    continue;
                if (mode->scan_mode != sys->b_progressive ? HDMI_NONINTERLACED : HDMI_INTERLACED)
                    continue;
            }

            score = fmod(supported_modes[i].frame_rate, frame_rate);
            if((best_id < 0) || (score < best_score)) {
                best_id = i;
                best_score = score;
            }
        }

        if((best_id >= 0) && (display_state.display.hdmi.mode != supported_modes[best_id].code)) {
            msg_Info(vd, "Setting HDMI refresh rate to %"PRIu32,
                            supported_modes[best_id].frame_rate);
            vc_tv_hdmi_power_on_explicit_new(HDMI_MODE_HDMI,
                            supported_modes[best_id].group,
                            supported_modes[best_id].code);
        }

        if (sys->native_interlaced &&
                supported_modes[best_id].scan_mode == HDMI_INTERLACED) {
            char hvs_mode = sys->b_top_field_first ? '1' : '2';
            if (vc_gencmd(response, sizeof(response), "hvs_update_fields %c",
                    hvs_mode) != 0 || response[18] != hvs_mode)
                msg_Warn(vd, "Could not set hvs field mode");
            else
                msg_Info(vd, "Configured hvs field mode for interlaced %s playback",
                        sys->b_top_field_first ? "tff" : "bff");
        }
    }
}

static void close_dmx(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;

    show_background(vd, false);

    vc_dispmanx_display_close(sys->dmx_handle);
    sys->dmx_handle = DISPMANX_NO_HANDLE;
}

static void maintain_phase_sync(vout_display_t *vd)
{
    MMAL_PARAMETER_VIDEO_RENDER_STATS_T render_stats = {
        .hdr = { MMAL_PARAMETER_VIDEO_RENDER_STATS, sizeof(render_stats) },
    };
    int32_t frame_duration = CLOCK_FREQ /
        ((double)vd->sys->i_frame_rate /
        vd->sys->i_frame_rate_base);
    vout_display_sys_t *sys = vd->sys;
    int32_t phase_offset;
    MMAL_STATUS_T status;

    status = mmal_port_parameter_get(sys->input, &render_stats.hdr);
    if (status != MMAL_SUCCESS) {
        msg_Err(vd, "Failed to read render stats on control port %s (status=%"PRIx32" %s)",
                        sys->input->name, status, mmal_status_to_string(status));
        return;
    }

    if (render_stats.valid) {
#ifndef NDEBUG
        msg_Dbg(vd, "render_stats: match: %u, period: %u ms, phase: %u ms, hvs: %u",
                render_stats.match, render_stats.period / 1000, render_stats.phase / 1000,
                render_stats.hvs_status);
#endif

        if (render_stats.phase > 0.1 * frame_duration &&
                render_stats.phase < 0.75 * frame_duration)
            return;

        phase_offset = frame_duration * PHASE_OFFSET_TARGET - render_stats.phase;
        if (phase_offset < 0)
            phase_offset += frame_duration;
        else
            phase_offset %= frame_duration;

        sys->phase_offset += phase_offset;
        sys->phase_offset %= frame_duration;
        msg_Dbg(vd, "Apply phase offset of %"PRId32" ms (total offset %"PRId32" ms)",
                phase_offset / 1000, sys->phase_offset / 1000);

        /* Reset the latency target, so that it does not get confused
         * by the jump in the offset */
        set_latency_target(vd, false);
        set_latency_target(vd, true);
    }
}

static void show_background(vout_display_t *vd, bool enable)
{
    vout_display_sys_t *sys = vd->sys;
    uint32_t image_ptr, color = 0xFF000000;
    VC_RECT_T dst_rect, src_rect;
    DISPMANX_UPDATE_HANDLE_T update;

    if (enable && !sys->bkg_element) {
        sys->bkg_resource = vc_dispmanx_resource_create(VC_IMAGE_RGBA32, 1, 1,
                        &image_ptr);
        vc_dispmanx_rect_set(&dst_rect, 0, 0, 1, 1);
        vc_dispmanx_resource_write_data(sys->bkg_resource, VC_IMAGE_RGBA32,
                        sizeof(color), &color, &dst_rect);
        vc_dispmanx_rect_set(&src_rect, 0, 0, 1 << 16, 1 << 16);
        vc_dispmanx_rect_set(&dst_rect, 0, 0, 0, 0);
        update = vc_dispmanx_update_start(0);
        sys->bkg_element = vc_dispmanx_element_add(update, sys->dmx_handle,
                        sys->layer - 1, &dst_rect, sys->bkg_resource, &src_rect,
                        DISPMANX_PROTECTION_NONE, NULL, NULL, VC_IMAGE_ROT0);
        vc_dispmanx_update_submit_sync(update);
    } else if (!enable && sys->bkg_element) {
        update = vc_dispmanx_update_start(0);
        vc_dispmanx_element_remove(update, sys->bkg_element);
        vc_dispmanx_resource_delete(sys->bkg_resource);
        vc_dispmanx_update_submit_sync(update);
        sys->bkg_element = DISPMANX_NO_HANDLE;
        sys->bkg_resource = DISPMANX_NO_HANDLE;
    }
}

static void CloseMmalVout(vlc_object_t *object)
{
    vout_display_t * const vd = (vout_display_t *)object;
    vout_display_sys_t * const sys = vd->sys;
    char response[20]; /* answer is hvs_update_fields=%1d */

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s", __func__);
#endif
    vc_tv_unregister_callback_full(tvservice_cb, vd);

    if (sys->dmx_handle)
        close_dmx(vd);

    if (sys->component && sys->component->control->is_enabled)
        mmal_port_disable(sys->component->control);

    {
        unsigned int i;
        for (i = 0; i != SUBS_MAX; ++i) {
            vout_subpic_t * const sub = sys->subs + i;
            if (sub->component != NULL) {
                hw_mmal_subpic_close(VLC_OBJECT(vd), &sub->sub);
                if (sub->component->is_enabled)
                    mmal_component_disable(sub->component);
                mmal_component_release(sub->component);
                sub->component = NULL;
            }
        }
    }

    if (sys->input && sys->input->is_enabled)
        mmal_port_disable(sys->input);

    if (sys->component && sys->component->is_enabled)
        mmal_component_disable(sys->component);

    if (sys->pool)
        mmal_pool_destroy(sys->pool);

    if (sys->component)
        mmal_component_release(sys->component);

    if (sys->picture_pool)
        picture_pool_Release(sys->picture_pool);

    vlc_mutex_destroy(&sys->manage_mutex);

    if (sys->native_interlaced) {
        if (vc_gencmd(response, sizeof(response), "hvs_update_fields 0") < 0 ||
                response[18] != '0')
            msg_Warn(vd, "Could not reset hvs field mode");
    }

    free(sys->pictures);
    free(sys);

    bcm_host_deinit();

#if TRACE_ALL
    msg_Dbg(vd, ">>> %s", __func__);
#endif
}

static int OpenMmalVout(vlc_object_t *object)
{
    vout_display_t *vd = (vout_display_t *)object;
    vout_display_sys_t *sys;
    vout_display_place_t place;
    MMAL_DISPLAYREGION_T display_region;
    MMAL_STATUS_T status;
    int ret = VLC_EGENERIC;

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s", __func__);
#endif
    if (vd->fmt.i_chroma != VLC_CODEC_MMAL_OPAQUE)
    {
#if TRACE_ALL
        msg_Dbg(vd, ">>> %s: Format not MMAL", __func__);
#endif
        return VLC_EGENERIC;
    }

    sys = calloc(1, sizeof(struct vout_display_sys_t));
    if (!sys)
        return VLC_ENOMEM;
    vd->sys = sys;

    sys->layer = var_InheritInteger(vd, MMAL_LAYER_NAME);

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_RENDERER, &sys->component);
    if (status != MMAL_SUCCESS) {
        msg_Err(vd, "Failed to create MMAL component %s (status=%"PRIx32" %s)",
                        MMAL_COMPONENT_DEFAULT_VIDEO_RENDERER, status, mmal_status_to_string(status));
        goto fail;
    }

    sys->component->control->userdata = (struct MMAL_PORT_USERDATA_T *)vd;
    status = mmal_port_enable(sys->component->control, vd_control_port_cb);
    if (status != MMAL_SUCCESS) {
        msg_Err(vd, "Failed to enable control port %s (status=%"PRIx32" %s)",
                        sys->component->control->name, status, mmal_status_to_string(status));
        goto fail;
    }

    sys->input = sys->component->input[0];
    sys->input->userdata = (struct MMAL_PORT_USERDATA_T *)vd;

    sys->input->format->encoding = MMAL_ENCODING_OPAQUE;
    sys->i_planes = 1;
    sys->buffer_size = sys->input->buffer_size_recommended;

    sys->input->format->es->video.width = vd->fmt.i_width;
    sys->input->format->es->video.height = vd->fmt.i_height;
    sys->input->format->es->video.crop.x = 0;
    sys->input->format->es->video.crop.y = 0;
    sys->input->format->es->video.crop.width = vd->fmt.i_width;
    sys->input->format->es->video.crop.height = vd->fmt.i_height;
    sys->input->format->es->video.par.num = vd->source.i_sar_num;
    sys->input->format->es->video.par.den = vd->source.i_sar_den;

    status = port_parameter_set_bool(sys->input, MMAL_PARAMETER_ZERO_COPY, true);
    if (status != MMAL_SUCCESS) {
       msg_Err(vd, "Failed to set zero copy on port %s (status=%"PRIx32" %s)",
                sys->input->name, status, mmal_status_to_string(status));
       goto fail;
    }

    status = mmal_port_format_commit(sys->input);
    if (status != MMAL_SUCCESS) {
        msg_Err(vd, "Failed to commit format for input port %s (status=%"PRIx32" %s)",
                        sys->input->name, status, mmal_status_to_string(status));
        goto fail;
    }
    sys->input->buffer_size = sys->input->buffer_size_recommended;
    sys->input->buffer_num = 30;

    vout_display_PlacePicture(&place, &vd->source, vd->cfg, false);
    display_region.hdr.id = MMAL_PARAMETER_DISPLAYREGION;
    display_region.hdr.size = sizeof(MMAL_DISPLAYREGION_T);
    display_region.fullscreen = MMAL_FALSE;
    display_region.src_rect.x = vd->fmt.i_x_offset;
    display_region.src_rect.y = vd->fmt.i_y_offset;
    display_region.src_rect.width = vd->fmt.i_visible_width;
    display_region.src_rect.height = vd->fmt.i_visible_height;
    display_region.dest_rect.x = place.x;
    display_region.dest_rect.y = place.y;
    display_region.dest_rect.width = place.width;
    display_region.dest_rect.height = place.height;
    display_region.layer = sys->layer;
    display_region.set = MMAL_DISPLAY_SET_FULLSCREEN | MMAL_DISPLAY_SET_SRC_RECT |
            MMAL_DISPLAY_SET_DEST_RECT | MMAL_DISPLAY_SET_LAYER;
    status = mmal_port_parameter_set(sys->input, &display_region.hdr);
    if (status != MMAL_SUCCESS) {
        msg_Err(vd, "Failed to set display region (status=%"PRIx32" %s)",
                        status, mmal_status_to_string(status));
        goto fail;
    }

    status = mmal_port_enable(sys->input, vd_input_port_cb);
    if (status != MMAL_SUCCESS) {
        msg_Err(vd, "Failed to enable input port %s (status=%"PRIx32" %s)",
                sys->input->name, status, mmal_status_to_string(status));
        goto fail;
    }

    status = mmal_component_enable(sys->component);
    if (status != MMAL_SUCCESS) {
        msg_Err(vd, "Failed to enable component %s (status=%"PRIx32" %s)",
                sys->component->name, status, mmal_status_to_string(status));
        goto fail;
    }

    if ((sys->pool = mmal_pool_create(sys->input->buffer_num, 0)) == NULL)
    {
        msg_Err(vd, "Failed to create input pool");
        goto fail;
    }

    {
        unsigned int i;
        for (i = 0; i != SUBS_MAX; ++i) {
            vout_subpic_t * const sub = sys->subs + i;
            if ((status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_RENDERER, &sub->component)) != MMAL_SUCCESS)
            {
                msg_Dbg(vd, "Failed to create subpic component %d", i);
                goto fail;
            }
            if ((status = hw_mmal_subpic_open(VLC_OBJECT(vd), &sub->sub, sub->component->input[0], sys->layer + i + 1)) != MMAL_SUCCESS) {
                msg_Dbg(vd, "Failed to open subpic %d", i);
                goto fail;
            }
            if ((status = mmal_component_enable(sub->component)) != MMAL_SUCCESS)
            {
                msg_Dbg(vd, "Failed to enable subpic component %d", i);
                goto fail;
            }
        }
    }


    vlc_mutex_init(&sys->manage_mutex);

    vd->pool = vd_pool;
    vd->prepare = vd_prepare;
    vd->display = vd_display;
    vd->control = vd_control;

    vc_tv_register_callback(tvservice_cb, vd);

    if (query_resolution(vd, &sys->display_width, &sys->display_height) >= 0) {
//        vout_window_ReportSize(vd->cfg->window,
//                               sys->display_width, sys->display_height);
    } else {
        sys->display_width = vd->cfg->display.width;
        sys->display_height = vd->cfg->display.height;
    }

    sys->dmx_handle = vc_dispmanx_display_open(0);

    msg_Dbg(vd, ">>> %s: ok", __func__);
    return VLC_SUCCESS;

fail:
    CloseMmalVout(object);

    msg_Dbg(vd, ">>> %s: rv=%d", __func__, ret);

    return ret == VLC_SUCCESS ? VLC_EGENERIC : ret;
}

vlc_module_begin()

    add_submodule()

    set_shortname(N_("MMAL vout"))
    set_description(N_("MMAL-based vout plugin for Raspberry Pi"))
    set_capability("vout display", 0)
    add_shortcut("mmal_vout")
    set_category( CAT_VIDEO )
    set_subcategory( SUBCAT_VIDEO_VOUT )

    add_integer(MMAL_LAYER_NAME, 1, MMAL_LAYER_TEXT, MMAL_LAYER_LONGTEXT, false)
    add_bool(MMAL_BLANK_BACKGROUND_NAME, true, MMAL_BLANK_BACKGROUND_TEXT,
                    MMAL_BLANK_BACKGROUND_LONGTEXT, true);
    add_bool(MMAL_ADJUST_REFRESHRATE_NAME, false, MMAL_ADJUST_REFRESHRATE_TEXT,
                    MMAL_ADJUST_REFRESHRATE_LONGTEXT, false)
    add_bool(MMAL_NATIVE_INTERLACED, false, MMAL_NATIVE_INTERLACE_TEXT,
                    MMAL_NATIVE_INTERLACE_LONGTEXT, false)
    set_callbacks(OpenMmalVout, CloseMmalVout)

vlc_module_end()


