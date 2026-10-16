# Plano de Containerização — S4 Evidência de Mídia

> Projeto ODS 2026/2 — Squad MOD-2 · Componente S4 (Camada 3 — Serviço)
> Referência: [`docs/PLANO_IMPLEMENTACAO_S4.md`](PLANO_IMPLEMENTACAO_S4.md) · [`docs/responsability.md`](../../../docs/referencia/responsability.md)

## 1. Objetivo

Empacotar o serviço `s4_media_evidence` como imagem OCI, pronta para rodar no alvo de produção
**NVIDIA Jetson Orin Nano (ARM64)** e também em desenvolvimento/CI (x86_64), sem acoplar o build ao
ambiente da máquina de desenvolvimento.

## 2. Decisões acordadas

| Decisão | Opção escolhida | Justificativa |
| :--- | :--- | :--- |
| Runtime de contêiner | **Docker Engine** (instalado via `dnf`) | Build/execução reais; infra-padrão |
| Arquitetura | **Multi-arch (arm64 + amd64)**; build arm64 **nativo na Jetson** | Deploy no Jetson Orin Nano; build seja arch-nativa no alvo |
| Imagem base | `debian:bookworm-slim` | Leve; libs disponíveis; compatível com híbrido dev/jetson |
| Tipo de build | **Multi-stage** (builder → runtime) | Imagem final sem toolchain de compilação |
| Rede no build | Apenas `apt` (dev libs); `httplib` já **vendado offline** | Build determinístico |

## 3. Estrutura de arquivos adicionada

```
mod2/s4-media-evidence/
├── Dockerfile              # Multi-stage (builder + runtime)
├── .dockerignore           # Contexto enxuto (build/, .git, *.db)
└── docker-compose.yml      # Build + port + volume persistente
```

## 4. Dockerfile (multi-stage)

- **`builder`** (`debian:bookworm-slim`): instala `g++`, `cmake`, `make`, `libsqlite3-dev`,
  `libssl-dev`, `uuid-dev`; copia `CMakeLists.txt`, `include/`, `src/`, `tests/`, `third_party/`;
  roda `cmake --build` e **`ctest` dentro da etapa de build** (impede subir imagem com teste vermelho).
- **`runtime`** (`debian:bookworm-slim`): instala somente libs de runtime (`libsqlite3-0`,
  `libssl3`, `libuuid1`, `libstdc++6`, `curl` para healthcheck); cria usuário não-root `s4`;
  copia o binário `s4_media_evidence`; expõe `8080`; `HEALTHCHECK` via `GET /healthz`.

Binário resultado: `CMD ["/app/s4_media_evidence", "--port", "8080", "--db", "/data/s4.db", "--media-dir", "/data/media"]`
→ banco SQLite e clipes ficam em volume montado em `/data` (não-no-runtime).

## 5. Mudança de aplicação: endpoint `/healthz`

Adiciona rota `GET /healthz` → `200 {"status":"ok"}` em `ClipDescriptorHttpServer`, consumida pelo
`HEALTHCHECK` do runtime e por testes de integração (expande cobertura HTTP).

## 6. Verificação (host x86_64)

```bash
docker build -t ods/s4-media-evidence:0.1.0 ./mod2/s4-media-evidence
docker run --rm -p 8080:8080 --name s4-test ods/s4-media-evidence:0.1.0 &
curl -sf http://127.0.0.1:8080/healthz
curl -s http://127.0.0.1:8080/api/v1/clips/<clip_id>     # 200 + JSON DTO
curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8080/api/v1/clips/inexistente  # 404

# Simular uma nova entrada de evento sem reiniciar o daemon
curl -s -X POST http://127.0.0.1:8080/api/v1/events \
  -H 'Content-Type: application/json' -d '{"event_id": "evt-smoke-1"}'   # 201 + DTO do clipe

docker cp s4-test:/data/media/event-demo.mp4 /tmp/      # conferir hash real
sha256sum /tmp/event-demo.mp4                            # == sha256_hash do JSON
docker stop s4-test
docker compose -f mod2/s4-media-evidence/docker-compose.yml up --build -d
```

O `ctest` roda dentro do `builder` a cada build → falha de teste quebra a imagem.

## 7. Build para a Jetson (ARM64)

- Rodar **neste** repositório na Jetson: `docker build -t ods/s4-media-evidence:0.1.0 .` (nativo arm64, sem emulação).
- Alternativa em x86_64 (CI): `docker buildx build --platform linux/arm64,linux/amd64 -t ods/s4-media-evidence:0.1.0 .`
  com `binfmt`/QEMU configurado.
- O código-alvo não usa GPU/NVENC ainda (mock), portanto a imagem arm64 roda igual; quando S4.1 real
  (GStreamer/`/dev/shm`) entrar, o runtime precisará do `gstreamer1.0-*` e privilégios de memória compartilhada.

## 8. Riscos e observações

- **instalação do Docker requer `sudo`** e re-login do usuário para o grupo `docker` (ou uso via `sudo docker`).
- `apt` precisa de rede no primeiro build; runtime não faz download.
- `/data` deve ser volume (senão clipes/DB morrem com o contêiner).
- SIGINT/SIGTERM já tratados por `main.cpp` → encerramento gracioso (`docker stop`).