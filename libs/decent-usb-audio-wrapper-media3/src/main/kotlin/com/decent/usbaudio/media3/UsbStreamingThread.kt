package com.decent.usbaudio.media3

import android.util.Log
import com.decent.usbaudio.UsbAudioStream
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * Dedicated thread for USB audio streaming, decoupled from ExoPlayer's
 * render thread.
 *
 * Problem: ExoPlayer's delegate AudioTrack (muted, routed to speaker)
 * blocks the render thread for ~75ms per buffer. When the delegate
 * returns consumed=false (up to 13 seconds), no new data reaches USB
 * and the DAC plays silence.
 *
 * Solution: A producer-consumer queue between the render thread and
 * a dedicated USB thread. The render thread enqueues PCM data (~0ms),
 * then feeds the delegate. The USB thread pulls from the queue and
 * writes to the DAC at the DAC's own clock rate (via reap backpressure).
 *
 * @param usbStream The native USB audio stream to write to.
 *                  Must only be accessed from the USB thread.
 */
class UsbStreamingThread(private val usbStream: UsbAudioStream) {

    companion object {
        private const val TAG = "UsbStreamingThread"

        /** Queue capacity in buffers. At ~85ms per buffer, 128 buffers ≈ 10.8 seconds.
         *  Large enough to absorb the initial burst when ExoPlayer fills the queue
         *  faster than the USB thread can consume (during pipeline fill). */
        private const val QUEUE_CAPACITY = 128

        /** Poll timeout — thread checks running flag at this interval when idle. */
        private const val POLL_TIMEOUT_MS = 100L
    }

    private val audioQueue = ArrayBlockingQueue<FloatArray>(QUEUE_CAPACITY)

    @Volatile
    private var running = false
    private var thread: Thread? = null

    /**
     * Start the USB streaming thread. Must be called after [UsbAudioStream.start].
     */
    fun start() {
        running = true
        thread = Thread({
            Log.i(TAG, "USB streaming thread started")
            while (running) {
                val buf = audioQueue.poll(POLL_TIMEOUT_MS, TimeUnit.MILLISECONDS)
                if (buf != null) {
                    usbStream.write(buf)
                }
            }
            Log.i(TAG, "USB streaming thread exited")
        }, "UsbStreamingThread").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    /**
     * Enqueue a PCM buffer for USB playback. Non-blocking.
     * If the queue is full, drops the oldest buffer to make room.
     */
    private var dropCount = 0

    fun enqueue(floatBuf: FloatArray) {
        if (!audioQueue.offer(floatBuf)) {
            audioQueue.poll()           // drop oldest
            audioQueue.offer(floatBuf)  // guaranteed space
            dropCount++
            if (dropCount <= 3 || dropCount % 100 == 0) {
                Log.w(TAG, "Queue full, dropped buffer #$dropCount")
            }
        }
    }

    /**
     * Clear the queue. Called on seek/flush to discard stale audio.
     */
    fun flush() {
        audioQueue.clear()
    }

    /**
     * Stop the thread and clear the queue. Blocks until thread exits
     * (up to 2 seconds). Must be called before releasing the USB stream.
     */
    fun stop() {
        running = false
        audioQueue.clear()
        thread?.join(2000)
        thread = null
    }
}
