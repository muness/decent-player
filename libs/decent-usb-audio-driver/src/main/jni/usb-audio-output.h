/**
 * @file usb-audio-output.h
 * @brief Direct USB audio output context for bit-perfect playback.
 *
 * Bypasses the entire Android audio stack (AudioFlinger, AudioTrack, AAudio)
 * by communicating directly with a USB Audio Class 2.0 DAC via Linux usbdevfs
 * isochronous transfers.
 *
 * Pipeline architecture (modeled after (removed)'s xHCI behavior):
 * - 64 URBs in flight for continuous streaming ((removed) uses ~74)
 * - Each URB = 8 packets x 125us = 1ms of audio
 * - Total pipeline buffer: ~64ms
 * - Proper drain-wait-reconfigure sequence on rate transitions
 *
 * The file descriptor is obtained from Android's UsbDeviceConnection on the
 * Kotlin side and passed to the native layer via JNI.
 */

#pragma once

#include <atomic>
#include <cstdint>

/**
 * Number of isochronous packets per URB submission.
 * At USB high-speed (125us microframes), 8 packets = 1ms of audio.
 * Smaller URBs = more granular pipeline, smoother drain.
 */
#define USB_AUDIO_PACKETS_PER_URB 8

/**
 * Number of URBs to keep in flight for continuous streaming.
 * (removed) uses ~74 URBs. We use 64 for a ~64ms pipeline buffer.
 * This provides enough buffering for glitch-free playback while
 * allowing clean drain during rate transitions.
 */
#define USB_AUDIO_NUM_URBS 64

/**
 * Maximum tracked URBs (slightly more than NUM_URBS for safety).
 */
#define USB_AUDIO_MAX_TRACKED_URBS 80

/**
 * Maximum bytes per isochronous packet.
 * Cayin RU7 reports max_packet_size = 776 bytes.
 * 32-bit stereo at 384kHz = 384000 * 4 * 2 / 8000 = 384 bytes/microframe.
 */
#define USB_AUDIO_MAX_PACKET_SIZE 1024

/**
 * URB tracking entry for safe deallocation after reap.
 * Android MTE (Memory Tagging Extension) may modify pointer tags on
 * kernel-returned pointers, making direct free() unsafe. We track our
 * original malloc'd pointers and free those instead.
 */
struct TrackedUrb {
    struct usbdevfs_urb *urb;
    void *buffer;
    bool active;  // true = submitted and not yet reaped
};

/**
 * Aggregate state for one USB audio output stream.
 *
 * Allocated by nativeUsbAudioCreate(), freed by nativeUsbAudioDestroy().
 */
struct UsbAudioContext {
    /** File descriptor from UsbDeviceConnection.getFileDescriptor(). */
    int fd;

    /** USB audio streaming interface number (typically 1). */
    int interfaceId;

    /** Isochronous OUT endpoint address (e.g. 0x01). */
    int endpointOut;

    /** Feedback IN endpoint address (e.g. 0x81). 0 if not used. */
    int endpointFeedback;

    /** Playback sample rate in Hz (e.g. 44100, 96000). */
    int32_t sampleRate;

    /** Number of interleaved channels (1=mono, 2=stereo). */
    int32_t channelCount;

    /** Bits per sample: 16, 24, or 32. */
    int32_t bitDepth;

    /** Bytes per single sample (bitDepth / 8). */
    int32_t bytesPerSample;

    /** Bytes per frame (bytesPerSample * channelCount). */
    int32_t bytesPerFrame;

    /** Max packet size reported by the endpoint descriptor. */
    int32_t maxPacketSize;

    /** True after start(), cleared by stop()/drain. */
    std::atomic<bool> running;

    /**
     * Scratch buffer for PCM format conversion (float -> int16/24/32).
     * Sized to hold one write() call's worth of audio data.
     */
    uint8_t *transferBuffer;

    /** Current capacity of transferBuffer in bytes. */
    int32_t transferBufferCapacity;

    /** Total audio frames written since start (for debug logging). */
    int64_t framesWritten;

    /** Whether the USB interface has been successfully claimed. */
    bool interfaceClaimed;

    /** Number of URBs currently submitted and not yet reaped. */
    int urbsInFlight;

    /**
     * Per-context URB tracking array for MTE-safe deallocation.
     * Each entry tracks the original malloc'd pointers for one URB.
     */
    TrackedUrb trackedUrbs[USB_AUDIO_MAX_TRACKED_URBS];
    int trackedCount;

    /**
     * Fractional accumulator for sample-rate-to-packet-size conversion.
     * Tracks sub-frame remainders across URB boundaries for jitter-free
     * isochronous packet sizing (e.g., 44100/8000 = 5.5125 frames/packet).
     */
    double frameAccumulator;
};
