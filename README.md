# Shadow of the Colossus — Android ARM64

Port nativo para Android ARM64 baseado no trabalho de recompilação estática do projeto [PS2Recomp](https://github.com/LightVelox/PS2Recomp) e no port de referência [sotc-vibe-pc](https://github.com/LightVelox/sotc-vibe-pc).

> **Estado:** o APK prepara a mídia, liga os módulos e executa o PS2Recomp ARM64 no próprio telefone, gerando ELF e arquivos C++. Ainda não há uma build jogável nem suporte completo ao renderer GS.

## Objetivo

Executar a versão PAL de *Shadow of the Colossus* (PS2, SCES-53326 v1.00) como aplicativo Android nativo para **arm64-v8a**, reutilizando a recompilação de código MIPS R5900 para C++ e adaptando o runtime e o subsistema gráfico ao Android.

O projeto não inclui ISO, BIOS, arquivos comerciais do jogo nem código C++ gerado a partir de uma imagem do jogo. Cada pessoa deve fornecer sua própria cópia legítima e compatível.

## Direção técnica

- Aplicativo Android com Java Activity e código nativo C++20 via JNI.
- Android NDK r30 LTS, CMake e Gradle.
- ARM64 como ABI inicial.
- SAF para selecionar a imagem sem exigir acesso amplo ao armazenamento.
- Leitura nativa por descritor e offsets, sem copiar a ISO inteira.
- Vulkan como backend gráfico Android pretendido.
- PS2Recomp mantido como submódulo com revisão fixada.

## Preparar sua imagem diretamente no Android

Não é necessário ter um computador para a etapa de preparação. No APK, selecione sua imagem PAL **SCES-53326 v1.00** e toque em **Preparar e recompilar neste telefone**. O aplicativo:

1. Valida a imagem, SYSTEM.CNF, o ELF de boot e os hashes dos três módulos XFF.
2. Extrai apenas os arquivos necessários para o armazenamento privado do aplicativo.
3. Liga KERNEL, MANAGER e GAMECORE e gera o ELF32/MIPS unificado.
4. Executa a biblioteca PS2Recomp compilada para ARM64 e gera os arquivos C++.
5. Mostra a etapa atual, percentual, funções processadas e estimativa de tempo quando disponível.

Ao terminar, use **Exportar resultados (ZIP)** para salvar o ELF, relatórios, configuração e código C++ gerado.

A imagem ISO e os módulos não são enviados para a internet nem incluídos no repositório. A primeira versão aceita apenas PAL SCES-53326 v1.00. A comparação com um dump de RAM de emulador continua sendo uma validação adicional; o APK não presume que o jogo já esteja jogável.

## Ferramentas de desenvolvimento no computador

Para quem quiser reproduzir a validação no computador:

    python tools/inspect_disc.py /caminho/para/SCES-53326.iso

Para preparar os arquivos necessários ao próximo passo de ligação/recompilação:

    python tools/inspect_disc.py /caminho/para/SCES-53326.iso --extract-dir build/game-input

O perfil estrito aceita apenas PAL **SCES-53326 v1.00**. A ferramenta confere SYSTEM.CNF, o ELF de boot e os três módulos XFF por tamanho e SHA-256. Os arquivos extraídos ficam em build/game-input, ignorado pelo Git. Consulte [docs/ISO_WORKFLOW.md](docs/ISO_WORKFLOW.md).

A ferramenta Python de desenvolvimento também pode executar a primeira etapa de ligação dos módulos XFF localmente:

    python tools/link_xff.py --files build/game-input --out build/xff-linked

Essa etapa valida e aplica as relocações MIPS conhecidas, combina o ELF de boot com os segmentos relocados, gera o mapa de funções e prepara o TOML do PS2Recomp. Ela também gera um inventário de imports dinâmicos e permite comparar o ELF com um dump de RAM do emulador: `python tools/verify_linked_ram.py --out build/xff-linked --ram /caminho/para/ee-ram.bin`. Ainda não executa a recompilação nem compila o código C++; veja [docs/XFF_LINKER.md](docs/XFF_LINKER.md).

## Build local

Requisitos: JDK 17, Gradle 9.6.0, Android SDK Platform 36, NDK 30.0.16248370 e CMake 3.22.1.

    gradle --no-daemon :app:assembleDebug

Para inicializar o submódulo:

    git submodule update --init --recursive

## Documentação

- [Arquitetura](docs/ARCHITECTURE.md)
- [Validação e preparação do disco](docs/ISO_WORKFLOW.md)
- [Parser e linker XFF2](docs/XFF_LINKER.md)
- [Plano de port](docs/ROADMAP.md)
- [Backend gráfico](docs/GRAPHICS_BACKEND.md)
- [Compilação](docs/BUILDING.md)
- [Referências e versões fixadas](docs/UPSTREAM.md)

## Aviso

Projeto de fã independente, sem vínculo com Sony Interactive Entertainment, Team Ico ou Japan Studio. *Shadow of the Colossus* e marcas associadas pertencem aos respectivos titulares. A licença do código-fonte não concede direitos sobre o jogo ou seus dados.
