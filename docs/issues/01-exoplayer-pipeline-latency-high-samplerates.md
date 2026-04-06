# Issue: ExoPlayer Pipeline Latency at High Sample Rates

## Status: Open

## Symptom

On the iBasso DX340, playback of 96kHz and 192kHz content via USB bit-perfect mode suffers periodic audio dropouts (3-10 second silence gaps). The music stops and resumes intermittently.

On the Samsung S26 Ultra (Snapdragon 8 Elite), the same content plays without any dropouts.

A reference USB audio app plays 96/192kHz flawlessly on the same iBasso DX340, proving the hardware is capable.

## Root Cause

The ExoPlayer-based audio pipeline has too many layers between the FLAC decoder and the USB output. On devices with slower CPUs, the decoder can only produce audio at approximately 1x real-time speed for high sample rates. Since the USB DAC consumes at exactly 1x real-time, there is zero headroom — any CPU hiccup (GC, scheduler, I/O) causes the streaming thread to starve and the USB pipeline to drain.

### Evidence from xHCI Traces

The USB streaming thread's queue was monitored during playback:

```
Queue nearly empty: 0 before writeRaw   ← queue empty, no headroom
GAP 9.2s @ 03:28:53                      ← 9.2 seconds of silence
GAP 6.9s @ 03:28:48                      ← another gap
```

Despite a backpressure threshold of 16 (allowing up to 16 buffers ahead), the queue **never exceeded 0-1 entries**. ExoPlayer delivered buffers at exactly the consumption rate.

On the S26 Ultra, the same queue stabilizes at 10-16 entries (the decoder runs at 3-5x real-time).

### The Pipeline Overhead

Current bit-perfect audio path:

```
Disk read (Java I/O)
  → FlacExtractor (Java, calls native libflac via JNI)
    → SampleQueue (Java, in-memory ring buffer)
      → MediaCodecAudioRenderer (Java, render loop)
        → handleBuffer (Kotlin, copies ByteBuffer → ByteArray)
          → ArrayBlockingQueue (Java, producer-consumer)
            → UsbStreamingThread (Kotlin, polls queue)
              → JNI call to nativeWriteRaw (C++)
                → padInt24ToInt32 (C++, bit-depth conversion)
                  → submitPcmToUrbs (C++, USB isochronous transfer)
```

**6 language boundary crossings, 3 JNI transitions, 2 data copies, 1 Java queue.**

A reference app's pipeline:

```
Disk read (native)
  → FLAC decode (native C)
    → USB isochronous transfer (native C)
```

**0 language crossings, 0 JNI, 0 copies, 0 queues.**

### Why Only iBasso?

The iBasso DX340 has a significantly weaker CPU than the Samsung S26 Ultra. The per-layer overhead that is negligible on the S26 becomes the bottleneck on the DX340:

| Operation | S26 Ultra | iBasso DX340 |
|-----------|-----------|--------------|
| FLAC decode 96kHz (1s audio) | ~200ms | ~800ms |
| Pipeline overhead per buffer | ~10ms | ~50ms |
| Total per 1s of audio | ~210ms (~5x headroom) | ~850ms (~1.2x headroom) |
| Effective decode-ahead | 10-16 buffers | 0-1 buffers |

At 192kHz, the DX340 likely exceeds 1x real-time, meaning it literally cannot decode fast enough.

### What Was Ruled Out

1. **USB driver**: Zero URB errors, zero short packets, stable 80 URBs in flight. xHCI traces confirmed clean USB transport during both gaps and normal playback.

2. **LoadControl buffering**: Increased ExoPlayer's minBuffer to 30s and maxBuffer to 60s — queue stayed at 0-1. The bottleneck is decoder throughput, not buffer configuration.

3. **Delegate AudioTrack stalling**: Already decoupled (position tracking via USB framesWritten). The delegate is no longer in the handleBuffer path.

4. **CPU overhead from diagnostics**: Removed ISO packet checks and boundary checks from the hot path. No improvement.

5. **Feedback URB overhead**: Continuous feedback adds ~1000 extra ioctls/sec but doesn't account for the full gap (tested with/without, similar results).

## Proposed Solution: Native Audio Thread

### Architecture

Keep ExoPlayer for control (playlists, media session, UI, seek) but bypass the entire ExoPlayer audio pipeline for bit-perfect USB mode. A native C++ thread handles decode → USB directly:

```
┌─────────────────────────────────────────────────┐
│ ExoPlayer (Java/Kotlin)                         │
│   - Playlist management                         │
│   - Media session (lock screen, notifications)  │
│   - Track metadata                              │
│   - Seek/pause/play control signals             │
└──────────────┬──────────────────────────────────┘
               │ control only (play/pause/seek/track path)
               ▼
┌─────────────────────────────────────────────────┐
│ NativeAudioEngine (C++)                         │
│   - Receives file fd + seek position from Java  │
│   - Decodes FLAC/WAV natively (libflac, direct) │
│   - Writes PCM to USB via submitPcmToUrbs       │
│   - Reports position to Java via JNI callback   │
│   - Single thread: decode → convert → USB       │
└─────────────────────────────────────────────────┘
```

### What Already Exists

| Component | Status | Location |
|-----------|--------|----------|
| libflac (native FLAC decoder) | Built and working | `libs/decent-media3-decoder-flac/src/main/jni/libflac/` |
| flac_parser.cc (FLAC stream parser) | Working (from media3) | `libs/decent-media3-decoder-flac/src/main/jni/flac_parser.cc` |
| USB isochronous output | Working | `libs/decent-usb-audio-driver/src/main/jni/usb-audio-output.cpp` |
| USB device management | Working | `libs/decent-usb-audio-driver/src/main/kotlin/com/decent/usbaudio/UsbAudioDevice.kt` |
| Bit-depth conversion | Working | `padInt16ToInt32`, `padInt24ToInt32`, `shiftInt32From24` in usb-audio-output.cpp |

### What Needs to Be Built

1. **NativeAudioEngine.cpp** — Main native class:
   - `open(fd, seekPositionUs)` — open audio file, seek to position
   - `start()` — begin decode+USB loop
   - `pause()` / `resume()` — pause/resume the loop
   - `seek(positionUs)` — seek within current file
   - `stop()` — stop and clean up
   - `getPositionUs()` — current playback position
   - Decode loop: read FLAC frames → convert bit depth → submitPcmToUrbs

2. **NativeAudioEngine.kt** — Kotlin JNI wrapper:
   - Mirrors the C++ API
   - Receives control signals from ExoPlayer callbacks
   - Reports position for media session

3. **Integration with ExoPlayer**:
   - Custom `AudioSink` that delegates to NativeAudioEngine instead of handling buffers
   - ExoPlayer still decodes metadata and manages the playlist
   - For non-FLAC formats (MP3, AAC): fall back to existing ExoPlayer pipeline (lower bitrate, no throughput issue)

4. **File format support**:
   - FLAC: via libflac (already have)
   - WAV: trivial (read PCM headers, pass raw data)
   - DSD: future (DoP or native DSD)
   - MP3/AAC: stay on ExoPlayer pipeline (lossy, low bitrate, no issue)

### Expected Performance

The native decode loop processes in a single thread without any Java/JNI overhead:

```cpp
void decodeLoop() {
    while (running) {
        // 1. Decode FLAC frame (~4096 samples) — native, ~0.1ms
        int frames = flac_decode_frame(decoder, pcmBuffer, bufferSize);
        
        // 2. Convert bit depth if needed — native, ~0.01ms
        padInt24ToInt32(pcmBuffer, usbBuffer, frames * channels);
        
        // 3. Submit to USB — native, blocks until DAC consumes (~1ms per URB)
        submitPcmToUrbs(ctx, usbBuffer, frames * bytesPerFrame);
    }
}
```

Total overhead per 1ms of audio: ~0.11ms. Even on the iBasso DX340, this leaves ~0.89ms of headroom per millisecond — nearly 10x the needed throughput. Gaps become impossible.

### Risk Assessment

| Risk | Mitigation |
|------|------------|
| Complex integration with ExoPlayer | Keep ExoPlayer for control only; native engine is self-contained |
| File seeking accuracy | Use libflac's seek API (sample-accurate) |
| Format support gaps | Fall back to ExoPlayer pipeline for unsupported formats |
| Position tracking accuracy | Native engine reports position via atomic counter; Kotlin reads via JNI |
| Regression on working devices | Feature flag: use native engine only when USB bit-perfect is active |

### Implementation Priority

1. **Phase 1**: NativeAudioEngine with FLAC support only (covers 90% of hi-res content)
2. **Phase 2**: WAV/AIFF support (trivial after Phase 1)
3. **Phase 3**: ExoPlayer integration (AudioSink that delegates to native engine)
4. **Phase 4**: Seek, gapless playback, track transitions
