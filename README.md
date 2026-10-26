# S4 — Evidência de Mídia (Media Evidence)

> Componente da **Camada 3 (Serviço)** do Squad **MOD-2** (Mídia, Métricas e Interfaces) no projeto **ODS 2026/2**.

> **Este repositório é o repositório definitivo do componente S4.** A raiz do repositório
> **é** o componente: o código, o build (CMake/Docker), a suíte de testes e a documentação
> do S4 estão aqui. Não há mais a pasta `mod2/s4-media-evidence/` — todos os comandos deste
> README rodam a partir da raiz. Os demais componentes do squad (S5, B6) vivem em seus
> próprios repositórios.

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
<raiz do repositório>/
├── CMakeLists.txt              # Configuração de build C++20 e dependências
├── README.md                   # Documentação do componente
├── Dockerfile                  # Imagem OCI multi-arch (builder + runtime)
├── docker-compose.yml          # Orquestração local (porta 8080 + volume)
├── .dockerignore               # Exclusões do contexto de build
├── docs/                       # Documentação: guias do componente + guias do squad ODS
├── scenarios/                  # Cenário gravado (.idx/.bin) usado no replay
├── video_test/                 # MP4 H.264 de teste (matéria-prima do replay E2E)
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
│   ├── infrastructure/         # hashing, database, filesystem, gstreamer
│   └── main.cpp                # Daemon demo executável
├── tools/                      # record_scenario: grava um cenário em .idx/.bin
│                               # measure_encoder: CPU do x264enc e RAM do buffer (placa)
├── scripts/smoke_e2e.sh        # Verificação E2E de ponta a ponta
└── tests/                      # Suíte de testes automatizados
    ├── unit/                   # Testes puros (domínio, casamento de uso, hash, ISO-BMFF)
    └── integration/            # Testes com SQLite, filesystem real, HTTP e MP4 real
```

### Documentação

| Documento | Para quê |
| :--- | :--- |
| [`docs/S4.4-demo-replay-e2e.md`](docs/S4.4-demo-replay-e2e.md) | **Guia de demonstração**: mostrar o fluxo do MP4 real ao artefato indexado, passo a passo. |
| [`docs/S4.1-ring-buffer.md`](docs/S4.1-ring-buffer.md) | Detalhes do ring buffer: arena, evicção, ponte de relógios; seção 12 = pendências da 6c (encoder, P4, `captured_at`, sessão, profundidade, `OUT_OF_BUFFER`). |

---

## 🧩 Submódulos e Responsabilidades (Squad MOD-2)

Conforme definido em [`docs/responsability.md`](docs/responsability.md):

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
| **Quadros de vídeo (6c)** | P4 ao vivo / P6 em replay: quadro **cru** (NV12) + metadados `ods.p4.frame`; o S4 codifica com `x264enc` (a Orin Nano não tem NVENC) | `P4RawFrameEncoderSource::submit(metadata, bytes)`, ou NV12 via `shmsink` (`p4RawShmPipeline`) |
| **Quadros de vídeo (demais)** | Replay de `.mp4`, cenário gravado ou fonte sintética (demo/testes) | `CapturedFrame { captureTsNs, data, length, isKeyframe, sessionId, timeBase }` — H.264 Annex-B, 1 quadro por chamada |
| **Marcador de sessão** | `ods.session.started`, entregue pelo consumidor B3 (S4.2) | `onSessionStarted(SessionStarted)`: zera o buffer em SEEK/LOOP/RESTART |
| **Pedido de trecho** | S4.2 (`ExtractClipUseCase`), disparado pelo `POST /api/v1/events` ou pelo barramento B3 | `readWindow(start, end)` em relógio de parede, ou `extractAround(tsEvento, preSeg, posSeg, timeout)` em relógio de captura (com `captured_at` de P4, é o próprio UTC) |
| **Configuração** | Build da aplicação | `RingBufferConfig { cameraId, windowSeconds, bitrateBps, safetyFactor, width, height, replaceStaleSegment, maxPostSeconds, depthRequirement }` |

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
| `std::vector<std::vector<uint8_t>>` (`readWindow`) | S4.2, via porta `IMediaBufferReader` | Payloads H.264 da janela, começando em keyframe. **Lista vazia** se a janela não está no buffer ou se a captura já foi parada (o S4.2 converte em `MediaBufferEmptyError` → HTTP 500). Contrato legado, mantido. |
| `OutOfBufferError` (`extractCaptureWindow`) | S4.2 / S4.4 | Código `OUT_OF_BUFFER` da 6c: janela já expulsa, fim além do `maxPostSeconds`, ou sessão trocada durante a espera. Recusado **antes** de esperar; HTTP **422** `{"status":"ERROR","error":{"code":"OUT_OF_BUFFER",…}}` |
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
| `ctest` (15 suítes) | 15/15, estáveis em execuções repetidas. Três podem sair **Skipped** (`77`) em vez de aprovadas: `test_s4_replay_e2e`, sem MP4 em `video_test/`, e `test_s4_gstreamer_capture`/`test_s4_p4_input`, sem `x264enc`/`avdec_h264`. Skipped é o resultado honesto para "não executado"; para reprovar nesses casos, `-DODS_S4_REQUIRE_SAMPLE_VIDEO=ON` |
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

### Revalidação (21/10/2026, Fedora 44, GCC 15, **com** GStreamer 1.24 no container)

Reestruturação do repositório: a raiz passou a ser o próprio componente S4 (antes `mod2/s4-media-evidence/`).

| Verificação | Resultado |
| :--- | :--- |
| Build local (`cmake -S . -B build`) | passa; sem os headers do GStreamer na máquina, só com `-DODS_S4_WITH_GSTREAMER=OFF` — e o `cmake` **falha** em vez de degradar em silêncio |
| `ctest` local (13 suítes) | 11 aprovadas, 2 **Skipped** (as de MP4 real, que exigem GStreamer) |
| `ctest` dentro do `docker build` (com GStreamer) | **13/13 aprovadas** — inclusive `test_s4_replay_e2e` e `test_s4_gstreamer_capture` |
| `docker build --target smoke` | `SMOKE OK` nas 6 etapas, com pixels decodificados por `avdec_h264` e hash estável entre execuções |
| `docker compose up -d --build` | serviço `healthy`; `GET /healthz` → `{"status": "ok"}`; `GET /api/v1/buffer/stats` devolve a arena em uso |
| Alvo padrão do `docker build` | **corrigido**: sem o estágio final `FROM runtime AS production`, a tag `0.1.0` era a imagem do `smoke` (com `jq` e `gstreamer1.0-tools` dentro) — o `docker compose` subia o serviço a partir dela |

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
    Em Fedora/RHEL os equivalentes são `gstreamer1-devel`,
    `gstreamer1-plugins-base-devel` (é ele que traz o `gstreamer-app-1.0.pc`),
    `gstreamer1-plugins-base`, `gstreamer1-plugins-good`,
    `gstreamer1-plugins-bad`, `gstreamer1-plugins-ugly` e `gstreamer1-libav`.
    O `cmake` **falha** se o GStreamer não for encontrado, em vez de compilar
    sem ele em silêncio; para desligar de propósito, `-DODS_S4_WITH_GSTREAMER=OFF`.

Installação completa em Fedora:

```bash
sudo dnf install -y gcc-c++ cmake pkgconf-pkg-config sqlite-devel openssl-devel \
     libuuid-devel gstreamer1-devel gstreamer1-plugins-base-devel \
     gstreamer1-plugins-base gstreamer1-plugins-good gstreamer1-plugins-bad \
     gstreamer1-plugins-ugly gstreamer1-libav
```

---

## 🔨 Compilação e Testes

Todos os comandos abaixo rodam **na raiz do repositório**.

### Compilando o Projeto

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

O `cmake` **falha** se o GStreamer não for encontrado, em vez de compilar sem captura de vídeo em silêncio
(`-DODS_S4_WITH_GSTREAMER=OFF` desliga isso de propósito, e a suíte sai com os cenários de MP4 real como
*Skipped*). Para reprovar quando o MP4 de teste não está em `video_test/`, use `-DODS_S4_REQUIRE_SAMPLE_VIDEO=ON`.

### Executando Testes

Os testes de unidade seguem a convenção `deve_[resultado]_quando_[condicao]`. Testes de unidade usam zero I/O
(exceto o cálculo de hash); a integração cobre SQLite em arquivo temporário, extração com filesystem real e servidor HTTP.

```bash
ctest --test-dir build --output-on-failure
```

São 15 suítes. Três podem sair **Skipped** (`77`) em vez de aprovadas: `test_s4_replay_e2e`, sem MP4 em
`video_test/` ou sem GStreamer, e `test_s4_gstreamer_capture` e `test_s4_p4_input`, sem `x264enc`/`avdec_h264`. Skipped é o resultado
honesto para "não executado"; para reprovar nesses casos, `-DODS_S4_REQUIRE_SAMPLE_VIDEO=ON`.

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
| `--p4-fake` | desligado | S4.1/6c: P4 falso em processo (NV12 + `ods.p4.frame` + `ods.session.started`), codificado pelo `x264enc`. Produz MP4 de verdade com `captured_at` e sessão oficiais. |
| `--p4-shm SOCKET` | — | S4.1/6c: NV12 cru de P4 via `shmsink`, codificado pelo `x264enc`; a sessão vem do marcador. |
| `--width/--height/--fps` | `1280`/`720`/`30` | Formato combinado com P4 nos modos `--p4-*`. |
| `--encoder-preset NAME` | `ultrafast` | Preset do `x264enc` (`ultrafast`…`fast`). |
| `--encoder-bitrate BPS` | `4000000` | Bitrate do `x264enc`; dimensiona o buffer nos modos `--p4-*`. |
| `--gop N` / `--encoder-threads N` | `30` / `0` | Quadros entre keyframes; threads do x264 (`0` = automático). |
| `--pre-roll-seconds N` | `5` | Pré-rolagem dos clipes (profundidade, item 5). |
| `--max-rule-persistence-seconds N` | `8` | Maior `min_duration_ms + debounce_ms` entre as regras da build. |
| `--event-latency-seconds N` | `2` | Folga de entrega do evento S2 → S4. |
| `--max-post-seconds N` | `10` | `post_s` máximo; acima disso, `OUT_OF_BUFFER`. |
| `--scenario` | desligado | Na subida, espera o ring buffer ter material e extrai um clipe de demonstração, imprimindo o descritor. Só produz MP4 com `--source`: a fonte sintética alimenta bytes que não são H.264 codificado, e o muxer avisa isso em vez de gravar um arquivo que só parece vídeo. |
| `--camera ID` | `cam0` | Identificador da câmera (vira o nome do segmento em `/dev/shm`). |
| `--window-seconds N` | maior entre `30` e a profundidade exigida | Segundos de ring buffer. Explícito e menor que pré-rolagem + persistência + latência + post_s + GOP: o daemon recusa subir. |
| `--bitrate BPS` | `134400` | Bitrate nominal, usado para dimensionar o ring buffer em `--source`/sintética. |

O daemon inicia o ring buffer e expõe a API:

```bash
curl http://localhost:8080/api/v1/clips/<clip_id>
sha256sum /tmp/s4-media/event-demo.mp4   # deve bater com "sha256_hash" do JSON
ls -lh /dev/shm/ods_s4_ring_cam0         # o buffer circular, enquanto o daemon roda
```

O daemon também sobe o expurgo do S4.3. Parâmetros opcionais (padrões do guia):

```bash
./build/s4_media_evidence ... --retention-days 7 --quota-trigger 0.85 --quota-target 0.80 \
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
- Janela inválida (`start_ms >= end_ms`) → `400`; janela sem frames no buffer → `500` (caminho legado, `start_ms`/`end_ms`).
- Pelo relógio de captura (`capture_ts_ns`): janela já expulsa do buffer ou com fim além do `post_s` máximo → `422` com `{"status":"ERROR","error":{"code":"OUT_OF_BUFFER","message":…}}` (6c).

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

### Construindo a imagem

O contexto de build é a **raiz deste repositório** — o `Dockerfile` copia `CMakeLists.txt`, `include/`, `src/`,
`tests/`, `tools/`, `third_party/`, `scenarios/` e `video_test/` a partir da raiz.

```bash
# Build (os testes rodam dentro do builder; falha de teste quebra a imagem)
docker build -t ods/s4-media-evidence:0.1.0 .

# Executar (dados persistidos em volume nomeado)
docker run -d --rm --name s4 -p 8080:8080 -v s4_data:/data ods/s4-media-evidence:0.1.0
curl http://127.0.0.1:8080/healthz                # {"status": "ok"}

# Ou via docker compose (healthcheck + porta + volume)
docker compose up -d --build
```

> **O alvo padrão do build é a imagem de produção (`runtime`).** O `docker build` sem `--target`
> usa o último estágio do Dockerfile, e o Dockerfile termina em `FROM runtime AS production`
> justamente para que a tag `ods/s4-media-evidence:0.1.0` **não** seja a imagem de validação:
> o estágio `smoke` traz `jq`, `gstreamer1.0-tools` e `avdec_h264` dentro, e uma imagem de
> produção com ferramentas de teste dentro não é a imagem de produção.

Há ainda o estágio `smoke`, que **não** faz parte da imagem final: ele roda
`scripts/smoke_e2e.sh` contra a imagem de runtime e falha o build se algo estiver errado — desde o container
decodificar o MP4 extraído até exigir que duas execuções produzam o mesmo SHA-256. As ferramentas de verificação
(`jq`, `gstreamer1.0-tools`) ficam só nesse estágio, para não inflar a imagem de produção.

```bash
# Verificação de ponta a ponta contra a imagem construída
docker build --target smoke -t ods/s4-media-evidence:smoke .
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
