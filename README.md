# DecentPlayer USB Bit-Perfect Audio Driver — Documentation Index

**Achievement:** First open-source bit-perfect USB audio driver for Android.
**Date:** 2026-04-03
**Status:** Working on Samsung S26 Ultra/iBasso DX340 with Cayin RU7 DAC.

## Documents

| # | Document | Description |
|---|----------|-------------|
| 01 | [Executive Summary](01_EXECUTIVE_SUMMARY.md) | What we built, why it matters, key breakthroughs |
| 02 | [Investigation Journey](02_INVESTIGATION_JOURNEY.md) | Chronological story of every step, dead end, and discovery |
| 03 | [Technical Architecture](03_TECHNICAL_ARCHITECTURE.md) | Data flow, component responsibilities, file map, USB protocol details |
| 04 | [Five Critical Bugs](04_FIVE_CRITICAL_BUGS.md) | The 5 bugs that each individually caused silence — and how each was solved |
| 05 | [Cayin RU7 Reference](05_CAYIN_RU7_HARDWARE_REFERENCE.md) | Complete hardware reference: descriptors, endpoints, clock, firmware quirks |
| 06 | [(removed) Reverse Engineering](06_(removed)_REVERSE_ENGINEERING.md) | How USB Audio Player Pro works — learned via system-level observation |
| 07 | [Verification & Diagnostics](07_VERIFICATION_AND_DIAGNOSTICS.md) | How to prove bit-perfect is active, diagnostic commands, metrics |
| 08 | [USB Descriptor Parsing](08_USB_DESCRIPTOR_PARSING.md) | How we auto-detect Clock Source ID and alt settings for any DAC |
| 09 | [Future Work](09_FUTURE_WORK.md) | Known limitations, planned improvements, compatibility roadmap |
| 10 | [Samsung S26 Ultra Specifics](10_SAMSUNG_S26_ULTRA_SPECIFICS.md) | Samsung UHQA, PAL, snd-usb-audio race condition, verified scenarios |

## Quick Start

1. Build: `cd study-case/Felicity && ./gradlew assembleDebug`
2. Install: `adb install -r music/build/outputs/apk/debug/music-debug.apk`
3. Enable: Settings → Engine → Bit-Perfect USB Audio (toggle ON)
4. Connect USB DAC → accept permission dialog
5. Play music → verify via: `adb shell "readlink /sys/bus/usb/devices/1-1:1.1/driver"` → should show `usbfs`
