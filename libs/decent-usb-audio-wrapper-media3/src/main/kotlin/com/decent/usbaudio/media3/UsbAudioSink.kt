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

/**
 * ExoPlayer AudioSink that sends PCM directly to a USB DAC via isochronous
 * transfers for bit-perfect output, bypassing the entire Android audio stack.
 */
@OptIn(UnstableApi::class)
class UsbAudioSink(
    private val delegate: DefaultAudioSink,
    private val context: Context,
    private val config: UsbAudioSinkConfig = UsbAudioSinkConfig()
) : ForwardingAudioSink(delegate) {

    private var usbAudioStream: UsbAudioStream? = null
    private val usbAudioDevice = UsbAudioDevice.getInstance(context)

    private var currentEncoding: Int = C.ENCODING_PCM_16BIT
    private var currentSampleRate: Int = 0
    private var currentChannelCount: Int = 0
    private var pendingVolume: Float = 1f
    private var delegateMuted: Boolean = false

    override fun configure(inputFormat: Format, specifiedBufferSize: Int, outputChannels: IntArray?) {
        val enc = inputFormat.pcmEncoding
        if (enc != Format.NO_VALUE) currentEncoding = enc
        val sr = inputFormat.sampleRate.takeIf { it > 0 }
        val ch = inputFormat.channelCount.takeIf { it > 0 }

        // Bit-perfect USB path: configure USB FIRST, then delegate
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
        if (config.bitPerfectEnabled && stream?.isReady == true) {
            muteDelegateIfNeeded()
            val snapshot: ByteBuffer = buffer.slice().order(buffer.order())
            val consumed = super.handleBuffer(buffer, presentationTimeUs, encodedAccessUnitCount)
            if (consumed) writeSnapshotToUsb(snapshot, stream)
            return consumed
        }
        unmuteDelegateIfNeeded()
        return super.handleBuffer(buffer, presentationTimeUs, encodedAccessUnitCount)
    }

    override fun play() {
        super.play()
        if (config.bitPerfectEnabled && usbAudioStream?.isReady == true) {
            muteDelegateIfNeeded()
            usbAudioStream?.start()
        }
    }

    override fun pause() {
        super.pause()
        usbAudioStream?.stop()
    }

    override fun setVolume(volume: Float) {
        pendingVolume = volume
        if (config.bitPerfectEnabled && usbAudioStream?.isReady == true) {
            muteDelegateIfNeeded()
        } else {
            unmuteDelegateIfNeeded()
        }
    }

    override fun flush() {
        super.flush()
        usbAudioStream?.apply { stop(); start() }
    }

    override fun reset() {
        super.reset()
        releaseUsbStream()
    }

    override fun release() {
        releaseUsbStream()
        super.release()
    }

    // -- Exact copy of the working configureUsbBitPerfect from AaudioAudioSink --

    private fun configureUsbBitPerfect(sampleRate: Int, channelCount: Int, encoding: Int) {
        // Reuse if same rate/channels AND stream is still functional
        val existing = usbAudioStream
        if (existing != null && existing.isReady
            && sampleRate == currentSampleRate && channelCount == currentChannelCount) {
            Log.d(TAG, "USB stream already configured for rate=$sampleRate ch=$channelCount")
            return
        }

        if (usbAudioStream != null) {
            releaseUsbStream()
        }

        val device = usbAudioDevice.findUsbAudioDevice() ?: return
        val deviceInfo = usbAudioDevice.openDevice(device)
        if (deviceInfo == null) {
            Log.e(TAG, "Failed to open USB device")
            return
        }

        val bitDepth = deviceInfo.bestBitDepth
        val altSetting = deviceInfo.bestAltSetting
        Log.i(TAG, "Auto-detected: alt=$altSetting bits=$bitDepth " +
                "clockSource=0x${deviceInfo.clockSourceId.toString(16)}")

        Log.i(TAG, "Configuring USB bit-perfect: rate=$sampleRate ch=$channelCount " +
                "bits=$bitDepth alt=$altSetting device=${deviceInfo.deviceName}")

        val stream = UsbAudioStream(
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

        // UAC2: alt=0 → SET_CUR → alt=N
        usbAudioDevice.setAltSetting(0)
        Thread.sleep(10)
        usbAudioDevice.setSampleRate(sampleRate)
        Thread.sleep(50)

        val actualRate = usbAudioDevice.readSampleRate()
        Log.i(TAG, "DAC reports sample rate: $actualRate Hz (requested: $sampleRate Hz)")

        val altResult = usbAudioDevice.setAltSetting(altSetting)
        Log.i(TAG, "Java setAltSetting($altSetting): $altResult")

        if (!stream.start()) {
            Log.e(TAG, "USB stream start failed")
            stream.release()
            return
        }

        usbAudioStream = stream
        currentSampleRate = sampleRate
        currentChannelCount = channelCount
        muteDelegateIfNeeded()

        Log.i(TAG, "USB bit-perfect stream ACTIVE: rate=$sampleRate ch=$channelCount " +
                "bits=$bitDepth device=${deviceInfo.deviceName}")
    }

    private fun writeSnapshotToUsb(snapshot: ByteBuffer, stream: UsbAudioStream) {
        val bps = PcmUtils.bytesPerSample(currentEncoding)
        val totalSamples = snapshot.remaining() / bps
        if (totalSamples <= 0) return

        val floatBuf = FloatArray(totalSamples)
        if (currentEncoding == C.ENCODING_PCM_FLOAT) {
            snapshot.asFloatBuffer().get(floatBuf)
        } else {
            for (i in 0 until totalSamples) {
                floatBuf[i] = PcmUtils.readFloat(snapshot, currentEncoding)
            }
        }
        stream.write(floatBuf)
    }

    private fun releaseUsbStream() {
        usbAudioStream?.release()
        usbAudioStream = null
        usbAudioDevice.closeDevice()
        clearForcedRouting()
        unmuteDelegateIfNeeded()
        Log.i(TAG, "USB audio stream released")
    }

    private fun muteDelegateIfNeeded() {
        if (!delegateMuted) { super.setVolume(0f); delegateMuted = true }
    }

    private fun unmuteDelegateIfNeeded() {
        if (delegateMuted) { super.setVolume(pendingVolume); delegateMuted = false }
    }

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
