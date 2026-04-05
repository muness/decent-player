# Architecture

## Audio Pipeline

```
Source File (FLAC/MP3/WAV)
    │
    ▼
FFmpeg Decoder (enableFloatOutput=true)
    │  int16/int24 → float32 (÷2^N, exact for 16/24-bit)
    ▼
ExoPlayer Render Thread
    │  calls handleBuffer() on UsbAudioSink
    ▼
┌─────────────────────────────────────────────┐
│  UsbAudioSink (ForwardingAudioSink)         │
│                                             │
│  1. Snapshot ByteBuffer                     │
│  2. Convert to FloatArray (PCM_FLOAT path)  │
│  3. Enqueue to UsbStreamingThread (~0ms)    │
│  4. Feed delegate for ExoPlayer clock       │
│  5. Return consumed to ExoPlayer            │
└──────────────┬──────────────────────────────┘
               │ ArrayBlockingQueue (128 buffers ≈ 10.8s)
               ▼
┌─────────────────────────────────────────────┐
│  UsbStreamingThread (dedicated thread)      │
│                                             │
│  1. Poll from queue (100ms timeout)         │
│  2. Call usbStream.write(floatBuf)          │
│  3. Native reap provides DAC clock          │
│     backpressure (~1ms per URB)             │
└──────────────┬──────────────────────────────┘
               │ JNI
               ▼
┌─────────────────────────────────────────────┐
│  usb-audio-output.cpp (native C++)          │
│                                             │
│  1. Float → int32 conversion (×2^N, exact)  │
│  2. Build ISO packets (8 per URB)           │
│  3. Submit to pre-allocated ring buffer     │
│  4. Reap completed URBs (FIFO order)        │
│  5. 20 URBs in flight = 20ms pipeline       │
└──────────────┬──────────────────────────────┘
               │ ioctl(USBDEVFS_SUBMITURB)
               ▼
┌─────────────────────────────────────────────┐
│  Linux Kernel (usbdevfs)                    │
│                                             │
│  xHCI host controller schedules ISO packets │
│  at 125µs microframe intervals (USB 2.0     │
│  high-speed). ISO_ASAP flag ensures correct │
│  scheduling.                                │
└──────────────┬──────────────────────────────┘
               │ USB cable
               ▼
           USB DAC
```

## Modules

### `decent-usb-audio-driver` (Core)

Native USB Audio Class 2.0 driver. Communicates directly with the DAC via Linux usbdevfs isochronous transfers.

| Component | File | Role |
|-----------|------|------|
| `UsbAudioDevice` | `UsbAudioDevice.kt` | Device discovery, permissions, descriptor parsing, clock control |
| `UsbAudioStream` | `UsbAudioStream.kt` | JNI wrapper for native stream (create, start, write, stop, drain, release) |
| Native driver | `usb-audio-output.cpp` | URB ring buffer, float→int conversion, ISO packet scheduling |
| Native header | `usb-audio-output.h` | Context struct, ring buffer slots, constants |

### `decent-usb-audio-wrapper-media3` (ExoPlayer Integration)

Drop-in `ForwardingAudioSink` for Media3/ExoPlayer apps.

| Component | File | Role |
|-----------|------|------|
| `UsbAudioSink` | `UsbAudioSink.kt` | ForwardingAudioSink with USB routing, rate transitions, stale fd detection |
| `UsbStreamingThread` | `UsbStreamingThread.kt` | Producer-consumer queue decoupling render thread from USB timing |
| `UsbAudioSinkConfig` | `UsbAudioSinkConfig.kt` | Configuration (bitPerfectEnabled, forceRouteToSpeaker) |
| `PcmUtils` | `PcmUtils.kt` | PCM encoding detection and float conversion |

## Thread Model

```
Thread                  Responsibility                    Blocks on
─────────────────────   ──────────────────────────────    ──────────────
ExoPlayer Render        Decode → handleBuffer → enqueue   Delegate AudioTrack (~5ms)
UsbStreamingThread      Poll queue → write to native      URB reap (~1ms per URB)
Main/UI                 Lifecycle, permissions             N/A
```

The render thread and USB thread are fully decoupled via the queue. The render thread never blocks on USB. The USB thread never blocks on ExoPlayer.

## Rate Transition Sequence

Matches (removed)'s exact behavior observed via xHCI ftrace on iBasso DX340:

```
1. stop()              Stop accepting new writes
2. drainUrbs()         Block until ALL in-flight URBs complete
3. setAlt(0)           xHCI Configure Endpoint: FREE old ISO ring
4. SET_CUR             Write new sample rate to Clock Source entity
5. CLOCK_VALID         GET_CUR verify clock locked (selector 0x02)
6. setAlt(0)           Defensive reset after clock change
7. setAlt(N)           xHCI Configure Endpoint: ALLOC new ISO ring
8. sleep(50ms)         DAC PLL lock time
9. start()             Begin submitting URBs on new ring
```

Steps 3 and 7 MUST use Java `UsbDeviceConnection.setInterface()` — the native `USBDEVFS_SETINTERFACE` ioctl does not trigger the xHCI Configure Endpoint Command properly.

## Samsung xHCI Ring Limitations

The Samsung S26 Ultra (Exynos) allocates ISO endpoint rings with ~256 TRB capacity. Each URB with 8 ISO packets uses ~10 TRBs (8 packets + overhead). This limits the pipeline to ~20 URBs.

| URBs | TRBs | Buffer | Status |
|------|------|--------|--------|
| 16 | ~160 | 16ms | Works (glitchy) |
| 20 | ~200 | 20ms | Works (stable) |
| 24 | ~240 | 24ms | Marginal |
| 32 | ~320 | 32ms | Fails (ring overflow) |
| 64 | ~640 | 64ms | Fails |

Other devices (e.g., iBasso DX340 with Qualcomm/Rockchip SoC) may support larger rings.

## Bit-Perfect Math

### Float Normalization (FFmpeg → Driver)

FFmpeg's libswresample normalizes integer PCM to float by dividing by `2^N`:

```
int16  →  float:  sample / 32768.0f     (÷2^15)
int24  →  float:  sample / 8388608.0f   (÷2^23)
```

The driver reconverts by multiplying by `2^N`:

```
float  →  int16:  sample × 32768.0f  + clamp    (×2^15)
float  →  int24:  sample × 8388608.0f + clamp   (×2^23)
float  →  int32:  sample × 2147483648.0 + clamp (×2^31, via double)
```

This is **mathematically lossless** for 16-bit and 24-bit:
- `2^N` is exactly representable in float32 (power of 2)
- Division/multiplication by a power of 2 only changes the exponent — zero rounding
- Float32 has 24-bit mantissa, exactly covering int16 (16-bit) and int24 (24-bit)

For 32-bit: float32 cannot represent all int32 values (24-bit mantissa < 31 bits needed). The `double` intermediate helps with scaling precision, but the int→float→int round-trip is inherently lossy for 32-bit. True 32-bit bit-perfect requires the raw bytes path (future Fase 3).

### Zero-Padding (Bit Depth Mismatch)

When the source bit depth is less than the DAC's bit depth, samples are zero-padded in the LSBs:

```
16-bit source → 32-bit DAC:  0xABCD → 0xABCD0000 (16 zeros in LSB)
24-bit source → 32-bit DAC:  0xABCDEF → 0xABCDEF00 (8 zeros in LSB)
```

This is standard bit-perfect practice ((removed) does the same). The original bits are preserved in the MSBs. The DAC's internal converter ignores the zero LSBs.
