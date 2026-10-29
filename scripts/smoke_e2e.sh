#!/usr/bin/env bash
#
# S4 — smoke test end-to-end do daemon com replay de MP4 real.
#
# O que este script comprova, em ordem:
#   1. o daemon sobe com --source e --scenario;
#   2. o ring buffer enche com quadros do video real;
#   3. o cenario extrai um container MP4 bem formado e legivel;
#   4. a API HTTP responde (buffer/stats, POST /events, GET /clips/{id});
#   5. o sha256 publicado bate com o arquivo em disco;
#   6. duas execucoes do mesmo video produzem o mesmo sha256.
#
# O ultimo item e o que diferencia "funciona" de "e evidencia": um hash que
# muda a cada extracao do mesmo footage nao identifica nada.
#
# Ferramentas: curl, jq e sha256sum sao OBRIGATORIAS — sem elas o script nao
# consegue conferir o hash, e um smoke que pula a verificacao central sem dizer
# que pulou seria pior do que nao rodar. gst-launch-1.0 e um decoder H.264
# (vaapih264dec/avdec_h264) sao OPCIONAIS: sem eles o script diz "pula" e segue,
# deixando claro no log que container e pixels sao coisas diferentes.
#
# Uso: scripts/smoke_e2e.sh [caminho/para/o/binario] [caminho/para/o/mp4]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BINARY="${1:-${S4_BINARY:-}}"
SOURCE="${2:-${S4_SOURCE:-}}"
if [[ -z "${SOURCE}" ]]; then
    # O nome do arquivo de teste tem espacos: sem aspas, o globbing quebra.
    for candidate in "${PROJECT_DIR}"/video_test/*.mp4; do
        [[ -f "${candidate}" ]] && SOURCE="${candidate}" && break
    done
fi

if [[ -z "${BINARY}" ]]; then
    echo "ERRO: informe o binario do daemon como primeiro argumento." >&2
    echo "      (ou exporte S4_BINARY)" >&2
    exit 2
fi
if [[ ! -x "${BINARY}" ]]; then
    echo "ERRO: binario inexistente ou sem permissao de execucao: ${BINARY}" >&2
    exit 2
fi
if [[ ! -f "${SOURCE}" ]]; then
    echo "ERRO: nenhum MP4 de teste encontrado. Passe o caminho como segundo argumento." >&2
    exit 2
fi

# Pre-flight das ferramentas obrigatorias, com o motivo de cada uma. Sem este
# check o script morre no meio, sob "set -e", em um "jq: command not found" que
# parece falha do S4 e nao e: e a maquina sem ferramenta. E o pior desfecho
# seria o script aceitar a ausencia e sair com SMOKE OK sem ter conferido hash
# de nada — que e exatamente o buraco que este smoke existe para fechar.
MISSING=()
for tool in curl jq sha256sum; do
    command -v "${tool}" >/dev/null 2>&1 || MISSING+=("${tool}")
done
if [[ ${#MISSING[@]} -gt 0 ]]; then
    echo "ERRO: ferramenta(s) obrigatoria(s) ausente(s): ${MISSING[*]}" >&2
    echo "      Fedora: sudo dnf install -y curl jq coreutils" >&2
    echo "      Debian: sudo apt-get install -y curl jq coreutils" >&2
    exit 2
fi

WORK_DIR="$(mktemp -d /tmp/s4_smoke_XXXXXX)"
DAEMON_PID=""
FAILURES=0

cleanup() {
    if [[ -n "${DAEMON_PID}" ]] && kill -0 "${DAEMON_PID}" 2>/dev/null; then
        kill "${DAEMON_PID}" 2>/dev/null || true
        wait "${DAEMON_PID}" 2>/dev/null || true
    fi
    if [[ "${S4_KEEP_SMOKE_ARTIFACTS:-0}" == "1" ]]; then
        echo "artefatos preservados em ${WORK_DIR}"
    else
        rm -rf "${WORK_DIR}"
    fi
}
trap cleanup EXIT

pass() { printf '  ok    %s\n' "$1"; }
fail() { printf '  FALHA %s\n' "$1"; FAILURES=$((FAILURES + 1)); }
step() { printf '\n== %s\n' "$1"; }

# sha256 do container no disco.
file_sha256() { sha256sum "$1" | cut -d' ' -f1; }

# ---------------------------------------------------------------------------
step "1. daemon sobe com replay de arquivo real"

LOG="${WORK_DIR}/daemon.log"
"${BINARY}" \
    --source "${SOURCE}" \
    --scenario \
    --port 0 \
    --db "${WORK_DIR}/evidence.db" \
    --media-dir "${WORK_DIR}/media" >"${LOG}" 2>&1 &
DAEMON_PID=$!

# A porta so e conhecida apos o bind, entao ela e lida do log em vez de fixada.
PORT=""
for _ in $(seq 1 100); do
    if [[ -s "${LOG}" ]]; then
        PORT="$(grep -oE 'listening on http://127\.0\.0\.1:[0-9]+' "${LOG}" | head -1 | grep -oE '[0-9]+$' || true)"
        [[ -n "${PORT}" ]] && break
    fi
    kill -0 "${DAEMON_PID}" 2>/dev/null || break
    sleep 0.2
done
if [[ -z "${PORT}" ]]; then
    fail "daemon nao informou a porta"
    sed -n '1,40p' "${LOG}" >&2
    exit 1
fi
BASE="http://127.0.0.1:${PORT}"
pass "daemon ouvindo em ${BASE}"

# O replay de um arquivo e rapido, mas o cenario precisa que o buffer encha.
CLIP_PATH="${WORK_DIR}/media/event-demo.mp4"
for _ in $(seq 1 150); do
    [[ -s "${CLIP_PATH}" ]] && break
    kill -0 "${DAEMON_PID}" 2>/dev/null || break
    sleep 0.2
done
if [[ ! -s "${CLIP_PATH}" ]]; then
    fail "o cenario nao produziu ${CLIP_PATH}"
    sed -n '1,40p' "${LOG}" >&2
    exit 1
fi
pass "cenario extraiu $(stat -c%s "${CLIP_PATH}") bytes"

SCENARIO_SHA="$(grep -oE 'sha256:  [0-9a-f]{64}' "${LOG}" | head -1 | awk '{print $2}')"
if [[ -z "${SCENARIO_SHA}" ]]; then
    fail "o cenario nao imprimiu o sha256"
else
    pass "sha256 publicado: ${SCENARIO_SHA:0:16}..."
fi

# ---------------------------------------------------------------------------
step "2. ring buffer alimentado pelo video real"

if grep -q "Replaying '" "${LOG}"; then
    pass "replay ativo"
else
    fail "o daemon nao assumiu o modo de replay"
fi

# ---------------------------------------------------------------------------
step "3. o MP4 do cenario e um container real e bem formado"

# So a assinatura ftyp nao prova nada: um container truncado tambem tem ftyp.
# O teste que vale e passar pelo demuxer e pelo parser de bitstream, que leem o
# moov inteiro e reparseiam cada amostra.
#
# ATENCAO ao que esta checagem prova: h264parse faz PARSE do bitstream, nao
# decodifica pixels. Ela descarta o erro classico de "Annex-B com nome de .mp4"
# porque o container tem ftyp/moov/mdat e as tabelas de amostra sao coerentes.
# Provar pixels exige um decoder, verificado a parte e so quando existe.
if command -v gst-launch-1.0 >/dev/null 2>&1; then
    if timeout 60 gst-launch-1.0 -q \
        filesrc location="${CLIP_PATH}" ! qtdemux ! h264parse ! fakesink >/dev/null 2>&1; then
        pass "qtdemux ! h264parse leu o container inteiro"
    else
        fail "o container nao passou por qtdemux ! h264parse"
    fi

    # Decoder e opcional: maquina sem VA-API e sem avdec (comum em host de
    # desenvolvimento) simplesmente nao tem como decodificar. Ausencia de
    # decoder e falha do ambiente, nao do artefato -- por isso nao reprova.
    DECODER=""
    for candidate in vaapih264dec avdec_h264; do
        if gst-inspect-1.0 "${candidate}" >/dev/null 2>&1; then
            DECODER="${candidate}"
            break
        fi
    done
    if [[ -n "${DECODER}" ]]; then
        if timeout 60 gst-launch-1.0 -q \
            filesrc location="${CLIP_PATH}" ! qtdemux ! h264parse \
            ! "${DECODER}" ! videoconvert ! fakesink >/dev/null 2>&1; then
            pass "pixels decodificados por ${DECODER}"
        else
            fail "o clipe nao decodificou em ${DECODER}: container OK, video nao"
        fi
    else
        echo "  pula  sem decoder H.264 neste host: pixels nao verificados"
    fi
else
    echo "  pula  gst-launch-1.0 ausente: container nao verificado"
fi

FIRST_BOX="$(head -c 8 "${CLIP_PATH}" | tail -c 4)"
if [[ "${FIRST_BOX}" == "ftyp" ]]; then
    pass "assinatura ftyp"
else
    fail "primeiro box do container e '${FIRST_BOX}', esperado 'ftyp'"
fi

# ---------------------------------------------------------------------------
step "4. API HTTP"

STATS_CODE="$(curl -s -o "${WORK_DIR}/stats.json" -w '%{http_code}' \
    "${BASE}/api/v1/buffer/stats")"
if [[ "${STATS_CODE}" == "200" ]]; then
    FRAMES="$(jq -r '.frames_stored // 0' "${WORK_DIR}/stats.json")"
    if [[ "${FRAMES}" -gt 0 ]]; then
        pass "GET /buffer/stats devolveu ${FRAMES} quadros guardados"
    else
        fail "GET /buffer/stats respondeu 200 mas sem quadros"
    fi
else
    fail "GET /buffer/stats respondeu ${STATS_CODE}"
fi

# O evento e ancorado num instante que o buffer realmente guarda, pelos
# mesmos motivos do cenario: apos o EOS de um arquivo nao existe futuro no
# relogio de captura.
EVENT_TS="$(jq -r '.newest_capture_ts_ns' "${WORK_DIR}/stats.json" 2>/dev/null || echo 0)"
if [[ "${EVENT_TS}" == "0" || -z "${EVENT_TS}" ]]; then
    fail "nao foi possivel ler newest_capture_ts_ns"
else
    EVENT_CODE="$(curl -s -o "${WORK_DIR}/event.json" -w '%{http_code}' \
        -X POST "${BASE}/api/v1/events" \
        -H 'Content-Type: application/json' \
        -d "{\"event_id\":\"evt-smoke-1\",\"capture_ts_ns\":${EVENT_TS},\"pre_window_seconds\":0.5,\"post_window_seconds\":0.0}")"
    if [[ "${EVENT_CODE}" == "201" ]]; then
        HTTP_CLIP_ID="$(jq -r '.clip_id // empty' "${WORK_DIR}/event.json")"
        HTTP_SHA="$(jq -r '.sha256_hash // empty' "${WORK_DIR}/event.json")"
        # O caminho vem da propria resposta. Deduzir o nome do arquivo aqui
        # seria repetir a regra de nomeacao do servidor numa segunda lugar, e
        # um nome errado no smoke seria um falso negativo, nao uma checagem.
        HTTP_CLIP_PATH="$(jq -r '.file_uri // empty' "${WORK_DIR}/event.json")"
        pass "POST /events respondeu 201 (clip ${HTTP_CLIP_ID})"
    else
        fail "POST /events respondeu ${EVENT_CODE}: $(head -c 200 "${WORK_DIR}/event.json")"
        HTTP_CLIP_ID=""
        HTTP_CLIP_PATH=""
    fi
fi

# ---------------------------------------------------------------------------
step "5. sha256 publicado bate com o arquivo em disco"

if [[ -n "${HTTP_CLIP_ID:-}" && -n "${HTTP_CLIP_PATH:-}" ]]; then
    DESCRIPTOR_CODE="$(curl -s -o "${WORK_DIR}/descriptor.json" -w '%{http_code}' \
        "${BASE}/api/v1/clips/${HTTP_CLIP_ID}")"
    if [[ "${DESCRIPTOR_CODE}" != "200" ]]; then
        fail "GET /clips/${HTTP_CLIP_ID} respondeu ${DESCRIPTOR_CODE}"
    elif [[ ! -f "${HTTP_CLIP_PATH}" ]]; then
        fail "o descriptor aponta para ${HTTP_CLIP_PATH}, que nao existe"
    else
        ON_DISK="$(file_sha256 "${HTTP_CLIP_PATH}")"
        if [[ "${ON_DISK}" == "${HTTP_SHA}" ]]; then
            pass "hash do descriptor confere com o arquivo"
        else
            fail "hash divergente: API ${HTTP_SHA} vs disco ${ON_DISK}"
        fi
    fi
else
    fail "sem clip da API para conferir o hash"
fi

# ---------------------------------------------------------------------------
step "6. a mesma extracao produz o mesmo hash"

# Reextrai o MESMO instante do MESMO video em uma segunda execucao do daemon.
# E o teste de repetibilidade: se o hash variar, a evidencia nao e
# identificavel e o hash nao cumpre a funcao.
SECOND_DIR="${WORK_DIR}/second"
SECOND_LOG="${WORK_DIR}/second.log"
"${BINARY}" \
    --source "${SOURCE}" \
    --scenario \
    --port 0 \
    --db "${WORK_DIR}/evidence2.db" \
    --media-dir "${SECOND_DIR}/media" >"${SECOND_LOG}" 2>&1 &
SECOND_PID=$!
for _ in $(seq 1 150); do
    [[ -s "${SECOND_DIR}/media/event-demo.mp4" ]] && break
    kill -0 "${SECOND_PID}" 2>/dev/null || break
    sleep 0.2
done
kill "${SECOND_PID}" 2>/dev/null || true
wait "${SECOND_PID}" 2>/dev/null || true

SECOND_CLIP="${SECOND_DIR}/media/event-demo.mp4"
if [[ ! -s "${SECOND_CLIP}" ]]; then
    fail "a segunda execucao nao produziu clipe"
    sed -n '1,40p' "${SECOND_LOG}" >&2
else
    FIRST_SHA="$(file_sha256 "${CLIP_PATH}")"
    SECOND_SHA="$(file_sha256 "${SECOND_CLIP}")"
    if [[ "${FIRST_SHA}" == "${SECOND_SHA}" ]]; then
        pass "hash estavel entre execucoes (${FIRST_SHA:0:16}...)"
    else
        fail "hash instavel: ${FIRST_SHA:0:16}... vs ${SECOND_SHA:0:16}..."
    fi
fi

# ---------------------------------------------------------------------------
printf '\n'
if [[ "${FAILURES}" -eq 0 ]]; then
    echo "SMOKE OK"
    exit 0
fi
echo "SMOKE FALHOU: ${FAILURES} verificacao(oes)"
exit 1
