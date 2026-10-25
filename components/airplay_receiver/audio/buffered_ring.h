#pragma once

// Byte ring between the buffered (type 103) TCP socket and the decode pipeline.
//
// The sender sizes how far ahead it pushes from the audioBufferSize this
// receiver advertises in SETUP. The PCM timeline holds ~4.5 s; the sender
// pushes far more. Gating socket reads on the timeline kept the TCP window at
// zero, and a window update the sender missed left it in its persist timer
// (5 s on macOS) while the board played dry. Draining the socket into this
// ring keeps the window open and makes the advertised size true.
//
// Pure C++ with no platform dependencies so it builds on the host for tests.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome {
namespace airplay_receiver {

/// Advertised to the sender as audioBufferSize and used as the ring capacity.
static constexpr size_t BUFFERED_AUDIO_BUFFER_BYTES = 512U * 1024U;

/// Length prefix of a buffered frame; its value counts the prefix itself.
static constexpr size_t BUFFERED_FRAME_PREFIX_BYTES = 2U;
/// A real frame carries at least a 12-byte RTP header after the prefix.
static constexpr size_t BUFFERED_FRAME_MIN_BYTES = BUFFERED_FRAME_PREFIX_BYTES + 12U;

class ByteRing {
 public:
  void attach(uint8_t *storage, size_t capacity) {
    storage_ = storage;
    capacity_ = capacity;
    clear();
  }

  void clear() {
    head_ = 0;
    size_ = 0;
  }

  size_t size() const { return size_; }
  size_t capacity() const { return capacity_; }
  size_t free_space() const { return capacity_ - size_; }

  /// Largest contiguous free region at the tail. Returns nullptr when full.
  uint8_t *write_region(size_t *len) {
    if (!storage_ || size_ == capacity_) {
      *len = 0;
      return nullptr;
    }
    const size_t tail = (head_ + size_) % capacity_;
    *len = tail >= head_ ? capacity_ - tail : head_ - tail;
    return storage_ + tail;
  }

  /// Publish `n` bytes written into the region from write_region().
  void commit(size_t n) { size_ += n; }

  /// Copy `n` bytes starting `offset` bytes past the head. False if not buffered.
  bool peek(size_t offset, uint8_t *out, size_t n) const {
    if (offset + n > size_) {
      return false;
    }
    const size_t start = (head_ + offset) % capacity_;
    const size_t first = n < capacity_ - start ? n : capacity_ - start;
    memcpy(out, storage_ + start, first);
    memcpy(out + first, storage_, n - first);
    return true;
  }

  void consume(size_t n) {
    head_ = (head_ + n) % capacity_;
    size_ -= n;
  }

 private:
  uint8_t *storage_ = nullptr;
  size_t capacity_ = 0;
  size_t head_ = 0;
  size_t size_ = 0;
};

enum class BufferedFrameStatus { INCOMPLETE, READY, INVALID };

/// Pop one length-prefixed frame (prefix stripped) into `out`.
///
/// @param ring      source ring; consumed only when READY is returned
/// @param out       destination for the frame body
/// @param out_cap   capacity of `out`; a larger declared frame is INVALID
/// @param out_len   set to the body length on READY, or the declared length on INVALID
/// @return INCOMPLETE if the whole frame is not buffered yet, INVALID if the
///         prefix is out of range (the stream is unparseable from here on)
inline BufferedFrameStatus buffered_ring_pop_frame(ByteRing *ring, uint8_t *out,
                                                   size_t out_cap, size_t *out_len) {
  uint8_t prefix[BUFFERED_FRAME_PREFIX_BYTES];
  if (!ring->peek(0, prefix, sizeof(prefix))) {
    return BufferedFrameStatus::INCOMPLETE;
  }
  const size_t declared = ((size_t) prefix[0] << 8) | prefix[1];
  if (declared < BUFFERED_FRAME_MIN_BYTES ||
      declared - BUFFERED_FRAME_PREFIX_BYTES > out_cap) {
    *out_len = declared;
    return BufferedFrameStatus::INVALID;
  }
  const size_t body = declared - BUFFERED_FRAME_PREFIX_BYTES;
  if (!ring->peek(BUFFERED_FRAME_PREFIX_BYTES, out, body)) {
    return BufferedFrameStatus::INCOMPLETE;
  }
  ring->consume(declared);
  *out_len = body;
  return BufferedFrameStatus::READY;
}

}  // namespace airplay_receiver
}  // namespace esphome
