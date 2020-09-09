#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>

#include <vlc_common.h>
#include <vlc_codec.h>
#include <vlc_plugin.h>
#include <vlc_fourcc.h>
#include <vlc_picture.h>
#include <vlc_picture_pool.h>

#include <libavcodec/avcodec.h>

#include "avcodec.h"
#include "va.h"

typedef struct vlc_drm_prime_sys_s {
    vlc_video_context * vctx;
} vlc_drm_prime_sys_t;

static const AVCodecHWConfig* find_hw_config(const AVCodecContext * const ctx)
{
  const AVCodecHWConfig* config = NULL;
  for (int n = 0; (config = avcodec_get_hw_config(ctx->codec, n)); n++)
  {
    if (config->pix_fmt != AV_PIX_FMT_DRM_PRIME)
      continue;

    if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
        config->device_type == AV_HWDEVICE_TYPE_DRM)
      return config;

    if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_INTERNAL))
      return config;
  }

  return NULL;
}

static int DrmPrimeGet(vlc_va_t *va, picture_t *pic, uint8_t **data)
{
//    vlc_drm_prime_sys_t * const sys = va->sys;
    msg_Info(va, "%s", __func__);
#if 0
    vlc_va_surface_t *va_surface = va_pool_Get(sys->va_pool);
    if (unlikely(va_surface == NULL))
        return VLC_ENOITEM;
    vaapi_dec_pic_context *vaapi_ctx = malloc(sizeof(*vaapi_ctx));
    if (unlikely(vaapi_ctx == NULL))
    {
        va_surface_Release(va_surface);
        return VLC_ENOMEM;
    }
    vaapi_ctx->ctx.s = (picture_context_t) {
        vaapi_dec_pic_context_destroy, vaapi_dec_pic_context_copy,
        sys->vctx,
    };
    vaapi_ctx->ctx.surface = sys->render_targets[va_surface_GetIndex(va_surface)];
    vaapi_ctx->ctx.va_dpy = sys->hw_ctx.display;
    vaapi_ctx->va_surface = va_surface;
    vlc_vaapi_PicSetContext(pic, &vaapi_ctx->ctx);
    data[3] = (void *) (uintptr_t) vaapi_ctx->ctx.surface;

    return VLC_SUCCESS;
#else
    return VLC_EGENERIC;
#endif
}

static void DrmPrimeDelete(vlc_va_t *va)
{
    vlc_drm_prime_sys_t * const sys = (vlc_drm_prime_sys_t *)va->sys;

    if (!sys)
        return;

    va->sys = NULL;
    va->ops = NULL;
    if (sys->vctx)
        vlc_video_context_Release(sys->vctx);
//    va_pool_Close(sys->va_pool);
    free(sys);
}

// *** Probably wrong but it doesn't matter
#define VLC_TIME_BASE 1000000

static int DrmPrimeCreate(vlc_va_t *va, AVCodecContext *ctx, enum PixelFormat hwfmt, const AVPixFmtDescriptor *desc,
                  const es_format_t *fmt_in, vlc_decoder_device *dec_device,
                  video_format_t *fmt_out, vlc_video_context **vtcx_out)
{
    VLC_UNUSED(desc);

    msg_Err(va, "<<< %s: hwfmt=%d, dec_device=%p, type=%d", __func__, hwfmt, dec_device, dec_device ? (int)dec_device->type : -1);

    if ( hwfmt != AV_PIX_FMT_DRM_PRIME || dec_device == NULL ||
        dec_device->type != VLC_DECODER_DEVICE_DRM_PRIME)
        return VLC_EGENERIC;

    vlc_drm_prime_sys_t *sys = malloc(sizeof *sys);
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;
    va->sys = sys;
    memset(sys, 0, sizeof (*sys));

    const AVCodecHWConfig* pConfig = find_hw_config(ctx);

    if (pConfig && (pConfig->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
        pConfig->device_type == AV_HWDEVICE_TYPE_DRM)
    {
        if (av_hwdevice_ctx_create(&ctx->hw_device_ctx, AV_HWDEVICE_TYPE_DRM,
                                 NULL, NULL, 0) < 0)
        {
            msg_Err(va, "%s: unable to create hwdevice context", __func__);
            return VLC_EGENERIC;
        }
    }

#if 0
    m_pCodecContext->pix_fmt = AV_PIX_FMT_DRM_PRIME;
    m_pCodecContext->opaque = static_cast<void*>(this);
    m_pCodecContext->get_format = GetFormat;
    m_pCodecContext->get_buffer2 = GetBuffer;
#endif
    ctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
    ctx->time_base.num = 1;
    ctx->time_base.den = VLC_TIME_BASE;
    ctx->pkt_timebase.num = 1;
    ctx->pkt_timebase.den = VLC_TIME_BASE;

    if ((sys->vctx = vlc_video_context_Create(dec_device, VLC_VIDEO_CONTEXT_DRM_PRIME, 0, NULL)) == NULL)
        goto error;

    {
        static const struct vlc_va_operations ops = {
            .get = DrmPrimeGet,
            .close = DrmPrimeDelete
        };
        va->ops = &ops;
    }

    fmt_out->i_chroma = VLC_CODEC_DRM_PRIME_OPAQUE;

    *vtcx_out = sys->vctx;

    return VLC_SUCCESS;

error:
    DrmPrimeDelete(va);
    return VLC_EGENERIC;
}


static void
DrmPrimeDecoderDeviceClose(vlc_decoder_device *device)
{
    msg_Err(device, "<<< %s", __func__);
}

static const struct vlc_decoder_device_operations dev_ops = {
    .close = DrmPrimeDecoderDeviceClose,
};

static int
DrmPrimeDecoderDeviceOpen(vlc_decoder_device *device, vout_window_t *window)
{
    if (!window)
        return VLC_EGENERIC;

    msg_Err(device, "<<< %s", __func__);

    device->ops = &dev_ops;
    device->type = VLC_DECODER_DEVICE_DRM_PRIME;
    device->opaque = NULL;
    return VLC_SUCCESS;
}


vlc_module_begin ()
    set_description( N_("DRM-PRIME video decoder") )
    set_va_callback( DrmPrimeCreate, 100 )
    add_shortcut( "drm_prime" )
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_VCODEC )

    add_submodule()
    set_callback_dec_device(DrmPrimeDecoderDeviceOpen, 300)

vlc_module_end ()

