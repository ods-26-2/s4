#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace ods::s4::domain {

// A atividade pede "definir por quanto tempo CADA TIPO de midia e guardado":
// um snapshot e um clipe podem ter prazos diferentes. O tipo e gravado junto
// do clipe (coluna media_type) porque e ele que decide o prazo: um SNAPSHOT
// pedido por capture.command pode ser gravado como .mp4 de 1 s, e a extensao
// sozinha o classificaria como clipe.
enum class MediaKind { VideoClip, Snapshot };

// Palpite pela extensao, para linhas antigas (sem media_type) e para quem cria
// um MediaClip sem dizer o tipo: .jpg/.jpeg/.png sao snapshot, o resto e clipe.
inline MediaKind media_kind_of(const std::string& filePath) {
    const std::size_t dot = filePath.rfind('.');
    if (dot == std::string::npos) {
        return MediaKind::VideoClip;
    }
    std::string extension = filePath.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool isImage = extension == "jpg" || extension == "jpeg" || extension == "png";
    return isImage ? MediaKind::Snapshot : MediaKind::VideoClip;
}

// Nome no contrato 6c (campo media_type de ods.s4.clip e de clip.query).
inline const char* to_contract_name(MediaKind kind) noexcept {
    return kind == MediaKind::Snapshot ? "SNAPSHOT" : "CLIP";
}

inline std::optional<MediaKind> media_kind_from_contract_name(const std::string& name) {
    if (name == "CLIP") return MediaKind::VideoClip;
    if (name == "SNAPSHOT") return MediaKind::Snapshot;
    return std::nullopt;
}

} // namespace ods::s4::domain
