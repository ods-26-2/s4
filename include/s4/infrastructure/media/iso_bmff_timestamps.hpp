#pragma once

#include <cstdint>
#include <vector>

namespace ods::s4::infrastructure {

// Zera os campos creation_time / modification_time dos boxes de cabecalho MP4
// (mvhd, tkhd, mdhd), deixando-os constantes.
//
// MOTIVACAO
// ---------
// O mp4mux do GStreamer carimba a hora em que o muxing aconteceu nesses tres
// boxes. E a unica fonte de nao-determinismo no container: os bytes do mdat
// sao as amostras H.264 em byte a byte, e as tabelas de amostra dependem so
// dos timestamps de captura. Sem normalizar, msm dois clips extraidos dos
// MESMOS frames do MESMO footage produzem sha256 diferentes, porque a
// diferenca e de segundos entre execucoes.
//
// A evidencia precisa ser repetivel: o hash e a identidade do artefato, e um
// hash que muda a cada extracao do mesmo clipe nao identifica nada.
//
// POR QUE ISSO NAO DESTRUI PROVENIENCIA
// ------------------------------------
// Os campos zerados descrevem QUANDO o container foi montado, nao o QUANDO o
// video foi capturado. Quem responde "quando isso foi gravado" e o RingBuffer
// (timestamps de captura, persistidos) e o descritor do clipe no banco. O
// cabecalho do container nao tem valor probatorio sobre o conteudo do video, e
// nenhum player, leitor ou verificador o usa para interpretar o material.
//
// A escolha do valor de substituicao e o ponto honesto do codigo. Deixar
// 0 significaria 1904-01-01, que e um tempo plausivel de verdade e por isso
// MENTIRA: pareceria um dado legitimo. Todo o container recebe entao o mesmo
// valor sentinela, escolhido para ser obviamente nao-real.
//
// O que ela NAO toca: nada dentro de mdat (as amostras de video) e nada nas
// tabelas de amostra. So os tres boxes de cabecalho sao reescritos.
//
// SEGURANCA
// ---------
// A funcao percorre a arvore de boxes respeitando tamanhos, entao um campo de
// data invalido ou um box de tamanho malformado nao faz a funcao escrever fora
// do buffer. Se o buffer nao parece ser ISO-BMFF, ela devolve sem tocar em
// nada — falhar e melhor do que corromper evidencia.
void normalize_iso_bmff_timestamps(std::vector<std::uint8_t>& container);

} // namespace ods::s4::infrastructure
