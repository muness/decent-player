# 05 — Cayin RU7 Hardware Reference

Complete technical reference for the Cayin RU7 USB DAC, extracted from USB descriptors and runtime analysis.

## Device Identity

| Field | Value |
|-------|-------|
| Manufacturer | Cayin |
| Product | Cayin RU7 |
| Vendor ID | 0x2D87 |
| Product ID | 0xC002 |
| bcdDevice | 2.01 |
| USB Speed | High-Speed (480 Mbps) |
| USB Audio Class | 2.0 (protocol=0x20) |
| Device Class | 0xEF (Miscellaneous/IAD) |

## USB Descriptor Map

### Configuration (1 configuration, 2 interfaces)

```
Configuration 1: bMaxPower=100mA, 2 interfaces
├── Interface 0: AudioControl (class=1, subclass=1, protocol=0x20)
│   ├── Clock Source (entity 0x05)
│   ├── Input Terminal (entity 0x01, USB Streaming, clock=0x05)
│   ├── Feature Unit (entity 0x03, source=0x01)
│   └── Output Terminal (entity 0x04, Speaker, source=0x03, clock=0x05)
│
└── Interface 1: AudioStreaming (class=1, subclass=2, protocol=0x20)
    ├── Alt 0: Zero-bandwidth (no endpoints)
    ├── Alt 1: PCM 16-bit, 2ch stereo
    ├── Alt 2: PCM 24-bit, 2ch stereo
    ├── Alt 3: PCM 32-bit, 2ch stereo
    └── Alt 4: RAW_DATA (native DSD), 4-byte subslot, 2ch stereo
```

### Clock Source Entity

| Field | Value |
|-------|-------|
| bClockID | **0x05** |
| bmAttributes | 0x03 (internal, non-programmable) |
| bmControls | 0x07 |

**CRITICAL:** This is the entity targeted by SET_CUR/GET_CUR for sample rate control.
`wIndex = (0x05 << 8) | 0 = 0x0500`

### Alt Settings Detail

| Alt | bBitResolution | bSubslotSize | Channels | Channel Config | bmFormats | Format |
|-----|----------------|-------------|----------|----------------|-----------|--------|
| 0 | — | — | — | — | — | Zero-bandwidth |
| 1 | 16 | 2 bytes | 2 | 0x03 (L+R) | `0x00000001` | PCM Type I |
| 2 | 24 | 3 bytes | 2 | 0x03 (L+R) | `0x00000001` | PCM Type I |
| 3 | 32 | 4 bytes | 2 | 0x03 (L+R) | `0x00000001` | PCM Type I |
| 4 | 32 | 4 bytes | 2 | 0x03 (L+R) | `0x80000000` | **RAW_DATA (native DSD)** |

Alt 4 is **not** a fourth PCM setting and **not** DoP. Its AS_GENERAL
`bmFormats` sets D31 (RAW_DATA), which is how UAC2 advertises an opaque
bitstream endpoint — the native-DSD alt setting. DoP needs no dedicated alt
setting at all; it rides inside ordinary PCM frames on alts 1–3.

This is descriptor fact, decoded from the hex dump below. See
[bmFormats decode](#bmformats-decode-from-the-hex-dump).

### Endpoints (same for alt 1-4)

**EP 0x01 OUT — Audio Data:**

| Field | Value |
|-------|-------|
| Address | 0x01 (OUT) |
| Type | Isochronous |
| Sync | Asynchronous (bmAttributes=0x05) |
| Max Packet Size | 776 bytes |
| Interval | 1 (125μs / every microframe) |

**EP 0x81 IN — Feedback:**

| Field | Value |
|-------|-------|
| Address | 0x81 (IN) |
| Type | Isochronous |
| Sync | Feedback (bmAttributes=0x11) |
| Max Packet Size | 4 bytes |
| Interval | 4 (every 4 microframes = 500μs) |

### Supported Sample Rates

Reported by ALSA driver probing:
```
44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000 Hz
```

Note: The `getSupportedMixerAttributes()` Android API does NOT expose 44100 Hz on the Samsung S26 Ultra. This is a HAL-level limitation, not a hardware limitation. The DAC hardware fully supports 44.1kHz (confirmed via direct USB SET_CUR + feedback endpoint).

### Max Packet Size Analysis

776 bytes allows:
- 16-bit stereo (4 bytes/frame): 194 frames → supports up to 1,552,000 Hz
- 24-bit stereo (6 bytes/frame): 129 frames → supports up to 1,032,000 Hz
- 32-bit stereo (8 bytes/frame): 97 frames → supports up to 776,000 Hz

All practical sample rates are well within the max packet size.

### Feedback Endpoint Behavior

Format: UAC2 16.16 fixed-point (4 bytes, little-endian)

| Sample Rate | Expected Feedback | Observed Feedback |
|-------------|-------------------|-------------------|
| 44100 Hz | 5.5125 | 5.5126 — 5.5127 |
| 96000 Hz | 12.0000 | 12.0002 — 12.0004 |
| 384000 Hz | 48.0000 | 48.0013 — 48.0017 |

The DAC's crystal oscillator has a slight positive offset (~0.003%) which is well within USB Audio spec tolerance.

### Firmware Behavior Notes

1. **SET_CUR to non-existent entity IDs**: The RU7 firmware accepts control transfers to any entity ID without STALL. Returns the requested number of bytes. This is non-standard behavior that can mislead developers.

2. **Clock change requires alt=0 first**: Setting sample rate while a streaming alt setting is active may be ignored. The correct sequence is: alt=0 → SET_CUR → alt=N.

3. **Clock PLL lock time**: After SET_CUR, the PLL needs ~50ms to lock to the new frequency. Submitting data too soon after rate change may produce glitches.

### Raw USB Descriptors (hex dump)

```
000000 12 01 00 02 ef 02 01 40 87 2d 02 c0 01 02 01 02
000010 03 01 09 02 37 01 02 01 00 80 32 08 0b 00 02 01
000020 00 20 00 09 04 00 00 00 01 01 20 02 09 24 01 00
000030 02 04 40 00 00 08 24 0a 05 03 07 00 00 11 24 02
000040 01 01 01 00 05 02 03 00 00 00 00 00 00 00 12 24
000050 06 03 01 0f 00 00 00 0c 00 00 00 0c 00 00 00 00
000060 0c 24 03 04 01 03 00 03 05 00 00 00 09 04 01 00
000070 00 01 02 20 04 09 04 01 01 02 01 02 20 00 10 24
000080 01 01 05 01 01 00 00 00 02 03 00 00 00 00 06 24
000090 02 01 02 10 07 05 01 05 08 03 01 08 25 01 00 00
0000a0 00 00 00 07 05 81 11 04 00 04 09 04 01 02 02 01
0000b0 02 20 00 10 24 01 01 05 01 01 00 00 00 02 03 00
0000c0 00 00 00 06 24 02 01 03 18 07 05 01 05 08 03 01
0000d0 08 25 01 00 00 00 00 00 07 05 81 11 04 00 04 09
0000e0 04 01 03 02 01 02 20 00 10 24 01 01 05 01 01 00
0000f0 00 00 02 03 00 00 00 00 06 24 02 01 04 20 07 05
000100 01 05 08 03 01 08 25 01 00 00 00 00 00 07 05 81
000110 11 04 00 04 09 04 01 04 02 01 02 20 00 10 24 01
000120 01 05 01 00 00 00 80 02 03 00 00 00 00 06 24 02
000130 01 04 20 07 05 01 05 08 03 01 08 25 01 00 00 00
000140 00 00 07 05 81 11 04 00 04
```

### bmFormats decode (from the hex dump)

Each AudioStreaming alt setting carries a class-specific **AS_GENERAL**
descriptor (`bLength=0x10`, `bDescriptorType=0x24`, `bDescriptorSubtype=0x01`)
laid out as:

```
byte 0  bLength            = 0x10 (16)
byte 1  bDescriptorType    = 0x24 (CS_INTERFACE)
byte 2  bDescriptorSubtype = 0x01 (AS_GENERAL)
byte 3  bTerminalLink
byte 4  bmControls
byte 5  bFormatType
bytes 6-9   bmFormats      (32-bit little-endian bitmap)
byte 10 bNrChannels
bytes 11-14 bmChannelConfig
byte 15 iChannelNames
```

`bmFormats` bit **D0 = PCM**, bit **D31 = RAW_DATA**.

| Alt | AS_GENERAL bytes | `bmFormats` bytes (LE) | Value | Meaning |
|-----|------------------|------------------------|-------|---------|
| 1 | `10 24 01 01 05 01` `01 00 00 00` `02 03 00 00 00 00` | `01 00 00 00` | `0x00000001` | D0 → PCM |
| 2 | `10 24 01 01 05 01` `01 00 00 00` `02 03 00 00 00 00` | `01 00 00 00` | `0x00000001` | D0 → PCM |
| 3 | `10 24 01 01 05 01` `01 00 00 00` `02 03 00 00 00 00` | `01 00 00 00` | `0x00000001` | D0 → PCM |
| 4 | `10 24 01 01 05 01` `00 00 00 80` `02 03 00 00 00 00` | `00 00 00 80` | `0x80000000` | **D31 → RAW_DATA** |

Alt 4's AS_GENERAL begins at offset `0x11d` in the dump: the bytes
`00 00 00 80` at offsets `0x123`–`0x126` are `bmFormats` little-endian, i.e.
`0x80000000`.

The paired **Format Type I** descriptor (`06 24 02 …`) gives the wire layout
for each alt setting:

| Alt | Format Type bytes | bSubslotSize | bBitResolution |
|-----|-------------------|--------------|----------------|
| 1 | `06 24 02 01 02 10` | 2 | 0x10 = 16 |
| 2 | `06 24 02 01 03 18` | 3 | 0x18 = 24 |
| 3 | `06 24 02 01 04 20` | 4 | 0x20 = 32 |
| 4 | `06 24 02 01 04 20` | 4 | 0x20 = 32 |

Alts 3 and 4 have **identical** Format Type descriptors. Only `bmFormats`
distinguishes them, which is why a scan that looks at `bBitResolution` alone
can mistake the DSD alt setting for a PCM one.

The endpoint descriptor is also the same for every alt setting
(`07 05 01 05 08 03 01`): address `0x01` OUT, `bmAttributes=0x05`
(isochronous, asynchronous), `wMaxPacketSize = 0x0308 = 776`, `bInterval=1`.
