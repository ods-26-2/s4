# S4 — Evidência de Mídia (Media Evidence)

> Componente da **Camada 3 (Serviço)** do Squad **MOD-2** (Mídia, Métricas e Interfaces) no projeto **ODS 2026/2**.

O serviço **S4** é responsável por gerenciar o ciclo de vida das evidências audiovisuais na borda (**NVIDIA Jetson Orin Nano**), incluindo captura contínua em buffer de memória RAM, extração de clipes probatórios vinculados a eventos do Barramento B3, persistência em SSD NVMe com integridade criptográfica (SHA-256) e expurgo automático conforme LGPD e políticas de cota de disco.

---

> ### ▶️ Demonstração rápida
>
> Para **mostrar o fluxo funcionando** para outra pessoa — do MP4 real até o
> artefato indexado, com o que esperar em cada passo e por que cada resultado é
> prova — veja o guia: **[`docs/S4.4-demo-replay-e2e.md`](docs/S4.4-demo-replay-e2e.md)**.
>
> Atalho: `./scripts/smoke_e2e.sh ./build/s4_media_evidence` sobe o daemon com
> replay de vídeo real, extrai um clipe, confere a decodificabilidade do
> container e exige que duas execuções produzam o mesmo SHA-256.

## 🏗️ Arquitetura e Estrutura de Diretórios

O projeto adota os princípios de **Clean Architecture**, **Domain-Driven Design (DDD)** e **Design Patterns (GoF)**, desenvolvido em **C++20** com sistema de build **CMake**.

```text
mod2/s4-media-evidence/
├── CMakeLists.txt              # Configuração de build C++20 e dependências
├── README.md                   # Documentação do componente
├── Dockerfile                  # Imagem OCI multi-arch (builder + runtime)
├── docker-compose.yml          # Orquestração local (porta 8080 + volume)
├── .dockerignore               # Exclusões do contexto de build
├── third_party/httplib/        # cpp-httplib vendido (HTTP server)
├── include/s4/                 # Headers públicos da biblioteca
│   ├── domain/                 # 1. Coração do negócio (zero dependências externas)
│   │   ├── entities/           # Entidades (MediaClip, RingBuffer)
│   │   ├── value_objects/      # Objetos de Valor (TimeWindow, ClipDescriptor,
│   │   │                       #   CaptureWindow, FrameDescriptor, BufferCapacity)
│   │   ├── repositories/       # Contratos (IMediaClipRepository, IFileStorage, IFrameStore)
│   │   ├── services/           # Portas do buffer (IMediaBufferReader, IFrameSource)
│   │   ├── strategies/         # Padrão GoF Strategy de retenção (IRetentionStrategy)
│   │   └── errors/             # Hierarquia de exceções de domínio
│   ├── application/            # 2. Orquestração de casos de uso e DTOs
│   │   ├── services/           # Portas do fluxo real (ICaptureBufferReader, IMediaMuxer)
│   │   ├── use_cases/          # Casos de uso (ExtractClipUseCase, PurgeMediaUseCase)
│   │   └── dtos/               # Data Transfer Objects (ClipDescriptorDTO, ISO 8601)
│   ├── infrastructure/         # 3. Adaptadores e integrações externas
│   │   ├── database/           # Implementações de repositório (SQLite, In-Memory)
│   │   ├── filesystem/         # Persistência em disco (FileStorage)
│   │   ├── hashing/            # Integridade criptográfica SHA-256 (OpenSSL)
│   │   ├── gstreamer/          # Fachada do buffer: contrato, mock, RingBufferRAMFacade,
│   │   │                       #   fonte de captura (appsink_frame_source) e
│   │   │                       #   o muxer MP4 (GStreamerMp4Muxer)
│   │   ├── media/              # ISO-BMFF: normalização dos timestamps do container
│   │   ├── ringbuffer/         # S4.1: arena /dev/shm, ponte de relógios,
│   │   │                       #   fonte sintética e cenário gravado
│   │   ├── retention/          # S4.3: daemon de expurgo, statvfs, log de expurgo
│   │   └── b3_bus/             # Consumidor de eventos do Barramento B3 (contrato)
│   └── presentation/           # 4. Controladores e exposição de API
│       └── http/               # Controller + servidor REST (ClipDescriptorHttpServer)
├── src/                        # Implementações (.cpp)
│   ├── application/use_cases/  # extract_clip.cpp
│   ├── presentation/http/      # controller + servidor REST
│   └── infrastructure/         # hashing, database, filesystem, gstreamer
├── main.cpp → src/main.cpp     # Daemon demo executável
├── tools/                      # record_scenario: grava um cenário em .idx/.bin
├── scripts/smoke_e2e.sh        # Verificação E2E de ponta a ponta
└── tests/                      # Suíte de testes automatizados
    ├── unit/                   # Testes puros (domínio, casamento de uso, hash, ISO-BMFF)
    └── integration/            # Testes com SQLite, filesystem real, HTTP e MP4 real
```

### Documentação

| Documento | Para quê |
| :--- | :--- |
| [`docs/S4.4-demo-replay-e2e.md`](docs/S4.4-demo-replay-e2e.md) | **Guia de demonstração**: mostrar o fluxo do MP4 real ao artefato indexado, passo a passo. |
| [`docs/S4.1-ring-buffer.md`](docs/S4.1-ring-buffer.md) | Detalhes do ring buffer: arena, evicção, ponte de relógios. |

---

## 🧩 Submódulos e Responsabilidades (Squad MOD-2)

Conforme definido em [`docs/responsability.md`](../../../docs/referencia/responsability.md):

| Submódulo | Funcionalidade | Padrão / Arquitetura | Responsável |
| :--- | :--- | :--- | :--- |
| **S4.1 — Ring Buffer Contínuo** | Gravação circular em RAM (`/dev/shm`) dos últimos $N$ segundos pré-evento, com alinhamento a keyframe e espera do pós-evento. **Implementado**: `RingBufferRAMFacade` (substitui o `MockRAMBufferFacade`, que permanece como dublê de teste). Ver [`docs/S4.1-ring-buffer.md`](docs/S4.1-ring-buffer.md). | Facade + POSIX Shared Memory / GStreamer | Henrique Azevedo |
| **S4.2 — Binding Evento-Mídia** | Extração de trecho pré/pós evento da RAM, exportação em arquivo e hash SHA-256 real (OpenSSL). **Implementado** | Command Handler + SHA-256 | Davi Gomes |
| **S4.3 — Retenção & Expurgo LGPD** | Expurgo automático após 7 dias (LGPD) e emergencial FIFO a 85% do NVMe (`statvfs`), respeitando `is_locked_for_audit`. **Implementado**: `PurgeDaemon` + `LgpdRetentionStrategy` / `DiskQuotaFifoStrategy`, ligado no `main.cpp`. Ver [`docs/S4.3-retencao-expurgo.md`](docs/S4.3-retencao-expurgo.md). | Daemon Worker + Strategy Pattern | Henrique Azevedo |
| **S4.4 — API Descritores de Clipe** | Exposição de metadados e URIs locais sem trafegar vídeo binário. **Implementado**: REST `GET /api/v1/clips/{id}` + `POST /api/v1/events` (simulação de novo evento em runtime) | REST Controller + Clean Architecture | Davi Gomes |

---

## 🎞️ S4.1 — Ring Buffer Contínuo: entrada → processamento → saída

Resumo do contrato do S4.1. O detalhamento completo (funcionamento interno, decisões de projeto, dimensionamento)
está em [`docs/S4.1-ring-buffer.md`](docs/S4.1-ring-buffer.md).

### O que recebe

| Entrada | De onde vem | Formato |
| :--- | :--- | :--- |
| **Quadros de vídeo** | Câmera (P4) via GStreamer `appsink`, replay gravado (P6) ou fonte sintética (demo/testes) | `CapturedFrame { captureTsNs, data, length, isKeyframe, sessionId }` — H.264 Annex-B, 1 quadro por chamada |
| **Pedido de trecho** | S4.2 (`ExtractClipUseCase`), disparado pelo `POST /api/v1/events` ou pelo barramento B3 | `readWindow(start, end)` em relógio de parede, ou `extractAround(tsEvento, preSeg, posSeg, timeout)` em relógio de captura |
| **Configuração** | Build da aplicação | `RingBufferConfig { cameraId, windowSeconds, bitrateBps, safetyFactor, width, height, replaceStaleSegment }` |

### O que faz

1. **Aloca uma arena fixa em RAM compartilhada** (`/dev/shm/ods_s4_ring_<cameraId>`), dimensionada por
   `bitrate / 8 × windowSeconds × safetyFactor`. Exemplo: 1080p a 4 Mbps com 30 s ≈ 21 MB por câmera. Nada é escrito no NVMe.
2. **Grava continuamente** cada quadro na posição do ponteiro de escrita. Quando a arena enche, o ponteiro volta ao início
   e **os quadros mais antigos são expulsos**, então o buffer guarda sempre os últimos *N* segundos.
3. **Mantém um índice** (`FrameDescriptor { sequence, captureTsNs, offset, length, isKeyframe, sessionId }`), ou seja,
   os "ponteiros de frames" da especificação.
4. **Protege a linha do tempo**: rejeita e conta quadros com relógio retrocedendo e reinicia o buffer quando o `sessionId`
   muda (replay/reinício), para nunca misturar duas sessões num mesmo clipe.
5. **Responde a pedidos de trecho**: devolve os quadros da janela **começando no keyframe anterior** (senão o trecho não
   seria decodificável) e, se o pós-evento ainda não foi capturado, **espera por ele** até o timeout.

### O que entrega

| Saída | Para quem | Conteúdo |
| :--- | :--- | :--- |
| `std::vector<std::vector<uint8_t>>` (`readWindow`) | S4.2, via porta `IMediaBufferReader` | Payloads H.264 da janela, começando em keyframe. **Lista vazia** se a janela não está no buffer ou se a captura já foi parada (o S4.2 converte em `MediaBufferEmptyError` → HTTP 500). |
| `std::vector<VideoFrame>` (`extractWindow`) | S4.2, via `IRAMBufferFacade` | Os mesmos quadros, com timestamp de parede e dimensões |
| `ExtractedSegment` (`extractAround`) | Quem conhece o relógio de captura | Quadros + `sessionId` + flags `isTruncatedAtStart` / `isTruncatedAtEnd` |
| `BufferStats` (`stats()`) | Observabilidade | Quadros guardados, segundos cobertos, bytes usados, total ingerido/expulso/rejeitado |
| Segmento `/dev/shm/ods_s4_ring_<cameraId>` | Outros processos (leitura sem cópia) | Os bytes brutos da arena circular |

### Integração com o S4.2 / S4.4 (fluxo de um evento)

Há dois caminhos de execução, e a diferença entre eles é a razão de existir o relógio de captura.

**Caminho do S4.4 — janela em relógio de captura** (`executeCaptureWindow`):

```text
POST /api/v1/events {"event_id":"evt-1","capture_ts_ns":T,
                     "pre_window_seconds":1.0,"post_window_seconds":0.5}
  └─> ExtractClipUseCase.executeCaptureWindow(...)            (S4.2)
        └─> RingBufferRAMFacade.extractCaptureWindow([T-1s, T+0.5s])   (S4.1)
        │     espera o pós-evento chegar, realinhando no keyframe mais próximo
        └─> GStreamerMp4Muxer.mux(...)                        (S4.4)
        │     appsrc → h264parse → mp4mux (faststart) → appsink
        │     normaliza os timestamps de cabeçalho do container
        └─> grava $media_dir/evt-1.mp4 + SHA-256 do arquivo final + SQLite
  <── 201 {clip_id, file_uri, start_time, end_time, sha256_hash, ...}
```

**Caminho legado — janela em relógio de parede** (`execute` com `start_ms`/`end_ms`): mantido para
compatibilidade, sem muxing de container e sem a espera do pós-evento.

As duas portas que sustentam o caminho novo são
[`ICaptureBufferReader`](include/s4/application/services/capture_buffer_reader.hpp) e
[`IMediaMuxer`](include/s4/application/services/media_muxer.hpp), injetadas no caso de uso. O `main.cpp` faz
essa ligação; o `MockRAMBufferFacade` continua no repositório apenas como dublê dos testes do S4.2.

**Sobre o relógio de captura:** o vídeo de teste tem B-frames, então o PTS não é monotônico na ordem de chegada e
descartar quadros no ring buffer. O carimbo usado é o **DTS**, com fallback para PTS. E o instante do evento é
sempre derivado do intervalo que o buffer **realmente guarda** (`GET /api/v1/buffer/stats`), nunca "agora": o
replay de arquivo entrega os 10 minutos de vídeo em ~500 ms e chega ao fim, e depois do fim não existe "depois"
no relógio de captura — uma janela ancorada no instante mais recente esperaria para sempre.

### Validação

| Verificação | Resultado |
| :--- | :--- |
| `ctest` (13 suítes) | 13/13, estáveis em execuções repetidas. Duas podem sair **Skipped** (`77`) em vez de aprovadas: `test_s4_replay_e2e`, sem MP4 em `video_test/`, e `test_s4_gstreamer_capture`, sem `x264enc`/`avdec_h264`. Skipped é o resultado honesto para "não executado"; para reprovar nesses casos, `-DODS_S4_REQUIRE_SAMPLE_VIDEO=ON` |
| E2E com MP4 real: replay → evento → extração | `201`; container aceito por `qtdemux ! h264parse ! fakesink` |
| SHA-256 do artefato | igual ao publicado no descritor **e igual entre execuções** do mesmo material |
| Mesma extração em máquina distinta | hash idêntico dentro do container Debian e no host |
| `scripts/smoke_e2e.sh` (6 etapas) | `SMOKE OK` |
| `docker build --target smoke` | passa; o estágio roda o smoke contra a imagem final |
| ThreadSanitizer nas suítes do S4.1 | nenhuma condição de corrida |
| Daemon em container: `POST /api/v1/events` | `201` com DTO; SHA-256 do arquivo igual ao do JSON; `400` para `event_id`/janela inválidos; `500` para janela fora do buffer |
| Buffer após mais de 30 s (arena já deu a volta) | extração continua funcionando; segmento `/dev/shm` com tamanho fixo (756 000 B no demo) |
| `docker stop` com eventos chegando | encerramento limpo (exit 0), sem acesso à arena já desmapeada |

### Revalidação (29/09/2026, WSL Ubuntu 24.04, GCC 13.3, **com** GStreamer 1.24)

| Verificação | Resultado |
| :--- | :--- |
| Build com GStreamer instalado | **quebrava** (`Impl` privado usado pela callback do appsink) — corrigido |
| Captura real `videotestsrc → x264enc → appsink` (`test_s4_gstreamer_capture`) | quadros ingeridos sem rejeição; trecho extraído começa em keyframe e **decodifica** no `avdec_h264` |
| Stream com B-frames (replay de `.mp4` comum, como o `video_test/`) | antes: ~2 de cada 3 quadros rejeitados (PTS fora de ordem); agora o carimbo usa o DTS |
| `isTruncatedAtStart` com keyframe anterior já expulso | antes: `false` mesmo perdendo parte do "antes"; agora `true` |
| `stopCapture()` com extração esperando o pós-evento | wakeup perdido podia prender a extração até o timeout; corrigido |
| `ctest`, ASan + UBSan, ThreadSanitizer | tudo passando; nenhuma condição de corrida |

---

## 🗑️ S4.3 — Retenção & Expurgo LGPD: entrada → processamento → saída

Resumo; o detalhamento está em [`docs/S4.3-retencao-expurgo.md`](docs/S4.3-retencao-expurgo.md).

| | |
| :--- | :--- |
| **Recebe** | Ocupação do NVMe (`statvfs` no `--media-dir`) + clipes do SQLite com `created_at`, `is_retained` e `is_locked_for_audit` |
| **Faz** | Um daemon varre ao iniciar, a cada 1 h e **na hora** em que o disco passa de 85% (checagem a cada 30 s). Cada varredura aplica, em ordem, `LgpdRetentionStrategy` (idade ≥ 7 dias) e `DiskQuotaFifoStrategy` (mais antigo primeiro, até 80%). Mídia travada para auditoria nunca é apagada — a trava é relida logo antes de cada exclusão |
| **Entrega** | Arquivo removido do NVMe, `is_retained = false` no SQLite (a API passa a mostrar isso), uma linha por mídia em `s4_purge_log.jsonl` e, se o disco está cheio sem nada apagável, `DiskQuotaExceededError` (alerta `[S4.3] ALERT` no stderr) |

---

## 📐 Padrões GoF Implementados

1. **Repository**: [`IMediaClipRepository`](include/s4/domain/repositories/media_clip_repository.hpp) isola completamente as regras de domínio do SQLite.
2. **Strategy**: [`IRetentionStrategy`](include/s4/domain/strategies/retention_strategy.hpp) permite alternar dinamicamente os algoritmos de expurgo (prazo LGPD vs. cota de disco FIFO).
3. **Facade**: [`IRAMBufferFacade`](include/s4/infrastructure/gstreamer/ram_buffer_facade.hpp) oculta a complexidade de ponteiros e pipes do GStreamer — implementado por [`RingBufferRAMFacade`](include/s4/infrastructure/gstreamer/ring_buffer_ram_facade.hpp) (produção) e `MockRAMBufferFacade` (testes).
4. **Observer / Consumer**: [`B3EventConsumer`](include/s4/infrastructure/b3_bus/b3_event_consumer.hpp) assina eventos do barramento B3 e dispara os casos de uso.

---

## ⚙️ Pré-requisitos e Dependências

- **Compilador C++**: GCC 11+, Clang 13+ ou compatível com **C++20**
- **CMake**: Versão 3.20 ou superior
- **Bibliotecas do Sistema**:
  - `sqlite3` (persistência de metadados)
  - `OpenSSL` (cálculo de hash SHA-256)
  - `GStreamer 1.0` (replay de vídeo, demux, parse H.264 e mux para MP4).
    Em Debian/Ubuntu os pacotes necessários são `gstreamer1.0-dev`,
    `libgstreamer-plugins-base1.0-dev` (é ele que traz o `gstreamer-app-1.0.pc`),
    `gstreamer1.0-plugins-base`, `gstreamer1.0-plugins-good` e
    `gstreamer1.0-plugins-bad` (é onde mora o `h264parse`).
    Mais `gstreamer1.0-plugins-ugly` (é onde mora o `x264enc`) e
    `gstreamer1.0-libav` (é onde mora o `avdec_h264`): os dois não são
    necessários para compilar, e sem eles a suíte de captura real e a
    verificação de pixels do smoke saem como *Skipped* em vez de prova.
    O `cmake` **falha** se o GStreamer não for encontrado, em vez de compilar
    sem ele em silêncio; para desligar de propósito, `-DODS_S4_WITH_GSTREAMER=OFF`.

---

## 🔨 Compilação e Testes

### Compilando o Projeto

```bash
mkdir -p build && cd build
cmake ..
cmake --build .
```

### Executando Testes

Os testes de unidade seguem a convenção `deve_[resultado]_quando_[condicao]`. Testes de unidade usam zero I/O
(exceto o cálculo de hash); a integração cobre SQLite em arquivo temporário, extração com filesystem real e servidor HTTP.

```bash
ctest --output-on-failure
```

### Executando o Serviço (daemon demo)

```bash
./build/s4_media_evidence --port 8080 --db /tmp/s4.db --media-dir /tmp/s4-media
```

Sem `--source`, o ring buffer real do S4.1 em `/dev/shm/ods_s4_ring_<camera>` é alimentado por uma fonte
sintética (a Jetson e a câmera não estão presentes em toda máquina de desenvolvimento).

Com `--source`, o daemon **replaya um MP4 H.264 real** e extrai um clipe de vídeo de verdade — este é o caminho
que comprova o fluxo completo:

```bash
./build/s4_media_evidence \
  --source "video_test/10 Minutes of Amazon Rainforest (Free Download).mp4" \
  --scenario --port 8080 --db /tmp/s4.db --media-dir /tmp/s4-media
```

| Flag | Padrão | Para quê |
|---|---|---|
| `--port N` | `8080` | Porta HTTP; `0` deixa o sistema escolher (o daemon imprime a escolhida). |
| `--db PATH` | `/tmp/s4_media_evidence.db` | Banco SQLite dos descritores. |
| `--media-dir PATH` | `/tmp/s4_media_evidence` | Onde os clipes são gravados. |
| `--source FILE` | — | Replay de um MP4 H.264 existente, em vez da fonte sintética. |
| `--scenario` | desligado | Na subida, espera o ring buffer ter material e extrai um clipe de demonstração, imprimindo o descritor. Só produz MP4 com `--source`: a fonte sintética alimenta bytes que não são H.264 codificado, e o muxer avisa isso em vez de gravar um arquivo que só parece vídeo. |
| `--camera ID` | `cam0` | Identificador da câmera (vira o nome do segmento em `/dev/shm`). |
| `--window-seconds N` | `30` | Segundos de ring buffer. |
| `--bitrate BPS` | `134400` | Bitrate nominal, usado para dimensionar o ring buffer. |

O daemon inicia o ring buffer e expõe a API:

```bash
curl http://localhost:8080/api/v1/clips/<clip_id>
sha256sum /tmp/s4-media/event-demo.mp4   # deve bater com "sha256_hash" do JSON
ls -lh /dev/shm/ods_s4_ring_cam0         # o buffer circular, enquanto o daemon roda
```

O daemon também sobe o expurgo do S4.3. Parâmetros opcionais (padrões do guia):

```bash
./s4_media_evidence ... --retention-days 7 --quota-trigger 0.85 --quota-target 0.80 \
    --sweep-interval 3600 --disk-check-interval 30 --purge-log /tmp/s4_purge_log.jsonl
```

### Simulando uma nova entrada de evento

Para injetar um evento em runtime (sem reiniciar o daemon), use `POST /api/v1/events`. Cada chamada extrai um novo
clipe do buffer, persiste em `$media_dir/<event_id>.mp4` e responde `201` com o DTO do clipe:

```bash
curl -X POST http://127.0.0.1:8080/api/v1/events \
  -H 'Content-Type: application/json' \
  -d '{"event_id": "evt-2026-0001"}'

# janela explícita em epoch milliseconds (opcional; default = últimos 1s)
curl -X POST http://127.0.0.1:8080/api/v1/events \
  -H 'Content-Type: application/json' \
  -d '{"event_id": "evt-2026-0002", "start_ms": 1720000000000, "end_ms": 1720000001000}'
```

**Por relógio de captura** (caminho do S4.4; recomendado). O instante do evento tem que estar no intervalo que
o buffer realmente guarda:

```bash
TS=$(curl -s http://127.0.0.1:8080/api/v1/buffer/stats | jq -r .newest_capture_ts_ns)

curl -X POST http://127.0.0.1:8080/api/v1/events \
  -H 'Content-Type: application/json' \
  -d "{\"event_id\":\"evt-2026-0003\",\"capture_ts_ns\":$TS,\"pre_window_seconds\":1.0,\"post_window_seconds\":0.5}"
```

- `event_id` ausente → gerado automaticamente (`event-<uuid>`); deve conter apenas `[A-Za-z0-9_-]`.
- Janela inválida (`start_ms >= end_ms`) → `400`; janela sem frames no buffer → `500`.

O estado atual do ring buffer, incluindo o intervalo do relógio de captura, fica em
`GET /api/v1/buffer/stats` — é o que permite escolher um `capture_ts_ns` válido:

```bash
curl -s http://127.0.0.1:8080/api/v1/buffer/stats | jq
```

| Rota | Para quê |
|---|---|
| `GET /healthz` | Sonda de saúde do serviço. |
| `GET /api/v1/buffer/stats` | Quadros guardados, bytes, taxa de uso, sessão e intervalo do relógio de captura. |
| `POST /api/v1/events` | Dispara um evento e extrai o clipe; responde `201` com o DTO. |
| `GET /api/v1/clips/{id}` | Descritor do clipe, incluindo o SHA-256. |

> **Nota de build:** o modo `Release` define `NDEBUG` e remove todos os `assert()`. Os testes do S4.1 usam
> `ODS_CHECK` ([`tests/ods_check.hpp`](tests/ods_check.hpp)), que vale em qualquer modo de build.

O plano de implementação original está em
[`docs/PLANO_IMPLEMENTACAO_S4.md`](docs/PLANO_IMPLEMENTACAO_S4.md); o que
ele listava como backlog já foi implementado — S4.1 em
[`docs/S4.1-ring-buffer.md`](docs/S4.1-ring-buffer.md) e S4.3 em
[`docs/S4.3-retencao-expurgo.md`](docs/S4.3-retencao-expurgo.md).

---

## 🐳 Containerização (Docker)

Imagem OCI **multi-arch** (`debian:bookworm-slim`, multi-stage): o estágio `builder` compila em C++20 e executa o
`ctest` completo (falha de teste quebra o build), e o estágio `runtime` final contém apenas as bibliotecas em tempo de
execução e o binário, rodando como usuário não-root `s4` com `VOLUME /data`.
# Guia de Desenvolvimento e Execução - Kit B6 (Módulo MOD-2)

Este repositório contém a implementação dos submódulos de interface **B6.1 (OverlayPlayer)** e **B6.2 (ZoneEditor)**, responsáveis pela renderização gráfica acelerada e edição topológica vetorial do ecossistema ODS.

## 🛠️ Pré-requisitos e Configuração Inicial

Para garantir que o ambiente local executa corretamente tanto o servidor de testes visuais (HTML) quanto a suíte de testes automatizados (TDD), é necessário preparar o ecossistema Node.js.

Na raiz do seu diretório (`~/MOD2_ODS/mod2`), execute os seguintes comandos:

1. **Inicializar o pacote NPM** (caso não exista um `package.json`):
   ```bash
   npm init -y
   ```

2. **Instalar as dependências de desenvolvimento**:
   ```bash
   npm install -D vite vitest jsdom
   ```
   * **vite:** Servidor de desenvolvimento rápido para processar o TypeScript e servir os ficheiros `index.html`.
   * **vitest & jsdom:** Framework de testes unitários e simulador de navegador para garantir a validação de regras de domínio (Zero I/O) sem depender da interface gráfica real.

---

## 🖥️ Executar os Testes Visuais (HTML)

Os ficheiros HTML permitem testar a interação física (cliques do rato, renderização a 60 FPS e sobreposição de vídeo) no navegador.

### B6.1 - OverlayPlayer (Visualizador e Sincronização por Intervalos)
1. Certifique-se de que o ficheiro `index_player.html` está configurado para importar o `b6_overlay_player.ts`.
2. Inicie o servidor Vite:
   ```bash
   npx vite
   ```
3. Abra o link fornecido na consola (normalmente `http://localhost:5173/index_player.html`).
4. **O que testar:** O player reproduz o clipe utilizando controlos customizados encapsulados (Play/Pause, barra de progresso temporal, volume e ecrã inteiro). As caixas de deteção e rótulos acompanham de forma fluida os **intervalos temporais** (`startTime` e `endTime`) injetados via DTO através do método `loadMetadataTimeline()`.

### B6.2 - ZoneEditor (Editor Interativo)
1. Certifique-se de que o ficheiro `index.html` está configurado com as importações do `b6_zone_editor.ts`.
2. Inicie o servidor Vite:
   ```bash
   npx vite
   ```
3. Abra a interface no navegador.
4. **O que testar (UX/UI):**
   * **Criar:** Clique esquerdo para desenhar pontos e clique direito (numa área vazia) para fechar o polígono.
   * **Mover:** Clique esquerdo e arraste para mover um ponto ou a zona inteira.
   * **Adicionar:** Passe o rato na linha da aresta (ficará branca), clique esquerdo e puxe para criar um novo vértice.
   * **Apagar:** Clique direito sobre um ponto ou sobre o corpo da zona para excluí-los (com validação anti-cruzamento).

---

## 🧪 Executar a Suíte de Testes Unitários (TDD)

Os testes automatizados cobrem as regras críticas de domínio, o motor de validação topológica (`PolygonValidator`) e as exceções da arquitetura. 

Para correr os testes de **ambos os módulos simultaneamente**, execute:

```bash
# Build (testes rodam dentro do builder)
docker build -t ods/s4-media-evidence:0.1.0 ./mod2/s4-media-evidence

# Executar (dados persistidos em volume nomeado)
docker run -d --rm --name s4 -p 8080:8080 -v s4_data:/data ods/s4-media-evidence:0.1.0
curl http://127.0.0.1:8080/healthz                # {"status": "ok"}

# Ou via docker compose (healthcheck + porta + volume)
docker compose -f mod2/s4-media-evidence/docker-compose.yml up -d --build
```

Há ainda um estágio `smoke`, que **não** faz parte da imagem final: ele roda
`scripts/smoke_e2e.sh` contra a imagem de runtime e falha o build se algo estiver errado — desde o container
decodificar o MP4 extraído até exigir que duas execuções produzam o mesmo SHA-256. As ferramentas de verificação
(`jq`, `gstreamer1.0-tools`) ficam só nesse estágio, para não inflar a imagem de produção.

```bash
# Verificação de ponta a ponta contra a imagem construída
docker build --target smoke -t ods/s4-media-evidence:smoke ./mod2/s4-media-evidence
```

O mesmo script roda direto na máquina, sem Docker:

```bash
./scripts/smoke_e2e.sh ./build/s4_media_evidence
```

`curl`, `jq` e `sha256sum` são obrigatórios — sem eles o script sai com erro `2`
dizendo o que falta, em vez de morrer no meio sob `set -e`. `gst-launch-1.0` e um
decoder H.264 são opcionais: o script avisa "pula" e continua, porque container
aceito pelo `h264parse` e pixels decodificados são evidências diferentes.

Detalhes, verificação e instruções para a **Jetson Orin Nano (arm64)** — build nativo na própria Jetson
ou alternativa com `buildx`/`binfmt` — estão em
[`docs/PLANO_CONTAINERIZACAO_S4.md`](docs/PLANO_CONTAINERIZACAO_S4.md).

---

## 📋 Checklist de Conformidade

Antes de submeter código ou PR:
- [x] **Clean Architecture**: Domínio não possui dependências de bibliotecas de terceiros ou frameworks.
- [x] **Sem Vídeo no Barramento**: Apenas metadados e DTOs trafegam nas APIs/mensageria.
- [x] **Nomenclatura Limpa**: Código autoexplicativo e funções focadas (SRP).
- [x] **Tratamento de Exceções**: Uso de exceções tipadas de `DomainError` e `ApplicationError`.
- [x] **Conformidade LGPD**: Entidade `MediaClip` possui controle de tempo de retenção e flag de trava de auditoria (`is_locked_for_audit`).
- [x] **Testes de Unidade**: Suíte `deve_..._quando_...` passando (unit + integration).
- [x] **S4.3 — Expurgo LGPD**: prazo de 7 dias + cota FIFO a 85% com trava de auditoria, daemon ligado no `main.cpp` (`test_s4_retention`, `test_s4_retention_runtime`).

npx vitest
```

### O que cada teste valida:

* **`b6_overlay_player.test.ts` (B6.1):**
  * Valida o lançamento de exceções rigorosas caso o contentor alvo não seja localizado no DOM.
  * Verifica a correta montagem estrutural do player e a ordenação cronológica de segurança das timelines baseadas em **intervalos temporais** (`startTime` e `endTime`).

* **`b6_zone_editor.test.ts` (B6.2):**
  * **Testes de Domínio:** Valida matematicamente que polígonos cruzados (em forma de "gravata") são rejeitados pelo sistema antes da exportação.
  * **Testes de Estado:** Utilizando fakes de DOM em memória (`jsdom`), garante que sequências complexas de cliques criam, movem e excluem dados de forma segura, respeitando sempre o formato de saída normalizado `[0.0, 1.0]`.

### Dica de Desenvolvimento
Para manter os testes a correr continuamente em segundo plano (Watch Mode) enquanto escreve código, basta deixar o comando `npx vitest` em execução no terminal. O sistema irá relatar falhas e sucessos em tempo real a cada ficheiro guardado.