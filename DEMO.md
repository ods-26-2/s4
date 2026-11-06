# Demo do S4 na Jetson

Kafka: `200.17.66.56:9093`.
API: `http://127.0.0.1:18084` na própria Jetson (ou `http://<IP_DA_JETSON>:18084`).
Pré-requisitos: `docker compose`, `curl`, `jq` e, para o teste Kafka, `kcat`.

## 1. Configurar

```bash
cd ~/MOD2_ODS
cp .env.example .env
```

Edite o `.env`:

```bash
# NVMe
ODS_S4_DATA_DIR=/mnt/nvme/s4

# Kafka
ODS_S4_KAFKA_BROKERS=200.17.66.56:9093

# P4 ao vivo (escolha UMA fonte)
ODS_S4_P4_SHM=/ipc/ods_p4_cam0
ODS_S4_IPC_DIR=/tmp
ODS_S4_WIDTH=1280
ODS_S4_HEIGHT=720
ODS_S4_FPS=30

# OU, sem P4, replay de um MP4 (comente as linhas do P4 acima)
# ODS_S4_SOURCE="/app/video_test/10 Minutes of Amazon Rainforest (Free Download).mp4"
```

## 2. Subir

```bash
# a porta deve estar livre
ss -ltnp | grep ':18084'

docker compose build
docker compose up -d
docker compose ps
docker compose logs | grep "Kafka/B3 bus enabled"
```

Deve aparecer: `Kafka/B3 bus enabled: 200.17.66.56:9093`.

## 3. Saúde e buffer

```bash
curl -s http://127.0.0.1:18084/healthz
curl -s http://127.0.0.1:18084/api/v1/buffer/stats \
  | jq '{session_id, frames_stored, newest_capture_ts_ns}'
```

## 4. Criar um clipe pela API

```bash
TS=$(curl -s http://127.0.0.1:18084/api/v1/buffer/stats | jq -r .newest_capture_ts_ns)

curl -s -X POST http://127.0.0.1:18084/api/v1/events \
  -H 'Content-Type: application/json' \
  -d "{\"event_id\":\"demo-http-1\",\"capture_ts_ns\":$TS,\"pre_window_seconds\":2.0}" \
  | tee /tmp/clip.json | jq

CLIP=$(jq -r .clip_id /tmp/clip.json)
```

## 5. Consultar o descritor

```bash
curl -s http://127.0.0.1:18084/api/v1/clips/$CLIP | jq
```

## 6. Baixar o vídeo e conferir o SHA-256

```bash
curl -s -o /tmp/$CLIP.mp4 http://127.0.0.1:18084/api/v1/clips/$CLIP/media
ls -lh /tmp/$CLIP.mp4
sha256sum /tmp/$CLIP.mp4
jq -r .sha256_hash /tmp/clip.json
```

Os dois hashes devem ser iguais.

## 7. CORS (opcional)

```bash
curl -si -X OPTIONS http://127.0.0.1:18084/api/v1/events \
  -H 'Origin: http://localhost:5173' | grep -i access-control
```

## 8. Kafka

Em um terminal, acompanhe o S4:

```bash
docker compose logs -f | grep -E "S2 event|Kafka"
```

Em outro terminal, se o `kcat` estiver instalado, publique um evento do S2
(use com o P4 ao vivo; o replay não serve para este teste):

```bash
kcat -b 200.17.66.56:9093 -P -t ods.s2.event <<'JSON'
{
  "message_type": "event",
  "message_id": "11111111-1111-4111-8111-111111111111",
  "schema": "ods.s2.event",
  "schema_version": "1.0",
  "producer": "S2",
  "published_at": "2026-11-06T09:00:00.000Z",
  "captured_at": "2026-11-06T09:00:00.000Z",
  "source_id": "cam0",
  "session_id": "22222222-2222-4222-8222-222222222222",
  "sequence": 1,
  "payload": {
    "event_id": "demo-kafka-1",
    "status": "CONFIRMED",
    "phase": "STARTED"
  }
}
JSON
```

Veja o descritor publicado pelo S4:

```bash
kcat -b 200.17.66.56:9093 -C -t ods.s4.clip -o -1
```

O consumidor fica aguardando; use `Ctrl+C` para sair.

## 9. Parar

```bash
docker compose down
```
