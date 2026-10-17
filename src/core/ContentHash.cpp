#include "core/ContentHash.h"
#include <openssl/evp.h>
#include <stdexcept>

namespace pf {
std::string Sha256(std::string_view bytes) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(),
                   nullptr) != 1)
        throw std::runtime_error("SHA-256 unavailable");
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned int i = 0; i < length; ++i) {
        result += hex[digest[i] >> 4];
        result += hex[digest[i] & 15];
    }
    return result;
}
} // namespace pf
