# 01 — Executive Summary: Bit-Perfect USB Audio Driver for Android

**Date:** 2026-04-03
**Project:** DecentPlayer (fork of [Felicity Music Player](https://github.com/Hamza417/Felicity))
**Achievement:** First open-source bit-perfect USB audio driver for Android, bypassing the entire Android audio stack (AudioFlinger, AudioTrack, AAudio, ALSA) via direct isochronous USB transfers.

## What We Built

A native USB Audio Class 2.0 driver that sends PCM audio data directly from the app to a USB DAC via Linux `usbdevfs` isochronous transfers. The driver:

- **Bypasses AudioFlinger completely** — no mixer, no resampler, no volume scaling
- **Sends PCM bit-for-bit** as decoded by ExoPlayer/Media3
- **Supports any sample rate** the DAC advertises (44.1kHz, 48kHz, 88.2kHz, 96kHz, 176.4kHz, 192kHz, 352.8kHz, 384kHz)
- **Auto-detects** Clock Source ID and optimal bit depth from USB descriptors
- **Works without root** on stock Android 13+ devices
- **Generic** — no hardcoding for any specific DAC

## Verified On

| Device | Android | DAC | Status |
|--------|---------|-----|--------|
| Samsung Galaxy S26 Ultra | 16 (API 36) | Cayin RU7 | Working, bit-perfect confirmed |
| iBasso DX340 | 13 (API 33) | Cayin RU7 | Working, bit-perfect confirmed |

## What "Bit-Perfect" Means Here

When our driver is active:
- The **kernel `snd-usb-audio` driver is detached** (confirmed via `/sys/bus/usb/devices/*/driver` → `usbfs`)
- **No ALSA card** exists for the USB device (confirmed via `/proc/asound/cards`)
- **AudioFlinger has zero USB output threads** (confirmed via `dumpsys media.audio_flinger`)
- **Qualcomm PAL has zero USB activity** (confirmed via logcat)
- The **DAC's feedback endpoint confirms the correct clock rate** (e.g., 44101.6 Hz for 44.1kHz source)
- **PCM data goes directly** from ExoPlayer decode → float conversion → isochronous USB packets → DAC hardware

## Prior Art

Before this, the only apps achieving bit-perfect USB audio on Android were:
- No open-source solution existed before this project
- Some apps use `dlsym` hacks for partial bypass


No open-source implementation existed. The Google Media3/ExoPlayer team has an open issue (#415) since 2023 requesting this feature.

## Key Technical Breakthroughs (in order of discovery)

1. **Clock Source Entity ID parsing** — the UAC2 Clock Source ID must be read from USB descriptors, not guessed
2. **`USBDEVFS_URB_ISO_ASAP` flag** — without it, the xHCI host controller silently drops all isochronous packets
3. **Java `setInterface()` for ISO bandwidth** — native `USBDEVFS_SETINTERFACE` does NOT allocate isochronous bandwidth in the xHCI scheduler
4. **Pipeline of 8+ URBs** — the xHCI host controller requires multiple URBs in flight simultaneously; single submit-reap produces silence
5. **32-bit PCM (alt=3) as default** — 32-bit is the standard choice regardless of source bit depth; some DACs may not output audio on 16-bit alt settings

## Architecture

```
ExoPlayer/Media3 decodes file → PCM float
    ↓
AaudioAudioSink.kt (intercepts buffer)
    ↓
UsbAudioOutputProcessor.kt (JNI wrapper)
    ↓
usb-audio-output.cpp (isochronous transfers via usbdevfs ioctl)
    ↓
/dev/bus/usb/XXX/YYY → USB DAC (bit-perfect!)
```

The delegate AudioTrack is configured but muted and routed to the built-in speaker, keeping ExoPlayer's clock and state machine functional without touching the USB device.
