# Referências upstream e versões

As referências são mantidas separadas do código Android para facilitar atualizações controladas.

## Port de referência para PC

- Repositório: https://github.com/LightVelox/sotc-vibe-pc
- Tag consultada: v0.2.0-preview.2
- Commit: 1ce4a5ea4955878bd2edabdc4e1a6b6f49a8e1ad
- Perfil documentado: PAL SCES-53326 v1.00.
- Limitação: a implementação upstream declara suporte somente a Windows e depende de OpenGL 4.6.

A revisão consultada também informa que o processo de build gera o código C++ localmente a partir da imagem do jogo; esses artefatos não devem ser enviados a este repositório.

## PS2Recomp

- Repositório usado pelo port de referência: https://github.com/LightVelox/PS2Recomp
- Commit fixado pelo submódulo: d06a4ab4a6902e0ae324f184c22581c3aa387c36
- Caminho: external/PS2Recomp
- O mesmo commit era o HEAD da branch main quando esta base foi preparada.

A configuração CMake dessa revisão detecta ANDROID e desativa analyzer/recompiler/testes/studio para o alvo móvel, mantendo o runtime. Isso não representa suporte completo ao jogo: o ponto de entrada Android ainda é um TODO e o renderer GS da aplicação PC depende de um contexto OpenGL criado em Windows.

## Referências Android ARM64 e GS

- [Tenkaichi3DecompAndroid](https://github.com/dfdx047/Tenkaichi3DecompAndroid): referência arquitetural para launcher Android, biblioteca de jogo ARM64 e separação entre runtime recompilado e Graphics Synthesizer. A documentação BT4 descreve o envio de eventos/transferências do GS por ring buffer e uma opção de renderer ARMSX2 em modo live. O projeto declara AGPL-3.0; nenhum código foi copiado para este repositório.
- [AetherSX2](https://github.com/Trixarian/NetherSX2-patch): referência histórica para emulação PS2 em Android. Os espelhos públicos consultados não disponibilizam a implementação proprietária completa do renderer; não é uma base para copiar o renderer integral.
- [ARMSX2](https://github.com/ARMSX2/ARMSX2): referência aberta baseada em PCSX2 e orientada a ARM64, com caminhos gráficos Vulkan/OpenGL. A licença declarada é GPL-3.0; qualquer incorporação ou ligação direta exigiria revisão de licença e validação da compatibilidade da interface GS. Não é dependência deste projeto.

Esses projetos são referências de arquitetura e comportamento, não componentes integrados. A implementação gráfica continuará desacoplada até que os eventos GIF/VIF/DMA, registradores GS e sincronização do PS2Recomp tenham um contrato validado.

## Regra de atualização

Não mover o submódulo automaticamente para a branch mais recente. Ao atualizar, registrar SHA novo, revisar mudanças em CMake, runtime, IOP, GS, VU e ferramentas, e só então ajustar os patches Android. Nunca atualizar a versão fixada silenciosamente.
