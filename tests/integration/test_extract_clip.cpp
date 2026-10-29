#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "s4/application/use_cases/extract_clip.hpp"
#include "s4/domain/value_objects/time_window.hpp"
#include "s4/infrastructure/database/in_memory_media_clip_repository.hpp"
#include "s4/infrastructure/filesystem/file_storage.hpp"
#include "s4/infrastructure/gstreamer/mock_ram_buffer_facade.hpp"
#include "s4/infrastructure/hashing/sha256_hasher.hpp"

using namespace ods::s4;

std::vector<std::uint8_t> to_bytes(const std::string& value) {
    return std::vector<std::uint8_t>(value.begin(), value.end());
}

std::filesystem::path make_temp_dir(const std::string& suffix) {
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    const auto dir = std::filesystem::temp_directory_path() /
                     ("s4_extract_" + std::to_string(stamp) + "_" + suffix);
    std::filesystem::create_directories(dir);
    return dir;
}

void test_deve_extrair_clipe_com_hash_verificavel_em_arquivo_real() {
    const auto mediaDir = make_temp_dir("media");
    const std::string outputPath = (mediaDir / "evt-abc.mp4").string();

    auto repository = std::make_shared<infrastructure::InMemoryMediaClipRepository>();
    auto buffer = std::make_shared<infrastructure::MockRAMBufferFacade>();
    auto storage = std::make_shared<infrastructure::FileStorage>();

    const auto start = std::chrono::system_clock::now();
    buffer->feedFrame({to_bytes("FRAME_00"), start});
    buffer->feedFrame({to_bytes("FRAME_01"), start + std::chrono::milliseconds(10)});
    buffer->feedFrame({to_bytes("FRAME_02"), start + std::chrono::milliseconds(20)});
    domain::TimeWindow timeWindow(start, start + std::chrono::milliseconds(30));

    application::ExtractClipUseCase useCase(repository, buffer, storage);
    const auto clip = useCase.execute("evt-abc", timeWindow, outputPath);

    std::vector<std::uint8_t> expectedPayload;
    const auto frame0 = to_bytes("FRAME_00");
    const auto frame1 = to_bytes("FRAME_01");
    const auto frame2 = to_bytes("FRAME_02");
    expectedPayload.insert(expectedPayload.end(), frame0.begin(), frame0.end());
    expectedPayload.insert(expectedPayload.end(), frame1.begin(), frame1.end());
    expectedPayload.insert(expectedPayload.end(), frame2.begin(), frame2.end());
    const std::string expectedHash = infrastructure::compute_sha256_hex(expectedPayload);

    assert(clip.sha256Hash() == expectedHash);
    assert(clip.sha256Hash() == "c5d94dd20a8429595a2d3232e6464179bba1e9c119d07d57e2494e56e230dd35");

    assert(storage->exists(outputPath));

    std::ifstream input(outputPath, std::ios::binary);
    std::vector<std::uint8_t> onDisk((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    assert(onDisk == expectedPayload);

    const auto stored = repository->findById(clip.clipId());
    assert(stored.has_value());
    assert(stored->sha256Hash() == expectedHash);

    std::error_code errorCode;
    std::filesystem::remove_all(mediaDir, errorCode);
}

int main() {
    std::cout << "Running S4 ExtractClip Integration Tests...\n";
    test_deve_extrair_clipe_com_hash_verificavel_em_arquivo_real();
    std::cout << "All S4 extract tests passed successfully!\n";
    return 0;
}