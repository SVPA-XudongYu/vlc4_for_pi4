#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdatomic.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_threads.h>
#include <vlc_vout_display.h>
#include <vlc_modules.h>

#include <bcm_host.h>
#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_default_components.h>

#include "mmal_picture.h"

#define TRACE_ALL 1


typedef struct mmal_x11_sys_s
{
    bool use_mmal;
    vout_display_t * cur_vout;
    vout_display_t * mmal_vout;
    vout_display_t * x_vout;
} mmal_x11_sys_t;

static void unload_display_module(vout_display_t * const x_vout)
{
    if (x_vout != NULL) {
       if (x_vout->module != NULL) {
            module_unneed(x_vout, x_vout->module);
        }
        vlc_object_release(x_vout);
    }
}

static void CloseMmalX11(vlc_object_t *object)
{
    vout_display_t * const vd = (vout_display_t *)object;
    mmal_x11_sys_t * const sys = (mmal_x11_sys_t *)vd->sys;

    msg_Dbg(vd, "<<< %s", __func__);

    if (sys == NULL)
        return;

    unload_display_module(sys->x_vout);

    unload_display_module(sys->mmal_vout);

    free(sys);

    msg_Dbg(vd, ">>> %s", __func__);
}

static void mmal_x11_event(vout_display_t * x_vd, int cmd, va_list args)
{
    vout_display_t * const vd = x_vd->owner.sys;
#if TRACE_ALL
    msg_Dbg(vd, "<<< %s (cmd=%d)", __func__, cmd);
#endif
    vd->owner.event(vd, cmd, args);
}

static vout_window_t * mmal_x11_window_new(vout_display_t * x_vd, unsigned type)
{
    vout_display_t * const vd = x_vd->owner.sys;
#if TRACE_ALL
    msg_Dbg(vd, "<<< %s (type=%d)", __func__, type);
#endif
    return vd->owner.window_new(vd, type);
}

static void mmal_x11_window_del(vout_display_t * x_vd, vout_window_t * win)
{
    vout_display_t * const vd = x_vd->owner.sys;
#if TRACE_ALL
    msg_Dbg(vd, "<<< %s", __func__);
#endif
    vd->owner.window_del(vd, win);
}


static vout_display_t * load_display_module(vout_display_t * const vd, mmal_x11_sys_t * const sys,
                                            const char * const cap, const char * const module_name)
{
    vout_display_t * const x_vout = vlc_object_create(vd, sizeof(*x_vout));

    if (!x_vout)
        return NULL;

    x_vout->owner.sys = vd;
    x_vout->owner.event = mmal_x11_event;
    x_vout->owner.window_new = mmal_x11_window_new;
    x_vout->owner.window_del = mmal_x11_window_del;

    x_vout->cfg    = vd->cfg;
    x_vout->source = vd->source;
    x_vout->info   = vd->info;

    x_vout->fmt = vd->fmt;

    if ((x_vout->module = module_need(x_vout, cap, module_name, true)) == NULL)
    {
        msg_Err(vd, "Failed to find X11 module");
        goto fail;
    }

    return x_vout;

fail:
    vlc_object_release(x_vout);
    return NULL;
}


/* Return a pointer over the current picture_pool_t* (mandatory).
 *
 * For performance reasons, it is best to provide at least count
 * pictures but it is not mandatory.
 * You can return NULL when you cannot/do not want to allocate
 * pictures.
 * The vout display module keeps the ownership of the pool and can
 * destroy it only when closing or on invalid pictures control.
 */
static picture_pool_t * mmal_x11_pool(vout_display_t * vd, unsigned count)
{
    mmal_x11_sys_t * const sys = (mmal_x11_sys_t *)vd->sys;
    vout_display_t * const x_vd = sys->cur_vout;
#if TRACE_ALL
    msg_Dbg(vd, "<<< %s (count=%d) %dx%d", __func__, count, x_vd->fmt.i_width, x_vd->fmt.i_height);
#endif
    return x_vd->pool(x_vd, count);
}

/* Prepare a picture and an optional subpicture for display (optional).
 *
 * It is called before the next pf_display call to provide as much
 * time as possible to prepare the given picture and the subpicture
 * for display.
 * You are guaranted that pf_display will always be called and using
 * the exact same picture_t and subpicture_t.
 * You cannot change the pixel content of the picture_t or of the
 * subpicture_t.
 */
static void mmal_x11_prepare(vout_display_t * vd, picture_t * pic, subpicture_t * sub)
{
    mmal_x11_sys_t * const sys = (mmal_x11_sys_t *)vd->sys;
    vout_display_t * const x_vd = sys->cur_vout;
#if TRACE_ALL
    msg_Dbg(vd, "<<< %s", __func__);
#endif
    if (x_vd->prepare)
        x_vd->prepare(x_vd, pic, sub);
}

/* Display a picture and an optional subpicture (mandatory).
 *
 * The picture and the optional subpicture must be displayed as soon as
 * possible.
 * You cannot change the pixel content of the picture_t or of the
 * subpicture_t.
 *
 * This function gives away the ownership of the picture and of the
 * subpicture, so you must release them as soon as possible.
 */
static void mmal_x11_display(vout_display_t * vd, picture_t * pic, subpicture_t * sub)
{
    mmal_x11_sys_t * const sys = (mmal_x11_sys_t *)vd->sys;
    vout_display_t * const x_vd = sys->cur_vout;
    const bool is_mmal_pic = (pic->format.i_chroma == VLC_CODEC_MMAL_OPAQUE);

#if TRACE_ALL
    msg_Dbg(vd, "<<< %s: fmt: %dx%d/%dx%d, pic:%dx%d", __func__, vd->fmt.i_width, vd->fmt.i_height, x_vd->fmt.i_width, x_vd->fmt.i_height, pic->format.i_width, pic->format.i_height);
#endif

    if (sys->use_mmal != is_mmal_pic)  {
        msg_Dbg(vd, "%s: Picture dropped", __func__);
        picture_Release(pic);
        if (sub != NULL)
            subpicture_Delete(sub);
        return;
    }

    x_vd->display(x_vd, pic, sub);
}


static int vout_display_Control(vout_display_t *vd, int query, ...)
{
    va_list args;
    int result;

    va_start(args, query);
    result = vd->control(vd, query, args);
    va_end(args);

    return result;
}

/* Control on the module (mandatory) */
static int mmal_x11_control(vout_display_t * vd, int ctl, va_list va)
{
    mmal_x11_sys_t * const sys = (mmal_x11_sys_t *)vd->sys;
    vout_display_t *x_vd = sys->cur_vout;
    int rv;
#if TRACE_ALL
    msg_Dbg(vd, "<<< %s[%d] (ctl=%d)", __func__, sys->use_mmal, ctl);
#endif
    switch (ctl) {
        case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
        {
            const vout_display_cfg_t * cfg = va_arg(va, const vout_display_cfg_t *);
            const bool want_mmal = cfg->display.width == 1920;
            vout_display_t *new_vd = want_mmal ? sys->mmal_vout : sys->x_vout;

            msg_Dbg(vd, "Change size: %d, %d", cfg->display.width, cfg->display.height);

            if (sys->use_mmal != want_mmal) {
                if (sys->use_mmal) {
                    vout_display_Control(x_vd, VOUT_DISPLAY_CHANGE_MMAL_HIDE);
                }
                vout_display_SendEventPicturesInvalid(x_vd);
            }

            rv = vout_display_Control(new_vd, ctl, cfg);
            if (rv == VLC_SUCCESS) {
                vd->fmt       = new_vd->fmt;
                sys->cur_vout = new_vd;
                sys->use_mmal = want_mmal;
            }
            break;
        }
        case VOUT_DISPLAY_RESET_PICTURES:
            msg_Dbg(vd, "Reset pictures");
            rv = x_vd->control(x_vd, ctl, va);
            msg_Dbg(vd, "<<< %s: Pic reset: fmt: %dx%d<-%dx%d, source: %dx%d/%dx%d", __func__,
                    vd->fmt.i_width, vd->fmt.i_height, x_vd->fmt.i_width, x_vd->fmt.i_height,
                    vd->source.i_width, vd->source.i_height, x_vd->source.i_width, x_vd->source.i_height);
            vd->fmt       = x_vd->fmt;
            break;
        default:
            rv = x_vd->control(x_vd, ctl, va);
            vd->fmt  = x_vd->fmt;
            break;
    }
#if TRACE_ALL
    msg_Dbg(vd, ">>> %s (rv=%d)", __func__, rv);
#endif
    return rv;
}

#define DO_MANAGE 0

#if DO_MANAGE
/* Manage pending event (optional) */
static void mmal_x11_manage(vout_display_t * vd)
{
    mmal_x11_sys_t * const sys = (mmal_x11_sys_t *)vd->sys;
    vout_display_t * const x_vd = sys->cur_vout;
#if TRACE_ALL
    msg_Dbg(vd, "<<< %s", __func__);
#endif
    x_vd->manage(x_vd);
}
#endif



static int OpenMmalX11(vlc_object_t *object)
{
    vout_display_t * const vd = (vout_display_t *)object;
    mmal_x11_sys_t * const sys = calloc(1, sizeof(*sys));
    int ret = VLC_SUCCESS;

    if (sys == NULL) {
        return VLC_EGENERIC;
    }
    vd->sys = (vout_display_sys_t *)sys;

    if ((sys->mmal_vout = load_display_module(vd, sys, "vout display", "mmal_vout")) == NULL)
        goto fail;
    if ((sys->x_vout = load_display_module(vd, sys, "vout display", "xcb_x11")) == NULL)
        goto fail;

    sys->cur_vout = sys->x_vout;
    vd->info = sys->cur_vout->info;
    vd->fmt  = sys->cur_vout->fmt;

    vd->pool = mmal_x11_pool;
    vd->prepare = mmal_x11_prepare;
    vd->display = mmal_x11_display;
    vd->control = mmal_x11_control;
#if DO_MANAGE
    vd->manage = mmal_x11_manage;
#endif

    return VLC_SUCCESS;

fail:
    CloseMmalX11(VLC_OBJECT(vd));
    return ret == VLC_SUCCESS ? VLC_EGENERIC : ret;
}




vlc_module_begin()
    set_shortname(N_("MMAL x11 splitter"))
    set_description(N_("MMAL x11 splitter for Raspberry Pi"))
    set_capability("vout display", 900)
    add_shortcut("mmal_x11")
    set_category( CAT_VIDEO )
    set_subcategory( SUBCAT_VIDEO_VOUT )
    set_callbacks(OpenMmalX11, CloseMmalX11)
vlc_module_end()

