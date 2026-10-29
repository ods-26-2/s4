cmake_minimum_required(VERSION 3.20)
project(s4_media_evidence
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Include directories
include_directories(include third_party/httplib)

# Third-party dependencies
find_package(PkgConfig QUIET)
find_package(SQLite3 REQUIRED)
find_package(OpenSSL QUIET) # For SHA256
find_package(Threads REQUIRED)

find_library(UUID_LIBRARY NAMES uuid REQUIRED)

# O nome do alvo importado do SQLite difere entre versões do CMake
# (SQLite::SQLite3 nas mais antigas, SQLite3::SQLite3 nas mais novas).
if(TARGET SQLite3::SQLite3)
    set(SQLITE_TARGET SQLite3::SQLite3)
else()
    set(SQLITE_TARGET SQLite::SQLite3)
endif()

# Core Library Sources
file(GLOB_RECURSE S4_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
)
list(REMOVE_ITEM S4_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

add_library(s4_media_evidence_lib
    ${S4_SOURCES}
)

target_include_directories(s4_media_evidence_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(s4_media_evidence_lib PRIVATE
    ${SQLITE_TARGET}
    ${UUID_LIBRARY}
    Threads::Threads
)

if(OpenSSL_FOUND)
    target_link_libraries(s4_media_evidence_lib PRIVATE OpenSSL::Crypto)
endif()

# --- S4.1 Ring Buffer -------------------------------------------------------
# shm_open/shm_unlink vivem em librt nas glibc antigas (JetPack incluso).
if(UNIX AND NOT APPLE)
    target_link_libraries(s4_media_evidence_lib PUBLIC rt)
endif()

# Captura ao vivo e replay de video sao o coracao do S4.1/S4.4: sem GStreamer
# nao existe caminho que produza um MP4 a partir de video real.
#
# O padrao e exigir o GStreamer. Antes o bloco abaixo degradava em silencio
# (message(STATUS)) quando o .pc nao aparecia, e o efeito pratico foi uma imagem
# Docker inteira construida, testada e publicada sem conseguir reproduzir um
# unico quadro -- com o ctest reportando "all tests passed", porque o unico
# teste de MP4 real esta sob #ifdef. Um aviso de uma linha num log de build
# longo nao e protecao.
#
# Quem realmente quiser compilar sem GStreamer (build de outra plataforma, ou
# desenvolvimento das partes que nao usam video) pede explicitamente com
# -DODS_S4_WITH_GSTREAMER=OFF e recebe o aviso.
option(ODS_S4_WITH_GSTREAMER "Compila o adaptador de captura GStreamer (S4.1)" ON)
option(ODS_S4_REQUIRE_GSTREAMER "Falha a configuracao se o GStreamer nao for encontrado" ON)

# O cenario de MP4 real depende de um arquivo em video_test/, que e material
# binario de 17 MB versionado no repositorio. Num clone onde ele nao esteja, o
# ctest reporta "Skipped" -- nunca "Passed". Quem quiser que a ausencia do video
# reprove o build (a imagem Docker, que tem o video, e o caso que importa) pede
# com -DODS_S4_REQUIRE_SAMPLE_VIDEO=ON.
option(ODS_S4_REQUIRE_SAMPLE_VIDEO
    "Reprova o cenario de replay quando video_test/ nao tem MP4" OFF)

set(ODS_S4_GSTREAMER_ENABLED OFF)
if(ODS_S4_WITH_GSTREAMER)
    # PkgConfig::GST carrega includes, diretorios de biblioteca e flags de link.
    # Usar apenas ${GST_LIBRARIES} (que sao so nomes) perde o -L e so funciona
    # por acaso quando o GStreamer esta em /usr/lib64.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GST QUIET gstreamer-1.0 gstreamer-app-1.0 IMPORTED_TARGET)
    if(GST_FOUND)
        set(ODS_S4_GSTREAMER_ENABLED ON)
        message(STATUS "GStreamer habilitado: captura e replay do S4.1")
    else()
        set(ODS_S4_GSTREAMER_MISSING_REASON
            "gstreamer-1.0/gstreamer-app-1.0 nao encontrados por pkg-config")
    endif()
endif()

if(ODS_S4_WITH_GSTREAMER AND NOT ODS_S4_GSTREAMER_ENABLED)
    # gstreamer-app-1.0.pc vem de libgstreamer-plugins-base1.0-dev, e nao de
    # gstreamer1.0-dev: e um descuido facil de cometer e o sintoma e sempre o
    # mesmo, "o build passa mas nao tem GStreamer".
    set(ODS_S4_GSTREAMER_HINT
        "Debian/Ubuntu: apt-get install gstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav
         Fedora/RHEL:   dnf install gstreamer1-devel gstreamer1-plugins-base-devel gstreamer1.0-plugins-base gstreamer1-plugins-good gstreamer1-plugins-bad gstreamer1-plugins-ugly gstreamer1-libav
         (plugins-good traz qtdemux/mp4mux; plugins-bad traz o h264parse; plugins-ugly traz o x264enc; libav traz o avdec_h264.)
         Se o .pc existe em outro prefixo, aponte PKG_CONFIG_PATH para ele.")
    if(ODS_S4_REQUIRE_GSTREAMER)
        message(FATAL_ERROR
            "ODS_S4_WITH_GSTREAMER=ON mas ${ODS_S4_GSTREAMER_MISSING_REASON}.\n"
            "O S4 precisa do GStreamer para reproduzir video e extrair MP4.\n"
            "${ODS_S4_GSTREAMER_HINT}\n"
            "Para compilar sem GStreamer de proposito: -DODS_S4_WITH_GSTREAMER=OFF")
    else()
        message(WARNING
            "GStreamer ausente (${ODS_S4_GSTREAMER_MISSING_REASON}). "
            "O S4 sera compilado SEM captura nem replay de video, e o cenario de "
            "MP4 real nao sera executado -- os testes passarão sem exercitar essa "
            "parte. ${ODS_S4_GSTREAMER_HINT}")
    endif()
endif()

if(ODS_S4_GSTREAMER_ENABLED)
    target_compile_definitions(s4_media_evidence_lib PUBLIC ODS_S4_WITH_GSTREAMER)
    target_link_libraries(s4_media_evidence_lib PUBLIC PkgConfig::GST)
endif()

# Main executable
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
    add_executable(s4_media_evidence src/main.cpp)
    target_link_libraries(s4_media_evidence PRIVATE s4_media_evidence_lib)
endif()

# Tools executables
add_executable(s4_record_scenario tools/record_scenario.cpp)
target_link_libraries(s4_record_scenario PRIVATE s4_media_evidence_lib)

# S4.1 item 1: CPU do x264enc e RAM do buffer, medidos na placa.
add_executable(s4_measure_encoder tools/measure_encoder.cpp)
target_link_libraries(s4_measure_encoder PRIVATE s4_media_evidence_lib)

# Testing
enable_testing()

function(s4_add_test name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${name} PRIVATE s4_media_evidence_lib)
    add_test(NAME ${name} COMMAND ${name})
    # O CTest roda a partir do diretorio de build, mas os testes leem cenarios
    # gravados e o video de teste por caminho relativo a raiz do projeto.
    set_tests_properties(${name} PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endfunction()

s4_add_test(test_s4_unit tests/unit/test_domain_and_usecase.cpp)
s4_add_test(test_s4_sha256 tests/unit/test_sha256_hasher.cpp)
s4_add_test(test_s4_sqlite_repository tests/integration/test_sqlite_repository.cpp)
s4_add_test(test_s4_extract_clip tests/integration/test_extract_clip.cpp)
s4_add_test(test_s4_http_server tests/integration/test_http_server.cpp)
s4_add_test(test_s4_http_post_event tests/integration/test_http_post_event.cpp)
s4_add_test(test_s4_ring_buffer tests/unit/test_ring_buffer.cpp)
s4_add_test(test_s4_ring_buffer_runtime tests/integration/test_ring_buffer_runtime.cpp)
s4_add_test(test_s4_iso_bmff_timestamps tests/unit/test_iso_bmff_timestamps.cpp)

# Replay de MP4 real: sem video em video_test/ o teste sai com 77 e o CTest o
# reporta como "Skipped", nunca como aprovado. Com ODS_S4_REQUIRE_SAMPLE_VIDEO
# a ausencia do video vira falha dura -- e o que a imagem Docker usa, porque
# ela copia o video para dentro do build.
s4_add_test(test_s4_replay_e2e tests/integration/test_replay_e2e.cpp)
set_tests_properties(test_s4_replay_e2e PROPERTIES SKIP_RETURN_CODE 77)
if(ODS_S4_REQUIRE_SAMPLE_VIDEO)
    target_compile_definitions(test_s4_replay_e2e PRIVATE ODS_S4_REQUIRE_SAMPLE_VIDEO)
endif()

# Captura real via GStreamer; sem a biblioteca ou sem x264enc/avdec_h264 o
# teste sai com 77 e o CTest o reporta como "Skipped", nao como aprovado.
s4_add_test(test_s4_gstreamer_capture tests/integration/test_gstreamer_capture.cpp)
set_tests_properties(test_s4_gstreamer_capture PROPERTIES SKIP_RETURN_CODE 77)

s4_add_test(test_s4_retention tests/unit/test_retention.cpp)
s4_add_test(test_s4_retention_runtime tests/integration/test_retention_runtime.cpp)

# S4.1 na arquitetura 6c: quadros crus de P4 + x264enc, captured_at e sessao
# oficiais. Sem GStreamer ou sem x264enc/avdec_h264/shm sai com 77 (Skipped).
s4_add_test(test_s4_p4_input tests/integration/test_p4_input.cpp)
set_tests_properties(test_s4_p4_input PROPERTIES SKIP_RETURN_CODE 77)

# OUT_OF_BUFFER tipado no POST /api/v1/events (caminho por relogio de captura).
s4_add_test(test_s4_out_of_buffer_http tests/integration/test_out_of_buffer_http.cpp)
# Kafka/B3 pub-sub support
option(ODS_S4_WITH_KAFKA "Enable Kafka/B3 event bus integration" ON)
option(ODS_S4_REQUIRE_KAFKA "Fail if Kafka not found when enabled" OFF)

set(ODS_S4_KAFKA_ENABLED OFF)
if(ODS_S4_WITH_KAFKA)
    pkg_check_modules(RDKAFKA QUIET rdkafka++ IMPORTED_TARGET)
    if(RDKAFKA_FOUND)
        set(ODS_S4_KAFKA_ENABLED ON)
        message(STATUS "Kafka/librdkafka enabled for B3 event bus")
        target_compile_definitions(s4_media_evidence_lib PUBLIC ODS_S4_WITH_KAFKA)
        target_link_libraries(s4_media_evidence_lib PUBLIC PkgConfig::RDKAFKA)
    else()
        if(ODS_S4_REQUIRE_KAFKA)
            message(FATAL_ERROR "ODS_S4_WITH_KAFKA=ON but librdkafka not found via pkg-config")
        else()
            message(WARNING "librdkafka not found; Kafka integration disabled")
        endif()
    endif()
endif()
# Ensure nlohmann/json include is available
include_directories(third_party)
