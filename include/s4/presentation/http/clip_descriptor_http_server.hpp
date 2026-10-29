#pragma once

#include <memory>
#include <string>

#include "s4/application/use_cases/extract_clip.hpp"
#include "s4/domain/repositories/media_clip_repository.hpp"

namespace ods::s4::presentation {

class ClipDescriptorHttpServer {
public:
    explicit ClipDescriptorHttpServer(
        std::shared_ptr<domain::IMediaClipRepository> repository,
        std::shared_ptr<application::ExtractClipUseCase> clipUseCase = {},
        std::string mediaDir = {}
    );
    ~ClipDescriptorHttpServer();

    [[nodiscard]] bool bind(int port);
    [[nodiscard]] bool start();
    void stop();
    [[nodiscard]] int port() const;
    [[nodiscard]] bool running() const;

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

} // namespace ods::s4::presentation