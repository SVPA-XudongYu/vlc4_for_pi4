/*****************************************************************************
 * mmal_picture.c: MMAL picture related shared functionality
 *****************************************************************************
 * Copyright © 2014 jusst technologies GmbH
 * $Id$
 *
 * Authors: Julian Scheel <julian@jusst.de>
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

// We would really like to use vlc_thread.h but the detach thread stuff can't be
// used here :-(
#include <pthread.h>

#include <stdatomic.h>

#include <vlc_common.h>
#include <vlc_picture.h>
#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_default_components.h>
#include <interface/vmcs_host/vcgencmd.h>
#include <interface/vcsm/user-vcsm.h>

#include "mmal_picture.h"


static void flush_range(void * start, size_t len)
{
    struct vcsm_user_clean_invalid2_s * b = calloc(1,
        sizeof(struct vcsm_user_clean_invalid2_s) + sizeof(struct vcsm_user_clean_invalid2_block_s));

    b->op_count = 1,

    b->s[0] = (struct vcsm_user_clean_invalid2_block_s){
        .invalidate_mode = 3,
        .block_count = 1,
        .start_address = start,
        .block_size = len,
        .inter_block_stride = 0
    };

    vcsm_clean_invalid2(b);

    free(b);
}

MMAL_FOURCC_T vlc_to_mmal_color_space(const video_color_space_t vlc_cs)
{
    switch (vlc_cs)
    {
        case COLOR_SPACE_BT601:
            return MMAL_COLOR_SPACE_ITUR_BT601;
        case COLOR_SPACE_BT709:
            return MMAL_COLOR_SPACE_ITUR_BT709;
        default:
            break;
    }
    return MMAL_COLOR_SPACE_UNKNOWN;
}

void vlc_to_mmal_video_fmt(MMAL_ES_FORMAT_T *const es_fmt, const video_frame_format_t * const vf_vlc)
{
    MMAL_VIDEO_FORMAT_T * const vf_mmal = &es_fmt->es->video;

    vf_mmal->width          = (vf_vlc->i_width + 31) & ~31;
    vf_mmal->height         = (vf_vlc->i_height + 15) & ~15;
    vf_mmal->crop.x         = vf_vlc->i_x_offset;
    vf_mmal->crop.y         = vf_vlc->i_y_offset;
    vf_mmal->crop.width     = vf_vlc->i_visible_width;
    vf_mmal->crop.height    = vf_vlc->i_visible_height;
    if (vf_vlc->i_sar_num == 0 || vf_vlc->i_sar_den == 0) {
        vf_mmal->par.num        = 1;
        vf_mmal->par.den        = 1;
    } else {
        vf_mmal->par.num        = vf_vlc->i_sar_num;
        vf_mmal->par.den        = vf_vlc->i_sar_den;
    }
    vf_mmal->frame_rate.num = vf_vlc->i_frame_rate;
    vf_mmal->frame_rate.den = vf_vlc->i_frame_rate_base;
    vf_mmal->color_space    = vlc_to_mmal_color_space(vf_vlc->space);
}

hw_mmal_port_pool_ref_t * hw_mmal_port_pool_ref_create(MMAL_PORT_T * const port,
   const unsigned int headers, const uint32_t payload_size)
{
    hw_mmal_port_pool_ref_t * ppr = calloc(1, sizeof(hw_mmal_port_pool_ref_t));
    if (ppr == NULL)
        return NULL;

    if ((ppr->pool = mmal_port_pool_create(port, headers, payload_size)) == NULL)
        goto fail;

    ppr->port = port;
    atomic_store(&ppr->refs, 1);
    return ppr;

fail:
    free(ppr);
    return NULL;
}

static void do_detached(void *(*fn)(void *), void * v)
{
    pthread_t dothread;
    pthread_create(&dothread, NULL, fn, v);
    pthread_detach(dothread);
}

// Destroy a ppr - aranged s.t. it has the correct prototype for a pthread
static void * kill_ppr(void * v)
{
    hw_mmal_port_pool_ref_t * const ppr = v;
    if (ppr->port->is_enabled)
        mmal_port_disable(ppr->port);  // Avoid annoyed messages from MMAL when we kill the pool
    mmal_port_pool_destroy(ppr->port, ppr->pool);
    free(ppr);
    return NULL;
}

void hw_mmal_port_pool_ref_release(hw_mmal_port_pool_ref_t * const ppr, const bool in_cb)
{
    if (ppr == NULL)
        return;
    if (atomic_fetch_sub(&ppr->refs, 1) != 1)
        return;
    if (in_cb)
        do_detached(kill_ppr, ppr);
    else
        kill_ppr(ppr);
}

// Put buffer in port if possible - if not then release to pool
// Returns true if sent, false if recycled
static bool hw_mmal_port_pool_ref_recycle(hw_mmal_port_pool_ref_t * const ppr, MMAL_BUFFER_HEADER_T * const buf)
{
    mmal_buffer_header_reset(buf);
    buf->user_data = NULL;

    if (mmal_port_send_buffer(ppr->port, buf) == MMAL_SUCCESS)
        return true;
    mmal_buffer_header_release(buf);
    return false;
}

MMAL_STATUS_T hw_mmal_port_pool_ref_fill(hw_mmal_port_pool_ref_t * const ppr)
{
    MMAL_BUFFER_HEADER_T * buf;
    MMAL_STATUS_T err = MMAL_SUCCESS;

    while ((buf = mmal_queue_get(ppr->pool->queue)) != NULL) {
        if ((err = mmal_port_send_buffer(ppr->port, buf)) != MMAL_SUCCESS)
        {
            mmal_queue_put_back(ppr->pool->queue, buf);
            break;
        }
    }
    return err;
}


MMAL_STATUS_T hw_mmal_opaque_output(vlc_object_t * const obj,
                                    hw_mmal_port_pool_ref_t ** pppr,
                                    MMAL_PORT_T * const port,
                                    const unsigned int extra_buffers, MMAL_PORT_BH_CB_T callback)
{
    MMAL_STATUS_T status;

    port->userdata = (struct MMAL_PORT_USERDATA_T *)obj;

    status = port_parameter_set_uint32(port, MMAL_PARAMETER_EXTRA_BUFFERS, extra_buffers);
    if (status != MMAL_SUCCESS) {
        msg_Err(obj, "Failed to set MMAL_PARAMETER_EXTRA_BUFFERS on output port (status=%"PRIx32" %s)",
                status, mmal_status_to_string(status));
        return status;
    }

    status = port_parameter_set_bool(port, MMAL_PARAMETER_ZERO_COPY, 1);
    if (status != MMAL_SUCCESS) {
       msg_Err(obj, "Failed to set zero copy on port %s (status=%"PRIx32" %s)",
                port->name, status, mmal_status_to_string(status));
       return status;
    }

    port->format->encoding = MMAL_ENCODING_OPAQUE;
    port->format->encoding_variant = 0;
    if ((status = mmal_port_format_commit(port)) != MMAL_SUCCESS)
    {
        msg_Err(obj, "Failed to commit format on port %s (status=%"PRIx32" %s)",
                 port->name, status, mmal_status_to_string(status));
        return status;
    }

    port->buffer_num = 30;
    port->buffer_size = port->buffer_size_recommended;

    if ((*pppr = hw_mmal_port_pool_ref_create(port, port->buffer_num, port->buffer_size)) == NULL) {
        msg_Err(obj, "Failed to create output pool");
        return status;
    }

    status = mmal_port_enable(port, callback);
    if (status != MMAL_SUCCESS) {
        msg_Err(obj, "Failed to enable output port %s (status=%"PRIx32" %s)",
                port->name, status, mmal_status_to_string(status));
        return status;
    }

    return MMAL_SUCCESS;
}


void hw_mmal_pic_ctx_destroy(picture_context_t * pic_ctx_cmn)
{
    pic_ctx_mmal_t * const ctx = (pic_ctx_mmal_t *)pic_ctx_cmn;
    unsigned int i;

    for (i = 0; i != ctx->buf_count; ++i) {
        mmal_buffer_header_release(ctx->bufs[i]);
    }
    free(ctx);
}

picture_context_t * hw_mmal_pic_ctx_copy(picture_context_t * pic_ctx_cmn)
{
    const pic_ctx_mmal_t * const src_ctx = (pic_ctx_mmal_t *)pic_ctx_cmn;
    pic_ctx_mmal_t * const dst_ctx = calloc(1, sizeof(*dst_ctx));
    unsigned int i;

    if (dst_ctx == NULL)
        return NULL;

    // Copy
    dst_ctx->cmn = src_ctx->cmn;

    dst_ctx->buf_count = src_ctx->buf_count;
    for (i = 0; i != src_ctx->buf_count; ++i) {
        dst_ctx->bufs[i] = src_ctx->bufs[i];
        mmal_buffer_header_acquire(dst_ctx->bufs[i]);
    }

    return &dst_ctx->cmn;
}

static MMAL_BOOL_T
buf_pre_release_cb(MMAL_BUFFER_HEADER_T * buf, void *userdata)
{
    hw_mmal_port_pool_ref_t * const ppr = userdata;

    // Kill the callback - otherwise we will go in circles!
    mmal_buffer_header_pre_release_cb_set(buf, (MMAL_BH_PRE_RELEASE_CB_T)0, NULL);
    mmal_buffer_header_acquire(buf);  // Ref it again

    // As we have re-acquired the buffer we need a full release
    // (not continue) to zap the ref count back to zero
    // This is "safe" 'cos we have already reset the cb
    hw_mmal_port_pool_ref_recycle(ppr, buf);
    hw_mmal_port_pool_ref_release(ppr, true); // Assume in callback

    return MMAL_TRUE;
}

// Buffer belongs to context on successful return from this fn
// is still valid on failure
picture_context_t *
hw_mmal_gen_context(const MMAL_FOURCC_T fmt, MMAL_BUFFER_HEADER_T * buf, hw_mmal_port_pool_ref_t * const ppr)
{
    pic_ctx_mmal_t * const ctx = calloc(1, sizeof(pic_ctx_mmal_t));

    if (ctx == NULL)
        return NULL;

    // If we have an associated ppr then ref & set appropriate callbacks
    if (ppr != NULL) {
        hw_mmal_port_pool_ref_acquire(ppr);
        mmal_buffer_header_pre_release_cb_set(buf, buf_pre_release_cb, ppr);
        buf->user_data = NULL;
    }

    ctx->cmn.copy = hw_mmal_pic_ctx_copy;
    ctx->cmn.destroy = hw_mmal_pic_ctx_destroy;

    ctx->fmt = fmt;
    ctx->buf_count = 1;
    ctx->bufs[0] = buf;

    return &ctx->cmn;
}

int hw_mmal_get_gpu_mem(void) {
    static int stashed_val = -2;
    VCHI_INSTANCE_T vchi_instance;
    VCHI_CONNECTION_T *vchi_connection = NULL;
    char rbuf[1024] = { 0 };

    if (stashed_val >= -1)
        return stashed_val;

    if (vchi_initialise(&vchi_instance) != 0)
        goto fail0;

    //create a vchi connection
    if (vchi_connect(NULL, 0, vchi_instance) != 0)
        goto fail0;

    vc_vchi_gencmd_init(vchi_instance, &vchi_connection, 1);

    //send the gencmd for the argument
    if (vc_gencmd_send("get_mem gpu") != 0)
        goto fail;

    if (vc_gencmd_read_response(rbuf, sizeof(rbuf) - 1) != 0)
        goto fail;

    if (strncmp(rbuf, "gpu=", 4) != 0)
        goto fail;

    char *p;
    unsigned long m = strtoul(rbuf + 4, &p, 10);

    if (p[0] != 'M' || p[1] != '\0')
        stashed_val = -1;
    else
        stashed_val = (int)m << 20;

    vc_gencmd_stop();

    //close the vchi connection
    vchi_disconnect(vchi_instance);

    return stashed_val;

fail:
    vc_gencmd_stop();
    vchi_disconnect(vchi_instance);
fail0:
    stashed_val = -1;
    return -1;
};

// ===========================================================================

typedef struct pool_ent_s
{
    struct pool_ent_s * next;
    struct pool_ent_s * prev;

    atomic_int ref_count;
    unsigned int seq;

    size_t size;

    int vcsm_hdl;
    int vc_hdl;
    void * buf;

    unsigned int width;
    unsigned int height;

    picture_t * pic;
} pool_ent_t;


typedef struct ent_list_hdr_s
{
    pool_ent_t * ents;
    pool_ent_t * tail;
    unsigned int n;
} ent_list_hdr_t;

#define ENT_LIST_HDR_INIT (ent_list_hdr_t){ \
   .ents = NULL, \
   .tail = NULL, \
   .n = 0 \
}

struct vzc_pool_ctl_s
{
    atomic_int ref_count;

    ent_list_hdr_t ent_pool;
    ent_list_hdr_t ents_cur;
    ent_list_hdr_t ents_prev;

    unsigned int max_n;
    unsigned int seq;

    vlc_mutex_t lock;

    MMAL_POOL_T * buf_pool;
};

typedef struct vzc_subbuf_ent_s
{
    pool_ent_t * ent;
    MMAL_RECT_T pic_rect;
    MMAL_RECT_T orig_dest_rect;
    MMAL_DISPLAYREGION_T dreg;
} vzc_subbuf_ent_t;


static pool_ent_t * ent_extract(ent_list_hdr_t * const elh, pool_ent_t * const ent)
{
//    printf("List %p [%d]: Ext %p\n", elh, elh->n, ent);

    if (ent == NULL)
        return NULL;

    if (ent->next == NULL)
        elh->tail = ent->prev;
    else
        ent->next->prev = ent->prev;

    if (ent->prev == NULL)
        elh->ents = ent->next;
    else
        ent->prev->next = ent->next;

    ent->prev = ent->next = NULL;

    --elh->n;

    return ent;  // For convienience
}

static inline pool_ent_t * ent_extract_tail(ent_list_hdr_t * const elh)
{
    return ent_extract(elh, elh->tail);
}

static void ent_add_head(ent_list_hdr_t * const elh, pool_ent_t * const ent)
{
//    printf("List %p [%d]: Add %p\n", elh, elh->n, ent);

    if ((ent->next = elh->ents) == NULL)
        elh->tail = ent;
    else
        ent->next->prev = ent;

    ent->prev = NULL;
    elh->ents = ent;
    ++elh->n;
}

static void ent_free(pool_ent_t * const ent)
{
//    printf("Free ent: %p\n", ent);
    if (ent != NULL) {
        // If we still have a ref to a pic - kill it now
        if (ent->pic != NULL)
            picture_Release(ent->pic);

        // Free contents
        vcsm_unlock_hdl(ent->vcsm_hdl);

        vcsm_free(ent->vcsm_hdl);

        free(ent);
    }
}

static void ent_free_list(ent_list_hdr_t * const elh)
{
    pool_ent_t * ent = elh->ents;

//    printf("Free list: %p [%d]\n", elh, elh->n);

    *elh = ENT_LIST_HDR_INIT;

    while (ent != NULL) {
        pool_ent_t * const t = ent;
        ent = t->next;
        ent_free(t);
    }
}

static void ent_list_move(ent_list_hdr_t * const dst, ent_list_hdr_t * const src)
{
//    printf("Move %p->%p\n", src, dst);

    *dst = *src;
    *src = ENT_LIST_HDR_INIT;
}

// Scans "backwards" as that should give us the fastest match if we are
// presented with pics in the same order each time
static pool_ent_t * ent_list_extract_pic_ent(ent_list_hdr_t * const elh, picture_t * const pic)
{
    pool_ent_t *ent = elh->tail;

//    printf("Find list: %p [%d]; pic:%p\n", elh, elh->n, pic);

    while (ent != NULL) {
//        printf("Check ent: %p, pic:%p\n", ent, ent->pic);

        if (ent->pic == pic)
            return ent_extract(elh, ent);
        ent = ent->prev;
    }
    return NULL;
}

#define POOL_ENT_ALLOC_BLOCK  0x10000

static pool_ent_t * pool_ent_alloc_new(size_t req_size)
{
    pool_ent_t * ent = calloc(1, sizeof(*ent));
    const size_t alloc_size = (req_size + POOL_ENT_ALLOC_BLOCK - 1) & ~(POOL_ENT_ALLOC_BLOCK - 1);

    if (ent == NULL)
        return NULL;

    ent->next = ent->prev = NULL;

    // Alloc from vcsm
    if ((ent->vcsm_hdl = vcsm_malloc_cache(alloc_size, VCSM_CACHE_TYPE_HOST, (char *)"vlc-subpic")) == -1)
        goto fail1;
    if ((ent->vc_hdl = vcsm_vc_hdl_from_hdl(ent->vcsm_hdl)) == 0)
        goto fail2;
    if ((ent->buf = vcsm_lock(ent->vcsm_hdl)) == NULL)
        goto fail2;

    ent->size = alloc_size;
    return ent;

fail2:
    vcsm_free(ent->vcsm_hdl);
fail1:
    free(ent);
    return NULL;
}

static inline pool_ent_t * pool_ent_ref(pool_ent_t * const ent)
{
//    int n = atomic_fetch_add(&ent->ref_count, 1) + 1;
//    printf("Ref: %p: %d\n", ent, n);
    atomic_fetch_add(&ent->ref_count, 1);
    return ent;
}

static void pool_recycle(vzc_pool_ctl_t * const pc, pool_ent_t * const ent)
{
    pool_ent_t * xs = NULL;
    int n;

    if (ent == NULL)
        return;

    n = atomic_fetch_sub(&ent->ref_count, 1) - 1;

//    printf("%s: Pool: %p: Ent: %p: %d\n", __func__, &pc->ent_pool, ent, n);

    if (n != 0)
        return;

    if (ent->pic != NULL) {
        picture_Release(ent->pic);
        ent->pic = NULL;
    }

    vlc_mutex_lock(&pc->lock);

    // If we have a full pool then extract the LRU and free it
    // Free done outside mutex
    if (pc->ent_pool.n >= pc->max_n)
        xs = ent_extract_tail(&pc->ent_pool);

    ent_add_head(&pc->ent_pool, ent);

    vlc_mutex_unlock(&pc->lock);

    ent_free(xs);
}

// * This could be made more efficient, but this is easy
static void pool_recycle_list(vzc_pool_ctl_t * const pc, ent_list_hdr_t * const elh)
{
    pool_ent_t * ent;
    while ((ent = ent_extract_tail(elh)) != NULL) {
        pool_recycle(pc, ent);
    }
}

static pool_ent_t * pool_best_fit(vzc_pool_ctl_t * const pc, size_t req_size)
{
    pool_ent_t * best = NULL;

    vlc_mutex_lock(&pc->lock);

    {
        pool_ent_t * ent = pc->ent_pool.ents;

        // Simple scan
        while (ent != NULL) {
            if (ent->size >= req_size && ent->size <= req_size * 2 + POOL_ENT_ALLOC_BLOCK &&
                    (best == NULL || best->size > ent->size))
                best = ent;
            ent = ent->next;
        }

        // extract best from chain if we've found it
        ent_extract(&pc->ent_pool, best);
    }

    vlc_mutex_unlock(&pc->lock);

    if (best == NULL)
        best = pool_ent_alloc_new(req_size);

    if ((best->seq = ++pc->seq) == 0)
        best->seq = ++pc->seq;  // Never allow to be zero

    atomic_store(&best->ref_count, 1);
    return best;
}


void hw_mmal_vzc_buf_get_wh(MMAL_BUFFER_HEADER_T * const buf, int * const pW, int * const pH)
{
    const pool_ent_t *const ent = ((vzc_subbuf_ent_t *)buf->user_data)->ent;
    *pW = ent->width;
    *pH = ent->height;
}

bool hw_mmal_vzc_buf_set_format(MMAL_BUFFER_HEADER_T * const buf, MMAL_ES_FORMAT_T * const es_fmt)
{
    const pool_ent_t *const ent = ((vzc_subbuf_ent_t *)buf->user_data)->ent;
    MMAL_VIDEO_FORMAT_T * const v_fmt = &es_fmt->es->video;

    es_fmt->type = MMAL_ES_TYPE_VIDEO;
    es_fmt->encoding = MMAL_ENCODING_BGRA;
    es_fmt->encoding_variant = MMAL_ENCODING_BGRA;

    v_fmt->width = ent->width;
    v_fmt->height = ent->height;
    v_fmt->crop.x = 0;
    v_fmt->crop.y = 0;
    v_fmt->crop.width = ent->width;
    v_fmt->crop.height = ent->height;

    return true;
}

void hw_mmal_vzc_buf_frame_size(MMAL_BUFFER_HEADER_T * const buf,
                                uint32_t * const pWidth, uint32_t * const pHeight)
{
    const pool_ent_t *const ent = ((vzc_subbuf_ent_t *)buf->user_data)->ent;
    *pWidth = ent->width;
    *pHeight = ent->height;
}


MMAL_DISPLAYREGION_T * hw_mmal_vzc_buf_region(MMAL_BUFFER_HEADER_T * const buf)
{
    vzc_subbuf_ent_t * sb = buf->user_data;
    return &sb->dreg;
}

void hw_mmal_vzc_buf_set_dest_rect(MMAL_BUFFER_HEADER_T * const buf, const int x, const int y, const int w, const int h)
{
    vzc_subbuf_ent_t * sb = buf->user_data;
    sb->orig_dest_rect.x = x;
    sb->orig_dest_rect.y = y;
    sb->orig_dest_rect.width = w;
    sb->orig_dest_rect.height = h;
}

static inline int rescale_x(int x, int mul, int div)
{
    return div == 0 ? x * mul : (x * mul + div/2) / div;
}

static void rescale_rect(MMAL_RECT_T * const d, const MMAL_RECT_T * const s, const MMAL_RECT_T * mul_rect, const MMAL_RECT_T * div_rect)
{
    d->x      = rescale_x(s->x,      mul_rect->width,  div_rect->width);
    d->y      = rescale_x(s->y,      mul_rect->height, div_rect->height);
    d->width  = rescale_x(s->width,  mul_rect->width,  div_rect->width);
    d->height = rescale_x(s->height, mul_rect->height, div_rect->height);
}

void hw_mmal_vzc_buf_scale_dest_rect(MMAL_BUFFER_HEADER_T * const buf, const MMAL_RECT_T * const scale_rect)
{
    vzc_subbuf_ent_t * sb = buf->user_data;
    if (scale_rect == NULL) {
        sb->dreg.dest_rect = sb->orig_dest_rect;
    }
    else
    {
        rescale_rect(&sb->dreg.dest_rect, &sb->orig_dest_rect,
                     scale_rect, &sb->pic_rect);
    }
}

unsigned int hw_mmal_vzc_buf_seq(MMAL_BUFFER_HEADER_T * const buf)
{
    vzc_subbuf_ent_t * sb = buf->user_data;
    return sb->ent->seq;
}


// The intent with the ents_cur & ents_last stuff is to remember the buffers
// we used on the last frame and reuse them on the current one if they are the
// same.  Unfortunately detection of "is_first" is only a heuristic (there are
// no rules governing the order in which things are blended) so we must deal
// (fairly) gracefully with it never (or always) being set.

MMAL_BUFFER_HEADER_T * hw_mmal_vzc_buf_from_pic(vzc_pool_ctl_t * const pc, picture_t * const pic, const picture_t * const dst_pic, const bool is_first)
{
    MMAL_BUFFER_HEADER_T * const buf = mmal_queue_get(pc->buf_pool->queue);
    vzc_subbuf_ent_t * sb;

    if (buf == NULL)
        return NULL;

    if ((sb = calloc(1, sizeof(*sb))) == NULL)
        goto fail1;

    // If first or we've had a lot of stuff move everything to the last list
    // (we could deal more gracefully with the "too many" case but it shouldn't
    // really happen)
    if (is_first || pc->ents_cur.n >= CTX_BUFS_MAX) {
        pool_recycle_list(pc, &pc->ents_prev);
        ent_list_move(&pc->ents_prev, &pc->ents_cur);
    }

    sb->dreg.hdr.id = MMAL_PARAMETER_DISPLAYREGION;
    sb->dreg.hdr.size = sizeof(sb->dreg);
    buf->user_data = sb;

    {
        // ?? Round start offset as well as length
        const video_format_t *const fmt = &pic->format;

        const unsigned int bpp = (fmt->i_bits_per_pixel + 7) >> 3;
        const unsigned int xl = (fmt->i_x_offset & ~15);
        const unsigned int xr = (fmt->i_x_offset + fmt->i_visible_width + 15) & ~15;
        const size_t dst_stride = (xr - xl) * bpp;
        const size_t dst_lines = ((fmt->i_visible_height + 15) & ~15);
        const size_t dst_size = dst_stride * dst_lines;

        pool_ent_t * ent = ent_list_extract_pic_ent(&pc->ents_prev, pic);

        // If we didn't find ent in last then look in cur in case is_first
        // isn't working
        if (ent == NULL)
            ent = ent_list_extract_pic_ent(&pc->ents_cur, pic);

//        printf("ent_found: %p\n", ent);

        if (ent == NULL)
        {
            if ((ent = pool_best_fit(pc, dst_size)) == NULL)
                goto fail2;
            ent->pic = picture_Hold(pic);
        }

        ent_add_head(&pc->ents_cur, ent);

        sb->ent = pool_ent_ref(ent);
        hw_mmal_vzc_pool_ref(pc);

        // Copy data
        buf->next = NULL;
        buf->cmd = 0;
        buf->data = (uint8_t *)(ent->vc_hdl);
        buf->alloc_size = buf->length = dst_size;
        buf->offset = 0;
        buf->flags = MMAL_BUFFER_HEADER_FLAG_FRAME_END;
        buf->pts = buf->dts = pic->date != VLC_TICK_INVALID ? pic->date : MMAL_TIME_UNKNOWN;
        buf->type->video = (MMAL_BUFFER_HEADER_VIDEO_SPECIFIC_T){
            .planes = 1,
            .pitch = { dst_stride }
        };

        // Remember offsets
        sb->dreg.set = MMAL_DISPLAY_SET_SRC_RECT;

//        printf("+++ bpp:%d, vis:%dx%d wxh:%dx%d, d:%dx%d\n", bpp, fmt->i_visible_width, fmt->i_visible_height, fmt->i_width, fmt->i_height, dst_stride, dst_lines);

        sb->dreg.src_rect = (MMAL_RECT_T){
            .x = (fmt->i_x_offset - xl),
            .y = 0,
            .width = fmt->i_visible_width,
            .height = fmt->i_visible_height
        };

        sb->pic_rect = (MMAL_RECT_T){
            .x      = dst_pic->format.i_x_offset,
            .y      = dst_pic->format.i_y_offset,
            .width  = dst_pic->format.i_visible_width,
            .height = dst_pic->format.i_visible_height
        };

        ent->width = dst_stride / bpp;
        ent->height = dst_lines;

        // 2D copy
        {
            unsigned int i;
            uint8_t *d = ent->buf;
            const uint8_t *s = pic->p[0].p_pixels + xl * bpp + fmt->i_y_offset * pic->p[0].i_pitch;
            for (i = 0; i != fmt->i_visible_height; ++i) {
                memcpy(d, s, dst_stride);
                d += dst_stride;
                s += pic->p[0].i_pitch;
            }

            // And make sure it is actually in memory
            flush_range(ent->buf, d - (uint8_t *)ent->buf);
        }
    }

    return buf;

fail2:
    free(sb);
fail1:
    mmal_buffer_header_release(buf);
    return NULL;
}

void hw_mmal_vzc_pool_flush(vzc_pool_ctl_t * const pc)
{
    pool_recycle_list(pc, &pc->ents_prev);
    pool_recycle_list(pc, &pc->ents_cur);
}

static void hw_mmal_vzc_pool_delete(vzc_pool_ctl_t * const pc)
{

//    printf("<<< %s\n", __func__);

    hw_mmal_vzc_pool_flush(pc);

    ent_free_list(&pc->ent_pool);

    if (pc->buf_pool != NULL)
        mmal_pool_destroy(pc->buf_pool);

    vlc_mutex_destroy(&pc->lock);

//    memset(pc, 0xba, sizeof(*pc)); // Zap for (hopefully) faster crash

    free (pc);

    vcsm_exit();

//    printf(">>> %s\n", __func__);
}

void hw_mmal_vzc_pool_release(vzc_pool_ctl_t * const pc)
{
    int n;

    if (pc == NULL)
        return;

    n = atomic_fetch_sub(&pc->ref_count, 1) - 1;

    if (n != 0)
        return;

    hw_mmal_vzc_pool_delete(pc);
}

void hw_mmal_vzc_pool_ref(vzc_pool_ctl_t * const pc)
{
    atomic_fetch_add(&pc->ref_count, 1);
}

static MMAL_BOOL_T vcz_pool_release_cb(MMAL_POOL_T * buf_pool, MMAL_BUFFER_HEADER_T *buf, void *userdata)
{
    vzc_pool_ctl_t * const pc = userdata;
    vzc_subbuf_ent_t * const sb = buf->user_data;

    VLC_UNUSED(buf_pool);

//    printf("<<< %s\n", __func__);

    if (sb != NULL) {
        buf->user_data = NULL;
        pool_recycle(pc, sb->ent);
        hw_mmal_vzc_pool_release(pc);
        free(sb);
    }

//    printf(">>> %s\n", __func__);

    return MMAL_TRUE;
}

vzc_pool_ctl_t * hw_mmal_vzc_pool_new()
{
    vzc_pool_ctl_t * const pc = calloc(1, sizeof(*pc));

    if (pc == NULL)
        return NULL;

    vcsm_init();

    pc->max_n = 8;
    vlc_mutex_init(&pc->lock);  // Must init before potential destruction

    if ((pc->buf_pool = mmal_pool_create(64, 0)) == NULL)
    {
        hw_mmal_vzc_pool_delete(pc);
        return NULL;
    }

    atomic_store(&pc->ref_count, 1);

    mmal_pool_callback_set(pc->buf_pool, vcz_pool_release_cb, pc);

    return pc;
}



