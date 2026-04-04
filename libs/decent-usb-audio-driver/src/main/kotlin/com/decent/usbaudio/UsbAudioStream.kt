package com.decent.usbaudio

import android.util.Log

/**
 * JNI handle for a direct USB audio output stream.
 *
 * This class is the Kotlin counterpart of the native [UsbAudioContext] defined
 * in [usb-audio-output.cpp]. It sends PCM data directly to a USB Audio Class 2.0
 * DAC via Linux usbdevfs isochronous transfers, bypassing the entire Android
 * audio stack (AudioFlinger, AudioTrack, AAudio).
 *
 * Lifecycle:
 * ```
 * UsbAudioStream(fd, iface, ep, ...)
 *     .setAltSetting(1)        // select format (bit depth)
 *     .setSampleRate(44100, 41) // configure DAC clock
 *     .start()
 *     .write(floatPcm)         // audio thread
 *     .stop()
 *     .release()
 * ```
 *
 * @param fd               File descriptor from [android.hardware.usb.UsbDeviceConnection.getFileDescriptor]
 * @param interfaceId      Audio streaming interface number (typically 1)
 * @param endpointOut      Isochronous OUT endpoint address (e.g. 0x01)
 * @param endpointFeedback Feedback IN endpoint address (e.g. 0x81), or 0 if unused
 * @param sampleRate       Initial sample rate in Hz
 * @param channelCount     Number of channels (1=mono, 2=stereo)
 * @param bitDepth         Bits per sample (16, 24, or 32)
 * @param maxPacketSize    Max packet size from endpoint descriptor
 *
 * @author DecentPlayer project
 */
class UsbAudioStream(
        fd: Int,
        interfaceId: Int,
        endpointOut: Int,
        endpointFeedback: Int,
        sampleRate: Int,
        channelCount: Int,
        bitDepth: Int,
        maxPacketSize: Int
) {

    private var nativeHandle: Long = 0L

    init {
        nativeHandle = nativeUsbAudioCreate(
                fd, interfaceId, endpointOut, endpointFeedback,
                sampleRate, channelCount, bitDepth, maxPacketSize
        )
        if (nativeHandle == 0L) {
            Log.e(TAG, "nativeUsbAudioCreate returned 0 — check logcat for native errors")
        }
    }

    /** True when the native context exists and the stream hasn't fatally errored. */
    val isReady: Boolean
        get() = nativeHandle != 0L

    /**
     * Select alternate setting on the USB streaming interface.
     * This determines the active format (bit depth) of the endpoint.
     *
     * @param altSetting Alternate setting number (1-4 typically)
     * @return True on success
     */
    fun setAltSetting(altSetting: Int): Boolean {
        if (nativeHandle == 0L) return false
        return nativeUsbAudioSetAltSetting(nativeHandle, altSetting)
    }

    /**
     * Configure the DAC's sample rate via UAC2 SET_CUR control request.
     *
     * @param sampleRateHz  Target sample rate (e.g. 44100, 96000)
     * @param clockSourceId UAC2 Clock Source entity ID. Pass 0 to skip
     *                      (some DACs auto-detect from the data stream).
     * @return True on success
     */
    fun setSampleRate(sampleRateHz: Int, clockSourceId: Int = 0): Boolean {
        if (nativeHandle == 0L) return false
        return nativeUsbAudioSetSampleRate(nativeHandle, sampleRateHz, clockSourceId)
    }

    /**
     * Start the USB audio stream.
     */
    fun start(): Boolean {
        if (nativeHandle == 0L) return false
        return nativeUsbAudioStart(nativeHandle)
    }

    /**
     * Write interleaved float32 PCM to the USB DAC.
     *
     * The native layer converts to the target bit depth and sends via
     * isochronous USB transfer. This call blocks until the transfer completes.
     *
     * @param pcmBuffer Interleaved float PCM; length = frameCount * channelCount
     */
    fun write(pcmBuffer: FloatArray) {
        if (nativeHandle == 0L) return
        nativeUsbAudioWrite(nativeHandle, pcmBuffer)
    }

    /**
     * Stop the USB audio stream.
     */
    fun stop() {
        if (nativeHandle == 0L) return
        nativeUsbAudioStop(nativeHandle)
    }

    /**
     * Release all native resources. The instance must not be used after this.
     */
    fun release() {
        if (nativeHandle == 0L) return
        nativeUsbAudioDestroy(nativeHandle)
        nativeHandle = 0L
        Log.i(TAG, "UsbAudioStream released")
    }

    // JNI declarations

    private external fun nativeUsbAudioCreate(
            fd: Int, interfaceId: Int, endpointOut: Int, endpointFeedback: Int,
            sampleRate: Int, channelCount: Int, bitDepth: Int, maxPacketSize: Int
    ): Long

    private external fun nativeUsbAudioSetAltSetting(handle: Long, altSetting: Int): Boolean
    private external fun nativeUsbAudioSetSampleRate(handle: Long, sampleRateHz: Int, clockSourceId: Int): Boolean
    private external fun nativeUsbAudioStart(handle: Long): Boolean
    private external fun nativeUsbAudioWrite(handle: Long, pcmBuffer: FloatArray)
    private external fun nativeUsbAudioStop(handle: Long)
    private external fun nativeUsbAudioDestroy(handle: Long)

    companion object {
        private const val TAG = "UsbAudioStream"

        init {
            System.loadLibrary("decent_usb_audio")
        }

        /**
         * Perform a USB port reset on the device. Resets the DAC's clock state.
         * The fd remains valid but all interface claims are released.
         * @return 0 on success, negative on error
         */
        @JvmStatic
        external fun nativeUsbReset(fd: Int): Int
    }
}
