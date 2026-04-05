# BIT-PERFECT USB AUDIO ACHIEVED — 2026-04-03

## Status: FUNCIONANDO!

Bit-perfect USB audio via driver direto USB, sem AudioFlinger, sem mixer,
sem resample. Confirmado no iBasso DX340 + Cayin RU7.

### Verificação
```
Driver: usbfs (nosso, NÃO snd-usb-audio)
#Iso: 7-8 URBs em voo (pipeline cheio)
Alt setting: 3 (32-bit PCM)
ALSA card1: inexistente (kernel NÃO controla o DAC)
AudioFlinger: NÃO toca no USB
Som: LIMPO, sem glitches após os 2 primeiros segundos
Sample rates testados: 44.1kHz, 96kHz — DAC mostra rate correto
```

### O que resolveu (em ordem de descoberta)
1. **Clock Source ID = 0x05** (não 0x0B) — descoberto parseando USB descriptors no iBasso com root
2. **USBDEVFS_URB_ISO_ASAP flag** — sem ela, pacotes eram aceitos mas nunca transmitidos
3. **Java setInterface() para alocar ISO bandwidth** — native USBDEVFS_SETINTERFACE não aloca
4. **Pipeline de 8 URBs** — apps comerciais usam ~74, nós 8. Sem pipeline (#Iso=0), DAC não produz som
5. **32-bit PCM (alt=3)** — prática padrão: sempre usar 32-bit, mesmo pra sources 16-bit

### Problemas conhecidos
- ~2 segundos de silence no início (pipeline de silence URBs sendo drenado)
- No Samsung S26 Ultra, snd-usb-audio binda antes do nosso handler (~3ms race condition)
- A preference bit-perfect precisa estar ativa ANTES de conectar o DAC
