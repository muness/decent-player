/**
 * @file usb-audio-output.h
 * @brief Direct USB audio output context for bit-perfect playback.
 *
 * Pipeline architecture:
 * - Pre-allocated ring buffer of 64 URB slots
 * - Each URB = 8 ISO packets (1ms of audio at high-speed)
 * - URBs complete in FIFO order (ISO_ASAP guarantees this)
 * - No malloc/free during streaming — avoids MTE pointer tag issues
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <linux/usbdevice_fs.h>

/**
 * Upper bound on isochronous packets per URB submission.
 * At USB high-speed (125us microframes), 8 packets = 1ms of audio, which is
 * what an endpoint with bInterval=1 (serviced every microframe) needs. An
 * endpoint with a larger bInterval is serviced less often and therefore needs
 * fewer, larger packets to cover the same millisecond — see
 * UsbAudioContext::packetsPerUrb. This macro sizes the fixed-length arrays.
 */
#define USB_AUDIO_PACKETS_PER_URB 8

/**
 * Number of URBs in the ring buffer.
 * 80 URBs ≈ 80 ms of in-flight audio at 44.1 kHz, which empirically gives
 * the xHCI host controller enough scheduling headroom to maintain
 * continuous isochronous output without underruns on commodity Android
 * SoCs. Smaller pipelines (< ~64 URBs at 44.1 kHz) trigger glitches as
 * the ring drains faster than the URB submit/reap cycle can refill it.
 */
#define USB_AUDIO_NUM_URBS 80

/**
 * Floor for the per-URB data buffer.
 * The old fixed size, kept as a lower bound so nothing shrinks: PCM up to
 * 384kHz * 4 bytes * 2 channels / 8000 microframes * 8 packets = 3072 bytes
 * still fits well inside it.
 *
 * The actual buffer is now sized from endpoint geometry
 * (wMaxPacketSize * packetsPerUrb, see UsbAudioContext::urbBufferSize),
 * because a fixed 1ms 4096-byte URB cannot hold every format a UAC2 endpoint
 * can carry. Stereo DSD512 in a 32-bit subslot is 705600 wire frames per
 * second at 8 bytes per frame — 5644.8 bytes per millisecond, which overflows
 * 4096. The Cayin RU7 advertises wMaxPacketSize=776 at bInterval=1, so its
 * geometry-derived buffer is 776 * 8 = 6208 bytes and the format fits.
 */
#define USB_AUDIO_URB_BUFFER_SIZE 4096

/**
 * One slot in the pre-allocated URB ring buffer.
 * The URB struct and data buffer are allocated once at stream creation
 * and reused for the lifetime of the stream. This avoids all MTE
 * pointer tag issues since we never pass malloc'd pointers through
 * the kernel and back.
 */
struct UrbSlot {
    /**
     * The URB struct, heap-allocated with enough trailing space for
     * USB_AUDIO_PACKETS_PER_URB iso_frame_desc entries.
     */
    struct usbdevfs_urb *urb;

    /** Data buffer for PCM audio. Fixed size, pre-allocated. */
    uint8_t *buffer;

    /** Actual number of bytes written to buffer for current submission. */
    int dataLength;
};

/**
 * Aggregate state for one USB audio output stream.
 */
struct UsbAudioContext {
    int fd;
    int interfaceId;
    int endpointOut;
    int endpointFeedback;
    int32_t sampleRate;
    int32_t channelCount;
    int32_t bitDepth;
    int32_t bytesPerSample;
    int32_t bytesPerFrame;
    /** wMaxPacketSize of the isochronous OUT endpoint, from its descriptor. */
    int32_t maxPacketSize;
    /** bInterval of the isochronous OUT endpoint. 1 = every microframe. */
    int32_t endpointInterval;

    // ── Endpoint geometry, derived at creation ──────────────────
    /**
     * Microframes between services of the data endpoint: 1 << (bInterval - 1).
     * bInterval=1 gives 1, which is what every DAC tested so far reports.
     */
    int32_t microframesPerPacket;

    /**
     * Isochronous packets in one URB, together covering 1 ms:
     * 8 / microframesPerPacket. Equals USB_AUDIO_PACKETS_PER_URB (8) whenever
     * bInterval is 1, so existing devices keep the exact packet layout they
     * had before.
     */
    int32_t packetsPerUrb;

    /**
     * Bytes one URB data buffer can hold:
     * max(maxPacketSize * packetsPerUrb, USB_AUDIO_URB_BUFFER_SIZE).
     */
    int32_t urbBufferSize;

    std::atomic<bool> running;

    /** Scratch buffer for PCM format conversion (float -> int16/24/32). */
    uint8_t *transferBuffer;
    int32_t transferBufferCapacity;

    int64_t framesWritten;
    bool interfaceClaimed;

    // ── Ring buffer ─────────────────────────────────────────────
    /** Pre-allocated URB slots. Never freed during streaming. */
    UrbSlot ring[USB_AUDIO_NUM_URBS];

    /** Index of next slot to fill and submit (wraps modulo NUM_URBS). */
    int submitIdx;

    /** Index of next slot to reap (wraps modulo NUM_URBS). */
    int reapIdx;

    /** Number of URBs currently submitted and not yet reaped. */
    int urbsInFlight;

    /** Whether ring buffers have been allocated. */
    bool ringAllocated;

    /** Fractional accumulator for sample-rate-to-packet-size conversion. */
    double frameAccumulator;

    /**
     * Frames per microframe, calibrated from the DAC's async feedback endpoint.
     * More accurate than the nominal sampleRate/8000.0 calculation because it
     * reflects the DAC's actual hardware clock frequency.
     * E.g., nominal 44100Hz = 5.5125 fpmf, but DAC may report 5.5127 (44101.6Hz).
     * Without calibration, the drift causes periodic pops as the DAC's internal
     * buffer underflows or overflows.
     */
    double calibratedFpmf;

    /**
     * Residual buffer for bytes that didn't fill a complete URB at the end of
     * a write() call. Prepended to the next call's data to avoid
     * micro-discontinuities (pops) at buffer boundaries. Heap-allocated with
     * urbBufferSize capacity, because the leftover can be just short of one
     * whole URB and that is no longer a compile-time constant.
     */
    uint8_t *residualBuffer;
    int residualCapacity;
    int residualBytes;

    // ── Continuous feedback ─────────────────────────────────────
    /**
     * Dedicated URB for the async feedback endpoint. Submitted at start
     * and continuously recycled during streaming. When the reap loop
     * gets this URB instead of an audio URB, it updates calibratedFpmf
     * and resubmits — tracking the DAC's actual clock in real-time.
     * Effective polling interval is ~1 ms (the host controller schedules
     * the feedback endpoint once per microframe).
     */
    struct usbdevfs_urb *feedbackUrb;
    uint8_t feedbackBuffer[4];
    bool feedbackInFlight;
};

// ── Functions accessible from native-audio-engine ──────────────────

/**
 * Submit PCM data (already in the target bit depth) to the USB pipeline.
 * Blocks when the URB ring is full (natural backpressure from DAC clock).
 */
void submitPcmToUrbs(UsbAudioContext *ctx, const uint8_t *pcmData, int totalBytes);

/** 16-bit → 32-bit: shift left 16. */
void padInt16ToInt32(const uint8_t *src, uint8_t *dst, int numSamples);

/** 24-bit packed (3 bytes/sample) → 32-bit: sign-extend + shift left 8. */
void padInt24ToInt32(const uint8_t *src, uint8_t *dst, int numSamples);

/** int32 (24-bit sign-extended from libFLAC) → 32-bit: shift left 8. */
void shiftInt32From24(const uint8_t *src, uint8_t *dst, int numSamples);
