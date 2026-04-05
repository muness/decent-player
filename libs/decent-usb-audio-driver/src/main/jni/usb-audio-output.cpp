/**
 * @file usb-audio-output.cpp
 * @brief Direct USB audio output via Linux usbdevfs isochronous transfers.
 *
 * Pipeline: pre-allocated ring buffer of 64 URBs. No malloc/free during
 * streaming — completely avoids ARM MTE pointer tag issues on Samsung devices.
 *
 * ISO URBs with ISO_ASAP complete in FIFO order, so we use a simple ring
 * with submit/reap indices. No need to identify which URB was reaped.
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
#include <time.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

#ifndef USBDEVFS_URB_ISO_ASAP
#define USBDEVFS_URB_ISO_ASAP 0x02
#endif

#define TAG "UsbAudioOutput"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ── Float → PCM conversion ──────────────────────────────────────────

static inline float clampf(float v) { return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v); }

// Bit-perfect float→int conversion matching FFmpeg's libswresample normalization.
// FFmpeg normalizes: int / 2^N (e.g., int16 / 32768.0).
// Reconversion: float × 2^N gives exact round-trip for 16-bit and 24-bit because:
//   - 2^N is exactly representable in float32 (power of 2)
//   - float32 has 24-bit mantissa, covering int16 (16-bit) and int24 (24-bit) exactly
// Clamp after scaling to handle the asymmetry: -1.0 × 32768 = -32768 (valid min),
// but +1.0 × 32768 = 32768 (exceeds max 32767, needs clamping).

static void convertFloatToInt16(const float *src, uint8_t *dst, int n) {
    auto *out = reinterpret_cast<int16_t *>(dst);
    for (int i = 0; i < n; i++) {
        float s = clampf(src[i]) * 32768.0f;
        if (s > 32767.0f) s = 32767.0f;
        if (s < -32768.0f) s = -32768.0f;
        out[i] = (int16_t)s;
    }
}
static void convertFloatToInt24(const float *src, uint8_t *dst, int n) {
    for (int i = 0; i < n; i++) {
        float s = clampf(src[i]) * 8388608.0f;
        if (s > 8388607.0f) s = 8388607.0f;
        if (s < -8388608.0f) s = -8388608.0f;
        int32_t v = (int32_t)s;
        dst[i*3] = v & 0xFF; dst[i*3+1] = (v>>8) & 0xFF; dst[i*3+2] = (v>>16) & 0xFF;
    }
}
static void convertFloatToInt32(const float *src, uint8_t *dst, int n) {
    auto *out = reinterpret_cast<int32_t *>(dst);
    for (int i = 0; i < n; i++) {
        // Use double: float32 can't represent 2147483648.0 exactly (needs 31 bits,
        // float32 has 24-bit mantissa). Double has 53-bit mantissa — sufficient.
        double s = (double)clampf(src[i]) * 2147483648.0;
        if (s > 2147483647.0) s = 2147483647.0;
        if (s < -2147483648.0) s = -2147483648.0;
        out[i] = (int32_t)s;
    }
}

// ── Ring buffer management ──────────────────────────────────────────

/**
 * Allocate all URB slots in the ring buffer.
 * Called once at stream creation. All memory stays alive until destroy.
 */
static bool allocRing(UsbAudioContext *ctx) {
    size_t urbStructSize = sizeof(struct usbdevfs_urb) +
                           USB_AUDIO_PACKETS_PER_URB * sizeof(struct usbdevfs_iso_packet_desc);
    for (int i = 0; i < USB_AUDIO_NUM_URBS; i++) {
        ctx->ring[i].urb = (struct usbdevfs_urb *)calloc(1, urbStructSize);
        ctx->ring[i].buffer = (uint8_t *)malloc(USB_AUDIO_URB_BUFFER_SIZE);
        ctx->ring[i].dataLength = 0;
        if (!ctx->ring[i].urb || !ctx->ring[i].buffer) {
            LOGE("allocRing: OOM at slot %d", i);
            // Free what we allocated
            for (int j = 0; j <= i; j++) {
                free(ctx->ring[j].urb);
                free(ctx->ring[j].buffer);
            }
            return false;
        }
    }
    ctx->ringAllocated = true;
    return true;
}

/**
 * Free all URB slots. Called once at stream destruction.
 */
static void freeRing(UsbAudioContext *ctx) {
    if (!ctx->ringAllocated) return;
    for (int i = 0; i < USB_AUDIO_NUM_URBS; i++) {
        free(ctx->ring[i].urb);
        free(ctx->ring[i].buffer);
        ctx->ring[i].urb = nullptr;
        ctx->ring[i].buffer = nullptr;
    }
    ctx->ringAllocated = false;
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

// ── ISO URB submission / reap ───────────────────────────────────────

/**
 * Submit the URB at ring[submitIdx] with the given packet layout.
 * @param numPackets  Actual number of packets (no zero-length padding)
 * Returns 0 on success, -1 on error.
 */
static int submitRingUrb(UsbAudioContext *ctx, const int *pktSizes, int numPackets, int totalBytes) {
    UrbSlot *slot = &ctx->ring[ctx->submitIdx];
    struct usbdevfs_urb *u = slot->urb;

    // Clear the full URB struct including iso_frame_desc (stale actual_length/status)
    size_t clearSize = sizeof(struct usbdevfs_urb) +
                       numPackets * sizeof(struct usbdevfs_iso_packet_desc);
    memset(u, 0, clearSize);

    u->type = USBDEVFS_URB_TYPE_ISO;
    u->flags = USBDEVFS_URB_ISO_ASAP;
    u->endpoint = (unsigned char)ctx->endpointOut;
    u->buffer = slot->buffer;
    u->buffer_length = totalBytes;
    u->number_of_packets = numPackets;
    for (int i = 0; i < numPackets; i++)
        u->iso_frame_desc[i].length = (unsigned)pktSizes[i];

    slot->dataLength = totalBytes;

    int ret = ioctl(ctx->fd, USBDEVFS_SUBMITURB, u);
    if (ret < 0) {
        LOGE("SUBMITURB FAILED slot=%d ep=0x%02x bytes=%d pkts=%d errno=%d (%s)",
             ctx->submitIdx, ctx->endpointOut, totalBytes, numPackets, errno, strerror(errno));
        return -1;
    }

    ctx->submitIdx = (ctx->submitIdx + 1) % USB_AUDIO_NUM_URBS;
    ctx->urbsInFlight++;
    return 0;
}

/**
 * Reap the oldest submitted URB (at ring[reapIdx]).
 * ISO URBs with ISO_ASAP complete in FIFO order, so we always reap
 * the oldest one. No need to identify which URB was returned.
 *
 * @param timeoutMs  Maximum wait in milliseconds
 * @return 0 = success, -1 = error, -2 = timeout
 */
static int reapOldestUrb(UsbAudioContext *ctx, int timeoutMs) {
    struct usbdevfs_urb *c = nullptr;

    // Poll with 125µs interval matching USB high-speed microframe timing.
    // At 1ms polling, the max reap latency was 1ms per URB — with 65+ reaps
    // per write call, this added up to significant jitter causing audible
    // silence gaps. At 125µs, max latency per reap drops to 0.125ms.
    int iterations = timeoutMs * 8;  // 8 iterations per ms (125µs each)
    for (int i = 0; i < iterations; i++) {
        int ret = ioctl(ctx->fd, USBDEVFS_REAPURBNDELAY, &c);
        if (ret == 0 && c != nullptr) {
            ctx->reapIdx = (ctx->reapIdx + 1) % USB_AUDIO_NUM_URBS;
            ctx->urbsInFlight--;
            return 0;
        }
        if (ret < 0 && errno != EAGAIN) {
            LOGE("reapOldestUrb: error errno=%d (%s)", errno, strerror(errno));
            return -1;
        }
        usleep(125);  // 125µs = 1 USB microframe
    }
    return -2;  // timeout
}

/**
 * Drain ALL in-flight URBs. Blocks until every URB is reaped or timeout.
 * @return Number of URBs successfully drained
 */
static int drainAllUrbs(UsbAudioContext *ctx) {
    int drained = 0;
    int initialCount = ctx->urbsInFlight;
    LOGI("drainAllUrbs: draining %d URBs...", initialCount);

    // Phase 1: try to reap naturally (URBs that completed normally)
    while (ctx->urbsInFlight > 0) {
        int result = reapOldestUrb(ctx, 500);
        if (result == 0) {
            drained++;
        } else {
            // Timeout or error — move to phase 2
            break;
        }
    }

    if (ctx->urbsInFlight > 0) {
        LOGW("drainAllUrbs: %d/%d reaped naturally, discarding remaining %d",
             drained, initialCount, ctx->urbsInFlight);

        // Phase 2: DISCARD all remaining URBs first
        int toDiscard = ctx->urbsInFlight;
        for (int i = 0; i < toDiscard; i++) {
            int slotIdx = (ctx->reapIdx + i) % USB_AUDIO_NUM_URBS;
            ioctl(ctx->fd, USBDEVFS_DISCARDURB, ctx->ring[slotIdx].urb);
        }

        // Phase 3: Reap ALL pending completions from the event ring.
        // CRITICAL: Don't match 1:1 with discards — REAPURBNDELAY returns
        // completions from ANY endpoint in ANY order. We must drain the
        // entire completion queue to prevent event ring accumulation that
        // would corrupt future streams.
        int reaped = 0;
        for (int attempt = 0; attempt < 500 && reaped < toDiscard; attempt++) {
            struct usbdevfs_urb *c = nullptr;
            int ret = ioctl(ctx->fd, USBDEVFS_REAPURBNDELAY, &c);
            if (ret == 0 && c != nullptr) {
                reaped++;
            } else if (ret < 0 && errno != EAGAIN) {
                LOGE("drainAllUrbs: reap error errno=%d", errno);
                break;
            } else {
                usleep(1000);  // 1ms
            }
        }
        LOGI("drainAllUrbs: discarded %d, reaped %d completions", toDiscard, reaped);
        drained += reaped;

        // Phase 4: Flush any remaining stale completions (from previous
        // sessions' leaked feedback URBs, etc.)
        for (int i = 0; i < 10; i++) {
            struct usbdevfs_urb *c = nullptr;
            if (ioctl(ctx->fd, USBDEVFS_REAPURBNDELAY, &c) == 0 && c != nullptr) {
                LOGW("drainAllUrbs: flushed stale completion %p", c);
                drained++;
            } else {
                break;
            }
        }
    }

    // Reset ring to clean state
    ctx->urbsInFlight = 0;
    ctx->submitIdx = 0;
    ctx->reapIdx = 0;

    LOGI("drainAllUrbs: drained %d/%d, ring reset", drained, initialCount);
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
    ctx->submitIdx = 0;
    ctx->reapIdx = 0;
    ctx->urbsInFlight = 0;
    ctx->ringAllocated = false;
    ctx->frameAccumulator = 0.0;
    memset(ctx->ring, 0, sizeof(ctx->ring));

    if (!allocRing(ctx)) {
        delete ctx;
        return 0;
    }

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
    ctx->submitIdx = 0;
    ctx->reapIdx = 0;
    ctx->urbsInFlight = 0;
    ctx->frameAccumulator = 0.0;

    // NOTE: Do NOT call readFeedback() here! It uses REAPURBNDELAY on the
    // shared fd, which can reap stale URBs from previous sessions' feedback
    // reads. Each leaked feedback URB corrupts the audio ring's FIFO order,
    // causing "xHCI stuck" after 2-3 transitions. Feedback is only safe to
    // read with a dedicated reap mechanism (not shared REAPURBNDELAY).

    LOGI("Start: rate=%d ch=%d bits=%d ring=%d slots",
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

    struct timespec writeStart, writeEnd;
    clock_gettime(CLOCK_MONOTONIC, &writeStart);
    static long writeCallCount = 0;
    writeCallCount++;

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

    // Build URBs from converted PCM data.
    // Packet sizes are computed inline so the frame accumulator only
    // advances for packets that actually carry data — no zero-length
    // packets, no accumulator drift at buffer boundaries.
    int offset = 0;
    double fpmf = ctx->sampleRate / 8000.0;

    while (offset < totalBytes && ctx->running.load()) {
        int pktSizes[USB_AUDIO_PACKETS_PER_URB];
        int numPackets = 0;
        int urbBytes = 0;

        // Build packets one by one, stopping when data runs out
        for (int p = 0; p < USB_AUDIO_PACKETS_PER_URB; p++) {
            int remaining = totalBytes - offset - urbBytes;
            if (remaining <= 0) break;

            ctx->frameAccumulator += fpmf;
            int frames = (int)ctx->frameAccumulator;
            ctx->frameAccumulator -= frames;
            int b = frames * ctx->bytesPerFrame;

            if (b > remaining) {
                b = (remaining / ctx->bytesPerFrame) * ctx->bytesPerFrame;
                if (b <= 0) break;
            }

            pktSizes[p] = b;
            urbBytes += b;
            numPackets++;
        }
        if (numPackets <= 0 || urbBytes <= 0) break;

        // Wait for a free slot if pipeline is full
        if (ctx->urbsInFlight >= USB_AUDIO_NUM_URBS) {
            struct timespec ts0, ts1;
            clock_gettime(CLOCK_MONOTONIC, &ts0);
            int result = reapOldestUrb(ctx, 200);
            clock_gettime(CLOCK_MONOTONIC, &ts1);
            long reapUs = (ts1.tv_sec - ts0.tv_sec) * 1000000L +
                          (ts1.tv_nsec - ts0.tv_nsec) / 1000L;
            if (reapUs > 5000) {  // log if reap took > 5ms
                LOGW("Write: reap took %ldus (%.1fms) inflight=%d",
                     reapUs, reapUs / 1000.0, ctx->urbsInFlight);
            }
            if (result == -2) {
                LOGE("Write: reap timeout 200ms, inflight=%d", ctx->urbsInFlight);
                drainAllUrbs(ctx);
                ctx->running.store(false);
                return;
            } else if (result < 0) {
                ctx->running.store(false);
                return;
            }
        }

        // Copy data into the ring slot's pre-allocated buffer
        UrbSlot *slot = &ctx->ring[ctx->submitIdx];
        memcpy(slot->buffer, ctx->transferBuffer + offset, urbBytes);

        if (submitRingUrb(ctx, pktSizes, numPackets, urbBytes) < 0) {
            LOGE("Write: submit failed, stopping stream");
            ctx->running.store(false);
            return;
        }
        offset += urbBytes;
    }

    ctx->framesWritten += totalFrames;

    clock_gettime(CLOCK_MONOTONIC, &writeEnd);
    long writeUs = (writeEnd.tv_sec - writeStart.tv_sec) * 1000000L +
                   (writeEnd.tv_nsec - writeStart.tv_nsec) / 1000L;
    // Log every call that took > 10ms, or every 100th call
    if (writeUs > 10000 || writeCallCount % 100 == 0) {
        LOGI("nativeWrite #%ld: %d samples, %ldus (%.1fms), inflight=%d",
             writeCallCount, totalSamples, writeUs, writeUs / 1000.0, ctx->urbsInFlight);
    }

    // Periodic logging (~once per second)
    // NOTE: Do NOT call readFeedback() during streaming! It uses REAPURBNDELAY
    // which reaps ANY completed URB, not just the feedback one. With 64 audio
    // URBs in flight, it would steal an audio URB from the ring, desync the
    // FIFO indices, and cause buffer overwrites (garbage audio).
    if (ctx->framesWritten % ctx->sampleRate < (int64_t)totalFrames) {
        LOGI("Write: %lld frames (~%.0f sec) inflight=%d",
             (long long)ctx->framesWritten, (double)ctx->framesWritten/ctx->sampleRate,
             ctx->urbsInFlight);
    }
}

JNIEXPORT void JNICALL
Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioStop(
        JNIEnv *, jobject, jlong h) {
    auto *ctx = reinterpret_cast<UsbAudioContext *>(h);
    if (!ctx) return;
    ctx->running.store(false);
    LOGI("Stop: %lld frames, %d URBs in flight",
         (long long)ctx->framesWritten, ctx->urbsInFlight);
}

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

    if (ctx->urbsInFlight > 0) {
        drainAllUrbs(ctx);
    }

    freeRing(ctx);
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

// ── Integer padding (lossless, zero float) ──────────────────────────

// 16-bit → 32-bit: shift left 16
static void padInt16ToInt32(const uint8_t *src, uint8_t *dst, int numSamples) {
    auto *out = reinterpret_cast<int32_t *>(dst);
    auto *in16 = reinterpret_cast<const int16_t *>(src);
    for (int i = 0; i < numSamples; i++) out[i] = (int32_t)in16[i] << 16;
}

// int32 (24-bit sign-extended from libFLAC) → 32-bit: shift left 8
static void shiftInt32From24(const uint8_t *src, uint8_t *dst, int numSamples) {
    auto *out = reinterpret_cast<int32_t *>(dst);
    auto *in32 = reinterpret_cast<const int32_t *>(src);
    for (int i = 0; i < numSamples; i++) out[i] = in32[i] << 8;
}

// 24-bit packed (3 bytes/sample) → 32-bit: read 3 bytes, sign-extend, shift left 8
static void padInt24ToInt32(const uint8_t *src, uint8_t *dst, int numSamples) {
    auto *out = reinterpret_cast<int32_t *>(dst);
    for (int i = 0; i < numSamples; i++) {
        int32_t s = src[i*3] | (src[i*3+1] << 8) | (src[i*3+2] << 16);
        if (s & 0x800000) s |= 0xFF000000;  // sign-extend from 24 to 32 bits
        out[i] = s << 8;  // shift to fill 32-bit range
    }
}

// ── Shared URB submission logic ─────────────────────────────────────

/**
 * Submit PCM data (already in the target bit depth) to the USB pipeline.
 * Used by both nativeUsbAudioWrite (float path) and nativeUsbAudioWriteRaw.
 */
static void submitPcmToUrbs(UsbAudioContext *ctx, const uint8_t *pcmData, int totalBytes) {
    int offset = 0;
    double fpmf = ctx->sampleRate / 8000.0;

    while (offset < totalBytes && ctx->running.load()) {
        int pktSizes[USB_AUDIO_PACKETS_PER_URB];
        int numPackets = 0;
        int urbBytes = 0;

        for (int p = 0; p < USB_AUDIO_PACKETS_PER_URB; p++) {
            int remaining = totalBytes - offset - urbBytes;
            if (remaining <= 0) break;

            ctx->frameAccumulator += fpmf;
            int frames = (int)ctx->frameAccumulator;
            ctx->frameAccumulator -= frames;
            int b = frames * ctx->bytesPerFrame;

            if (b > remaining) {
                b = (remaining / ctx->bytesPerFrame) * ctx->bytesPerFrame;
                if (b <= 0) break;
            }

            pktSizes[p] = b;
            urbBytes += b;
            numPackets++;
        }
        if (numPackets <= 0 || urbBytes <= 0) break;

        if (ctx->urbsInFlight >= USB_AUDIO_NUM_URBS) {
            int result = reapOldestUrb(ctx, 200);
            if (result == -2) {
                LOGE("submitPcmToUrbs: reap timeout, inflight=%d", ctx->urbsInFlight);
                drainAllUrbs(ctx);
                ctx->running.store(false);
                return;
            } else if (result < 0) {
                ctx->running.store(false);
                return;
            }
        }

        UrbSlot *slot = &ctx->ring[ctx->submitIdx];
        memcpy(slot->buffer, pcmData + offset, urbBytes);

        if (submitRingUrb(ctx, pktSizes, numPackets, urbBytes) < 0) {
            LOGE("submitPcmToUrbs: submit failed, stopping stream");
            ctx->running.store(false);
            return;
        }
        offset += urbBytes;
    }
}

// ── Raw bytes write (no float, for libFLAC integer path) ────────────

JNIEXPORT void JNICALL
Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioWriteRaw(
        JNIEnv *env, jobject, jlong h, jbyteArray pcm, jint inputBitDepth) {
    auto *ctx = reinterpret_cast<UsbAudioContext *>(h);
    if (!ctx || !ctx->running.load()) return;

    jint inputBytes = env->GetArrayLength(pcm);
    if (inputBytes <= 0) return;

    int inputBps = inputBitDepth / 8;
    int totalSamples = inputBytes / inputBps;
    int totalFrames = totalSamples / ctx->channelCount;
    int outputBytes = totalSamples * ctx->bytesPerSample;

    // Resize transfer buffer if needed
    if (!ctx->transferBuffer || ctx->transferBufferCapacity < outputBytes) {
        free(ctx->transferBuffer);
        ctx->transferBuffer = (uint8_t *)malloc(outputBytes);
        ctx->transferBufferCapacity = outputBytes;
    }

    jbyte *rawData = env->GetByteArrayElements(pcm, nullptr);
    if (!rawData) return;

    // Bit-depth matching: pad input to DAC's bit depth (lossless integer ops)
    if (inputBitDepth == ctx->bitDepth) {
        // Same bit depth: zero-copy
        memcpy(ctx->transferBuffer, rawData, inputBytes);
    } else if (inputBitDepth == 16 && ctx->bitDepth == 32) {
        padInt16ToInt32((uint8_t *)rawData, ctx->transferBuffer, totalSamples);
    } else if (inputBitDepth == 24 && ctx->bitDepth == 32) {
        // 24-bit packed (3 bytes/sample) → 32-bit: sign-extend + shift left 8
        padInt24ToInt32((uint8_t *)rawData, ctx->transferBuffer, totalSamples);
    } else if (inputBitDepth == 32 && ctx->bitDepth == 32) {
        // libFLAC 24-bit → PCM_32BIT (sign-extended): shift left 8 to fill 32-bit range
        shiftInt32From24((uint8_t *)rawData, ctx->transferBuffer, totalSamples);
    } else {
        LOGE("WriteRaw: unsupported bit-depth conversion %d → %d", inputBitDepth, ctx->bitDepth);
        env->ReleaseByteArrayElements(pcm, rawData, JNI_ABORT);
        return;
    }

    env->ReleaseByteArrayElements(pcm, rawData, JNI_ABORT);

    // Submit to USB pipeline
    submitPcmToUrbs(ctx, ctx->transferBuffer, outputBytes);

    ctx->framesWritten += totalFrames;
    if (ctx->framesWritten % ctx->sampleRate < (int64_t)totalFrames) {
        LOGI("WriteRaw: %lld frames (~%.0f sec) inflight=%d inputBits=%d",
             (long long)ctx->framesWritten, (double)ctx->framesWritten/ctx->sampleRate,
             ctx->urbsInFlight, inputBitDepth);
    }
}

} // extern "C"
