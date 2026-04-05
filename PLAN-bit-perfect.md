# Plano: Atingir Bit-Perfect End-to-End no Felicity

## Contexto

O pipeline atual do Felicity para USB bit-perfect é:
```
FLAC → FFmpeg decoder → float PCM → AaudioAudioSink.handleBuffer() → writeSnapshotToUsb()
    → FloatArray → JNI → usb-audio-output.cpp (float→intN) → isochronous URB → USB DAC
```

O driver USB funciona. O bypass do AudioFlinger funciona. Mas existem **3 problemas** que impedem bit-perfect real ao nível do sample.

---

## Problema 1: Constantes de conversão float→int no C++ (BUG CRÍTICO)

### Arquivo: `libs/decent-usb-audio-driver/src/main/jni/usb-audio-output.cpp` linhas 39-52

O FFmpeg normaliza dividindo por **potências de 2** (padrão libswresample):
- 16-bit: `sample / 32768.0` (2^15)
- 24-bit: `sample / 8388608.0` (2^23)

Mas as funções de reconversão multiplicam por **(2^N - 1)**:
```cpp
// Linha 41 — ERRADO pra bit-perfect
out[i] = (int16_t)(clampf(src[i]) * 32767.0f);    // deveria ser 32768.0f

// Linha 45 — ERRADO pra bit-perfect  
int32_t s = (int32_t)(clampf(src[i]) * 8388607.0f);  // deveria ser 8388608.0f

// Linha 51 — ERRADO pra bit-perfect
out[i] = (int32_t)(clampf(src[i]) * 2147483647.0f);  // deveria ser 2147483648.0
```

### Prova do off-by-one:
```
int16 original: 12345
FFmpeg: 12345 / 32768.0 = 0.376800537109375 (exato em float32)
C++ atual: 0.376800537109375 × 32767.0 = 12344.62... → trunca pra 12344 ❌

Com 32768: 0.376800537109375 × 32768.0 = 12345.0 → 12345 ✓
```

### Por que funciona matematicamente com 2^N:
- float32 tem 24 bits de mantissa → representa exatamente qualquer inteiro até 2^24
- 16-bit (max 32767) e 24-bit (max 8388607) cabem perfeitamente
- A round-trip `valor / 2^N * 2^N` é exata em float32

### Correção:
```cpp
static void convertFloatToInt16(const float *src, uint8_t *dst, int n) {
    auto *out = reinterpret_cast<int16_t *>(dst);
    for (int i = 0; i < n; i++) {
        float scaled = clampf(src[i]) * 32768.0f;
        // Clamp to int16 range: [-32768, 32767]
        if (scaled > 32767.0f) scaled = 32767.0f;
        if (scaled < -32768.0f) scaled = -32768.0f;
        out[i] = (int16_t)scaled;
    }
}

static void convertFloatToInt24(const float *src, uint8_t *dst, int n) {
    for (int i = 0; i < n; i++) {
        float scaled = clampf(src[i]) * 8388608.0f;
        if (scaled > 8388607.0f) scaled = 8388607.0f;
        if (scaled < -8388608.0f) scaled = -8388608.0f;
        int32_t s = (int32_t)scaled;
        dst[i*3] = s & 0xFF;
        dst[i*3+1] = (s >> 8) & 0xFF;
        dst[i*3+2] = (s >> 16) & 0xFF;
    }
}

static void convertFloatToInt32(const float *src, uint8_t *dst, int n) {
    auto *out = reinterpret_cast<int32_t *>(dst);
    for (int i = 0; i < n; i++) {
        double scaled = (double)clampf(src[i]) * 2147483648.0;
        if (scaled > 2147483647.0) scaled = 2147483647.0;
        if (scaled < -2147483648.0) scaled = -2147483648.0;
        out[i] = (int32_t)scaled;
    }
}
```

Nota: 32-bit usa `double` no cálculo porque `2147483648.0f` perde precisão em float32.

### IMPORTANTE: Este mesmo bug existe em DUAS cópias do arquivo:
1. `libs/decent-usb-audio-driver/src/main/jni/usb-audio-output.cpp` (lib standalone)
2. O equivalente no engine do Felicity (se ainda existir) — verificar se o Felicity já aponta pra lib ou tem cópia própria.

---

## Problema 2: Garantir que FFmpeg é SEMPRE o decoder quando USB bit-perfect está ativo

### Arquivo: `driver/Felicity/engine/src/main/java/app/simple/felicity/engine/services/FelicityPlayerService.kt`

### O problema:
Linha 314-318 — o decoder é configurável pelo usuário:
```kotlin
val extensionMode = if (AudioPreferences.getAudioDecoder() == AudioPreferences.FFMPEG) {
    DefaultRenderersFactory.EXTENSION_RENDERER_MODE_PREFER
} else {
    DefaultRenderersFactory.EXTENSION_RENDERER_MODE_OFF
}
```

Se o usuário escolher MediaCodec E tiver USB bit-perfect ligado:
- FLAC 16-bit → MediaCodec entrega `ENCODING_PCM_16BIT` (NÃO float)
- O `handleBuffer()` recebe bytes de 16-bit
- O código trata corretamente via `PcmUtils.readFloat()` (não vai distorcer)
- MAS: a reconversão float→int16 no C++ introduz o off-by-one do Problema 1
- E pior: a round-trip 16-bit → float (PcmUtils) → int16 (C++) adiciona DOIS pontos de erro

### Solução:
Quando `AudioPreferences.isBitPerfectUsbEnabled()` for true, **forçar FFmpeg** independente da preferência do usuário:
```kotlin
val extensionMode = if (AudioPreferences.isBitPerfectUsbEnabled() ||
                        AudioPreferences.getAudioDecoder() == AudioPreferences.FFMPEG) {
    DefaultRenderersFactory.EXTENSION_RENDERER_MODE_PREFER
} else {
    DefaultRenderersFactory.EXTENSION_RENDERER_MODE_OFF
}
```

Local: `buildPlayer()` em `FelicityPlayerService.kt`, linha ~314.

---

## Problema 3: Garantir que `enableFloatOutput(true)` está ativo quando USB bit-perfect está ligado

### Arquivo: `driver/Felicity/engine/src/main/java/app/simple/felicity/engine/services/FelicityPlayerService.kt`

### O problema:
Linha 181 e 224:
```kotlin
val hiresEnabled = AudioPreferences.isHiresOutputEnabled()
// ...
.setEnableFloatOutput(hiresEnabled)
```

Se o usuário tem USB bit-perfect ligado MAS hi-res desligado:
- FFmpeg recebe sinal de que o sink NÃO suporta float
- FFmpeg entrega `ENCODING_PCM_16BIT` pra TUDO (inclusive 24-bit FLAC!)
- O 24-bit é truncado pra 16-bit pelo FFmpeg → perda irreversível

### Solução:
Quando USB bit-perfect está ativo, forçar float output:
```kotlin
val hiresEnabled = AudioPreferences.isHiresOutputEnabled() ||
                   AudioPreferences.isBitPerfectUsbEnabled()
// ...
.setEnableFloatOutput(hiresEnabled)
```

Local: `buildAudioSink()` em `FelicityPlayerService.kt`, linha ~181.

---

## O que NÃO precisa mudar

### ✅ `AaudioAudioSink.kt` — handleBuffer() já está correto
Linhas 256-261: O check `currentEncoding == C.ENCODING_PCM_FLOAT` antes de `asFloatBuffer()` está correto. O fallback pra `PcmUtils.readFloat()` cobre todos os encodings. Não mexer.

### ✅ `PcmUtils.kt` — conversões estão corretas
As constantes de normalização (32768, 8388608, 2147483648) estão certas. Não mexer.

### ✅ Pipeline USB (claim, alt setting, SET_CUR, URBs) — funciona
O setup do dispositivo USB, isochronous transfers, ring buffer — tudo funciona. Não mexer.

### ✅ Delegate muting e forceMediaToSpeaker — funciona
O bypass do AudioFlinger está correto. Não mexer.

### ✅ AudioProcessors DSP (EQ, bass, etc.)
Quando USB bit-perfect está ativo, o `handleBuffer()` recebe o buffer DIRETO do renderer (pré-DefaultAudioSink processing). Os DSP processors estão dentro do DefaultAudioSink, que recebe o buffer muted. O audio que vai pro USB NÃO passa por nenhum DSP. Isso é correto pra bit-perfect.

---

## Ordem de execução

1. **Corrigir as 3 funções de conversão em `usb-audio-output.cpp`** (Problema 1)
   - Arquivo: `libs/decent-usb-audio-driver/src/main/jni/usb-audio-output.cpp` linhas 39-52
   - Trocar multiplicadores: 32767→32768, 8388607→8388608, 2147483647→2147483648
   - Adicionar clamp pós-multiplicação pra evitar overflow
   - Usar `double` pra 32-bit (float32 não tem precisão suficiente pra 2147483648)

2. **Forçar FFmpeg quando USB bit-perfect está ativo** (Problema 2)
   - Arquivo: `driver/Felicity/engine/src/main/java/app/simple/felicity/engine/services/FelicityPlayerService.kt` ~linha 314
   - Adicionar `AudioPreferences.isBitPerfectUsbEnabled()` ao check do extensionMode

3. **Forçar enableFloatOutput quando USB bit-perfect está ativo** (Problema 3)
   - Arquivo: `driver/Felicity/engine/src/main/java/app/simple/felicity/engine/services/FelicityPlayerService.kt` ~linha 181
   - Adicionar `AudioPreferences.isBitPerfectUsbEnabled()` ao check do hiresEnabled

4. **Verificar se o Felicity usa a lib em `libs/` ou tem cópia própria do .cpp**
   - Se tem cópia em `driver/Felicity/engine/src/main/jni/`, corrigir lá também
   - Idealmente, o Felicity deveria apontar pra lib (mas isso é outra task)

5. **Commit na branch `dev-modular-libs`** com mensagem descritiva

---

## Resultado esperado

Após as correções, o pipeline bit-perfect é:
```
FLAC 16-bit → FFmpeg → float (÷32768, exato) → USB driver → int16 (×32768, exato) → DAC
FLAC 24-bit → FFmpeg → float (÷8388608, exato) → USB driver → int24 (×8388608, exato) → DAC
```

Cada sample sobrevive a round-trip **sem perda de nenhum bit**.

Nota: Para fontes 32-bit (raríssimas), float32 tem apenas 24 bits de mantissa, então a round-trip 32-bit → float → 32-bit perde os 8 bits menos significativos. Isso é uma limitação fundamental do float32, não do driver. Solução futura: aceitar ByteBuffer raw e passar direto pro URB sem conversão (zero-copy path).

---

## Referências das pesquisas

### ExoPlayer/Media3 — Comportamento do `setEnableFloatOutput(true)`
- `DefaultAudioSink.shouldUseFloatOutput()` retorna true SOMENTE para encodings "high-resolution" (24-bit, 32-bit, float). **16-bit NÃO é considerado high-resolution**.
- Com MediaCodec: FLAC 16-bit chega como `ENCODING_PCM_16BIT` mesmo com float habilitado.
- Com FFmpeg: TUDO chega como `ENCODING_PCM_FLOAT` quando o sink suporta float.

### Tabela de encodings por cenário:
| Fonte | Decoder | enableFloatOutput | Encoding no handleBuffer |
|-------|---------|-------------------|--------------------------|
| FLAC 16-bit | MediaCodec | true | `PCM_16BIT` |
| FLAC 16-bit | FFmpeg | true | `PCM_FLOAT` |
| FLAC 24-bit | MediaCodec | true | `PCM_FLOAT` |
| FLAC 24-bit | FFmpeg | true | `PCM_FLOAT` |
| Qualquer | Qualquer | false | `PCM_16BIT` |

### Fontes consultadas:
- DefaultAudioSink.java (Media3 source) — `shouldUseFloatOutput()`, `isEncodingHighResolutionPcm()`
- FfmpegAudioRenderer.java — `shouldOutputFloat()`
- MediaCodecAudioRenderer.java — `onOutputFormatChanged()`, `KEY_PCM_ENCODING`
- Util.java — `isEncodingHighResolutionPcm()` retorna false pra 16-bit
