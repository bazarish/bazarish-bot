// Bazarish project (c) 2026
#include "Password.hpp"

#include <bazarish/Bytes.hpp>

#include <openssl/crypto.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace bazarish::daemon {

namespace {

constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kHashBytes = 32;
// The cost of one sign-in on the machine a daemon runs on. An account key is
// derived with more (that file can be stolen and attacked offline); this guards
// an interface that is on loopback and answers at human speed.
constexpr std::uint32_t kMemoryKiB = 64 * 1024;
constexpr std::uint32_t kPasses = 3;
constexpr std::uint32_t kLanes = 1;

Bytes derive(const std::string& password, const Bytes& salt)
{
    EVP_KDF* const kdf = EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr);
    if (kdf == nullptr) {
        throw std::runtime_error("password: this OpenSSL has no Argon2id");
    }
    EVP_KDF_CTX* const ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (ctx == nullptr) {
        throw std::runtime_error("password: could not start the derivation");
    }
    std::uint32_t memory = kMemoryKiB;
    std::uint32_t passes = kPasses;
    std::uint32_t lanes = kLanes;
    const OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string(
            "pass", const_cast<char*>(password.data()), password.size()),
        OSSL_PARAM_construct_octet_string(
            "salt", const_cast<unsigned char*>(salt.data()), salt.size()),
        OSSL_PARAM_construct_uint("memcost", &memory),
        OSSL_PARAM_construct_uint("iter", &passes),
        OSSL_PARAM_construct_uint("lanes", &lanes),
        OSSL_PARAM_construct_uint("threads", &lanes),
        OSSL_PARAM_construct_end(),
    };
    Bytes out(kHashBytes);
    const int ok = EVP_KDF_derive(ctx, out.data(), out.size(), params);
    EVP_KDF_CTX_free(ctx);
    if (ok <= 0) {
        throw std::runtime_error("password: the derivation failed");
    }
    return out;
}

}  // namespace

PasswordRecord hashPassword(const std::string& password)
{
    Bytes salt(kSaltBytes);
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
        throw std::runtime_error("password: no randomness available");
    }
    return {toHex(salt), toHex(derive(password, salt))};
}

bool passwordMatches(const PasswordRecord& record, const std::string& password)
{
    if (record.salt.empty() || record.hash.empty()) {
        return false;
    }
    const Bytes salt = fromHex(record.salt);
    const Bytes expected = fromHex(record.hash);
    const Bytes actual = derive(password, salt);
    if (expected.size() != actual.size()) {
        return false;
    }
    return CRYPTO_memcmp(expected.data(), actual.data(), actual.size()) == 0;
}

}  // namespace bazarish::daemon
