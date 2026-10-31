# Dependências externas

PS2Recomp está configurado como submódulo Git em external/PS2Recomp e deve permanecer fixado na revisão registrada em docs/UPSTREAM.md.

Inicialização:

    git submodule update --init --recursive

Não copiar ISO, BIOS, assets comerciais ou arquivos C++ gerados do jogo para esta pasta ou para o repositório. Alterações necessárias no código PS2Recomp devem ser mantidas como patches documentados ou propostas upstream, em vez de mudanças locais não rastreadas no submódulo.
