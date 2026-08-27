#pragma once
// airplay_receiver RTSP encrypted control channel (port of main/rtsp/rtsp_crypto.c).
//
// Once PAIR-VERIFY (TLV8) completes, the RTSP control channel is carried as
// ChaCha20-Poly1305 frames: [2-byte little-endian block length][ciphertext +
// 16-byte tag]. The 2-byte length prefix is passed as ChaCha20-Poly1305
// additional authenticated data (matching upstream), routed through the
// CryptoModule's established-session session_encrypt/session_decrypt (the
// per-frame encrypt/decrypt nonce counter is owned by the HAP session).

#include <cstddef>
#include <cstdint>

#include "rtsp_conn.h"

namespace esphome {
namespace airplay_receiver {

// Maximum plaintext size of a single encrypted control block (matches upstream).
#define AIRPLAY_RTSP_ENCRYPTED_BLOCK_MAX 0x400

/**
 * Read and decrypt one encrypted control block from the socket.
 * @return decrypted length on success, -1 on error.
 */
int rtsp_crypto_read_block(int socket, RtspConn *conn, uint8_t *buffer, size_t buffer_size);

/**
 * Encrypt and write `data` to the socket, splitting into blocks of at most
 * AIRPLAY_RTSP_ENCRYPTED_BLOCK_MAX plaintext bytes each.
 * @return 0 on success, -1 on error.
 */
int rtsp_crypto_write_frame(int socket, RtspConn *conn, const uint8_t *data, size_t data_len);

}  // namespace airplay_receiver
}  // namespace esphome
