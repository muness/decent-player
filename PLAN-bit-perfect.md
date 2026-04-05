# Plano: Bit-Perfect End-to-End — Abordagem libFLAC (estilo (removed))

## Objetivo

Eliminar TODA conversão float do pipeline de áudio FLAC. Os bytes PCM do decoder vão **direto** pro USB DAC sem nenhum cálculo matemático. Mesmo caminho que o (removed) usa.

## Pipeline Atual (com problema)
```
FLAC → FFmpeg → float PCM → FloatArray → JNI → float×32767→int16 (C++) → URB → DAC
                   ↑ conversão 1            ↑ conversão 2 (off-by-one!)
```

## Pipeline Proposto (bit-perfect absoluto)
```
FLAC → media3-decoder-flac (libFLAC) → int16/32 nativo → ByteArray → JNI → memcpy → URB → DAC
                                         ↑ zero conversão                    ↑ zero math
```

---

## Pesquisa: Comportamento do media3-decoder-flac

### Maven artifact:
```groovy
implementation "androidx.media3:media3-decoder-flac:1.9.3"
```

### Classes internas:
- **`LibflacAudioRenderer`** — Renderer customizado (extends `DecoderAudioRenderer`)
- **`FlacDecoder`** — Usa libFLAC via JNI (`FlacDecoderJni`)
- Integra automaticamente com ExoPlayer via `DefaultRenderersFactory` com `EXTENSION_RENDERER_MODE_PREFER`

### Formatos de saída (com `enableFloatOutput = false`):

| Fonte FLAC | `inputBitsPerSample` | Output Encoding | Bytes/sample |
|------------|---------------------|-----------------|-------------|
| 16-bit | 16 | `C.ENCODING_PCM_16BIT` | 2 |
| 24-bit | 24 | `C.ENCODING_PCM_32BIT` | 4 |
| 32-bit | 32 | `C.ENCODING_PCM_32BIT` | 4 |

### Código fonte da decisão (LibflacAudioRenderer):
```java
private @C.PcmEncoding int getOutputPcmEncoding(int inputBitsPerSample) {
    if (shouldOutputFloat) {
        return C.ENCODING_PCM_FLOAT;
    }
    if (inputBitsPerSample == 24 || inputBitsPerSample == 32) {
        return C.ENCODING_PCM_32BIT;  // 24-bit em container int32, sign-extended
    }
    return C.ENCODING_PCM_16BIT;
}
```

### Detalhe crítico sobre 24-bit:
libFLAC entrega samples 24-bit como **int32 sign-extended** (NÃO left-shifted).
- Valor 24-bit `1000` → int32 `1000` (0x000003E8)
- Valor 24-bit `-1` (0xFFFFFF) → int32 `-1` (0xFFFFFFFF)
- Os 3 bytes inferiores (little-endian) SÃO os bytes originais do sample 24-bit

### `shouldOutputFloat` é determinado assim:
```java
shouldOutputFloat = supportsFormatInternal(
    Util.getPcmFormat(C.ENCODING_PCM_FLOAT, channelCount, sampleRate)
) != SINK_FORMAT_UNSUPPORTED;
```
Ou seja: se `enableFloatOutput=false` no DefaultAudioSink → `shouldOutputFloat=false` → saída em inteiro nativo.

**Para nosso path bit-perfect: `enableFloatOutput` DEVE ser `false`** quando USB bit-perfect está ativo. Isso garante que libFLAC entrega inteiro nativo, não float.

---

## Arquivos que precisam mudar

### Visão geral:

| Arquivo | Mudança |
|---------|---------|
| `engine/build.gradle` | Adicionar dep `media3-decoder-flac` |
| `FelicityPlayerService.kt` | Forçar decoder FLAC + `enableFloatOutput=false` quando USB ativo |
| `AaudioAudioSink.kt` | Adicionar branch raw bytes no handleBuffer USB |
| `UsbStreamingThread.kt` | Suportar `ByteArray` além de `FloatArray` na queue |
| `UsbAudioStream.kt` (na lib) | Adicionar `writeRaw(ByteArray, inputBitDepth)` |
| `usb-audio-output.cpp` (na lib) | Adicionar `nativeUsbAudioWriteRaw` com memcpy + padding |
| `usb-audio-output.cpp` (na lib) | TAMBÉM corrigir constantes float (fallback pra non-FLAC) |

---

## Mudança 1: Adicionar dependência media3-decoder-flac

### Arquivo: `driver/Felicity/engine/build.gradle`

Adicionar:
```groovy
implementation "androidx.media3:media3-decoder-flac:1.9.3"
```

Isso inclui `libflacJNI.so` no APK. O ExoPlayer auto-detecta e usa quando `EXTENSION_RENDERER_MODE_PREFER` está ativo.

---

## Mudança 2: Configurar decoder e float output pra USB bit-perfect

### Arquivo: `driver/Felicity/engine/src/main/java/app/simple/felicity/engine/services/FelicityPlayerService.kt`

### 2a. Forçar FLAC decoder (Extension mode PREFER) — ~linha 314
```kotlin
// ANTES:
val extensionMode = if (AudioPreferences.getAudioDecoder() == AudioPreferences.FFMPEG) {
    DefaultRenderersFactory.EXTENSION_RENDERER_MODE_PREFER
} else {
    DefaultRenderersFactory.EXTENSION_RENDERER_MODE_OFF
}

// DEPOIS:
val extensionMode = if (AudioPreferences.isBitPerfectUsbEnabled() ||
                        AudioPreferences.getAudioDecoder() == AudioPreferences.FFMPEG) {
    DefaultRenderersFactory.EXTENSION_RENDERER_MODE_PREFER
} else {
    DefaultRenderersFactory.EXTENSION_RENDERER_MODE_OFF
}
```

Com `EXTENSION_RENDERER_MODE_PREFER`, o ExoPlayer usa media3-decoder-flac pra FLAC (e FFmpeg pra o resto, se disponível). A prioridade é: extension decoder > MediaCodec.

### 2b. DESABILITAR float output quando USB bit-perfect — ~linha 181 e 224
```kotlin
// ANTES:
val hiresEnabled = AudioPreferences.isHiresOutputEnabled()
// ...
.setEnableFloatOutput(hiresEnabled)

// DEPOIS:
val usbBitPerfect = AudioPreferences.isBitPerfectUsbEnabled()
val hiresEnabled = if (usbBitPerfect) false else AudioPreferences.isHiresOutputEnabled()
// ...
.setEnableFloatOutput(hiresEnabled)
```

**Por quê `false`?** Porque com `enableFloatOutput=true`, o LibflacAudioRenderer converte TUDO pra float — exatamente o que queremos EVITAR. Com `false`, libFLAC entrega int16 ou int32 nativo.

---

## Mudança 3: Raw bytes path no handleBuffer

### Arquivo: `driver/Felicity/engine/src/main/java/app/simple/felicity/engine/audio/AaudioAudioSink.kt`

### Trecho atual (linhas 251-263):
```kotlin
if (thread != null && usbWritePendingForCurrentBuffer) {
    val bps = PcmUtils.bytesPerSample(currentEncoding)
    val totalSamples = snapshot.remaining() / bps
    if (totalSamples > 0) {
        val floatBuf = FloatArray(totalSamples)
        if (currentEncoding == C.ENCODING_PCM_FLOAT) {
            snapshot.asFloatBuffer().get(floatBuf)
        } else {
            for (i in 0 until totalSamples) {
                floatBuf[i] = PcmUtils.readFloat(snapshot, currentEncoding)
            }
        }
        thread.enqueue(floatBuf)
    }
    usbWritePendingForCurrentBuffer = false
}
```

### Substituir por:
```kotlin
if (thread != null && usbWritePendingForCurrentBuffer) {
    if (currentEncoding == C.ENCODING_PCM_FLOAT) {
        // Float path (fallback pra formatos non-FLAC via FFmpeg)
        val totalSamples = snapshot.remaining() / 4
        if (totalSamples > 0) {
            val floatBuf = FloatArray(totalSamples)
            snapshot.asFloatBuffer().get(floatBuf)
            thread.enqueue(floatBuf)
        }
    } else {
        // RAW BYTES PATH (libFLAC → int nativo → direto pro USB)
        val remaining = snapshot.remaining()
        if (remaining > 0) {
            val rawBytes = ByteArray(remaining)
            snapshot.get(rawBytes)
            thread.enqueueRaw(rawBytes, currentEncoding)
        }
    }
    usbWritePendingForCurrentBuffer = false
}
```

**Nota**: O float path fica como fallback pra MP3, AAC, Vorbis (decodados pelo FFmpeg que entrega float). FLAC nunca mais passa pelo float path.

---

## Mudança 4: UsbStreamingThread suportar ByteArray

### Arquivo: `driver/Felicity/engine/src/main/java/app/simple/felicity/engine/audio/UsbStreamingThread.kt`

### Mudanças:
```kotlin
// ANTES:
private val audioQueue = ArrayBlockingQueue<FloatArray>(QUEUE_CAPACITY)

// DEPOIS:
// Sealed class pra tipar os dois tipos de buffer
private sealed class AudioBuffer {
    class FloatBuffer(val data: FloatArray) : AudioBuffer()
    class RawBuffer(val data: ByteArray, val encoding: Int) : AudioBuffer()
}

private val audioQueue = ArrayBlockingQueue<AudioBuffer>(QUEUE_CAPACITY)

fun enqueue(floatBuf: FloatArray) {
    val buf = AudioBuffer.FloatBuffer(floatBuf)
    if (!audioQueue.offer(buf)) {
        audioQueue.poll()
        audioQueue.offer(buf)
    }
}

fun enqueueRaw(rawBytes: ByteArray, encoding: Int) {
    val buf = AudioBuffer.RawBuffer(rawBytes, encoding)
    if (!audioQueue.offer(buf)) {
        audioQueue.poll()
        audioQueue.offer(buf)
    }
}

// No loop da thread:
val buf = audioQueue.poll(POLL_TIMEOUT_MS, TimeUnit.MILLISECONDS) ?: continue
when (buf) {
    is AudioBuffer.FloatBuffer -> usbStream.write(buf.data)
    is AudioBuffer.RawBuffer -> usbStream.writeRaw(buf.data, buf.encoding)
}
```

---

## Mudança 5: JNI bridge — adicionar writeRaw

### Arquivo: `libs/decent-usb-audio-driver/src/main/kotlin/com/decent/usbaudio/UsbAudioStream.kt`

Adicionar ao lado do `write(FloatArray)` existente:

```kotlin
/**
 * Writes raw PCM bytes directly to the USB DAC without any conversion.
 * The native layer handles bit-depth matching (padding if needed).
 *
 * @param pcmBuffer  Raw PCM bytes in the source encoding (little-endian)
 * @param inputBitDepth  Bit depth of the input data (16 or 32)
 *                       Note: 24-bit FLAC arrives as 32-bit (int32 sign-extended)
 */
fun writeRaw(pcmBuffer: ByteArray, encoding: Int) {
    if (nativeHandle == 0L) return
    val inputBitDepth = when (encoding) {
        C.ENCODING_PCM_16BIT -> 16
        C.ENCODING_PCM_24BIT -> 24
        C.ENCODING_PCM_32BIT -> 32
        else -> return  // float ou desconhecido → não usar raw path
    }
    nativeUsbAudioWriteRaw(nativeHandle, pcmBuffer, inputBitDepth)
}

private external fun nativeUsbAudioWriteRaw(handle: Long, pcmBuffer: ByteArray, inputBitDepth: Int)
```

---

## Mudança 6: C++ — nativeUsbAudioWriteRaw (memcpy + padding)

### Arquivo: `libs/decent-usb-audio-driver/src/main/jni/usb-audio-output.cpp`

### 6a. Adicionar funções de padding inteiro (sem float):

```cpp
// ── Integer bit-depth padding (lossless, zero math) ──────────────

// 16-bit → 24-bit: pad each sample with 1 zero byte (shift left 8 bits)
static void padInt16ToInt24(const uint8_t *src, uint8_t *dst, int numSamples) {
    for (int i = 0; i < numSamples; i++) {
        dst[i*3]   = 0;                // LSB = zero padding
        dst[i*3+1] = src[i*2];         // original byte 0
        dst[i*3+2] = src[i*2+1];       // original byte 1
    }
}

// 16-bit → 32-bit: pad each sample with 2 zero bytes (shift left 16 bits)
static void padInt16ToInt32(const uint8_t *src, uint8_t *dst, int numSamples) {
    auto *out = reinterpret_cast<int32_t *>(dst);
    auto *in16 = reinterpret_cast<const int16_t *>(src);
    for (int i = 0; i < numSamples; i++) {
        out[i] = (int32_t)in16[i] << 16;
    }
}

// 32-bit (sign-extended 24-bit from libFLAC) → 24-bit: extract lower 3 bytes
static void extractInt32ToInt24(const uint8_t *src, uint8_t *dst, int numSamples) {
    auto *in32 = reinterpret_cast<const int32_t *>(src);
    for (int i = 0; i < numSamples; i++) {
        int32_t s = in32[i];
        dst[i*3]   = s & 0xFF;
        dst[i*3+1] = (s >> 8) & 0xFF;
        dst[i*3+2] = (s >> 16) & 0xFF;
    }
}

// 32-bit (sign-extended 24-bit from libFLAC) → 32-bit: shift left 8 to fill range
static void shiftInt32From24(const uint8_t *src, uint8_t *dst, int numSamples) {
    auto *out = reinterpret_cast<int32_t *>(dst);
    auto *in32 = reinterpret_cast<const int32_t *>(src);
    for (int i = 0; i < numSamples; i++) {
        out[i] = in32[i] << 8;
    }
}
```

### 6b. Adicionar JNI function `nativeUsbAudioWriteRaw`:

```cpp
extern "C" JNIEXPORT void JNICALL
Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioWriteRaw(
        JNIEnv *env, jobject, jlong h, jbyteArray pcm, jint inputBitDepth) {
    auto *ctx = reinterpret_cast<UsbAudioContext *>(h);
    if (!ctx || !ctx->running.load()) return;

    jint inputBytes = env->GetArrayLength(pcm);
    if (inputBytes <= 0) return;

    int inputBps = inputBitDepth / 8;  // bytes per sample
    int totalSamples = inputBytes / inputBps;
    int outputBytes = totalSamples * ctx->bytesPerSample;

    // Resize transfer buffer if needed
    if (!ctx->transferBuffer || ctx->transferBufferCapacity < outputBytes) {
        free(ctx->transferBuffer);
        ctx->transferBuffer = (uint8_t *)malloc(outputBytes);
        ctx->transferBufferCapacity = outputBytes;
    }

    jbyte *rawData = env->GetByteArrayElements(pcm, nullptr);
    if (!rawData) return;

    // ── BIT-DEPTH MATCHING ──
    if (inputBitDepth == ctx->bitDepth) {
        // EXACT MATCH → memcpy direto (TRUE ZERO-COPY)
        memcpy(ctx->transferBuffer, rawData, inputBytes);
    } else if (inputBitDepth == 16 && ctx->bitDepth == 24) {
        padInt16ToInt24((uint8_t *)rawData, ctx->transferBuffer, totalSamples);
    } else if (inputBitDepth == 16 && ctx->bitDepth == 32) {
        padInt16ToInt32((uint8_t *)rawData, ctx->transferBuffer, totalSamples);
    } else if (inputBitDepth == 32 && ctx->bitDepth == 24) {
        // libFLAC 24-bit → DAC 24-bit (extract from int32 container)
        extractInt32ToInt24((uint8_t *)rawData, ctx->transferBuffer, totalSamples);
    } else if (inputBitDepth == 32 && ctx->bitDepth == 32) {
        // libFLAC 24-bit in int32 → DAC 32-bit (shift to fill range)
        shiftInt32From24((uint8_t *)rawData, ctx->transferBuffer, totalSamples);
    } else {
        LOGE("Unsupported bit-depth conversion: %d → %d", inputBitDepth, ctx->bitDepth);
        env->ReleaseByteArrayElements(pcm, rawData, JNI_ABORT);
        return;
    }

    env->ReleaseByteArrayElements(pcm, rawData, JNI_ABORT);

    // ── SUBMIT URBs (idêntico ao float path) ──
    int offset = 0;
    double fpmf = ctx->sampleRate / 8000.0;

    while (offset < outputBytes && ctx->running.load()) {
        // [mesma lógica de packet sizing e URB submission do nativeUsbAudioWrite]
        // ...frameAccumulator, pktSizes, reapOldestUrb, submitRingUrb...
    }
}
```

**Nota**: A lógica de packet sizing + URB submission é IDÊNTICA à do `nativeUsbAudioWrite` existente. Deve ser extraída numa função compartilhada `submitPcmToUrbs(ctx, data, totalBytes)` pra evitar duplicação.

### 6c. TAMBÉM corrigir constantes do float path (fallback)

As funções `convertFloatToInt16/24/32` AINDA precisam da correção das constantes (32767→32768, etc.) porque o float path continua sendo usado pra MP3, AAC, e outros formatos via FFmpeg.

---

## Mudança 7: Corrigir constantes float→int (fallback path)

### Arquivo: `libs/decent-usb-audio-driver/src/main/jni/usb-audio-output.cpp` linhas 39-52

Mesmo fix do plano anterior — trocar multiplicadores:
- `32767.0f` → `32768.0f` (16-bit) + clamp
- `8388607.0f` → `8388608.0f` (24-bit) + clamp
- `2147483647.0f` → `2147483648.0` (32-bit, usar double) + clamp

---

## O que NÃO muda

| Componente | Por quê |
|-----------|---------|
| ExoPlayer (UI, queue, gapless, seekbar) | delegate muted (linha 269) continua alimentando o clock |
| Pipeline USB (claim, alt setting, URBs, ring buffer) | Funciona igual — só muda o conteúdo dos bytes |
| DSP processors | Dentro do DefaultAudioSink, não tocam no áudio USB |
| Formatos lossy (MP3, AAC, Vorbis, Opus) | Continuam pelo FFmpeg → float path (com constantes corrigidas) |

---

## Tabela de bit-depth matching (raw path)

| Fonte FLAC | Encoding recebido | DAC | Operação C++ | Bit-perfect? |
|------------|-------------------|-----|--------------|-------------|
| 16-bit | `PCM_16BIT` (2 bytes) | 16-bit | `memcpy` | ✅ Absoluto |
| 16-bit | `PCM_16BIT` (2 bytes) | 24-bit | `padInt16ToInt24` (shift<<8) | ✅ Lossless |
| 16-bit | `PCM_16BIT` (2 bytes) | 32-bit | `padInt16ToInt32` (shift<<16) | ✅ Lossless |
| 24-bit | `PCM_32BIT` (4 bytes) | 24-bit | `extractInt32ToInt24` (3 bytes) | ✅ Absoluto |
| 24-bit | `PCM_32BIT` (4 bytes) | 32-bit | `shiftInt32From24` (shift<<8) | ✅ Lossless |

Nenhuma operação de ponto flutuante. Apenas shift e memcpy. **Matematicamente impossível perder um bit.**

---

## Ordem de execução

1. **Adicionar `media3-decoder-flac` ao build.gradle** (Mudança 1)
2. **Configurar decoder + desabilitar float output pra USB** (Mudança 2)
3. **Corrigir constantes float no C++** (Mudança 7 — independente, pode ser paralelo)
4. **Adicionar `nativeUsbAudioWriteRaw` no C++** com padding functions (Mudança 6)
5. **Adicionar `writeRaw()` no `UsbAudioStream.kt`** (Mudança 5)
6. **Modificar `UsbStreamingThread.kt`** pra suportar ByteArray (Mudança 4)
7. **Modificar `handleBuffer()` no `AaudioAudioSink.kt`** pra usar raw path (Mudança 3)
8. **Testar**: FLAC 16-bit, FLAC 24-bit, MP3 (fallback float), transição entre formatos
9. **Commit na branch `dev-modular-libs`**

---

## Resultado final

```
FLAC 16-bit → libFLAC → int16 nativo → ByteArray → memcpy → URB → DAC
                          ↑ zero conversão           ↑ zero math

FLAC 24-bit → libFLAC → int32 (24-bit sign-ext) → ByteArray → extract 3 bytes → URB → DAC
                          ↑ zero float                          ↑ shift inteiro (exato)

MP3/AAC     → FFmpeg  → float → FloatArray → float×32768→int (corrigido) → URB → DAC
                          ↑ fallback path (constantes corrigidas)
```

## Sobre DSD (futuro)

DSD (.dsf/.dff) não é suportado por nenhum decoder do Media3/ExoPlayer. Requer:
1. Parser/decoder custom (libdsd2pcm ou implementação própria)
2. Empacotador DoP (DSD over PCM — bits DSD em frames 24-bit com marcador 0x05/0xFA)
3. O driver USB atual JÁ suportaria DoP — é só mandar frames 24-bit como se fossem PCM

Ou, pra DACs com suporte nativo:
1. Parsing de descriptors USB DSD (alt settings específicos)
2. Raw bitstream transfer (sem empacotamento PCM)

**Não faz parte deste plano.** Pode ser adicionado depois sem quebrar nada.
