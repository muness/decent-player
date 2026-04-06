# USB Audio Pops Analysis — Debug Report

## Symptom

During playback of the same FLAC file:
- **(removed)**: Perfect, clean, bit-perfect sound. ~60-70 URBs in flight.
- **Felicity/decent-player**: 95% perfect, bit-perfect (probably), BUT ~5% has subtle "pops" — tiny clicks most noticeable in quiet/silent passages. Not constant, but present. ~20 URBs in flight.

## Root Cause Analysis — 8 Suspects Ranked by Severity

### #1: Pipeline too shallow — 20 URBs vs (removed)'s ~74 (SEVERITY: HIGH)

**File:** `libs/decent-usb-audio-driver/src/main/jni/usb-audio-output.h` line 28

```cpp
#define USB_AUDIO_NUM_URBS 20   // Comment says "Optimal is ~74" but uses 20
```

20 URBs × 8 packets × 125µs = **20ms of buffer**.
(removed) uses ~74 URBs = **~74ms of buffer**.

Any OS jitter (GC pause, scheduler preemption, context switch) longer than 20ms starves the xHCI — empty microframes sent to DAC — **pop**. With 74ms headroom, these jitters are absorbed silently.

**This is the single most likely cause.** It directly explains why (removed) (same device, same file, same DAC) has zero pops.

**Fix:** Change to `#define USB_AUDIO_NUM_URBS 64` (or 74).

---

### #2: Residual bytes discarded between buffers (SEVERITY: HIGH)

**File:** `libs/decent-usb-audio-driver/src/main/jni/usb-audio-output.cpp` lines 437-440

```cpp
if (b > remaining) {
    b = (remaining / ctx->bytesPerFrame) * ctx->bytesPerFrame;
    if (b <= 0) break;  // ← silently discards leftover bytes!
}
```

When ExoPlayer delivers a buffer that's not an exact multiple of `bytesPerFrame`, the leftover bytes (1-7 bytes depending on format) are **silently dropped**. Next buffer starts fresh, but those lost bytes create a micro-discontinuity at the boundary → **pop**.

Example: 32-bit stereo = 8 bytes/frame. Buffer of 4098 bytes → 512 frames processed, 2 bytes dropped.

The same logic exists in `submitPcmToUrbs()` (line 617-619).

**Fix:** Add a residual buffer to `UsbAudioContext`:
```cpp
uint8_t residualBuffer[32];  // max bytesPerFrame is 8 (32-bit stereo)
int residualBytes;
```
Prepend residual from previous call to current buffer before processing. Clear on start/flush.

---

### #3: No silence pre-fill on stream start (SEVERITY: MEDIUM)

**File:** `libs/decent-usb-audio-driver/src/main/jni/usb-audio-output.cpp` lines 358-379

`nativeUsbAudioStart()` only sets flags and resets counters. It does NOT submit any URBs. The xHCI pipeline is completely empty until the first real audio buffer arrives from ExoPlayer (which may take several milliseconds due to decoder startup, thread scheduling, etc.).

During this gap, the xHCI has nothing to send → empty microframes → **pop at the start of each track**.

(removed) pre-fills the pipeline with silence URBs before starting real audio.

**Fix:** In `nativeUsbAudioStart`, submit ~16 URBs filled with zero (silence) to prime the pipeline. Real audio data will naturally replace them as it arrives.

---

### #4: Drop-oldest queue overflow (SEVERITY: MEDIUM)

**File:** `libs/decent-usb-audio-wrapper-media3/src/main/kotlin/com/decent/usbaudio/media3/UsbStreamingThread.kt` lines 59-68

```kotlin
fun enqueue(floatBuf: FloatArray) {
    val buf = AudioBuffer.FloatBuffer(floatBuf)
    if (!audioQueue.offer(buf)) {
        audioQueue.poll()        // ← DROPS oldest buffer
        audioQueue.offer(buf)
        dropCount++
    }
}
```

Queue capacity is 128. If it fills up and drops a buffer → gap in audio → **pop**. Logging only fires on first 3 drops, then every 100th — most drops are silent.

**Fix:** Consider using `audioQueue.put(buf)` which blocks when full. ExoPlayer handles backpressure via `handleBuffer()` returning false. Alternatively, increase queue capacity, but blocking is more correct since it provides natural backpressure matching the DAC clock.

---

### #5: Memory allocation on every handleBuffer call (SEVERITY: MEDIUM)

**File:** `libs/decent-usb-audio-wrapper-media3/src/main/kotlin/com/decent/usbaudio/media3/UsbAudioSink.kt` lines 117, 129

```kotlin
val floatBuf = FloatArray(totalSamples)  // NEW allocation every call
// or
val rawBytes = ByteArray(remaining)      // NEW allocation every call
```

At 44.1kHz with ~1024-sample buffers, `handleBuffer()` is called ~43 times/second. Each call allocates a fresh array. This creates GC pressure. When Android's GC runs, it pauses all threads momentarily → USB streaming thread stalls → pipeline drains slightly → **pop**.

(removed) likely uses pre-allocated reusable buffers.

**Fix:** Pre-allocate a `FloatArray` and `ByteArray` at stream creation time, resize only when needed (similar to how the C++ `transferBuffer` works). Or use a buffer pool.

---

### #6: Delegate AudioTrack blocking the render thread (SEVERITY: MEDIUM)

**File:** `libs/decent-usb-audio-wrapper-media3/src/main/kotlin/com/decent/usbaudio/media3/UsbAudioSink.kt` line 141

```kotlin
val consumed = super.handleBuffer(buffer, presentationTimeUs, encodedAccessUnitCount)
```

This calls `DefaultAudioSink.handleBuffer()` which writes to the muted `AudioTrack`. If AudioTrack's internal buffer is full, this call **blocks** until space is available. During the block, no new audio is enqueued to USB. If the block exceeds our pipeline depth (20ms), the xHCI starves → **pop**.

The USB enqueue happens before this line, but the *next* call from ExoPlayer only comes after this one returns. So a slow delegate delays the entire pipeline.

**Fix:** This is hard to fix without architectural changes. Increasing URB count (#1) is the practical mitigation — more pipeline depth absorbs longer delegate stalls.

---

### #7: flush() doesn't reset frameAccumulator (SEVERITY: LOW)

**File:** `libs/decent-usb-audio-driver/src/main/jni/usb-audio-output.cpp`

When ExoPlayer seeks, it calls `flush()` on the AudioSink. The Kotlin `flush()` clears the queue (line 161), but there's no JNI call to reset `ctx->frameAccumulator` in the C++ layer. If the accumulator was at 0.7 from the previous buffer and new post-seek data starts, the first packet after seek has a slightly wrong size → possible **pop on seek**.

**Fix:** Add a `nativeFlush()` JNI that resets `frameAccumulator = 0.0` and drains any partial state.

---

### #8: Duplicated URB submission logic (SEVERITY: LOW)

**File:** `libs/decent-usb-audio-driver/src/main/jni/usb-audio-output.cpp`

`nativeUsbAudioWrite` (float path, lines 422-481) has its own inline URB submission loop, while `nativeUsbAudioWriteRaw` calls the shared `submitPcmToUrbs()` (line 700). The float path does NOT use `submitPcmToUrbs`. If there's any subtle difference between the two loops, it could cause different behavior. Not a direct pop cause, but makes debugging harder.

**Fix:** Refactor `nativeUsbAudioWrite` to use `submitPcmToUrbs()` after the float→int conversion, eliminating the duplicated loop.

---

## Recommended Fix Priority

| Priority | Fix | Impact | Effort |
|----------|-----|--------|--------|
| **1** | Increase URBs to 64 | Eliminates most pops from OS jitter | 1 line |
| **2** | Add residual buffer for leftover bytes | Eliminates boundary pops | ~30 lines C++ |
| **3** | Pre-fill silence URBs on start | Eliminates start-of-track pop | ~20 lines C++ |
| **4** | Blocking queue (put instead of poll/offer) | Eliminates drop-induced pops | ~5 lines Kotlin |
| **5** | Pre-allocate buffers in handleBuffer | Reduces GC pressure | ~15 lines Kotlin |
| **6** | Add nativeFlush to reset accumulator | Eliminates seek pops | ~10 lines C++/Kotlin |
| **7** | Refactor float path to use submitPcmToUrbs | Code quality, easier debugging | ~20 lines C++ |

**Fixes #1 + #2 alone likely eliminate 90%+ of the audible pops.**

## Key Comparison: Us vs (removed)

| Aspect | (removed) | Decent Player |
|--------|------|---------------|
| URBs in flight | ~60-74 | 20 |
| Pipeline depth | ~74ms | ~20ms |
| Pre-fill on start | Yes (silence) | No |
| Buffer overflow strategy | Backpressure | Drop oldest |
| Allocation in hot path | Pre-allocated | New array every call |
| Residual byte handling | Carried over | Discarded |
