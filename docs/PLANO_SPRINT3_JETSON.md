# Plano da Sprint 3 — S4 na Jetson (sem nuvem)

> Projeto ODS 2026/2 — Squad MOD-2 · Componente S4 (Camada 3 — Serviço)
> Alvo: **NVIDIA Jetson Orin Nano (ARM64)** · Deploy: **Docker / Docker Compose**
> Este documento substitui a frente de armazenamento em nuvem descrita no
> [`COMPLEMENTO_GUIA_SPRINT3.md`](referencia/COMPLEMENTO_GUIA_SPRINT3.md).

## 1. Mudança de plano

A Sprint 3 **não usa mais armazenamento em nuvem (GCS)**. Toda a mídia
probatória fica no **SSD NVMe da Jetson**, e o serviço roda na **borda** via
Docker. O barramento B3 continua baseado em Kafka, mas **o broker é operado por
outra equipe**; o S4 apenas consome a configuração fornecida pela integração.

| Tema | Antes (nuvem) | Agora (Jetson) |
| :--- | :--- | :--- |
| Mídia | GCS + URL assinada | NVMe local (`FileStorage`) |
| Metadados | SQLite local | SQLite local (sem mudança) |
| Kafka | Broker de nuvem fixo no código | Broker padrão do guia, configurável por env/flag |
| Deploy | Imagem em nuvem | Docker Compose ARM64 na Jetson |
| Retenção | Expurgo no bucket | `PurgeDaemon` + `statvfs` no NVMe (já implementado) |

O código de GCS **nunca existiu neste repositório**; nenhuma remoção de feature é
necessária. A mudança é de planejamento, configuração e remoção de endereço de
nuvem embutido.

## 2. Objetivo da sprint

Entregar o S4 demonstrável na Jetson, integrado ao barramento B3 (fornecido por
TRA-1) e às interfaces homologadas por TRA-2, com armazenamento local e
conformidade LGPD.

## 3. Decisões

| Decisão | Escolha | Justificativa |
| :--- | :--- | :--- |
| Armazenamento | NVMe local via `FileStorage` | Simplicidade operacional na borda; retenção LGPD já implementada |
| Volume | Bind mount `${ODS_S4_DATA_DIR}:/data` | `statvfs` mede o NVMe real (cota FIFO de 85% do S4.3) |
| Kafka | Padrão do guia, sobrescrito por `ODS_S4_KAFKA_*` ou flags | Broker é operado pela equipe de integração |
| Sem broker | `--kafka-disabled`: S4 sobe e atende somente HTTP | Critério: software rodando mesmo com provedor indisponível |
| Deploy | Docker Compose arm64 nativo na Jetson | Mantém a containerização já validada |
| Demonstração | Pelo IP da Jetson, nunca `localhost` | Critério explícito da Sprint 3 |
| Segurança | Credenciais só em env/.env (fora do git); sem log de segredo | Service account/tokens não pertencem ao repositório |

## 4. Configuração do S4

### 4.1 Armazenamento (NVMe)

```bash
ODS_S4_DATA_DIR=/mnt/nvme/s4   # contém s4.db, media/, s4_purge_log.jsonl
```

O container grava em `/data`; o bind mount garante que o `statvfs` enxergue o
volume NVMe que hospeda as mídias.

### 4.2 Kafka (fornecido pela integração)

| Variável | Uso |
| :--- | :--- |
| `ODS_S4_KAFKA_BROKERS` | Lista de brokers (`host:porta,...`). Padrão: `34.66.235.91:9093`. |
| `ODS_S4_KAFKA_GROUP_ID` | `group.id` do consumidor (padrão `s4-media-evidence`). |
| `ODS_S4_KAFKA_CLIENT_ID` | `client.id` (padrão `s4`). |
| `ODS_S4_KAFKA_SECURITY_PROTOCOL` | Opcional, ex.: `SASL_SSL`. |
| `ODS_S4_KAFKA_SASL_MECHANISM` | Opcional, ex.: `SCRAM-SHA-512`. |
| `ODS_S4_KAFKA_SASL_USERNAME` | Opcional; nunca logado. |
| `ODS_S4_KAFKA_SASL_PASSWORD` | Opcional; nunca logado. |
| `ODS_S4_KAFKA_SSL_CA_LOCATION` | Opcional, caminho do CA no container. |

As flags `--kafka-brokers`, `--kafka-group-id` e `--kafka-client-id` têm
precedência sobre o ambiente. `--kafka-disabled` força a operação sem Kafka.

Tópicos já tratados pelo S4:

- consome `ods.s2.event`, `ods.session.started`, `ods.s4.capture.command`,
  `ods.s4.clip.query`;
- publica `ods.s4.clip` e replies de command/query.

## 5. Trabalhos (WIs)

| WI | Entregável | Estado |
| :--- | :--- | :--- |
| J1 | Restaurar o broker padrão do guia em `main.cpp`, header, Dockerfile e compose | Concluído |
| J2 | Configuração Kafka por env/flag, com `--kafka-disabled` gracioso | Concluído |
| J3 | Repasse opcional de SASL/TLS sem logar segredos | Concluído |
| J4 | Compose com bind mount NVMe e `.env.example` | Concluído |
| J5 | Testes unitários da configuração do Kafka | Concluído |
| J6 | Build ARM64 e execução na Jetson | Aguardando acesso à placa/IP |
| J7 | E2E com broker real da equipe de integração | Aguardando broker da TRA-1 |
| J8 | Homologação das interfaces pela TRA-2 | Aguardando integração |

## 6. Cronograma

| Fase | Atividade | Entregável |
| :--- | :--- | :--- |
| 1 | Configuração e testes locais | `ctest` verde no host (x86_64) |
| 2 | Build/imagem na Jetson | `docker compose build` arm64 |
| 3 | Subida sem Kafka (`--kafka-disabled`) | `/healthz` + `POST /api/v1/events` + clipe no NVMe |
| 4 | Integração B3 | Consumo/produção nos tópicos com broker da equipe |
| 5 | Demonstração | Evidência acessada pelo IP da Jetson |

## 7. Verificação (Definition of Done)

### Host de desenvolvimento

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Os testes de GStreamer/Kafka podem sair como *Skipped* quando a lib não está
instalada no host; na imagem Docker (Jetson) eles são exigidos pelo build.

### Jetson (quando houver IP/acesso)

```bash
cp .env.example .env        # preencher ODS_S4_DATA_DIR e broker da TRA-1
docker compose build
docker compose up -d
curl http://<IP_DA_JETSON>:8080/healthz
curl -X POST http://<IP_DA_JETSON>:8080/api/v1/events \
  -H 'Content-Type: application/json' -d '{"event_id":"evt-demo"}'
curl http://<IP_DA_JETSON>:8080/api/v1/clips/<clip_id>
sha256sum "$ODS_S4_DATA_DIR"/media/evt-demo.mp4
```

Critérios de aceite:

- serviço sobe com `--kafka-disabled` e continua servindo HTTP;
- com o broker padrão/configurado, consome `ods.s2.event` e publica `ods.s4.clip`;
- clipe e SQLite persistidos no NVMe;
- expurgo LGPD/85% continua ativo sobre o volume real;
- demonstração feita pelo IP da Jetson, nunca por `localhost`.

## 8. Aderência aos critérios da Sprint 3 (`docs/criterios.txt`)

| Critério | Como é atendido |
| :--- | :--- |
| Requisitos funcionais do planejamento | S4.1–S4.4 e integração B3 mantidos |
| Requisitos não funcionais da Jetson | Build/execução ARM64, `/dev/shm`, NVMe e medição de encoder |
| Hospedagem em nuvem | Fora de escopo por mudança de plano, registrada neste documento |
| Software rodando ao fim da sprint | Sobe com `--kafka-disabled`; HTTP sempre disponível |
| Borda, nunca localhost | Deploy e demonstração pelo IP da Jetson |
| Integração pub/sub (TRA-1) | Tópicos e envelope B1 mantidos; broker padrão e configurável |
| Interfaces aprovadas (TRA-2) | REST `/healthz`, `/api/v1/events`, `/api/v1/clips/{id}` estáveis |
| Bônus de testes unitários | Suíte existente + novos testes de configuração do Kafka |
| Qualidade do planejamento | Este documento e atualização do README/plano de containerização |
| Segurança da informação | Credenciais só em env; segredos não logados; sem IP fixo |

## 9. Riscos e pendências

- **IP da Jetson ainda não definido** — toda a documentação usa
  `<IP_DA_JETSON>`/variáveis de ambiente; nada fica hardcoded.
- **Broker da TRA-1** pode exigir SASL/TLS; o repasse por env já está pronto,
  mas falta validar com o broker real.
- **Permissão do bind mount**: o usuário `s4` (uid 1000) precisa escrever em
  `ODS_S4_DATA_DIR`; ajustar dono/permissão no NVMe antes da subida.
- **Build arm64**: validar `librdkafka`, GStreamer e `x264enc` na placa.
- **Desempenho**: rodar `s4_measure_encoder` na Jetson com a inferência ligada
  para registrar CPU/RAM reais.
