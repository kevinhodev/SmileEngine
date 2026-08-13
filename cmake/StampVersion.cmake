# Carimba o VersionInfo.h com o commit ATUAL, em tempo de BUILD.
#
# Por que nao no configure: o commit vai no manifesto de cada captura deterministica
# (Docs/CAPTURE-PROTOCOL.md), e um campo cuja unica funcao e ser confiavel nao pode envelhecer.
# Gerado no configure, bastaria commitar e rebuildar sem reconfigurar para todos os manifestos
# seguintes apontarem para o commit anterior — e o erro seria invisivel exatamente quando importa,
# que e ao comparar uma captura de hoje com uma de tres semanas atras.
#
# Chamado via `cmake -P` por um alvo custom, com INPUT/OUTPUT/SOURCE_DIR e as macros de versao.
# O configure_file so reescreve quando o conteudo muda, entao um build sem commit novo nao
# recompila nada.

find_package(Git QUIET)

set(SMILE_BUILD_COMMIT "desconhecido")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${SOURCE_DIR}
        OUTPUT_VARIABLE GIT_HEAD
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_RESULT
    )
    if(GIT_RESULT EQUAL 0 AND GIT_HEAD)
        set(SMILE_BUILD_COMMIT "${GIT_HEAD}")
        # Arvore suja = a build NAO e reproduzivel a partir do commit. O sufixo existe para uma
        # captura tirada no meio de uma edicao nao se passar por uma tirada do commit limpo.
        #
        # Arquivo NAO RASTREADO tambem conta, e essa e a diferenca que importa aqui. A tentacao e
        # ignora-lo ("nao esta no commit, logo nao afeta a build"), mas afeta: um .hlsli novo
        # incluido por um shader existente, ou um header novo, entram na compilacao sem produzir
        # uma linha de diff rastreada. Marcar limpo nesse estado poria um commit em que a imagem
        # nao se reproduz dentro de um manifesto cuja unica funcao e ser confiavel. O preco e um
        # `-dirty` a mais quando ha rascunho na arvore, e esse e o lado certo de errar.
        execute_process(
            COMMAND ${GIT_EXECUTABLE} status --porcelain
            WORKING_DIRECTORY ${SOURCE_DIR}
            OUTPUT_VARIABLE GIT_DIRTY
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(NOT GIT_DIRTY STREQUAL "")
            set(SMILE_BUILD_COMMIT "${SMILE_BUILD_COMMIT}-dirty")
        endif()
    endif()
endif()

configure_file(${INPUT} ${OUTPUT} @ONLY)
