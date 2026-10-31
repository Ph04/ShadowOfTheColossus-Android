# Backend gráfico Android

## Por que Vulkan

O renderer do port PC usa OpenGL 4.6 e recursos especializados para reproduzir regras do Graphics Synthesizer (GS), incluindo blending, testes de profundidade e ordenação de fragmentos. Esse caminho não pode ser usado diretamente em Android.

O backend deste projeto será Vulkan. O primeiro incremento cria uma superfície Android, seleciona uma GPU e apresenta frames. Ele também registra se VK_EXT_fragment_shader_interlock e o recurso pixel-interlock estão disponíveis, mas ainda não executa rasterização GS.

## Caminhos a implementar

1. Vulkan com fragment shader interlock, quando a GPU oferecer a extensão e o recurso necessário.
2. Rasterizador GS por compute shaders, preservando as regras de ordenação e sincronização.
3. Backend CPU de referência para validação e dispositivos que não atendam aos requisitos mínimos.

Não assumir que Vulkan 1.1 implica suporte a fragment shader interlock. A extensão e o recurso devem ser consultados em tempo de execução. A falta dessa extensão não pode causar crash nem produzir uma imagem incorreta silenciosamente.

## Critérios de fidelidade

- Formatos e swizzles de VRAM do GS.
- Alpha blending e framebuffer masks.
- Depth test, scissor, dithering e alpha test.
- Semântica de transferências locais, host-to-local e local-to-host.
- Sincronização entre CPU, VIF, GIF, GS e apresentação.
- Comparação de capturas com o backend de referência antes de priorizar velocidade.

O frame escuro atual é apenas um teste de apresentação. Nenhuma cena do jogo é renderizada ainda.
