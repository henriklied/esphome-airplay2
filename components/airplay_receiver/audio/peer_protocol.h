#pragma once
// Stereo-pair link: wire format between a pair primary and its secondary.
//
// Header-only and free of ESP-IDF includes so tests/host can exercise it.
// Every field is packed little-endian by hand; nothing depends on struct
// layout or host byte order.
//
// The primary sends three kinds of datagram to the secondary:
//   PCM   one channel (or both) of a decoded timeline frame, split into
//         chunks that fit one unfragmented UDP datagram. lwIP ships with IPv4
//         reassembly off, so a fragmented datagram would never arrive.
//   STATE an idempotent snapshot of everything the secondary mirrors: epoch,
//         format, clock map, play state, volume. Sent on change and as a
//         heartbeat, so a lost one is repaired by the next.
//   PONG  the answer to a secondary's PING: the primary's local clock at
//         receipt and at reply (NTP-style four-timestamp exchange), plus the
//         local -> network offset its own playback uses at that moment. The
//         local clock never stops, so the secondary's fit of it survives the
//         primary losing and regaining its network clock between sessions.
//
// Each PCM chunk repeats the epoch and the latest deferred-flush boundary, so
// the secondary can apply a flush whose STATE was lost before it accepts the
// replacement audio that follows it.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome {
namespace airplay_receiver {
namespace peer {

static constexpr uint32_t MAGIC = 0x52505041u;  // "APPR" read little-endian
static constexpr uint8_t VERSION = 3;
static constexpr uint16_t DEFAULT_PORT = 7011;

// Keeps every datagram under a 1500-byte Ethernet/Wi-Fi MTU after IPv4 (20)
// and UDP (8) headers, so lwIP never fragments it.
static constexpr size_t MAX_DATAGRAM = 1400;
static constexpr size_t MAX_PCM_PAYLOAD_BYTES = 1280;

enum class MsgType : uint8_t {
  PCM = 1,
  STATE = 2,
  PING = 3,
  PONG = 4,
};

// Which clock the primary's anchor is expressed in.
enum class AnchorDomain : uint8_t {
  NONE = 0,
  PTP = 1,
  NTP = 2,
  // The primary's own clock: its local-anchor fallback, unsynchronised with
  // the sender. Only the relayed clock can follow it.
  LOCAL = 3,
};

struct Header {
  uint8_t type;
  uint32_t boot_id;  // random per primary boot: resets the secondary's epoch tracking
  uint32_t seq;      // per-sender datagram counter, for loss accounting
};
static constexpr size_t HEADER_BYTES = 4 + 1 + 1 + 2 + 4 + 4;

struct PcmChunk {
  uint32_t epoch;
  uint32_t rtp;           // first sample of the whole frame
  uint32_t flush_serial;  // deferred flushes applied on the primary so far
  uint32_t flush_rtp;     // boundary of the latest one
  uint16_t frame_samples; // samples per channel in the whole frame
  uint16_t offset;        // first sample of this chunk within the frame
  uint16_t count;         // samples per channel carried here
  uint8_t channels;       // 1 or 2, interleaved
};
static constexpr size_t PCM_FIELDS_BYTES = 4 * 4 + 2 * 3 + 2;

struct State {
  uint32_t epoch;
  uint32_t flush_serial;
  uint32_t flush_rtp;
  uint32_t sample_rate;
  uint32_t frame_samples;
  bool engine_ready;
  bool playing;
  bool output_running;
  bool anchor_valid;
  AnchorDomain domain;
  uint32_t anchor_rtp;
  uint64_t anchor_network_ns;
  int64_t playout_offset_ns;
  uint64_t clock_id;
  int32_t volume_q15;
  // The primary is forwarding this session. A primary with a pair
  // destination of its own forwards only sessions on that destination.
  bool forwarding;
  // Bumped each time forwarding starts: a secondary that yielded to a local
  // session waits for the next one rather than snatching this one back.
  uint32_t session;
};
static constexpr size_t STATE_FIELDS_BYTES = 4 * 5 + 4 + 1 + 4 + 8 + 8 + 8 + 4 + 4;

struct Ping {
  int64_t t1_local_ns;  // secondary's local time at send
};
static constexpr size_t PING_FIELDS_BYTES = 8;

struct Pong {
  int64_t t1_local_ns;        // echoed from the PING
  int64_t t2_primary_ns;      // primary's local time at receipt
  int64_t t3_primary_ns;      // primary's local time at reply
  int64_t network_offset_ns;  // primary network = primary local + this, at t3
  bool network_valid;         // false: the primary has no network clock now
};
static constexpr size_t PONG_FIELDS_BYTES = 8 * 4 + 1;

// ---------------------------------------------------------------------------
// Byte packing
// ---------------------------------------------------------------------------

class Writer {
 public:
  Writer(uint8_t *buffer, size_t capacity) : buffer_(buffer), capacity_(capacity) {}

  void u8(uint8_t value) { this->put_(&value, 1); }
  void u16(uint16_t value) {
    const uint8_t bytes[2] = {(uint8_t) value, (uint8_t) (value >> 8)};
    this->put_(bytes, 2);
  }
  void u32(uint32_t value) {
    uint8_t bytes[4];
    for (int i = 0; i < 4; i++) {
      bytes[i] = (uint8_t) (value >> (8 * i));
    }
    this->put_(bytes, 4);
  }
  void u64(uint64_t value) {
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++) {
      bytes[i] = (uint8_t) (value >> (8 * i));
    }
    this->put_(bytes, 8);
  }
  void i32(int32_t value) { this->u32((uint32_t) value); }
  void i64(int64_t value) { this->u64((uint64_t) value); }
  void samples(const int16_t *pcm, size_t count) {
    for (size_t i = 0; i < count; i++) {
      this->u16((uint16_t) pcm[i]);
    }
  }

  bool ok() const { return !this->overflow_; }
  size_t size() const { return this->used_; }

 private:
  void put_(const uint8_t *bytes, size_t length) {
    if (this->overflow_ || this->used_ + length > this->capacity_) {
      this->overflow_ = true;
      return;
    }
    std::memcpy(this->buffer_ + this->used_, bytes, length);
    this->used_ += length;
  }

  uint8_t *buffer_;
  size_t capacity_;
  size_t used_{0};
  bool overflow_{false};
};

class Reader {
 public:
  Reader(const uint8_t *buffer, size_t length) : buffer_(buffer), length_(length) {}

  uint8_t u8() {
    uint8_t value = 0;
    this->get_(&value, 1);
    return value;
  }
  uint16_t u16() {
    uint8_t bytes[2] = {};
    this->get_(bytes, 2);
    return (uint16_t) (bytes[0] | (bytes[1] << 8));
  }
  uint32_t u32() {
    uint8_t bytes[4] = {};
    this->get_(bytes, 4);
    uint32_t value = 0;
    for (int i = 3; i >= 0; i--) {
      value = (value << 8) | bytes[i];
    }
    return value;
  }
  uint64_t u64() {
    uint8_t bytes[8] = {};
    this->get_(bytes, 8);
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--) {
      value = (value << 8) | bytes[i];
    }
    return value;
  }
  int32_t i32() { return (int32_t) this->u32(); }
  int64_t i64() { return (int64_t) this->u64(); }
  void samples(int16_t *pcm, size_t count) {
    for (size_t i = 0; i < count; i++) {
      pcm[i] = (int16_t) this->u16();
    }
  }

  bool ok() const { return !this->underflow_; }
  size_t remaining() const { return this->length_ - this->used_; }

 private:
  void get_(uint8_t *bytes, size_t length) {
    if (this->underflow_ || this->used_ + length > this->length_) {
      this->underflow_ = true;
      return;
    }
    std::memcpy(bytes, this->buffer_ + this->used_, length);
    this->used_ += length;
  }

  const uint8_t *buffer_;
  size_t length_;
  size_t used_{0};
  bool underflow_{false};
};

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

inline void write_header(Writer &w, MsgType type, uint32_t boot_id, uint32_t seq) {
  w.u32(MAGIC);
  w.u8(VERSION);
  w.u8((uint8_t) type);
  w.u16(0);  // reserved
  w.u32(boot_id);
  w.u32(seq);
}

/// Parse and check the common header. False for a foreign or newer datagram.
inline bool read_header(Reader &r, Header *header) {
  if (r.u32() != MAGIC) {
    return false;
  }
  if (r.u8() != VERSION) {
    return false;
  }
  header->type = r.u8();
  (void) r.u16();
  header->boot_id = r.u32();
  header->seq = r.u32();
  return r.ok();
}

/// Samples per channel that fit one PCM datagram.
inline size_t pcm_chunk_capacity(uint8_t channels) {
  return channels == 0 ? 0 : MAX_PCM_PAYLOAD_BYTES / (sizeof(int16_t) * channels);
}

/**
 * Encode one PCM chunk.
 *
 * @param pcm  interleaved samples for this chunk only (count * channels values)
 * @return datagram length, or 0 if it does not fit `capacity`
 */
inline size_t encode_pcm(uint8_t *buffer, size_t capacity, uint32_t boot_id, uint32_t seq, const PcmChunk &chunk,
                         const int16_t *pcm) {
  Writer w(buffer, capacity);
  write_header(w, MsgType::PCM, boot_id, seq);
  w.u32(chunk.epoch);
  w.u32(chunk.rtp);
  w.u32(chunk.flush_serial);
  w.u32(chunk.flush_rtp);
  w.u16(chunk.frame_samples);
  w.u16(chunk.offset);
  w.u16(chunk.count);
  w.u8(chunk.channels);
  w.u8(0);  // reserved
  w.samples(pcm, (size_t) chunk.count * chunk.channels);
  return w.ok() ? w.size() : 0;
}

/**
 * Decode a PCM chunk body (after read_header()).
 *
 * Rejects a chunk that claims more samples than it carries or that runs past
 * the end of its frame, so a corrupt datagram cannot write outside the
 * reassembly buffer.
 */
inline bool decode_pcm(Reader &r, PcmChunk *chunk, int16_t *pcm, size_t pcm_capacity) {
  chunk->epoch = r.u32();
  chunk->rtp = r.u32();
  chunk->flush_serial = r.u32();
  chunk->flush_rtp = r.u32();
  chunk->frame_samples = r.u16();
  chunk->offset = r.u16();
  chunk->count = r.u16();
  chunk->channels = r.u8();
  (void) r.u8();
  if (!r.ok() || chunk->channels < 1 || chunk->channels > 2 || chunk->count == 0 ||
      (uint32_t) chunk->offset + chunk->count > chunk->frame_samples) {
    return false;
  }
  const size_t values = (size_t) chunk->count * chunk->channels;
  if (values > pcm_capacity || r.remaining() != values * sizeof(int16_t)) {
    return false;
  }
  r.samples(pcm, values);
  return r.ok();
}

inline size_t encode_state(uint8_t *buffer, size_t capacity, uint32_t boot_id, uint32_t seq, const State &s) {
  Writer w(buffer, capacity);
  write_header(w, MsgType::STATE, boot_id, seq);
  w.u32(s.epoch);
  w.u32(s.flush_serial);
  w.u32(s.flush_rtp);
  w.u32(s.sample_rate);
  w.u32(s.frame_samples);
  const uint8_t flags = (s.engine_ready ? 1 : 0) | (s.playing ? 2 : 0) | (s.output_running ? 4 : 0) |
                        (s.anchor_valid ? 8 : 0) | (s.forwarding ? 16 : 0);
  w.u8(flags);
  w.u8((uint8_t) s.domain);
  w.u16(0);  // reserved
  w.u8(0);
  w.u32(s.anchor_rtp);
  w.u64(s.anchor_network_ns);
  w.i64(s.playout_offset_ns);
  w.u64(s.clock_id);
  w.i32(s.volume_q15);
  w.u32(s.session);
  return w.ok() ? w.size() : 0;
}

inline bool decode_state(Reader &r, State *s) {
  s->epoch = r.u32();
  s->flush_serial = r.u32();
  s->flush_rtp = r.u32();
  s->sample_rate = r.u32();
  s->frame_samples = r.u32();
  const uint8_t flags = r.u8();
  s->engine_ready = (flags & 1) != 0;
  s->playing = (flags & 2) != 0;
  s->output_running = (flags & 4) != 0;
  s->anchor_valid = (flags & 8) != 0;
  s->forwarding = (flags & 16) != 0;
  const uint8_t domain = r.u8();
  (void) r.u16();
  (void) r.u8();
  s->anchor_rtp = r.u32();
  s->anchor_network_ns = r.u64();
  s->playout_offset_ns = r.i64();
  s->clock_id = r.u64();
  s->volume_q15 = r.i32();
  s->session = r.u32();
  if (domain > (uint8_t) AnchorDomain::LOCAL) {
    return false;
  }
  s->domain = (AnchorDomain) domain;
  return r.ok();
}

inline size_t encode_ping(uint8_t *buffer, size_t capacity, uint32_t boot_id, uint32_t seq, const Ping &ping) {
  Writer w(buffer, capacity);
  write_header(w, MsgType::PING, boot_id, seq);
  w.i64(ping.t1_local_ns);
  return w.ok() ? w.size() : 0;
}

inline bool decode_ping(Reader &r, Ping *ping) {
  ping->t1_local_ns = r.i64();
  return r.ok();
}

inline size_t encode_pong(uint8_t *buffer, size_t capacity, uint32_t boot_id, uint32_t seq, const Pong &pong) {
  Writer w(buffer, capacity);
  write_header(w, MsgType::PONG, boot_id, seq);
  w.i64(pong.t1_local_ns);
  w.i64(pong.t2_primary_ns);
  w.i64(pong.t3_primary_ns);
  w.i64(pong.network_offset_ns);
  w.u8(pong.network_valid ? 1 : 0);
  return w.ok() ? w.size() : 0;
}

inline bool decode_pong(Reader &r, Pong *pong) {
  pong->t1_local_ns = r.i64();
  pong->t2_primary_ns = r.i64();
  pong->t3_primary_ns = r.i64();
  pong->network_offset_ns = r.i64();
  pong->network_valid = r.u8() != 0;
  return r.ok();
}

/// True when serial `a` is newer than `b`, across a 32-bit wrap.
inline bool serial_newer(uint32_t a, uint32_t b) { return (int32_t) (a - b) > 0; }

}  // namespace peer
}  // namespace airplay_receiver
}  // namespace esphome
