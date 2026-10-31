# Plano de desenvolvimento

## M0 — Base Android ARM64

- [x] Criar o repositório separado.
- [x] Fixar a revisão de PS2Recomp usada pelo port de referência.
- [x] Criar Activity Android, JNI e alvo CMake.
- [x] Implementar seleção da imagem com SAF e descritor nativo.
- [x] Implementar inicialização Vulkan, swapchain e frame de teste.
- [ ] Confirmar build APK pelo GitHub Actions.
- [ ] Validar instalação e ciclo de vida em dispositivo físico.

## M1 — Leitura e identificação do disco

- [ ] Verificar assinatura e estrutura ISO9660.
- [ ] Identificar o executável de boot e a revisão de disco suportada.
- [ ] Validar hashes e intervalos dos módulos conforme o perfil upstream.
- [ ] Adicionar logs nativos úteis sem expor dados pessoais do URI.
- [ ] Testar provedores SAF diferentes e descritores com offset.

## M2 — Pipeline de recompilação do jogo

- [ ] Adaptar as ferramentas de geração para ambiente host (Windows/Linux).
- [ ] Gerar código C++ localmente a partir da mídia fornecida pelo usuário.
- [ ] Manter os arquivos gerados fora do Git.
- [ ] Compilar objetos ARM64 com NDK.
- [ ] Separar geração host-side e execução Android.

## M3 — Runtime PS2

- [ ] Adaptar o ponto de entrada e o ciclo de vida do runtime.
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
