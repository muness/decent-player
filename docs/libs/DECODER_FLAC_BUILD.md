# decent-media3-decoder-flac

Native FLAC decoder for AndroidX Media3 / ExoPlayer, built from source (xiph/flac + media3 JNI).

ExoPlayer auto-detects this decoder via reflection. When present, FLAC files are decoded natively instead of using FFmpeg, delivering raw integer PCM (no float conversion) for true bit-perfect output.

## Setup

Before building, you must clone the xiph/flac source code:

```bash
cd libs/decent-media3-decoder-flac
./setup.sh
```

Or manually:

```bash
cd libs/decent-media3-decoder-flac/src/main/jni
git clone https://github.com/xiph/flac.git --depth=1 libflac
```

## Build

```bash
cd libs
./gradlew :decent-media3-decoder-flac:assembleDebug
```

## Requirements

- Android NDK 29+
- CMake 3.21+
- The `libflac/` directory must exist (run setup.sh first)

## Output format

With `enableFloatOutput = false`:

| FLAC source | Output encoding | Description |
|-------------|-----------------|-------------|
| 16-bit | `PCM_16BIT` | Raw int16, zero conversion |
| 24-bit | `PCM_32BIT` | int32, sign-extended from 24-bit |

With `enableFloatOutput = true`:

| FLAC source | Output encoding | Description |
|-------------|-----------------|-------------|
| 16-bit | `PCM_FLOAT` | float32 (÷2^15) |
| 24-bit | `PCM_FLOAT` | float32 (÷2^23) |

## License

- Java/JNI sources: Apache 2.0 (from [androidx/media](https://github.com/androidx/media))
- libFLAC: BSD-like (from [xiph/flac](https://github.com/xiph/flac))
