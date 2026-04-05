# Getting Started

Bit-perfect USB audio output for Android, bypassing the entire Android audio stack (AudioFlinger, AudioTrack, AAudio). Audio goes directly from your app to the USB DAC via Linux usbdevfs isochronous transfers.

## What You Get

- **True bit-perfect output** — no resampling, no format conversion, no volume scaling, no effects
- **Automatic sample rate switching** — DAC switches to match the source file (44.1kHz, 48kHz, 96kHz, 192kHz, etc.)
- **Automatic bit depth handling** — 16-bit and 24-bit sources are zero-padded to the DAC's native format
- **Seamless track transitions** — xHCI-verified transition sequence for reliable rate changes
- **Works with any USB Audio Class 2.0 DAC**

## Requirements

- Android 10+ (API 29+)
- USB Audio Class 2.0 DAC (e.g., Cayin RU7, iFi GO, Questyle M15, etc.)
- App must use AndroidX Media3 / ExoPlayer for audio playback
- FFmpeg decoder recommended for hi-res FLAC (e.g., `org.jellyfin.media3:media3-ffmpeg-decoder`)

## Installation

### Gradle Dependencies

Add both libraries to your app module:

```gradle
dependencies {
    // Core USB driver (native URB pipeline, JNI)
    implementation 'com.decent:usb-audio-driver:1.0.0'
    
    // ExoPlayer/Media3 AudioSink wrapper
    implementation 'com.decent:usb-audio-wrapper-media3:1.0.0'
}
```

### USB Setup

Add to your `AndroidManifest.xml`:

```xml
<!-- Optional: declare USB host support (required=false so the app
     still installs on devices without USB host capability) -->
<uses-feature android:name="android.hardware.usb.host" android:required="false" />
```

Register your activity to receive USB DAC connection events. This lets Android show a permission dialog when a DAC is plugged in, and ensures your app claims the device before the kernel's `snd-usb-audio` driver binds:

```xml
<activity android:name=".MainActivity">
    <intent-filter>
        <action android:name="android.hardware.usb.action.USB_DEVICE_ATTACHED" />
    </intent-filter>
    <meta-data
        android:name="android.hardware.usb.action.USB_DEVICE_ATTACHED"
        android:resource="@xml/usb_audio_device_filter" />
</activity>
```

Create `res/xml/usb_audio_device_filter.xml`:

```xml
<?xml version="1.0" encoding="utf-8"?>
<resources>
    <!-- Matches any USB Audio Class device (class 1 = Audio) -->
    <usb-device class="1" />
</resources>
```

## Quick Integration (5 Lines)

In your `DefaultRenderersFactory.buildAudioSink()` override:

```kotlin
private var usbSink: UsbAudioSink? = null  // keep reference for trackBitDepth

override fun buildAudioSink(
    context: Context,
    enableFloatOutput: Boolean,
    enableOffload: Boolean
): AudioSink {
    val delegate = DefaultAudioSink.Builder(context)
        .setEnableFloatOutput(true)  // Required for 24-bit precision
        .build()
    
    return UsbAudioSink(delegate, context).also { usbSink = it }
}
```

You also need to force FFmpeg decoder and set `EXTENSION_RENDERER_MODE_PREFER` on your `DefaultRenderersFactory`. Without FFmpeg, the Android built-in decoder truncates 24-bit sources to 16-bit.

See the [Integration Guide](INTEGRATION_GUIDE.md) for the complete step-by-step with all details (RenderersFactory setup, track bit depth propagation, stale fd handling, lifecycle, etc.).

## Setting Track Bit Depth (Required for Bit-Perfect)

The sink must know the source file's bit depth to select the correct USB alt setting and verify bit-perfect delivery. Set it **synchronously** on each track change, before ExoPlayer calls `configure()`:

```kotlin
// Keep a reference to the sink (set it in buildAudioSink)
private var usbSink: UsbAudioSink? = null

// In your player's MediaItem transition listener:
player.addListener(object : Player.Listener {
    override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) {
        val sink = usbSink ?: return
        mediaItem?.let { item ->
            // MUST be synchronous — this must complete before ExoPlayer
            // calls configure() on the sink. If your bit depth comes from
            // a database query, use runBlocking:
            runBlocking(Dispatchers.IO) {
                val bitDepth = getTrackBitDepth(item)  // from your metadata/DB
                sink.trackBitDepth = bitDepth           // 16, 24, or 32
            }
        }
    }
})
```

## Sample App

The Felicity music player in `driver/Felicity/` is a complete working example. See `FelicityPlayerService.kt` for the full integration.

## Next Steps

- [Integration Guide](INTEGRATION_GUIDE.md) — detailed setup with all configuration options
- [Architecture](ARCHITECTURE.md) — how the pipeline works under the hood
