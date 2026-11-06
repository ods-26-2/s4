#!/usr/bin/env bash
#
# S4 — demonstracao rapida contra um daemon JA em execucao.
#
# Diferente do smoke_e2e.sh, este script nao sobe nem derruba nada: ele conversa
# com a API que ja esta no ar (docker compose up -d, por exemplo) e mostra o
# fluxo inteiro em poucos segundos:
#
#   saude -> ring buffer -> evento -> descritor -> download -> SHA-256 -> CORS
#   -> Kafka/B3
#
# Uso: scripts/demo.sh [BASE_URL]
#      BASE_URL padrao: http://127.0.0.1:18084 (ou S4_BASE_URL)
#
# Kafka: le o log do daemon (S4_LOG_FILE ou docker compose) e testa o broker
#        200.17.66.56:9093 com kcat/kafkacat (se houver) ou TCP. Pule com
#        S4_SKIP_KAFKA_CHECK=1; para outro endereco use S4_KAFKA_PROBE_BROKERS.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BASE="${1:-${S4_BASE_URL:-http://127.0.0.1:18084}}"
BASE="${BASE%/}"

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

WORK_DIR="$(mktemp -d /tmp/s4_demo_XXXXXX)"
trap 'rm -rf "${WORK_DIR}"' EXIT

ok()   { printf '  \033[32mok\033[0m    %s\n' "$1"; }
fail() { printf '  \033[31mFALHA\033[0m %s\n' "$1"; }
warn() { printf '  \033[33maviso\033[0m %s\n' "$1"; }
info() { printf '        %s\n' "$1"; }
step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

FAILURES=0
check() {
    if [[ "$1" == "0" ]]; then ok "$2"; else fail "$2"; FAILURES=$((FAILURES + 1)); fi
}

# ---------------------------------------------------------------------------
step "1. o servico esta no ar"
HEALTH="$(curl -s -o "${WORK_DIR}/healthz" -w '%{http_code}' "${BASE}/healthz" || true)"
if [[ "${HEALTH}" == "200" ]]; then
    ok "GET /healthz respondeu 200 em ${BASE}"
else
    fail "GET /healthz respondeu '${HEALTH}' em ${BASE}"
    info "suba o servico antes: docker compose up -d"
    info "ou informe outra URL: scripts/demo.sh http://<ip-da-jetson>:18084"
    exit 1
fi

# ---------------------------------------------------------------------------
step "2. o ring buffer tem quadros"
STATS_CODE="$(curl -s -o "${WORK_DIR}/stats.json" -w '%{http_code}' \
    "${BASE}/api/v1/buffer/stats" || true)"
check "$([[ "${STATS_CODE}" == "200" ]] && echo 0 || echo 1)" \
    "GET /api/v1/buffer/stats respondeu 200"

if [[ "${STATS_CODE}" == "200" ]]; then
    FRAMES="$(jq -r '.frames_stored // 0' "${WORK_DIR}/stats.json")"
    SESSION="$(jq -r '.session_id // "?"' "${WORK_DIR}/stats.json")"
    SPAN="$(jq -r '.span_seconds // 0' "${WORK_DIR}/stats.json")"
    info "sessao: ${SESSION}"
    info "quadros guardados: ${FRAMES} | janela: ${SPAN}s"
    check "$([[ "${FRAMES}" -gt 0 ]] && echo 0 || echo 1)" \
        "ha quadros no buffer"
fi

# ---------------------------------------------------------------------------
step "3. disparar um evento e extrair o clipe"

# O instante do evento vem do proprio buffer: no replay o stream ja terminou,
# e "agora" nao existe depois do EOS no relogio de captura.
TS="$(jq -r '.newest_capture_ts_ns // 0' "${WORK_DIR}/stats.json" 2>/dev/null || echo 0)"
if [[ "${TS}" == "0" || -z "${TS}" ]]; then
    fail "nao foi possivel ler newest_capture_ts_ns"
    exit 1
fi

EVENT_ID="demo-$((RANDOM))-$(date +%s)"
EVENT_CODE="$(curl -s -o "${WORK_DIR}/event.json" -w '%{http_code}' \
    -X POST "${BASE}/api/v1/events" \
    -H 'Content-Type: application/json' \
    -d "{\"event_id\":\"${EVENT_ID}\",\"capture_ts_ns\":${TS},\"pre_window_seconds\":1.0,\"post_window_seconds\":0.5}" \
    || true)"
check "$([[ "${EVENT_CODE}" == "201" ]] && echo 0 || echo 1)" \
    "POST /api/v1/events respondeu ${EVENT_CODE} (esperado 201)"

if [[ "${EVENT_CODE}" != "201" ]]; then
    info "resposta: $(head -c 300 "${WORK_DIR}/event.json")"
    if grep -q "No valid frames found" "${WORK_DIR}/event.json" 2>/dev/null; then
        info "a fonte atual nao gera H.264 (fonte sintetica)."
        info "para o fluxo de video real, suba com replay de MP4:"
        info "  ODS_S4_SOURCE=\"/app/video_test/10 Minutes of Amazon Rainforest (Free Download).mp4\" \\"
        info "    docker compose up -d"
    fi
    exit 1
fi

CLIP_ID="$(jq -r '.clip_id // empty' "${WORK_DIR}/event.json")"
SHA_API="$(jq -r '.sha256_hash // empty' "${WORK_DIR}/event.json")"
FILE_URI="$(jq -r '.file_uri // empty' "${WORK_DIR}/event.json")"
info "clip_id: ${CLIP_ID}"
info "arquivo: ${FILE_URI}"
info "janela: $(jq -r '.start_time' "${WORK_DIR}/event.json") -> $(jq -r '.end_time' "${WORK_DIR}/event.json")"

# ---------------------------------------------------------------------------
step "4. o descritor esta indexado"
DESC_CODE="$(curl -s -o "${WORK_DIR}/descriptor.json" -w '%{http_code}' \
    "${BASE}/api/v1/clips/${CLIP_ID}" || true)"
check "$([[ "${DESC_CODE}" == "200" ]] && echo 0 || echo 1)" \
    "GET /api/v1/clips/${CLIP_ID} respondeu 200"

if [[ "${DESC_CODE}" == "200" ]]; then
    check "$([[ "$(jq -r '.sha256_hash // empty' "${WORK_DIR}/descriptor.json")" == "${SHA_API}" ]] && echo 0 || echo 1)" \
        "sha256 do descritor confere com o da criacao"
fi

# ---------------------------------------------------------------------------
step "5. baixar o video e conferir a integridade"
MEDIA_PATH="${WORK_DIR}/${CLIP_ID}.mp4"
MEDIA_CODE="$(curl -s -o "${MEDIA_PATH}" -w '%{http_code}' \
    "${BASE}/api/v1/clips/${CLIP_ID}/media" || true)"
MEDIA_OK=0

if [[ "${MEDIA_CODE}" == "200" ]]; then
    ok "GET /api/v1/clips/${CLIP_ID}/media respondeu 200"
    MEDIA_OK=1
else
    # Imagem antiga (anterior ao S4.3) nao tem a rota /media. O MP4 continua no
    # bind mount ./data; usa-lo mantem a demo honesta, desde que rotulado.
    FALLBACK_PATH=""
    if [[ -n "${FILE_URI}" && -f "${FILE_URI}" ]]; then
        FALLBACK_PATH="${FILE_URI}"
    elif [[ -n "${FILE_URI}" ]]; then
        DATA_DIR="${S4_DATA_DIR:-}"
        if [[ -z "${DATA_DIR}" && -f "${PROJECT_DIR}/.env" ]]; then
            DATA_DIR="$(sed -n 's/^ODS_S4_DATA_DIR=//p' "${PROJECT_DIR}/.env" \
                | tail -1 | tr -d '\r"'"'")"
        fi
        DATA_DIR="${DATA_DIR:-${PROJECT_DIR}/data}"
        CANDIDATE="${DATA_DIR}/media/$(basename "${FILE_URI}")"
        [[ -f "${CANDIDATE}" ]] && FALLBACK_PATH="${CANDIDATE}"
    fi

    if [[ -n "${FALLBACK_PATH}" && "${S4_REQUIRE_MEDIA_API:-0}" != "1" ]]; then
        ok "rota /media ausente (HTTP ${MEDIA_CODE}); usando arquivo do bind mount"
        info "fallback local: ${FALLBACK_PATH}"
        info "S4_REQUIRE_MEDIA_API=1 exige a rota /media e reprova este fallback"
        cp "${FALLBACK_PATH}" "${MEDIA_PATH}"
        MEDIA_OK=1
    else
        fail "GET /api/v1/clips/${CLIP_ID}/media respondeu ${MEDIA_CODE} (esperado 200)"
        if [[ "${S4_REQUIRE_MEDIA_API:-0}" == "1" ]]; then
            info "S4_REQUIRE_MEDIA_API=1: fallback local desabilitado"
        fi
    fi
fi

if [[ "${MEDIA_OK}" -eq 1 ]]; then
    SIZE="$(stat -c%s "${MEDIA_PATH}")"
    SHA_DISCO="$(sha256sum "${MEDIA_PATH}" | cut -d' ' -f1)"
    ok "arquivo disponivel: ${SIZE} bytes"
    if [[ "${SHA_DISCO}" == "${SHA_API}" ]]; then
        ok "sha256 confere: ${SHA_DISCO:0:16}..."
    else
        fail "sha256 divergente: API ${SHA_API:0:16}... vs arquivo ${SHA_DISCO:0:16}..."
    fi

    if command -v gst-launch-1.0 >/dev/null 2>&1; then
        if timeout 30 gst-launch-1.0 -q \
            filesrc location="${MEDIA_PATH}" \
            ! qtdemux ! h264parse ! fakesink >/dev/null 2>&1; then
            ok "container MP4 valido (qtdemux ! h264parse)"
        else
            fail "o container nao passou por qtdemux ! h264parse"
        fi
    else
        info "pula: gst-launch-1.0 ausente, container nao verificado"
    fi
fi

# ---------------------------------------------------------------------------
step "6. CORS (preflight)"
CORS_HEADERS="$(curl -si -X OPTIONS "${BASE}/api/v1/events" \
    -H 'Origin: http://localhost:5173' 2>/dev/null | grep -i 'access-control' || true)"
if [[ -n "${CORS_HEADERS}" ]]; then
    ok "resposta CORS presente"
    info "$(echo "${CORS_HEADERS}" | tr '\n' ' ')"
else
    # CORS e opcional (ODS_S4_CORS_ORIGIN); nao reprova a demonstracao do fluxo.
    warn "nenhum cabecalho Access-Control na resposta (CORS opcional)"
fi

# ---------------------------------------------------------------------------
step "7. conexao com o Kafka/B3"

# Broker efetivo: ambiente do demo, .env do projeto, container em execucao ou
# o padrao do daemon. Mostrar o mesmo endereco que o S4 usa evita confusao.
KAFKA_BROKERS="${S4_KAFKA_BROKERS:-${ODS_S4_KAFKA_BROKERS:-}}"
if [[ -z "${KAFKA_BROKERS}" && -f "${PROJECT_DIR}/.env" ]]; then
    KAFKA_BROKERS="$(sed -n 's/^ODS_S4_KAFKA_BROKERS=//p' "${PROJECT_DIR}/.env" \
        | tail -1 | tr -d '\r"' | tr -d "'")"
fi
KAFKA_CONTAINER=0
if command -v docker >/dev/null 2>&1 && \
   docker ps --format '{{.Names}}' 2>/dev/null | grep -qx 's4-media-evidence'; then
    KAFKA_CONTAINER=1
    if [[ -z "${KAFKA_BROKERS}" ]]; then
        KAFKA_BROKERS="$(docker inspect s4-media-evidence \
            --format '{{range .Config.Env}}{{println .}}{{end}}' 2>/dev/null \
            | sed -n 's/^ODS_S4_KAFKA_BROKERS=//p' | tail -1 | tr -d '\r')"
    fi
fi
KAFKA_BROKERS="${KAFKA_BROKERS:-200.17.66.56:9093}"

# O broker fica em 200.17.66.56:9093. O probe usa o mesmo endereco do daemon,
# a menos que S4_KAFKA_PROBE_BROKERS aponte para outro.
KAFKA_PROBE_BROKERS="${S4_KAFKA_PROBE_BROKERS:-${KAFKA_BROKERS}}"
KAFKA_PROBE_EXTERNAL=0
BASE_HOST="${BASE#*://}"
BASE_HOST="${BASE_HOST%%/*}"
if [[ "${BASE_HOST}" == \[*\]* ]]; then
    BASE_HOST="${BASE_HOST#[}"
    BASE_HOST="${BASE_HOST%%]*}"
else
    BASE_HOST="${BASE_HOST%%:*}"
fi
if [[ "${BASE_HOST}" != "127.0.0.1" && "${BASE_HOST}" != "localhost" && \
      "${BASE_HOST}" != "::1" && "${KAFKA_CONTAINER}" -eq 0 ]]; then
    KAFKA_PROBE_EXTERNAL=1
fi

info "broker do daemon: ${KAFKA_BROKERS}"
if [[ "${KAFKA_PROBE_BROKERS}" != "${KAFKA_BROKERS}" ]]; then
    info "probe: ${KAFKA_PROBE_BROKERS} (S4_KAFKA_PROBE_BROKERS)"
fi

# Log do daemon: arquivo explicito ou docker compose quando o container esta
# aqui. E o log que diz se o S4 realmente subiu o transporte B3.
S4_LOGS=""
if [[ -n "${S4_LOG_FILE:-}" && -r "${S4_LOG_FILE}" ]]; then
    S4_LOGS="$(tail -n 2000 "${S4_LOG_FILE}" 2>/dev/null || true)"
elif [[ "${KAFKA_CONTAINER}" -eq 1 ]]; then
    S4_LOGS="$(docker compose --project-directory "${PROJECT_DIR}" logs \
        --no-color --tail=2000 s4-media-evidence 2>/dev/null || true)"
fi

KAFKA_ENABLED_LINE="$(printf '%s\n' "${S4_LOGS}" \
    | grep -m1 'Kafka/B3 bus enabled:' || true)"
KAFKA_DISABLED_LINE="$(printf '%s\n' "${S4_LOGS}" \
    | grep -m1 -E 'Kafka/B3 bus disabled|Kafka requested but not compiled|Kafka disabled after initialization error' || true)"
KAFKA_DOWN_LINE="$(printf '%s\n' "${S4_LOGS}" \
    | grep -m1 'Kafka: all brokers are down' || true)"

KAFKA_REACHABLE=0
KAFKA_PROBLEM=0
KAFKA_LOG_CONFIRMED=0
KAFKA_EXPECTED=1

# Kafka e uma integracao externa: na demo local a indisponibilidade nao deve
# reprovar o fluxo de midia. S4_REQUIRE_KAFKA=1 restaura o modo estrito.
kafka_fail() {
    if [[ "${S4_REQUIRE_KAFKA:-0}" == "1" ]]; then
        fail "$1"
        FAILURES=$((FAILURES + 1))
    else
        warn "$1"
        info "Kafka opcional nesta demo; use S4_REQUIRE_KAFKA=1 para exigir"
    fi
    KAFKA_PROBLEM=1
}

if [[ "${S4_SKIP_KAFKA_CHECK:-0}" == "1" ]]; then
    KAFKA_EXPECTED=0
    warn "verificacao Kafka pulada (S4_SKIP_KAFKA_CHECK=1)"
elif [[ -n "${KAFKA_DISABLED_LINE}" ]]; then
    KAFKA_EXPECTED=0
    warn "Kafka desabilitado no daemon; nada a verificar"
    info "${KAFKA_DISABLED_LINE}"
else
    if [[ -n "${KAFKA_ENABLED_LINE}" ]]; then
        KAFKA_LOG_CONFIRMED=1
        ok "o daemon subiu o transporte Kafka/B3"
        info "${KAFKA_ENABLED_LINE}"
    elif [[ -z "${S4_LOGS}" ]]; then
        warn "log do daemon indisponivel; nao da para confirmar o lado do S4"
        info "defina S4_LOG_FILE ou mantenha o container s4-media-evidence ativo"
    else
        warn "o log do daemon nao mostra 'Kafka/B3 bus enabled'"
    fi

    if [[ -n "${KAFKA_DOWN_LINE}" ]]; then
        kafka_fail "o daemon reporta todos os brokers fora do ar"
        info "${KAFKA_DOWN_LINE}"
    fi
fi

kafka_tcp_probe() {
    local host="$1" port="$2" scope="$3"
    if [[ "${scope}" == "container" ]]; then
        timeout 8 docker exec s4-media-evidence bash -c \
            "exec 3<>/dev/tcp/${host}/${port}" >/dev/null 2>&1
    else
        timeout 8 bash -c "exec 3<>/dev/tcp/${host}/${port}" >/dev/null 2>&1
    fi
}

kafka_tcp_all() {
    local scope="$1" broker host port
    local -a brokers
    IFS=',' read -r -a brokers <<< "${KAFKA_PROBE_BROKERS}"
    for broker in "${brokers[@]}"; do
        host="${broker%:*}"
        port="${broker##*:}"
        [[ "${host}" == "${port}" ]] && port=9093
        kafka_tcp_probe "${host}" "${port}" "${scope}" || return 1
    done
    return 0
}

if [[ "${KAFKA_EXPECTED}" -eq 1 && "${KAFKA_PROBLEM}" -eq 0 ]]; then
    KCAT_BIN=""
    if command -v kcat >/dev/null 2>&1; then
        KCAT_BIN="$(command -v kcat)"
    elif command -v kafkacat >/dev/null 2>&1; then
        KCAT_BIN="$(command -v kafkacat)"
    fi

    if [[ -n "${KCAT_BIN}" ]] && \
       timeout 10 "${KCAT_BIN}" -b "${KAFKA_PROBE_BROKERS}" -L >/dev/null 2>&1; then
        ok "broker respondeu ao metadata request em ${KAFKA_PROBE_BROKERS} (${KCAT_BIN##*/})"
        KAFKA_REACHABLE=1
    elif [[ "${KAFKA_CONTAINER}" -eq 1 ]] && kafka_tcp_all container; then
        ok "broker Kafka alcancavel de dentro do container do S4: ${KAFKA_PROBE_BROKERS}"
        KAFKA_REACHABLE=1
    elif [[ "${KAFKA_CONTAINER}" -eq 1 ]]; then
        kafka_fail "broker Kafka inalcancavel a partir do container do S4: ${KAFKA_PROBE_BROKERS}"
    elif [[ "${KAFKA_PROBE_EXTERNAL}" -eq 1 ]] && kafka_tcp_all host; then
        ok "broker Kafka alcancavel de fora da Jetson: ${KAFKA_PROBE_BROKERS}"
        KAFKA_REACHABLE=1
    elif [[ "${KAFKA_PROBE_EXTERNAL}" -eq 1 ]]; then
        kafka_fail "broker Kafka inalcancavel de fora da Jetson: ${KAFKA_PROBE_BROKERS}"
        info "ajuste S4_KAFKA_PROBE_BROKERS se o endereco do broker for outro"
    elif kafka_tcp_all host; then
        ok "broker Kafka alcancavel por TCP a partir deste host: ${KAFKA_PROBE_BROKERS}"
        KAFKA_REACHABLE=1
    else
        warn "broker Kafka inalcancavel deste host: ${KAFKA_PROBE_BROKERS}"
    fi
fi

if [[ "${KAFKA_EXPECTED}" -eq 1 && "${KAFKA_PROBLEM}" -eq 0 ]]; then
    if [[ "${KAFKA_LOG_CONFIRMED}" -eq 1 && "${KAFKA_REACHABLE}" -eq 1 ]]; then
        ok "Kafka OK: transporte habilitado no S4 e broker alcancavel em ${KAFKA_PROBE_BROKERS}"
    elif [[ "${KAFKA_PROBE_EXTERNAL}" -eq 1 && "${KAFKA_REACHABLE}" -eq 1 ]]; then
        ok "Kafka externo OK: broker alcancavel em ${KAFKA_PROBE_BROKERS}"
        info "log do S4 nao lido; o transporte do daemon nao foi confirmado por este script"
    elif [[ "${KAFKA_LOG_CONFIRMED}" -eq 1 ]]; then
        warn "transporte habilitado no S4, mas o broker nao foi confirmado"
    elif [[ "${KAFKA_REACHABLE}" -eq 1 ]]; then
        warn "broker alcancavel, mas o log do S4 nao confirmou o transporte"
    fi
fi

# ---------------------------------------------------------------------------
printf '\n'
if [[ "${FAILURES}" -eq 0 ]]; then
    printf '\033[32mDEMO OK\033[0m — fluxo evento -> clipe -> hash verificado'
    if [[ "${KAFKA_EXPECTED}" -eq 1 && "${KAFKA_PROBLEM}" -eq 0 && \
          "${KAFKA_REACHABLE}" -eq 1 ]]; then
        if [[ "${KAFKA_LOG_CONFIRMED}" -eq 1 ]]; then
            printf ' + Kafka OK'
        elif [[ "${KAFKA_PROBE_EXTERNAL}" -eq 1 ]]; then
            printf ' + Kafka externo OK'
        fi
    fi
    printf '.\n'
    exit 0
fi
printf '\033[31mDEMO FALHOU\033[0m: %d verificacao(oes)\n' "${FAILURES}"
exit 1
