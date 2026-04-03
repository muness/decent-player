# BRIEFING: Implementar Bit-Perfect USB Audio no Felicity Music Player

## Contexto do Projeto

Estamos fazendo um fork do **Felicity Music Player** (https://github.com/Hamza417/Felicity), um player de música Android open-source (AGPL v3) com engine de áudio nativa em C++/JNI. O objetivo é adicionar suporte a **bit-perfect USB audio** usando a API oficial do Android 14+ (`AudioMixerAttributes` com `MIXER_BEHAVIOR_BIT_PERFECT`), tornando-o o **primeiro app público a usar essa API** para saída USB DAC.

---

## O Problema

O Android, por padrão, roteia todo áudio pelo **AudioFlinger** (mixer do sistema), que:
- Resampla tudo para 48kHz (ou a taxa padrão do dispositivo)
- Converte bit depth
- Mistura streams de múltiplos apps
- Aplica volume e efeitos do sistema

Isso destrói a qualidade original do arquivo — um FLAC 24-bit/96kHz sai como 16-bit/48kHz genérico.

Até hoje, a única solução é o **USB Audio Player Pro ((removed))**, que usa um **driver USB proprietário** para contornar completamente o Android. Funciona, mas a UI é horrível e o código é fechado.

## A Solução: API Oficial do Android 14 (API 34)

Desde o Android 14, existe uma API oficial para bit-perfect:

```kotlin
// API-chave:
AudioManager.setPreferredMixerAttributes(
    audioAttributes,    // USAGE_MEDIA
    usbAudioDevice,     // AudioDeviceInfo do DAC USB
    mixerAttributes     // com MIXER_BEHAVIOR_BIT_PERFECT
)
```

Referência oficial: https://developer.android.com/media/platform/improve-audio-playback
Código de exemplo do Google: https://developer.android.com/reference/android/media/AudioMixerAttributes

**IMPORTANTE - Limitações conhecidas:**
1. O suporte a `MIXER_BEHAVIOR_BIT_PERFECT` é **OPCIONAL** para fabricantes — o HAL do dispositivo precisa ter a flag `AUDIO_OUTPUT_FLAG_BIT_PERFECT` habilitada
2. **Nenhum app público** implementou isso ainda (nem ExoPlayer/Media3 do Google — issue #415 aberta desde 2023)
3. Mesmo Pixels com Android 14+ nem sempre suportam (depende do HAL)
4. O dispositivo-alvo é um **Samsung Galaxy S26 Ultra** — não sabemos se a Samsung habilitou a flag

---

## Arquitetura Atual do Felicity (o que já existe)

O Felicity já tem um pipeline de áudio **muito bem estruturado** que está 90% pronto:

### Camada Kotlin (engine module)
```
engine/src/main/java/app/simple/felicity/engine/
├── audio/
│   └── AaudioAudioSink.kt          # ← PONTO DE INSERÇÃO PRINCIPAL
├── processors/
│   └── AaudioOutputProcessor.kt     # Wrapper JNI para o stream AAudio nativo
├── services/
│   └── FelicityPlayerService.kt     # Já detecta USB devices (TYPE_USB_DEVICE)
├── managers/
│   └── AudioPipelineManager.kt
└── ...
```

### Camada Nativa C++ (JNI)
```
engine/src/main/jni/
├── aaudio-player.cpp    # Abre AAudioStream, escreve PCM, converte float→int16
├── aaudio-player.h      # Struct AaudioContext
└── dsp-engine.cpp       # DSP com NEON SIMD
```

### Preferences
```
preferences/src/main/java/.../AudioPreferences.kt
# Já tem toggle AAUDIO_ENABLED (boolean)
```

### Como funciona HOJE:
1. ExoPlayer decodifica o arquivo (FLAC, etc.) → PCM float
2. `AaudioAudioSink.kt` intercepta o buffer PCM via `handleBuffer()`
3. Se AAudio está habilitado, envia o PCM para `AaudioOutputProcessor` (JNI)
4. No C++, `aaudio-player.cpp` abre um `AAudioStream` com `AAUDIO_SHARING_MODE_EXCLUSIVE` e `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY`
5. Escreve PCM float direto no stream (com fallback para int16 se HAL rejeitar float)

**O que falta:** O AAudioStream atual ainda passa pelo AudioFlinger. O `EXCLUSIVE` mode do AAudio NÃO é bit-perfect — ele apenas garante acesso exclusivo ao device, mas o mixer ainda processa o áudio. Para bit-perfect real, precisamos chamar `setPreferredMixerAttributes()` com `MIXER_BEHAVIOR_BIT_PERFECT` ANTES de abrir o stream.

---

## TAREFA 1: Diagnóstico HAL (PRIORITÁRIA)

Antes de qualquer implementação bit-perfect, precisamos verificar se o dispositivo suporta a flag. Crie um **diagnóstico acessível pela UI do Felicity** (pode ser um botão nas settings de áudio ou um dialog) que:

### O que verificar:
```kotlin
// API 34+ (Android 14+)
val audioManager = getSystemService(AudioManager::class.java)

// 1. Listar dispositivos USB conectados
val usbDevices = audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
    .filter { it.type == AudioDeviceInfo.TYPE_USB_DEVICE 
           || it.type == AudioDeviceInfo.TYPE_USB_HEADSET }

// 2. Para cada USB device, consultar mixer attributes suportados
for (device in usbDevices) {
    val supportedMixerAttrs = audioManager.getSupportedMixerAttributes(device)
    
    // 3. Verificar se BIT_PERFECT está entre os suportados
    val hasBitPerfect = supportedMixerAttrs.any { 
        it.mixerBehavior == AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT 
    }
    
    // 4. Listar todos os formatos/sample rates suportados
    for (attr in supportedMixerAttrs) {
        // attr.format → AudioFormat (encoding, sampleRate, channelMask)
        // attr.mixerBehavior → DEFAULT ou BIT_PERFECT
    }
}
```

### O que mostrar no AlertDialog:
- Nome do dispositivo USB (product name)
- Se `MIXER_BEHAVIOR_BIT_PERFECT` é suportado (SIM/NÃO em destaque)
- Lista de formatos suportados (sample rates, bit depths)
- Se nenhum USB está conectado, mostrar mensagem orientando conectar um DAC
- Versão do Android (deve ser >= 14 / API 34)

### Onde colocar o botão:
O Felicity já tem um toggle AAudio em `AudioPreferences`. Adicione um botão "Diagnóstico USB Audio" perto dele, ou um ícone de info que abre o dialog.

---

## TAREFA 2: Implementar Bit-Perfect

Se o diagnóstico confirmar suporte, implementar bit-perfect. As mudanças são cirúrgicas:

### 2.1 — Nova preference em `AudioPreferences.kt`

```kotlin
const val BIT_PERFECT_ENABLED = "bit_perfect_enabled"

fun setBitPerfectEnabled(enabled: Boolean) {
    SharedPreferences.getSharedPreferences().edit { putBoolean(BIT_PERFECT_ENABLED, enabled) }
}

fun isBitPerfectEnabled(): Boolean {
    return SharedPreferences.getSharedPreferences().getBoolean(BIT_PERFECT_ENABLED, false)
}
```

### 2.2 — Mudança principal em `AaudioAudioSink.kt`

No método `configure()`, ANTES de criar o `AaudioOutputProcessor`, adicionar:

```kotlin
if (Build.VERSION.SDK_INT >= 34 && AudioPreferences.isBitPerfectEnabled()) {
    val audioManager = context.getSystemService(AudioManager::class.java)
    val usbDevice = audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
        .firstOrNull { 
            it.type == AudioDeviceInfo.TYPE_USB_DEVICE || 
            it.type == AudioDeviceInfo.TYPE_USB_HEADSET 
        }
    
    if (usbDevice != null) {
        try {
            val format = AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_FLOAT) // ou PCM_24BIT_PACKED
                .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                .setSampleRate(sr) // sample rate nativo do arquivo
                .build()
            
            val mixerAttrs = AudioMixerAttributes(format, AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT)
            
            val attrs = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .build()
            
            val success = audioManager.setPreferredMixerAttributes(attrs, usbDevice, mixerAttrs)
            Log.i(TAG, "setPreferredMixerAttributes BIT_PERFECT result: $success")
            
            // Registrar listener para mudanças
            audioManager.addOnPreferredMixerAttributesChangedListener(
                Executors.newSingleThreadExecutor(),
                object : AudioManager.OnPreferredMixerAttributesChangedListener {
                    override fun onPreferredMixerAttributesChanged(
                        attributes: AudioAttributes,
                        device: AudioDeviceInfo,
                        mixerAttributes: AudioMixerAttributes?
                    ) {
                        Log.i(TAG, "Mixer attributes changed: ${mixerAttributes?.mixerBehavior}")
                    }
                }
            )
        } catch (e: Exception) {
            Log.e(TAG, "Failed to set BIT_PERFECT mixer attributes", e)
        }
    }
}
```

### 2.3 — Considerações importantes para o modo bit-perfect

Quando bit-perfect está ativo:
- **Volume do sistema NÃO funciona** (o áudio passa sem modificação)
- **DSP/EQ/efeitos devem ser desabilitados** (qualquer processamento destrói o bit-perfect)
- O app deve avisar o usuário sobre isso
- Se o arquivo é 44.1kHz, o sample rate deve ser setado para 44.1kHz (não 48kHz!)
- Se o arquivo é 96kHz, deve ser 96kHz — o sample rate MUDA conforme o arquivo

### 2.4 — Sample rate switching

No modo bit-perfect, cada arquivo deve abrir o stream com o sample rate nativo. O `AaudioAudioSink.configure()` já recebe o `inputFormat.sampleRate` — basta garantir que ao chamar `setPreferredMixerAttributes()`, o format builder use esse sample rate, não um fixo.

---

## TAREFA 3: UI Toggle com Warnings

Na tela de configurações de áudio:
- Toggle "Bit-Perfect USB Audio" (desabilitado por padrão)
- Ao ativar, mostrar warning: "O modo bit-perfect desabilita controle de volume do sistema e todos os efeitos DSP. Funciona apenas com DACs USB compatíveis."
- Se Android < 14, mostrar o toggle desabilitado com texto explicativo
- Se ativado mas sem USB conectado, mostrar aviso

---

## Arquivos-chave para editar

| Arquivo | O que fazer |
|---------|-------------|
| `engine/src/main/java/.../audio/AaudioAudioSink.kt` | Adicionar `setPreferredMixerAttributes()` em `configure()` |
| `engine/src/main/java/.../processors/AaudioOutputProcessor.kt` | Possivelmente adaptar para receber sample rate dinâmico |
| `engine/src/main/java/.../services/FelicityPlayerService.kt` | Já tem `detectActiveOutputDevice()` e `outputDevicePriority()` — reusar para detectar USB |
| `preferences/src/main/java/.../AudioPreferences.kt` | Adicionar `BIT_PERFECT_ENABLED` preference |
| `engine/src/main/jni/aaudio-player.cpp` | Talvez nenhuma mudança necessária — o stream nativo já aceita qualquer sample rate |
| UI de settings (localizar) | Adicionar toggle bit-perfect + botão diagnóstico |

---

## Referências técnicas

- API doc: https://developer.android.com/reference/android/media/AudioMixerAttributes
- Sample code Google: https://developer.android.com/media/platform/improve-audio-playback
- AOSP implementation guide: https://source.android.com/docs/core/audio/preferred-mixer-attr
- Issue Media3 (aberta): https://github.com/androidx/media/issues/415
- Issue Auxio (blocked): https://github.com/OxygenCobalt/Auxio/issues/1210

---

## Ordem de execução recomendada

1. **PRIMEIRO:** Implementar o diagnóstico HAL (Tarefa 1) — sem isso não sabemos se o hardware suporta
2. **SEGUNDO:** Testar no Samsung S26 Ultra com um DAC USB conectado — verificar output do dialog
3. **TERCEIRO:** Se BIT_PERFECT aparecer como suportado, implementar a Tarefa 2
4. **QUARTO:** Adicionar UI toggle com warnings (Tarefa 3)
5. **QUINTO:** Testar reprodução real — verificar no display do DAC se o sample rate muda conforme o arquivo

---

## Notas finais

- Este seria o **PRIMEIRO app público** a usar a API `MIXER_BEHAVIOR_BIT_PERFECT` do Android 14+
- Se funcionar, é um marco significativo para a comunidade audiófila Android
- Se o HAL do S26 Ultra NÃO suportar, o diagnóstico vai revelar isso imediatamente e evita perda de tempo
- O codebase do Felicity é excelente — bem modularizado, documentado, com pipeline nativo otimizado
- Licença: AGPL v3 — fork é permitido, mas deve manter código aberto
