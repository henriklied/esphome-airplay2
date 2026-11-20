#include "audio_udp_diagnostics.h"

extern "C" {
#include "lwip/api.h"
#include "lwip/priv/sockets_priv.h"
#include "lwip/tcpip.h"

err_t __real_sys_mbox_trypost(sys_mbox_t *mbox, void *message);
}

namespace {

sys_mbox_t *audio_recv_mbox;
uint32_t audio_queued;
uint32_t audio_dropped;

struct watch_request_t {
  int socket_fd;
  bool installed;
};

/* Runs synchronously on the lwIP core. Its serialization with UDP input
 * prevents a previous socket's last packet from entering the new counters. */
void set_audio_mailbox(void *context) {
  auto *request = static_cast<watch_request_t *>(context);
  __atomic_store_n(&audio_recv_mbox, nullptr, __ATOMIC_RELEASE);
  if (request->socket_fd < 0) {
    return;
  }
  lwip_sock *socket = lwip_socket_dbg_get_socket(request->socket_fd);
  if (!socket || !socket->conn ||
      NETCONNTYPE_GROUP(netconn_type(socket->conn)) != NETCONN_UDP) {
    return;
  }
  __atomic_store_n(&audio_queued, 0U, __ATOMIC_RELAXED);
  __atomic_store_n(&audio_dropped, 0U, __ATOMIC_RELAXED);
  __atomic_store_n(&audio_recv_mbox, &socket->conn->recvmbox, __ATOMIC_RELEASE);
  request->installed = true;
}

}  // namespace

/* The linker wraps the real post, so this records its actual outcome. It
 * neither predicts queue fullness nor replaces lwIP's ownership/error path.
 * Only the audio mailbox is counted. Control, PTP, mDNS and TCP are excluded. */
extern "C" err_t __wrap_sys_mbox_trypost(sys_mbox_t *mbox, void *message) {
  const err_t result = __real_sys_mbox_trypost(mbox, message);
  if (mbox == __atomic_load_n(&audio_recv_mbox, __ATOMIC_ACQUIRE)) {
    uint32_t *counter = result == ERR_OK ? &audio_queued : &audio_dropped;
    __atomic_add_fetch(counter, 1U, __ATOMIC_RELAXED);
  }
  return result;
}

namespace esphome {
namespace airplay_receiver {

bool audio_udp_diagnostics_start(int socket_fd) {
  watch_request_t request{socket_fd, false};
  return tcpip_callback_wait(set_audio_mailbox, &request) == ERR_OK && request.installed;
}

void audio_udp_diagnostics_stop() {
  /* Clear immediately even if scheduling the core callback fails. Waiting
   * then drains any in-flight input before the owner closes the socket. */
  __atomic_store_n(&audio_recv_mbox, nullptr, __ATOMIC_RELEASE);
  watch_request_t request{-1, false};
  tcpip_callback_wait(set_audio_mailbox, &request);
}

audio_udp_counters_t audio_udp_diagnostics_counters() {
  return {__atomic_load_n(&audio_queued, __ATOMIC_RELAXED),
          __atomic_load_n(&audio_dropped, __ATOMIC_RELAXED)};
}

}  // namespace airplay_receiver
}  // namespace esphome
