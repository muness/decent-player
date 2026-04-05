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
 * Number of isochronous packets per URB submission.
 * At USB high-speed (125us microframes), 8 packets = 1ms of audio.
 */
#define USB_AUDIO_PACKETS_PER_URB 8

/**
 * Number of URBs in the ring buffer.
 * (removed) uses ~74 URBs. We use 64 for a ~64ms pipeline buffer.
 */
#define USB_AUDIO_NUM_URBS 16

/**
 * Max bytes per URB data buffer.
 * Worst case: 384kHz * 4 bytes * 2 channels / 8000 microframes * 8 packets
 *           = 384 * 8 = 3072 bytes per URB. Round up generously.
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
    int32_t maxPacketSize;

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
};
