#include "s4/infrastructure/hashing/sha256_hasher.hpp"

#include "s4/domain/errors/domain_error.hpp"

#include <openssl/evp.h>

#include <array>
#include <iomanip>
#include <sstream>

namespace ods::s4::infrastructure {

std::string compute_sha256_hex(const std::vector<std::uint8_t>& data) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        throw domain::HashingError("failed to allocate EVP digest context");
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestLen = 0;
    const bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
                    EVP_DigestUpdate(ctx, data.data(), data.size()) == 1 &&
                    EVP_DigestFinal_ex(ctx, digest.data(), &digestLen) == 1;
    EVP_MD_CTX_free(ctx);

    if (!ok) {
        throw domain::HashingError("failed to compute SHA-256 digest");
    }

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digestLen; ++i) {
        out << std::setw(2) << static_cast<int>(digest[i]);
    }
    return out.str();
}

} // namespace ods::s4::infrastructure