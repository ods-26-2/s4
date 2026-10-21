# ===============================================================
# S4 Media Evidence — multi-stage build (amd64 e arm64)
# ===============================================================

# ---------- Estágio 1: builder (toolchain + testes) ----------
FROM debian:bookworm-slim AS builder

ENV DEBIAN_FRONTEND=noninteractive

# gstreamer1.0-dev traz os headers do core; os plugins base (coreelements,
# appsink/appsrc) vem de gstreamer1.0-plugins-base. O replay de .mp4 depende
# de qtdemux e mp4mux (gstreamer1.0-plugins-good) E de h264parse
# (gstreamer1.0-plugins-bad).
#
# A separacao entre "bad" e "good" e o que faz este Dockerfile parecer correto
# e nao funcionar: h264parse mora em libgstvideoparsersbad.so, que fica no
# conjunto "bad", e nao em "base" nem em "good". Sem o pacote -bad o configure
# acha o GStreamer (headers e .pc estao la), o build passa, e so na hora de
# montar o pipeline aparece: no element "h264parse".
#
# libgstreamer-plugins-base1.0-dev tambem nao e redundante com gstreamer1.0-dev:
# e ele que instala o gstreamer-app-1.0.pc, o .pc de que o CMake usa para achar
# gstappsrc/gstappsink. Sem esse pacote o configure passa, o build passa e o
# ctest passa -- com o caminho de GStreamer silenciosamente desligado, porque o
# unico teste de MP4 real e compilado sob #ifdef. Foi assim que uma imagem
# inteira saiu daqui sem conseguir reproduzir um unico quadro.
#
# E o mesmo raciocinio vale para os dois ultimos pacotes, por um motivo
# diferente: x264enc (plugins-ugly) e avdec_h264 (libav) sao o que o
# test_s4_gstreamer_capture usa para codificar e decodificar um trecho de
# verdade. Sem eles o teste sai com 77 e o CTest o reporta como Skipped — honesto,
# mas a imagem para de provar que o material de video passa pelo encoder e pelo
# decoder. E avdec_h264 e o mesmo elemento que o smoke procura para verificar
# pixels; sem ele esse ramo do smoke tambem vira "pula".
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ \
        cmake \
        make \
        pkg-config \
        libsqlite3-dev \
        libssl-dev \
        uuid-dev \
        gstreamer1.0-dev \
        libgstreamer-plugins-base1.0-dev \
        gstreamer1.0-plugins-base \
        gstreamer1.0-plugins-good \
        gstreamer1.0-plugins-bad \
        gstreamer1.0-plugins-ugly \
        gstreamer1.0-libav \
        gstreamer1.0-tools \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tests ./tests
COPY tools ./tools
COPY third_party ./third_party
# Cenarios gravados e video de teste: o ctest le os dois a partir da raiz do
# projeto, entao precisam existir no contexto do builder.
COPY scenarios ./scenarios
COPY video_test ./video_test

# Release build; falha de teste aqui impede a imagem final.
#
# ODS_S4_REQUIRE_SAMPLE_VIDEO=ON porque aqui o video de teste ESTA no contexto
# (a linha COPY video_test acima): se ele sumisse, o cenario de replay deixaria
# de rodar e sairia com 77 — "Skipped", que o build aceitaria. Com a option, a
# ausencia do material reprova. Numa imagem que tem o video,(video ausente) e
# uma falha de build, nao uma opcao.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DODS_S4_REQUIRE_SAMPLE_VIDEO=ON \
 && cmake --build build -j"$(nproc)" \
 && ctest --test-dir build --output-on-failure

# ---------- Estágio 2: runtime (somente libs e binário) ------
FROM debian:bookworm-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# O runtime precisa dos mesmos plugins que o builder. O h264parse (plugins
# -bad) e obrigatorio porque e ele que converte o sample em access unit
# alinhada, que e o formato que o ring buffer assume.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsqlite3-0 \
        libssl3 \
        libuuid1 \
        libstdc++6 \
        curl \
        gstreamer1.0 \
        gstreamer1.0-plugins-base \
        gstreamer1.0-plugins-good \
        gstreamer1.0-plugins-bad \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 1000 s4 \
    && mkdir -p /data/media \
    && chown -R s4:s4 /data

WORKDIR /app

COPY --from=builder /src/build/s4_media_evidence /app/s4_media_evidence
COPY --from=builder /src/video_test /app/video_test
COPY --from=builder /src/scenarios /app/scenarios

USER s4

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD curl -fsS http://127.0.0.1:8080/healthz || exit 1

VOLUME ["/data"]

CMD ["/app/s4_media_evidence", "--port", "8080", "--db", "/data/s4.db", "--media-dir", "/data/media"]

# ---------- Estágio 3: smoke (validação, não vai para a imagem final) ----
# Existe para provar que a imagem de runtime realmente faz o que o Dockerfile
# promete: replay de MP4 real, extracao de container decodificavel e hash
# repetivel.
#
# As ferramentas de teste (jq, gstreamer1.0-tools) ficam AQUI e nao no runtime.
# Encher a imagem de produção com ferramentas de verificação seria trocar
# superfície de ataque por conveniência de quem testa.
#
# Uso: docker build --target smoke -t s4-media-evidence:smoke .
FROM runtime AS smoke

USER root

# libav entra pelo mesmo motivo do builder: e o que da ao smoke um decoder
# H.264 para conferir pixels de verdade. Sem ele o script segue funcionando e
# imprime "pula sem decoder" — honesto, porem fraco: o ramo de pixels do smoke
# so vira prova se existe decoder, e nao ha decoder em debian-slim sem este
# pacote.
RUN apt-get update && apt-get install -y --no-install-recommends \
        jq \
        gstreamer1.0-tools \
        gstreamer1.0-libav \
    && rm -rf /var/lib/apt/lists/*

COPY scripts/smoke_e2e.sh /app/scripts/smoke_e2e.sh
RUN chmod +x /app/scripts/smoke_e2e.sh

USER s4

# O script sobe dois daemons, sobe o replay, extrai um clipe, checa a API e
# compara o hash entre as duas execucoes. Qualquer falha quebra o build.
RUN /app/scripts/smoke_e2e.sh /app/s4_media_evidence

# ---------- Estágio 4: imagem de produção (alvo padrão do build) -----------
# O `docker build` sem --target usa o ÚLTIMO estágio do Dockerfile. Como o
# smoke vem depois do runtime, sem este estágio final a tag
# ods/s4-media-evidence:0.1.0 apontaria para a imagem de VALIDAÇÃO: 1,6 GB,
# com jq, gstreamer1.0-tools e avdec_h264 dentro, enquanto o README e o
# docker-compose descrevem a tag como a imagem de produção enxuta. O estágio
# smoke continua existindo e continua acessível com --target smoke; o que muda
# é só qual imagem nasce quando ninguém pede nada.
FROM runtime AS production
