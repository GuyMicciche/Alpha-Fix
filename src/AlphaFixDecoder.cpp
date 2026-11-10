/******************************************************************************
 * AlphaFixDecoder.cpp — FFmpeg-based alpha channel decoder implementation
 *
 * This is the engine that reads the .mov file directly and extracts the
 * correct alpha channel that AE mangles on import.
 ******************************************************************************/

#include "AlphaFixDecoder.h"

// FFmpeg C headers
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <new>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <cstdio>

// ── Debug logging ────────────────────────────────────────────────────────────
// Controlled at runtime via Context::debugLog (toggled by AE checkbox).
// Writes to %TEMP%\AlphaFix_debug.log (Windows) or /tmp/AlphaFix_debug.log.

static FILE* GetLogFile() {
    static FILE* f = nullptr;
    if (!f) {
#ifdef _WIN32
        char path[512];
        const char* tmp = getenv("TEMP");
        if (!tmp) tmp = "C:\\Temp";
        snprintf(path, sizeof(path), "%s\\AlphaFix_debug.log", tmp);
        f = fopen(path, "w");
#else
        f = fopen("/tmp/AlphaFix_debug.log", "w");
#endif
    }
    return f;
}

static void CloseLogFile() {
    // Called on Destroy so the log flushes cleanly
    FILE* f = GetLogFile();
    if (f) {
        fflush(f);
    }
}

#define AFLOG(ctx, fmt, ...) do { \
    if ((ctx) && (ctx)->debugLog) { \
        FILE* _f = GetLogFile(); \
        if (_f) { fprintf(_f, fmt "\n", ##__VA_ARGS__); fflush(_f); } \
    } \
} while(0)

#ifdef _WIN32
    #include <windows.h>
    #define MUTEX_TYPE          CRITICAL_SECTION
    #define MUTEX_INIT(m)       InitializeCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(m))
    #define MUTEX_LOCK(m)       EnterCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(m))
    #define MUTEX_UNLOCK(m)     LeaveCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(m))
    #define MUTEX_DESTROY(m)    DeleteCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(m))
#else
    #include <pthread.h>
    #define MUTEX_TYPE          pthread_mutex_t
    #define MUTEX_INIT(m)       pthread_mutex_init(reinterpret_cast<pthread_mutex_t*>(m), nullptr)
    #define MUTEX_LOCK(m)       pthread_mutex_lock(reinterpret_cast<pthread_mutex_t*>(m))
    #define MUTEX_UNLOCK(m)     pthread_mutex_unlock(reinterpret_cast<pthread_mutex_t*>(m))
    #define MUTEX_DESTROY(m)    pthread_mutex_destroy(reinterpret_cast<pthread_mutex_t*>(m))
#endif


namespace AlphaFixDecoder {

// ═══════════════════════════════════════════════════════════════════════════════
// Create — Open file, find alpha stream, set up decoder
// ═══════════════════════════════════════════════════════════════════════════════

Context* Create(const char* filePath)
{
    if (!filePath || filePath[0] == '\0')
        return nullptr;

    Context* ctx = new (std::nothrow) Context();
    if (!ctx) return nullptr;

    memset(ctx, 0, sizeof(Context));
    strncpy(ctx->filePath, filePath, MAX_PATH_LENGTH - 1);

    // Initialize cache
    for (int i = 0; i < MAX_CACHE_FRAMES; i++) {
        ctx->cache[i].frameNumber = -1;
        ctx->cache[i].alphaData   = nullptr;
        ctx->cache[i].accessTime  = 0;
    }
    ctx->cacheAccessCounter = 0;

    // Initialize mutex
    ctx->mutex = malloc(sizeof(MUTEX_TYPE));
    if (ctx->mutex) {
        MUTEX_INIT(ctx->mutex);
    }

    // ── Open the file ──
    AVFormatContext* fmtCtx = nullptr;
    int ret = avformat_open_input(&fmtCtx, filePath, nullptr, nullptr);
    if (ret < 0) {
        Destroy(ctx);
        return nullptr;
    }
    ctx->formatCtx = fmtCtx;

    // Find stream info
    ret = avformat_find_stream_info(fmtCtx, nullptr);
    if (ret < 0) {
        Destroy(ctx);
        return nullptr;
    }

    // ── Find the video stream with alpha ──
    ctx->videoStreamIndex = Internal::FindAlphaVideoStream(fmtCtx);
    if (ctx->videoStreamIndex < 0) {
        // No alpha stream found — try the first video stream anyway
        // (we'll extract what we can)
        for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
            if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                ctx->videoStreamIndex = i;
                break;
            }
        }
        if (ctx->videoStreamIndex < 0) {
            Destroy(ctx);
            return nullptr;
        }
        ctx->hasAlpha = false;
    } else {
        ctx->hasAlpha = true;
    }

    AVStream* vStream = fmtCtx->streams[ctx->videoStreamIndex];

    // ── Set up the codec ──
    const AVCodec* codec = avcodec_find_decoder(vStream->codecpar->codec_id);
    if (!codec) {
        Destroy(ctx);
        return nullptr;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        Destroy(ctx);
        return nullptr;
    }
    ctx->codecCtx = codecCtx;

    ret = avcodec_parameters_to_context(codecCtx, vStream->codecpar);
    if (ret < 0) {
        Destroy(ctx);
        return nullptr;
    }

    // Request thread-safe decoding
    codecCtx->thread_count = 1;  // Single-threaded per context for safety

    ret = avcodec_open2(codecCtx, codec, nullptr);
    if (ret < 0) {
        Destroy(ctx);
        return nullptr;
    }

    // Store dimensions
    ctx->videoWidth  = codecCtx->width;
    ctx->videoHeight = codecCtx->height;

    // Calculate FPS — derive from actual frame count and duration when possible,
    // because r_frame_rate and avg_frame_rate can be wrong (doubled) for ProRes.
    // The nb_frames / duration method gives the true content frame rate.

    bool fpsResolved = false;

    // Method 1: nb_frames + stream duration → most reliable
    if (vStream->nb_frames > 1 && vStream->duration > 0 && vStream->time_base.den > 0) {
        double duration = vStream->duration * av_q2d(vStream->time_base);
        ctx->fps = static_cast<double>(vStream->nb_frames) / duration;
        ctx->totalFrames = vStream->nb_frames;
        // Compute clean rational: round fps to nearest integer if it's close
        double rounded = round(ctx->fps);
        if (fabs(ctx->fps - rounded) < 0.01) {
            ctx->fpsNum = static_cast<int32_t>(rounded);
            ctx->fpsDen = 1;
            ctx->fps = rounded;
        } else {
            // Non-integer fps (e.g. 29.97) — use 1001-style rational
            // Check for common NTSC rates
            if (fabs(ctx->fps - 23.976) < 0.01) {
                ctx->fpsNum = 24000; ctx->fpsDen = 1001;
            } else if (fabs(ctx->fps - 29.97) < 0.01) {
                ctx->fpsNum = 30000; ctx->fpsDen = 1001;
            } else if (fabs(ctx->fps - 59.94) < 0.01) {
                ctx->fpsNum = 60000; ctx->fpsDen = 1001;
            } else {
                // Fallback: use avg_frame_rate rational
                ctx->fpsNum = vStream->avg_frame_rate.num;
                ctx->fpsDen = vStream->avg_frame_rate.den;
            }
        }
        fpsResolved = true;
    }

    // Method 2: fall back to avg_frame_rate / r_frame_rate
    if (!fpsResolved) {
        if (vStream->avg_frame_rate.den > 0 && vStream->avg_frame_rate.num > 0) {
            ctx->fps = av_q2d(vStream->avg_frame_rate);
            ctx->fpsNum = vStream->avg_frame_rate.num;
            ctx->fpsDen = vStream->avg_frame_rate.den;
        } else if (vStream->r_frame_rate.den > 0 && vStream->r_frame_rate.num > 0) {
            ctx->fps = av_q2d(vStream->r_frame_rate);
            ctx->fpsNum = vStream->r_frame_rate.num;
            ctx->fpsDen = vStream->r_frame_rate.den;
        } else {
            ctx->fps = 24.0;
            ctx->fpsNum = 24;
            ctx->fpsDen = 1;
        }
    }

    // Estimate total frames if not already set
    if (!fpsResolved) {
        if (vStream->nb_frames > 0) {
            ctx->totalFrames = vStream->nb_frames;
        } else if (vStream->duration > 0 && vStream->time_base.den > 0) {
            double duration = vStream->duration * av_q2d(vStream->time_base);
            ctx->totalFrames = static_cast<int64_t>(duration * ctx->fps);
        } else {
            ctx->totalFrames = 0;
        }
    }

    // Allocate reusable frame and packet
    ctx->frame  = av_frame_alloc();
    ctx->packet = av_packet_alloc();
    if (!ctx->frame || !ctx->packet) {
        Destroy(ctx);
        return nullptr;
    }

    return ctx;
}


// ═══════════════════════════════════════════════════════════════════════════════
// Destroy — Clean up everything
// ═══════════════════════════════════════════════════════════════════════════════

void Destroy(Context* ctx)
{
    if (!ctx) return;

    // Free cache
    for (int i = 0; i < MAX_CACHE_FRAMES; i++) {
        if (ctx->cache[i].alphaData) {
            free(ctx->cache[i].alphaData);
            ctx->cache[i].alphaData = nullptr;
        }
    }

    // Free FFmpeg objects
    if (ctx->packet) {
        av_packet_free(reinterpret_cast<AVPacket**>(&ctx->packet));
    }
    if (ctx->frame) {
        av_frame_free(reinterpret_cast<AVFrame**>(&ctx->frame));
    }
    if (ctx->codecCtx) {
        avcodec_free_context(reinterpret_cast<AVCodecContext**>(&ctx->codecCtx));
    }
    if (ctx->formatCtx) {
        avformat_close_input(reinterpret_cast<AVFormatContext**>(&ctx->formatCtx));
    }

    // Free mutex
    if (ctx->mutex) {
        MUTEX_DESTROY(ctx->mutex);
        free(ctx->mutex);
    }

    delete ctx;
}


// ═══════════════════════════════════════════════════════════════════════════════
// DecodeAlpha — The main function: get the alpha for a specific frame
// ═══════════════════════════════════════════════════════════════════════════════

PF_Err DecodeAlpha(
    Context*    ctx,
    int32_t     frameNumber,
    uint8_t**   outAlpha,
    int32_t*    outWidth,
    int32_t*    outHeight)
{
    if (!ctx || !outAlpha || !outWidth || !outHeight)
        return PF_Err_BAD_CALLBACK_PARAM;

    *outAlpha  = nullptr;
    *outWidth  = 0;
    *outHeight = 0;

    if (ctx->mutex) MUTEX_LOCK(ctx->mutex);

    // ── Check cache first ──
    for (int i = 0; i < MAX_CACHE_FRAMES; i++) {
        if (ctx->cache[i].frameNumber == frameNumber && ctx->cache[i].alphaData) {
            // Cache hit!
            AFLOG(ctx, "[frame %d] CACHE HIT (slot %d)", frameNumber, i);
            ctx->cache[i].accessTime = ++ctx->cacheAccessCounter;

            // Return a copy so the caller can safely use it
            size_t bufSize = ctx->cache[i].width * ctx->cache[i].height;
            uint8_t* copy = static_cast<uint8_t*>(malloc(bufSize));
            if (copy) {
                memcpy(copy, ctx->cache[i].alphaData, bufSize);
                *outAlpha  = copy;
                *outWidth  = ctx->cache[i].width;
                *outHeight = ctx->cache[i].height;
            }

            if (ctx->mutex) MUTEX_UNLOCK(ctx->mutex);
            return copy ? PF_Err_NONE : PF_Err_OUT_OF_MEMORY;
        }
    }

    // ── Cache miss: decode the frame ──

    AVFormatContext* fmtCtx  = reinterpret_cast<AVFormatContext*>(ctx->formatCtx);
    AVCodecContext*  codecCtx = reinterpret_cast<AVCodecContext*>(ctx->codecCtx);
    AVFrame*         frame   = reinterpret_cast<AVFrame*>(ctx->frame);
    AVPacket*        packet  = reinterpret_cast<AVPacket*>(ctx->packet);

    // Flush codec buffers first, then seek once
    avcodec_flush_buffers(codecCtx);

    if (!Internal::SeekToFrame(ctx, frameNumber)) {
        AFLOG(ctx, "[frame %d] SeekToFrame FAILED", frameNumber);
        if (ctx->mutex) MUTEX_UNLOCK(ctx->mutex);
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    // Decode frames until we hit the target
    bool decoded = false;
    int  framesDecoded = 0;
    const int MAX_DECODE_ATTEMPTS = 300;  // Safety limit

    // Compute target PTS in stream time_base units using rational math (no floats)
    // Use ctx->fpsNum/fpsDen which is the verified frame rate (not r_frame_rate which can be wrong)
    AVStream* vStream = fmtCtx->streams[ctx->videoStreamIndex];
    AVRational fpsRational = { ctx->fpsNum, ctx->fpsDen };
    int64_t targetPts = av_rescale_q(
        frameNumber,
        av_inv_q(fpsRational),
        vStream->time_base);

    // Also compute one frame duration in stream time_base for tolerance checking
    int64_t frameDurationPts = av_rescale_q(
        1,
        av_inv_q(fpsRational),
        vStream->time_base);

    AFLOG(ctx, "========================================");
    AFLOG(ctx, "[frame %d] DECODE REQUEST", frameNumber);
    AFLOG(ctx, "  targetPts      = %lld", (long long)targetPts);
    AFLOG(ctx, "  frameDuration  = %lld", (long long)frameDurationPts);
    AFLOG(ctx, "  window         = [%lld, %lld)", (long long)targetPts, (long long)(targetPts + frameDurationPts));
    AFLOG(ctx, "  fpsRational    = %d/%d", ctx->fpsNum, ctx->fpsDen);
    AFLOG(ctx, "  r_frame_rate   = %d/%d", vStream->r_frame_rate.num, vStream->r_frame_rate.den);
    AFLOG(ctx, "  time_base      = %d/%d", vStream->time_base.num, vStream->time_base.den);
    AFLOG(ctx, "  fps (double)   = %.10f", ctx->fps);

    while (framesDecoded < MAX_DECODE_ATTEMPTS) {
        int ret = av_read_frame(fmtCtx, packet);
        if (ret < 0) {
            AFLOG(ctx, "[frame %d] av_read_frame returned %d (EOF/error) after %d decoded", frameNumber, ret, framesDecoded);
            break;
        }

        if (packet->stream_index != ctx->videoStreamIndex) {
            av_packet_unref(packet);
            continue;
        }

        AFLOG(ctx, "[frame %d]   packet PTS=%lld DTS=%lld", frameNumber, (long long)packet->pts, (long long)packet->dts);

        ret = avcodec_send_packet(codecCtx, packet);
        av_packet_unref(packet);
        if (ret < 0) {
            AFLOG(ctx, "[frame %d]   send_packet error: %d", frameNumber, ret);
            continue;
        }

        while (ret >= 0) {
            ret = avcodec_receive_frame(codecCtx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            int64_t framePts = frame->pts;

            AFLOG(ctx, "[frame %d]   decoded frame PTS=%lld  (target window [%lld, %lld))",
                  frameNumber, (long long)framePts,
                  (long long)targetPts, (long long)(targetPts + frameDurationPts));

            // Accept this frame if its PTS is within [targetPts, targetPts + frameDuration).
            // This ensures we get exactly the right frame — not the one before or after.
            if (framePts >= targetPts && framePts < targetPts + frameDurationPts) {
                AFLOG(ctx, "[frame %d]   >>> MATCH! Using this frame (PTS %lld)", frameNumber, (long long)framePts);

                // Extract alpha from this frame
                uint8_t* alpha = Internal::ExtractAlphaFromFrame(
                    frame, ctx->videoWidth, ctx->videoHeight);

                if (alpha) {
                    // Store in cache
                    CachedFrame* slot = Internal::FindOrAllocCacheSlot(ctx, frameNumber);
                    if (slot) {
                        if (slot->alphaData) free(slot->alphaData);
                        slot->frameNumber = frameNumber;
                        slot->alphaData   = alpha;
                        slot->width       = ctx->videoWidth;
                        slot->height      = ctx->videoHeight;
                        slot->accessTime  = ++ctx->cacheAccessCounter;
                    }

                    // Return a copy
                    size_t bufSize = ctx->videoWidth * ctx->videoHeight;
                    uint8_t* copy = static_cast<uint8_t*>(malloc(bufSize));
                    if (copy) {
                        memcpy(copy, alpha, bufSize);
                        *outAlpha  = copy;
                        *outWidth  = ctx->videoWidth;
                        *outHeight = ctx->videoHeight;
                        decoded = true;
                    }
                }

                av_frame_unref(frame);
                break;
            }

            // If we've overshot the target (past the window), stop — don't keep decoding
            if (framePts >= targetPts + frameDurationPts) {
                AFLOG(ctx, "[frame %d]   >>> OVERSHOT! frame PTS %lld >= window end %lld — stopping",
                      frameNumber, (long long)framePts, (long long)(targetPts + frameDurationPts));
                av_frame_unref(frame);
                break;
            }

            AFLOG(ctx, "[frame %d]   skipping frame PTS %lld (before target)", frameNumber, (long long)framePts);
            av_frame_unref(frame);
            framesDecoded++;
        }

        if (decoded) break;
        framesDecoded++;
    }

    AFLOG(ctx, "[frame %d] RESULT: %s (decoded %d frames in loop)",
          frameNumber, decoded ? "SUCCESS" : "FAILED", framesDecoded);

    if (ctx->mutex) MUTEX_UNLOCK(ctx->mutex);

    return decoded ? PF_Err_NONE : PF_Err_INTERNAL_STRUCT_DAMAGED;
}


// ═══════════════════════════════════════════════════════════════════════════════
// FreeAlphaBuffer
// ═══════════════════════════════════════════════════════════════════════════════

void FreeAlphaBuffer(uint8_t* buffer)
{
    if (buffer) {
        free(buffer);
    }
}


// ═══════════════════════════════════════════════════════════════════════════════
// GetFileInfo — Quick probe without creating a full context
// ═══════════════════════════════════════════════════════════════════════════════

bool GetFileInfo(
    const char* filePath,
    int32_t*    outWidth,
    int32_t*    outHeight,
    double*     outFps,
    int64_t*    outTotalFrames,
    bool*       outHasAlpha)
{
    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, filePath, nullptr, nullptr) < 0)
        return false;

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    int videoIdx = Internal::FindAlphaVideoStream(fmtCtx);
    bool hasAlpha = (videoIdx >= 0);

    if (videoIdx < 0) {
        // Find any video stream
        for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
            if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                videoIdx = i;
                break;
            }
        }
    }

    if (videoIdx < 0) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    AVStream* vStream = fmtCtx->streams[videoIdx];

    if (outWidth)       *outWidth  = vStream->codecpar->width;
    if (outHeight)      *outHeight = vStream->codecpar->height;
    if (outHasAlpha)    *outHasAlpha = hasAlpha;

    if (outFps) {
        // Derive fps from nb_frames / duration when possible (most reliable)
        if (vStream->nb_frames > 1 && vStream->duration > 0 && vStream->time_base.den > 0) {
            double duration = vStream->duration * av_q2d(vStream->time_base);
            double fps = static_cast<double>(vStream->nb_frames) / duration;
            double rounded = round(fps);
            *outFps = (fabs(fps - rounded) < 0.01) ? rounded : fps;
        } else if (vStream->avg_frame_rate.den > 0) {
            *outFps = av_q2d(vStream->avg_frame_rate);
        } else {
            *outFps = 24.0;
        }
    }

    if (outTotalFrames) {
        if (vStream->nb_frames > 0)
            *outTotalFrames = vStream->nb_frames;
        else
            *outTotalFrames = 0;
    }

    avformat_close_input(&fmtCtx);
    return true;
}


// ═══════════════════════════════════════════════════════════════════════════════
// Internal Helpers
// ═══════════════════════════════════════════════════════════════════════════════

namespace Internal {

int FindAlphaVideoStream(void* formatCtx)
{
    AVFormatContext* fmtCtx = reinterpret_cast<AVFormatContext*>(formatCtx);

    for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
        AVStream* stream = fmtCtx->streams[i];
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            continue;

        // Check if the pixel format has an alpha channel
        enum AVPixelFormat pixFmt =
            static_cast<enum AVPixelFormat>(stream->codecpar->format);

        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pixFmt);
        if (desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA)) {
            return i;
        }

        // ProRes 4444 sometimes doesn't report alpha in the stream params
        // but the codec_tag indicates it. Check for 'ap4h' (ProRes 4444 with alpha)
        if (stream->codecpar->codec_id == AV_CODEC_ID_PRORES) {
            uint32_t tag = stream->codecpar->codec_tag;
            // ap4h = ProRes 4444 with alpha, ap4x = ProRes 4444 XQ with alpha
            if (tag == MKTAG('a','p','4','h') || tag == MKTAG('a','p','4','x')) {
                return i;
            }
        }
    }

    return -1;
}


uint8_t* ExtractAlphaFromFrame(
    void*       avFrame,
    int32_t     width,
    int32_t     height)
{
    AVFrame* frame = reinterpret_cast<AVFrame*>(avFrame);
    if (!frame || width <= 0 || height <= 0)
        return nullptr;

    size_t bufSize = static_cast<size_t>(width) * height;
    uint8_t* alpha = static_cast<uint8_t*>(malloc(bufSize));
    if (!alpha) return nullptr;

    enum AVPixelFormat pixFmt = static_cast<enum AVPixelFormat>(frame->format);

    // ── Handle different pixel formats ──

    switch (pixFmt) {
        case AV_PIX_FMT_YUVA444P10LE:
        case AV_PIX_FMT_YUVA444P10BE: {
            // 10-bit alpha in plane 3, stored as uint16_t
            // We need to scale 10-bit (0-1023) down to 8-bit (0-255)
            if (frame->data[3]) {
                for (int32_t y = 0; y < height; y++) {
                    uint16_t* srcRow = reinterpret_cast<uint16_t*>(
                        frame->data[3] + y * frame->linesize[3]);
                    uint8_t*  dstRow = alpha + y * width;

                    for (int32_t x = 0; x < width; x++) {
                        uint16_t val = srcRow[x];
                        // Scale 10-bit to 8-bit
                        dstRow[x] = static_cast<uint8_t>((val * 255 + 512) / 1023);
                    }
                }
            } else {
                // No alpha plane — fill with opaque
                memset(alpha, 255, bufSize);
            }
            break;
        }

        case AV_PIX_FMT_YUVA444P: {
            // 8-bit alpha in plane 3
            if (frame->data[3]) {
                for (int32_t y = 0; y < height; y++) {
                    uint8_t* srcRow = frame->data[3] + y * frame->linesize[3];
                    uint8_t* dstRow = alpha + y * width;
                    memcpy(dstRow, srcRow, width);
                }
            } else {
                memset(alpha, 255, bufSize);
            }
            break;
        }

        case AV_PIX_FMT_YUVA422P:
        case AV_PIX_FMT_YUVA420P: {
            // Alpha plane exists but may be subsampled differently
            if (frame->data[3]) {
                for (int32_t y = 0; y < height; y++) {
                    uint8_t* srcRow = frame->data[3] + y * frame->linesize[3];
                    uint8_t* dstRow = alpha + y * width;
                    memcpy(dstRow, srcRow, width);
                }
            } else {
                memset(alpha, 255, bufSize);
            }
            break;
        }

        case AV_PIX_FMT_YUVA444P16LE:
        case AV_PIX_FMT_YUVA444P16BE: {
            // 16-bit alpha in plane 3
            if (frame->data[3]) {
                for (int32_t y = 0; y < height; y++) {
                    uint16_t* srcRow = reinterpret_cast<uint16_t*>(
                        frame->data[3] + y * frame->linesize[3]);
                    uint8_t*  dstRow = alpha + y * width;

                    for (int32_t x = 0; x < width; x++) {
                        dstRow[x] = static_cast<uint8_t>(srcRow[x] >> 8);
                    }
                }
            } else {
                memset(alpha, 255, bufSize);
            }
            break;
        }

        case AV_PIX_FMT_RGBA:
        case AV_PIX_FMT_BGRA: {
            // Interleaved RGBA/BGRA — alpha is the 4th byte
            if (frame->data[0]) {
                for (int32_t y = 0; y < height; y++) {
                    uint8_t* srcRow = frame->data[0] + y * frame->linesize[0];
                    uint8_t* dstRow = alpha + y * width;

                    for (int32_t x = 0; x < width; x++) {
                        dstRow[x] = srcRow[x * 4 + 3];
                    }
                }
            } else {
                memset(alpha, 255, bufSize);
            }
            break;
        }

        case AV_PIX_FMT_RGBA64LE:
        case AV_PIX_FMT_RGBA64BE: {
            // 16-bit interleaved RGBA
            if (frame->data[0]) {
                for (int32_t y = 0; y < height; y++) {
                    uint16_t* srcRow = reinterpret_cast<uint16_t*>(
                        frame->data[0] + y * frame->linesize[0]);
                    uint8_t*  dstRow = alpha + y * width;

                    for (int32_t x = 0; x < width; x++) {
                        dstRow[x] = static_cast<uint8_t>(srcRow[x * 4 + 3] >> 8);
                    }
                }
            } else {
                memset(alpha, 255, bufSize);
            }
            break;
        }

        default: {
            // Unknown format — check if the pixel format descriptor says it has alpha
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pixFmt);
            if (desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA) && frame->data[3]) {
                // Try treating plane 3 as 8-bit alpha
                int bitsPerComp = desc->comp[3].depth;
                for (int32_t y = 0; y < height; y++) {
                    if (bitsPerComp <= 8) {
                        uint8_t* srcRow = frame->data[3] + y * frame->linesize[3];
                        uint8_t* dstRow = alpha + y * width;
                        memcpy(dstRow, srcRow, width);
                    } else {
                        // Assume 16-bit and scale
                        uint16_t* srcRow = reinterpret_cast<uint16_t*>(
                            frame->data[3] + y * frame->linesize[3]);
                        uint8_t*  dstRow = alpha + y * width;
                        uint16_t maxVal = (1 << bitsPerComp) - 1;
                        for (int32_t x = 0; x < width; x++) {
                            dstRow[x] = static_cast<uint8_t>(
                                (static_cast<uint32_t>(srcRow[x]) * 255 + maxVal/2) / maxVal);
                        }
                    }
                }
            } else {
                // No alpha — fill opaque
                memset(alpha, 255, bufSize);
            }
            break;
        }
    }

    return alpha;
}


bool SeekToFrame(Context* ctx, int32_t frameNumber)
{
    AVFormatContext* fmtCtx = reinterpret_cast<AVFormatContext*>(ctx->formatCtx);
    AVStream* vStream = fmtCtx->streams[ctx->videoStreamIndex];

    // Convert frame number to stream time_base using pure rational math — no floats.
    // Use ctx->fpsNum/fpsDen (verified frame rate) instead of r_frame_rate.
    AVRational fpsRational = { ctx->fpsNum, ctx->fpsDen };
    int64_t timestamp = av_rescale_q(
        frameNumber,
        av_inv_q(fpsRational),
        vStream->time_base);

    AFLOG(ctx, "[frame %d] SeekToFrame: timestamp=%lld (time_base %d/%d, fps %d/%d)",
          frameNumber, (long long)timestamp,
          vStream->time_base.num, vStream->time_base.den,
          ctx->fpsNum, ctx->fpsDen);

    // Seek backward to nearest keyframe before our target
    int ret = av_seek_frame(
        fmtCtx,
        ctx->videoStreamIndex,
        timestamp,
        AVSEEK_FLAG_BACKWARD);

    AFLOG(ctx, "[frame %d] SeekToFrame: av_seek_frame returned %d", frameNumber, ret);
    return (ret >= 0);
}


CachedFrame* FindOrAllocCacheSlot(Context* ctx, int32_t frameNumber)
{
    // First, check if there's already a slot for this frame
    for (int i = 0; i < MAX_CACHE_FRAMES; i++) {
        if (ctx->cache[i].frameNumber == frameNumber) {
            return &ctx->cache[i];
        }
    }

    // Find an empty slot
    for (int i = 0; i < MAX_CACHE_FRAMES; i++) {
        if (ctx->cache[i].frameNumber < 0) {
            return &ctx->cache[i];
        }
    }

    // All slots full — evict LRU
    int lruIdx = 0;
    uint64_t minTime = ctx->cache[0].accessTime;
    for (int i = 1; i < MAX_CACHE_FRAMES; i++) {
        if (ctx->cache[i].accessTime < minTime) {
            minTime = ctx->cache[i].accessTime;
            lruIdx = i;
        }
    }

    // Free the evicted slot's data
    if (ctx->cache[lruIdx].alphaData) {
        free(ctx->cache[lruIdx].alphaData);
        ctx->cache[lruIdx].alphaData = nullptr;
    }
    ctx->cache[lruIdx].frameNumber = -1;

    return &ctx->cache[lruIdx];
}

}  // namespace Internal
}  // namespace AlphaFixDecoder