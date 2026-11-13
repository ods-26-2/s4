#!/usr/bin/env bash
#
# S4 — publica a imagem de PRODUCAO num registry (Docker Hub por padrao),
# multi-arch, para que a Jetson (arm64) e uma VM na nuvem (amd64) apenas facam
# `docker pull` em vez de compilar na borda.
#
# O que ele faz: cria/usa um builder buildx (driver docker-container, necessario
# para multi-arch) e roda `docker buildx build --target production`. O gate de
# testes continua valendo (RUN_TESTS=ON por padrao) — ver o aviso sobre
# emulacao abaixo.
#
# Uso:
#   scripts/publish_image.sh                       # amd64+arm64, tag 0.1.0, --push
#   IMAGE=seuuser/s4-media-evidence scripts/publish_image.sh
#   PLATFORMS=linux/arm64 scripts/publish_image.sh  # so a Jetson
#   RUN_TESTS=OFF scripts/publish_image.sh          # sem ctest (so o binario)
#   PUSH=0 PLATFORMS=linux/amd64 scripts/publish_image.sh   # build local (--load)
#   CACHE=1 scripts/publish_image.sh                # cache de build no registry
#   DRY_RUN=1 scripts/publish_image.sh              # so mostra o comando
#
# Requer login previo no registry:  docker login
#
# Variaveis: IMAGE TAG PLATFORMS RUN_TESTS PUSH BUILDER CACHE DRY_RUN

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

IMAGE="${IMAGE:-ods/s4-media-evidence}"
TAG="${TAG:-0.1.0}"
PLATFORMS="${PLATFORMS:-linux/amd64,linux/arm64}"
RUN_TESTS="${RUN_TESTS:-ON}"
PUSH="${PUSH:-1}"
BUILDER="${BUILDER:-s4-builder}"
CACHE="${CACHE:-0}"

# Builder docker-container: o driver "docker" padrao nao faz cache de registry e
# tem limitacoes com multiplas plataformas. Nao usamos --use para nao trocar o
# builder padrao do usuario; o alvo e passado com --builder.
if ! docker buildx inspect "${BUILDER}" >/dev/null 2>&1; then
    echo "==> criando builder '${BUILDER}' (driver docker-container)"
    docker buildx create --name "${BUILDER}" --driver docker-container >/dev/null
fi
docker buildx inspect --bootstrap "${BUILDER}" >/dev/null

# Um unico `docker load` nao comporta varias arquiteturas.
if [[ "${PLATFORMS}" == *,* && "${PUSH}" != "1" ]]; then
    echo "ERRO: build multi-arch so faz sentido com --push (nao da para --load varias arquiteturas)." >&2
    echo "      Para build local: PLATFORMS=linux/amd64 PUSH=0 scripts/publish_image.sh" >&2
    exit 2
fi

# ctest sob emulacao (arquitetura estrangeira) e lento e, com GStreamer, nem
# sempre confiavel. O caminho recomendado e validar nativo e publicar sem testes.
if [[ "${RUN_TESTS}" = "ON" && "${PLATFORMS}" == *,* ]]; then
    echo "AVISO: RUN_TESTS=ON com multiplas plataformas roda o ctest sob emulacao."
    echo "       Recomendado: validar nativo (docker build --target production .) e"
    echo "       publicar com RUN_TESTS=OFF."
fi

ARGS=(
    buildx build
    --builder "${BUILDER}"
    --platform "${PLATFORMS}"
    --target production
    --build-arg "RUN_TESTS=${RUN_TESTS}"
    # Sem attestations: mantem um indice de manifesto limpo, que qualquer Docker
    # (inclusive o da Jetson) entende sem surpresa.
    --provenance=false
    -f "${PROJECT_DIR}/Dockerfile"
    -t "${IMAGE}:${TAG}"
)
[[ "${TAG}" != "latest" ]] && ARGS+=(-t "${IMAGE}:latest")

if [[ "${CACHE}" = "1" ]]; then
    ARGS+=(--cache-from "type=registry,ref=${IMAGE}:buildcache")
    ARGS+=(--cache-to "type=registry,ref=${IMAGE}:buildcache,mode=max")
fi

if [[ "${PUSH}" = "1" ]]; then
    ARGS+=(--push)
else
    ARGS+=(--load)
fi

echo "==> docker ${ARGS[*]} ${PROJECT_DIR}"
if [[ "${DRY_RUN:-0}" = "1" ]]; then
    echo "(DRY_RUN=1: nada foi executado)"
    exit 0
fi
docker "${ARGS[@]}" "${PROJECT_DIR}"

echo
echo "Publicado: ${IMAGE}:${TAG} (${PLATFORMS})"
echo "Deploy na Jetson/VM:"
echo "  S4_IMAGE=${IMAGE}:${TAG} docker compose -f docker-compose.prod.yml up -d"
