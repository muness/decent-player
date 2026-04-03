# 🎵 decent-player

> A "Decent" Android music player with bit-perfect USB audio output.

Ships a custom userspace USB Audio Class 2.0 driver that bypasses the entire Android audio stack — no resampling, no mixer, no compromise.

---

## 🤬 What makes it indecent

Android resamples everything to 48kHz before sending it to your USB DAC. Your 24-bit/96kHz FLAC? Butchered to 16-bit/48kHz by the time it reaches your $500 dongle.

Every other app accepts this. We didn't.

**decent-player** includes a from-scratch USB audio driver that talks directly to your DAC via isochronous USB transfers — completely bypassing AudioFlinger, ALSA, and the kernel audio driver. The bits that come out of the decoder are the exact bits your DAC receives. Nothing in between.

---

## ⚡ The driver

🔇 Bypasses **AudioFlinger**, **AudioTrack**, **AAudio**, **ALSA**, and the kernel `snd-usb-audio` driver

🎯 Sends PCM data directly via **Linux usbdevfs isochronous transfers**

🔍 Auto-detects **Clock Source ID** and **optimal bit depth** from USB descriptors

🎛️ Supports any sample rate the DAC advertises (44.1kHz — 384kHz)

📱 Works on **stock Android 13+**, no root required

🔄 Pipeline of 8 isochronous URBs for glitch-free streaming

### ✅ Verified

| Device | Android | DAC | Status |
|--------|---------|-----|--------|
| Samsung Galaxy S26 Ultra | 16 | Cayin RU7 | ✅ Bit-perfect confirmed |
| iBasso DX340 | 13 | Cayin RU7 | ✅ Bit-perfect confirmed |

### 🔬 How we know it's bit-perfect

```
USB driver binding:     usbfs (ours, NOT snd-usb-audio)
ALSA USB card:          none (kernel doesn't touch the DAC)
AudioFlinger output:    SPEAKER only (not USB)  
Qualcomm PAL:           zero USB activity
Feedback endpoint:      44101.6 Hz (DAC hardware clock confirms rate)
URB pipeline:           8 in-flight, zero drops over 578 consecutive samples
```

---

## 🏚️ Prior art

Before this, bit-perfect USB audio on Android was exclusive to closed-source commercial apps:

| App | Price | Source | Approach |
|-----|-------|--------|----------|
| USB Audio Player Pro | $8 | 🔒 Closed | Custom USB driver |


| **decent-player** | **Free** | **🔓 Open** | **Custom USB driver** |

Google's own Media3/ExoPlayer team has an [open issue](https://github.com/androidx/media/issues/415) requesting this since 2023. No open-source solution existed.

**Until now.**

---

## 🐛 Five bugs that took a night to find

Building this driver meant discovering five critical bugs, each of which independently caused **complete silence**:

### 1️⃣ Wrong Clock Source Entity ID
The DAC accepts SET_CUR to non-existent entity IDs without error. Only parsing the raw USB descriptors reveals the real one. Our code brute-forced ID `0x0B` — the real one was `0x05`.

### 2️⃣ Missing `USBDEVFS_URB_ISO_ASAP` flag
Without this single flag (`0x02`), the xHCI host controller silently drops every isochronous packet. The kernel reports success. One byte, hours of debugging.

### 3️⃣ Java vs native `setInterface()`
The native `USBDEVFS_SETINTERFACE` ioctl does NOT allocate isochronous bandwidth in the xHCI scheduler. Only the Java `UsbDeviceConnection.setInterface()` does. Completely undocumented.

### 4️⃣ URB pipeline depth
Single submit-reap produces `#Iso=0` in the host controller. The DAC needs multiple URBs queued simultaneously. (removed) uses 74. We use 8. With 1, silence.

### 5️⃣ Kernel driver race condition
`snd-usb-audio` binds ~3ms after USB connect, before any Android intent fires. It configures the DAC to 384kHz. Our SET_CUR must target the correct clock entity to override this.

> 📖 Full investigation: [`study-case/DRIVER INITIAL DOCUMENTATION/`](study-case/DRIVER%20INITIAL%20DOCUMENTATION/)

---

## 🚧 Status

**The driver works. The player doesn't exist yet.**

This repo currently contains:
- 📄 Complete technical documentation of the USB audio driver
- 🔍 Investigation notes, (removed) reverse engineering, hardware analysis
- 🧪 Proof-of-concept implementation (inside a Felicity Music Player fork)

The goal is to build a standalone music player from scratch with a UI that doesn't look like it was designed in 2012.

---

## 📚 Documentation

| # | Document | What's inside |
|---|----------|---------------|
| 📋 | [Executive Summary](study-case/DRIVER%20INITIAL%20DOCUMENTATION/01_EXECUTIVE_SUMMARY.md) | What we built and why it matters |
| 🗺️ | [Investigation Journey](study-case/DRIVER%20INITIAL%20DOCUMENTATION/02_INVESTIGATION_JOURNEY.md) | The full story — every dead end and breakthrough |
| 🏗️ | [Technical Architecture](study-case/DRIVER%20INITIAL%20DOCUMENTATION/03_TECHNICAL_ARCHITECTURE.md) | Data flow, components, USB protocol details |
| 🐛 | [Five Critical Bugs](study-case/DRIVER%20INITIAL%20DOCUMENTATION/04_FIVE_CRITICAL_BUGS.md) | Each bug that caused silence — and the fix |
| 🔧 | [Cayin RU7 Reference](study-case/DRIVER%20INITIAL%20DOCUMENTATION/05_CAYIN_RU7_HARDWARE_REFERENCE.md) | Complete hardware analysis with raw USB descriptors |
| 🕵️ | [(removed) Analysis](study-case/DRIVER%20INITIAL%20DOCUMENTATION/06_(removed)_REVERSE_ENGINEERING.md) | How USB Audio Player Pro works under the hood |
| ✅ | [Verification Guide](study-case/DRIVER%20INITIAL%20DOCUMENTATION/07_VERIFICATION_AND_DIAGNOSTICS.md) | How to prove bit-perfect is actually happening |
| 🧬 | [Descriptor Parsing](study-case/DRIVER%20INITIAL%20DOCUMENTATION/08_USB_DESCRIPTOR_PARSING.md) | Auto-detecting DAC capabilities from USB descriptors |
| 🔮 | [Future Work](study-case/DRIVER%20INITIAL%20DOCUMENTATION/09_FUTURE_WORK.md) | Known limitations and roadmap |
| 📱 | [Samsung Specifics](study-case/DRIVER%20INITIAL%20DOCUMENTATION/10_SAMSUNG_S26_ULTRA_SPECIFICS.md) | UHQA, Qualcomm PAL, kernel race condition |
| 📦 | [Library Architecture](study-case/DRIVER%20INITIAL%20DOCUMENTATION/11_STANDALONE_LIBRARY_ARCHITECTURE.md) | How to package the driver for any Android app |

---

## 📄 License

The USB audio driver is original work — not derived from any existing project.

The proof-of-concept was developed inside a fork of [Felicity Music Player](https://github.com/Hamza417/Felicity) (AGPL v3). The final decent-player app will be built from scratch.

---

<p align="center">
  <i>Built in one night. Because your music deserves better than 48kHz.</i>
</p>
