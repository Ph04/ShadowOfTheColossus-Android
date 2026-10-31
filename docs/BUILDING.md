# Compilação

## Ferramentas

- JDK 17.
- Gradle 9.6.0.
- Android SDK Platform 36 e Build Tools 36.0.0.
- Android NDK 30.0.16248370 (LTS).
- CMake 3.22.1.
- Git com suporte a submódulos.

## Preparação

Clone o repositório e inicialize o runtime:

    git clone --recurse-submodules https://github.com/Ph04/ShadowOfTheColossus-Android.git
    cd ShadowOfTheColossus-Android

Se já clonou sem submódulos:

    git submodule update --init --recursive

## Build debug

Com Gradle 9.6.0 disponível no PATH:

    gradle --no-daemon :app:assembleDebug

Saída esperada:

    android/app/build/outputs/apk/debug/app-debug.apk

A ABI configurada inicialmente é somente arm64-v8a. A compilação valida o esqueleto Android, JNI, acesso à imagem e apresentação Vulkan. Ela ainda não gera um port jogável.

## Observações

O submódulo PS2Recomp está fixado em um commit conhecido. O runtime ainda não é ligado ao aplicativo por padrão, pois seu backend host precisa de adaptação para Android. Não habilite SOTC_LINK_PS2_RUNTIME até essa integração ser concluída.
