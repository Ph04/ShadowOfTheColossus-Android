# Backend gráfico Android

## Por que Vulkan

O renderer do port PC usa OpenGL 4.6 e recursos especializados para reproduzir regras do Graphics Synthesizer (GS), incluindo blending, testes de profundidade e ordenação de fragmentos. Esse caminho não pode ser usado diretamente em Android.

O backend deste projeto será Vulkan. O primeiro incremento cria uma superfície Android, seleciona uma GPU e apresenta frames. Ele também registra se `VK_EXT_fragment_shader_interlock` e o recurso pixel-interlock estão disponíveis, mas ainda não executa rasterização GS.

## Caminhos a implementar

1. Vulkan com fragment shader interlock, quando a GPU oferecer a extensão e o recurso necessário.
2. Rasterizador GS por compute shaders, preservando as regras de ordenação e sincronização.
3. Backend CPU de referência para validação e dispositivos que não atendam aos requisitos mínimos.

Não assumir que Vulkan 1.1 implica suporte a fragment shader interlock. A extensão e o recurso devem ser consultados em tempo de execução. A falta dessa extensão não pode causar crash nem produzir uma imagem incorreta silenciosamente.

## Referências Android/PS2 avaliadas

### Tenkaichi3DecompAndroid

O projeto `dfdx047/Tenkaichi3DecompAndroid` demonstra uma separação útil: launcher Android, motor do jogo em biblioteca ARM64 e camada gráfica própria. A documentação de BT4 também descreve um runtime de recompilação sem janela que envia um fluxo ao GS da aplicação por um ring buffer; como opção, o projeto integra o GS do ARMSX2 através de um formato de fluxo compatível com GSDumpReplayer.

Isso é uma referência arquitetural, não uma integração pronta para este jogo. O PS2Recomp que usamos ainda precisa expor a interface de comandos/transferências do GS de modo compatível. O fluxo de BT4 não deve ser conectado diretamente ao nosso runtime sem provar a correspondência semântica de GIF, VIF, DMA, registradores e sincronização.

### AetherSX2 e ARMSX2

AetherSX2 é útil como referência histórica de otimizações Android, mas os repositórios públicos encontrados declaram que disponibilizam apenas a parte LGPL do código, não a implementação proprietária completa. Por isso, não é uma base adequada para copiar ou incorporar o renderer inteiro.

ARMSX2 é uma alternativa de código aberto baseada em PCSX2, orientada a ARM64, com backend GS Vulkan/OpenGL e adaptações para GPUs móveis. Sua licença declarada é GPL-3.0. Integrar seu GS diretamente no mesmo binário teria consequências de licenciamento que precisam ser avaliadas antes de qualquer incorporação. Por enquanto, ele será usado como referência técnica e como possível backend experimental isolado, não como dependência copiada para este repositório.

## Direção proposta para o nosso port

1. Definir uma interface interna independente de renderer para os eventos e dados do GS que o runtime realmente produzir.
2. Manter o Vulkan nativo como caminho principal a desenvolver, com backend CPU de referência para testes.
3. Avaliar um backend alternativo baseado em ARMSX2 somente depois de validar a compatibilidade do fluxo e revisar a licença GPL-3.0.
4. Não depender de drivers personalizados, como requisito obrigatório. Eles podem ser uma opção adicional no futuro, mas o renderer deve detectar recursos e funcionar com o driver Android padrão dentro dos limites suportados.

## Critérios de fidelidade

- Formatos e swizzles de VRAM do GS.
- Alpha blending e framebuffer masks.
- Depth test, scissor, dithering e alpha test.
- Semântica de transferências locais, host-to-local e local-to-host.
- Sincronização entre CPU, VIF, GIF, GS e apresentação.
- Comparação de capturas com o backend de referência antes de priorizar velocidade.

O frame escuro atual é apenas um teste de apresentação. Nenhuma cena do jogo é renderizada ainda.
