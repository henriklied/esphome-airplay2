#include "audio/audio_udp_diagnostics.h"
#include "lwip/api.h"
#include "lwip/priv/sockets_priv.h"
#include "lwip/tcpip.h"

#include <cassert>
#include <cstdio>

using namespace esphome::airplay_receiver;

static netconn audio{NETCONN_UDP, {0, 2}};
static netconn unrelated{NETCONN_UDP, {0, 1}};
static netconn tcp{NETCONN_TCP, {0, 2}};
static lwip_sock sockets[]{{&audio}, {&unrelated}, {&tcp}};
static err_t callback_error = ERR_OK;
static uint32_t posts;

extern "C" lwip_sock *lwip_socket_dbg_get_socket(int fd) {
  return fd >= 0 && fd < 3 ? &sockets[fd] : nullptr;
}

extern "C" err_t tcpip_callback_wait(tcpip_callback_fn callback, void *context) {
  if (callback_error == ERR_OK) {
    callback(context);
  }
  return callback_error;
}

extern "C" err_t __real_sys_mbox_trypost(sys_mbox_t *mbox, void *) {
  posts++;
  if (mbox->used == mbox->capacity) {
    return ERR_MEM;
  }
  mbox->used++;
  return ERR_OK;
}

extern "C" err_t __wrap_sys_mbox_trypost(sys_mbox_t *, void *);

int main() {
  assert(audio_udp_diagnostics_start(0));
  assert(__wrap_sys_mbox_trypost(&audio.recvmbox, nullptr) == ERR_OK);
  assert(__wrap_sys_mbox_trypost(&audio.recvmbox, nullptr) == ERR_OK);
  assert(__wrap_sys_mbox_trypost(&audio.recvmbox, nullptr) == ERR_MEM);
  assert(__wrap_sys_mbox_trypost(&unrelated.recvmbox, nullptr) == ERR_OK);
  assert(__wrap_sys_mbox_trypost(&unrelated.recvmbox, nullptr) == ERR_MEM);
  auto counters = audio_udp_diagnostics_counters();
  assert(counters.queued == 2 && counters.dropped == 1 && posts == 5);

  // Failed posts after stop and mailbox reuse belong to a closed session.
  audio_udp_diagnostics_stop();
  assert(__wrap_sys_mbox_trypost(&audio.recvmbox, nullptr) == ERR_MEM);
  assert(audio_udp_diagnostics_counters().dropped == 1);
  assert(audio_udp_diagnostics_start(0));
  counters = audio_udp_diagnostics_counters();
  assert(counters.queued == 0 && counters.dropped == 0);
  assert(__wrap_sys_mbox_trypost(&audio.recvmbox, nullptr) == ERR_MEM);
  assert(audio_udp_diagnostics_counters().dropped == 1);

  assert(!audio_udp_diagnostics_start(2));
  assert(!audio_udp_diagnostics_start(99));
  callback_error = ERR_MEM;
  assert(!audio_udp_diagnostics_start(0));
  audio_udp_diagnostics_stop();
  assert(__wrap_sys_mbox_trypost(&audio.recvmbox, nullptr) == ERR_MEM);
  assert(audio_udp_diagnostics_counters().dropped == 1);
  std::puts("ok");
}
