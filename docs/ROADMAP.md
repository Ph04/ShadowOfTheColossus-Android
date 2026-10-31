# Plano de desenvolvimento

## M0 — Base Android ARM64

- [x] Criar o repositório separado.
- [x] Fixar a revisão de PS2Recomp usada pelo port de referência.
- [x] Criar Activity Android, JNI e alvo CMake.
- [x] Implementar seleção da imagem com SAF e descritor nativo.
- [x] Implementar inicialização Vulkan, swapchain e frame de teste.
- [x] Compilar e publicar o artefato APK debug ARM64 pela CI.
- [ ] Validar instalação e ciclo de vida em dispositivo físico.

## M1 — Leitura e identificação do disco

- [x] Verificar descritores, blocos e diretórios ISO9660.
- [x] Identificar SYSTEM.CNF, executável de boot e versão PAL SCES-53326 v1.00.
- [x] Validar formato do ELF, endereço de entrada e hash SHA-256.
- [x] Validar tamanho, assinatura xff2 e SHA-256 de KERNEL, MANAGER e GAMECORE.
- [x] Criar ferramenta host para verificar a imagem e extrair os quatro insumos de recompilação.
- [x] Manter a ISO e os binários extraídos fora do Git.
- [x] Integrar validação de mídia diretamente no aplicativo Android.
- [x] Extrair os arquivos exigidos no armazenamento privado do aplicativo, sem copiar a ISO inteira.
- [ ] Testar com imagem legítima em dispositivo físico.

## M2 — Pipeline de recompilação do jogo

- [x] Implementar parser XFF2 com validação de limites, seções, símbolos e tabelas de relocação.
- [x] Aplicar as quatro relocations MIPS do perfil e resolver imports exportados por módulos anteriores.
- [x] Gerar seções relocadas, manifesto local e lista de imports ainda não resolvidos para o runtime.
- [x] Criar verificador de ELF contra dump de RAM, com máscara para imports dinâmicos conhecidos.
- [ ] Validar os módulos XFF reais contra dump de RAM do PCSX2/ARMSX2 em estado conhecido.
- [x] Gerar mapa de funções e símbolos a partir dos símbolos XFF disponíveis.
- [x] Gerar ELF32/MIPS unificado com entry point, PT_LOAD, seções BSS e tabela de símbolos.
- [x] Gerar configuração TOML e lista de relocations dinâmicas suportadas pelo PS2Recomp.
- [x] Compilar a biblioteca PS2Recomp para ARM64 e chamá-la pelo JNI, sem executar binários Linux.
- [x] Executar a etapa de recompilação de funções no próprio aplicativo.
- [x] Exibir progresso, contagem de funções processadas e estimativa de tempo.
- [x] Exportar ELF, relatórios, configuração e código C++ em um ZIP local.
- [ ] Validar o resultado com a mídia real e comparar com dump de RAM do emulador.
- [ ] Revisar cobertura/erros do código gerado e adaptar os módulos do jogo ao runtime Android.
- [ ] Compilar objetos gerados ARM64 e ligá-los ao runtime Android.
- [ ] Compilar objetos ARM64 com NDK e ligar ao runtime.

## M3 — Runtime PS2

- [ ] Adaptar o ponto de entrada e ciclo de vida do runtime.
- [ ] Ligar scheduler/threads e memória do Emotion Engine.
- [ ] Integrar IOP, SIF, DMA, VIF, VU0/VU1, CDVD e SPU2.
- [ ] Implementar suspensão/retomada e encerramento limpo.
- [ ] Testar boot ELF antes de tentar gameplay.

## M4 — Graphics Synthesizer

- [ ] Mapear a interface GS do PS2Recomp.
- [ ] Implementar backend Vulkan fiel às regras do GS.
- [ ] Implementar variante compute e CPU de referência.
- [ ] Comparar imagens e transferências com a referência.
- [ ] Tratar shader cache, perda de contexto e mudança de superfície.

## M5 — Controles, áudio e armazenamento

- [ ] DualShock 2 virtual em tela com layout configurável.
- [ ] Gamepads Bluetooth e mapeamento de botões.
- [ ] Áudio Android e sincronização com o relógio do runtime.
- [ ] Memory card, save states e retomada.
- [ ] Orientação, pausa e foco de janela.

## M6 — Desempenho e compatibilidade

- [ ] Perfil térmico e de memória em aparelhos ARM64 modestos.
- [ ] Limites adaptativos de resolução e apresentação.
- [ ] Otimizações de VU1 e rasterização sem alterar precisão.
- [ ] Testes de longa duração e cenas pesadas.
- [ ] Matriz de compatibilidade por GPU/driver.

Uma etapa só muda para concluída após teste reproduzível. APK compilado não significa que o jogo iniciou ou está jogável.
