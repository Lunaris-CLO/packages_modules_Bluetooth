/******************************************************************************
 *
 *  Copyright 2009-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#define LOG_TAG "bt_btif_sock_rfcomm"

#include <bluetooth/log.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <cstdint>
#include <mutex>

#include "bta/include/bta_jv_api.h"
#include "bta/include/bta_rfcomm_scn.h"
#include "btif/include/btif_metrics_logging.h"
#include "btif/include/btif_sock.h"
#include "btif/include/btif_sock_l2cap.h"
#include "btif/include/btif_sock_logging.h"
#include "btif/include/btif_sock_sdp.h"
#include "btif/include/btif_sock_thread.h"
#include "btif/include/btif_sock_util.h"
#include "include/hardware/bt_sock.h"
#include "os/log.h"
#include "osi/include/allocator.h"
#include "osi/include/compat.h"
#include "osi/include/list.h"
#include "osi/include/osi.h"  // INVALID_FD
#include "stack/include/bt_hdr.h"
#include "stack/include/port_api.h"
#include "types/bluetooth/uuid.h"
#include "types/raw_address.h"

using bluetooth::Uuid;
using namespace bluetooth;

// Maximum number of RFCOMM channels (1-30 inclusive).
#define MAX_RFC_CHANNEL 30

// Maximum number of devices we can have an RFCOMM connection with.
#define MAX_RFC_SESSION 7

typedef struct {
  int outgoing_congest : 1;
  int pending_sdp_request : 1;
  int doing_sdp_request : 1;
  int server : 1;
  int connected : 1;
  int closing : 1;
} flags_t;

typedef struct {
  flags_t f;
  uint32_t id;  // Non-zero indicates a valid (in-use) slot.
  int security;
  int scn;  // Server channel number
  int scn_notified;
  RawAddress addr;
  int is_service_uuid_valid;
  Uuid service_uuid;
  char service_name[256];
  int fd;
  int app_fd;   // Temporary storage for the half of the socketpair that's
                // sent back to upper layers.
  int app_uid;  // UID of the app for which this socket was created.
  int mtu;
  uint8_t* packet;
  int sdp_handle;
  int rfc_handle;
  int rfc_port_handle;
  int role;
  list_t* incoming_queue;
  // Cumulative number of bytes transmitted on this socket
  int64_t tx_bytes;
  // Cumulative number of bytes received on this socket
  int64_t rx_bytes;
} rfc_slot_t;

static rfc_slot_t rfc_slots[MAX_RFC_CHANNEL];
static uint32_t rfc_slot_id;
static volatile int pth = -1;  // poll thread handle
static std::recursive_mutex slot_lock;
static uid_set_t* uid_set = NULL;

static rfc_slot_t* find_free_slot(void);
static void cleanup_rfc_slot(rfc_slot_t* rs);
static void jv_dm_cback(tBTA_JV_EVT event, tBTA_JV* p_data, uint32_t id);
static uint32_t rfcomm_cback(tBTA_JV_EVT event, tBTA_JV* p_data,
                             uint32_t rfcomm_slot_id);
static bool send_app_scn(rfc_slot_t* rs);
static void handle_discovery_comp(tBTA_JV_STATUS status, int scn, uint32_t id);

static bool is_init_done(void) { return pth != -1; }

bt_status_t btsock_rfc_init(int poll_thread_handle, uid_set_t* set) {
  pth = poll_thread_handle;
  uid_set = set;

  memset(rfc_slots, 0, sizeof(rfc_slots));
  for (size_t i = 0; i < ARRAY_SIZE(rfc_slots); ++i) {
    rfc_slots[i].scn = -1;
    rfc_slots[i].sdp_handle = 0;
    rfc_slots[i].fd = INVALID_FD;
    rfc_slots[i].app_fd = INVALID_FD;
    rfc_slots[i].incoming_queue = list_new(osi_free);
    log::assert_that(rfc_slots[i].incoming_queue != NULL,
                     "assert failed: rfc_slots[i].incoming_queue != NULL");
  }

  BTA_JvEnable(jv_dm_cback);

  return BT_STATUS_SUCCESS;
}

void btsock_rfc_cleanup(void) {
  pth = -1;

  BTA_JvDisable();

  std::unique_lock<std::recursive_mutex> lock(slot_lock);
  for (size_t i = 0; i < ARRAY_SIZE(rfc_slots); ++i) {
    if (rfc_slots[i].id) cleanup_rfc_slot(&rfc_slots[i]);
    list_free(rfc_slots[i].incoming_queue);
    rfc_slots[i].incoming_queue = NULL;
  }

  uid_set = NULL;

  log::debug("cleanup finished");
}

static rfc_slot_t* find_free_slot(void) {
  for (size_t i = 0; i < ARRAY_SIZE(rfc_slots); ++i)
    if (rfc_slots[i].fd == INVALID_FD) return &rfc_slots[i];
  return NULL;
}

static rfc_slot_t* find_rfc_slot_by_id(uint32_t id) {
  CHECK(id != 0);

  for (size_t i = 0; i < ARRAY_SIZE(rfc_slots); ++i)
    if (rfc_slots[i].id == id) return &rfc_slots[i];

  return NULL;
}

static rfc_slot_t* find_rfc_slot_by_pending_sdp(void) {
  uint32_t min_id = UINT32_MAX;
  int slot = -1;
  for (size_t i = 0; i < ARRAY_SIZE(rfc_slots); ++i)
    if (rfc_slots[i].id && rfc_slots[i].f.pending_sdp_request &&
        rfc_slots[i].id < min_id) {
      min_id = rfc_slots[i].id;
      slot = i;
    }

  return (slot == -1) ? NULL : &rfc_slots[slot];
}

static bool is_requesting_sdp(void) {
  for (size_t i = 0; i < ARRAY_SIZE(rfc_slots); ++i)
    if (rfc_slots[i].id && rfc_slots[i].f.doing_sdp_request) {
      log::info("slot id {} is doing sdp request", rfc_slots[i].id);
      return true;
    }
  return false;
}

static rfc_slot_t* alloc_rfc_slot(const RawAddress* addr, const char* name,
                                  const Uuid& uuid, int channel, int flags,
                                  bool server) {
  int security = 0;
  if (flags & BTSOCK_FLAG_ENCRYPT)
    security |= server ? BTM_SEC_IN_ENCRYPT : BTM_SEC_OUT_ENCRYPT;
  if (flags & BTSOCK_FLAG_AUTH)
    security |= server ? BTM_SEC_IN_AUTHENTICATE : BTM_SEC_OUT_AUTHENTICATE;
  if (flags & BTSOCK_FLAG_AUTH_MITM)
    security |= server ? BTM_SEC_IN_MITM : BTM_SEC_OUT_MITM;
  if (flags & BTSOCK_FLAG_AUTH_16_DIGIT)
    security |= BTM_SEC_IN_MIN_16_DIGIT_PIN;

  rfc_slot_t* slot = find_free_slot();
  if (!slot) {
    log::error("unable to find free RFCOMM slot.");
    return NULL;
  }

  int fds[2] = {INVALID_FD, INVALID_FD};
  if (socketpair(AF_LOCAL, SOCK_STREAM, 0, fds) == -1) {
    log::error("error creating socketpair: {}", strerror(errno));
    return NULL;
  }

  // Increment slot id and make sure we don't use id=0.
  if (++rfc_slot_id == 0) rfc_slot_id = 1;

  slot->fd = fds[0];
  slot->app_fd = fds[1];
  slot->security = security;
  slot->scn = channel;
  slot->app_uid = -1;

  slot->is_service_uuid_valid = !uuid.IsEmpty();
  slot->service_uuid = uuid;

  if (name && *name) {
    strlcpy(slot->service_name, name, sizeof(slot->service_name));
  } else {
    memset(slot->service_name, 0, sizeof(slot->service_name));
  }
  if (addr) {
    slot->addr = *addr;
  } else {
    slot->addr = RawAddress::kEmpty;
  }
  slot->id = rfc_slot_id;
  slot->f.server = server;
  slot->role = server;
  slot->tx_bytes = 0;
  slot->rx_bytes = 0;
  return slot;
}

static rfc_slot_t* create_srv_accept_rfc_slot(rfc_slot_t* srv_rs,
                                              const RawAddress* addr,
                                              int open_handle,
                                              int new_listen_handle) {
  rfc_slot_t* accept_rs = alloc_rfc_slot(
      addr, srv_rs->service_name, srv_rs->service_uuid, srv_rs->scn, 0, false);
  if (!accept_rs) {
    log::error("unable to allocate RFCOMM slot.");
    return NULL;
  }

  accept_rs->f.server = false;
  accept_rs->f.connected = true;
  accept_rs->security = srv_rs->security;
  accept_rs->mtu = srv_rs->mtu;
  accept_rs->role = srv_rs->role;
  accept_rs->rfc_handle = open_handle;
  accept_rs->rfc_port_handle = BTA_JvRfcommGetPortHdl(open_handle);
  accept_rs->app_uid = srv_rs->app_uid;

  srv_rs->rfc_handle = new_listen_handle;
  srv_rs->rfc_port_handle = BTA_JvRfcommGetPortHdl(new_listen_handle);

  if (accept_rs->rfc_port_handle == srv_rs->rfc_port_handle) {
    log::error(
        "accept_rs->rfc_port_handle == srv_rs->rfc_port_handle, "
        "rfc_port_handle={}",
        accept_rs->rfc_port_handle);
  }
  log::assert_that(
      accept_rs->rfc_port_handle != srv_rs->rfc_port_handle,
      "assert failed: accept_rs->rfc_port_handle != srv_rs->rfc_port_handle");

  // now swap the slot id
  uint32_t new_listen_id = accept_rs->id;
  accept_rs->id = srv_rs->id;
  srv_rs->id = new_listen_id;

  return accept_rs;
}

bt_status_t btsock_rfc_control_req(uint8_t dlci, const RawAddress& bd_addr,
                                   uint8_t modem_signal, uint8_t break_signal,
                                   uint8_t discard_buffers,
                                   uint8_t break_signal_seq, bool fc) {
  int status =
      RFCOMM_ControlReqFromBTSOCK(dlci, bd_addr, modem_signal, break_signal,
                                  discard_buffers, break_signal_seq, fc);
  if (status != PORT_SUCCESS) {
    log::warn("failed to send control parameters, status={}", status);
    return BT_STATUS_FAIL;
  }
  return BT_STATUS_SUCCESS;
}

bt_status_t btsock_rfc_listen(const char* service_name,
                              const Uuid* service_uuid, int channel,
                              int* sock_fd, int flags, int app_uid) {
  log::assert_that(sock_fd != NULL, "assert failed: sock_fd != NULL");
  log::assert_that(
      (service_uuid != NULL) || (channel >= 1 && channel <= MAX_RFC_CHANNEL) ||
          ((flags & BTSOCK_FLAG_NO_SDP) != 0),
      "assert failed: (service_uuid != NULL) || (channel >= 1 && channel <= "
      "MAX_RFC_CHANNEL) || ((flags & BTSOCK_FLAG_NO_SDP) != 0)");

  *sock_fd = INVALID_FD;

  // TODO(sharvil): not sure that this check makes sense; seems like a logic
  // error to call
  // functions on RFCOMM sockets before initializing the module. Probably
  // should be an assert.
  if (!is_init_done()) {
    log::error("BT not ready");
    return BT_STATUS_NOT_READY;
  }

  if ((flags & BTSOCK_FLAG_NO_SDP) == 0) {
    if (!service_uuid || service_uuid->IsEmpty()) {
      // Use serial port profile to listen to specified channel
      service_uuid = &UUID_SPP;
    } else {
      // Check the service_uuid. overwrite the channel # if reserved
      int reserved_channel = get_reserved_rfc_channel(*service_uuid);
      if (reserved_channel > 0) {
        channel = reserved_channel;
      }
    }
  }

  std::unique_lock<std::recursive_mutex> lock(slot_lock);

  rfc_slot_t* slot =
      alloc_rfc_slot(NULL, service_name, *service_uuid, channel, flags, true);
  if (!slot) {
    log::error("unable to allocate RFCOMM slot");
    return BT_STATUS_NOMEM;
  }
  log::info("Adding listening socket service_name: {} - channel: {}",
            service_name, channel);
  BTA_JvGetChannelId(tBTA_JV_CONN_TYPE::RFCOMM, slot->id, channel);
  *sock_fd = slot->app_fd;  // Transfer ownership of fd to caller.
  /*TODO:
   * We are leaking one of the app_fd's - either the listen socket, or the
   connection socket.
   * WE need to close this in native, as the FD might belong to another process
    - This is the server socket FD
    - For accepted connections, we close the FD after passing it to JAVA.
    - Try to simply remove the = -1 to free the FD at rs cleanup.*/
  //        close(rs->app_fd);
  slot->app_fd = INVALID_FD;  // Drop our reference to the fd.
  slot->app_uid = app_uid;
  btsock_thread_add_fd(pth, slot->fd, BTSOCK_RFCOMM, SOCK_THREAD_FD_EXCEPTION,
                       slot->id);

  return BT_STATUS_SUCCESS;
}

bt_status_t btsock_rfc_connect(const RawAddress* bd_addr,
                               const Uuid* service_uuid, int channel,
                               int* sock_fd, int flags, int app_uid) {
  log::assert_that(sock_fd != NULL, "assert failed: sock_fd != NULL");
  log::assert_that(
      (service_uuid != NULL) || (channel >= 1 && channel <= MAX_RFC_CHANNEL),
      "assert failed: (service_uuid != NULL) || (channel >= 1 && channel <= "
      "MAX_RFC_CHANNEL)");

  *sock_fd = INVALID_FD;

  // TODO(sharvil): not sure that this check makes sense; seems like a logic
  // error to call
  // functions on RFCOMM sockets before initializing the module. Probably should
  // be an assert.
  if (!is_init_done()) {
    log::error("BT not ready");
    return BT_STATUS_NOT_READY;
  }

  std::unique_lock<std::recursive_mutex> lock(slot_lock);

  rfc_slot_t* slot =
      alloc_rfc_slot(bd_addr, NULL, *service_uuid, channel, flags, false);
  if (!slot) {
    log::error("unable to allocate RFCOMM slot. bd_addr:{}", *bd_addr);
    return BT_STATUS_NOMEM;
  }

  if (!service_uuid || service_uuid->IsEmpty()) {
    tBTA_JV_STATUS ret = BTA_JvRfcommConnect(
        slot->security, slot->scn, slot->addr, rfcomm_cback, slot->id);
    if (ret != tBTA_JV_STATUS::SUCCESS) {
      log::error(
          "unable to initiate RFCOMM connection. status:{}, scn:{}, bd_addr:{}",
          bta_jv_status_text(ret), slot->scn, slot->addr);
      cleanup_rfc_slot(slot);
      return BT_STATUS_SOCKET_ERROR;
    }

    if (!send_app_scn(slot)) {
      log::error("send_app_scn() failed, closing slot->id:{}", slot->id);
      cleanup_rfc_slot(slot);
      return BT_STATUS_SOCKET_ERROR;
    }
  } else {
    log::info("service_uuid:{}, bd_addr:{}, slot_id:{}",
              service_uuid->ToString(), *bd_addr, slot->id);
    if (!is_requesting_sdp()) {
      BTA_JvStartDiscovery(*bd_addr, 1, service_uuid, slot->id);
      slot->f.pending_sdp_request = false;
      slot->f.doing_sdp_request = true;
    } else {
      slot->f.pending_sdp_request = true;
      slot->f.doing_sdp_request = false;
    }
  }

  *sock_fd = slot->app_fd;    // Transfer ownership of fd to caller.
  slot->app_fd = INVALID_FD;  // Drop our reference to the fd.
  slot->app_uid = app_uid;
  btsock_thread_add_fd(pth, slot->fd, BTSOCK_RFCOMM, SOCK_THREAD_FD_RD,
                       slot->id);

  return BT_STATUS_SUCCESS;
}

static int create_server_sdp_record(rfc_slot_t* slot) {
  if (slot->scn == 0) {
    return false;
  }
  slot->sdp_handle =
      add_rfc_sdp_rec(slot->service_name, slot->service_uuid, slot->scn);
  return (slot->sdp_handle > 0);
}

static void free_rfc_slot_scn(rfc_slot_t* slot) {
  if (slot->scn <= 0) return;

  if (slot->f.server && !slot->f.closing && slot->rfc_handle) {
    BTA_JvRfcommStopServer(slot->rfc_handle, slot->id);
    slot->rfc_handle = 0;
  }

  if (slot->f.server) BTA_FreeSCN(slot->scn);
  slot->scn = 0;
}

static void cleanup_rfc_slot(rfc_slot_t* slot) {
  if (slot->fd != INVALID_FD) {
    shutdown(slot->fd, SHUT_RDWR);
    close(slot->fd);
    log::info(
        "disconnected from RFCOMM socket connections for device: {}, scn: {}, "
        "app_uid: {}, id: {}",
        slot->addr, slot->scn, slot->app_uid, slot->id);
    btif_sock_connection_logger(
        slot->addr, slot->id, BTSOCK_RFCOMM,
        SOCKET_CONNECTION_STATE_DISCONNECTED,
        slot->f.server ? SOCKET_ROLE_LISTEN : SOCKET_ROLE_CONNECTION,
        slot->app_uid, slot->scn, slot->tx_bytes, slot->rx_bytes,
        slot->role ? slot->service_name
                   : slot->service_uuid.ToString().c_str());

    slot->fd = INVALID_FD;
  }

  if (slot->app_fd != INVALID_FD) {
    close(slot->app_fd);
    slot->app_fd = INVALID_FD;
  }

  if (slot->sdp_handle > 0) {
    del_rfc_sdp_rec(slot->sdp_handle);
    slot->sdp_handle = 0;
  }

  if (slot->rfc_handle && !slot->f.closing && !slot->f.server) {
    BTA_JvRfcommClose(slot->rfc_handle, slot->id);
    slot->rfc_handle = 0;
  }

  free_rfc_slot_scn(slot);
  list_clear(slot->incoming_queue);

  slot->rfc_port_handle = 0;
  memset(&slot->f, 0, sizeof(slot->f));
  slot->id = 0;
  slot->scn_notified = false;
  slot->tx_bytes = 0;
  slot->rx_bytes = 0;
}

static bool send_app_scn(rfc_slot_t* slot) {
  if (slot->scn_notified) {
    // already sent, just return success.
    return true;
  }
  log::debug("Sending scn for slot {}. bd_addr:{}", slot->id, slot->addr);
  slot->scn_notified = true;
  return sock_send_all(slot->fd, (const uint8_t*)&slot->scn,
                       sizeof(slot->scn)) == sizeof(slot->scn);
}

static bool send_app_connect_signal(int fd, const RawAddress* addr, int channel,
                                    int status, int send_fd) {
  sock_connect_signal_t cs;
  cs.size = sizeof(cs);
  cs.bd_addr = *addr;
  cs.channel = channel;
  cs.status = status;
  cs.max_rx_packet_size = 0;  // not used for RFCOMM
  cs.max_tx_packet_size = 0;  // not used for RFCOMM
  cs.conn_uuid_lsb = 0;       // not used for RFCOMM
  cs.conn_uuid_msb = 0;       // not used for RFCOMM
  if (send_fd == INVALID_FD)
    return sock_send_all(fd, (const uint8_t*)&cs, sizeof(cs)) == sizeof(cs);

  return sock_send_fd(fd, (const uint8_t*)&cs, sizeof(cs), send_fd) ==
         sizeof(cs);
}

static void on_cl_rfc_init(tBTA_JV_RFCOMM_CL_INIT* p_init, uint32_t id) {
  std::unique_lock<std::recursive_mutex> lock(slot_lock);
  rfc_slot_t* slot = find_rfc_slot_by_id(id);
  if (!slot) {
    log::error("RFCOMM slot with id {} not found. p_init->status={}", id,
               bta_jv_status_text(p_init->status));
  } else if (p_init->status != tBTA_JV_STATUS::SUCCESS) {
    log::warn("INIT unsuccessful, status {}. Cleaning up slot with id {}",
              bta_jv_status_text(p_init->status), slot->id);
    cleanup_rfc_slot(slot);
  } else {
    slot->rfc_handle = p_init->handle;
  }
}

static void on_srv_rfc_listen_started(tBTA_JV_RFCOMM_START* p_start,
                                      uint32_t id) {
  std::unique_lock<std::recursive_mutex> lock(slot_lock);
  rfc_slot_t* slot = find_rfc_slot_by_id(id);
  if (!slot) {
    log::error("RFCOMM slot with id {} not found", id);
    return;
  } else if (p_start->status != tBTA_JV_STATUS::SUCCESS) {
    log::warn("START unsuccessful, status {}. Cleaning up slot with id {}",
              bta_jv_status_text(p_start->status), slot->id);
    cleanup_rfc_slot(slot);
    return;
  }

  slot->rfc_handle = p_start->handle;
  log::info(
      "listening for RFCOMM socket connections for device: {}, scn: {}, "
      "app_uid: {}, id: {}",
      slot->addr, slot->scn, slot->app_uid, id);
  btif_sock_connection_logger(
      slot->addr, slot->id, BTSOCK_RFCOMM, SOCKET_CONNECTION_STATE_LISTENING,
      slot->f.server ? SOCKET_ROLE_LISTEN : SOCKET_ROLE_CONNECTION,
      slot->app_uid, slot->scn, 0, 0, slot->service_name);
}

static uint32_t on_srv_rfc_connect(tBTA_JV_RFCOMM_SRV_OPEN* p_open,
                                   uint32_t id) {
  log::verbose("id:{}", id);
  std::unique_lock<std::recursive_mutex> lock(slot_lock);
  rfc_slot_t* accept_rs;
  rfc_slot_t* srv_rs = find_rfc_slot_by_id(id);
  if (!srv_rs) {
    log::error("RFCOMM slot with id {} not found.", id);
    return 0;
  }

  accept_rs = create_srv_accept_rfc_slot(
      srv_rs, &p_open->rem_bda, p_open->handle, p_open->new_listen_handle);
  if (!accept_rs) return 0;

  log::info(
      "connected to RFCOMM socket connections for device: {}, scn: {}, "
      "app_uid: {}, id: {}",
      accept_rs->addr, accept_rs->scn, accept_rs->app_uid, id);
  btif_sock_connection_logger(
      accept_rs->addr, accept_rs->id, BTSOCK_RFCOMM,
      SOCKET_CONNECTION_STATE_DISCONNECTED,
      accept_rs->f.server ? SOCKET_ROLE_LISTEN : SOCKET_ROLE_CONNECTION,
      accept_rs->app_uid, accept_rs->scn, 0, 0, accept_rs->service_name);

  // Start monitoring the socket.
  btsock_thread_add_fd(pth, srv_rs->fd, BTSOCK_RFCOMM, SOCK_THREAD_FD_EXCEPTION,
                       srv_rs->id);
  btsock_thread_add_fd(pth, accept_rs->fd, BTSOCK_RFCOMM, SOCK_THREAD_FD_RD,
                       accept_rs->id);
  send_app_connect_signal(srv_rs->fd, &accept_rs->addr, srv_rs->scn, 0,
                          accept_rs->app_fd);
  accept_rs->app_fd =
      INVALID_FD;  // Ownership of the application fd has been transferred.
  return srv_rs->id;
}

static void on_cli_rfc_connect(tBTA_JV_RFCOMM_OPEN* p_open, uint32_t id) {
  log::verbose("id:{}", id);
  std::unique_lock<std::recursive_mutex> lock(slot_lock);
  rfc_slot_t* slot = find_rfc_slot_by_id(id);
  if (!slot) {
    log::error("RFCOMM slot with id {} not found.", id);
    return;
  }

  if (p_open->status != tBTA_JV_STATUS::SUCCESS) {
    log::warn("CONNECT unsuccessful, status {}. Cleaning up slot with id {}",
              bta_jv_status_text(p_open->status), slot->id);
    cleanup_rfc_slot(slot);
    return;
  }

  slot->rfc_port_handle = BTA_JvRfcommGetPortHdl(p_open->handle);
  slot->addr = p_open->rem_bda;

  log::info(
      "connected to RFCOMM socket connections for device: {}, scn: {}, "
      "app_uid: {}, id: {}",
      slot->addr, slot->scn, slot->app_uid, id);
  btif_sock_connection_logger(
      slot->addr, slot->id, BTSOCK_RFCOMM, SOCKET_CONNECTION_STATE_CONNECTED,
      slot->f.server ? SOCKET_ROLE_LISTEN : SOCKET_ROLE_CONNECTION,
      slot->app_uid, slot->scn, 0, 0, slot->service_uuid.ToString().c_str());

  if (send_app_connect_signal(slot->fd, &slot->addr, slot->scn, 0, -1)) {
    slot->f.connected = true;
  } else {
    log::error("unable to send connect completion signal to caller.");
  }
}

static void on_rfc_close(tBTA_JV_RFCOMM_CLOSE* /* p_close */, uint32_t id) {
  log::verbose("id:{}", id);
  std::unique_lock<std::recursive_mutex> lock(slot_lock);

  // rfc_handle already closed when receiving rfcomm close event from stack.
  rfc_slot_t* slot = find_rfc_slot_by_id(id);
  if (!slot) {
    log::warn("RFCOMM slot with id {} not found.", id);
    return;
  }
  log_socket_connection_state(
      slot->addr, slot->id, BTSOCK_RFCOMM,
      android::bluetooth::SOCKET_CONNECTION_STATE_DISCONNECTING, 0, 0,
      slot->app_uid, slot->scn,
      slot->f.server ? android::bluetooth::SOCKET_ROLE_LISTEN
                     : android::bluetooth::SOCKET_ROLE_CONNECTION);
  cleanup_rfc_slot(slot);
}

static void on_rfc_write_done(tBTA_JV_RFCOMM_WRITE* p, uint32_t id) {
  if (p->status != tBTA_JV_STATUS::SUCCESS) {
    log::error("error writing to RFCOMM socket with slot {}.", p->req_id);
    return;
  }

  int app_uid = -1;
  std::unique_lock<std::recursive_mutex> lock(slot_lock);

  rfc_slot_t* slot = find_rfc_slot_by_id(id);
  if (!slot) {
    log::error("RFCOMM slot with id {} not found.", id);
    return;
  }
  app_uid = slot->app_uid;
  if (!slot->f.outgoing_congest)
    btsock_thread_add_fd(pth, slot->fd, BTSOCK_RFCOMM, SOCK_THREAD_FD_RD,
                         slot->id);
  slot->tx_bytes += p->len;
  uid_set_add_tx(uid_set, app_uid, p->len);
}

static void on_rfc_outgoing_congest(tBTA_JV_RFCOMM_CONG* p, uint32_t id) {
  std::unique_lock<std::recursive_mutex> lock(slot_lock);

  rfc_slot_t* slot = find_rfc_slot_by_id(id);
  if (!slot) {
    log::error("RFCOMM slot with id {} not found.", id);
    return;
  }

  slot->f.outgoing_congest = p->cong ? 1 : 0;
  if (!slot->f.outgoing_congest)
    btsock_thread_add_fd(pth, slot->fd, BTSOCK_RFCOMM, SOCK_THREAD_FD_RD,
                         slot->id);
}

static uint32_t rfcomm_cback(tBTA_JV_EVT event, tBTA_JV* p_data,
                             uint32_t rfcomm_slot_id) {
  uint32_t id = 0;

  if (rfcomm_slot_id == 0) {
    log::info("invalid rfcomm_slot_id");
    return id;
  }
  switch (event) {
    case BTA_JV_RFCOMM_START_EVT:
      log::info("handling {}, rfcomm_slot_id:{}", bta_jv_event_text(event),
                rfcomm_slot_id);
      on_srv_rfc_listen_started(&p_data->rfc_start, rfcomm_slot_id);
      break;

    case BTA_JV_RFCOMM_CL_INIT_EVT:
      log::info("handling {}, rfcomm_slot_id:{}", bta_jv_event_text(event),
                rfcomm_slot_id);
      on_cl_rfc_init(&p_data->rfc_cl_init, rfcomm_slot_id);
      break;

    case BTA_JV_RFCOMM_OPEN_EVT:
      log::info("handling {}, rfcomm_slot_id:{}", bta_jv_event_text(event),
                rfcomm_slot_id);
      BTA_JvSetPmProfile(p_data->rfc_open.handle, BTA_JV_PM_ID_1,
                         BTA_JV_CONN_OPEN);
      on_cli_rfc_connect(&p_data->rfc_open, rfcomm_slot_id);
      break;

    case BTA_JV_RFCOMM_SRV_OPEN_EVT:
      log::info("handling {}, rfcomm_slot_id:{}", bta_jv_event_text(event),
                rfcomm_slot_id);
      BTA_JvSetPmProfile(p_data->rfc_srv_open.handle, BTA_JV_PM_ALL,
                         BTA_JV_CONN_OPEN);
      id = on_srv_rfc_connect(&p_data->rfc_srv_open, rfcomm_slot_id);
      break;

    case BTA_JV_RFCOMM_CLOSE_EVT:
      log::info("handling {}, rfcomm_slot_id:{}", bta_jv_event_text(event),
                rfcomm_slot_id);
      on_rfc_close(&p_data->rfc_close, rfcomm_slot_id);
      break;

    case BTA_JV_RFCOMM_WRITE_EVT:
      log::verbose("handling {}, rfcomm_slot_id:{}", bta_jv_event_text(event),
                   rfcomm_slot_id);
      on_rfc_write_done(&p_data->rfc_write, rfcomm_slot_id);
      break;

    case BTA_JV_RFCOMM_CONG_EVT:
      log::verbose("handling {}, rfcomm_slot_id:{}", bta_jv_event_text(event),
                   rfcomm_slot_id);
      on_rfc_outgoing_congest(&p_data->rfc_cong, rfcomm_slot_id);
      break;

    case BTA_JV_RFCOMM_DATA_IND_EVT:
      // Unused.
      break;

    default:
      log::warn("unhandled event {}, slot id: {}", bta_jv_event_text(event),
                rfcomm_slot_id);
      break;
  }
  return id;
}

static void jv_dm_cback(tBTA_JV_EVT event, tBTA_JV* p_data, uint32_t id) {
  log::info("handling event:{}, id:{}", bta_jv_event_text(event), id);
  switch (event) {
    case BTA_JV_GET_SCN_EVT: {
      std::unique_lock<std::recursive_mutex> lock(slot_lock);
      rfc_slot_t* rs = find_rfc_slot_by_id(id);
      if (!rs) {
        log::error("RFCOMM slot with id {} not found. event:{}", id,
                   bta_jv_event_text(event));
        break;
      }
      if (p_data->scn == 0) {
        log::error(
            "Unable to allocate scn: all resources exhausted. slot found: {}",
            fmt::ptr(rs));
        cleanup_rfc_slot(rs);
        break;
      }

      rs->scn = p_data->scn;
      // Send channel ID to java layer
      if (!send_app_scn(rs)) {
        log::warn("send_app_scn() failed, closing rs->id:{}", rs->id);
        cleanup_rfc_slot(rs);
        break;
      }

      if (rs->is_service_uuid_valid) {
        // BTA_JvCreateRecordByUser will only create a record if a UUID is
        // specified. RFC-only profiles
        BTA_JvCreateRecordByUser(rs->id);
      } else {
        // If uuid is null, just allocate a RFC channel and start the RFCOMM
        // thread needed for the java layer to get a RFCOMM channel.
        // create_sdp_record() will be called from Java when it has received the
        // RFCOMM and L2CAP channel numbers through the sockets.
        log::debug(
            "Since UUID is not valid; not setting SDP-record and just starting "
            "the RFCOMM server");
        // now start the rfcomm server after sdp & channel # assigned
        BTA_JvRfcommStartServer(rs->security, rs->scn, MAX_RFC_SESSION,
                                rfcomm_cback, rs->id);
      }
      break;
    }

    case BTA_JV_GET_PSM_EVT: {
      log::verbose("Received PSM: 0x{:04x}", p_data->psm);
      on_l2cap_psm_assigned(id, p_data->psm);
      break;
    }

    case BTA_JV_CREATE_RECORD_EVT: {
      std::unique_lock<std::recursive_mutex> lock(slot_lock);
      rfc_slot_t* slot = find_rfc_slot_by_id(id);

      if (!slot) {
        log::error("RFCOMM slot with id {} not found. event:{}", id,
                   bta_jv_event_text(event));
        break;
      }

      if (!create_server_sdp_record(slot)) {
        log::error("cannot start server, slot found: {}", fmt::ptr(slot));
        cleanup_rfc_slot(slot);
        break;
      }

      // Start the rfcomm server after sdp & channel # assigned.
      BTA_JvRfcommStartServer(slot->security, slot->scn, MAX_RFC_SESSION,
                              rfcomm_cback, slot->id);
      break;
    }

    case BTA_JV_DISCOVERY_COMP_EVT: {
      std::unique_lock<std::recursive_mutex> lock(slot_lock);
      handle_discovery_comp(p_data->disc_comp.status, p_data->disc_comp.scn,
                            id);
      // Find the next slot that needs to perform an SDP request and service it.
      rfc_slot_t* slot = find_rfc_slot_by_pending_sdp();
      if (slot) {
        BTA_JvStartDiscovery(slot->addr, 1, &slot->service_uuid, slot->id);
        slot->f.pending_sdp_request = false;
        slot->f.doing_sdp_request = true;
      }
      break;
    }

    default:
      log::debug("unhandled event:{}, slot id:{}", bta_jv_event_text(event),
                 id);
      break;
  }
}

static void handle_discovery_comp(tBTA_JV_STATUS status, int scn, uint32_t id) {
  rfc_slot_t* slot = find_rfc_slot_by_id(id);
  if (!slot) {
    log::error(
        "RFCOMM slot with id {} not found. event: BTA_JV_DISCOVERY_COMP_EVT",
        id);
    return;
  }

  if (!slot->f.doing_sdp_request) {
    log::error(
        "SDP response returned but RFCOMM slot {} did not request SDP record.",
        id);
    return;
  }

  if (status != tBTA_JV_STATUS::SUCCESS || !scn) {
    log::error(
        "SDP service discovery completed for slot id: {} with the result "
        "status: {}, scn: {}",
        id, bta_jv_status_text(status), scn);
    cleanup_rfc_slot(slot);
    return;
  }

  if (BTA_JvRfcommConnect(slot->security, scn, slot->addr, rfcomm_cback,
                          slot->id) != tBTA_JV_STATUS::SUCCESS) {
    log::warn(
        "BTA_JvRfcommConnect() returned BTA_JV_FAILURE for RFCOMM slot with "
        "id: {}",
        id);
    cleanup_rfc_slot(slot);
    return;
  }
  // Establish connection if successfully found channel number to connect.
  slot->scn = scn;
  slot->f.doing_sdp_request = false;

  if (!send_app_scn(slot)) {
    log::warn("send_app_scn() failed, closing slot->id {}", slot->id);
    cleanup_rfc_slot(slot);
    return;
  }
}

typedef enum {
  SENT_FAILED,
  SENT_NONE,
  SENT_PARTIAL,
  SENT_ALL,
} sent_status_t;

static sent_status_t send_data_to_app(int fd, BT_HDR* p_buf) {
  if (p_buf->len == 0) return SENT_ALL;

  ssize_t sent;
  OSI_NO_INTR(
      sent = send(fd, p_buf->data + p_buf->offset, p_buf->len, MSG_DONTWAIT));

  if (sent == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return SENT_NONE;
    log::error("error writing RFCOMM data back to app: {}", strerror(errno));
    return SENT_FAILED;
  }

  if (sent == 0) return SENT_FAILED;

  if (sent == p_buf->len) return SENT_ALL;

  p_buf->offset += sent;
  p_buf->len -= sent;
  return SENT_PARTIAL;
}

static bool flush_incoming_que_on_wr_signal(rfc_slot_t* slot) {
  while (!list_is_empty(slot->incoming_queue)) {
    BT_HDR* p_buf = (BT_HDR*)list_front(slot->incoming_queue);
    switch (send_data_to_app(slot->fd, p_buf)) {
      case SENT_NONE:
      case SENT_PARTIAL:
        // monitor the fd to get callback when app is ready to receive data
        btsock_thread_add_fd(pth, slot->fd, BTSOCK_RFCOMM, SOCK_THREAD_FD_WR,
                             slot->id);
        return true;

      case SENT_ALL:
        list_remove(slot->incoming_queue, p_buf);
        break;

      case SENT_FAILED:
        list_remove(slot->incoming_queue, p_buf);
        return false;
    }
  }

  // app is ready to receive data, tell stack to start the data flow
  // fix me: need a jv flow control api to serialize the call in stack
  log::verbose(
      "enable data flow, rfc_handle:0x{:x}, rfc_port_handle:0x{:x}, user_id:{}",
      slot->rfc_handle, slot->rfc_port_handle, slot->id);
  if (PORT_FlowControl_MaxCredit(slot->rfc_port_handle, true) != PORT_SUCCESS) {
    log::warn("Unable to open RFCOMM port peer:{}", slot->addr);
  }
  return true;
}

void btsock_rfc_signaled(int /* fd */, int flags, uint32_t id) {
  bool need_close = false;
  std::unique_lock<std::recursive_mutex> lock(slot_lock);
  rfc_slot_t* slot = find_rfc_slot_by_id(id);
  if (!slot) {
    log::warn("RFCOMM slot with id {} not found.", id);
    return;
  }

  // Data available from app, tell stack we have outgoing data.
  if (flags & SOCK_THREAD_FD_RD && !slot->f.server) {
    if (slot->f.connected) {
      // Make sure there's data pending in case the peer closed the socket.
      int size = 0;
      if (!(flags & SOCK_THREAD_FD_EXCEPTION) ||
          (ioctl(slot->fd, FIONREAD, &size) == 0 && size)) {
        BTA_JvRfcommWrite(slot->rfc_handle, slot->id);
      }
    } else {
      log::error(
          "socket signaled for read while disconnected, slot: {}, channel: {}",
          slot->id, slot->scn);
      need_close = true;
    }
  }

  if (flags & SOCK_THREAD_FD_WR) {
    // App is ready to receive more data, tell stack to enable data flow.
    if (!slot->f.connected || !flush_incoming_que_on_wr_signal(slot)) {
      log::error(
          "socket signaled for write while disconnected (or write failure), "
          "slot: {}, channel: {}",
          slot->id, slot->scn);
      need_close = true;
    }
  }

  if (need_close || (flags & SOCK_THREAD_FD_EXCEPTION)) {
    // Clean up if there's no data pending.
    int size = 0;
    if (need_close || ioctl(slot->fd, FIONREAD, &size) != 0 || !size)
      cleanup_rfc_slot(slot);
  }
}

int bta_co_rfc_data_incoming(uint32_t id, BT_HDR* p_buf) {
  int app_uid = -1;
  uint64_t bytes_rx = 0;
  int ret = 0;
  std::unique_lock<std::recursive_mutex> lock(slot_lock);
  rfc_slot_t* slot = find_rfc_slot_by_id(id);
  if (!slot) {
    log::error("RFCOMM slot with id {} not found.", id);
    return 0;
  }

  app_uid = slot->app_uid;
  bytes_rx = p_buf->len;

  if (list_is_empty(slot->incoming_queue)) {
    switch (send_data_to_app(slot->fd, p_buf)) {
      case SENT_NONE:
      case SENT_PARTIAL:
        list_append(slot->incoming_queue, p_buf);
        btsock_thread_add_fd(pth, slot->fd, BTSOCK_RFCOMM, SOCK_THREAD_FD_WR,
                             slot->id);
        break;

      case SENT_ALL:
        osi_free(p_buf);
        ret = 1;  // Enable data flow.
        break;

      case SENT_FAILED:
        osi_free(p_buf);
        cleanup_rfc_slot(slot);
        break;
    }
  } else {
    list_append(slot->incoming_queue, p_buf);
  }

  slot->rx_bytes += bytes_rx;
  uid_set_add_rx(uid_set, app_uid, bytes_rx);

  return ret;  // Return 0 to disable data flow.
}

int bta_co_rfc_data_outgoing_size(uint32_t id, int* size) {
  *size = 0;
  std::unique_lock<std::recursive_mutex> lock(slot_lock);
  rfc_slot_t* slot = find_rfc_slot_by_id(id);
  if (!slot) {
    log::error("RFCOMM slot with id {} not found.", id);
    return false;
  }

  if (ioctl(slot->fd, FIONREAD, size) != 0) {
    log::error("unable to determine bytes remaining to be read on fd {}: {}",
               slot->fd, strerror(errno));
    cleanup_rfc_slot(slot);
    return false;
  }

  return true;
}

int bta_co_rfc_data_outgoing(uint32_t id, uint8_t* buf, uint16_t size) {
  std::unique_lock<std::recursive_mutex> lock(slot_lock);
  rfc_slot_t* slot = find_rfc_slot_by_id(id);
  if (!slot) {
    log::error("RFCOMM slot with id {} not found.", id);
    return false;
  }

  ssize_t received;
  OSI_NO_INTR(received = recv(slot->fd, buf, size, 0));

  if (received != size) {
    log::error("error receiving RFCOMM data from app: {}", strerror(errno));
    cleanup_rfc_slot(slot);
    return false;
  }

  return true;
}

bt_status_t btsock_rfc_disconnect(const RawAddress* bd_addr) {
  log::assert_that(bd_addr != NULL, "assert failed: bd_addr != NULL");
  if (!is_init_done()) {
    log::error("BT not ready");
    return BT_STATUS_NOT_READY;
  }

  std::unique_lock<std::recursive_mutex> lock(slot_lock);
  for (size_t i = 0; i < ARRAY_SIZE(rfc_slots); ++i) {
    if (rfc_slots[i].id && rfc_slots[i].addr == *bd_addr) {
      cleanup_rfc_slot(&rfc_slots[i]);
    }
  }

  return BT_STATUS_SUCCESS;
}
