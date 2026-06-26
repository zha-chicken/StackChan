/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "secret_logic.h"
#include <sdkconfig.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <array>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace secret_logic {

namespace {

static const char* TAG = "secret_logic";

static const char* kStackChanServerPublicKey = R"(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAptf3oJPaOREMyJJ6lmMM
iTk/OdLDLxms1WC4EmM+Q0U7rCFBTrlWIGRBfUlNEodqhsTj2TjyFIUkMbYvWgVK
S8BbvXPHqqbkaUOPuqir83/4MQj9dSTWpTqjNjDQS4nombfrYO98Agdj5M21QPty
kbYNQnAJailq34SgMCYFXRY8wnBj07yy00webfikX5hsMjLj7VLnSUTFVT+56/si
OT/XMKYiUP7xQ91tL4LU9x1VqR8xdS4YuZAqjXJ53CWwz8+6cMO3GW7BR8GfiYfH
S4WVwnXDC4VS5ZhXyHKvC3qvkw2zZyPSUfBGbCr3fFhtm7E6PvIGGAUuTNjnihwi
eQIDAQAB
-----END PUBLIC KEY-----)";

static const char* kStackChanBluetoothPublicKey = R"(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAzEA9BzulpncMvpUp2aAs
TRSiB5nvF4oQpbfYjGeGInDka1ZzFy0yJ4mjxlPFJ9AcZUIJD2vWxUUKQOf9feU8
RREJCHIe2rEhx1LzvIbt2FaDgup2QUSQsjAX+MKeS6121AScrHJTv9M7tWnVYwsz
6pHk7/6qJ3MP1E7JHbqc8y93VBRqlOFNgUGXmspP5MHuhSyTj8WrKew+jfMyuxVB
mIWpGN5weM3gewVKJufiC2geF4+D9gHHivjrkG/4k5YM5u3tFQ7N+3g1cx7rC1Oa
9Umydxd0UMdCVacUtPpo3HsmK5fTwPJ/nS6n5Elc18q+081ypE1Y3aY8MMji07VJ
jQIDAQAB
-----END PUBLIC KEY-----)";

static int rng(void*, unsigned char* output, size_t len)
{
    esp_fill_random(output, len);
    return 0;
}

static std::string base64_encode(const unsigned char* data, size_t len)
{
    size_t out_len = 0;
    mbedtls_base64_encode(nullptr, 0, &out_len, data, len);

    std::string out(out_len, '\0');
    int ret = mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &out_len, data, len);
    if (ret != 0) {
        ESP_LOGE(TAG, "base64 encode failed: -0x%04x", -ret);
        return {};
    }

    out.resize(out_len);
    return out;
}

static std::string rsa_oaep_sha256_encrypt(std::string_view plain_text, const char* public_key_pem)
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    int ret = mbedtls_pk_parse_public_key(&pk, reinterpret_cast<const unsigned char*>(public_key_pem),
                                          std::strlen(public_key_pem) + 1);
    if (ret != 0) {
        ESP_LOGE(TAG, "parse public key failed: -0x%04x", -ret);
        mbedtls_pk_free(&pk);
        return {};
    }

    if (!mbedtls_pk_can_do(&pk, MBEDTLS_PK_RSA)) {
        ESP_LOGE(TAG, "public key is not RSA");
        mbedtls_pk_free(&pk);
        return {};
    }

    mbedtls_rsa_context* rsa = mbedtls_pk_rsa(pk);
    mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);

    std::vector<unsigned char> cipher(mbedtls_pk_get_len(&pk));
    size_t cipher_len = 0;
    ret = mbedtls_pk_encrypt(&pk, reinterpret_cast<const unsigned char*>(plain_text.data()), plain_text.size(),
                             cipher.data(), &cipher_len, cipher.size(), rng, nullptr);

    mbedtls_pk_free(&pk);

    if (ret != 0) {
        ESP_LOGE(TAG, "rsa encrypt failed: -0x%04x", -ret);
        return {};
    }

    return base64_encode(cipher.data(), cipher_len);
}

static std::array<uint8_t, 6> factory_mac()
{
    std::array<uint8_t, 6> mac{};
    esp_read_mac(mac.data(), ESP_MAC_EFUSE_FACTORY);
    return mac;
}

static std::string mac_compact()
{
    auto mac = factory_mac();
    char out[13];
    std::snprintf(out, sizeof(out), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return out;
}

static std::string mac_colon()
{
    auto mac = factory_mac();
    char out[18];
    std::snprintf(out, sizeof(out), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return out;
}

static std::string random_nonce()
{
    uint8_t bytes[8];
    esp_fill_random(bytes, sizeof(bytes));

    char out[17];
    std::snprintf(out, sizeof(out), "%02X%02X%02X%02X%02X%02X%02X%02X", bytes[0], bytes[1], bytes[2], bytes[3],
                  bytes[4], bytes[5], bytes[6], bytes[7]);
    return out;
}

}  // namespace

__attribute__((weak)) std::string get_server_url()
{
#ifdef CONFIG_STACKCHAN_SERVER_URL
    return CONFIG_STACKCHAN_SERVER_URL;
#else
    return "http://localhost:3000";
#endif
}

__attribute__((weak)) std::string generate_auth_token()
{
    const time_t now = std::time(nullptr);
    const std::string plain_text = mac_colon() + "|" + random_nonce() + "|" + std::to_string(static_cast<long long>(now));
    return rsa_oaep_sha256_encrypt(plain_text, kStackChanServerPublicKey);
}

__attribute__((weak)) std::string generate_handshake_token(std::string_view data)
{
    const std::string plain_text = mac_compact();
    ESP_LOGI(TAG, "generate handshake token input_len=%u plain_len=%u", static_cast<unsigned>(data.size()),
             static_cast<unsigned>(plain_text.size()));
    return rsa_oaep_sha256_encrypt(plain_text, kStackChanBluetoothPublicKey);
}

}  // namespace secret_logic
