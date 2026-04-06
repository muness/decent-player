package com.decent.usbaudio.media3

import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.util.Log
import androidx.annotation.OptIn
import androidx.media3.common.C
import androidx.media3.common.Format
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.audio.AudioSink
import androidx.media3.exoplayer.audio.DefaultAudioSink
import androidx.media3.exoplayer.audio.ForwardingAudioSink
import com.decent.usbaudio.NativeAudioEngine
import com.decent.usbaudio.UsbAudioDevice
import com.decent.usbaudio.UsbAudioStream
import java.io.File
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

    /** File path of the current track. Set by the app on each track transition.
     *  When non-null and pointing to a FLAC file, the native audio engine is used. */
    var currentTrackPath: String? = null

    /** Call from FelicityPlayerService.onMediaItemTransition to clean up a finished engine.
     *  @return true if an engine was cleaned up (caller should restart playback). */
    fun cleanupFinishedEngine(): Boolean {
        val engine = nativeEngine
        if (engine != null && !engine.isRunning) {
            engine.destroy()
            nativeEngine = null
            activeEnginePath = null
            windowOffsetUs = -1L
            usbStartMediaTimeNeedsInit = true
            Log.i(TAG, "cleanupFinishedEngine: old engine cleared")

            // Apply deferred USB reconfiguration (cross-rate transition)
            if (hasDeferredConfig) {
                Log.i(TAG, "cleanupFinishedEngine: applying deferred config rate=$deferredRate")
                configureUsbBitPerfect(deferredRate, deferredChannels, deferredEncoding)
                hasDeferredConfig = false
            }
            return true
        }
        return false
    }

    /** Call from FelicityPlayerService AFTER setting currentTrackPath.
     *  Creates a native engine if the USB stream is ready and no engine exists.
     *  Replaces the streaming thread fallback if one was set up due to rate mismatch. */
    fun createEngineIfNeeded() {
        if (nativeEngine?.isRunning == true) return  // already running
        val stream = usbAudioStream
        if (stream != null && stream.isAlive) {
            // Clean up dead engine if exists
            val old = nativeEngine
            if (old != null && !old.isRunning) {
                old.destroy()
                nativeEngine = null
                activeEnginePath = null
            }
            if (nativeEngine == null) {
                windowOffsetUs = -1L
                usbStartMediaTimeNeedsInit = true
                startNativeEngineIfFlac(stream)
                // Resume immediately at position 0 — don't wait for handleBuffer
                val engine = nativeEngine
                if (engine != null) {
                    engine.seek(0)
                    engine.resume()
                    engineNeedsInitialSeek = false
                }
                Log.i(TAG, "createEngineIfNeeded: engine=${nativeEngine != null}")
            }
        }
    }

    private var usbAudioStream: UsbAudioStream? = null
    private val usbAudioDevice = UsbAudioDevice.getInstance(context)
    private var usbStreamingThread: UsbStreamingThread? = null
    private var nativeEngine: NativeAudioEngine? = null
    private val engineLock = Any()

    private var currentEncoding: Int = C.ENCODING_PCM_16BIT
    private var currentSampleRate: Int = 0
    private var currentChannelCount: Int = 0
    private var pendingVolume: Float = 1f
    private var delegateMuted: Boolean = false
    private var handleBufferCallCount: Long = 0

    /**
     * Media timeline offset captured from the first buffer's presentationTimeUs
     * after each flush/init. Maps framesWritten=0 to the correct song position.
     * DefaultAudioSink calls this startMediaTimeUs internally.
     */
    private var usbStartMediaTimeUs: Long = 0L
    private var usbStartMediaTimeNeedsInit: Boolean = true
    private var handledEndOfStream: Boolean = false

    /** ExoPlayer's window offset, captured once per track. Never reset by flush.
     *  Used to convert between ExoPlayer timeline and FLAC absolute position. */
    private var windowOffsetUs: Long = -1L

    /** True when the engine was just created and needs its first seek from handleBuffer.
     *  Prevents play() from resuming the engine before the correct position is known. */
    private var engineNeedsInitialSeek: Boolean = false

    /** Path of the file the current native engine is decoding. Used to detect track changes. */
    private var activeEnginePath: String? = null

    /** Max queue entries before returning false for backpressure (paces ExoPlayer).
     *  Pause responsiveness is handled by pauseStreaming(), not queue size. */
    private val QUEUE_BACKPRESSURE_THRESHOLD = 16

    /** Tracks ExoPlayer's play/pause state so seek-while-paused doesn't auto-resume. */
    private var isPlaying = false

    /** Deferred USB reconfiguration — applied after engine finishes playing. */
    private var deferredRate: Int = 0
    private var deferredChannels: Int = 0
    private var deferredEncoding: Int = 0
    private var hasDeferredConfig: Boolean = false


    override fun configure(inputFormat: Format, specifiedBufferSize: Int, outputChannels: IntArray?) {
        val enc = inputFormat.pcmEncoding
        if (enc != Format.NO_VALUE) currentEncoding = enc

        // If native engine is still playing the SAME track AND the rate didn't change,
        // don't touch it. This happens when ExoPlayer pre-buffers the next track ~10s
        // before EOF. But if the track or rate changed, destroy and reconfigure.
        if (nativeEngine?.isRunning == true) {
            val trackChanged = currentTrackPath != activeEnginePath
            if (!trackChanged) {
                // Same track, ExoPlayer pre-buffering — defer reconfiguration
                if (inputFormat.sampleRate != currentSampleRate || inputFormat.channelCount != currentChannelCount) {
                    deferredRate = inputFormat.sampleRate
                    deferredChannels = inputFormat.channelCount
                    deferredEncoding = enc
                    hasDeferredConfig = true
                    Log.i(TAG, "configure: engine running, pre-buffer — deferred rate=${inputFormat.sampleRate}")
                } else {
                    Log.i(TAG, "configure: engine running, same rate — keeping alive")
                }
                super.configure(inputFormat, specifiedBufferSize, outputChannels)
                muteDelegateIfNeeded()
                return
            }
            // Track changed (manual skip) — destroy engine and proceed
            Log.i(TAG, "configure: track changed, destroying engine")
        }
        // Track changed or engine finished — destroy old engine
        val oldEngine = nativeEngine
        if (oldEngine != null) {
            oldEngine.stop()
            oldEngine.destroy()
            nativeEngine = null
            activeEnginePath = null
            Log.i(TAG, "configure: destroyed old engine")
        }

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
                windowOffsetUs = -1L
                usbStartMediaTimeNeedsInit = true
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

            // Fallback engine creation: if no engine and no streaming thread,
            // try creating one now (path and USB rate should both be correct by this point)
            if (nativeEngine == null && usbStreamingThread == null) {
                startNativeEngineIfFlac(stream)
                // If engine was just created paused, seek to 0 and resume
                val newEngine = nativeEngine
                if (newEngine != null && engineNeedsInitialSeek) {
                    newEngine.seek(0)
                    if (isPlaying) newEngine.resume()
                    engineNeedsInitialSeek = false
                }
            }

            // Capture media timeline offset from first buffer (needed for position tracking)
            if (usbStartMediaTimeNeedsInit) {
                usbStartMediaTimeUs = maxOf(0L, presentationTimeUs)
                usbStartMediaTimeNeedsInit = false
                // Save window offset once per track (not reset by flush/seek)
                if (windowOffsetUs < 0) windowOffsetUs = usbStartMediaTimeUs
                Log.i(TAG, "startMediaTimeUs=$usbStartMediaTimeUs windowOffset=$windowOffsetUs")

                // After a flush (seek) or initial start, seek the native engine
                // to the correct position and resume it.
                val engine = nativeEngine
                if (engine != null && windowOffsetUs >= 0) {
                    val flacPositionUs = presentationTimeUs - windowOffsetUs
                    if (flacPositionUs >= 0) {
                        engine.seek(flacPositionUs)
                        if (isPlaying) engine.resume()
                        engineNeedsInitialSeek = false
                        Log.i(TAG, "Native engine seek to ${flacPositionUs / 1_000_000}s (playing=$isPlaying)")
                    }
                }
            }

            // Native FLAC engine handles decode+USB directly — ignore ExoPlayer data
            val engine = nativeEngine
            if (engine != null) {
                if (engine.isRunning) {
                    buffer.position(buffer.limit())
                    return true
                }
                // Engine finished playing — clean up for next track.
                // Lazy creation at the top of handleBuffer will create a new engine
                // with the correct currentTrackPath on the next call.
                Log.i(TAG, "Native engine finished — cleaning up for next track")
                engine.destroy()
                nativeEngine = null
                activeEnginePath = null
                windowOffsetUs = -1L
                usbStartMediaTimeNeedsInit = true
                // Return true for this buffer — next handleBuffer will create new engine
                buffer.position(buffer.limit())
                return true
            }

            val thread = usbStreamingThread ?: return true

            // Backpressure: if queue is nearly full, tell ExoPlayer to retry later.
            // This paces the renderer to the USB DAC's consumption rate without
            // depending on the delegate AudioTrack.
            if (thread.queueSize() >= QUEUE_BACKPRESSURE_THRESHOLD) {
                return false
            }

            handleBufferCallCount++
            val snapshot: ByteBuffer = buffer.slice().order(buffer.order())

            if (currentEncoding == C.ENCODING_PCM_FLOAT) {
                val totalSamples = snapshot.remaining() / 4
                if (totalSamples > 0) {
                    val floatBuf = FloatArray(totalSamples)
                    snapshot.asFloatBuffer().get(floatBuf)
                    if (handleBufferCallCount <= 3) {
                        Log.i(TAG, "handleBuffer #$handleBufferCallCount: FLOAT samples=$totalSamples")
                    }
                    thread.enqueue(floatBuf)
                }
            } else {
                val remaining = snapshot.remaining()
                if (remaining > 0) {
                    val rawBytes = ByteArray(remaining)
                    snapshot.get(rawBytes)
                    if (handleBufferCallCount <= 3) {
                        val bps = PcmUtils.bytesPerSample(currentEncoding)
                        Log.i(TAG, "handleBuffer #$handleBufferCallCount: RAW ${bps*8}bit bytes=$remaining")
                    }
                    thread.enqueueRaw(rawBytes, currentEncoding)
                }
            }

            // Advance buffer and return true — no delegate dependency.
            buffer.position(buffer.limit())
            return true
        }

        unmuteDelegateIfNeeded()
        return super.handleBuffer(buffer, presentationTimeUs, encodedAccessUnitCount)
    }

    // ── Position tracking via USB framesWritten ────────────────────────

    private var posLogCount = 0L

    override fun getCurrentPositionUs(sourceEnded: Boolean): Long {
        if (config.bitPerfectEnabled) {
            val streamAlive = usbAudioStream?.isAlive == true
            val engine = nativeEngine
            val engineCreated = engine?.isCreated == true

            if (++posLogCount % 500 == 1L) {
                Log.i(TAG, "getPositionUs: streamAlive=$streamAlive engine=$engineCreated " +
                        "window=$windowOffsetUs enginePos=${engine?.getPositionUs()}")
            }

            // Native engine: absolute FLAC position + window offset
            if (streamAlive && engineCreated && windowOffsetUs >= 0) {
                return windowOffsetUs + engine!!.getPositionUs()
            }

            // ExoPlayer pipeline fallback: relative framesWritten + startMediaTime
            if (streamAlive) {
                if (usbStartMediaTimeNeedsInit) return AudioSink.CURRENT_POSITION_NOT_SET
                val frames = usbAudioStream?.framesWritten ?: 0L
                return if (currentSampleRate > 0) {
                    usbStartMediaTimeUs + frames * C.MICROS_PER_SECOND / currentSampleRate
                } else AudioSink.CURRENT_POSITION_NOT_SET
            }
        }
        return super.getCurrentPositionUs(sourceEnded)
    }

    override fun isEnded(): Boolean {
        if (config.bitPerfectEnabled) {
            val engine = nativeEngine
            // Engine still running → not ended
            if (engine != null && engine.isRunning) return false
            // Engine finished or null → delegate to super (which checks playToEndOfStream)
        }
        return super.isEnded()
    }

    override fun hasPendingData(): Boolean {
        if (config.bitPerfectEnabled) {
            // Engine running → has pending data
            if (nativeEngine?.isRunning == true) return true
            if (usbStreamingThread?.hasPendingData() == true) return true
        }
        return super.hasPendingData()
    }

    override fun playToEndOfStream() {
        handledEndOfStream = true
        // Always propagate to delegate — ExoPlayer needs this signal to
        // detect end-of-stream and transition to the next track.
        super.playToEndOfStream()
    }

    override fun play() {
        super.play()
        isPlaying = true
        val resumed = if (!engineNeedsInitialSeek) { nativeEngine?.resume(); true } else false
        usbStreamingThread?.resumeStreaming()
        Log.i(TAG, "play() needsSeek=$engineNeedsInitialSeek resumed=$resumed")
    }

    override fun pause() {
        isPlaying = false
        if (!engineNeedsInitialSeek) nativeEngine?.pause()
        usbStreamingThread?.pauseStreaming()
        super.pause()
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
        // Native engine handles its own flush/seek internally
        // ExoPlayer pipeline: flush queue + native stream
        usbStreamingThread?.flush()
        usbAudioStream?.flush()
        usbStartMediaTimeNeedsInit = true
        handledEndOfStream = false
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
        // NOTE: engine is NOT destroyed here. configure() returns early if engine
        // is still running. If we reach here, the engine is already dead or null.

        // Cache check — avoid needless USB stream recreation
        if (sampleRate == currentSampleRate && channelCount == currentChannelCount
            && usbAudioStream?.isAlive == true) {
            Log.d(TAG, "USB stream cached for rate=$sampleRate ch=$channelCount — reusing")
            // Engine will be created lazily in handleBuffer when currentTrackPath is set
            return
        }

        if (usbAudioStream != null) releaseUsbStream()

        val usbDevice = usbAudioDevice.findUsbAudioDevice() ?: return
        var deviceInfo = usbAudioDevice.openDevice(usbDevice)
        if (deviceInfo == null) {
            Log.e(TAG, "Failed to open USB device")
            return
        }

        // Always use the DAC's highest supported bit depth (standard practice).
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

        // ─── xHCI-verified transition sequence (from USB protocol analysis) ───
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

        usbAudioStream = stream
        currentSampleRate = sampleRate
        currentChannelCount = channelCount
        muteDelegateIfNeeded()

        // Try to create engine now (works for first track where onMediaItemTransition
        // fired before configure). For subsequent tracks, createEngineIfNeeded() in
        // onMediaItemTransition handles it (path is correct by then).
        startNativeEngineIfFlac(stream)

        Log.i(TAG, "USB bit-perfect stream ACTIVE: rate=$sampleRate ch=$channelCount " +
                "bits=$bitDepth device=${deviceInfo.deviceName}")
    }

    /** Try to start a native FLAC engine. Falls back to ExoPlayer streaming thread. */
    @Synchronized
    private fun startNativeEngineIfFlac(stream: UsbAudioStream) {
        if (nativeEngine != null) return  // already created (synchronized method)

        // Stop existing streaming thread (mutually exclusive with native engine)
        usbStreamingThread?.stop()
        usbStreamingThread = null

        val path = currentTrackPath
        if (path != null && path.lowercase().endsWith(".flac")) {
            val engine = NativeAudioEngine()
            try {
                val fd = android.os.ParcelFileDescriptor.open(
                    File(path), android.os.ParcelFileDescriptor.MODE_READ_ONLY
                )
                val created = engine.createFromFd(fd.fd, stream.nativeHandle)
                fd.close()
                if (created && engine.start()) {
                    // Verify FLAC sample rate matches USB stream — prevents distortion
                    // when ExoPlayer's queue and onMediaItemTransition disagree about
                    // which track is playing (e.g., cross-album Recently Played lists).
                    if (engine.getSampleRate() != currentSampleRate) {
                        Log.w(TAG, "Rate mismatch: FLAC=${engine.getSampleRate()} USB=$currentSampleRate" +
                                " — falling back to ExoPlayer pipeline")
                        engine.stop()
                        engine.destroy()
                    } else {
                        // Start paused — will resume in handleBuffer after capturing
                        // the correct seek position from ExoPlayer's presentationTimeUs.
                        engine.pause()
                        nativeEngine = engine
                        engineNeedsInitialSeek = true
                        activeEnginePath = path
                        Log.i(TAG, "Native FLAC engine started (paused, awaiting seek) for: ${File(path).name}")
                        return
                    }
                }
            } catch (e: Exception) {
                Log.w(TAG, "Native engine failed: ${e.message}")
            }
            engine.destroy()
        }

        // Fallback: ExoPlayer pipeline via streaming thread
        usbStreamingThread = UsbStreamingThread(stream).also { it.start() }
        Log.i(TAG, "Using ExoPlayer pipeline (non-FLAC or engine failed)")
    }

    // ── USB stream release ──────────────────────────────────────────

    private fun releaseUsbStream() {
        val stream = usbAudioStream ?: return
        usbAudioStream = null

        // Stop USB stream FIRST — sets ctx->running=false, which unblocks
        // submitPcmToUrbs inside the native engine's decode thread.
        // Without this, nativeEngine.stop() deadlocks on pthread_join.
        stream.stop()

        // Now safe to stop native engine (decode thread can exit)
        nativeEngine?.stop()
        nativeEngine?.destroy()
        nativeEngine = null

        // Stop the streaming thread (drains queue, joins thread)
        usbStreamingThread?.stop()
        usbStreamingThread = null

        // Drain ALL in-flight URBs — MUST complete before setAlt(0)
        val drained = stream.drainUrbs()
        Log.i(TAG, "USB stream drained $drained URBs")

        // Release native context
        stream.release()

        // Keep device connection open between tracks (standard practice)
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
