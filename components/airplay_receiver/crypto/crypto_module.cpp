// airplay_receiver crypto module — AirPlay 2 HomeKit pairing + ChaCha20-Poly1305
// audio crypto.
//
// Port of rbouteiller/airplay-esp32, restricted to the AirPlay 2 path:
//   * main/hap/hap.c, hap_crypto.c, hap_pair_verify.c, hap_pair_setup.c,
//     srp.c/.h, tlv8.c/.h              -> HomeKit pairing (SRP-6a 3072-bit +
//                                          Ed25519 + X25519 + ChaCha20-Poly1305)
//   * main/audio/audio_crypto.c        -> ChaCha20-Poly1305 audio frame decrypt
//
// DELIBERATELY SKIPPED / NOT PORTED:
//   * main/rtsp/rtsp_rsa.c             -> AirPlay 1 RSA auth
//   * AUDIO_ENCRYPT_AES_CBC            -> AirPlay 1 AES-CBC audio encryption
//   * main/rtsp/rtsp_fairplay.c        -> FairPlay handshake (separate task)
//
// All heap routes through airplay_alloc/airplay_calloc/airplay_free (realtime
// = false: pairing + audio key setup are control-plane / non-realtime). The
// device Ed25519 identity persists via ESPHome preferences.

#include "crypto_module.h"
#include "../allocator.h"

#include "esphome/core/log.h"
#include "esphome/core/preferences.h"

#include <cstdio>
#include <cstring>

#include "sodium.h"

#include "esp_err.h"
#include "esp_mac.h"
#include "esp_random.h"

#include "mbedtls/aes.h"
#include "mbedtls/bignum.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_crypto";

// ---------------------------------------------------------------------------
// Internal session state (opaque to callers; allocations below route through
// the airplay_* allocator).
// ---------------------------------------------------------------------------
struct SrpSession {
  uint8_t salt[SRP_SALT_BYTES];
  uint8_t server_public_key[SRP_PRIME_BYTES];   // B
  uint8_t server_secret[SRP_PRIME_BYTES];       // b
  uint8_t client_public_key[SRP_PRIME_BYTES];   // A
  uint8_t session_key[SRP_SESSION_KEY_BYTES];   // K
  size_t session_key_len;
  uint8_t proof_m1[SRP_SESSION_KEY_BYTES];
  uint8_t proof_m2[SRP_SESSION_KEY_BYTES];
  int state;
  bool verified;
};

struct HAPSession {
  // Device long-term Ed25519 identity (copied from the module at creation).
  uint8_t device_public_key[HAP_ED25519_PUBLIC_KEY_SIZE];
  uint8_t device_secret_key[HAP_ED25519_SECRET_KEY_SIZE];
  // Ephemeral X25519 keypair for this session.
  uint8_t session_public_key[HAP_X25519_KEY_SIZE];
  uint8_t session_secret_key[HAP_X25519_KEY_SIZE];
  // Client ephemeral public key + shared secret.
  uint8_t client_public_key[HAP_X25519_KEY_SIZE];
  uint8_t shared_secret[HAP_X25519_KEY_SIZE];
  // Derived session (control) keys.
  uint8_t encrypt_key[HAP_CHACHA20_KEY_SIZE];
  uint8_t decrypt_key[HAP_CHACHA20_KEY_SIZE];
  // Encryption nonces/counters.
  uint64_t encrypt_nonce;
  uint64_t decrypt_nonce;
  // State machine.
  int pair_verify_state;
  int pair_setup_state;
  bool pair_setup_transient;
  bool session_established;
  SrpSession *srp;
};

namespace {

// ---------------------------------------------------------------------------
// TLV8 encoder/decoder (from main/hap/tlv8.c)
// ---------------------------------------------------------------------------
struct Tlv8Encoder {
  uint8_t *buffer;
  size_t size;
  size_t capacity;
};

void tlv8_encoder_init(Tlv8Encoder *enc, uint8_t *buffer, size_t capacity) {
  enc->buffer = buffer;
  enc->size = 0;
  enc->capacity = capacity;
}

bool tlv8_encode(Tlv8Encoder *enc, uint8_t type, const uint8_t *value, size_t len) {
  size_t offset = 0;
  while (offset < len || (offset == 0 && len == 0)) {
    size_t chunk_len = len - offset;
    if (chunk_len > 255) {
      chunk_len = 255;
    }
    if (enc->size + 2 + chunk_len > enc->capacity) {
      return false;
    }
    enc->buffer[enc->size++] = type;
    enc->buffer[enc->size++] = static_cast<uint8_t>(chunk_len);
    if (chunk_len > 0) {
      std::memcpy(enc->buffer + enc->size, value + offset, chunk_len);
      enc->size += chunk_len;
    }
    offset += chunk_len;
    if (len == 0) {
      break;
    }
  }
  return true;
}

bool tlv8_encode_byte(Tlv8Encoder *enc, uint8_t type, uint8_t value) {
  return tlv8_encode(enc, type, &value, 1);
}

size_t tlv8_encoder_size(const Tlv8Encoder *enc) { return enc->size; }

const uint8_t *tlv8_find(const uint8_t *data, size_t data_len, uint8_t type, size_t *value_len) {
  size_t offset = 0;
  while (offset + 2 <= data_len) {
    uint8_t t = data[offset];
    uint8_t l = data[offset + 1];
    if (offset + 2 + l > data_len) {
      break;
    }
    if (t == type) {
      *value_len = l;
      return data + offset + 2;
    }
    offset += 2 + l;
  }
  *value_len = 0;
  return nullptr;
}

bool tlv8_decode_concat(const uint8_t *data, size_t data_len, uint8_t type, uint8_t *out_buffer,
                        size_t out_capacity, size_t *out_len) {
  size_t offset = 0;
  size_t out_offset = 0;
  bool found = false;
  bool in_sequence = false;
  while (offset + 2 <= data_len) {
    uint8_t t = data[offset];
    uint8_t l = data[offset + 1];
    if (offset + 2 + l > data_len) {
      break;
    }
    if (t == type) {
      found = true;
      in_sequence = true;
      if (out_offset + l > out_capacity) {
        return false;
      }
      std::memcpy(out_buffer + out_offset, data + offset + 2, l);
      out_offset += l;
    } else if (in_sequence) {
      break;
    }
    offset += 2 + l;
  }
  *out_len = out_offset;
  return found;
}

// ---------------------------------------------------------------------------
// HKDF-SHA512 (from main/hap/hap_crypto.c) — HKDF extract + expand with the
// IETF building block (HMAC-SHA512).
// ---------------------------------------------------------------------------
int hap_hkdf_sha512(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
                    const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len) {
  uint8_t prk[crypto_auth_hmacsha512_BYTES];
  crypto_auth_hmacsha512_state state;

  if (salt != nullptr && salt_len > 0) {
    crypto_auth_hmacsha512_init(&state, salt, salt_len);
  } else {
    uint8_t zero_salt[crypto_auth_hmacsha512_BYTES] = {0};
    crypto_auth_hmacsha512_init(&state, zero_salt, sizeof(zero_salt));
  }
  crypto_auth_hmacsha512_update(&state, ikm, ikm_len);
  crypto_auth_hmacsha512_final(&state, prk);

  uint8_t t[crypto_auth_hmacsha512_BYTES];
  uint8_t counter = 1;
  size_t t_len = 0;
  size_t pos = 0;
  while (pos < okm_len) {
    crypto_auth_hmacsha512_init(&state, prk, sizeof(prk));
    if (t_len > 0) {
      crypto_auth_hmacsha512_update(&state, t, t_len);
    }
    if (info != nullptr && info_len > 0) {
      crypto_auth_hmacsha512_update(&state, info, info_len);
    }
    crypto_auth_hmacsha512_update(&state, &counter, 1);
    crypto_auth_hmacsha512_final(&state, t);
    t_len = crypto_auth_hmacsha512_BYTES;

    size_t copy_len = okm_len - pos;
    if (copy_len > crypto_auth_hmacsha512_BYTES) {
      copy_len = crypto_auth_hmacsha512_BYTES;
    }
    std::memcpy(okm + pos, t, copy_len);
    pos += copy_len;
    counter++;
  }

  sodium_memzero(prk, sizeof(prk));
  sodium_memzero(t, sizeof(t));
  return 0;
}

// ---------------------------------------------------------------------------
// SRP-6a helpers (from main/hap/srp.c)
// ---------------------------------------------------------------------------
// 3072-bit prime N (RFC 5054 group 15).
const uint8_t kSrpN[SRP_PRIME_BYTES] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC9, 0x0F, 0xDA, 0xA2, 0x21, 0x68, 0xC2,
    0x34, 0xC4, 0xC6, 0x62, 0x8B, 0x80, 0xDC, 0x1C, 0xD1, 0x29, 0x02, 0x4E, 0x08, 0x8A, 0x67,
    0xCC, 0x74, 0x02, 0x0B, 0xBE, 0xA6, 0x3B, 0x13, 0x9B, 0x22, 0x51, 0x4A, 0x08, 0x79, 0x8E,
    0x34, 0x04, 0xDD, 0xEF, 0x95, 0x19, 0xB3, 0xCD, 0x3A, 0x43, 0x1B, 0x30, 0x2B, 0x0A, 0x6D,
    0xF2, 0x5F, 0x14, 0x37, 0x4F, 0xE1, 0x35, 0x6D, 0x6D, 0x51, 0xC2, 0x45, 0xE4, 0x85, 0xB5,
    0x76, 0x62, 0x5E, 0x7E, 0xC6, 0xF4, 0x4C, 0x42, 0xE9, 0xA6, 0x37, 0xED, 0x6B, 0x0B, 0xFF,
    0x5C, 0xB6, 0xF4, 0x06, 0xB7, 0xED, 0xEE, 0x38, 0x6B, 0xFB, 0x5A, 0x89, 0x9F, 0xA5, 0xAE,
    0x9F, 0x24, 0x11, 0x7C, 0x4B, 0x1F, 0xE6, 0x49, 0x28, 0x66, 0x51, 0xEC, 0xE4, 0x5B, 0x3D,
    0xC2, 0x00, 0x7C, 0xB8, 0xA1, 0x63, 0xBF, 0x05, 0x98, 0xDA, 0x48, 0x36, 0x1C, 0x55, 0xD3,
    0x9A, 0x69, 0x16, 0x3F, 0xA8, 0xFD, 0x24, 0xCF, 0x5F, 0x83, 0x65, 0x5D, 0x23, 0xDC, 0xA3,
    0xAD, 0x96, 0x1C, 0x62, 0xF3, 0x56, 0x20, 0x85, 0x52, 0xBB, 0x9E, 0xD5, 0x29, 0x07, 0x70,
    0x96, 0x96, 0x6D, 0x67, 0x0C, 0x35, 0x4E, 0x4A, 0xBC, 0x98, 0x04, 0xF1, 0x74, 0x6C, 0x08,
    0xCA, 0x18, 0x21, 0x7C, 0x32, 0x90, 0x5E, 0x46, 0x2E, 0x36, 0xCE, 0x3B, 0xE3, 0x9E, 0x77,
    0x2C, 0x18, 0x0E, 0x86, 0x03, 0x9B, 0x27, 0x83, 0xA2, 0xEC, 0x07, 0xA2, 0x8F, 0xB5, 0xC5,
    0x5D, 0xF0, 0x6F, 0x4C, 0x52, 0xC9, 0xDE, 0x2B, 0xCB, 0xF6, 0x95, 0x58, 0x17, 0x18, 0x39,
    0x95, 0x49, 0x7C, 0xEA, 0x95, 0x6A, 0xE5, 0x15, 0xD2, 0x26, 0x18, 0x98, 0xFA, 0x05, 0x10,
    0x15, 0x72, 0x8E, 0x5A, 0x8A, 0xAA, 0xC4, 0x2D, 0xAD, 0x33, 0x17, 0x0D, 0x04, 0x50, 0x7A,
    0x33, 0xA8, 0x55, 0x21, 0xAB, 0xDF, 0x1C, 0xBA, 0x64, 0xEC, 0xFB, 0x85, 0x04, 0x58, 0xDB,
    0xEF, 0x0A, 0x8A, 0xEA, 0x71, 0x57, 0x5D, 0x06, 0x0C, 0x7D, 0xB3, 0x97, 0x0F, 0x85, 0xA6,
    0xE1, 0xE4, 0xC7, 0xAB, 0xF5, 0xAE, 0x8C, 0xDB, 0x09, 0x33, 0xD7, 0x1E, 0x8C, 0x94, 0xE0,
    0x4A, 0x25, 0x61, 0x9D, 0xCE, 0xE3, 0xD2, 0x26, 0x1A, 0xD2, 0xEE, 0x6B, 0xF1, 0x2F, 0xFA,
    0x06, 0xD9, 0x8A, 0x08, 0x64, 0xD8, 0x76, 0x02, 0x73, 0x3E, 0xC8, 0x6A, 0x64, 0x52, 0x1F,
    0x2B, 0x18, 0x17, 0x7B, 0x20, 0x0C, 0xBB, 0xE1, 0x17, 0x57, 0x7A, 0x61, 0x5D, 0x6C, 0x77,
    0x09, 0x88, 0xC0, 0xBA, 0xD9, 0x46, 0xE2, 0x08, 0xE2, 0x4F, 0xA0, 0x74, 0xE5, 0xAB, 0x31,
    0x43, 0xDB, 0x5B, 0xFC, 0xE0, 0xFD, 0x10, 0x8E, 0x4B, 0x82, 0xD1, 0x20, 0xA9, 0x3A, 0xD2,
    0xCA, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

constexpr uint8_t kSrpGenerator = 5;

size_t mpi_to_bytes_min(const mbedtls_mpi *mpi, uint8_t *buf, size_t len) {
  size_t mpi_size = mbedtls_mpi_size(mpi);
  if (mpi_size == 0) {
    if (len < 1) {
      return 0;
    }
    buf[0] = 0;
    return 1;
  }
  if (mpi_size > len) {
    return 0;
  }
  if (mbedtls_mpi_write_binary(mpi, buf, mpi_size) != 0) {
    return 0;
  }
  return mpi_size;
}

int mpi_to_bytes_padded(const mbedtls_mpi *mpi, uint8_t *buf, size_t len) {
  size_t mpi_size = mbedtls_mpi_size(mpi);
  if (mpi_size > len) {
    return -1;
  }
  std::memset(buf, 0, len);
  return mbedtls_mpi_write_binary(mpi, buf + (len - mpi_size), mpi_size);
}

void trim_leading_zeros(const uint8_t *in, size_t in_len, const uint8_t **out, size_t *out_len) {
  while (in_len > 1 && *in == 0) {
    in++;
    in_len--;
  }
  *out = in;
  *out_len = in_len;
}

// Compute M1 = H(H(N)^H(g) || H(I) || s || A || B || K)
void compute_m1(uint8_t *out, const uint8_t *h_ng_xor, const uint8_t *h_i, const uint8_t *salt,
                size_t salt_len, const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len,
                const uint8_t *k, size_t k_len) {
  crypto_hash_sha512_state state;
  crypto_hash_sha512_init(&state);
  crypto_hash_sha512_update(&state, h_ng_xor, 64);
  crypto_hash_sha512_update(&state, h_i, 64);
  crypto_hash_sha512_update(&state, salt, salt_len);
  crypto_hash_sha512_update(&state, a, a_len);
  crypto_hash_sha512_update(&state, b, b_len);
  crypto_hash_sha512_update(&state, k, k_len);
  crypto_hash_sha512_final(&state, out);
}

// A pointer to the memory-cleared copy of a secret used for cleanup on error.
void zero_bytes(void *ptr, size_t len) { sodium_memzero(ptr, len); }

}  // namespace

// ---------------------------------------------------------------------------
// SRP-6a client verification — dedicated helper so the mbedtls MPIs can be
// initialised/freed along a single cleanup path (C++ forbids goto past a
// non-vacuous initializer, so every local is declared default/vacuous at the
// top and assigned below).
// ---------------------------------------------------------------------------
static int srp_verify_client_internal(SrpSession *srp, const uint8_t *client_pk, size_t pk_len,
                                      const uint8_t *client_proof, size_t proof_len) {
  if (srp == nullptr || client_pk == nullptr || client_proof == nullptr ||
      proof_len < SRP_PROOF_BYTES) {
    return -1;
  }

  std::memset(srp->client_public_key, 0, SRP_PRIME_BYTES);
  size_t ck_len = pk_len;
  if (ck_len > SRP_PRIME_BYTES) {
    ck_len = SRP_PRIME_BYTES;
  }
  std::memcpy(srp->client_public_key + (SRP_PRIME_BYTES - ck_len), client_pk, ck_len);

  int ret = -1;

  mbedtls_mpi N, g, A, B, b, u, S, k, v, x, tmp, tmp2;
  mbedtls_mpi_init(&N);
  mbedtls_mpi_init(&g);
  mbedtls_mpi_init(&A);
  mbedtls_mpi_init(&B);
  mbedtls_mpi_init(&b);
  mbedtls_mpi_init(&u);
  mbedtls_mpi_init(&S);
  mbedtls_mpi_init(&k);
  mbedtls_mpi_init(&v);
  mbedtls_mpi_init(&x);
  mbedtls_mpi_init(&tmp);
  mbedtls_mpi_init(&tmp2);

  uint8_t s_bytes[SRP_PRIME_BYTES];
  uint8_t expected_m1[64];
  uint8_t ab_concat[SRP_PRIME_BYTES * 2];
  uint8_t hash_input[SRP_PRIME_BYTES * 2];
  uint8_t u_hash[64];
  uint8_t k_hash[64];
  uint8_t inner_hash[64];
  uint8_t x_hash[64];
  uint8_t h_n[64];
  uint8_t h_g[64];
  uint8_t h_ng_xor[64];
  uint8_t h_i[64];
  uint8_t a_bytes[SRP_PRIME_BYTES];
  uint8_t b_bytes[SRP_PRIME_BYTES];
  uint8_t g_byte;
  size_t s_len;
  size_t a_len;
  size_t b_len;
  size_t salt_len;
  const uint8_t *salt_ptr;

  if (mbedtls_mpi_read_binary(&N, kSrpN, sizeof(kSrpN)) != 0) {
    goto cleanup;
  }
  mbedtls_mpi_lset(&g, kSrpGenerator);
  if (mbedtls_mpi_read_binary(&A, srp->client_public_key, SRP_PRIME_BYTES) != 0) {
    goto cleanup;
  }
  mbedtls_mpi_read_binary(&B, srp->server_public_key, SRP_PRIME_BYTES);
  mbedtls_mpi_read_binary(&b, srp->server_secret, SRP_PRIME_BYTES);

  if (mbedtls_mpi_cmp_int(&A, 0) == 0) {
    ESP_LOGE(TAG, "Invalid client public key (zero)");
    goto cleanup;
  }
  mbedtls_mpi_mod_mpi(&tmp, &A, &N);
  if (mbedtls_mpi_cmp_int(&tmp, 0) == 0) {
    ESP_LOGE(TAG, "Invalid client public key (multiple of N)");
    goto cleanup;
  }

  // u = H(PAD(A) || PAD(B))
  std::memcpy(ab_concat, srp->client_public_key, SRP_PRIME_BYTES);
  std::memcpy(ab_concat + SRP_PRIME_BYTES, srp->server_public_key, SRP_PRIME_BYTES);
  crypto_hash_sha512(u_hash, ab_concat, sizeof(ab_concat));
  mbedtls_mpi_read_binary(&u, u_hash, 64);

  // k = H(N || pad(g))
  std::memset(hash_input, 0, sizeof(hash_input));
  std::memcpy(hash_input, kSrpN, SRP_PRIME_BYTES);
  hash_input[SRP_PRIME_BYTES * 2 - 1] = kSrpGenerator;
  crypto_hash_sha512(k_hash, hash_input, sizeof(hash_input));
  mbedtls_mpi_read_binary(&k, k_hash, 64);
  mbedtls_mpi_mod_mpi(&k, &k, &N);

  // x = H(s || H(I || ":" || P)) for "Pair-Setup:3939"
  crypto_hash_sha512_state state2;
  crypto_hash_sha512_init(&state2);
  crypto_hash_sha512_update(&state2, reinterpret_cast<const uint8_t *>("Pair-Setup"), 10);
  crypto_hash_sha512_update(&state2, reinterpret_cast<const uint8_t *>(":"), 1);
  crypto_hash_sha512_update(&state2, reinterpret_cast<const uint8_t *>("3939"), 4);
  crypto_hash_sha512_final(&state2, inner_hash);
  crypto_hash_sha512_init(&state2);
  crypto_hash_sha512_update(&state2, srp->salt, SRP_SALT_BYTES);
  crypto_hash_sha512_update(&state2, inner_hash, 64);
  crypto_hash_sha512_final(&state2, x_hash);
  mbedtls_mpi_read_binary(&x, x_hash, 64);

  if (mbedtls_mpi_exp_mod(&v, &g, &x, &N, nullptr) != 0) {
    goto cleanup;
  }
  // S = (A * v^u)^b mod N
  if (mbedtls_mpi_exp_mod(&tmp, &v, &u, &N, nullptr) != 0) {
    goto cleanup;
  }
  if (mbedtls_mpi_mul_mpi(&tmp2, &A, &tmp) != 0) {
    goto cleanup;
  }
  mbedtls_mpi_mod_mpi(&tmp2, &tmp2, &N);
  if (mbedtls_mpi_exp_mod(&S, &tmp2, &b, &N, nullptr) != 0) {
    goto cleanup;
  }

  s_len = mpi_to_bytes_min(&S, s_bytes, sizeof(s_bytes));
  crypto_hash_sha512(srp->session_key, s_bytes, s_len);
  srp->session_key_len = 64;

  // Expected M1 = H(H(N)^H(g) || H(I) || s || A || B || K)
  crypto_hash_sha512(h_n, kSrpN, sizeof(kSrpN));
  g_byte = kSrpGenerator;
  crypto_hash_sha512(h_g, &g_byte, 1);
  for (int i = 0; i < 64; i++) {
    h_ng_xor[i] = h_n[i] ^ h_g[i];
  }
  crypto_hash_sha512(h_i, reinterpret_cast<const uint8_t *>("Pair-Setup"), 10);

  trim_leading_zeros(srp->salt, SRP_SALT_BYTES, &salt_ptr, &salt_len);
  a_len = mpi_to_bytes_min(&A, a_bytes, sizeof(a_bytes));
  b_len = mpi_to_bytes_min(&B, b_bytes, sizeof(b_bytes));
  compute_m1(expected_m1, h_ng_xor, h_i, salt_ptr, salt_len, a_bytes, a_len, b_bytes, b_len,
             srp->session_key, 64);

  if (std::memcmp(client_proof, expected_m1, SRP_PROOF_BYTES) != 0) {
    ESP_LOGE(TAG, "Client proof verification failed");
    goto cleanup;
  }

  std::memcpy(srp->proof_m1, client_proof, SRP_PROOF_BYTES);
  a_len = mpi_to_bytes_min(&A, a_bytes, sizeof(a_bytes));
  crypto_hash_sha512_init(&state2);
  crypto_hash_sha512_update(&state2, a_bytes, a_len);
  crypto_hash_sha512_update(&state2, srp->proof_m1, SRP_PROOF_BYTES);
  crypto_hash_sha512_update(&state2, srp->session_key, srp->session_key_len);
  crypto_hash_sha512_final(&state2, srp->proof_m2);

  ret = 0;
  srp->verified = true;
  srp->state = 2;

cleanup:
  mbedtls_mpi_free(&N);
  mbedtls_mpi_free(&g);
  mbedtls_mpi_free(&A);
  mbedtls_mpi_free(&B);
  mbedtls_mpi_free(&b);
  mbedtls_mpi_free(&u);
  mbedtls_mpi_free(&S);
  mbedtls_mpi_free(&k);
  mbedtls_mpi_free(&v);
  mbedtls_mpi_free(&x);
  mbedtls_mpi_free(&tmp);
  mbedtls_mpi_free(&tmp2);
  return ret;
}

// ---------------------------------------------------------------------------
// CryptoModule
// ---------------------------------------------------------------------------
CryptoModule::CryptoModule() = default;
CryptoModule::~CryptoModule() {
  if (this->initialized_) {
    zero_bytes(this->device_secret_key_, sizeof(this->device_secret_key_));
  }
}

void CryptoModule::setup() {
  if (this->initialized_) {
    return;
  }

  if (sodium_init() < 0) {
    ESP_LOGE(TAG, "Failed to initialize libsodium");
    return;
  }

  // Persistent keypair blob (trivially copyable -> ESPHome preference).
  struct Keypair {
    uint8_t public_key[HAP_ED25519_PUBLIC_KEY_SIZE];
    uint8_t secret_key[HAP_ED25519_SECRET_KEY_SIZE];
  } kp;

  this->pref_keypair_ = global_preferences->make_preference<Keypair>(0x4150);  // "AP"
  bool loaded = this->pref_keypair_.load(&kp);

  if (!loaded) {
    crypto_sign_keypair(kp.public_key, kp.secret_key);
    if (!this->pref_keypair_.save(&kp)) {
      ESP_LOGW(TAG, "Failed to persist Ed25519 identity to preferences");
    }
    ESP_LOGI(TAG, "Generated new device Ed25519 identity");
  }

  std::memcpy(this->device_public_key_, kp.public_key, HAP_ED25519_PUBLIC_KEY_SIZE);
  std::memcpy(this->device_secret_key_, kp.secret_key, HAP_ED25519_SECRET_KEY_SIZE);
  zero_bytes(&kp, sizeof(kp));

  this->initialized_ = true;
}

void CryptoModule::loop() {  // no background work; handled per-session on demand
}

bool CryptoModule::paired() const { return this->paired_; }

const uint8_t *CryptoModule::device_public_key() const { return this->device_public_key_; }

HAPSession *CryptoModule::create_session() {
  if (!this->initialized_) {
    this->setup();
  }
  HAPSession *session = static_cast<HAPSession *>(airplay_calloc(1, sizeof(HAPSession), false));
  if (session == nullptr) {
    return nullptr;
  }

  std::memcpy(session->device_public_key, this->device_public_key_, HAP_ED25519_PUBLIC_KEY_SIZE);
  std::memcpy(session->device_secret_key, this->device_secret_key_, HAP_ED25519_SECRET_KEY_SIZE);

  crypto_box_keypair(session->session_public_key, session->session_secret_key);

  session->pair_verify_state = 0;
  session->pair_setup_state = 0;
  session->pair_setup_transient = false;
  session->session_established = false;
  session->encrypt_nonce = 0;
  session->decrypt_nonce = 0;
  session->srp = nullptr;
  return session;
}

void CryptoModule::free_session(HAPSession *session) {
  if (session == nullptr) {
    return;
  }
  if (session->srp != nullptr) {
    airplay_free(session->srp);
    session->srp = nullptr;
  }
  zero_bytes(session->device_secret_key, sizeof(session->device_secret_key));
  zero_bytes(session->session_secret_key, sizeof(session->session_secret_key));
  zero_bytes(session->shared_secret, sizeof(session->shared_secret));
  zero_bytes(session->encrypt_key, sizeof(session->encrypt_key));
  zero_bytes(session->decrypt_key, sizeof(session->decrypt_key));
  airplay_free(session);
}

// ---------------------------------------------------------------------------
// Pair-verify (TLV8) — AirPlay 2 transient pairing
// ---------------------------------------------------------------------------
int CryptoModule::pair_verify_m1(HAPSession *session, const uint8_t *input, size_t input_len,
                                 uint8_t *output, size_t output_capacity, size_t *output_len) {
  if (session == nullptr || input == nullptr || output == nullptr) {
    return -1;
  }
  size_t state_len = 0;
  const uint8_t *state = tlv8_find(input, input_len, TLV8_STATE, &state_len);
  if (state == nullptr || state_len != 1 || state[0] != PAIR_VERIFY_STATE_M1) {
    ESP_LOGE(TAG, "Invalid pair-verify M1 state");
    return -1;
  }
  size_t client_pk_len = 0;
  const uint8_t *client_pk = tlv8_find(input, input_len, TLV8_PUBLIC_KEY, &client_pk_len);
  if (client_pk == nullptr || client_pk_len != HAP_X25519_KEY_SIZE) {
    ESP_LOGE(TAG, "Invalid pair-verify M1 public key");
    return -1;
  }

  std::memcpy(session->client_public_key, client_pk, HAP_X25519_KEY_SIZE);
  if (crypto_scalarmult(session->shared_secret, session->session_secret_key,
                        session->client_public_key) != 0) {
    ESP_LOGE(TAG, "X25519 key exchange failed");
    return -1;
  }

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char device_id[18];
  std::snprintf(device_id, sizeof(device_id), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
                mac[2], mac[3], mac[4], mac[5]);
  const size_t device_id_len = 17;

  uint8_t accessory_info[128];
  size_t accessory_info_len = 0;
  std::memcpy(accessory_info + accessory_info_len, session->session_public_key, HAP_X25519_KEY_SIZE);
  accessory_info_len += HAP_X25519_KEY_SIZE;
  std::memcpy(accessory_info + accessory_info_len, device_id, device_id_len);
  accessory_info_len += device_id_len;
  std::memcpy(accessory_info + accessory_info_len, session->client_public_key, HAP_X25519_KEY_SIZE);
  accessory_info_len += HAP_X25519_KEY_SIZE;

  uint8_t signature[crypto_sign_BYTES];
  std::memset(signature, 0, sizeof(signature));
  crypto_sign_detached(signature, nullptr, accessory_info, accessory_info_len,
                       session->device_secret_key);

  uint8_t sub_tlv[256];
  Tlv8Encoder sub_enc;
  tlv8_encoder_init(&sub_enc, sub_tlv, sizeof(sub_tlv));
  tlv8_encode(&sub_enc, TLV8_IDENTIFIER, reinterpret_cast<const uint8_t *>(device_id),
              device_id_len);
  tlv8_encode(&sub_enc, TLV8_SIGNATURE, signature, crypto_sign_BYTES);

  uint8_t session_key[HAP_CHACHA20_KEY_SIZE];
  hap_hkdf_sha512(reinterpret_cast<const uint8_t *>("Pair-Verify-Encrypt-Salt"), 24,
                  session->shared_secret, HAP_X25519_KEY_SIZE,
                  reinterpret_cast<const uint8_t *>("Pair-Verify-Encrypt-Info"), 24, session_key,
                  sizeof(session_key));

  uint8_t nonce[HAP_CHACHA20_NONCE_SIZE] = {0, 0, 0, 0, 'P', 'V', '-', 'M', 's', 'g', '0', '2'};
  uint8_t encrypted[256 + crypto_aead_chacha20poly1305_ietf_ABYTES];
  unsigned long long encrypted_len = 0;
  crypto_aead_chacha20poly1305_ietf_encrypt(encrypted, &encrypted_len, sub_tlv,
                                            tlv8_encoder_size(&sub_enc), nullptr, 0, nullptr, nonce,
                                            session_key);

  Tlv8Encoder enc;
  tlv8_encoder_init(&enc, output, output_capacity);
  tlv8_encode_byte(&enc, TLV8_STATE, PAIR_VERIFY_STATE_M2);
  tlv8_encode(&enc, TLV8_PUBLIC_KEY, session->session_public_key, HAP_X25519_KEY_SIZE);
  tlv8_encode(&enc, TLV8_ENCRYPTED_DATA, encrypted, static_cast<size_t>(encrypted_len));

  *output_len = tlv8_encoder_size(&enc);
  session->pair_verify_state = PAIR_VERIFY_STATE_M2;
  std::memcpy(session->encrypt_key, session_key, sizeof(session_key));
  return 0;
}

int CryptoModule::pair_verify_m3(HAPSession *session, const uint8_t *input, size_t input_len,
                                 uint8_t *output, size_t output_capacity, size_t *output_len) {
  if (session == nullptr || input == nullptr || output == nullptr) {
    return -1;
  }
  size_t state_len = 0;
  const uint8_t *state = tlv8_find(input, input_len, TLV8_STATE, &state_len);
  if (state == nullptr || state_len != 1 || state[0] != PAIR_VERIFY_STATE_M3) {
    ESP_LOGE(TAG, "Invalid pair-verify M3 state");
    return -1;
  }

  uint8_t encrypted[512];
  size_t encrypted_len = 0;
  if (!tlv8_decode_concat(input, input_len, TLV8_ENCRYPTED_DATA, encrypted, sizeof(encrypted),
                          &encrypted_len)) {
    ESP_LOGE(TAG, "Missing pair-verify M3 encrypted data");
    return -1;
  }

  uint8_t nonce[HAP_CHACHA20_NONCE_SIZE] = {0, 0, 0, 0, 'P', 'V', '-', 'M', 's', 'g', '0', '3'};
  uint8_t decrypted[512];
  unsigned long long decrypted_len = 0;
  if (crypto_aead_chacha20poly1305_ietf_decrypt(decrypted, &decrypted_len, nullptr, encrypted,
                                                encrypted_len, nullptr, 0, nonce,
                                                session->encrypt_key) != 0) {
    ESP_LOGE(TAG, "Pair-verify M3 decryption failed");
    Tlv8Encoder enc;
    tlv8_encoder_init(&enc, output, output_capacity);
    tlv8_encode_byte(&enc, TLV8_STATE, PAIR_VERIFY_STATE_M4);
    tlv8_encode_byte(&enc, TLV8_ERROR, TLV8_ERROR_AUTHENTICATION);
    *output_len = tlv8_encoder_size(&enc);
    return -1;
  }

  hap_hkdf_sha512(reinterpret_cast<const uint8_t *>("Control-Salt"), 12, session->shared_secret,
                  HAP_X25519_KEY_SIZE, reinterpret_cast<const uint8_t *>("Control-Read-Encryption-Key"),
                  27, session->encrypt_key, 32);
  hap_hkdf_sha512(reinterpret_cast<const uint8_t *>("Control-Salt"), 12, session->shared_secret,
                  HAP_X25519_KEY_SIZE,
                  reinterpret_cast<const uint8_t *>("Control-Write-Encryption-Key"), 28,
                  session->decrypt_key, 32);

  Tlv8Encoder enc;
  tlv8_encoder_init(&enc, output, output_capacity);
  tlv8_encode_byte(&enc, TLV8_STATE, PAIR_VERIFY_STATE_M4);

  *output_len = tlv8_encoder_size(&enc);
  session->pair_verify_state = PAIR_VERIFY_STATE_M4;
  session->session_established = true;
  this->paired_ = true;
  return 0;
}

// ---------------------------------------------------------------------------
// Pair-verify raw (non-TLV AirPlay 2 format). AES-CTR is used only to protect
// the Ed25519 signature during this legacy transport — it is NOT audio
// encryption (no AES-CBC audio path is implemented).
// ---------------------------------------------------------------------------
int CryptoModule::pair_verify_m1_raw(HAPSession *session, const uint8_t *input, size_t input_len,
                                     uint8_t *output, size_t output_capacity, size_t *output_len) {
  if (session == nullptr || input == nullptr || output == nullptr) {
    return -1;
  }
  const uint8_t *client_epk;
  if (input_len >= 68) {
    client_epk = input + 4;
  } else if (input_len >= 64) {
    client_epk = input;
  } else if (input_len >= 32) {
    client_epk = input;
  } else {
    ESP_LOGE(TAG, "Input too short for raw pair-verify: %zu", input_len);
    return -1;
  }

  std::memcpy(session->client_public_key, client_epk, HAP_X25519_KEY_SIZE);
  if (crypto_scalarmult(session->shared_secret, session->session_secret_key,
                        session->client_public_key) != 0) {
    ESP_LOGE(TAG, "X25519 key exchange failed");
    return -1;
  }

  uint8_t aes_key[16];
  uint8_t aes_iv[16];
  crypto_hash_sha512_state state;
  uint8_t hash[64];
  crypto_hash_sha512_init(&state);
  crypto_hash_sha512_update(&state, reinterpret_cast<const uint8_t *>("Pair-Verify-AES-Key"), 19);
  crypto_hash_sha512_update(&state, session->shared_secret, 32);
  crypto_hash_sha512_final(&state, hash);
  std::memcpy(aes_key, hash, sizeof(aes_key));

  crypto_hash_sha512_init(&state);
  crypto_hash_sha512_update(&state, reinterpret_cast<const uint8_t *>("Pair-Verify-AES-IV"), 18);
  crypto_hash_sha512_update(&state, session->shared_secret, 32);
  crypto_hash_sha512_final(&state, hash);
  std::memcpy(aes_iv, hash, sizeof(aes_iv));

  uint8_t signed_data[64];
  std::memcpy(signed_data, session->session_public_key, 32);
  std::memcpy(signed_data + 32, session->client_public_key, 32);

  uint8_t signature[64];
  std::memset(signature, 0, sizeof(signature));
  crypto_sign_detached(signature, nullptr, signed_data, sizeof(signed_data),
                       session->device_secret_key);

  if (output_capacity < 96) {
    ESP_LOGE(TAG, "Output buffer too small for raw M2 (need 96, have %zu)", output_capacity);
    return -1;
  }

  std::memcpy(output, session->session_public_key, 32);

  mbedtls_aes_context aes_ctx;
  mbedtls_aes_init(&aes_ctx);
  mbedtls_aes_setkey_enc(&aes_ctx, aes_key, 128);
  uint8_t stream_block[16] = {0};
  size_t nc_off = 0;
  uint8_t nonce_counter[16];
  std::memcpy(nonce_counter, aes_iv, sizeof(nonce_counter));
  mbedtls_aes_crypt_ctr(&aes_ctx, sizeof(signature), &nc_off, nonce_counter, stream_block, signature,
                        output + 32);
  mbedtls_aes_free(&aes_ctx);

  *output_len = 96;
  session->pair_verify_state = PAIR_VERIFY_STATE_M2;
  std::memcpy(session->encrypt_key, aes_key, sizeof(aes_key));
  std::memcpy(session->decrypt_key, aes_iv, sizeof(aes_iv));
  return 0;
}

int CryptoModule::pair_verify_m3_raw(HAPSession *session, const uint8_t *input, size_t input_len,
                                     uint8_t *output, size_t output_capacity, size_t *output_len) {
  (void)output_capacity;
  if (session == nullptr || input == nullptr) {
    return -1;
  }
  const uint8_t *encrypted_sig;
  if (input_len >= 68) {
    encrypted_sig = input + 4;
  } else if (input_len >= 64) {
    encrypted_sig = input;
  } else {
    ESP_LOGE(TAG, "Input too short for raw M3: %zu", input_len);
    return -1;
  }

  uint8_t aes_key[16];
  uint8_t aes_iv[16];
  crypto_hash_sha512_state state;
  uint8_t hash[64];
  crypto_hash_sha512_init(&state);
  crypto_hash_sha512_update(&state, reinterpret_cast<const uint8_t *>("Pair-Verify-AES-Key"), 19);
  crypto_hash_sha512_update(&state, session->shared_secret, 32);
  crypto_hash_sha512_final(&state, hash);
  std::memcpy(aes_key, hash, sizeof(aes_key));

  crypto_hash_sha512_init(&state);
  crypto_hash_sha512_update(&state, reinterpret_cast<const uint8_t *>("Pair-Verify-AES-IV"), 18);
  crypto_hash_sha512_update(&state, session->shared_secret, 32);
  crypto_hash_sha512_final(&state, hash);
  std::memcpy(aes_iv, hash, sizeof(aes_iv));

  // Decrypt the client's payload. It is a stream cipher so the length can be
  // less than a block. The raw M3 payload is the AES-CTR-encrypted
  // [client Ed25519 public key (32) | Ed25519 signature (64)] = 96 bytes; some
  // senders carry only the 64-byte signature. Decrypt whatever is present.
  uint8_t client_payload[96];
  size_t payload_len = (input_len >= 68) ? input_len - 4 : input_len;
  if (payload_len < 64 || payload_len > sizeof(client_payload)) {
    ESP_LOGE(TAG, "Raw pair-verify M3 payload length invalid: %zu", payload_len);
    return -1;
  }

  std::memset(client_payload, 0, sizeof(client_payload));
  mbedtls_aes_context aes_ctx;
  mbedtls_aes_init(&aes_ctx);
  mbedtls_aes_setkey_enc(&aes_ctx, aes_key, 128);
  uint8_t stream_block[16] = {0};
  size_t nc_off = 0;
  uint8_t nonce_counter[16];
  std::memcpy(nonce_counter, aes_iv, sizeof(nonce_counter));
  mbedtls_aes_crypt_ctr(&aes_ctx, payload_len, &nc_off, nonce_counter, stream_block,
                        encrypted_sig, client_payload);
  mbedtls_aes_free(&aes_ctx);

  // Verify the peer's Ed25519 signature so that anyone on the LAN cannot play.
  // The device signs session_public_key || client_public_key in M1; the client
  // signs the same 64-byte blob. The verification key is the client's Ed25519
  // public key carried in the payload when present (96-byte layout), else the
  // peer public key held in the session.
  const uint8_t *peer_signature = client_payload;
  const uint8_t *peer_public_key = session->client_public_key;
  if (payload_len >= 96) {
    peer_public_key = client_payload;         // client Ed25519 public key
    peer_signature = client_payload + 32;     // client Ed25519 signature (64)
  }

  uint8_t signed_data[64];
  std::memcpy(signed_data, session->session_public_key, 32);
  std::memcpy(signed_data + 32, session->client_public_key, 32);

  if (crypto_sign_verify_detached(peer_signature, signed_data, sizeof(signed_data),
                                  peer_public_key) != 0) {
    ESP_LOGE(TAG, "Peer Ed25519 signature verification failed; rejecting session");
    return -1;
  }
  ESP_LOGI(TAG, "Peer Ed25519 signature verified");

  hap_hkdf_sha512(reinterpret_cast<const uint8_t *>("Control-Salt"), 12, session->shared_secret,
                  HAP_X25519_KEY_SIZE, reinterpret_cast<const uint8_t *>("Control-Read-Encryption-Key"),
                  27, session->decrypt_key, 32);
  hap_hkdf_sha512(reinterpret_cast<const uint8_t *>("Control-Salt"), 12, session->shared_secret,
                  HAP_X25519_KEY_SIZE,
                  reinterpret_cast<const uint8_t *>("Control-Write-Encryption-Key"), 28,
                  session->encrypt_key, 32);

  session->encrypt_nonce = 0;
  session->decrypt_nonce = 0;
  *output_len = 0;
  session->pair_verify_state = PAIR_VERIFY_STATE_M4;
  session->session_established = true;
  this->paired_ = true;
  return 0;
}

// ---------------------------------------------------------------------------
// Pair-setup (SRP-6a 3072-bit + Ed25519) — AirPlay 2 transient pairing
// ---------------------------------------------------------------------------
static constexpr size_t kSrpSetupEncryptSaltLen = 23;  // "Pair-Setup-Encrypt-Salt"
static constexpr size_t kSrpSetupEncryptInfoLen = 23;  // "Pair-Setup-Encrypt-Info"

int CryptoModule::pair_setup_m1(HAPSession *session, const uint8_t *input, size_t input_len,
                                uint8_t *output, size_t output_capacity, size_t *output_len) {
  if (session == nullptr || input == nullptr || output == nullptr) {
    return -1;
  }
  size_t state_len = 0;
  const uint8_t *state = tlv8_find(input, input_len, TLV8_STATE, &state_len);
  if (state == nullptr || state_len != 1 || state[0] != PAIR_SETUP_STATE_M1) {
    ESP_LOGE(TAG, "Invalid pair-setup M1 state");
    return -1;
  }

  // AirPlay 2 uses TLV type 0x13 for the flags field.
  size_t flags_len = 0;
  const uint8_t *flags = tlv8_find(input, input_len, 0x13, &flags_len);
  bool transient = false;
  if (flags != nullptr && flags_len == 1) {
    transient = (flags[0] & 0x10) != 0;
  }
  session->pair_setup_transient = transient;

  if (session->srp != nullptr) {
    airplay_free(session->srp);
    session->srp = nullptr;
  }
  session->srp = static_cast<SrpSession *>(airplay_calloc(1, sizeof(SrpSession), false));
  if (session->srp == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate SRP session");
    return -1;
  }

  const char *password = transient ? "3939" : "0000";

  // srp_start
  {
    SrpSession *srp = session->srp;
    mbedtls_mpi N, g, k, v, b, B, x, tmp, tmp2;
    mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&g);
    mbedtls_mpi_init(&k);
    mbedtls_mpi_init(&v);
    mbedtls_mpi_init(&b);
    mbedtls_mpi_init(&B);
    mbedtls_mpi_init(&x);
    mbedtls_mpi_init(&tmp);
    mbedtls_mpi_init(&tmp2);
    int ret = -1;

    esp_fill_random(srp->salt, SRP_SALT_BYTES);
    if (mbedtls_mpi_read_binary(&N, kSrpN, sizeof(kSrpN)) != 0) {
      goto srp_start_cleanup;
    }
    mbedtls_mpi_lset(&g, kSrpGenerator);

    // k = H(N || pad(g))
    {
      uint8_t hash_input[SRP_PRIME_BYTES * 2];
      std::memset(hash_input, 0, sizeof(hash_input));
      std::memcpy(hash_input, kSrpN, SRP_PRIME_BYTES);
      hash_input[SRP_PRIME_BYTES * 2 - 1] = kSrpGenerator;
      uint8_t k_hash[64];
      crypto_hash_sha512(k_hash, hash_input, sizeof(hash_input));
      mbedtls_mpi_read_binary(&k, k_hash, 64);
      mbedtls_mpi_mod_mpi(&k, &k, &N);
    }

    // x = H(s || H(I || ":" || P))
    {
      uint8_t inner_hash[64];
      crypto_hash_sha512_state state2;
      crypto_hash_sha512_init(&state2);
      crypto_hash_sha512_update(&state2, reinterpret_cast<const uint8_t *>("Pair-Setup"), 10);
      crypto_hash_sha512_update(&state2, reinterpret_cast<const uint8_t *>(":"), 1);
      crypto_hash_sha512_update(&state2, reinterpret_cast<const uint8_t *>(password),
                                std::strlen(password));
      crypto_hash_sha512_final(&state2, inner_hash);

      uint8_t x_hash[64];
      crypto_hash_sha512_init(&state2);
      crypto_hash_sha512_update(&state2, srp->salt, SRP_SALT_BYTES);
      crypto_hash_sha512_update(&state2, inner_hash, 64);
      crypto_hash_sha512_final(&state2, x_hash);
      mbedtls_mpi_read_binary(&x, x_hash, 64);
    }

    if (mbedtls_mpi_exp_mod(&v, &g, &x, &N, nullptr) != 0) {
      goto srp_start_cleanup;
    }

    {
      uint8_t b_bytes[SRP_PRIME_BYTES];
      esp_fill_random(b_bytes, sizeof(b_bytes));
      mbedtls_mpi_read_binary(&b, b_bytes, sizeof(b_bytes));
      mbedtls_mpi_mod_mpi(&b, &b, &N);
      mpi_to_bytes_padded(&b, srp->server_secret, SRP_PRIME_BYTES);
    }

    if (mbedtls_mpi_exp_mod(&tmp, &g, &b, &N, nullptr) != 0) {
      goto srp_start_cleanup;
    }
    if (mbedtls_mpi_mul_mpi(&tmp2, &k, &v) != 0) {
      goto srp_start_cleanup;
    }
    if (mbedtls_mpi_add_mpi(&B, &tmp2, &tmp) != 0) {
      goto srp_start_cleanup;
    }
    mbedtls_mpi_mod_mpi(&B, &B, &N);
    mpi_to_bytes_padded(&B, srp->server_public_key, SRP_PRIME_BYTES);
    srp->state = 1;
    ret = 0;

  srp_start_cleanup:
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&g);
    mbedtls_mpi_free(&k);
    mbedtls_mpi_free(&v);
    mbedtls_mpi_free(&b);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&x);
    mbedtls_mpi_free(&tmp);
    mbedtls_mpi_free(&tmp2);
    if (ret != 0) {
      airplay_free(session->srp);
      session->srp = nullptr;
      ESP_LOGE(TAG, "Failed to start SRP");
      return -1;
    }
  }

  size_t pk_len = SRP_PRIME_BYTES;
  const uint8_t *pk = session->srp->server_public_key;

  Tlv8Encoder enc;
  tlv8_encoder_init(&enc, output, output_capacity);
  tlv8_encode_byte(&enc, TLV8_STATE, PAIR_SETUP_STATE_M2);
  tlv8_encode(&enc, TLV8_SALT, session->srp->salt, SRP_SALT_BYTES);
  tlv8_encode(&enc, TLV8_PUBLIC_KEY, pk, pk_len);

  *output_len = tlv8_encoder_size(&enc);
  session->pair_setup_state = PAIR_SETUP_STATE_M2;
  return 0;
}

int CryptoModule::pair_setup_m3(HAPSession *session, const uint8_t *input, size_t input_len,
                                uint8_t *output, size_t output_capacity, size_t *output_len) {
  if (session == nullptr || input == nullptr || output == nullptr) {
    return -1;
  }
  if (session->srp == nullptr) {
    ESP_LOGE(TAG, "No SRP session for pair-setup M3");
    return -1;
  }
  size_t state_len = 0;
  const uint8_t *state = tlv8_find(input, input_len, TLV8_STATE, &state_len);
  if (state == nullptr || state_len != 1 || state[0] != PAIR_SETUP_STATE_M3) {
    ESP_LOGE(TAG, "Invalid pair-setup M3 state");
    return -1;
  }

  uint8_t client_pk[512];
  size_t pk_len = 0;
  if (!tlv8_decode_concat(input, input_len, TLV8_PUBLIC_KEY, client_pk, sizeof(client_pk), &pk_len)) {
    ESP_LOGE(TAG, "Missing client public key in pair-setup M3");
    return -1;
  }
  uint8_t client_proof[64];
  size_t proof_len = 0;
  if (!tlv8_decode_concat(input, input_len, TLV8_PROOF, client_proof, sizeof(client_proof),
                          &proof_len)) {
    ESP_LOGE(TAG, "Missing client proof in pair-setup M3");
    return -1;
  }

  // ---- srp_verify_client ----
  SrpSession *srp = session->srp;
  int verify_ret = srp_verify_client_internal(srp, client_pk, pk_len, client_proof, proof_len);
  if (verify_ret != 0) {
    ESP_LOGE(TAG, "Client verification failed");
    Tlv8Encoder enc;
    tlv8_encoder_init(&enc, output, output_capacity);
    tlv8_encode_byte(&enc, TLV8_STATE, PAIR_SETUP_STATE_M4);
    tlv8_encode_byte(&enc, TLV8_ERROR, 0x02);
    *output_len = tlv8_encoder_size(&enc);
    return 0;  // M4 error is a normal protocol response, not a hard failure.
  }

  if (session->pair_setup_transient) {
    const uint8_t *srp_key = srp->session_key;
    size_t srp_key_len = srp->session_key_len;
    if (srp_key == nullptr || srp_key_len == 0) {
      ESP_LOGE(TAG, "Missing SRP session key for transient pairing");
      return -1;
    }
    std::memcpy(session->shared_secret, srp_key, 32);
    hap_hkdf_sha512(reinterpret_cast<const uint8_t *>("Control-Salt"), 12, srp_key, srp_key_len,
                    reinterpret_cast<const uint8_t *>("Control-Read-Encryption-Key"), 27,
                    session->encrypt_key, 32);
    hap_hkdf_sha512(reinterpret_cast<const uint8_t *>("Control-Salt"), 12, srp_key, srp_key_len,
                    reinterpret_cast<const uint8_t *>("Control-Write-Encryption-Key"), 28,
                    session->decrypt_key, 32);
    session->encrypt_nonce = 0;
    session->decrypt_nonce = 0;
    session->session_established = true;
    this->paired_ = true;
  }

  Tlv8Encoder enc;
  tlv8_encoder_init(&enc, output, output_capacity);
  tlv8_encode_byte(&enc, TLV8_STATE, PAIR_SETUP_STATE_M4);
  tlv8_encode(&enc, TLV8_PROOF, srp->proof_m2, SRP_PROOF_BYTES);

  *output_len = tlv8_encoder_size(&enc);
  session->pair_setup_state = PAIR_SETUP_STATE_M4;
  return 0;
}

int CryptoModule::pair_setup_m5(HAPSession *session, const uint8_t *input, size_t input_len,
                                uint8_t *output, size_t output_capacity, size_t *output_len) {
  (void)output;
  (void)output_capacity;
  (void)output_len;
  if (session == nullptr || input == nullptr) {
    return -1;
  }
  if (session->srp == nullptr) {
    ESP_LOGE(TAG, "No SRP session for pair-setup M5");
    return -1;
  }
  size_t state_len = 0;
  const uint8_t *state = tlv8_find(input, input_len, TLV8_STATE, &state_len);
  if (state == nullptr || state_len != 1 || state[0] != PAIR_SETUP_STATE_M5) {
    ESP_LOGE(TAG, "Invalid pair-setup M5 state");
    return -1;
  }

  uint8_t encrypted[512];
  size_t encrypted_len = 0;
  if (!tlv8_decode_concat(input, input_len, TLV8_ENCRYPTED_DATA, encrypted, sizeof(encrypted),
                          &encrypted_len)) {
    ESP_LOGE(TAG, "Missing encrypted data in pair-setup M5");
    return -1;
  }

  size_t srp_key_len = session->srp->session_key_len;
  const uint8_t *srp_key = session->srp->session_key;
  if (srp_key == nullptr || srp_key_len == 0) {
    ESP_LOGE(TAG, "Missing SRP session key");
    return -1;
  }

  uint8_t setup_key[32];
  hap_hkdf_sha512(reinterpret_cast<const uint8_t *>("Pair-Setup-Encrypt-Salt"), kSrpSetupEncryptSaltLen,
                  srp_key, srp_key_len,
                  reinterpret_cast<const uint8_t *>("Pair-Setup-Encrypt-Info"), kSrpSetupEncryptInfoLen,
                  setup_key, sizeof(setup_key));

  uint8_t nonce[HAP_CHACHA20_NONCE_SIZE] = {0, 0, 0, 0, 'P', 'S', '-', 'M', 's', 'g', '0', '5'};
  uint8_t decrypted[512];
  unsigned long long decrypted_len = 0;
  if (crypto_aead_chacha20poly1305_ietf_decrypt(decrypted, &decrypted_len, nullptr, encrypted,
                                                encrypted_len, nullptr, 0, nonce, setup_key) != 0) {
    ESP_LOGE(TAG, "Pair-setup M5 decryption failed");
    return -1;
  }

  session->encrypt_nonce = 0;
  session->decrypt_nonce = 0;
  return 0;
}

bool CryptoModule::is_pair_setup_transient(HAPSession *session) const {
  return session != nullptr && session->pair_setup_transient;
}

// ---------------------------------------------------------------------------
// Established-session control-channel ChaCha20-Poly1305
// ---------------------------------------------------------------------------
int CryptoModule::session_encrypt(HAPSession *session, const uint8_t *plaintext, size_t plaintext_len,
                                  const uint8_t *aad, size_t aad_len, uint8_t *ciphertext,
                                  size_t *ciphertext_len) {
  if (session == nullptr || plaintext == nullptr || ciphertext == nullptr ||
      !session->session_established) {
    return -1;
  }
  uint8_t nonce[HAP_CHACHA20_NONCE_SIZE] = {0};
  std::memcpy(nonce + 4, &session->encrypt_nonce, 8);
  unsigned long long ct_len = 0;
  crypto_aead_chacha20poly1305_ietf_encrypt(ciphertext, &ct_len, plaintext, plaintext_len, aad,
                                            aad_len, nullptr, nonce, session->encrypt_key);
  *ciphertext_len = static_cast<size_t>(ct_len);
  session->encrypt_nonce++;
  return 0;
}

int CryptoModule::session_decrypt(HAPSession *session, const uint8_t *ciphertext, size_t ciphertext_len,
                                  const uint8_t *aad, size_t aad_len, uint8_t *plaintext,
                                  size_t *plaintext_len) {
  if (session == nullptr || ciphertext == nullptr || plaintext == nullptr ||
      !session->session_established) {
    return -1;
  }
  uint8_t nonce[HAP_CHACHA20_NONCE_SIZE] = {0};
  std::memcpy(nonce + 4, &session->decrypt_nonce, 8);
  unsigned long long pt_len = 0;
  if (crypto_aead_chacha20poly1305_ietf_decrypt(plaintext, &pt_len, nullptr, ciphertext,
                                                ciphertext_len, aad, aad_len, nonce,
                                                session->decrypt_key) != 0) {
    return -1;
  }
  *plaintext_len = static_cast<size_t>(pt_len);
  session->decrypt_nonce++;
  return 0;
}

// ---------------------------------------------------------------------------
// Audio encryption setup + ChaCha20-Poly1305 frame decrypt
// ---------------------------------------------------------------------------
int CryptoModule::derive_audio_key(HAPSession *session, uint8_t *audio_key, size_t key_len) {
  if (session == nullptr || audio_key == nullptr || key_len < 16) {
    return -1;
  }
  if (!session->session_established) {
    ESP_LOGW(TAG, "Cannot derive audio key before session established");
    return -1;
  }
  hap_hkdf_sha512(reinterpret_cast<const uint8_t *>("Control-Salt"), 12, session->shared_secret, 32,
                  reinterpret_cast<const uint8_t *>("Control-Read-Encryption-Key"), 27, audio_key,
                  key_len);
  return 0;
}

int CryptoModule::configure_audio_encryption(HAPSession *session, const uint8_t *ekey,
                                             size_t ekey_len, const uint8_t *eiv, size_t eiv_len,
                                             const uint8_t *shk, size_t shk_len,
                                             AudioEncrypt *out) {
  if (out == nullptr) {
    return -1;
  }
  out->type = AudioEncryptType::NOT_SET;
  out->key_len = 0;

  if (shk != nullptr && shk_len >= 16) {
    out->type = AudioEncryptType::CHACHA20_POLY1305;
    size_t n = shk_len > 32 ? 32 : shk_len;
    std::memcpy(out->key, shk, n);
    out->key_len = n;
    if (eiv != nullptr && eiv_len >= 16) {
      std::memcpy(out->iv, eiv, 16);
    }
    return 0;
  }

  if (ekey != nullptr && ekey_len > 16 && session != nullptr && session->session_established) {
    uint8_t nonce[HAP_CHACHA20_NONCE_SIZE] = {0};
    uint8_t decrypted_key[32];
    unsigned long long decrypted_len = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(decrypted_key, &decrypted_len, nullptr, ekey,
                                                  ekey_len, nullptr, 0, nonce,
                                                  session->shared_secret) == 0 &&
        decrypted_len >= 16) {
      out->type = AudioEncryptType::CHACHA20_POLY1305;
      size_t n = decrypted_len > 32 ? 32 : static_cast<size_t>(decrypted_len);
      std::memcpy(out->key, decrypted_key, n);
      out->key_len = n;
      if (eiv != nullptr && eiv_len >= 16) {
        std::memcpy(out->iv, eiv, 16);
      }
      return 0;
    }
  }

  if (session != nullptr && session->session_established) {
    uint8_t derived[HAP_CHACHA20_KEY_SIZE];
    if (this->derive_audio_key(session, derived, sizeof(derived)) == 0) {
      out->type = AudioEncryptType::CHACHA20_POLY1305;
      std::memcpy(out->key, derived, sizeof(derived));
      out->key_len = sizeof(derived);
      if (eiv != nullptr && eiv_len >= 16) {
        std::memcpy(out->iv, eiv, 16);
      }
      return 0;
    }
  }

  return -1;
}

int CryptoModule::audio_decrypt_rtp(const AudioEncrypt *encrypt, const uint8_t *input,
                                    size_t input_len, uint8_t *output, size_t output_capacity,
                                    const uint8_t *full_packet, size_t full_packet_len) {
  if (encrypt == nullptr || input == nullptr || output == nullptr) {
    return -1;
  }
  if (encrypt->type == AudioEncryptType::NOT_SET) {
    if (input_len > output_capacity) {
      return -1;
    }
    std::memcpy(output, input, input_len);
    return static_cast<int>(input_len);
  }
  if (encrypt->type != AudioEncryptType::CHACHA20_POLY1305) {
    return -1;
  }
  if (full_packet == nullptr || full_packet_len < 12) {
    return -1;
  }
  if (input_len < crypto_aead_chacha20poly1305_ietf_ABYTES + 8) {
    return -1;
  }
  uint8_t nonce[HAP_CHACHA20_NONCE_SIZE] = {0};
  std::memcpy(nonce + 4, full_packet + full_packet_len - 8, 8);
  const uint8_t *aad = full_packet + 4;
  size_t aad_len = 8;
  size_t ciphertext_len = input_len - 8;
  if (ciphertext_len > output_capacity) {
    return -1;
  }
  unsigned long long decrypted_len = 0;
  int ret = crypto_aead_chacha20poly1305_ietf_decrypt(
      output, &decrypted_len, nullptr, input, ciphertext_len, aad, aad_len, nonce, encrypt->key);
  if (ret != 0) {
    return -1;
  }
  return static_cast<int>(decrypted_len);
}

int CryptoModule::audio_decrypt_buffered(const AudioEncrypt *encrypt, const uint8_t *packet,
                                         size_t packet_len, uint8_t *output, size_t output_capacity) {
  if (packet == nullptr || output == nullptr) {
    return -1;
  }
  if (encrypt == nullptr || encrypt->type != AudioEncryptType::CHACHA20_POLY1305) {
    if (packet_len <= 12) {
      return -1;
    }
    size_t payload_len = packet_len - 12;
    if (payload_len > output_capacity) {
      return -1;
    }
    std::memcpy(output, packet + 12, payload_len);
    return static_cast<int>(payload_len);
  }
  if (packet_len < 36) {
    return -1;
  }
  uint8_t nonce[HAP_CHACHA20_NONCE_SIZE] = {0};
  std::memcpy(nonce + 4, packet + packet_len - 8, 8);
  const uint8_t *aad = packet + 4;
  size_t aad_len = 8;
  const uint8_t *ciphertext = packet + 12;
  size_t ciphertext_len = packet_len - 12 - 8;
  if (ciphertext_len > output_capacity + crypto_aead_chacha20poly1305_ietf_ABYTES) {
    return -1;
  }
  unsigned long long decrypted_len = 0;
  int ret = crypto_aead_chacha20poly1305_ietf_decrypt(
      output, &decrypted_len, nullptr, ciphertext, ciphertext_len, aad, aad_len, nonce, encrypt->key);
  if (ret != 0) {
    return -1;
  }
  return static_cast<int>(decrypted_len);
}

}  // namespace airplay_receiver
}  // namespace esphome
