# Documentation

## USB Audio Driver

The core technical documentation for the bit-perfect USB audio driver.

| # | Document | Description |
|---|----------|-------------|
| 📋 | [Executive Summary](driver/01-executive-summary.md) | What we built and why it matters |
| 🗺️ | [Investigation Journey](driver/02-investigation-journey.md) | The full story — every dead end and breakthrough |
| 🏗️ | [Technical Architecture](driver/03-technical-architecture.md) | Data flow, components, USB protocol details |
| 🐛 | [Five Critical Bugs](driver/04-five-critical-bugs.md) | Each bug that caused silence — and the fix |
| 🔧 | [Cayin RU7 Reference](driver/05-cayin-ru7-hardware-reference.md) | Complete hardware analysis with raw USB descriptors |
| 🕵️ | [USB Protocol Analysis](driver/06-usb-protocol-analysis.md) | USB audio protocol analysis via xHCI ftrace |
| ✅ | [Verification Guide](driver/07-verification-and-diagnostics.md) | How to prove bit-perfect is actually happening |
| 🧬 | [Descriptor Parsing](driver/08-usb-descriptor-parsing.md) | Auto-detecting DAC capabilities from USB descriptors |
| 🔮 | [Future Work](driver/09-future-work.md) | Known limitations and roadmap |
| 📱 | [Samsung Specifics](driver/10-samsung-s26-ultra-specifics.md) | UHQA, Qualcomm PAL, kernel race condition |
| 📦 | [Library Architecture](driver/11-standalone-library-architecture.md) | How to package the driver for any Android app |

## Investigation Notes

Raw notes from the development session.

| Document | Description |
|----------|-------------|
| [Bit-Perfect Briefing](investigation/bitperfect-briefing.md) | Original project briefing (pt-BR) |
| [Bit-Perfect Achieved](investigation/bitperfect-achieved.md) | The moment it worked |
| [Progress Log](investigation/progress-usb-driver.md) | Session progress snapshots |

## Hardware Analysis

| Document | Description |
|----------|-------------|
| [Cayin RU7 USB Analysis](hardware/cayin-ru7-usb-analysis.md) | Full USB dump analysis |
| [Clock Source Discovery](hardware/cayin-ru7-clock-source.md) | How we found the correct clock entity ID |
| [USB DAC Behavior](hardware/usb-dac-behavior-analysis.md) | USB audio runtime observation on Samsung S26 Ultra |
