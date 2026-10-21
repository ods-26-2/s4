#pragma once

#include <chrono>

#include "s4/application/dtos/extracted_segment.hpp"
#include "s4/domain/entities/ring_buffer.hpp"
#include "s4/domain/value_objects/capture_window.hpp"

namespace ods::s4::application {

// Porta de leitura do ring buffer NO RELOGIO DE CAPTURA.
//
// Por que uma porta alem de domain::IMediaBufferReader?
//
// IMediaBufferReader e o contrato do S4.2 em tempo de PAREDE (system_clock):
// ele existe desde o comeco, e a API REST ja o consome. Ele nao serve para o
// fluxo deterministico porque o relogio de parede nao e a base do buffer.
//
// Quando um evento chega, o operador nao consegue saber qual o capture_ts que
// o buffer vai registrar daqui a dois segundos — e o que importa e o instante
// do evento EM TERMOS DE CAPTURA. Esta porta responde a janela em torno de um
// capture_ts e, diferentemente de readWindow(), ESPERA o fim da janela: o
// trecho posterior ao evento ainda esta sendo capturado no instante do disparo.
//
// A porta vive em application (e nao em domain) porque o tipo de retorno,
// ExtractedSegment, tambem e um DTO de application. A inversao continua
// correta: quem implementa (RingBufferRAMFacade) depende do contrato, nunca o
// contrario.
class ICaptureBufferReader {
public:
    virtual ~ICaptureBufferReader() = default;

    // Trecho que cobre [window.startNs, window.endNs] no relogio de captura.
    //
    // waitTimeoutSeconds > 0 bloqueia ate a janela ser alcancada (ou o
    // timeout): e o que permite a API responder 201 com o pos-evento dentro
    // do clipe. Com timeout esgotado devolve o que existe e marca
    // isTruncatedAtEnd, em vez de falhar.
    [[nodiscard]] virtual ExtractedSegment extractCaptureWindow(
        const domain::CaptureWindow& window,
        double waitTimeoutSeconds
    ) = 0;

    // Estado do buffer, usado pela API para expor a faixa de capture_ts
    // realmente disponivel — sem isso o cliente nao tem como alinhar uma
    // janela e so pode chutar.
    [[nodiscard]] virtual domain::BufferStats bufferStats() const = 0;

    // Instante de captura mais recente. Permite que a API trate "agora" sem
    // o cliente precisar consultar /buffer/stats antes de todo evento.
    [[nodiscard]] virtual domain::Nanoseconds newestCaptureTsNs() const = 0;

    // Conversao de volta para o relogio de parede, so para montar o descritor
    // do clipe (que e em ISO-8601 de parede). Fica na porta de proposito: a
    // traducao entre os dois relogios acontece em UM lugar do componente
    // (CaptureClockBridge) e nao se espalha pelos casos de uso.
    [[nodiscard]] virtual std::chrono::system_clock::time_point toWallClock(
        domain::Nanoseconds captureTsNs
    ) const = 0;
};

} // namespace ods::s4::application
