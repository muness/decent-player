/**
 * @file usb-audio-output.cpp
 * @brief Direct USB audio output via Linux usbdevfs isochronous transfers.
 *
 * Sends PCM audio directly to a USB Audio Class 2.0 DAC, bypassing the
 * entire Android audio stack. Maintains a pipeline of multiple URBs in
 * flight for continuous streaming.
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
 * URB tracking for safe deallocation after reap.
 * Android MTE may modify pointer tags, making direct free() on kernel-returned
 * pointers unsafe. We track our original allocations and free those instead.
 */
#define MAX_TRACKED_URBS 64
static struct { struct usbdevfs_urb *urb; void *buffer; } trackedUrbs[MAX_TRACKED_URBS];
static int trackedCount = 0;

static void trackUrb(struct usbdevfs_urb *urb, void *buffer) {
    if (trackedCount < MAX_TRACKED_URBS) {
        trackedUrbs[trackedCount].urb = urb;
        trackedUrbs[trackedCount].buffer = buffer;
        trackedCount++;
    }
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
    u->usercontext = data;  // Store original pointer for safe free after reap (MTE compat)
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
    // Track for safe deallocation on reap
    trackUrb(u, data);
    return totalBytes;
}

/**
 * Reap one completed URB (blocking). Frees both URB and its data buffer.
 * Returns 0 on success, -1 on error.
 */
static int reapOneUrb(int fd) {
    struct usbdevfs_urb *c = nullptr;

    // Use non-blocking reap with retry to avoid infinite hang.
    // The xHCI host controller sometimes stops processing URBs after
    // multiple close/reopen cycles, causing blocking REAPURB to hang forever.
    for (int attempt = 0; attempt < 200; attempt++) {  // 200 * 1ms = 200ms max
        int ret = ioctl(fd, USBDEVFS_REAPURBNDELAY, &c);
        if (ret == 0 && c != nullptr) {
            goto found;
        }
        if (ret < 0 && errno != EAGAIN) {
            return -1;  // real error (ENODEV etc)
        }
        usleep(1000);  // 1ms
    }
    LOGE("reapOneUrb: timed out after 200ms — URBs stuck in xHCI");
    return -2;  // timeout — distinct from error

    found:
    int ret = 0;

    // Find and free using our tracked pointers, not the kernel-returned ones
    for (int i = 0; i < trackedCount; i++) {
        if (trackedUrbs[i].urb == c) {
            free(trackedUrbs[i].buffer);
            free(trackedUrbs[i].urb);
            // Remove from tracking by swapping with last
            trackedUrbs[i] = trackedUrbs[trackedCount - 1];
            trackedCount--;
            return 0;
        }
    }

    // Fallback: not tracked (shouldn't happen)
    LOGW("reapOneUrb: reaped untracked URB %p", c);
    return 0;
}

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioCreate(
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
Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioSetAltSetting(
        JNIEnv *, jobject, jlong h, jint alt) {
    auto *ctx = reinterpret_cast<UsbAudioContext *>(h);
    if (!ctx) return JNI_FALSE;
    return setAlternateSetting(ctx->fd, ctx->interfaceId, alt) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioSetSampleRate(
        JNIEnv *, jobject, jlong h, jint rate, jint csId) {
    auto *ctx = reinterpret_cast<UsbAudioContext *>(h);
    if (!ctx) return JNI_FALSE;
    ctx->sampleRate = rate;
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioStart(
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

    // No silence pre-fill. Pipeline fills naturally with real data.
    // Silence pre-fill at old rate interferes with xHCI rate transitions.
    int preFillCount = 0;
    double fpmf = ctx->sampleRate / 8000.0;
    double acc = 0.0;
    for (int u = 0; u < preFillCount; u++) {
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
Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioWrite(
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
            static int reapInWriteCount = 0;
            reapInWriteCount++;
            if (reapInWriteCount <= 20 || reapInWriteCount % 500 == 0) {
                LOGI("nativeUsbAudioWrite: about to reap, inflight=%d tracked=%d call#%d",
                     ctx->urbsInFlight, trackedCount, reapInWriteCount);
            }
            int reapResult = reapOneUrb(ctx->fd);
            if (reapResult == 0) {
                ctx->urbsInFlight--;
            } else if (reapResult == -2) {
                // URBs stuck in xHCI — discard all tracked URBs and stop
                LOGE("nativeUsbAudioWrite: xHCI stuck, discarding %d URBs", trackedCount);
                for (int i = 0; i < trackedCount; i++) {
                    ioctl(ctx->fd, USBDEVFS_DISCARDURB, trackedUrbs[i].urb);
                }
                // Reap the discarded URBs (they return immediately with error)
                for (int i = 0; i < trackedCount; i++) {
                    struct usbdevfs_urb *disc = nullptr;
                    ioctl(ctx->fd, USBDEVFS_REAPURBNDELAY, &disc);
                    if (disc) { free(trackedUrbs[i].buffer); free(trackedUrbs[i].urb); }
                }
                trackedCount = 0;
                ctx->urbsInFlight = 0;
                free(buf);
                ctx->running.store(false);
                return;
            } else {
                free(buf);
                ctx->running.store(false);
                return;
            }
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
Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioStop(
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
Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioDestroy(
        JNIEnv *, jobject, jlong h) {
    auto *ctx = reinterpret_cast<UsbAudioContext *>(h);
    if (!ctx) return;
    ctx->running.store(false);

    // Drain URBs with timeout-based reap
    while (ctx->urbsInFlight > 0) {
        if (reapOneUrb(ctx->fd) < 0) break;
        ctx->urbsInFlight--;
    }
    LOGI("Destroy: drained to %d URBs remaining", ctx->urbsInFlight);

    // Do NOT setAltSetting or releaseInterface from native code.
    // The Java layer handles setAltSetting(0) via UsbDeviceConnection.setInterface()
    // which properly triggers the xHCI Configure Endpoint Command to free/alloc
    // the isochronous ring. Native USBDEVFS_SETINTERFACE does NOT do this correctly.
    // Free any remaining tracked URBs that weren't reaped
    for (int i = 0; i < trackedCount; i++) {
        free(trackedUrbs[i].buffer);
        free(trackedUrbs[i].urb);
    }
    trackedCount = 0;

    free(ctx->transferBuffer);
    LOGI("Destroy: %lld frames total", (long long)ctx->framesWritten);
    delete ctx;
}

JNIEXPORT jboolean JNICALL
Java_com_decent_usbaudio_UsbAudioStream_nativeIsRunning(
        JNIEnv *, jobject, jlong h) {
    auto *ctx = reinterpret_cast<UsbAudioContext *>(h);
    if (!ctx) return JNI_FALSE;
    return ctx->running.load() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_decent_usbaudio_UsbAudioStream_nativeUsbReset(
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
