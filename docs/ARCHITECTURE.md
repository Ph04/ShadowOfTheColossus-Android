# Arquitetura Android

## Separação de responsabilidades

- android/app: Activity, interface mínima e integração com o Storage Access Framework.
- JNI: ponte entre o ciclo de vida Android e os componentes C++.
- IsoSource: leitura aleatória da imagem por descritor de arquivo, sem cópia integral.
- VulkanRenderer: superfície Android, seleção de GPU, swapchain e apresentação de teste.
- external/PS2Recomp: biblioteca de recompilação estática, mantida como submódulo e versão fixada; compilada para ARM64 e chamada diretamente pelo JNI durante a preparação.

## Fluxo inicial

A Activity abre o seletor de documentos do Android. O URI recebe uma permissão persistente quando o provedor oferece essa possibilidade. Em seguida a aplicação abre um AssetFileDescriptor, transfere a propriedade do descritor ao código nativo e conserva o URI para reabertura no próximo início. O usuário não precisa copiar a ISO para uma pasta específica.

IsoSource usa pread para leituras posicionais, permitindo ao pipeline buscar setores sem depender de um caminho de arquivo exposto pelo provedor. O leitor ISO9660 e o inspetor nativo validam SYSTEM.CNF, o executável de boot, STARTUP.XFF e os hashes dos três módulos. O pipeline extrai somente os arquivos necessários para getFilesDir(), mantendo a imagem original no provedor escolhido.

A SurfaceView entrega a superfície Android ao VulkanRenderer. O renderer seleciona dispositivo e fila de apresentação, cria swapchain e apresenta um frame de limpeza continuamente. Isso comprova somente o caminho de apresentação Vulkan, não a renderização do Graphics Synthesizer do PS2.

O pipeline nativo roda em uma thread de trabalho, com progresso protegido por mutex. Os estágios são validação, extração, parsing/ligação XFF2, geração ELF, inicialização PS2Recomp, recompilação de funções e gravação dos arquivos C++. O relatório de progresso do PS2Recomp recebeu uma pequena extensão local, aplicada durante o CMake, para permitir snapshots seguros dos contadores de funções sem alterar o pin do submódulo.

## Fronteira com o upstream

O port PC contém código específico de Windows e um backend GS OpenGL 4.6. O aplicativo compila apenas a biblioteca de recompilação estática do PS2Recomp para ARM64 e chama a classe PS2Recompiler no próprio processo. O executável CLI e o runtime PS2 completo não são embutidos nesta etapa. O runtime Android e o GS ainda precisam de adaptação específica.

Não modificar o código upstream diretamente sem registrar as alterações como patch revisável. A implementação Android deve ficar em módulos próprios sempre que possível.
