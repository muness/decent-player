# Integration Guide

## Scenario 1: Wrapper + Driver (Media3 / ExoPlayer Apps)

This is the recommended approach for apps that use AndroidX Media3 or ExoPlayer. The wrapper provides a drop-in `ForwardingAudioSink` that handles all USB communication automatically.

### Step 1: Add Dependencies

```gradle
dependencies {
    // Core USB driver (native URB pipeline, JNI)
    implementation 'com.decent:usb-audio-driver:1.0.0'
    
    // ExoPlayer/Media3 AudioSink wrapper
    implementation 'com.decent:usb-audio-wrapper-media3:1.0.0'
    
    // FFmpeg decoder — REQUIRED for bit-perfect non-FLAC formats. The Android
    // built-in decoder truncates 24-bit to 16-bit. FFmpeg delivers genuine
    // float32 for all sources.
    implementation 'org.jellyfin.media3:media3-ffmpeg-decoder:1.9.0+1'
    
    // Optional: Native FLAC decoder (zero-float integer path for FLAC files).
    // When present, FLAC is decoded to raw int PCM at the extractor level —
    // zero float math in the entire pipeline. When absent, FFmpeg handles
    // FLAC via the float path (also bit-perfect via x2^N round-trip).
    implementation 'com.decent:media3-decoder-flac:1.0.0'
    
    // Media3 (your app probably already has these)
    implementation 'androidx.media3:media3-exoplayer:1.9.3'
    implementation 'androidx.media3:media3-common:1.9.3'
    implementation 'androidx.media3:media3-session:1.9.3'
}
```

### Step 2: AndroidManifest.xml — USB Device Handling

The app must register for USB device attach events. This makes Android show a permission dialog when a DAC is plugged in, and ensures your app claims the device before the kernel's `snd-usb-audio` driver binds (~3ms after connection).

```xml
<manifest xmlns:android="http://schemas.android.com/apk/res/android">
    
    <!-- Optional: declare USB host support. Set required=false so the app
         still installs on devices without USB host capability. -->
    <uses-feature android:name="android.hardware.usb.host" android:required="false" />
    
    <application ...>
        <activity android:name=".MainActivity">
            
            <!-- Receive USB DAC connection events -->
            <intent-filter>
                <action android:name="android.hardware.usb.action.USB_DEVICE_ATTACHED" />
            </intent-filter>
            
            <!-- Filter: only USB Audio Class devices (class 1) -->
            <meta-data
                android:name="android.hardware.usb.action.USB_DEVICE_ATTACHED"
                android:resource="@xml/usb_audio_device_filter" />
                
        </activity>
    </application>
</manifest>
```

Create `res/xml/usb_audio_device_filter.xml`:

```xml
<?xml version="1.0" encoding="utf-8"?>
<!-- Matches any USB Audio Class device (class 1 = Audio).
     This makes Android offer your app as a handler when a USB DAC is connected. -->
<resources>
    <usb-device class="1" />
</resources>
```

### Step 3: Handle USB Device Attach in Your Activity

When a USB DAC is connected, your Activity receives the `USB_DEVICE_ATTACHED` intent. You must handle it immediately to claim the device before the kernel's `snd-usb-audio` driver binds (~3ms window):

```kotlin
import com.decent.usbaudio.UsbAudioPermissionHelper

class MainActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Handle USB device attached (app launched by USB connect)
        handleUsbDeviceAttached(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        // Handle USB device attached (app already running)
        handleUsbDeviceAttached(intent)
    }

    private fun handleUsbDeviceAttached(intent: Intent) {
        // UsbAudioPermissionHelper handles:
        // 1. Checking if intent is USB_DEVICE_ATTACHED
        // 2. Finding the USB audio device
        // 3. Requesting permission if needed
        // 4. Claiming the device (openDevice) to prevent kernel driver binding
        UsbAudioPermissionHelper.handleIntent(applicationContext, intent)
    }
}
```

`UsbAudioPermissionHelper` is provided by the `usb-audio-driver` library. It handles the full flow: detect audio device, check/request permission, and claim the device.

### Step 4: Create RenderersFactory with UsbAudioSink

In your player service or activity, create a custom `DefaultRenderersFactory` that:
1. Forces FFmpeg decoder (for bit-perfect float output on non-FLAC formats)
2. Creates `UsbAudioSink` as the audio sink
3. Conditionally enables float output based on libFLAC availability
4. Keeps a reference to the sink for track bit depth updates

```kotlin
import com.decent.usbaudio.media3.UsbAudioSink
import com.decent.usbaudio.media3.UsbAudioSinkConfig
import androidx.media3.exoplayer.DefaultRenderersFactory
import androidx.media3.exoplayer.audio.AudioSink
import androidx.media3.exoplayer.audio.DefaultAudioSink
import androidx.media3.exoplayer.audio.AudioCapabilities

class MyPlayerService : ... {

    // Keep a reference to set trackBitDepth on track transitions
    private var currentUsbSink: UsbAudioSink? = null
    
    private fun createRenderersFactory(): DefaultRenderersFactory {
        val factory = object : DefaultRenderersFactory(this) {
            override fun buildAudioSink(
                context: Context,
                enableFloatOutput: Boolean,
                enableOffload: Boolean
            ): AudioSink {
                // Detect libFLAC at runtime. When present, FLAC files are decoded
                // at the extractor level (FlacExtractor), delivering raw integer PCM.
                val hasLibFlac = try {
                    Class.forName("androidx.media3.decoder.flac.LibflacAudioRenderer")
                    true
                } catch (_: ClassNotFoundException) { false }
                
                // enableFloatOutput logic:
                // - With libFLAC:    false — libFLAC delivers raw int for FLAC,
                //                    The FFmpeg extension still handles non-FLAC as float when
                //                    EXTENSION_RENDERER_MODE_PREFER is set.
                // - Without libFLAC: true  — FFmpeg delivers float32 for everything.
                //                    Required for 24-bit precision (Android's built-in
                //                    decoder truncates 24-bit to 16-bit).
                val useFloat = !hasLibFlac

                val delegate = DefaultAudioSink.Builder(context)
                    .setEnableFloatOutput(useFloat)
                    .setAudioCapabilities(AudioCapabilities.getCapabilities(context))
                    // Add your audio processors here if needed (EQ, balance, etc.)
                    // .setAudioProcessors(arrayOf(...))
                    .build()

                return UsbAudioSink(delegate, context).also {
                    currentUsbSink = it
                }
            }
        }
        
        // Force FFmpeg decoder. The Android built-in MediaCodec decoder
        // outputs PCM_16BIT for all sources, even 24-bit FLAC. FFmpeg outputs
        // genuine PCM_FLOAT with full precision.
        factory.setExtensionRendererMode(
            DefaultRenderersFactory.EXTENSION_RENDERER_MODE_PREFER
        )
        
        return factory
    }
}
```

**How the two decoding paths work:**

| Format | libFLAC in classpath? | Decoder used | Output encoding | Float math? |
|--------|----------------------|-------------|----------------|-------------|
| FLAC | Yes | libFLAC (FlacExtractor) | `PCM_16BIT` or `PCM_32BIT` (raw int) | No |
| FLAC | No | FFmpeg | `PCM_FLOAT` | Yes (x2^N round-trip) |
| MP3, AAC, WAV | Either | FFmpeg | `PCM_FLOAT` | Yes (x2^N round-trip) |

The `UsbAudioSink` handles both paths automatically in `handleBuffer()`:
- `PCM_FLOAT` buffers are converted to `FloatArray` and enqueued via `enqueue()`
- Non-float buffers (raw int from libFLAC) are enqueued as `ByteArray` via `enqueueRaw()`

No app code is needed to distinguish the paths.

**Note on `buildAudioRenderers()`:** You do NOT need to override `buildAudioRenderers()` or remove any renderers. When libFLAC is in the classpath, ExoPlayer's `FlacExtractor` decodes FLAC at the extractor level (before any renderer is consulted), so renderer selection is irrelevant for FLAC files.

### Step 5: Build the ExoPlayer

```kotlin
val player = ExoPlayer.Builder(this)
    .setRenderersFactory(createRenderersFactory())
    .build()
```

### Step 6: Set Track Bit Depth on Each Track Transition

The sink needs to know the source file's bit depth so it can select the correct USB alt setting and verify bit-perfect delivery. Set it **synchronously** in `onMediaItemTransition` — this must complete before ExoPlayer calls `configure()` on the sink:

```kotlin
player.addListener(object : Player.Listener {
    override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) {
        mediaItem?.let { item ->
            val audioId = item.mediaId.toLongOrNull() ?: return@let
            
            // Get bit depth from your metadata source.
            // This MUST be synchronous (runBlocking) so the bit depth is set
            // before ExoPlayer calls configure() on the audio sink.
            val usbSink = currentUsbSink ?: return@let
            runBlocking(Dispatchers.IO) {
                val audio = audioRepository.getAudioById(audioId) ?: return@runBlocking
                usbSink.trackBitDepth = audio.bitPerSample.toInt()  // 16, 24, or 32
            }
        }
    }
})
```

**Why synchronous?** ExoPlayer calls `onMediaItemTransition` then `configure()` on the sink. If `trackBitDepth` isn't set before `configure()` runs, the sink uses the previous track's bit depth for logging/verification.

### Step 7: Configuration Options (Optional)

The default config works for most cases. Customize if needed:

```kotlin
val config = UsbAudioSinkConfig(
    bitPerfectEnabled = true,        // Enable USB output (default: true)
    forceRouteToSpeaker = true       // Route muted delegate to speaker instead of USB
                                     // (prevents AudioFlinger/Qualcomm PAL conflicts)
)

val sink = UsbAudioSink(delegate, context, config)
```

### Complete Working Example

See the Felicity Music Player integration in `driver/Felicity/`:
- **`FelicityPlayerService.kt`** `buildAudioSink()`: Full processor chain, libFLAC detection, conditional `enableFloatOutput`, and `UsbAudioSink` creation
- **`FelicityPlayerService.kt`** `onMediaItemTransition()`: Synchronous bit depth propagation via `runBlocking(Dispatchers.IO)`
- **`MainActivity.kt`** `handleUsbDeviceAttached()`: `UsbAudioPermissionHelper.handleIntent()` for USB device claiming

### Lifecycle Details

The `UsbAudioSink` manages the USB stream lifecycle automatically:

| ExoPlayer Event | What UsbAudioSink Does |
|-------|-------------|
| `configure(format)` | Opens USB device, claims interface, sets sample rate via UAC2 sequence, starts streaming thread |
| Track change (same rate) | Cache hit — stream continues, no reconfiguration |
| Track change (different rate) | Full UAC2 transition: stop thread -> drain URBs -> setAlt(0) -> SET_CUR -> CLOCK_VALID -> setAlt(0) -> setAlt(N) -> sleep(50ms) -> start |
| `flush()` | Clears the streaming queue (for seeks/discontinuities) |
| `reset()` | USB stream **survives** — ExoPlayer calls reset() frequently on track changes, killing USB here causes audio to briefly route to speaker |
| `release()` | Full cleanup: stop streaming thread, drain all URBs, release native resources |
| Stale fd detected | setAlt(0) fails -> auto close/reopen USB device with fresh fd |

### enableFloatOutput: When and Why

With `enableFloatOutput = true` (no libFLAC), the FFmpeg decoder delivers PCM as float32 for **all** sources:

| Source File | Without Float | With Float |
|-------------|--------------|------------|
| 16-bit FLAC/MP3 | PCM_16BIT (correct) | PCM_FLOAT (16->float via /2^15, lossless) |
| 24-bit FLAC | **PCM_16BIT (truncated!)** | PCM_FLOAT (24->float via /2^23, lossless) |
| 32-bit WAV | **PCM_16BIT (truncated!)** | PCM_FLOAT (32->float, lossy -- float32 has only 24-bit mantissa) |

**Without float output and without libFLAC, 24-bit sources lose 8 bits of precision before reaching the USB driver.**

With `enableFloatOutput = false` AND libFLAC in classpath, FLAC files get raw integer PCM:

| Source File | Decoder | Output | Float math? |
|-------------|---------|--------|-------------|
| 16-bit FLAC | libFLAC (FlacExtractor) | PCM_16BIT | None |
| 24-bit FLAC | libFLAC (FlacExtractor) | PCM_32BIT (sign-extended) | None |
| 24-bit MP3 | FFmpeg | PCM_FLOAT | Yes (lossless x2^N) |

The reconversion in the native C++ driver uses `x2^N` scaling with clamping, which is the exact inverse of FFmpeg's `/2^N` normalization. The round-trip is mathematically lossless for 16-bit and 24-bit sources.

---

## Scenario 2: Driver Only (Non-Media3 Apps)

If your app doesn't use ExoPlayer/Media3, you can use the USB driver library directly. You manage the audio pipeline and USB communication yourself.

### Add Dependency

```gradle
dependencies {
    implementation 'com.decent:usb-audio-driver:1.0.0'
}
```

### AndroidManifest.xml

Same USB setup as Scenario 1 (see Step 2 above).

### Open the USB Device

```kotlin
import com.decent.usbaudio.UsbAudioDevice

val usbAudioDevice = UsbAudioDevice.getInstance(context)

// Find a connected USB Audio Class 2.0 device
val usbDevice = usbAudioDevice.findUsbAudioDevice() ?: return
val deviceInfo = usbAudioDevice.openDevice(usbDevice) ?: return

// deviceInfo contains everything auto-detected from USB descriptors:
//   fd                    — file descriptor for usbdevfs ioctls
//   interfaceId           — audio streaming interface number (typically 1)
//   endpointOutAddress    — isochronous OUT endpoint (e.g., 0x01)
//   endpointFeedbackAddress — async feedback IN endpoint (e.g., 0x81)
//   maxPacketSize         — from endpoint descriptor (e.g., 776 bytes for Cayin RU7)
//   bestAltSetting        — highest bit depth alt setting (e.g., 3 for 32-bit)
//   bestBitDepth          — highest supported bit depth (e.g., 32)
//   clockSourceId         — UAC2 Clock Source entity ID (e.g., 0x05)
```

### Configure Sample Rate (UAC2 Transition Sequence)

This sequence must be followed exactly. It was confirmed via xHCI ftrace analysis:

```kotlin
// Step 1: FREE old ISO rings
usbAudioDevice.setAltSetting(0)

// Step 2: SET_CUR — write new sample rate to Clock Source entity
usbAudioDevice.setSampleRate(96000)

// Step 3: GET_CUR(CLOCK_VALID_CONTROL) — verify DAC clock locked
val clockValid = usbAudioDevice.readClockValid()  // should return true

// Step 4: Defensive reset (required after SET_CUR per xHCI protocol analysis)
usbAudioDevice.setAltSetting(0)

// Step 5: ALLOC new ISO rings
usbAudioDevice.setAltSetting(deviceInfo.bestAltSetting)

// Step 6: Wait for DAC PLL to lock onto new frequency
Thread.sleep(50)
```

**Why this exact sequence?** The xHCI host controller uses Configure Endpoint Commands to allocate/free isochronous transfer rings. Sending URBs on a stale ring corrupts the host controller state. xHCI ftrace analysis confirms this exact sequence with two setAlt(0) calls -- the second one after SET_CUR is critical for xHCI ring cleanup.

### Create and Start a Stream

```kotlin
import com.decent.usbaudio.UsbAudioStream

val stream = UsbAudioStream(
    fd = deviceInfo.fd,
    interfaceId = deviceInfo.interfaceId,
    endpointOut = deviceInfo.endpointOutAddress,
    endpointFeedback = deviceInfo.endpointFeedbackAddress,
    sampleRate = 96000,
    channelCount = 2,
    bitDepth = 32,               // must match the alt setting's bit depth
    maxPacketSize = deviceInfo.maxPacketSize
)

if (!stream.isReady) { /* handle error */ }
stream.start()
```

### Write Audio Data

Two write methods are available:

```kotlin
// ── Float path (FFmpeg, or any source delivering float32 PCM) ──

// Write interleaved float32 PCM [-1.0, 1.0].
// The native layer converts to the target bit depth (int16/int24/int32)
// and manages the 20-URB isochronous pipeline automatically.
// This call BLOCKS when the pipeline is full — natural backpressure
// matching the DAC's hardware clock rate.
val floatPcm = FloatArray(8192)  // 4096 stereo frames
// ... fill with your audio data ...
stream.write(floatPcm)

// ── Raw integer path (libFLAC, or any source delivering raw int PCM) ──

// Write raw integer PCM bytes directly — zero float conversion.
// The native layer pads to the DAC's bit depth using integer shift
// operations (<< 8 or << 16). True bit-perfect by construction.
val rawBytes = ByteArray(16384)  // raw PCM bytes
// ... fill with your audio data ...
stream.writeRaw(rawBytes, C.ENCODING_PCM_24BIT)  // or PCM_16BIT, PCM_32BIT
```

For continuous streaming, call `write()` or `writeRaw()` in a loop. Each call blocks for approximately the audio duration of the buffer (~85ms at 96kHz for 8192 samples). This is the DAC's clock driving the pace.

### Stop and Release

```kotlin
// Stop accepting new writes
stream.stop()

// CRITICAL: Drain ALL in-flight URBs before calling setAlt(0).
// The xHCI Configure Endpoint Command triggered by setAlt(0) frees the
// isochronous ring. If URBs are still pending, the host controller state
// becomes corrupted and subsequent transfers will fail silently.
val drained = stream.drainUrbs()

// Release native resources (ring buffer, context)
stream.release()

// Do NOT close the device connection between tracks!
// Closing/reopening corrupts the xHCI endpoint state after ~3 cycles.
// Keep the same UsbDeviceConnection and fd for the entire session.
// Use setAlt(0) -> SET_CUR -> setAlt(N) to change rate on the same fd.
```

### Stale Connection Detection

If the app was killed and restarted, the cached fd may be dead:

```kotlin
// Try setAlt(0) — if it fails, the connection is stale
if (!usbAudioDevice.setAltSetting(0)) {
    // Close and reopen
    usbAudioDevice.closeDevice()
    val freshInfo = usbAudioDevice.openDevice(usbDevice)
    // Recreate stream with fresh fd
}
```

### Important Notes

- **Always drain URBs before setAlt(0)** — pending URBs corrupt the xHCI host controller state
- **Keep the device connection open** between tracks — close/reopen corrupts xHCI after ~3 cycles
- **Use Java `setInterface()`** for alt setting changes — the native `USBDEVFS_SETINTERFACE` ioctl does NOT trigger the xHCI Configure Endpoint Command that properly allocates/frees ISO bandwidth
- **Samsung S26 Ultra xHCI limit** — max ~20 URBs in flight (~256 TRBs ring capacity). Other devices (iBasso DX340, etc.) may support more
- **Float conversion math** — if converting float->int yourself, use `x2^N` (not `x(2^N-1)`) to match FFmpeg's `/2^N` normalization. Use `double` for 32-bit scaling since float32 can't represent 2^31 exactly
