# Shadow of the Colossus — Android ARM64

Port nativo para Android ARM64 baseado no trabalho de recompilação estática do projeto [PS2Recomp](https://github.com/LightVelox/PS2Recomp) e no port de referência [sotc-vibe-pc](https://github.com/LightVelox/sotc-vibe-pc).

> **Estado:** início do port Android. A base de aplicativo, o acesso à imagem do disco e a camada Vulkan estão sendo preparados. Ainda não há uma build jogável nem suporte completo ao renderer GS neste repositório.

## Objetivo

Executar a versão PAL de Shadow of the Colossus (PS2, referência SCES-53326 v1.00) como aplicativo Android nativo para **arm64-v8a**, reutilizando a recompilação de código MIPS R5900 para C++ e adaptando o runtime e o subsistema gráfico ao Android.

O projeto não inclui ISO, BIOS, arquivos comerciais do jogo nem código C++ gerado a partir de uma imagem do jogo. Cada pessoa deve fornecer sua própria cópia legítima e compatível.

## Direção técnica

- Aplicativo Android com Java Activity e código nativo C++20 via JNI.
- Android NDK r30 LTS, CMake e Gradle.
- ARM64 como ABI inicial; outras ABIs não fazem parte do escopo inicial.
- SAF (Storage Access Framework) para selecionar a imagem do disco sem exigir acesso amplo ao armazenamento.
- Leitura nativa por descritor de arquivo, com offsets, para evitar copiar uma imagem de vários gigabytes para o armazenamento privado.
- Vulkan como backend gráfico Android pretendido.
- O runtime PS2Recomp será integrado depois de adaptar suas fronteiras de plataforma e o renderer GS.

## Requisitos de desenvolvimento

- Android Studio/SDK com Android Platform 36.
- JDK 17.
- Gradle 9.6.0.
- Android NDK 30.0.16248370.
- CMake 3.22.1 ou superior.
- Dispositivo Android ARM64 com suporte Vulkan para validar a camada gráfica.

## Build local

Depois de instalar as dependências acima:

```bash
gradle --version
gradle --no-daemon :app:assembleDebug
```

O APK de depuração é gerado em android/app/build/outputs/apk/debug/.

Para inicializar o submódulo PS2Recomp:

```bash
git submodule update --init --recursive
```

O workflow do GitHub Actions também executa a compilação Android em cada push e pull request.

## Estado e planejamento

Consulte:

- [Arquitetura](docs/ARCHITECTURE.md)
- [Plano de port](docs/ROADMAP.md)
- [Backend gráfico](docs/GRAPHICS_BACKEND.md)
- [Compilação](docs/BUILDING.md)
- [Referências e versões fixadas](docs/UPSTREAM.md)

## Aviso

Este é um projeto de fã, independente e sem vínculo com Sony Interactive Entertainment, Team Ico ou Japan Studio. Shadow of the Colossus e marcas associadas pertencem aos respectivos titulares. A licença do código-fonte não concede direitos sobre o jogo ou seus dados.