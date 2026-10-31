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

## Regra de atualização

Não mover o submódulo automaticamente para a branch mais recente. Ao atualizar, registrar SHA novo, revisar mudanças em CMake, runtime, IOP, GS, VU e ferramentas, e só então ajustar os patches Android. Nunca atualizar a versão fixada silenciosamente.
