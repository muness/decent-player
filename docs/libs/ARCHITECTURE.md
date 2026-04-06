# Architecture

## Audio Pipeline

There are two decoding paths, both bit-perfect. The `UsbAudioSink` handles both automatically.

### Path 1: FFmpeg (float, all formats)

```
Source File (FLAC/MP3/AAC/WAV)
    |
    v
FFmpeg Decoder (enableFloatOutput=true)
    |  int16/int24 -> float32 (/2^N, exact for 16/24-bit)
    v
ExoPlayer Render Thread
    |  calls handleBuffer() on UsbAudioSink
    v
+---------------------------------------------+
|  UsbAudioSink (ForwardingAudioSink)         |
|                                             |
|  1. Snapshot ByteBuffer                     |
|  2. Detect PCM_FLOAT encoding               |
|  3. Convert to FloatArray                   |
|  4. enqueue(FloatArray) to streaming thread  |
|  5. Feed delegate for ExoPlayer clock       |
+----------------+----------------------------+
                 | ArrayBlockingQueue (128 buffers)
                 v
+---------------------------------------------+
|  UsbStreamingThread (dedicated thread)      |
|                                             |
|  1. Poll AudioBuffer.FloatBuffer            |
|  2. Call usbStream.write(floatBuf)          |
|  3. Native reap provides DAC clock          |
|     backpressure (~1ms per URB)             |
+----------------+----------------------------+
                 | JNI
                 v
+---------------------------------------------+
|  usb-audio-output.cpp (native C++)          |
|                                             |
|  1. Float -> int32 conversion (x2^N, exact) |
|  2. Build ISO packets (8 per URB)           |
|  3. Submit to pre-allocated ring buffer     |
|  4. Reap completed URBs (FIFO order)        |
|  5. 20 URBs in flight = ~20ms pipeline      |
+----------------+----------------------------+
                 | ioctl(USBDEVFS_SUBMITURB)
                 v
+---------------------------------------------+
|  Linux Kernel (usbdevfs)                    |
|                                             |
|  xHCI host controller schedules ISO packets |
|  at 125us microframe intervals (USB 2.0     |
|  high-speed). ISO_ASAP flag ensures correct |
|  scheduling.                                |
+----------------+----------------------------+
                 | USB cable
                 v
             USB DAC
```

### Path 2: libFLAC (raw integer, FLAC only)

```
FLAC File
    |
    v
FlacExtractor (libFLAC, extractor level)
    |  Decodes to raw int: PCM_16BIT or PCM_32BIT (24-bit sign-extended)
    v
ExoPlayer Render Thread
    |  calls handleBuffer() on UsbAudioSink
    v
+---------------------------------------------+
|  UsbAudioSink (ForwardingAudioSink)         |
|                                             |
|  1. Snapshot ByteBuffer                     |
|  2. Detect non-float encoding (PCM_16BIT,   |
|     PCM_32BIT, PCM_24BIT)                   |
|  3. Copy raw bytes to ByteArray             |
|  4. enqueueRaw(ByteArray, encoding)         |
|  5. Feed delegate for ExoPlayer clock       |
+----------------+----------------------------+
                 | ArrayBlockingQueue (128 buffers)
                 v
+---------------------------------------------+
|  UsbStreamingThread (dedicated thread)      |
|                                             |
|  1. Poll AudioBuffer.RawBuffer              |
|  2. Call usbStream.writeRaw(bytes, enc)     |
|  3. Native reap provides DAC clock          |
|     backpressure (~1ms per URB)             |
+----------------+----------------------------+
                 | JNI
                 v
+---------------------------------------------+
|  usb-audio-output.cpp (native C++)          |
|                                             |
|  1. Integer shift: pad to DAC bit depth     |
|     (e.g., 24-bit << 8 -> 32-bit)          |
|  2. Build ISO packets (8 per URB)           |
|  3. Submit to pre-allocated ring buffer     |
|  4. 20 URBs in flight = ~20ms pipeline      |
+----------------+----------------------------+
                 | ioctl(USBDEVFS_SUBMITURB)
                 v
             USB DAC
```

**Key difference:** Path 2 has zero float math in the entire pipeline. The original integer samples from the FLAC file reach the DAC with only a lossless bit shift.

## Modules

### `decent-usb-audio-driver` (Core)

Native USB Audio Class 2.0 driver. Communicates directly with the DAC via Linux usbdevfs isochronous transfers.

| Component | File | Role |
|-----------|------|------|
| `UsbAudioDevice` | `UsbAudioDevice.kt` | Device discovery, permissions, descriptor parsing, clock control |
| `UsbAudioStream` | `UsbAudioStream.kt` | JNI wrapper for native stream (create, start, write, writeRaw, stop, drain, release) |
| `UsbAudioPermissionHelper` | `UsbAudioPermissionHelper.kt` | Handle USB_DEVICE_ATTACHED intent, request permission, claim device |
| Native driver | `usb-audio-output.cpp` | URB ring buffer, float->int conversion, raw int padding, ISO packet scheduling |
| Native header | `usb-audio-output.h` | Context struct, ring buffer slots, constants |

### `decent-usb-audio-wrapper-media3` (ExoPlayer Integration)

Drop-in `ForwardingAudioSink` for Media3/ExoPlayer apps.

| Component | File | Role |
|-----------|------|------|
| `UsbAudioSink` | `UsbAudioSink.kt` | ForwardingAudioSink with USB routing, rate transitions, stale fd detection, dual-path handleBuffer (float + raw) |
| `UsbStreamingThread` | `UsbStreamingThread.kt` | Producer-consumer queue decoupling render thread from USB timing; supports FloatBuffer and RawBuffer types |
| `UsbAudioSinkConfig` | `UsbAudioSinkConfig.kt` | Configuration (bitPerfectEnabled, forceRouteToSpeaker) |
| `PcmUtils` | `PcmUtils.kt` | PCM encoding detection, bytes-per-sample, float conversion utilities |

### `decent-media3-decoder-flac` (Optional Native FLAC Decoder)

Native FLAC decoder built from xiph/flac source. Decodes FLAC at the extractor level.

| Component | File | Role |
|-----------|------|------|
| `FlacExtractor` | `FlacExtractor.java` | Decodes FLAC to raw integer PCM at extractor level (before renderer) |
| `LibflacAudioRenderer` | `LibflacAudioRenderer.java` | Marker class for runtime detection via reflection |
| `FlacLibrary` | `FlacLibrary.java` | Native library loader (libflacJNI.so) |

## Thread Model

```
Thread                  Responsibility                    Blocks on
---                     ---                               ---
ExoPlayer Render        Decode -> handleBuffer -> enqueue  Delegate AudioTrack (~5ms)
UsbStreamingThread      Poll queue -> write to native      URB reap (~1ms per URB)
Main/UI                 Lifecycle, permissions             N/A
```

The render thread and USB thread are fully decoupled via the `ArrayBlockingQueue` (capacity 128). The render thread never blocks on USB. The USB thread never blocks on ExoPlayer.

The `UsbStreamingThread` uses a sealed class `AudioBuffer` with two variants (`FloatBuffer` and `RawBuffer`) for type-safe queueing of both paths through a single queue.

## Rate Transition Sequence

Matches the exact behavior observed via xHCI ftrace analysis on iBasso DX340:

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

Steps 3, 6, and 7 MUST use Java `UsbDeviceConnection.setInterface()` -- the native `USBDEVFS_SETINTERFACE` ioctl does not trigger the xHCI Configure Endpoint Command properly.

## Samsung xHCI Ring Limitations

The Samsung S26 Ultra (Exynos) allocates ISO endpoint rings with ~256 TRB capacity. Each URB with 8 ISO packets uses ~10 TRBs (8 packets + overhead). This limits the pipeline to ~20 URBs (the current `USB_AUDIO_NUM_URBS` value).

| URBs | TRBs | Buffer | Status |
|------|------|--------|--------|
| 16 | ~160 | 16ms | Works (glitchy) |
| 20 | ~200 | 20ms | Works (stable) |
| 24 | ~240 | 24ms | Marginal |
| 32 | ~320 | 32ms | Fails (ring overflow) |
| 64 | ~640 | 64ms | Fails |

Other devices (e.g., iBasso DX340 with Qualcomm/Rockchip SoC) may support larger rings.

## Bit-Perfect Math

### Float Normalization (FFmpeg -> Driver)

FFmpeg's libswresample normalizes integer PCM to float by dividing by `2^N`:

```
int16  ->  float:  sample / 32768.0f     (/2^15)
int24  ->  float:  sample / 8388608.0f   (/2^23)
```

The driver reconverts by multiplying by `2^N`:

```
float  ->  int16:  sample x 32768.0f  + clamp    (x2^15)
float  ->  int24:  sample x 8388608.0f + clamp   (x2^23)
float  ->  int32:  sample x 2147483648.0 + clamp (x2^31, via double)
```

This is **mathematically lossless** for 16-bit and 24-bit:
- `2^N` is exactly representable in float32 (power of 2)
- Division/multiplication by a power of 2 only changes the exponent -- zero rounding
- Float32 has 24-bit mantissa, exactly covering int16 (16-bit) and int24 (24-bit)

For 32-bit: float32 cannot represent all int32 values (24-bit mantissa < 31 bits needed). The `double` intermediate helps with scaling precision, but the int->float->int round-trip is inherently lossy for 32-bit.

### Raw Integer Path (libFLAC -> Driver)

When libFLAC delivers raw integer PCM, the native driver pads to the DAC's bit depth using integer shift:

```
16-bit source -> 32-bit DAC:  sample << 16  (0xABCD -> 0xABCD0000)
24-bit source -> 32-bit DAC:  sample << 8   (0xABCDEF -> 0xABCDEF00)
32-bit source -> 32-bit DAC:  pass-through
```

No float math at any point. The original bits are preserved exactly. This is trivially lossless by construction.

### Zero-Padding (Bit Depth Mismatch)

When the source bit depth is less than the DAC's bit depth, samples are zero-padded in the LSBs:

```
16-bit source -> 32-bit DAC:  0xABCD -> 0xABCD0000 (16 zeros in LSB)
24-bit source -> 32-bit DAC:  0xABCDEF -> 0xABCDEF00 (8 zeros in LSB)
```

This is standard bit-perfect practice per USB Audio Class 2.0 spec. The original bits are preserved in the MSBs. The DAC's internal converter ignores the zero LSBs.
