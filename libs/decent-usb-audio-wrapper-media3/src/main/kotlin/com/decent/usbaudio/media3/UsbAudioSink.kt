package com.decent.usbaudio.media3

import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.util.Log
import androidx.annotation.OptIn
import androidx.media3.common.C
import androidx.media3.common.Format
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.audio.DefaultAudioSink
import androidx.media3.exoplayer.audio.ForwardingAudioSink
import com.decent.usbaudio.UsbAudioDevice
import com.decent.usbaudio.UsbAudioStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * ExoPlayer [androidx.media3.exoplayer.audio.AudioSink] that sends PCM directly
 * to a USB Audio Class 2.0 DAC via isochronous transfers, bypassing the entire
 * Android audio stack (AudioFlinger, AudioTrack, AAudio).
 *
 * The delegate [DefaultAudioSink] is kept alive (muted) for ExoPlayer's clock
 * and position tracking. Audio data is routed to the USB DAC via a dedicated
 * streaming thread with a producer-consumer queue, decoupling USB timing from
 * the delegate's AudioTrack timing.
 *
 * Usage:
 * ```kotlin
 * val baseSink = DefaultAudioSink.Builder(context)
 *     .setEnableFloatOutput(true)
 *     .build()
 * val usbSink = UsbAudioSink(baseSink, context)
 *
 * // On track change, set the source bit depth for optimal alt setting:
 * usbSink.trackBitDepth = 24  // from file metadata
 * ```
 *
 * @param delegate  The [DefaultAudioSink] owned by the ExoPlayer renderer.
 * @param context   Application context for USB device detection and audio routing.
 * @param config    Configuration options (default: bit-perfect enabled, route to speaker).
 */
@OptIn(UnstableApi::class)
class UsbAudioSink(
    private val delegate: DefaultAudioSink,
    private val context: Context,
    private val config: UsbAudioSinkConfig = UsbAudioSinkConfig()
) : ForwardingAudioSink(delegate) {

    /** Source file bit depth (16, 24, 32). Set by the app on each track transition. */
    var trackBitDepth: Int = 0

    private var usbAudioStream: UsbAudioStream? = null
    private val usbAudioDevice = UsbAudioDevice.getInstance(context)
    private var usbStreamingThread: UsbStreamingThread? = null

    private var currentEncoding: Int = C.ENCODING_PCM_16BIT
    private var currentSampleRate: Int = 0
    private var currentChannelCount: Int = 0
    private var pendingVolume: Float = 1f
    private var delegateMuted: Boolean = false
    private var usbWritePendingForCurrentBuffer: Boolean = true
    private var handleBufferCallCount: Long = 0

    override fun configure(inputFormat: Format, specifiedBufferSize: Int, outputChannels: IntArray?) {
        val enc = inputFormat.pcmEncoding
        if (enc != Format.NO_VALUE) currentEncoding = enc
        handleBufferCallCount = 0
        val sr = inputFormat.sampleRate.takeIf { it > 0 }
        val ch = inputFormat.channelCount.takeIf { it > 0 }

        Log.i(TAG, "configure: pcmEncoding=${when(enc) {
            C.ENCODING_PCM_FLOAT -> "FLOAT"; C.ENCODING_PCM_16BIT -> "16BIT"
            C.ENCODING_PCM_24BIT -> "24BIT"; C.ENCODING_PCM_32BIT -> "32BIT"
            else -> "UNKNOWN($enc)"
        }} rate=${inputFormat.sampleRate} ch=${inputFormat.channelCount}")

        if (config.bitPerfectEnabled && sr != null && ch != null) {
            val device = usbAudioDevice.findUsbAudioDevice()
            if (device != null && usbAudioDevice.hasPermission(device)) {
                configureUsbBitPerfect(sr, ch, enc)
                if (config.forceRouteToSpeaker) forceMediaToSpeaker()
                super.configure(inputFormat, specifiedBufferSize, outputChannels)
                muteDelegateIfNeeded()
                Log.i(TAG, "Delegate configured (muted, routed to speaker)")
                return
            } else if (device != null) {
                Log.w(TAG, "USB DAC found but no permission")
            }
        }

        super.configure(inputFormat, specifiedBufferSize, outputChannels)

        if (usbAudioStream != null && !config.bitPerfectEnabled) {
            releaseUsbStream()
        }
    }

    override fun handleBuffer(
        buffer: ByteBuffer,
        presentationTimeUs: Long,
        encodedAccessUnitCount: Int
    ): Boolean {
        val stream = usbAudioStream
        if (config.bitPerfectEnabled && stream?.isAlive == true) {
            muteDelegateIfNeeded()
            val snapshot: ByteBuffer = buffer.slice().order(buffer.order())

            val thread = usbStreamingThread
            if (thread != null && usbWritePendingForCurrentBuffer) {
                handleBufferCallCount++
                val bps = PcmUtils.bytesPerSample(currentEncoding)
                val totalSamples = snapshot.remaining() / bps
                if (totalSamples > 0) {
                    val floatBuf = FloatArray(totalSamples)
                    if (currentEncoding == C.ENCODING_PCM_FLOAT) {
                        snapshot.asFloatBuffer().get(floatBuf)
                    } else {
                        snapshot.order(ByteOrder.LITTLE_ENDIAN)
                        for (i in 0 until totalSamples) {
                            floatBuf[i] = PcmUtils.readFloat(snapshot, currentEncoding)
                        }
                    }
                    if (handleBufferCallCount <= 3) {
                        val s0 = if (totalSamples > 0) floatBuf[0] else 0f
                        val s1 = if (totalSamples > 1) floatBuf[1] else 0f
                        Log.i(TAG, "handleBuffer #$handleBufferCallCount: encoding=${
                            if (currentEncoding == C.ENCODING_PCM_FLOAT) "FLOAT" else "${bps*8}bit"
                        } samples=$totalSamples first=[$s0, $s1]")
                    }
                    thread.enqueue(floatBuf)
                }
                usbWritePendingForCurrentBuffer = false
            }

            val consumed = super.handleBuffer(buffer, presentationTimeUs, encodedAccessUnitCount)
            if (consumed) usbWritePendingForCurrentBuffer = true
            return consumed
        }

        unmuteDelegateIfNeeded()
        return super.handleBuffer(buffer, presentationTimeUs, encodedAccessUnitCount)
    }

    override fun setVolume(volume: Float) {
        pendingVolume = volume
        if (config.bitPerfectEnabled && usbAudioStream?.isAlive == true) {
            muteDelegateIfNeeded()
        } else {
            unmuteDelegateIfNeeded()
        }
    }

    override fun flush() {
        super.flush()
        usbStreamingThread?.flush()
    }

    override fun reset() {
        super.reset()
        // USB stream survives reset — configure() manages its lifecycle.
        // ExoPlayer calls reset() frequently (track changes, seeks).
        // Killing USB here causes audio to briefly route to the speaker.
    }

    override fun release() {
        releaseUsbStream()
        super.release()
    }

    // ── USB bit-perfect configuration ───────────────────────────────

    private fun configureUsbBitPerfect(sampleRate: Int, channelCount: Int, encoding: Int) {
        // Cache check FIRST — avoid needless stream recreation
        if (sampleRate == currentSampleRate && channelCount == currentChannelCount
            && usbAudioStream?.isAlive == true) {
            Log.d(TAG, "USB stream already configured for rate=$sampleRate ch=$channelCount — keeping it")
            return
        }

        if (usbAudioStream != null) releaseUsbStream()

        val usbDevice = usbAudioDevice.findUsbAudioDevice() ?: return
        var deviceInfo = usbAudioDevice.openDevice(usbDevice)
        if (deviceInfo == null) {
            Log.e(TAG, "Failed to open USB device")
            return
        }

        // Always use the DAC's highest supported bit depth (like (removed)).
        // Sources with lower bit depth are zero-padded in the LSBs.
        val bitDepth = deviceInfo.bestBitDepth
        val altSetting = deviceInfo.bestAltSetting
        Log.i(TAG, "Bit-perfect: source=${trackBitDepth}bit → alt=$altSetting usb=${bitDepth}bit " +
                "clockSource=0x${deviceInfo.clockSourceId.toString(16)}")

        var stream = UsbAudioStream(
            fd = deviceInfo.fd,
            interfaceId = deviceInfo.interfaceId,
            endpointOut = deviceInfo.endpointOutAddress,
            endpointFeedback = deviceInfo.endpointFeedbackAddress,
            sampleRate = sampleRate,
            channelCount = channelCount,
            bitDepth = bitDepth,
            maxPacketSize = deviceInfo.maxPacketSize
        )

        if (!stream.isReady) {
            Log.e(TAG, "USB stream creation failed")
            stream.release()
            return
        }

        // ─── (removed)-matched transition sequence (from xHCI ftrace) ───
        //
        // 1. setAlt(0)       → xHCI Configure Endpoint (FREE old rings)
        // 2. SET_CUR          → write new sample rate to Clock Source
        // 3. GET_CUR          → verify clock accepted (CLOCK_VALID_CONTROL)
        // 4. setAlt(0) AGAIN  → defensive reset after clock change
        // 5. setAlt(N)        → xHCI Configure Endpoint (ALLOC new rings)
        // 6. wait ~47ms       → DAC PLL lock time
        // 7. start            → submit URBs

        // Step 1: setAlt(0) — FREE old ISO rings
        if (!usbAudioDevice.setAltSetting(0)) {
            Log.w(TAG, "setAlt(0) failed — stale fd, reopening device...")
            usbAudioDevice.closeDevice()
            stream.release()
            deviceInfo = usbAudioDevice.openDevice(usbDevice)
            if (deviceInfo == null) {
                Log.e(TAG, "Failed to reopen USB device")
                return
            }
            stream = UsbAudioStream(
                fd = deviceInfo.fd,
                interfaceId = deviceInfo.interfaceId,
                endpointOut = deviceInfo.endpointOutAddress,
                endpointFeedback = deviceInfo.endpointFeedbackAddress,
                sampleRate = sampleRate,
                channelCount = channelCount,
                bitDepth = bitDepth,
                maxPacketSize = deviceInfo.maxPacketSize
            )
            if (!stream.isReady) {
                Log.e(TAG, "USB stream recreation failed after reopen")
                stream.release()
                return
            }
            Log.i(TAG, "Device reopened with fresh fd=${deviceInfo.fd}")
        }
        Log.i(TAG, "Step 1: setAlt(0) — old ISO ring freed")

        // Step 2: SET_CUR — write new sample rate
        usbAudioDevice.setSampleRate(sampleRate)

        // Step 3: GET_CUR(CLOCK_VALID_CONTROL) — verify clock is locked
        val clockValid = usbAudioDevice.readClockValid()
        Log.i(TAG, "Step 2-3: SET_CUR=$sampleRate, CLOCK_VALID=$clockValid")

        // Step 4: setAlt(0) AGAIN — defensive reset after clock change
        usbAudioDevice.setAltSetting(0)
        Log.i(TAG, "Step 4: setAlt(0) again — defensive reset")

        // Step 5: setAlt(N) — ALLOC new ISO rings
        val altResult = usbAudioDevice.setAltSetting(altSetting)
        Log.i(TAG, "Step 5: setAlt($altSetting): $altResult — new ISO ring allocated")

        // Step 6: wait ~47ms — DAC PLL lock time
        Thread.sleep(50)

        if (!stream.start()) {
            Log.e(TAG, "USB stream start failed")
            stream.release()
            return
        }

        usbStreamingThread = UsbStreamingThread(stream).also { it.start() }
        usbAudioStream = stream
        currentSampleRate = sampleRate
        currentChannelCount = channelCount
        muteDelegateIfNeeded()

        Log.i(TAG, "USB bit-perfect stream ACTIVE: rate=$sampleRate ch=$channelCount " +
                "bits=$bitDepth device=${deviceInfo.deviceName}")
    }

    // ── USB stream release ──────────────────────────────────────────

    private fun releaseUsbStream() {
        val stream = usbAudioStream ?: return
        usbAudioStream = null

        // Stop the streaming thread FIRST (drains queue, joins thread)
        usbStreamingThread?.stop()
        usbStreamingThread = null

        // Stop accepting new writes
        stream.stop()

        // Drain ALL in-flight URBs — MUST complete before setAlt(0)
        val drained = stream.drainUrbs()
        Log.i(TAG, "USB stream drained $drained URBs")

        // Release native context
        stream.release()

        // Keep device connection open between tracks ((removed) approach)
        clearForcedRouting()
        unmuteDelegateIfNeeded()
        Log.i(TAG, "USB audio stream released (device kept open)")
    }

    // ── Delegate volume management ──────────────────────────────────

    private fun muteDelegateIfNeeded() {
        if (!delegateMuted) { super.setVolume(0f); delegateMuted = true }
    }

    private fun unmuteDelegateIfNeeded() {
        if (delegateMuted) { super.setVolume(pendingVolume); delegateMuted = false }
    }

    // ── Audio routing helpers ───────────────────────────────────────

    private fun forceMediaToSpeaker() {
        try {
            val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
            val speaker = audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
                .firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER }
            if (speaker != null) {
                delegate.setPreferredDevice(speaker)
                Log.i(TAG, "Delegate routed to speaker")
            }
        } catch (e: Exception) {
            Log.w(TAG, "forceMediaToSpeaker failed: ${e.message}")
        }
    }

    private fun clearForcedRouting() {
        try { delegate.setPreferredDevice(null) } catch (_: Exception) {}
    }

    companion object {
        private const val TAG = "UsbAudioSink"
    }
}
