# decent-player

> A "Decent" Android music player with bit-perfect USB audio output.

Ships a custom direct USB Audio Class 2.0 driver that bypasses the entire Android audio stack — no resampling, no mixer, no compromise.

---

## What makes it indecent

Android resamples everything to 48kHz before sending it to your USB DAC. Your 24-bit/96kHz FLAC? Butchered to 16-bit/48kHz by the time it reaches your $500 dongle.

Every other app accepts this. We didn't.

**decent-player** includes a from-scratch USB audio driver that talks directly to your DAC via isochronous USB transfers — completely bypassing AudioFlinger, ALSA, and the kernel audio driver. The bits that come out of the decoder are the exact bits your DAC receives. Nothing in between.

---

## Libraries

Two standalone libraries that any Android app can use:

### `com.decent:usb-audio-driver`
Core USB Audio Class 2.0 driver. Native C++ with JNI. Handles device detection, descriptor parsing, clock control, isochronous URB pipeline, and float-to-integer conversion with bit-perfect math.

### `com.decent:usb-audio-wrapper-media3`
Drop-in ExoPlayer/Media3 `AudioSink` wrapper. Adds a dedicated streaming thread, automatic sample rate switching, and xHCI-verified transition sequences. Five lines to integrate.

```kotlin
// That's it. Bit-perfect USB audio in your Media3 app.
val delegate = DefaultAudioSink.Builder(context)
    .setEnableFloatOutput(true)
    .build()
return UsbAudioSink(delegate, context)
```

See [Getting Started](docs/libs/GETTING_STARTED.md) for the full quick-start guide.

---

## The driver

- Bypasses **AudioFlinger**, **AudioTrack**, **AAudio**, **ALSA**, and the kernel `snd-usb-audio` driver
- Sends PCM data directly via **Linux usbdevfs isochronous transfers**
- Auto-detects **Clock Source ID** and **optimal bit depth** from USB descriptors
- Supports any sample rate the DAC advertises (44.1kHz — 384kHz)
- Works on **stock Android 10+**, no root required
- Pipeline of 20 isochronous URBs with dedicated streaming thread for glitch-free output
- Protocol-matched rate transition sequence (from xHCI ftrace analysis)
- Bit-perfect float-to-integer conversion (x2^N scaling, mathematically lossless for 16/24-bit)

### Verified

| Device | Android | DAC | Status |
|--------|---------|-----|--------|
| Samsung Galaxy S26 Ultra | 16 | Cayin RU7 | Bit-perfect confirmed |
| iBasso DX340 | 13 | Cayin RU7 | Bit-perfect confirmed |

### How we know it's bit-perfect

```
USB driver binding:     usbfs (ours, NOT snd-usb-audio)
ALSA USB card:          none (kernel doesn't touch the DAC)
AudioFlinger output:    SPEAKER only (not USB)
Qualcomm PAL:           zero USB activity
CLOCK_VALID:            true (DAC confirms clock locked)
Float conversion:       x2^N round-trip (exact for 16/24-bit)
URB pipeline:           20 in-flight, zero drops, zero timeouts
Streaming thread:       dedicated, decoupled from ExoPlayer
```

---

## Why this exists

Before this, bit-perfect USB audio on Android was not available as open-source. Google's own Media3/ExoPlayer team has an [open issue](https://github.com/androidx/media/issues/415) requesting this since 2023. No open-source solution existed.

**Until now.**

---

## Status

**The driver and libraries work. The player is a proof-of-concept.**

This repo contains:
- Two standalone libraries (`libs/`) ready for integration in any Media3 app
- Complete technical documentation of the USB audio driver and libraries
- Investigation notes, USB protocol analysis, xHCI ftrace analysis
- Proof-of-concept integration inside a Felicity Music Player fork (`driver/Felicity/`)

---

## Documentation

### Library docs (for developers integrating the libraries)

| Document | What's inside |
|----------|---------------|
| [Getting Started](docs/libs/GETTING_STARTED.md) | Quick-start guide, 5-line integration example |
| [Integration Guide](docs/libs/INTEGRATION_GUIDE.md) | Full setup for Media3 apps and standalone driver usage |
| [Architecture](docs/libs/ARCHITECTURE.md) | Pipeline diagram, thread model, rate transitions, bit-perfect math |

### Driver investigation docs (deep technical reference)

| Document | What's inside |
|----------|---------------|
| [Executive Summary](docs/driver/01-executive-summary.md) | What we built and why it matters |
| [Investigation Journey](docs/driver/02-investigation-journey.md) | The full story — every dead end and breakthrough |
| [Technical Architecture](docs/driver/03-technical-architecture.md) | Data flow, components, USB protocol details |
| [Five Critical Bugs](docs/driver/04-five-critical-bugs.md) | Each bug that caused silence — and the fix |
| [Cayin RU7 Reference](docs/driver/05-cayin-ru7-hardware-reference.md) | Complete hardware analysis with raw USB descriptors |
| [USB Protocol Analysis](docs/driver/06-usb-protocol-analysis.md) | USB audio protocol analysis via xHCI ftrace |
| [Verification Guide](docs/driver/07-verification-and-diagnostics.md) | How to prove bit-perfect is actually happening |
| [Descriptor Parsing](docs/driver/08-usb-descriptor-parsing.md) | Auto-detecting DAC capabilities from USB descriptors |
| [Future Work](docs/driver/09-future-work.md) | Known limitations and roadmap |
| [Samsung Specifics](docs/driver/10-samsung-s26-ultra-specifics.md) | UHQA, Qualcomm PAL, kernel race condition |
| [Library Architecture](docs/driver/11-standalone-library-architecture.md) | How to package the driver for any Android app |

### Hardware traces

| File | What's inside |
|------|---------------|
| [xHCI Trace — Rate Transitions](docs/hardware/xhci-trace-rate-transitions.txt) | 449k-line ftrace capture of exact transition sequence |
| [USB DAC Behavior Analysis](docs/hardware/usb-dac-behavior-analysis.md) | Analysis of USB audio protocol behavior on Samsung S26 Ultra |
| [Cayin RU7 USB Analysis](docs/hardware/cayin-ru7-usb-analysis.md) | Raw USB descriptor dump and clock source mapping |

---

## License

The USB audio driver and libraries are original work — not derived from any existing project.

The proof-of-concept was developed inside a fork of [Felicity Music Player](https://github.com/Hamza417/Felicity) (AGPL v3) by [Hamza417](https://github.com/Hamza417). The final decent-player app will be built from scratch.

---

<p align="center">
  <i>Built because your music deserves better than 48kHz.</i>
</p>
