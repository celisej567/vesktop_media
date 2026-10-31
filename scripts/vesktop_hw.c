// Linked into libvesktop_ffmpeg.so (scripts/build-ffmpeg.sh) and compiled against its FFmpeg headers: the hardware
// frame plumbing VA-API encoding needs. vesktop_media uses FFmpeg through dlsym without its headers, and the
// VA-API encoders take frames in GPU surfaces only (AV_PIX_FMT_VAAPI). Exported with the av_ prefix (the library's
// version script exports av_* only).
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixfmt.h>

#include <stdio.h>
#include <string.h>

// Makes `ctx` a VA-API encoder of width x height on `device` (a DRM render node; NULL = libva's default): a device,
// a pool of NV12 surfaces, pix_fmt VAAPI. Before avcodec_open2. The driver's vendor string goes to `vendor` (for the
// log). 0 or a negative AVERROR
int av_vesktop_vaapi_open(AVCodecContext* ctx, int width, int height, const char* device, char* vendor, int vendorSize)
{
    AVBufferRef* dev = NULL;
    int rc = av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_VAAPI, device, NULL, 0);
    if (rc < 0) return rc;
    if (vendor && vendorSize > 0) {
        AVVAAPIDeviceContext* hw = ((AVHWDeviceContext*)dev->data)->hwctx;
        const char* v = vaQueryVendorString(hw->display);
        snprintf(vendor, vendorSize, "%s", v ? v : "unknown");
    }
    AVBufferRef* frames = av_hwframe_ctx_alloc(dev);
    av_buffer_unref(&dev);
    if (!frames) return AVERROR(ENOMEM);
    AVHWFramesContext* fc = (AVHWFramesContext*)frames->data;
    fc->format = AV_PIX_FMT_VAAPI;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = width;
    fc->height = height;
    fc->initial_pool_size = 8;
    if ((rc = av_hwframe_ctx_init(frames)) < 0) {
        av_buffer_unref(&frames);
        return rc;
    }
    av_buffer_unref(&ctx->hw_frames_ctx);
    ctx->hw_frames_ctx = frames;
    ctx->pix_fmt = AV_PIX_FMT_VAAPI;
    ctx->sw_pix_fmt = AV_PIX_FMT_NV12;
    return 0;
}

// Sends a YUV 4:2:0 frame (pts, pict_type and size set) to a VA-API encoder: interleaved into NV12, uploaded into a
// surface of the pool, sent. As avcodec_send_frame
int av_vesktop_vaapi_send(AVCodecContext* ctx, const AVFrame* src)
{
    if (!src) return avcodec_send_frame(ctx, NULL);
    AVFrame* nv12 = av_frame_alloc();
    AVFrame* hw = av_frame_alloc();
    int rc = AVERROR(ENOMEM);
    if (!nv12 || !hw) goto end;
    nv12->format = AV_PIX_FMT_NV12;
    nv12->width = src->width;
    nv12->height = src->height;
    if ((rc = av_frame_get_buffer(nv12, 0)) < 0) goto end;
    for (int y = 0; y < src->height; y++)
        memcpy(nv12->data[0] + y * nv12->linesize[0], src->data[0] + y * src->linesize[0], src->width);
    for (int y = 0; y < src->height / 2; y++) {
        const uint8_t* u = src->data[1] + y * src->linesize[1];
        const uint8_t* v = src->data[2] + y * src->linesize[2];
        uint8_t* uv = nv12->data[1] + y * nv12->linesize[1];
        for (int x = 0; x < src->width / 2; x++) {
            uv[2 * x] = u[x];
            uv[2 * x + 1] = v[x];
        }
    }
    if ((rc = av_hwframe_get_buffer(ctx->hw_frames_ctx, hw, 0)) < 0) goto end;
    if ((rc = av_hwframe_transfer_data(hw, nv12, 0)) < 0) goto end;
    av_frame_copy_props(hw, src); // pts, pict_type (a requested key frame)
    rc = avcodec_send_frame(ctx, hw);
end:
    av_frame_free(&nv12);
    av_frame_free(&hw);
    return rc;
}
