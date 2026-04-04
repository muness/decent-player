/**
 * @file usb-audio-output.cpp
 * @brief Direct USB audio output via Linux usbdevfs isochronous transfers.
 *
 * Sends PCM audio directly to a USB Audio Class 2.0 DAC, bypassing the
 * entire Android audio stack. Maintains a pipeline of 64 URBs in flight
 * for continuous streaming, modeled after (removed)'s xHCI behavior.
 *
 * Pipeline architecture:
 * - 64 URBs, each carrying 8 ISO packets (1ms of audio)
 * - Total pipeline: ~64ms of buffered audio
 * - Write path: reap one completed URB, fill it with new data, resubmit
 * - Drain path: stop submitting, reap ALL URBs (blocking), return to Kotlin
 *
 * Rate transition sequence (from (removed) xHCI ftrace analysis):
 * 1. Kotlin calls stop() -> native stops accepting writes
 * 2. Kotlin calls drainUrbs() -> native reaps ALL in-flight URBs (blocking)
 * 3. Kotlin calls setAlt(0) via Java -> xHCI frees old ISO ring
 * 4. Kotlin calls SET_CUR sample rate
 * 5. Kotlin calls setAlt(N) via Java -> xHCI allocs new ISO ring
 * 6. Kotlin creates new UsbAudioStream with new rate
 * 7. Kotlin calls start() + write() -> pipeline fills naturally
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

// ── Float → PCM conversion ──────────────────────────────────────────

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

// ── URB tracking (MTE-safe) ─────────────────────────────────────────

static void trackUrb(UsbAudioContext *ctx, struct usbdevfs_urb *urb, void *buffer) {
    if (ctx->trackedCount < USB_AUDIO_MAX_TRACKED_URBS) {
        ctx->trackedUrbs[ctx->trackedCount].urb = urb;
        ctx->trackedUrbs[ctx->trackedCount].buffer = buffer;
        ctx->trackedUrbs[ctx->trackedCount].active = true;
        ctx->trackedCount++;
    } else {
        LOGE("trackUrb: overflow! trackedCount=%d", ctx->trackedCount);
    }
}

static int findTrackedUrb(UsbAudioContext *ctx, struct usbdevfs_urb *urb) {
    for (int i = 0; i < ctx->trackedCount; i++) {
        if (ctx->trackedUrbs[i].urb == urb) return i;
    }
    return -1;
}

static void freeTrackedUrb(UsbAudioContext *ctx, int idx) {
    free(ctx->trackedUrbs[idx].buffer);
    free(ctx->trackedUrbs[idx].urb);
    // Swap with last entry to keep array compact
    ctx->trackedUrbs[idx] = ctx->trackedUrbs[ctx->trackedCount - 1];
    ctx->trackedCount--;
}

// ── USB helpers ─────────────────────────────────────────────────────

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

// ── ISO URB submission ──────────────────────────────────────────────

/**
 * Build packet sizes for one URB based on sample rate.
 * Uses the context's frame accumulator for jitter-free sizing.
 *
 * @param ctx       Audio context (accumulator is updated)
 * @param pktSizes  Output array (must hold USB_AUDIO_PACKETS_PER_URB entries)
 * @return          Total bytes for this URB
 */
static int buildPacketSizes(UsbAudioContext *ctx, int *pktSizes) {
    double fpmf = ctx->sampleRate / 8000.0;  // frames per microframe (125us)
    int totalBytes = 0;
    for (int p = 0; p < USB_AUDIO_PACKETS_PER_URB; p++) {
        ctx->frameAccumulator += fpmf;
        int frames = (int)ctx->frameAccumulator;
        ctx->frameAccumulator -= frames;
        int b = frames * ctx->bytesPerFrame;
        pktSizes[p] = b;
        totalBytes += b;
    }
    return totalBytes;
}

/**
 * Submit one ISO URB with pre-built packet data.
 * The data buffer is owned by the URB until reaped.
 * Returns 0 on success, -1 on error.
 */
static int submitIsoUrb(UsbAudioContext *ctx, uint8_t *data, const int *pktSizes, int totalBytes) {
    size_t sz = sizeof(struct usbdevfs_urb) +
                USB_AUDIO_PACKETS_PER_URB * sizeof(struct usbdevfs_iso_packet_desc);
    auto *u = (struct usbdevfs_urb *)calloc(1, sz);
    if (!u) return -1;

    u->type = USBDEVFS_URB_TYPE_ISO;
    u->flags = USBDEVFS_URB_ISO_ASAP;
    u->endpoint = (unsigned char)ctx->endpointOut;
    u->buffer = data;
    u->buffer_length = totalBytes;
    u->number_of_packets = USB_AUDIO_PACKETS_PER_URB;
    for (int i = 0; i < USB_AUDIO_PACKETS_PER_URB; i++)
        u->iso_frame_desc[i].length = (unsigned)pktSizes[i];

    int ret = ioctl(ctx->fd, USBDEVFS_SUBMITURB, u);
    if (ret < 0) {
        LOGE("SUBMITURB FAILED ep=0x%02x bytes=%d errno=%d (%s)",
             ctx->endpointOut, totalBytes, errno, strerror(errno));
        free(u);
        return -1;
    }
    trackUrb(ctx, u, data);
    ctx->urbsInFlight++;
    return 0;
}

/**
 * Reap one completed URB. Uses non-blocking with retry.
 * @param fd         USB file descriptor
 * @param ctx        Audio context (for URB tracking)
 * @param timeoutMs  Maximum wait in milliseconds
 * @return 0 = success, -1 = error, -2 = timeout
 */
static int reapOneUrb(UsbAudioContext *ctx, int timeoutMs) {
    struct usbdevfs_urb *c = nullptr;
    int attempts = timeoutMs;  // 1ms per attempt

    for (int i = 0; i < attempts; i++) {
        int ret = ioctl(ctx->fd, USBDEVFS_REAPURBNDELAY, &c);
        if (ret == 0 && c != nullptr) {
            // Found a completed URB — free it via tracked pointers (MTE-safe)
            int idx = findTrackedUrb(ctx, c);
            if (idx >= 0) {
                freeTrackedUrb(ctx, idx);
            } else {
                LOGW("reapOneUrb: reaped untracked URB %p", c);
            }
            ctx->urbsInFlight--;
            return 0;
        }
        if (ret < 0 && errno != EAGAIN) {
            LOGE("reapOneUrb: error errno=%d (%s)", errno, strerror(errno));
            return -1;
        }
        usleep(1000);  // 1ms
    }
    return -2;  // timeout
}

/**
 * Drain ALL in-flight URBs. Blocks until every URB is reaped or timeout.
 * This is the critical step before setInterface() — all URBs must complete
 * before the xHCI Configure Endpoint Command can safely free the ISO ring.
 *
 * @return Number of URBs successfully drained
 */
static int drainAllUrbs(UsbAudioContext *ctx) {
    int drained = 0;
    int initialCount = ctx->urbsInFlight;
    LOGI("drainAllUrbs: draining %d URBs...", initialCount);

    while (ctx->urbsInFlight > 0) {
        int result = reapOneUrb(ctx, 500);  // 500ms timeout per URB
        if (result == 0) {
            drained++;
        } else if (result == -2) {
            // Timeout — try to discard remaining URBs
            LOGW("drainAllUrbs: timeout after draining %d/%d, discarding remaining %d",
                 drained, initialCount, ctx->urbsInFlight);
            for (int i = 0; i < ctx->trackedCount; i++) {
                ioctl(ctx->fd, USBDEVFS_DISCARDURB, ctx->trackedUrbs[i].urb);
            }
            // Reap the discarded URBs (they return immediately)
            for (int i = 0; i < 100 && ctx->urbsInFlight > 0; i++) {
                struct usbdevfs_urb *disc = nullptr;
                if (ioctl(ctx->fd, USBDEVFS_REAPURBNDELAY, &disc) == 0 && disc != nullptr) {
                    int idx = findTrackedUrb(ctx, disc);
                    if (idx >= 0) freeTrackedUrb(ctx, idx);
                    ctx->urbsInFlight--;
                    drained++;
                } else {
                    usleep(1000);
                }
            }
            break;
        } else {
            // Real error (ENODEV, etc.)
            LOGE("drainAllUrbs: reap error after draining %d/%d", drained, initialCount);
            break;
        }
    }

    LOGI("drainAllUrbs: drained %d/%d URBs, remaining=%d tracked=%d",
         drained, initialCount, ctx->urbsInFlight, ctx->trackedCount);
    return drained;
}

// ── JNI entry points ────────────────────────────────────────────────

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioCreate(
        JNIEnv *, jobject, jint fd, jint ifId, jint epOut, jint epFb,
        jint rate, jint ch, jint bits, jint maxPkt) {
    LOGI("Create: fd=%d ep=0x%02x rate=%d ch=%d bits=%d maxPkt=%d",
         fd, epOut, rate, ch, bits, maxPkt);
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
    ctx->trackedCount = 0;
    ctx->frameAccumulator = 0.0;
    memset(ctx->trackedUrbs, 0, sizeof(ctx->trackedUrbs));
    return reinterpret_cast<jlong>(ctx);
}

JNIEXPORT jboolean JNICALL
Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioSetAltSetting(
        JNIEnv *, jobject, jlong h, jint alt) {
    auto *ctx = reinterpret_cast<UsbAudioContext *>(h);
    if (!ctx) return JNI_FALSE;
    struct usbdevfs_setinterface si = {};
    si.interface = (unsigned)ctx->interfaceId;
    si.altsetting = (unsigned)alt;
    int r = ioctl(ctx->fd, USBDEVFS_SETINTERFACE, &si);
    LOGI("setAlt(%d,%d): ret=%d errno=%d", ctx->interfaceId, alt, r, errno);
    return r >= 0 ? JNI_TRUE : JNI_FALSE;
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
    ctx->trackedCount = 0;
    ctx->frameAccumulator = 0.0;

    if (ctx->endpointFeedback > 0) {
        double fb = readFeedback(ctx->fd, ctx->endpointFeedback);
        if (fb > 0) LOGI("Start: feedback=%.4f frames/mf (%.1f Hz)", fb, fb * 8000.0);
        else LOGW("Start: feedback not responding");
    }

    LOGI("Start: rate=%d ch=%d bits=%d pipeline=%d URBs target",
         ctx->sampleRate, ctx->channelCount, ctx->bitDepth, USB_AUDIO_NUM_URBS);
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

    // Resize transfer buffer if needed
    if (!ctx->transferBuffer || ctx->transferBufferCapacity < totalBytes) {
        free(ctx->transferBuffer);
        ctx->transferBuffer = (uint8_t *)malloc(totalBytes);
        ctx->transferBufferCapacity = totalBytes;
    }

    // Convert float PCM to target bit depth
    jfloat *f = env->GetFloatArrayElements(pcm, nullptr);
    if (!f) return;
    switch (ctx->bitDepth) {
        case 16: convertFloatToInt16(f, ctx->transferBuffer, totalSamples); break;
        case 24: convertFloatToInt24(f, ctx->transferBuffer, totalSamples); break;
        case 32: convertFloatToInt32(f, ctx->transferBuffer, totalSamples); break;
        default: env->ReleaseFloatArrayElements(pcm, f, JNI_ABORT); return;
    }
    env->ReleaseFloatArrayElements(pcm, f, JNI_ABORT);

    // Build and submit URBs from the converted PCM data
    int offset = 0;

    while (offset < totalBytes && ctx->running.load()) {
        // Build packet sizes for this URB
        int pktSizes[USB_AUDIO_PACKETS_PER_URB];
        int urbBytes = buildPacketSizes(ctx, pktSizes);

        // Clamp to remaining data
        if (urbBytes > totalBytes - offset) {
            // Recalculate with clamped sizes
            urbBytes = 0;
            double fpmf = ctx->sampleRate / 8000.0;
            // Rewind the accumulator and recalculate
            for (int p = 0; p < USB_AUDIO_PACKETS_PER_URB; p++) {
                int remaining = totalBytes - offset - urbBytes;
                if (remaining <= 0) {
                    pktSizes[p] = 0;
                    continue;
                }
                int b = pktSizes[p];
                if (b > remaining) {
                    b = (remaining / ctx->bytesPerFrame) * ctx->bytesPerFrame;
                }
                pktSizes[p] = b;
                urbBytes += b;
            }
            if (urbBytes <= 0) break;
        }

        // Wait for a slot: reap one old URB if pipeline is full
        if (ctx->urbsInFlight >= USB_AUDIO_NUM_URBS) {
            int reapResult = reapOneUrb(ctx, 200);
            if (reapResult == -2) {
                // URBs stuck in xHCI — signal to stop
                LOGE("Write: xHCI stuck, draining %d URBs", ctx->urbsInFlight);
                drainAllUrbs(ctx);
                ctx->running.store(false);
                return;
            } else if (reapResult < 0) {
                ctx->running.store(false);
                return;
            }
        }

        // Allocate and fill URB data buffer
        auto *buf = (uint8_t *)malloc(urbBytes);
        if (!buf) break;
        memcpy(buf, ctx->transferBuffer + offset, urbBytes);

        if (submitIsoUrb(ctx, buf, pktSizes, urbBytes) < 0) {
            free(buf);
            break;
        }
        offset += urbBytes;
    }

    ctx->framesWritten += totalFrames;

    // Periodic logging (~once per second)
    if (ctx->framesWritten % ctx->sampleRate < (int64_t)totalFrames) {
        double fb = 0;
        if (ctx->endpointFeedback > 0) fb = readFeedback(ctx->fd, ctx->endpointFeedback);
        LOGI("Write: %lld frames (~%.0f sec) inflight=%d tracked=%d fb=%.1fHz",
             (long long)ctx->framesWritten, (double)ctx->framesWritten/ctx->sampleRate,
             ctx->urbsInFlight, ctx->trackedCount, fb * 8000.0);
    }
}

JNIEXPORT void JNICALL
Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioStop(
        JNIEnv *, jobject, jlong h) {
    auto *ctx = reinterpret_cast<UsbAudioContext *>(h);
    if (!ctx) return;
    ctx->running.store(false);
    LOGI("Stop: %lld frames written, %d URBs still in flight",
         (long long)ctx->framesWritten, ctx->urbsInFlight);
}

/**
 * Drain all in-flight URBs. Blocks until all URBs are reaped.
 *
 * This MUST be called after stop() and BEFORE the Kotlin layer calls
 * setAlt(0). The xHCI Configure Endpoint Command triggered by setAlt(0)
 * frees the isochronous ring — if any URBs are still in the ring, the
 * host controller state becomes corrupted.
 *
 * (removed) sequence: stop URBs -> wait ~195ms (drain) -> setAlt(0)
 *
 * @return Number of URBs successfully drained
 */
JNIEXPORT jint JNICALL
Java_com_decent_usbaudio_UsbAudioStream_nativeDrainUrbs(
        JNIEnv *, jobject, jlong h) {
    auto *ctx = reinterpret_cast<UsbAudioContext *>(h);
    if (!ctx) return 0;
    ctx->running.store(false);
    return drainAllUrbs(ctx);
}

JNIEXPORT void JNICALL
Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioDestroy(
        JNIEnv *, jobject, jlong h) {
    auto *ctx = reinterpret_cast<UsbAudioContext *>(h);
    if (!ctx) return;
    ctx->running.store(false);

    // Drain any remaining URBs
    if (ctx->urbsInFlight > 0) {
        drainAllUrbs(ctx);
    }

    // Free any tracked URBs that weren't reaped during drain
    for (int i = 0; i < ctx->trackedCount; i++) {
        free(ctx->trackedUrbs[i].buffer);
        free(ctx->trackedUrbs[i].urb);
    }
    ctx->trackedCount = 0;

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
        JNIEnv *, jclass, jint fd) {
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
