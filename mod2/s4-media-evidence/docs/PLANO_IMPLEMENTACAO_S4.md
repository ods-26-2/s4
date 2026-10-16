# Plano de Implementação — S4 Evidência de Mídia

> Projeto ODS 2026/2 — Squad MOD-2 · Componente S4 (Camada 3 — Serviço)
> Guia de referência: [`docs/responsability.md`](../../../docs/referencia/responsability.md) · Checklist do guia (seção 9)

## 1. Objetivo

Evoluir `mod2/s4-media-evidence` de scaffold/protótipo para um serviço funcional e testável, fechando as
lacunas de conformidade com o Guia de Desenvolvimento. Escopo definido: **S4 completo** (S4.2 e S4.4 reais;
S4.1 e S4.3 mockados por não serem de responsabilidade do autor). S5 e B6 estão fora do escopo (não existem
no repositório).

## 2. Decisões acordadas

| Decisão | Opção escolhida | Justificativa |
| :--- | :--- | :--- |
| **S4.1 Ring Buffer** (Henrique) | `MockRAMBufferFacade` — frames sintéticos em memória | Interface `IRAMBufferFacade` mantida; mock testável sem GStreamer |
| **S4.3 Retenção & Expurgo** (Henrique) | **Diferido totalmente** — sem `PurgeMediaUseCase`; apenas contratos + mocks no-op | Não implementar lógica de outro responsável |
| **S4.4 API Descritores** | Mini servidor REST real com **cpp-httplib** (single-header, vendido) | Endpoint `GET /api/v1/clips/{clip_id}` |
| Salvar plano | `docs/PLANO_IMPLEMENTACAO_S4.md` | Junto ao guia |

## 3. Matriz de responsabilidade (mock-first)

| WI | Item | Dono (guia) | Ação |
| :-- | :-- | :-- | :-- |
| WI-1 | SHA-256 via OpenSSL EVP | Davi — S4.2 | **Implementar** |
| WI-2 | `MockRAMBufferFacade` + contrato `IMediaBufferReader` | Henrique — S4.1 | **Mock** (test double; real fica no backlog) |
| WI-3 | `ExtractClipUseCase` real (janela → arquivo + UUID + SHA-256 + persistir) + `IFileStorage`/`FileStorage` | Davi — S4.2 | **Implementar** |
| WI-4 | `LgpdRetentionStrategy` / `DiskQuotaRetentionStrategy` | Henrique — S4.3 | **Mock** — só a interface `IRetentionStrategy` (já existente); estratégias reais no backlog |
| WI-5 | `IDiskUsageProvider` / `statvfs` (85%) | Henrique — S4.3 | **Mock** — só a interface; implementação real no backlog |
| WI-6 | `PurgeMediaUseCase` / daemon de expurgo | Henrique — S4.3 | **Diferido** — sem código; não acoplado ao `main.cpp` |
| WI-7 | `SQLiteMediaClipRepository` real (registro S4.2 + descritores S4.4) | Davi | **Implementar** |
| WI-8 | `ClipDescriptorController` com DTO completo (ISO timestamps + POIs) | Davi — S4.4 | **Implementar** |
| WI-9 | `ClipDescriptorHttpServer` REST (`GET /api/v1/clips/{id}`, cpp-httplib) | Davi — S4.4 | **Implementar** |
| WI-10 | `src/main.cpp` daemon demo (SQLite + mock buffer + HTTP) | Davi — integração | **Implementar** (purge não acoplado) |
| WI-11 | CMake (alvo `SQLite3::SQLite3`, `uuid`, httplib, novos CTests) | Davi | **Implementar** |
| WI-12 | Testes unit + integration | Davi | **Implementar** (usando mocks de S4.1/S4.3) |
| WI-13 | `docs/PLANO_IMPLEMENTACAO_S4.md` + README | Davi | **Implementar** |

Também permanece **somente contrato**: `B3EventConsumer` (pub/sub do barramento B3).

## 4. Mudanças em entidades, erros e contratos

- `MediaClip`: novo campo `pointsOfInterest` (`std::vector<PointOfInterest>`, default vazio) para mapear o DTO de S4.4.
- Novas exceções tipadas em `domain/errors/domain_error.hpp`:
  - `HashingError`, `SqliteStorageError`, `FileOperationError`, `MediaBufferEmptyError` (todas `ApplicationError`).
- Novo contrato de domínio `IMediaBufferReader` (read-only, retorna payloads brutos) — preserva Clean Architecture
  (a application não depende da infra).
- Novo contrato de domínio `IFileStorage` (write/exists/remove) com impl `FileStorage` (infra, `std::filesystem`).
- `ExtractClipUseCase` deixa de usar hash `"dummy_sha256_hash"` e o include morto `<uuid/uuid.h>`.

## 5. Backlog — Henrique (integrações reais futuras)

- Ring buffer real em RAM compartilhada (`/dev/shm`) ou via pipeline GStreamer (`appsink`/`shmdata`).
- Muxing/encode MP4 real (NVENC ou H.264) — atualmente o arquivo contém os bytes dos frames concatenados.
- Estratégias de retenção **LGPD (7 dias)** e **FIFO por cota (85% NVMe)** + `statvfs`.
- `PurgeMediaUseCase` + daemon de expurgo com trava de auditoria (`is_locked_for_audit`).
- Consumo Pub/Sub do barramento B3.

## 6. Verificação

```bash
cmake -S mod2/s4-media-evidence -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Smoke test do serviço:

```bash
./build/s4_media_evidence --port 8080 --db /tmp/s4.db --media-dir /tmp/s4-media
curl http://localhost:8080/api/v1/clips/<clip_id>
sha256sum /tmp/s4-media/*.mp4   # comparar com o campo sha256_hash do JSON
```

Reavaliar o checklist de conformidade do guia (seção 9) ao final.

## 7. Dependências e riscos

- cpp-httplib vendido em `mod2/s4-media-evidence/third_party/httplib/httplib.h` (v0.56.0) — download via rede feito uma vez.
- Host: Fedora, gcc 16.2.1, CMake 4.3; deps `sqlite-devel`, `openssl-devel`, `libuuid-devel` já instaladas.
- Limitação conhecida: POIs persistidos em SQLite como texto `x,y,label` separados por `;` (label não pode conter `,` ou `;`).