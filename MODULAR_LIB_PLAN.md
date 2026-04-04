# Plano: Extrair USB Audio Driver em Libs Standalone

## Context

O repo `decent-player` contém um driver USB bit-perfect dentro do Felicity (harness de teste). O objetivo é extrair esse driver em **duas bibliotecas Android standalone** que qualquer dev possa importar via Gradle, mantendo tudo no mesmo repo. O Felicity depois passa a depender dessas libs em vez de ter o código USB inline.

Cada lib é um **projeto Gradle independente** com seu próprio wrapper, build files, etc. Ficam em pastas **separadas** na raiz do repo. Todos os nomes usam o prefixo "decent" como assinatura.

---

## Estrutura Final

```

decent-player/

├── docs/

├── driver/

│   ├── Felicity/                                  ← harness (atualizado pra depender das libs)

│   └── decent-usb-audio-driver/                   ← NOVO: projeto Gradle independente (core)

│       ├── settings.gradle.kts

│       ├── build.gradle.kts

│       ├── gradle.properties

│       ├── gradle/wrapper/

│       ├── gradlew, gradlew.bat

│       └── decent-usb-audio-driver/               ← módulo (nome = assinatura "decent")

│           ├── build.gradle.kts

│           ├── consumer-rules.pro

│           └── src/main/

│               ├── AndroidManifest.xml

│               ├── jni/

│               │   ├── CMakeLists.txt

│               │   ├── usb-audio-output.cpp

│               │   └── usb-audio-output.h

│               ├── kotlin/com/decentplayer/usbaudio/

│               │   ├── UsbAudioDevice.kt

│               │   ├── UsbAudioStream.kt

│               │   ├── UsbAudioDeviceInfo.kt

│               │   ├── UsbAudioPermissionHelper.kt

│               │   └── UsbAudioException.kt

│               └── res/xml/

│                   └── usb_audio_device_filter.xml

│

├── wrapper/

│   └── decent-usb-audio-wrapper-media3/           ← NOVO: projeto Gradle independente (wrapper)

│       ├── settings.gradle.kts

│       ├── build.gradle.kts

│       ├── gradle.properties

│       ├── gradle/wrapper/

│       ├── gradlew, gradlew.bat

│       └── decent-usb-audio-wrapper-media3/       ← módulo (nome = assinatura "decent")

│           ├── build.gradle.kts

│           ├── consumer-rules.pro

│           └── src/main/

│               ├── AndroidManifest.xml

│               └── kotlin/com/decentplayer/usbaudio/media3/

│                   ├── UsbAudioSink.kt

│                   ├── UsbAudioSinkConfig.kt

│                   └── PcmUtils.kt

```

---

## Fase 1: Criar projeto do driver (`driver/decent-usb-audio-driver/`)

### 1.1 Criar estrutura de diretórios

```

driver/decent-usb-audio-driver/

├── decent-usb-audio-driver/src/main/kotlin/com/decentplayer/usbaudio/

├── decent-usb-audio-driver/src/main/jni/

├── decent-usb-audio-driver/src/main/res/xml/

└── gradle/wrapper/

```

### 1.2 Criar arquivos de build

**`settings.gradle.kts`** (root):

```kotlin

pluginManagement {

    repositories {

        gradlePluginPortal()

        google()

        mavenCentral()

    }

}

dependencyResolutionManagement {

    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)

    repositories {

        google()

        mavenCentral()

    }

}

rootProject.name = "decent-usb-audio-driver"

include(":decent-usb-audio-driver")

```

**`build.gradle.kts`** (root):

```kotlin

plugins {

    id("com.android.library") version "9.1.0" apply false

    id("org.jetbrains.kotlin.android") version "2.3.10" apply false

}

```

**`gradle.properties`**:

```properties

org.gradle.jvmargs=-Xmx2048m -Dfile.encoding=UTF-8

android.useAndroidX=true

kotlin.code.style=official

android.nonTransitiveRClass=true

```

### 1.3 Copiar Gradle wrapper do Felicity

- Copiar `gradlew`, `gradlew.bat`, `gradle/wrapper/*` do Felicity

---

## Fase 2: Módulo Core (`decent-usb-audio-driver/decent-usb-audio-driver/`)

### 2.1 `build.gradle.kts`

```kotlin

plugins {

    id("com.android.library")

    id("org.jetbrains.kotlin.android")

}

android {

    namespace = "com.decentplayer.usbaudio"

    compileSdk = 36

    defaultConfig {

        minSdk = 29

        externalNativeBuild { cmake { cppFlags("") } }

    }

    ndkVersion = "29.0.14206865"

    externalNativeBuild { cmake { path("src/main/jni/CMakeLists.txt") } }

    compileOptions {

        sourceCompatibility = JavaVersion.VERSION_21

        targetCompatibility = JavaVersion.VERSION_21

    }

    kotlin { jvmToolchain(21) }

}

dependencies {

    implementation("androidx.core:core-ktx:1.18.0")

}

```

**Deps**: SOMENTE `androidx.core:core-ktx`. Zero Hilt, zero Media3, zero módulos internos.

### 2.2 `CMakeLists.txt` (novo, limpo)

```cmake

cmake_minimum_required(VERSION 3.22.1)

project("decent_usb_audio")

add_library(decent_usb_audio SHARED usb-audio-output.cpp)

find_library(log-lib log)

target_link_libraries(decent_usb_audio ${log-lib})

```

Sem PFFFT, sem AAudio, sem DSP. Só o driver USB.

### 2.3 `usb-audio-output.cpp` — Copiar e renomear JNI symbols

Copiar de `engine/src/main/jni/usb-audio-output.cpp` e `.h`. Renomear **8 funções JNI**:

Prefixo antigo: `Java_app_simple_felicity_engine_processors_UsbAudioOutputProcessor`

Prefixo novo: `Java_com_decentplayer_usbaudio_UsbAudioStream`

Funções: `nativeUsbAudioCreate`, `nativeUsbAudioSetAltSetting`, `nativeUsbAudioSetSampleRate`, `nativeUsbAudioStart`, `nativeUsbAudioWrite`, `nativeUsbAudioStop`, `nativeUsbAudioDestroy`, `nativeUsbReset`

### 2.4 `UsbAudioStream.kt` (ex-`UsbAudioOutputProcessor.kt`)

- **Arquivo fonte**: `engine/src/main/java/.../processors/UsbAudioOutputProcessor.kt` (152 linhas)

- Package: `com.decentplayer.usbaudio`

- Classe: `UsbAudioStream` (era `UsbAudioOutputProcessor`)

- `System.loadLibrary("felicity_audio_engine")` → `System.loadLibrary("decent_usb_audio")`

- Remover imports de `app.simple.felicity.*`

### 2.5 `UsbAudioDevice.kt` (ex-`UsbAudioManager.kt`)

- **Arquivo fonte**: `engine/src/main/java/.../audio/UsbAudioManager.kt` (573 linhas)

- Package: `com.decentplayer.usbaudio`

- Classe: `UsbAudioDevice` (era `UsbAudioManager`)

- Manter singleton pattern (necessário pra compartilhar conexão USB entre componentes)

- Action string de permissão: `"app.simple.felicity.USB_PERMISSION"` → `"${context.packageName}.USB_AUDIO_PERMISSION"` (dinâmico)

- Referência interna: `UsbAudioOutputProcessor.nativeUsbReset(fd)` → `UsbAudioStream.nativeUsbReset(fd)`

- Extrair `UsbAudioDeviceInfo` data class pro próprio arquivo

### 2.6 `UsbAudioDeviceInfo.kt` (novo, extraído)

Data class pública com campos: connection, fd, deviceName, interfaceId, endpointOutAddress, endpointFeedbackAddress, maxPacketSize, altSettingCount, clockSourceId, bestAltSetting, bestBitDepth.

### 2.7 `UsbAudioPermissionHelper.kt` (novo)

- Valida em runtime se o manifest do app consumidor tem o intent filter USB correto

- Logga warning claro se faltar configuração

- Método `handleIntent(intent): UsbDevice?` pra facilitar `onNewIntent`

### 2.8 `UsbAudioException.kt` (novo)

Sealed class com subtipos: `DeviceNotFoundException`, `PermissionDeniedException`, `DeviceOpenFailedException`, `StreamCreationFailedException`, `InterfaceClaimFailedException`.

### 2.9 `AndroidManifest.xml` (mínimo)

```xml

<manifest xmlns:android="http://schemas.android.com/apk/res/android">

    <uses-feature android:name="android.hardware.usb.host" android:required="false" />

</manifest>

```

### 2.10 `usb_audio_device_filter.xml`

Copiar de `music/src/main/res/xml/usb_audio_device_filter.xml`. Shipped dentro do AAR — merge automático.

---

## Fase 3: Criar projeto do wrapper (`wrapper/decent-usb-audio-wrapper-media3/`)

### 3.1 Criar estrutura de diretórios

```

wrapper/decent-usb-audio-wrapper-media3/

├── decent-usb-audio-wrapper-media3/src/main/kotlin/com/decentplayer/usbaudio/media3/

└── gradle/wrapper/

```

### 3.2 Criar arquivos de build

**`settings.gradle.kts`** (root do wrapper):

```kotlin

pluginManagement {

    repositories {

        gradlePluginPortal()

        google()

        mavenCentral()

    }

}

dependencyResolutionManagement {

    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)

    repositories {

        google()

        mavenCentral()

    }

}

rootProject.name = "decent-usb-audio-wrapper-media3"

include(":decent-usb-audio-wrapper-media3")



// Inclui o driver como composite build pra resolver project dependency

includeBuild("../../driver/decent-usb-audio-driver") {

    dependencySubstitution {

        substitute(module("com.decentplayer:decent-usb-audio-driver")).using(project(":decent-usb-audio-driver"))

    }

}

```

**`build.gradle.kts`** (root):

```kotlin

plugins {

    id("com.android.library") version "9.1.0" apply false

    id("org.jetbrains.kotlin.android") version "2.3.10" apply false

}

```

**`gradle.properties`**: (mesmo do driver)

### 3.3 Copiar Gradle wrapper do Felicity

- Copiar `gradlew`, `gradlew.bat`, `gradle/wrapper/*`

---

## Fase 4: Módulo Wrapper (`decent-usb-audio-wrapper-media3/decent-usb-audio-wrapper-media3/`)

### 4.1 `build.gradle.kts`

```kotlin

plugins {

    id("com.android.library")

    id("org.jetbrains.kotlin.android")

}

android {

    namespace = "com.decentplayer.usbaudio.media3"

    compileSdk = 36

    defaultConfig { minSdk = 29 }

    compileOptions {

        sourceCompatibility = JavaVersion.VERSION_21

        targetCompatibility = JavaVersion.VERSION_21

    }

    kotlin { jvmToolchain(21) }

}

dependencies {

    api("com.decentplayer:decent-usb-audio-driver")

    implementation("androidx.core:core-ktx:1.18.0")

    implementation("androidx.media3:media3-exoplayer:1.9.3")

    implementation("androidx.media3:media3-common:1.9.3")

}

```

### 4.2 `UsbAudioSinkConfig.kt` (novo)

```kotlin

data class UsbAudioSinkConfig(

    val bitPerfectEnabled: Boolean = true,

    val forceRouteToSpeaker: Boolean = true

)

```

Substitui todas as chamadas a `AudioPreferences`.

### 4.3 `UsbAudioSink.kt` (extraído de `AaudioAudioSink.kt`)

**Arquivo fonte**: `engine/src/main/java/.../audio/AaudioAudioSink.kt` (598 linhas)

Extrair SOMENTE o path USB. Manter:

- `ForwardingAudioSink(delegate)` pattern

- `usbAudioStream: UsbAudioStream?`

- `usbAudioDevice: UsbAudioDevice`

- `configureUsbBitPerfect()` (linhas 453-558)

- `writeSnapshotToUsb()` (linhas 566-582)

- `releaseUsbStream()` (linhas 585-592)

- `forceMediaToSpeaker()` / `clearForcedRouting()`

- `muteDelegateIfNeeded()` / `unmuteDelegateIfNeeded()`

Remover:

- Todo o path AAudio (`aaudioStream`, `AaudioOutputProcessor`, `writeSnapshotToAaudio`)

- Todo acesso a `AudioPreferences` → substituir por `config: UsbAudioSinkConfig`

- Bluetooth detection (irrelevante pra USB)

Constructor:

```kotlin

class UsbAudioSink(

    private val delegate: DefaultAudioSink,

    private val context: Context,

    private val config: UsbAudioSinkConfig = UsbAudioSinkConfig()

) : ForwardingAudioSink(delegate)

```

### 4.4 `PcmUtils.kt` (movido de engine/utils/)

- **Arquivo fonte**: `engine/src/main/java/.../utils/PcmUtils.kt` (115 linhas)

- Package: `com.decentplayer.usbaudio.media3`

- Manter `internal object` (só usado pelo wrapper)

- Sem mudanças na lógica

### 4.5 `AndroidManifest.xml` (vazio)

```xml

<manifest xmlns:android="http://schemas.android.com/apk/res/android" />

```

---

## Fase 5: Atualizar Felicity pra depender das libs

### 5.1 `settings.gradle` do Felicity — adicionar `includeBuild`

```groovy

includeBuild('../decent-usb-audio-driver') {

    dependencySubstitution {

        substitute module('com.decentplayer:decent-usb-audio-driver') using project(':decent-usb-audio-driver')

    }

}

includeBuild('../../wrapper/decent-usb-audio-wrapper-media3') {

    dependencySubstitution {

        substitute module('com.decentplayer:decent-usb-audio-wrapper-media3') using project(':decent-usb-audio-wrapper-media3')

    }

}

```

### 5.2 `engine/build.gradle` — adicionar dependência da lib

```groovy

dependencies {

    api 'com.decentplayer:decent-usb-audio-wrapper-media3'

    // ... manter outras deps existentes

}

```

### 5.3 `engine/CMakeLists.txt` — remover `usb-audio-output.cpp`

```cmake

add_library(felicity_audio_engine SHARED

            visualizer-fft.cpp

            dsp-engine.cpp

            aaudio-player.cpp)

            # usb-audio-output.cpp REMOVIDO

```

### 5.4 `AaudioAudioSink.kt` — atualizar imports

```kotlin

// ANTES:

import app.simple.felicity.engine.processors.UsbAudioOutputProcessor

import app.simple.felicity.engine.audio.UsbAudioManager

import app.simple.felicity.engine.utils.PcmUtils



// DEPOIS:

import com.decentplayer.usbaudio.UsbAudioStream

import com.decentplayer.usbaudio.UsbAudioDevice

import com.decentplayer.usbaudio.media3.PcmUtils

```

E atualizar referências: `UsbAudioOutputProcessor` → `UsbAudioStream`, `UsbAudioManager` → `UsbAudioDevice`.

### 5.5 Remover arquivos extraídos do engine

- `engine/.../processors/UsbAudioOutputProcessor.kt` — DELETAR

- `engine/.../audio/UsbAudioManager.kt` — DELETAR

- `engine/.../utils/PcmUtils.kt` — DELETAR

- `engine/src/main/jni/usb-audio-output.cpp` — DELETAR

- `engine/src/main/jni/usb-audio-output.h` — DELETAR

### 5.6 Atualizar `MainActivity.kt` (se referencia UsbAudioManager)

- `UsbAudioManager` → `UsbAudioDevice`

- Package: `com.decentplayer.usbaudio`

---

## Fase 6: Verificação

1. **Build do driver standalone**: `cd driver/decent-usb-audio-driver && ./gradlew assembleDebug`
   - Verificar que o AAR contém `libdecent_usb_audio.so` pra todas ABIs

   - Verificar que `res/xml/usb_audio_device_filter.xml` está no AAR

2. **Build do wrapper standalone**: `cd wrapper/decent-usb-audio-wrapper-media3 && ./gradlew assembleDebug`
   - Verificar que compila com referência ao driver via `includeBuild`

3. **Build do Felicity**: `cd driver/Felicity && ./gradlew assembleDebug`
   - Verificar que compila sem erros com as libs via `includeBuild`

4. **Verificar JNI**: Confirmar que os symbols no `.so` batem com o package `com.decentplayer.usbaudio.UsbAudioStream`

---

## Ordem de Execução

### Driver (Fases 1-2)

1. Criar estrutura de diretórios do driver

2. Criar build files (settings, root build, gradle.properties)

3. Copiar Gradle wrapper do Felicity

4. Criar módulo `decent-usb-audio-driver` (build.gradle.kts, manifest, CMake)

5. Copiar e refatorar C++ (JNI rename das 8 funções)

6. Copiar e refatorar `UsbAudioStream.kt` (ex-UsbAudioOutputProcessor)

7. Copiar e refatorar `UsbAudioDevice.kt` (ex-UsbAudioManager)

8. Criar novos arquivos (UsbAudioDeviceInfo, UsbAudioPermissionHelper, UsbAudioException)

9. Copiar `usb_audio_device_filter.xml`

10. **BUILD CHECK**: `./gradlew :decent-usb-audio-driver:assembleDebug`

### Wrapper (Fases 3-4)

11. Criar estrutura de diretórios do wrapper

12. Criar build files (settings com includeBuild do driver, root build, gradle.properties)

13. Copiar Gradle wrapper

14. Criar módulo `decent-usb-audio-wrapper-media3` (build.gradle.kts, manifest)

15. Extrair `UsbAudioSink.kt` do `AaudioAudioSink.kt` (somente USB path)

16. Criar `UsbAudioSinkConfig.kt`

17. Copiar `PcmUtils.kt` com novo package

18. **BUILD CHECK**: `./gradlew :decent-usb-audio-wrapper-media3:assembleDebug`

### Felicity Update (Fase 5)

19. Adicionar `includeBuild` no `settings.gradle` do Felicity

20. Atualizar `engine/build.gradle` (adicionar dep da lib)

21. Atualizar `engine/CMakeLists.txt` (remover usb-audio-output.cpp)

22. Atualizar imports no `AaudioAudioSink.kt` e `MainActivity.kt`

23. Remover arquivos extraídos do engine

24. **BUILD CHECK**: Felicity `./gradlew assembleDebug`

---

## Arquivos Críticos

| Arquivo Fonte (Felicity) | Ação | Destino |

|--------------------------|------|---------|

| `engine/src/main/jni/usb-audio-output.cpp` | Copiar + renomear 8 JNI symbols | `driver/decent-usb-audio-driver/decent-usb-audio-driver/src/main/jni/` |

| `engine/src/main/jni/usb-audio-output.h` | Copiar as-is | `driver/decent-usb-audio-driver/decent-usb-audio-driver/src/main/jni/` |

| `engine/.../UsbAudioManager.kt` (573 linhas) | Copiar → renomear + extrair DeviceInfo | `driver/.../kotlin/com/decentplayer/usbaudio/UsbAudioDevice.kt` |

| `engine/.../UsbAudioOutputProcessor.kt` (152 linhas) | Copiar → renomear + mudar loadLibrary | `driver/.../kotlin/com/decentplayer/usbaudio/UsbAudioStream.kt` |

| `engine/.../AaudioAudioSink.kt` (598 linhas) | Extrair USB path only | `wrapper/.../kotlin/com/decentplayer/usbaudio/media3/UsbAudioSink.kt` |

| `engine/.../utils/PcmUtils.kt` (115 linhas) | Copiar com novo package | `wrapper/.../kotlin/com/decentplayer/usbaudio/media3/PcmUtils.kt` |

| `music/.../usb_audio_device_filter.xml` (8 linhas) | Copiar | `driver/.../res/xml/usb_audio_device_filter.xml` |

| `Felicity/settings.gradle` | Adicionar 2x `includeBuild` | (editar in-place) |

| `engine/build.gradle` | Adicionar dep, manter o resto | (editar in-place) |

| `engine/CMakeLists.txt` | Remover `usb-audio-output.cpp` | (editar in-place) |
