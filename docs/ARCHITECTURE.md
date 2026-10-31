# Arquitetura Android

## Separação de responsabilidades

- android/app: Activity, interface mínima e integração com o Storage Access Framework.
- JNI: ponte entre o ciclo de vida Android e os componentes C++.
- IsoSource: leitura aleatória da imagem por descritor de arquivo, sem cópia integral.
- VulkanRenderer: superfície Android, seleção de GPU, swapchain e apresentação de teste.
- external/PS2Recomp: runtime e ferramentas de recompilação, mantidos como submódulo e versão fixada.

## Fluxo inicial

A Activity abre o seletor de documentos do Android. O URI recebe uma permissão persistente quando o provedor oferece essa possibilidade. Em seguida a aplicação abre um AssetFileDescriptor, transfere a propriedade do descritor ao código nativo e conserva o URI para reabertura no próximo início.

IsoSource usa pread para leituras posicionais, permitindo ao runtime buscar setores sem depender de um caminho de arquivo exposto pelo provedor. O leitor atual valida tamanho e capacidade de leitura; a identificação exata do disco e a validação dos módulos serão etapas posteriores.

A SurfaceView entrega a superfície Android ao VulkanRenderer. O renderer seleciona dispositivo e fila de apresentação, cria swapchain e apresenta um frame de limpeza continuamente. Isso comprova somente o caminho de apresentação Vulkan, não a renderização do Graphics Synthesizer do PS2.

## Fronteira com o upstream

O port PC contém código específico de Windows e um backend GS OpenGL 4.6. A configuração CMake do PS2Recomp reconhece Android e compila o runtime, mas o arquivo ps2_android_runtime.cpp nessa revisão é apenas um TODO. Por isso o runtime está versionado como submódulo, porém sua ligação ao aplicativo está atrás da opção SOTC_LINK_PS2_RUNTIME até que a camada de plataforma seja adaptada.

Não modificar o código upstream diretamente sem registrar as alterações como patch revisável. A implementação Android deve ficar em módulos próprios sempre que possível.
