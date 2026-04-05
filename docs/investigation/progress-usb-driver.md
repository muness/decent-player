# Progresso do Driver USB Bit-Perfect — 2026-04-03 (Sessão Final)

## Status: BLOQUEADO — kernel race condition

### O problema final
O módulo kernel `snd-usb-audio` binda automaticamente a qualquer USB audio device em ~3ms
durante o probe/re-enumerate. Ele configura o clock do DAC a 384kHz (Samsung UHQA default).
Nosso `claimInterface(force=true)` desconecta o kernel driver MAS o clock do DAC já foi
setado e NÃO muda com nosso SET_CUR subsequente (o registro de controle é atualizado mas
o PLL do hardware permanece em 384kHz — confirmado via feedback endpoint).

Tentamos `USBDEVFS_RESET` + claim imediato no native code (mesma chamada, microsegundos
de diferença) — o kernel AINDA binda antes de nós por ~3ms e configura o clock.

### O que funciona comprovadamente
- ✅ Tudo da lista anterior (ISO_ASAP, singleton, feedback, etc)
- ✅ `USBDEVFS_RESET` via ioctl nativo (SUCCESS, fd permanece válido)
- ✅ Claim nativo imediato após reset (ret=0)
- ✅ Driver binding final = `usbfs` (nosso, não snd-usb-audio)
- ✅ URBs transmitidos no barramento (feedback endpoint responde)
- ✅ DAC reconhece stream a 384kHz (mostra no display)
- ✅ SET_CUR formatado identicamente ao kernel snd-usb-audio

### O que NÃO funciona
- ❌ SET_CUR não muda o clock REAL do hardware (feedback sempre 384kHz)
- ❌ snd-usb-audio sempre binda antes de nós por ~3ms no reset/re-enumerate
- ❌ Zero som no DAC (dados a 44100Hz mandados para clock de 384kHz = incompatibilidade)

### Abordagem recomendada para próxima sessão

**Opção A: Aceitar o clock do DAC e fazer resampling em software**
- Ler o feedback endpoint para saber o clock real do DAC
- Se 384kHz: fazer resampling de 44100→384000 em software (não é bit-perfect, mas funciona)
- Vantagem: áudio funcional imediatamente
- Desvantagem: não é bit-perfect (mas pode ser melhor que AudioFlinger)

**Opção B: Usar libusb compilado pro Android**  
- libusb_reset_device() pode ter comportamento diferente do USBDEVFS_RESET
- libusb lida com a re-enumeração e pode clamar antes do kernel
- Adicionar libusb como dependência CMake (FetchContent igual ao PFFFT)

**Opção C: Blacklist snd-usb-audio via propriedade do sistema (precisa de root)**
- `echo "blacklist snd-usb-audio" > /etc/modprobe.d/blacklist.conf`
- Ou `setprop sys.usb.config "none"` antes de conectar
- Requer root — não viável pra distribuição

**Opção D: Investigar como apps comerciais previnem o kernel de bindar**
- Talvez usam uma API privada do Android
- Talvez registram como default handler de USB audio no manifest de forma especial
- Talvez fazem o claim numa etapa mais cedo do lifecycle USB do Android

### Todos os arquivos do driver (em C:/tmp/Felicity)
```
engine/src/main/jni/usb-audio-output.cpp      — V3 com ISO_ASAP + USBDEVFS_RESET + native claim
engine/src/main/jni/usb-audio-output.h
engine/src/main/jni/CMakeLists.txt             — usb-audio-output.cpp adicionado
engine/src/main/java/.../processors/UsbAudioOutputProcessor.kt — com nativeUsbReset()
engine/src/main/java/.../audio/UsbAudioManager.kt — singleton + resetAndReopen()
engine/src/main/java/.../audio/AaudioAudioSink.kt — USB branch + delegate speaker routing
preferences/src/main/java/.../AudioPreferences.kt — BIT_PERFECT_USB_ENABLED
music/src/main/java/.../extensions/fragments/PreferenceFragment.kt — toggle
music/src/main/java/.../activities/MainActivity.kt — USB_DEVICE_ATTACHED handler
music/src/main/res/xml/usb_audio_device_filter.xml
music/src/main/AndroidManifest.xml — intent filter + launchMode=singleTask
music/src/main/java/.../dialogs/app/UsbAudioDiagnosticDialog.kt
music/src/main/res/layout/dialog_usb_audio_diagnostic.xml
shared/src/main/res/values/strings.xml — bit-perfect strings
```

### Dados do Cayin RU7 (referência completa)
- USB Audio Class 2.0 (protocol=32)
- Clock Source Entity ID: **0x0B**
- Interface 0: AudioControl
- Interface 1: AudioStreaming (alt 0-4)
- EP OUT 0x01: Isochronous, Async, maxPacket=776, interval=1 (125μs)  
- EP IN 0x81: Feedback, maxPacket=4, interval=4 (500μs)
- Alt settings: 0=zero-bw, 1=16bit, 2=24bit, 3=32bit, 4=DSD(?)
- Supported rates (ALSA): 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000
- Samsung UHQA default: 384kHz (setado automaticamente pelo snd-usb-audio probe)
- Feedback format: 16.16 fixed-point (48.0016 = 384012.7 Hz típico)
