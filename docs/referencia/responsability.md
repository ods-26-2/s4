9/12/26, 3:58 PM
Guia Completo de Desenvolvimento de Software — Squad MOD-2
Projeto: ODS 2026/2 — Oficina de Desenvolvimento de Software
Squad: MOD-2 (Mídia, Métricas e Interfaces)
Componentes: S4 (Evidência de Mídia), S5 (Métricas e Séries Temporais), B6 (Kit de Interface)
Alvos: NVIDIA Jetson Orin Nano (8 GB RAM) | Barramento B3 | Aplicações A1–A8
Diretrizes: Clean Code, Design Patterns (GoF), Domain-Driven Design (DDD) & Clean Architecture
1. Visão Geral e Responsabilidades do Squad MOD-2
O Squad MOD-2 é responsável por três grandes pilares do ecossistema ODS 2026/2:
S4 (Evidência de Mídia — Camada 3): Serviço responsável por gravar contínuamente em buffer de memória, salvar recortes de vídeo
probatórios vinculados a eventos, controlar o armazenamento físico e garantir o expurgo automático por LGPD ou cota de disco.
S5 (Métricas e Séries Temporais — Camada 3): Serviço responsável por transformar eventos brutos ruidosos em dados agregados
no tempo (ocupação, permanência, horários de pico) e no espaço (mapas de calor em coordenadas de mundo via I3).
B6 (Kit de Interface e Operação — Camada 4): Biblioteca de pacotes de interface reutilizáveis ( OverlayPlayer , ZoneEditor ,
AlertConsole , AppShell ) embutida diretamente na build de cada aplicação (A1–A8).
2. Quadro Executivo de Módulos (Entradas, Saídas, Arquitetura e Atribuições)
A tabela a seguir consolida as funcionalidades, entradas esperadas, estruturas de dados de saída, arquitetura interna e as
responsabilidades diretas dos membros do squad:
Componente / Funcionalidades
Módulo
ListadasEntradas Esperadas (Inputs)
S4.1 — Ring
Buffer
ContínuoStream H.264/RAW via
GStreamer (RAM
compartilhada /dev/shm ).
Captura contínua
de vídeo em RAM;
manutenção de
file:///C:/Users/davig/AppData/Local/Temp/ods_guia_tmp.html
Saídas & Estrutura de
Dados (Outputs)Arquitetura Interna do
Módulo
Buffer circular em memória
com ponteiros de framesFacade + Ring Buffer de
RAM (POSIX Shared
Memory C++/Python).
Responsável
Henrique
Azevedo
1/79/12/26, 3:58 PM
janela circular pré-
evento ($N$s).
S4.2 —
Binding
Evento-Mídia
(timestamp, pointer,
length) .
Mapeamento
evento-mídia;
recorte pré/pós
evento; gravação
de MP4 e hash
SHA256.Envelope Evento B1/B3
(JSON) + solicitação de
extração da janela temporal.
S4.3 —
Retenção &
Expurgo
LGPDExpurgo
automático após 7
dias (LGPD);
expurgo
emergencial FIFO a
85% do NVMe;
trava de auditoria.Estado de ocupação do
NVMe ( statvfs ) +
sinalizador
S4.4 — API
Descritores
de Clipe
Arquivo .mp4 no NVMe +
Registro SQLite:
Domain Command
Handler + Encoder
NVENC / Pipeline
GStreamer.Davi Gomes
Daemon Worker +
Strategy Pattern
( RetentionStrategy ).Henrique
Azevedo
SQLite.Log de Expurgo +
Liberação de Espaço no
NVMe + Update SQLite
( is_retained =
false ).Exposição de
metadados de
mídia via
REST/gRPC;
registro imutável de
acesso RBAC.Requisição HTTP GETDTO JSON: {clip_id,file_uri, start_time,REST/gRPC Controller +
Clean Architecture
Presentation Layer.Davi Gomes
/api/v1/clips/{clip_id}S5.1 —
Ingestor de
Eventos B3Consumo PubSub
B3; parsing do
Envelope B1/B3;
debounce/histerese
para ruídos na
borda.Fila PubSub B3 com
mensagens JSON contendo
envelope B1/B3.Stream de Eventos
Normalizados no domínioEvent Consumer + Filter
Strategy (Domain
Service).Rafael
Castro
S5.2 — Motor
de AgregaçãoCálculo de tempo
de permanência
(dwell time), taxa
de ocupação
instantânea e
horário de pico.Stream de Eventos
Normalizados + Janelas
temporais móveis (1m, 15m,
1h).DTO Métrica AcumuladaTime Window Engine +
Aggregator Strategy.Rafael
Castro
S5.3 —
Gerador de
Mapa de
CalorProjeção espacial
sem distorção em
coordenadas
métricas de plano
de mundo.Coordenadas de pixels $(u,
v)$ + Matriz de Homografia
I3 ($H \in
\mathbb{R}^{3\times 3}$).Matriz 2D de Densidade
Espacial {grid_id,Spatial Coordinate
Converter + Grid Density
Calculator.Rafael
Castro
S5.4 — Time-
Series Store
(DuckDB)Armazenamento
analítico colunar
OLAP; Worker de
Downsampling
(Rollup 1s ➡️ 1m ➡️
1h).Métricas e Grades de Calor +
Cron Trigger de Rollup.Tabelas DuckDB OLAP +
Consultas SQL de séries
temporais para a
aplicação.DuckDB Repository +
Downsampling Rollup
Worker.Rafael
Castro
B6.1 —
OverlayPlayerPlayer de vídeo
com renderização
acelerada por GPU
de bounding boxes
e polígonos.Stream RTSP/WebRTC direto
da Camada 1 + Array de
metadados {boxes[],
zones[]} .Elemento de UI
WebGL/Canvas2D
sobreposto ao player em
60 FPS.Componente
WebGL/Canvas2D +
Accelerated Video
Player.Eduardo
Gomes
B6.2 —
ZoneEditorEditor gráfico
interativo vetorial
para criação, ajuste
e serialização de
polígonos de risco.Imagem da câmera +
Interação de clique/arraste
do usuário na tela.Objeto JSON de Zona
Serializada {zone_id,Vector Canvas
Component +
Topological Polygon
Validator.Eduardo
Gomes
B6.3 —
AlertConsoleFeed reativo de
notificações
operacionais com
ações de
reconhecer,
silenciar e encerrar.Feed de Alertas via
WebSocket/State Controller.Evento de Ação
Operacional `{alert_id,
action: "ACK""MUTE""CLOSE",
user_id}`.
B6.4 —
AppShellLayout base,
navegação, status
de conexão da
placa Jetson e
Design System
Tokens.Propriedades da aplicação +
status de hardware da
Jetson.Casca de Layout da
Aplicação + Tokens
CSS/Design System (Dark
Mode).Shell Layout Component
+ Design System Token
Provider.Eduardo
Gomes
{clip_id, event_id,
file_path,
hash_sha256} .
is_locked_for_audit do
com token_rbac e
justificativa .
end_time,
points_of_interest[]}
+ Audit Log.
{event_type, origin,
coords, timestamp} .
{zone_id,
dwell_time_avg,
occupancy_peak,
total_count} .
cells: [[x, y,
density_value]]} .
name, type, points:
[{x, y}]} $[0.0, 1.0]$.
Componente
de Lista
Reativa + Store
de
Gerenciamento
de Estado.
Eduardo
Gomes
3. Critérios de Divisão e Atribuição de Responsabilidades
A separação dos membros do Squad MOD-2 foi definida estritamente sob quatro critérios técnicos de engenharia:
3.1. Alinhamento por Bounded Context & Domínio (DDD)
file:///C:/Users/davig/AppData/Local/Temp/ods_guia_tmp.html
2/79/12/26, 3:58 PM
S4 (Evidência de Mídia): Davi Gomes (Arquitetura & Regras de Domínio) e Henrique Azevedo (Infraestrutura de Baixo Nível & Mídia).
S5 (Métricas e Séries Temporais): Rafael Castro (Processamento Analítico, Geometria I3 & DuckDB).
B6 (Kit de Interface e Operação): Eduardo Gomes (Desenvolvimento Frontend, Canvas/WebGL & Design System).
3.2. Especialização de Stack & Afinidade Técnica
Henrique Azevedo (Sistemas & Baixo Nível): Alocação de memória RAM compartilhada /dev/shm , ponteiros de Ring Buffer do
GStreamer, monitoramento statvfs e daemons de expurgo.
Davi Gomes (Arquitetura & Integração de Aplicação): Liderança do squad, definição de contratos DTOs de API, integridade
probatória (hash SHA256), regras de auditoria RBAC/LGPD e barramento de eventos.
Rafael Castro (Engenharia de Dados & Matemática Espacial): Pipelines analíticos, séries temporais no DuckDB, agregações móveis e
álgebra linear para homografia espacial I3 ($H \in \mathbb{R}^{3\times 3}$).
Eduardo Gomes (Engenharia de Frontend & Computação Gráfica Web): Pacotes de UI reutilizáveis ( @ods/b6-* ), renderização
acelerada em Canvas2D/WebGL e monorepo npm.
3.3. Equilíbrio de Carga e Dimensionamento ODS (24 Pontos)
As 13 atividades do Squad MOD-2 (24 pontos) foram divididas em parcelas equivalentes de 6 pontos para cada desenvolvedor:
Davi Gomes: 3 atividades de S4 = 6 pts
Henrique Azevedo: 2 atividades de S4 = 6 pts
Rafael Castro: 4 atividades de S5 = 6 pts
Eduardo Gomes: 4 atividades de B6 = 6 pts
4. Diretrizes de Arquitetura e Submódulos dos Componentes
> [!NOTE]
> As diretrizes abaixo estabelecem as responsabilidades de alto nível e os contratos funcionais de cada submódulo. Os detalhes exatos
de implementação interna, bibliotecas específicas e escolha de estruturas internas de código devem ser discutidos e alinhados pelos
responsáveis diretos durante a fase de desenvolvimento.
4.1. Componente S4 — Evidência de Mídia (Camada 3 - Serviço)
🎯 Diretrizes Gerais do Componente
Garantir a captura eficiente de evidências visuais (vídeos/snapshots) e gerenciar seu ciclo de vida em borda, desde o buffer circular
inicial até o expurgo seguro no disco NVMe.
🧩 Submódulos e Diretrizes de Implementação:
S4.1 — Ring Buffer Contínuo (Alocação em RAM)
Responsável: Henrique Azevedo
Diretriz de Design: Manter um buffer circular em memória RAM compartilhada (ex: /dev/shm ) para armazenar os últimos $N$
segundos pré-evento sem desgastar o SSD NVMe.
Tópicos para Discussão do Desenvolvedor:
Definir o tamanho ideal da janela pré-evento (ex: 15s a 30s) de acordo com o consumo de RAM por câmera.
Avaliar entre implementar a fila circular via C/C++ vinculada com ctypes ou usar abstrações nativas do GStreamer ( appsink /
shmdata ).
S4.2 — Binding Evento-Mídia (Vinculação Imutável)
Responsável: Davi Gomes
Diretriz de Design: Ao receber a notificação de um evento do Barramento B3, extrair o trecho de vídeo pré e pós-evento da RAM,
consolidar em um arquivo definitivo .mp4 (ou snapshot .jpg ) e gerar uma assinatura de integridade (ex: hash SHA256).
Tópicos para Discussão do Desenvolvedor:
Definir se a codificação do vídeo utilizará aceleração gráfica via NVENC ou encoder padrão H.264.
Alinhar com a equipe o formato final dos metadados gravados na tabela SQLite.
S4.3 — Retenção & Expurgo LGPD (Purge Manager Daemon)
Responsável: Henrique Azevedo
Diretriz de Design: Garantir a remoção automática de mídias por ultrapassagem de prazo da LGPD (ex: 7 dias) ou por estouro
emergencial de cota de disco NVMe (ex: 85%), respeitando a trava de auditoria ( is_locked_for_audit ).
Tópicos para Discussão do Desenvolvedor:
Escolher a frequência da rotina de limpeza (ex: a cada 1h ou orientada a eventos de I/O).
Alinhar a política FIFO de descarte para mídias não travadas em caso de emergência de disco.
S4.4 — API Descritores de Clipe
Responsável: Davi Gomes
Diretriz de Design: Expor metadados de mídias via API local (REST ou gRPC) para fornecimento de URIs e pontos de interesse para a
aplicação, sem trafegar o binário pesado do vídeo no payload JSON.
Tópicos para Discussão do Desenvolvedor:
file:///C:/Users/davig/AppData/Local/Temp/ods_guia_tmp.html
3/79/12/26, 3:58 PM
Tópicos para Discussão do Desenvolvedor:
Definir a paleta de temas (Dark Mode) e tokens visuais compartilhados.
5. Arquitetura de Código, DDD e Estrutura de Pastas
Cada serviço backend (S4 e S5) deve ser construído seguindo rigorosamente a Clean Architecture (Arquitetura Limpa), dividida em 4
camadas internas sem dependências circulares:
src/
├── domain/# 1. CORAÇÃO: Entidades, VOs, Contratos de Repositórios (Zero dependências externas)
├── application/# 2. CASOS DE USO: Orquestração das regras de negócio e DTOs
├── infrastructure/# 3. INFRAESTRUTURA: SQLite, DuckDB, GStreamer, Pub/Sub B3
└── presentation/# 4. APIS: Controllers REST / gRPC
📂 Estrutura de Diretórios Recomendada
mod2/
├── s4-media-evidence/
│├── src/││├── domain/│││├── entities/# MediaClip.py
│││├── value_objects/# TimeWindow.py, ClipDescriptor.py
│││├── repositories/# IMediaClipRepository.py (Interface)
│││├── strategies/# RetentionStrategy.py (GoF Strategy)
│││└── exceptions/# DomainException.py
││├── application/│││├── use_cases/# ExtractClipUseCase.py, PurgeMediaUseCase.py
│││└── dtos/# ClipDescriptorDTO.py
││├── infrastructure/│││├── database/# SQLiteMediaClipRepository.py
│││├── gstreamer/# GStreamerRAMBufferAdapter.py
│││└── b3_bus/# B3EventConsumer.py
││└── presentation/││└── http/│└── tests/
# ClipDescriptorController.py
│├── unit/# Testes puros de domínio (sem I/O)
│└── integration/# Testes com SQLite e sistema de arquivos
│
├── s5-time-series/
│├── src/││├── domain/│││├── entities/# HeatmapGrid.py, OccupancyMetric.py
│││├── value_objects/# WorldCoordinate.py, SpatialCell.py
│││├── repositories/# ITimeSeriesRepository.py
│││└── services/# HeatmapDomainService.py
││├── application/│││├── use_cases/# AggregateDwellTimeUseCase.py, GenerateHeatmapUseCase.py
│││└── dtos/# HeatmapQueryDTO.py
││├── infrastructure/│││├── database/# DuckDBTimeSeriesRepository.py
│││└── i3_calibration/# HomographyCalibratorAdapter.py
││└── presentation/││└── http/│└── tests/│├── unit/
│└── integration/
# MetricsQueryController.py
│
└── b6-ui-kit/
# Monorepo NPM
├── packages/
│├── overlay-player/
│├── zone-editor/
│├── alert-console/
│└── app-shell/
└── package.json
6. Padrões de Projeto (GoF) a Utilizar
Para manter o código manutenível e extensível, aplique os seguintes Design Patterns:
Problema de Engenharia
file:///C:/Users/davig/AppData/Local/Temp/ods_guia_tmp.html
Padrão GoF
Aplicado
Onde e Como Utilizar
5/79/12/26, 3:58 PM
Alternar regras de expurgo por tempo (LGPD) ou por espaço
(Cota 85% NVMe)Strategy
RetentionStrategy no S4. Permite trocar o algoritmo de deleção em
Isolar o banco de dados (SQLite/DuckDB) das regras de
negócio do domínioRepositoryInterfaces IMediaClipRepository e ITimeSeriesRepository no
domínio.
Inscrição e reação aos eventos brutos do Barramento B3Observer /
ConsumerB3EventConsumer escuta a fila Pub/Sub e dispara os Casos de Uso.
Fornecer uma interface simplificada para extração de vídeo
do RingBufferFacade
tempo de execução.
RAMBufferFacade esconde a complexidade de ponteiros de memória
do GStreamer.
7. Regras de Código (Clean Code & Segurança)
7.1. Nomenclatura Expressiva
Váriaveis e Funções: Devem revelar a intenção pura sem necessidade de comentários explicativos.
❌ def proc(d, t):
✅ def extract_clip_for_event(event_id: str, time_window: TimeWindow) -> MediaClip:
Booleanos: Devem ser lidos como perguntas predicadas ( is_retained , has_exceeded_quota , is_locked_for_audit ).
7.2. Regra de Funções Pequenas
Funções devem ter apenas uma responsabilidade (Single Responsibility Principle - SRP) e idealmente até 20 linhas.
Separação Comando-Consulta (CQS): Uma função ou altera o estado do sistema (Command) ou retorna um valor (Query), nunca os
dois simultaneamente.
7.3. Hierarquia Estrita de Exceções
Nunca lance exceções genéricas como Exception ou RuntimeException . Utilize exceções domain-specific:
class ODSBaseException(Exception):
"""Exceção base para todo o sistema ODS."""
class DomainError(ODSBaseException):
"""Erros de violação de regras de negócio do domínio."""
class InvalidTimeWindowError(DomainError):
"""Lançado quando start_time >= end_time."""
class DiskQuotaExceededError(DomainError):
"""Lançado quando o NVMe atinge limite sem mídia para expurgar."""
class ApplicationError(ODSBaseException):
"""Erros na camada de casos de uso."""
class MediaFileNotFoundError(ApplicationError):
"""Lançado quando o arquivo .mp4 não existe mais no NVMe."""
8. Estratégia de Testes (TDD & Qualidade)
> "Código sem testes não está limpo." — Dave Thomas
8.1. Regras de Testes de Unidade ( tests/unit/ )
Testes de unidade testam apenas a lógica pura do domínio.
Zero I/O: Sem chamadas de banco de dados real, sem rede, sem sistema de arquivos real (use Stubs e Fakes em memória).
Nomenclatura Obrigatória:
$$\text{deve\_[resultado\_esperado]\_quando\_[condição\_testada]}$$
Exemplo de Teste de Unidade Limpo:
# tests/unit/domain/test_time_window.py
import pytest
from datetime import datetime, timedelta
from src.domain.value_objects.time_window import TimeWindow
from src.domain.exceptions import InvalidTimeWindowError
def test_deve_calcular_duracao_corretamente_quando_janela_for_valida():
start = datetime(2026, 9, 10, 10, 0, 0)
end = start + timedelta(seconds=30)
tw = TimeWindow(start_time=start, end_time=end)
file:///C:/Users/davig/AppData/Local/Temp/ods_guia_tmp.html
6/79/12/26, 3:58 PM
assert tw.duration_seconds == 30.0
def test_deve_lancar_excecao_quando_start_time_for_maior_ou_igual_ao_end_time():
start = datetime(2026, 9, 10, 10, 0, 0)
end = start - timedelta(seconds=1)
with pytest.raises(InvalidTimeWindowError):
TimeWindow(start_time=start, end_time=end)
9. Checklist Obrigatório para o Desenvolvedor (Antes do Commit)
Antes de submeter qualquer Pull Request (PR) ou código para revisão do squad MOD-2, garanta que:
[ ] Clean Architecture Respeitada: As dependências apontam obrigatoriamente para dentro (Domínio não importa nada da
Infraestrutura ou Frameworks).
[ ] Sem Vídeo no Barramento: Nenhuma mídia binária ou arquivo pesado foi transmitido pelo barramento B3.
[ ] Nomes Limpos: Variáveis e métodos dispensam comentários para explicar o que fazem.
[ ] Tratamento de Erros: Todas as falhas lançam exceções tipadas de DomainError ou ApplicationError .
[ ] Testes de Unidade: As regras de negócio alteradas possuem testes de unidade passando na suíte automatizada.
[ ] Conformidade LGPD: As novas entidades de mídia incluem sinalizadores de tempo de vida e flag de auditoria.
file:///C:/Users/davig/AppData/Local/Temp/ods_guia_tmp.html
7/7