#pragma once

#include <nlohmann/json.hpp>

#include "s4/domain/entities/media_clip.hpp"

namespace ods::s4::infrastructure {

// Payload 6c de um clipe (ods.s4.clip e cada item de clip.query.reply).
//
// Um lugar so para os tres handlers: o que o S4 publica quando o clipe nasce e
// o que responde quando alguem pergunta depois tem de ser o MESMO descritor —
// inclusive o expires_at, que e uma promessa (depois dele o media_uri nao
// resolve) e por isso vem do valor gravado no banco, nunca do relogio de agora.
nlohmann::json clip_payload(const domain::MediaClip& clip);

// "s4://clips/<arquivo>": a referencia que o contrato publica no lugar do
// caminho local do NVMe.
std::string clip_media_uri(const domain::MediaClip& clip);

} // namespace ods::s4::infrastructure
