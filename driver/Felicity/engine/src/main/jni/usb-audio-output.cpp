/**
 * @file usb-audio-output.cpp
 * @brief Direct USB audio output — V4 (minimal working version)
 *
 * Based on the version that produced sound (grunido), with sine wave
 * replaced by real ExoPlayer data. Key insight: the xHCI host controller
 * needs multiple URBs in flight (pipeline). Single submit-reap produces
 * silence because #Iso=0 between URBs.
 */

#include "usb-audio-output.h"

#include <jni.h>
#include <android/log.h>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

#ifndef USBDEVFS_URB_ISO_ASAP
#define USBDEVFS_URB_ISO_ASAP 0x02
#endif

#define TAG "UsbAudioOutput"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Float → PCM conversion
static inline float clampf(float v) { return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v); }

static void convertFloatToInt16(const float *src, uint8_t *dst, int n) {
    auto *out = reinterpret_cast<int16_t *>(dst);
    for (int i = 0; i < n; i++) out[i] = (int16_t)(clampf(src[i]) * 32767.0f);
}
static void convertFloatToInt24(const float *src, uint8_t *dst, int n) {
    for (int i = 0; i < n; i++) {
        int32_t s = (int32_t)(clampf(src[i]) * 8388607.0f);
        dst[i*3] = s & 0xFF; dst[i*3+1] = (s>>8) & 0xFF; dst[i*3+2] = (s>>16) & 0xFF;
    }
}
static void convertFloatToInt32(const float *src, uint8_t *dst, int n) {
    auto *out = reinterpret_cast<int32_t *>(dst);
    for (int i = 0; i < n; i++) out[i] = (int32_t)(clampf(src[i]) * 2147483647.0f);
}

// USB helpers
static bool setAlternateSetting(int fd, int ifId, int alt) {
    struct usbdevfs_setinterface si = {};
    si.interface = (unsigned)ifId;
    si.altsetting = (unsigned)alt;
    int r = ioctl(fd, USBDEVFS_SETINTERFACE, &si);
    LOGI("setAlt(%d,%d): ret=%d errno=%d", ifId, alt, r, errno);
    return r >= 0;
}
static void releaseInterface(int fd, int ifId) {
    ioctl(fd, USBDEVFS_RELEASEINTERFACE, &ifId);
}
static double readFeedback(int fd, int ep) {
    uint8_t fb[4] = {};
    size_t sz = sizeof(struct usbdevfs_urb) + sizeof(struct usbdevfs_iso_packet_desc);
    auto *u = (struct usbdevfs_urb *)calloc(1, sz);
    if (!u) return 0;
    u->type = USBDEVFS_URB_TYPE_ISO;
    u->flags = USBDEVFS_URB_ISO_ASAP;
    u->endpoint = (unsigned char)ep;
    u->buffer = fb;
    u->buffer_length = 4;
    u->number_of_packets = 1;
    u->iso_frame_desc[0].length = 4;
    if (ioctl(fd, USBDEVFS_SUBMITURB, u) < 0) { free(u); return 0; }
    struct usbdevfs_urb *c = nullptr;
    usleep(2000);
    if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &c) < 0) {
        ioctl(fd, USBDEVFS_DISCARDURB, u);
        ioctl(fd, USBDEVFS_REAPURBNDELAY, &c);
        free(u); return 0;
    }
    double r = 0;
    if (u->iso_frame_desc[0].actual_length >= 4) {
        uint32_t raw = fb[0] | (fb[1]<<8) | (fb[2]<<16) | (fb[3]<<24);
        r = raw / 65536.0;
    }
    free(u);
    return r;
}

/**
 * Submit one ISO URB. Does NOT reap — caller manages the pipeline.
 * The data buffer is owned by the URB until reaped.
 */
static int submitIsoUrb(int fd, int ep, uint8_t *data, const int *pktSizes, int numPkts) {
    int totalBytes = 0;
    for (int i = 0; i < numPkts; i++) totalBytes += pktSizes[i];

    size_t sz = sizeof(struct usbdevfs_urb) + numPkts * sizeof(struct usbdevfs_iso_packet_desc);
    auto *u = (struct usbdevfs_urb *)calloc(1, sz);
    if (!u) return -1;

    u->type = USBDEVFS_URB_TYPE_ISO;
    u->flags = USBDEVFS_URB_ISO_ASAP;
    u->endpoint = (unsigned char)ep;
    u->buffer = data;
    u->buffer_length = totalBytes;
    u->number_of_packets = numPkts;
    for (int i = 0; i < numPkts; i++)
        u->iso_frame_desc[i].length = (unsigned)pktSizes[i];

    int ret = ioctl(fd, USBDEVFS_SUBMITURB, u);
    if (ret < 0) {
        LOGE("SUBMITURB FAILED ep=0x%02x pkts=%d bytes=%d errno=%d (%s)",
             ep, numPkts, totalBytes, errno, strerror(errno));
        free(u);
        return -1;
    }
    // URB now owned by kernel — don't free until reaped
    return totalBytes;
}

/**
 * Reap one completed URB (blocking). Frees both URB and its data buffer.
 * Returns 0 on success, -1 on error.
 */
static int reapOneUrb(int fd) {
    struct usbdevfs_urb *c = nullptr;
    int ret = ioctl(fd, USBDEVFS_REAPURB, &c);
    if (ret < 0) return -1;
    if (c) { free(c->buffer); free(c); }
    return 0;
}

extern "C" {

JNIEXPORT jlong JNICALL
Java_app_simple_felicity_engine_processors_UsbAudioOutputProcessor_nativeUsbAudioCreate(
        JNIEnv *, jobject, jint fd, jint ifId, jint epOut, jint epFb,
        jint rate, jint ch, jint bits, jint maxPkt) {
    LOGI("Create: fd=%d ep=0x%02x rate=%d ch=%d bits=%d", fd, epOut, rate, ch, bits);
    auto *ctx = new(std::nothrow) UsbAudioContext();
    if (!ctx) return 0;
    ctx->fd = fd;
    ctx->interfaceId = ifId;
    ctx->endpointOut = epOut;
    ctx->endpointFeedback = epFb;
    ctx->sampleRate = rate;
    ctx->channelCount = ch;
    ctx->bitDepth = bits;
    ctx->bytesPerSample = bits / 8;
    ctx->bytesPerFrame = (bits / 8) * ch;
    ctx->maxPacketSize = maxPkt;
    ctx->running.store(false);
    ctx->transferBuffer = nullptr;
    ctx->transferBufferCapacity = 0;
    ctx->framesWritten = 0;
    ctx->interfaceClaimed = true;
    ctx->urbsInFlight = 0;
    return reinterpret_cast<jlong>(ctx);
}

JNIEXPORT jboolean JNICALL
Java_app_simple_felicity_engine_processors_UsbAudioOutputProcessor_nativeUsbAudioSetAltSetting(
        JNIEnv *, jobject, jlong h, jint alt) {
    auto *ctx = reinterpret_cast<UsbAudioContext *>(h);
    if (!ctx) return JNI_FALSE;
    return setAlternateSetting(ctx->fd, ctx->interfaceId, alt) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_app_simple_felicity_engine_processors_UsbAudioOutputProcessor_nativeUsbAudioSetSampleRate(
        JNIEnv *, jobject, jlong h, jint rate, jint csId) {
    auto *ctx = reinterpret_cast<UsbAudioContext *>(h);
    if (!ctx) return JNI_FALSE;
    ctx->sampleRate = rate;
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_app_simple_felicity_engine_processors_UsbAudioOutputProcessor_nativeUsbAudioStart(
        JNIEnv *, jobject, jlong h) {
    auto *ctx = reinterpret_cast<UsbAudioContext *>(h);
    if (!ctx) return JNI_FALSE;
    ctx->running.store(true);
    ctx->framesWritten = 0;
    ctx->urbsInFlight = 0;

    if (ctx->endpointFeedback > 0) {
        double fb = readFeedback(ctx->fd, ctx->endpointFeedback);
        if (fb > 0) LOGI("Start: feedback=%.4f frames/mf (%.1f Hz)", fb, fb * 8000.0);
        else LOGW("Start: feedback not responding");
    }

    // Pre-fill pipeline with silence URBs (fire and forget — no reap)
    double fpmf = ctx->sampleRate / 8000.0;
    double acc = 0.0;
    for (int u = 0; u < USB_AUDIO_NUM_URBS; u++) {
        int pktSizes[USB_AUDIO_MAX_PACKETS_PER_URB];
        int pkts = 0, bytes = 0;
        for (int p = 0; p < USB_AUDIO_MAX_PACKETS_PER_URB; p++) {
            acc += fpmf;
            int frames = (int)acc;
            acc -= frames;
            int b = frames * ctx->bytesPerFrame;
            pktSizes[p] = b;
            bytes += b;
            pkts++;
        }
        auto *buf = (uint8_t *)calloc(1, bytes);
        if (!buf) break;
        if (submitIsoUrb(ctx->fd, ctx->endpointOut, buf, pktSizes, pkts) < 0) {
            free(buf);
            break;
        }
        ctx->urbsInFlight++;
    }

    LOGI("Start: rate=%d ch=%d bits=%d pipeline=%d URBs",
         ctx->sampleRate, ctx->channelCount, ctx->bitDepth, ctx->urbsInFlight);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_app_simple_felicity_engine_processors_UsbAudioOutputProcessor_nativeUsbAudioWrite(
        JNIEnv *env, jobject, jlong h, jfloatArray pcm) {
    auto *ctx = reinterpret_cast<UsbAudioContext *>(h);
    if (!ctx || !ctx->running.load()) return;

    jint totalSamples = env->GetArrayLength(pcm);
    if (totalSamples <= 0) return;
    int totalFrames = totalSamples / ctx->channelCount;
    int totalBytes = totalSamples * ctx->bytesPerSample;

    if (!ctx->transferBuffer || ctx->transferBufferCapacity < totalBytes) {
        free(ctx->transferBuffer);
        ctx->transferBuffer = (uint8_t *)malloc(totalBytes);
        ctx->transferBufferCapacity = totalBytes;
    }

    jfloat *f = env->GetFloatArrayElements(pcm, nullptr);
    if (!f) return;
    switch (ctx->bitDepth) {
        case 16: convertFloatToInt16(f, ctx->transferBuffer, totalSamples); break;
        case 24: convertFloatToInt24(f, ctx->transferBuffer, totalSamples); break;
        case 32: convertFloatToInt32(f, ctx->transferBuffer, totalSamples); break;
        default: env->ReleaseFloatArrayElements(pcm, f, JNI_ABORT); return;
    }
    env->ReleaseFloatArrayElements(pcm, f, JNI_ABORT);

    // Build packets and submit URBs
    double fpmf = ctx->sampleRate / 8000.0;
    static double acc = 0.0;
    int offset = 0;

    while (offset < totalBytes && ctx->running.load()) {
        int pktSizes[USB_AUDIO_MAX_PACKETS_PER_URB];
        int pkts = 0, urbBytes = 0;

        for (int p = 0; p < USB_AUDIO_MAX_PACKETS_PER_URB && offset + urbBytes < totalBytes; p++) {
            acc += fpmf;
            int frames = (int)acc;
            acc -= frames;
            int b = frames * ctx->bytesPerFrame;
            if (urbBytes + b > totalBytes - offset) {
                b = ((totalBytes - offset - urbBytes) / ctx->bytesPerFrame) * ctx->bytesPerFrame;
                if (b <= 0) break;
            }
            pktSizes[p] = b;
            urbBytes += b;
            pkts++;
        }
        if (pkts <= 0 || urbBytes <= 0) break;

        // Copy data for this URB (transferBuffer is reused)
        auto *buf = (uint8_t *)malloc(urbBytes);
        if (!buf) break;
        memcpy(buf, ctx->transferBuffer + offset, urbBytes);

        // Reap one old URB before submitting new one (maintain pipeline depth)
        if (ctx->urbsInFlight >= USB_AUDIO_NUM_URBS) {
            if (reapOneUrb(ctx->fd) == 0) ctx->urbsInFlight--;
        }

        if (submitIsoUrb(ctx->fd, ctx->endpointOut, buf, pktSizes, pkts) < 0) {
            free(buf);
            break;
        }
        ctx->urbsInFlight++;
        offset += urbBytes;
    }

    ctx->framesWritten += totalFrames;
    if (ctx->framesWritten % ctx->sampleRate < totalFrames) {
        double fb = 0;
        if (ctx->endpointFeedback > 0) fb = readFeedback(ctx->fd, ctx->endpointFeedback);
        LOGI("Write: %lld frames (~%.0f sec) inflight=%d fb=%.1fHz",
             (long long)ctx->framesWritten, (double)ctx->framesWritten/ctx->sampleRate,
             ctx->urbsInFlight, fb * 8000.0);
    }
}

JNIEXPORT void JNICALL
Java_app_simple_felicity_engine_processors_UsbAudioOutputProcessor_nativeUsbAudioStop(
        JNIEnv *, jobject, jlong h) {
    auto *ctx = reinterpret_cast<UsbAudioContext *>(h);
    if (!ctx) return;
    ctx->running.store(false);
    // Drain pipeline
    while (ctx->urbsInFlight > 0) {
        if (reapOneUrb(ctx->fd) < 0) break;
        ctx->urbsInFlight--;
    }
    LOGI("Stop: %lld frames, drained to %d URBs", (long long)ctx->framesWritten, ctx->urbsInFlight);
}

JNIEXPORT void JNICALL
Java_app_simple_felicity_engine_processors_UsbAudioOutputProcessor_nativeUsbAudioDestroy(
        JNIEnv *, jobject, jlong h) {
    auto *ctx = reinterpret_cast<UsbAudioContext *>(h);
    if (!ctx) return;
    ctx->running.store(false);
    while (ctx->urbsInFlight > 0) {
        if (reapOneUrb(ctx->fd) < 0) break;
        ctx->urbsInFlight--;
    }
    if (ctx->interfaceClaimed) {
        setAlternateSetting(ctx->fd, ctx->interfaceId, 0);
        releaseInterface(ctx->fd, ctx->interfaceId);
    }
    free(ctx->transferBuffer);
    LOGI("Destroy: %lld frames total", (long long)ctx->framesWritten);
    delete ctx;
}

JNIEXPORT jint JNICALL
Java_app_simple_felicity_engine_processors_UsbAudioOutputProcessor_nativeUsbReset(
        JNIEnv *, jobject, jint fd) {
    LOGI("USBDEVFS_RESET fd=%d", fd);
    int ret = ioctl(fd, USBDEVFS_RESET, 0);
    if (ret < 0) { LOGE("RESET FAILED errno=%d", errno); return ret; }
    LOGI("RESET OK, claiming interfaces...");
    struct usbdevfs_ioctl cmd = {}; cmd.ifno = 0; cmd.ioctl_code = USBDEVFS_DISCONNECT;
    ioctl(fd, USBDEVFS_IOCTL, &cmd);
    int i0 = 0; ioctl(fd, USBDEVFS_CLAIMINTERFACE, &i0);
    cmd.ifno = 1; ioctl(fd, USBDEVFS_IOCTL, &cmd);
    int i1 = 1; ioctl(fd, USBDEVFS_CLAIMINTERFACE, &i1);
    struct usbdevfs_setinterface si = {}; si.interface = 1; si.altsetting = 0;
    ioctl(fd, USBDEVFS_SETINTERFACE, &si);
    return 0;
}

} // extern "C"
