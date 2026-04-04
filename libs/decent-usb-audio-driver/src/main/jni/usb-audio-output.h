/**
 * @file usb-audio-output.h
 * @brief Direct USB audio output context for bit-perfect playback.
 *
 * Bypasses the entire Android audio stack (AudioFlinger, AudioTrack, AAudio)
 * by communicating directly with a USB Audio Class 2.0 DAC via Linux usbdevfs
 * isochronous transfers.
 *
 * The file descriptor is obtained from Android's UsbDeviceConnection on the
 * Kotlin side and passed to the native layer via JNI. All USB protocol
 * operations (interface claiming, alternate setting selection, sample rate
 * configuration, isochronous data transfer) are performed via ioctl() on
 * that fd.
 *
 * @author DecentPlayer project
 */

#pragma once

#include <atomic>
#include <cstdint>

/**
 * Maximum number of isochronous packets per URB submission.
 * At USB high-speed (125µs microframes), 64 packets = 8ms of audio.
 */
#define USB_AUDIO_MAX_PACKETS_PER_URB 64

/**
 * Number of URBs to keep in flight for double-buffering.
 * While one URB is being sent by the host controller, we fill the next one.
 */
/**
 * (removed) uses ~74 URBs in flight. We use a smaller number but still enough
 * for continuous streaming. Each URB = 64 packets × 125µs = 8ms.
 * 8 URBs = 64ms of audio buffered, which is plenty.
 */
#define USB_AUDIO_NUM_URBS 8

/**
 * Maximum bytes per isochronous packet.
 * Cayin RU7 reports max_packet_size = 776 bytes.
 * 32-bit stereo at 384kHz = 384000 * 4 * 2 / 8000 = 384 bytes/microframe.
 * We round up generously to support any reasonable configuration.
 */
#define USB_AUDIO_MAX_PACKET_SIZE 1024

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

    /** Currently active alternate setting on the streaming interface. */
    int altSetting;

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

    /** True after start(), cleared by stop(). */
    std::atomic<bool> running;

    /**
     * Scratch buffer for PCM format conversion (float → int16/24/32).
     * Sized to hold one URB's worth of audio data.
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
};
