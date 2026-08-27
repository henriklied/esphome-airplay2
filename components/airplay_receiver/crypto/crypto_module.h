#pragma once
// airplay_receiver crypto module — AirPlay 2 HomeKit pairing + ChaCha20-Poly1305
// audio encryption (port of rbouteiller/airplay-esp32 main/hap + the AirPlay 2
// ChaCha20-Poly1305 path of main/audio/audio_crypto.c).
//
// AirPlay 2 ONLY. The AirPlay 1 (RAOP) path is intentionally NOT ported:
//   * main/rtsp/rtsp_rsa.c (RSA auth) is skipped;
//   * AES-CBC audio encryption (AUDIO_ENCRYPT_AES_CBC) is skipped — only
//     ChaCha20-Poly1305 is implemented.
//
// The pairing handshake is the HomeKit accessory protocol: SRP-6a (3072-bit,
// RFC 5054 group 15, SHA-512) for pair-setup, Ed25519 (libsodium) for the
// accessory long-term identity, X25519 for the pair-verify key exchange, and
// ChaCha20-Poly1305 (IETF) for the encrypted control channel + audio frames.
//
// Identity/credentials persist through ESPHome preferences; every heap
// allocation in this module routes through airplay_alloc/airplay_calloc/
// airplay_free with realtime=false (pairing is control-plane / non-realtime).

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace airplay_receiver {

// ---------------------------------------------------------------------------
// Fixed key/parameter sizes (HomeKit / HAP)
// ---------------------------------------------------------------------------
constexpr size_t HAP_ED25519_PUBLIC_KEY_SIZE = 32;
constexpr size_t HAP_ED25519_SECRET_KEY_SIZE = 64;
constexpr size_t HAP_X25519_KEY_SIZE = 32;
constexpr size_t HAP_CHACHA20_KEY_SIZE = 32;
constexpr size_t HAP_CHACHA20_NONCE_SIZE = 12;
constexpr size_t HAP_POLY1305_TAG_SIZE = 16;

// SRP-6a (3072-bit, RFC 5054 group 15)
constexpr size_t SRP_PRIME_BITS = 3072;
constexpr size_t SRP_PRIME_BYTES = (SRP_PRIME_BITS / 8);  // 384
constexpr size_t SRP_SALT_BYTES = 16;
constexpr size_t SRP_PROOF_BYTES = 64;
constexpr size_t SRP_SESSION_KEY_BYTES = 64;  // SHA-512 output

// HAP TLV8 types
enum Tlv8Type : uint8_t {
  TLV8_METHOD = 0x00,
  TLV8_IDENTIFIER = 0x01,
  TLV8_SALT = 0x02,
  TLV8_PUBLIC_KEY = 0x03,
  TLV8_PROOF = 0x04,
  TLV8_ENCRYPTED_DATA = 0x05,
  TLV8_STATE = 0x06,
  TLV8_ERROR = 0x07,
  TLV8_SIGNATURE = 0x0A,
};

// HAP error codes
enum Tlv8Error : uint8_t {
  TLV8_ERROR_UNKNOWN = 0x01,
  TLV8_ERROR_AUTHENTICATION = 0x02,
  TLV8_ERROR_BACKOFF = 0x03,
  TLV8_ERROR_MAX_PEERS = 0x04,
  TLV8_ERROR_MAX_TRIES = 0x05,
  TLV8_ERROR_UNAVAILABLE = 0x06,
  TLV8_ERROR_BUSY = 0x07,
};

// Pair-verify message states
enum PairVerifyState : uint8_t {
  PAIR_VERIFY_STATE_M1 = 0x01,
  PAIR_VERIFY_STATE_M2 = 0x02,
  PAIR_VERIFY_STATE_M3 = 0x03,
  PAIR_VERIFY_STATE_M4 = 0x04,
};

// Pair-setup message states
enum PairSetupState : uint8_t {
  PAIR_SETUP_STATE_M1 = 0x01,
  PAIR_SETUP_STATE_M2 = 0x02,
  PAIR_SETUP_STATE_M3 = 0x03,
  PAIR_SETUP_STATE_M4 = 0x04,
  PAIR_SETUP_STATE_M5 = 0x05,
  PAIR_SETUP_STATE_M6 = 0x06,
};

// ---------------------------------------------------------------------------
// Audio encryption (AirPlay 2 / ChaCha20-Poly1305 only)
// ---------------------------------------------------------------------------
enum class AudioEncryptType : uint8_t {
  NOT_SET = 0,
  CHACHA20_POLY1305 = 2,  // AirPlay 2. AES-CBC (AirPlay 1) deliberately unsupported.
};

// Encryption configuration handed to the audio pipeline. The ChaCha20-Poly1305
// path uses `key` (32 bytes) + the RTP per-packet nonce derived from the last
// 8 bytes of the packet; `iv` is unused (kept for API symmetry).
struct AudioEncrypt {
  AudioEncryptType type{AudioEncryptType::NOT_SET};
  uint8_t key[32]{};   // 32-byte ChaCha20-Poly1305 key (AirPlay 2)
  uint8_t iv[16]{};    // unused for ChaCha20-Poly1305
  size_t key_len{0};
};

// ---------------------------------------------------------------------------
// Pairing session (one per client / RTSP connection). Opaque to callers:
// created/freed by CryptoModule so all memory routes through the allocator.
// ---------------------------------------------------------------------------
struct HAPSession;

/**
 * Crypto module: HomeKit pairing + ChaCha20-Poly1305 audio crypto manager.
 *
 * Setup() loads/generates the device Ed25519 identity (persisted via ESPHome
 * preferences). Sessions encapsulate per-connection pairing state; the
 * pair-verify / pair-setup handlers are the SRP-6a + Ed25519 + X25519 +
 * ChaCha20-Poly1305 state machines.
 */
class CryptoModule {
 public:
  CryptoModule();
  ~CryptoModule();

  void setup();
  void loop();

  /// True once a pairing (transient pair-verify session) has been established.
  bool paired() const;

  /// Device long-term Ed25519 public key (for the mDNS pk field).
  const uint8_t *device_public_key() const;

  // --- session lifecycle ---------------------------------------------------
  HAPSession *create_session();
  void free_session(HAPSession *session);

  // --- pair-verify (AirPlay 2 transient pairing, TLV8) ------------------------
  int pair_verify_m1(HAPSession *session, const uint8_t *input, size_t input_len,
                     uint8_t *output, size_t output_capacity, size_t *output_len);
  int pair_verify_m3(HAPSession *session, const uint8_t *input, size_t input_len,
                     uint8_t *output, size_t output_capacity, size_t *output_len);

  // --- pair-verify (raw / non-TLV AirPlay 2 format) ---------------------------
  int pair_verify_m1_raw(HAPSession *session, const uint8_t *input, size_t input_len,
                         uint8_t *output, size_t output_capacity, size_t *output_len);
  int pair_verify_m3_raw(HAPSession *session, const uint8_t *input, size_t input_len,
                         uint8_t *output, size_t output_capacity, size_t *output_len);

  // --- pair-setup (AirPlay 2 transient SRP-6a pairing) ------------------------
  int pair_setup_m1(HAPSession *session, const uint8_t *input, size_t input_len,
                    uint8_t *output, size_t output_capacity, size_t *output_len);
  int pair_setup_m3(HAPSession *session, const uint8_t *input, size_t input_len,
                    uint8_t *output, size_t output_capacity, size_t *output_len);
  int pair_setup_m5(HAPSession *session, const uint8_t *input, size_t input_len,
                    uint8_t *output, size_t output_capacity, size_t *output_len);

  // --- established-session ChaCha20-Poly1305 control channel ------------------
  int session_encrypt(HAPSession *session, const uint8_t *plaintext, size_t plaintext_len,
                      uint8_t *ciphertext, size_t *ciphertext_len);
  int session_decrypt(HAPSession *session, const uint8_t *ciphertext, size_t ciphertext_len,
                      uint8_t *plaintext, size_t *plaintext_len);

  // --- audio encryption setup + decrypt (ChaCha20-Poly1305 only) ---------------
  /// Derive the AirPlay 2 audio encryption key from the pair-verify shared
  /// secret (HKDF-SHA512 "Control-Read-Encryption-Key").
  int derive_audio_key(HAPSession *session, uint8_t *audio_key, size_t key_len);

  /// Configure an AudioEncrypt for a SETUP body: prefer the provided shk, else
  /// the ekey encrypted under the shared secret, else derive from the session.
  int configure_audio_encryption(HAPSession *session, const uint8_t *ekey,
                                 size_t ekey_len, const uint8_t *eiv,
                                 size_t eiv_len, const uint8_t *shk,
                                 size_t shk_len, AudioEncrypt *out);

  /// Decrypt an AirPlay 2 RTP audio frame (ChaCha20-Poly1305).
  int audio_decrypt_rtp(const AudioEncrypt *encrypt, const uint8_t *input, size_t input_len,
                        uint8_t *output, size_t output_capacity, const uint8_t *full_packet,
                        size_t full_packet_len);

  /// Decrypt a buffered AirPlay 2 audio packet (ChaCha20-Poly1305).
  int audio_decrypt_buffered(const AudioEncrypt *encrypt, const uint8_t *packet,
                             size_t packet_len, uint8_t *output, size_t output_capacity);

 private:
  bool paired_{false};
  bool initialized_{false};
  // Device long-term Ed25519 identity (persisted). session-independent.
  uint8_t device_public_key_[HAP_ED25519_PUBLIC_KEY_SIZE]{};
  uint8_t device_secret_key_[HAP_ED25519_SECRET_KEY_SIZE]{};
  // ESPHome preference handle (created lazily in setup()).
  ESPPreferenceObject pref_keypair_;
};

}  // namespace airplay_receiver
}  // namespace esphome
