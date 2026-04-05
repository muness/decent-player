# Plano: Bit-Perfect End-to-End — 3 Fases

## Visão Geral

| Fase | O quê | Resultado |
|------|-------|-----------|
| **1** | FFmpeg (já temos) + corrigir constantes float | Bit-perfect funcional HOJE |
| **2** | Publicar `com.decentplayer:media3-decoder-flac` | libFLAC como dep Maven |
| **3** | Raw bytes path no driver USB | Zero float, zero math pra FLAC |

---

# FASE 1: Bit-Perfect via FFmpeg (AGORA)

## Contexto

O Jellyfin FFmpeg decoder que já usamos (`org.jellyfin.media3:media3-ffmpeg-decoder:1.9.0+1`) **já tem decoder FLAC compilado**. A lista de decoders habilitados no build é:

```bash
ENABLED_DECODERS=(flac alac pcm_mulaw pcm_alaw mp3 aac ac3 eac3 dca mlp truehd)
```

Ou seja: **FLAC já funciona via FFmpeg**. O problema é que com `enableFloatOutput=true`, o FFmpeg converte tudo pra float, e as constantes de reconversão float→int no C++ estão erradas (off-by-one).

## Mudanças da Fase 1

### 1.1 Corrigir constantes float→int no C++

**Arquivo:** `libs/decent-usb-audio-driver/src/main/jni/usb-audio-output.cpp` linhas 39-52

```cpp
// ANTES (ERRADO — off-by-one):
out[i] = (int16_t)(clampf(src[i]) * 32767.0f);
int32_t s = (int32_t)(clampf(src[i]) * 8388607.0f);
out[i] = (int32_t)(clampf(src[i]) * 2147483647.0f);

// DEPOIS (CORRETO — match com FFmpeg libswresample):
// 16-bit:
float scaled = clampf(src[i]) * 32768.0f;
if (scaled > 32767.0f) scaled = 32767.0f;
if (scaled < -32768.0f) scaled = -32768.0f;
out[i] = (int16_t)scaled;

// 24-bit:
float scaled = clampf(src[i]) * 8388608.0f;
if (scaled > 8388607.0f) scaled = 8388607.0f;
if (scaled < -8388608.0f) scaled = -8388608.0f;
int32_t s = (int32_t)scaled;

// 32-bit (usar double — float32 perde precisão pra 2147483648):
double scaled = (double)clampf(src[i]) * 2147483648.0;
if (scaled > 2147483647.0) scaled = 2147483647.0;
if (scaled < -2147483648.0) scaled = -2147483648.0;
out[i] = (int32_t)scaled;
```

**Por quê funciona:** FFmpeg normaliza com `÷2^N`, a reconversão com `×2^N` é exata em float32 pra 16-bit e 24-bit (float32 tem 24 bits de mantissa).

### 1.2 Forçar FFmpeg quando USB bit-perfect ativo

**Arquivo:** `driver/Felicity/engine/src/main/java/app/simple/felicity/engine/services/FelicityPlayerService.kt` ~linha 314

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

### 1.3 Forçar enableFloatOutput quando USB bit-perfect ativo

**Arquivo:** `driver/Felicity/engine/src/main/java/app/simple/felicity/engine/services/FelicityPlayerService.kt` ~linha 181

```kotlin
// ANTES:
val hiresEnabled = AudioPreferences.isHiresOutputEnabled()

// DEPOIS:
val hiresEnabled = AudioPreferences.isHiresOutputEnabled() ||
                   AudioPreferences.isBitPerfectUsbEnabled()
```

**Por quê:** Com `enableFloatOutput=true`, o FFmpeg entrega `ENCODING_PCM_FLOAT` pra TUDO (inclusive FLAC 16-bit). Sem isso, FLAC 24-bit é truncado pra 16-bit.

### O que NÃO muda na Fase 1

- `AaudioAudioSink.kt` — handleBuffer já trata float e int corretamente
- `PcmUtils.kt` — constantes de normalização já estão certas
- Pipeline USB (claim, alt setting, URBs) — funciona
- UI/ExoPlayer — delegate muted continua pro clock

### Resultado da Fase 1
```
FLAC → FFmpeg → float (÷2^N, exato) → USB driver → int (×2^N, exato) → DAC
```
**Bit-perfect comprovável matematicamente.** Round-trip lossless pra 16-bit e 24-bit.

---

# FASE 2: Publicar com.decentplayer:media3-decoder-flac

## Objetivo

Criar e publicar nosso próprio build do decoder FLAC do media3, compatível com media3 1.9.x. Igual o Jellyfin fez pro FFmpeg.

## Pesquisa: TIDAL como referência

### O que o TIDAL tem hoje

- **Artefato:** `com.tidal.androidx.media3:media3-flac:1.5.0.1`
- **Conteúdo:** AAR com `libflacJNI.so` pré-compilado (arm64-v8a, armeabi-v7a, x86, x86_64) + 8 classes Java
- **Tamanho:** ~624KB
- **Baseado em:** media3 1.5.0
- **Problema:** Depende de `com.tidal.androidx.media3:media3-exoplayer:1.5.0.1` (fork completo do media3 — incompatível com nosso 1.9.3)
- **Source:** Repo privado `github.com/tidal-music/tidal-androidx-media` (404)

### Compatibilidade binária TIDAL ↔ media3 1.9.3

**Achado crítico:** O código nativo (JNI) é **IDÊNTICO** entre media3 1.5.0 e 1.9.3:

| Arquivo | Mudanças 1.5.0 → 1.9.3 |
|---------|------------------------|
| `flac_jni.cc` | Só whitespace (`Type *name` → `Type* name`) |
| `flac_parser.cc` | Só whitespace |
| `FlacDecoderJni.java` | IDÊNTICO (zero mudanças) |
| `FlacDecoder.java` | IDÊNTICO |
| `FlacLibrary.java` | IDÊNTICO |
| `LibflacAudioRenderer.java` | Só removeu `@hide` do javadoc |
| `CMakeLists.txt` | Removeu 16KB ELF alignment (TIDAL tem, é melhor) |

**Os `.so` do TIDAL produzem código de máquina idêntico ao que seria compilado do 1.9.3.** As 14 symbols JNI exportadas são as mesmas.

### Opção rápida: Extrair .so do TIDAL + Java do 1.9.3

1. Baixar AAR do TIDAL (`media3-flac-1.5.0.1.aar`)
2. Extrair `jni/{arm64-v8a,armeabi-v7a,x86,x86_64}/libflacJNI.so`
3. Criar módulo com Java sources do tag `1.9.3` do `androidx/media`
4. Colocar os `.so` em `src/main/jniLibs/`
5. Depender de `androidx.media3:media3-decoder:1.9.3` e `media3-exoplayer:1.9.3` (oficiais)
6. Publicar como `com.decentplayer:media3-decoder-flac:1.9.3+1`

**Funciona porque o JNI ABI é congelado** — os `.so` são binariamente compatíveis.

### Opção robusta: Build completo do source (modelo Jellyfin)

O Jellyfin faz assim pro FFmpeg:

1. **Repo:** `jellyfin/jellyfin-androidx-media`
2. **Submodules:** `media/` → `androidx/media@release`, `ffmpeg/` → `git.ffmpeg.org/ffmpeg@release/6.0`
3. **Build nativo:** Chama `build_ffmpeg.sh` do upstream com NDK 26+
4. **Módulo wrapper:** `media3-ffmpeg-decoder/build.gradle.kts` — reempacota o AAR
5. **CI:** GitHub Actions com ubuntu-24.04, JDK 17, NDK 26.1, CMake 3.31
6. **Publish:** Sonatype via `gradle-nexus-publish-plugin`
7. **Versionamento:** `{media3_version}+{revision}` (ex: `1.9.0+1`)

**Para FLAC, replicar:**

```
decentplayer-media3-flac/
├── media/                          ← submodule: androidx/media@1.9.3
├── libflac/                        ← submodule: xiph/flac (fonte do libFLAC)
├── media3-decoder-flac/            ← módulo wrapper
│   └── build.gradle.kts            ← reempacota AAR, deps oficiais
├── build.sh                        ← link libflac source + trigger build
├── .github/workflows/publish.yml   ← CI/CD
├── settings.gradle.kts
└── build.gradle.kts
```

**Deps nativas do decoder_flac (do CMakeLists.txt oficial):**
- libFLAC source de `xiph/flac` (linkado via `add_subdirectory`)
- NDK 26+ com CMake 3.21+
- Compila pra 4 ABIs: arm64-v8a, armeabi-v7a, x86, x86_64

### Classes Java do decoder_flac (8 arquivos):

| Classe | Função |
|--------|--------|
| `LibflacAudioRenderer` | Renderer que o ExoPlayer auto-detecta via reflection |
| `FlacDecoder` | Implementação do decoder |
| `FlacDecoderJni` | Bridge JNI → libflacJNI.so |
| `FlacDecoderException` | Exceções tipadas |
| `FlacExtractor` | Extrator de containers FLAC |
| `FlacBinarySearchSeeker` | Seek support |
| `FlacLibrary` | Carrega `libflacJNI` via `System.loadLibrary` |
| `package-info` | Metadados |

### Auto-detecção pelo ExoPlayer

`DefaultRenderersFactory.buildAudioRenderers()` usa reflection:
```java
Class.forName("androidx.media3.decoder.flac.LibflacAudioRenderer")
```

Como nossas classes ficam no **mesmo package** (`androidx.media3.decoder.flac`), o ExoPlayer detecta automaticamente. Zero config necessário no app consumidor — só adicionar a dep.

### Formato de saída do LibflacAudioRenderer

```java
private @C.PcmEncoding int getOutputPcmEncoding(int inputBitsPerSample) {
    if (shouldOutputFloat) return C.ENCODING_PCM_FLOAT;
    if (inputBitsPerSample == 24 || inputBitsPerSample == 32) return C.ENCODING_PCM_32BIT;
    return C.ENCODING_PCM_16BIT;
}
```

| FLAC source | enableFloatOutput | Output encoding | Bytes/sample |
|-------------|-------------------|-----------------|-------------|
| 16-bit | false | `PCM_16BIT` | 2 |
| 24-bit | false | `PCM_32BIT` (int32, sign-extended) | 4 |
| 16-bit | true | `PCM_FLOAT` | 4 |
| 24-bit | true | `PCM_FLOAT` | 4 |

**Com `enableFloatOutput=false`:** saída em inteiro nativo. Ideal pro raw bytes path da Fase 3.

### Recomendação pra Fase 2

**Começar com a opção rápida** (extrair .so do TIDAL + Java 1.9.3) pra validar. Depois migrar pro build completo do source (modelo Jellyfin) pra sustentabilidade.

---

# FASE 3: Raw Bytes Path no USB Driver

## Pré-requisito: Fase 2 concluída (media3-decoder-flac disponível)

## Pipeline final
```
FLAC 16-bit → libFLAC → int16 nativo → ByteArray → memcpy → URB → DAC
FLAC 24-bit → libFLAC → int32 (24-bit sign-ext) → ByteArray → extract 3 bytes → URB → DAC
```

**Zero float. Zero math (exceto integer shift pra padding de bit-depth).**

## Mudanças da Fase 3

### 3.1 Desabilitar float output quando USB bit-perfect + libFLAC

**Arquivo:** `FelicityPlayerService.kt` ~linha 181

```kotlin
// Inverter a lógica da Fase 1:
// Com libFLAC disponível, queremos inteiro nativo, NÃO float
val hiresEnabled = if (AudioPreferences.isBitPerfectUsbEnabled()) false
                   else AudioPreferences.isHiresOutputEnabled()
```

**Por quê:** Com `enableFloatOutput=false`, o `LibflacAudioRenderer` entrega PCM no bit depth nativo (16-bit ou 32-bit container). Com `true`, converte pra float — exatamente o que queremos evitar.

### 3.2 Raw bytes branch no handleBuffer

**Arquivo:** `AaudioAudioSink.kt` linhas 251-263

```kotlin
if (thread != null && usbWritePendingForCurrentBuffer) {
    if (currentEncoding == C.ENCODING_PCM_FLOAT) {
        // FALLBACK: Float path (MP3, AAC via FFmpeg)
        val totalSamples = snapshot.remaining() / 4
        if (totalSamples > 0) {
            val floatBuf = FloatArray(totalSamples)
            snapshot.asFloatBuffer().get(floatBuf)
            thread.enqueue(floatBuf)
        }
    } else {
        // RAW PATH: libFLAC → int nativo → direto pro USB
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

### 3.3 UsbStreamingThread — suportar ByteArray

**Arquivo:** `UsbStreamingThread.kt`

```kotlin
private sealed class AudioBuffer {
    class FloatBuffer(val data: FloatArray) : AudioBuffer()
    class RawBuffer(val data: ByteArray, val encoding: Int) : AudioBuffer()
}

private val audioQueue = ArrayBlockingQueue<AudioBuffer>(QUEUE_CAPACITY)

fun enqueue(floatBuf: FloatArray) {
    val buf = AudioBuffer.FloatBuffer(floatBuf)
    if (!audioQueue.offer(buf)) { audioQueue.poll(); audioQueue.offer(buf) }
}

fun enqueueRaw(rawBytes: ByteArray, encoding: Int) {
    val buf = AudioBuffer.RawBuffer(rawBytes, encoding)
    if (!audioQueue.offer(buf)) { audioQueue.poll(); audioQueue.offer(buf) }
}

// No loop da thread:
when (val buf = audioQueue.poll(POLL_TIMEOUT_MS, TimeUnit.MILLISECONDS)) {
    is AudioBuffer.FloatBuffer -> usbStream.write(buf.data)
    is AudioBuffer.RawBuffer -> usbStream.writeRaw(buf.data, buf.encoding)
    null -> continue
}
```

### 3.4 JNI bridge — writeRaw

**Arquivo:** `libs/decent-usb-audio-driver/src/main/kotlin/com/decent/usbaudio/UsbAudioStream.kt`

```kotlin
fun writeRaw(pcmBuffer: ByteArray, encoding: Int) {
    if (nativeHandle == 0L) return
    val inputBitDepth = when (encoding) {
        C.ENCODING_PCM_16BIT -> 16
        C.ENCODING_PCM_24BIT -> 24
        C.ENCODING_PCM_32BIT -> 32
        else -> return
    }
    nativeUsbAudioWriteRaw(nativeHandle, pcmBuffer, inputBitDepth)
}

private external fun nativeUsbAudioWriteRaw(handle: Long, pcmBuffer: ByteArray, inputBitDepth: Int)
```

### 3.5 C++ — nativeUsbAudioWriteRaw

**Arquivo:** `libs/decent-usb-audio-driver/src/main/jni/usb-audio-output.cpp`

**Funções de padding inteiro (lossless):**
```cpp
// 16-bit → 24-bit (shift left 8)
static void padInt16ToInt24(const uint8_t *src, uint8_t *dst, int numSamples) {
    for (int i = 0; i < numSamples; i++) {
        dst[i*3]   = 0;
        dst[i*3+1] = src[i*2];
        dst[i*3+2] = src[i*2+1];
    }
}

// 16-bit → 32-bit (shift left 16)
static void padInt16ToInt32(const uint8_t *src, uint8_t *dst, int numSamples) {
    auto *out = reinterpret_cast<int32_t *>(dst);
    auto *in16 = reinterpret_cast<const int16_t *>(src);
    for (int i = 0; i < numSamples; i++) out[i] = (int32_t)in16[i] << 16;
}

// int32 (24-bit sign-extended from libFLAC) → 24-bit (extract lower 3 bytes)
static void extractInt32ToInt24(const uint8_t *src, uint8_t *dst, int numSamples) {
    auto *in32 = reinterpret_cast<const int32_t *>(src);
    for (int i = 0; i < numSamples; i++) {
        int32_t s = in32[i];
        dst[i*3]   = s & 0xFF;
        dst[i*3+1] = (s >> 8) & 0xFF;
        dst[i*3+2] = (s >> 16) & 0xFF;
    }
}

// int32 (24-bit sign-extended) → 32-bit (shift left 8 to fill range)
static void shiftInt32From24(const uint8_t *src, uint8_t *dst, int numSamples) {
    auto *out = reinterpret_cast<int32_t *>(dst);
    auto *in32 = reinterpret_cast<const int32_t *>(src);
    for (int i = 0; i < numSamples; i++) out[i] = in32[i] << 8;
}
```

**JNI function:**
```cpp
extern "C" JNIEXPORT void JNICALL
Java_com_decent_usbaudio_UsbAudioStream_nativeUsbAudioWriteRaw(
        JNIEnv *env, jobject, jlong h, jbyteArray pcm, jint inputBitDepth) {
    auto *ctx = reinterpret_cast<UsbAudioContext *>(h);
    if (!ctx || !ctx->running.load()) return;

    jint inputBytes = env->GetArrayLength(pcm);
    if (inputBytes <= 0) return;

    int inputBps = inputBitDepth / 8;
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

    // BIT-DEPTH MATCHING
    if (inputBitDepth == ctx->bitDepth) {
        memcpy(ctx->transferBuffer, rawData, inputBytes);  // ZERO-COPY
    } else if (inputBitDepth == 16 && ctx->bitDepth == 24) {
        padInt16ToInt24((uint8_t *)rawData, ctx->transferBuffer, totalSamples);
    } else if (inputBitDepth == 16 && ctx->bitDepth == 32) {
        padInt16ToInt32((uint8_t *)rawData, ctx->transferBuffer, totalSamples);
    } else if (inputBitDepth == 32 && ctx->bitDepth == 24) {
        extractInt32ToInt24((uint8_t *)rawData, ctx->transferBuffer, totalSamples);
    } else if (inputBitDepth == 32 && ctx->bitDepth == 32) {
        shiftInt32From24((uint8_t *)rawData, ctx->transferBuffer, totalSamples);
    } else {
        LOGE("Unsupported bit-depth: %d → %d", inputBitDepth, ctx->bitDepth);
        env->ReleaseByteArrayElements(pcm, rawData, JNI_ABORT);
        return;
    }

    env->ReleaseByteArrayElements(pcm, rawData, JNI_ABORT);

    // SUBMIT URBs — mesma lógica do nativeUsbAudioWrite
    // (extrair pra função compartilhada submitPcmToUrbs)
    // ...frameAccumulator, pktSizes, reapOldestUrb, submitRingUrb...
}
```

**Nota:** A lógica de URB submission (packet sizing, frame accumulator, backpressure) deve ser extraída numa função compartilhada `submitPcmToUrbs(ctx, data, totalBytes)` usada por ambos `nativeUsbAudioWrite` e `nativeUsbAudioWriteRaw`.

### Tabela de bit-depth matching

| FLAC | Encoding | DAC | Operação | Bit-perfect? |
|------|----------|-----|----------|-------------|
| 16-bit | `PCM_16BIT` | 16-bit | `memcpy` | ✅ Absoluto |
| 16-bit | `PCM_16BIT` | 24-bit | `padInt16ToInt24` | ✅ Lossless |
| 16-bit | `PCM_16BIT` | 32-bit | `padInt16ToInt32` | ✅ Lossless |
| 24-bit | `PCM_32BIT` | 24-bit | `extractInt32ToInt24` | ✅ Absoluto |
| 24-bit | `PCM_32BIT` | 32-bit | `shiftInt32From24` | ✅ Lossless |

---

## Resumo das 3 fases

### Fase 1 — Agora (branch dev-modular-libs)
- **3 mudanças em 2 arquivos** + build/test
- Bit-perfect via FFmpeg com constantes corrigidas
- Zero risco — não muda arquitetura, só corrige valores

### Fase 2 — Próximo (repo/branch separado)
- Criar `com.decentplayer:media3-decoder-flac:1.9.3+1`
- Atalho: extrair `.so` do TIDAL + Java do 1.9.3 (compatibilidade binária confirmada)
- Sustentável: build do source (modelo Jellyfin) com submodules
- Publicar no Maven Central via Sonatype

### Fase 3 — Depois da Fase 2
- **5 mudanças em 5 arquivos** (Kotlin + C++)
- Raw bytes path: ByteArray → memcpy → URB
- Zero float pra FLAC, FFmpeg como fallback pra lossy
- (removed)-level bit-perfect

---

## Referências

### Repositórios
- [Official media3 decoder_flac source](https://github.com/androidx/media/tree/release/libraries/decoder_flac)
- [Jellyfin media3-ffmpeg-decoder build](https://github.com/jellyfin/jellyfin-androidx-media)
- [TIDAL SDK Android](https://github.com/tidal-music/tidal-sdk-android)

### Artefatos Maven
- TIDAL FLAC: `com.tidal.androidx.media3:media3-flac:1.5.0.1` ([Maven Central](https://central.sonatype.com/artifact/com.tidal.androidx.media3/media3-flac))
- Jellyfin FFmpeg: `org.jellyfin.media3:media3-ffmpeg-decoder:1.9.0+1` ([Maven Central](https://central.sonatype.com/artifact/org.jellyfin.media3/media3-ffmpeg-decoder))

### Documentação
- [Media3 supported formats](https://developer.android.com/media/media3/exoplayer/supported-formats)
- [Media3 migration mappings](https://developer.android.com/media/media3/exoplayer/mappings)
- [decoder_flac README (build from source)](https://github.com/androidx/media/tree/release/libraries/decoder_flac)

### Achados técnicos
- JNI do FLAC é idêntico entre media3 1.5.0 e 1.9.3 (zero mudanças funcionais no nativo)
- TIDAL `.so` tem 16KB ELF alignment (melhor pra Android moderno)
- Jellyfin FFmpeg já inclui decoder FLAC nos `ENABLED_DECODERS`
- `LibflacAudioRenderer` é auto-detectado por `DefaultRenderersFactory` via reflection: `Class.forName("androidx.media3.decoder.flac.LibflacAudioRenderer")`
- Com `enableFloatOutput=false`, libFLAC entrega: 16-bit FLAC → `PCM_16BIT`, 24-bit FLAC → `PCM_32BIT` (int32 sign-extended)
