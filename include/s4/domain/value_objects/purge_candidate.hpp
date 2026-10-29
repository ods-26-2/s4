#pragma once

#include <cstdint>

#include "s4/domain/entities/media_clip.hpp"

namespace ods::s4::domain {

// Uma midia que PODE ser expurgada, com o espaco que o arquivo ocupa no NVMe.
//
// O tamanho vem junto porque a estrategia de cota precisa projetar quanto
// espaco cada exclusao libera; 0 quando o arquivo ja nao existe.
struct PurgeCandidate {
    MediaClip clip;
    std::uintmax_t sizeBytes{0};
};

} // namespace ods::s4::domain
