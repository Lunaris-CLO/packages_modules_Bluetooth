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

/*******************************************************************************
 *
 *  Filename:      btif_hh.c
 *
 *  Description:   HID Host Profile Bluetooth Interface
 *
 *
 ******************************************************************************/

#define LOG_TAG "bt_btif_hh"

#include "btif/include/btif_hh.h"

#include <bluetooth/log.h>
#include <com_android_bluetooth_flags.h>

#include <cstdint>

#include "bta_hh_co.h"
#include "bta_sec_api.h"
#include "btif/include/btif_common.h"
#include "btif/include/btif_metrics_logging.h"
#include "btif/include/btif_profile_storage.h"
#include "btif/include/btif_storage.h"
#include "btif/include/btif_util.h"
#include "include/hardware/bt_hh.h"
#include "main/shim/dumpsys.h"
#include "os/log.h"
#include "osi/include/allocator.h"
#include "stack/include/acl_api.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/bt_uuid16.h"
#include "stack/include/btm_ble_api.h"
#include "stack/include/hidh_api.h"
#include "types/raw_address.h"

#define COD_HID_KEYBOARD 0x0540
#define COD_HID_POINTING 0x0580
#define COD_HID_COMBO 0x05C0

#define HID_REPORT_CAPSLOCK 0x39
#define HID_REPORT_NUMLOCK 0x53
#define HID_REPORT_SCROLLLOCK 0x47

// For Apple Magic Mouse
#define MAGICMOUSE_VENDOR_ID 0x05ac
#define MAGICMOUSE_PRODUCT_ID 0x030d

#define LOGITECH_KB_MX5500_VENDOR_ID 0x046D
#define LOGITECH_KB_MX5500_PRODUCT_ID 0xB30B

using namespace bluetooth;

static int btif_hh_keylockstates = 0;  // The current key state of each key

#define BTIF_TIMEOUT_VUP_MS (3 * 1000)

/* HH request events */
typedef enum {
  BTIF_HH_CONNECT_REQ_EVT = 0,
  BTIF_HH_DISCONNECT_REQ_EVT,
  BTIF_HH_VUP_REQ_EVT
} btif_hh_req_evt_t;

/*******************************************************************************
 *  Constants & Macros
 ******************************************************************************/

/*******************************************************************************
 *  Local type definitions
 ******************************************************************************/

typedef struct hid_kb_list {
  uint16_t product_id;
  uint16_t version_id;
  const char* kb_name;
} tHID_KB_LIST;

/*******************************************************************************
 *  Static variables
 ******************************************************************************/
btif_hh_cb_t btif_hh_cb;

static bthh_callbacks_t* bt_hh_callbacks = NULL;
static bthh_profile_enable_t bt_hh_enable_type = {.hidp_enabled = true,
                                                  .hogp_enabled = true};

/* List of HID keyboards for which the NUMLOCK state needs to be
 * turned ON by default. Add devices to this list to apply the
 * NUMLOCK state toggle on fpr first connect.*/
static tHID_KB_LIST hid_kb_numlock_on_list[] = {{LOGITECH_KB_MX5500_PRODUCT_ID,
                                                 LOGITECH_KB_MX5500_VENDOR_ID,
                                                 "Logitech MX5500 Keyboard"}};

#define CHECK_BTHH_INIT()                 \
  do {                                    \
    if (bt_hh_callbacks == NULL) {        \
      log::error("BTHH not initialized"); \
      return BT_STATUS_NOT_READY;         \
    }                                     \
  } while (0)

#define BTHH_CHECK_NOT_DISABLED()                                           \
  do {                                                                      \
    if (btif_hh_cb.status == BTIF_HH_DISABLED) {                            \
      log::error("HH status = {}", btif_hh_status_text(btif_hh_cb.status)); \
      return BT_STATUS_UNEXPECTED_STATE;                                    \
    }                                                                       \
  } while (0)

#define BTHH_LOG_UNKNOWN_LINK(_link_spec) \
  log::error("Unknown link: {}", (_link_spec).ToRedactedStringForLogging())
#define BTHH_LOG_LINK(_link_spec) \
  log::verbose("link spec: {}", (_link_spec).ToRedactedStringForLogging())

#define BTHH_STATE_UPDATE(_link_spec, _state)                                \
  do {                                                                       \
    log::verbose("link spec: {} state: {}",                                  \
                 (_link_spec).ToRedactedStringForLogging(),                  \
                 bthh_connection_state_text(_state));                        \
    HAL_CBACK(bt_hh_callbacks, connection_state_cb, &(_link_spec).addrt.bda, \
              (_link_spec).addrt.type, (_link_spec).transport, (_state));    \
  } while (0)

/*******************************************************************************
 *  Static functions
 ******************************************************************************/

static void btif_hh_transport_select(tAclLinkSpec& link_spec);
/*******************************************************************************
 *  Externs
 ******************************************************************************/
bool check_cod(const RawAddress* remote_bdaddr, uint32_t cod);
bool check_cod_hid(const RawAddress* remote_bdaddr);
bool check_cod_hid_major(const RawAddress& bd_addr, uint32_t cod);
void bta_hh_co_close(btif_hh_device_t* p_dev);
void bta_hh_co_send_hid_info(btif_hh_device_t* p_dev, const char* dev_name,
                             uint16_t vendor_id, uint16_t product_id,
                             uint16_t version, uint8_t ctry_code, int dscp_len,
                             uint8_t* p_dscp);
void bta_hh_co_write(int fd, uint8_t* rpt, uint16_t len);
static void bte_hh_evt(tBTA_HH_EVT event, tBTA_HH* p_data);
void btif_dm_hh_open_failed(RawAddress* bdaddr);
void btif_hd_service_registration();
void btif_hh_timer_timeout(void* data);

/*******************************************************************************
 *  Functions
 ******************************************************************************/

static int get_keylockstates() { return btif_hh_keylockstates; }

static void set_keylockstate(int keymask, bool isSet) {
  if (isSet) btif_hh_keylockstates |= keymask;
}

/*******************************************************************************
 *
 * Function         toggle_os_keylockstates
 *
 * Description      Function to toggle the keyboard lock states managed by the
 linux.
 *                  This function is used in by two call paths
 *                  (1) if the lock state change occurred from an onscreen
 keyboard,
 *                  this function is called to update the lock state maintained
                    for the HID keyboard(s)
 *                  (2) if a HID keyboard is disconnected and reconnected,
 *                  this function is called to update the lock state maintained
                    for the HID keyboard(s)
 * Returns          void
 ******************************************************************************/

static void toggle_os_keylockstates(int fd, int changedlockstates) {
  log::verbose("fd = {}, changedlockstates = 0x{:x}", fd, changedlockstates);
  uint8_t hidreport[9];
  int reportIndex;
  memset(hidreport, 0, 9);
  hidreport[0] = 1;
  reportIndex = 4;

  if (changedlockstates & BTIF_HH_KEYSTATE_MASK_CAPSLOCK) {
    log::verbose("Setting CAPSLOCK");
    hidreport[reportIndex++] = (uint8_t)HID_REPORT_CAPSLOCK;
  }

  if (changedlockstates & BTIF_HH_KEYSTATE_MASK_NUMLOCK) {
    log::verbose("Setting NUMLOCK");
    hidreport[reportIndex++] = (uint8_t)HID_REPORT_NUMLOCK;
  }

  if (changedlockstates & BTIF_HH_KEYSTATE_MASK_SCROLLLOCK) {
    log::verbose("Setting SCROLLLOCK");
    hidreport[reportIndex++] = (uint8_t)HID_REPORT_SCROLLLOCK;
  }

  log::verbose("Writing hidreport #1 to os:");
  log::verbose("| {:x} {:x} {:x}", hidreport[0], hidreport[1], hidreport[2]);
  log::verbose("| {:x} {:x} {:x}", hidreport[3], hidreport[4], hidreport[5]);
  log::verbose("| {:x} {:x} {:x}", hidreport[6], hidreport[7], hidreport[8]);
  bta_hh_co_write(fd, hidreport, sizeof(hidreport));
  usleep(200000);
  memset(hidreport, 0, 9);
  hidreport[0] = 1;
  log::verbose("Writing hidreport #2 to os:");
  log::verbose("| {:x} {:x} {:x}", hidreport[0], hidreport[1], hidreport[2]);
  log::verbose("| {:x} {:x} {:x}", hidreport[3], hidreport[4], hidreport[5]);
  log::verbose("| {:x} {:x} {:x}", hidreport[6], hidreport[7], hidreport[8]);
  bta_hh_co_write(fd, hidreport, sizeof(hidreport));
}

/*******************************************************************************
 *
 * Function         create_pbuf
 *
 * Description      Helper function to create p_buf for send_data or set_report
 *
 ******************************************************************************/
static BT_HDR* create_pbuf(uint16_t len, uint8_t* data) {
  BT_HDR* p_buf = (BT_HDR*)osi_malloc(len + BTA_HH_MIN_OFFSET + sizeof(BT_HDR));
  uint8_t* pbuf_data;

  p_buf->len = len;
  p_buf->offset = BTA_HH_MIN_OFFSET;

  pbuf_data = (uint8_t*)(p_buf + 1) + p_buf->offset;
  memcpy(pbuf_data, data, len);

  return p_buf;
}

/*******************************************************************************
 *
 * Function         update_keyboard_lockstates
 *
 * Description      Sends a report to the keyboard to set the lock states of
 *                  keys.
 *
 ******************************************************************************/
static void update_keyboard_lockstates(btif_hh_device_t* p_dev) {
  uint8_t len = 2; /* reportid + 1 byte report*/
  BT_HDR* p_buf;
  uint8_t data[] = {0x01, /* report id */
                    static_cast<uint8_t>(btif_hh_keylockstates)}; /* keystate */

  /* Set report for other keyboards */
  log::verbose("setting report on dev_handle {} to 0x{:x}", p_dev->dev_handle,
               btif_hh_keylockstates);

  /* Get SetReport buffer */
  p_buf = create_pbuf(len, data);
  if (p_buf != NULL) {
    p_buf->layer_specific = BTA_HH_RPTT_OUTPUT;
    BTA_HhSendData(p_dev->dev_handle, p_dev->link_spec, p_buf);
  }
}

/*******************************************************************************
 *
 * Function         sync_lockstate_on_connect
 *
 * Description      Function to update the keyboard lock states managed by the
 *                  OS when a HID keyboard is connected or disconnected and
 *                  reconnected
 *
 * Returns          void
 ******************************************************************************/
static void sync_lockstate_on_connect(btif_hh_device_t* p_dev) {
  int keylockstates;

  log::verbose("Syncing keyboard lock states after reconnect...");
  /*If the device is connected, update keyboard state */
  update_keyboard_lockstates(p_dev);

  /*Check if the lockstate of caps,scroll,num is set.
   If so, send a report to the kernel
  so the lockstate is in sync */
  keylockstates = get_keylockstates();
  if (keylockstates) {
    log::verbose(
        "Sending hid report to kernel indicating lock key state 0x{:x}",
        keylockstates);
    usleep(200000);
    toggle_os_keylockstates(p_dev->uhid.fd, keylockstates);
  } else {
    log::verbose(
        "NOT sending hid report to kernel indicating lock key state 0x{:x}",
        keylockstates);
  }
}

/*******************************************************************************
 *
 * Function         btif_hh_find_added_dev
 *
 * Description      Return the added device pointer of the specified link spec
 *
 * Returns          Added device entry
 ******************************************************************************/
btif_hh_added_device_t* btif_hh_find_added_dev(const tAclLinkSpec& link_spec) {
  for (int i = 0; i < BTIF_HH_MAX_ADDED_DEV; i++) {
    btif_hh_added_device_t* added_dev = &btif_hh_cb.added_devices[i];
    if (added_dev->link_spec == link_spec) {
      return added_dev;
    }
  }
  return NULL;
}

/*******************************************************************************
 *
 * Function         btif_hh_find_connected_dev_by_handle
 *
 * Description      Return the connected device pointer of the specified device
 *                  handle
 *
 * Returns          Device entry pointer in the device table
 ******************************************************************************/
btif_hh_device_t* btif_hh_find_connected_dev_by_handle(uint8_t handle) {
  uint32_t i;
  for (i = 0; i < BTIF_HH_MAX_HID; i++) {
    if (btif_hh_cb.devices[i].dev_status == BTHH_CONN_STATE_CONNECTED &&
        btif_hh_cb.devices[i].dev_handle == handle) {
      return &btif_hh_cb.devices[i];
    }
  }
  return NULL;
}

/*******************************************************************************
 *
 * Function         btif_hh_find_dev_by_handle
 *
 * Description      Return the device pointer of the specified device handle
 *
 * Returns          Device entry pointer in the device table
 ******************************************************************************/
btif_hh_device_t* btif_hh_find_dev_by_handle(uint8_t handle) {
  for (int i = 0; i < BTIF_HH_MAX_HID; i++) {
    btif_hh_device_t* p_dev = &btif_hh_cb.devices[i];
    if (p_dev->dev_status != BTHH_CONN_STATE_UNKNOWN &&
        p_dev->dev_handle == handle) {
      return p_dev;
    }
  }
  return nullptr;
}

/*******************************************************************************
 *
 * Function         btif_hh_find_empty_dev
 *
 * Description      Return an empty device
 *
 * Returns          Device entry pointer in the device table
 ******************************************************************************/
btif_hh_device_t* btif_hh_find_empty_dev(void) {
  for (int i = 0; i < BTIF_HH_MAX_HID; i++) {
    btif_hh_device_t* p_dev = &btif_hh_cb.devices[i];
    if (p_dev->dev_status == BTHH_CONN_STATE_UNKNOWN) {
      return p_dev;
    }
  }
  return nullptr;
}

/*******************************************************************************
 *
 * Function         btif_hh_find_dev_by_link_spec
 *
 * Description      Return the device pointer of the specified ACL link
 *                  specification.
 *
 * Returns          Device entry pointer in the device table
 ******************************************************************************/
static btif_hh_device_t* btif_hh_find_dev_by_link_spec(
    const tAclLinkSpec& link_spec) {
  uint32_t i;
  for (i = 0; i < BTIF_HH_MAX_HID; i++) {
    if (btif_hh_cb.devices[i].dev_status != BTHH_CONN_STATE_UNKNOWN &&
        btif_hh_cb.devices[i].link_spec == link_spec) {
      return &btif_hh_cb.devices[i];
    }
  }
  return NULL;
}

/*******************************************************************************
 *
 * Function         btif_hh_find_connected_dev_by_link_spec
 *
 * Description      Return the connected device pointer of the specified ACL
 *                  link specification.
 *
 * Returns          Device entry pointer in the device table
 ******************************************************************************/
static btif_hh_device_t* btif_hh_find_connected_dev_by_link_spec(
    const tAclLinkSpec& link_spec) {
  uint32_t i;
  for (i = 0; i < BTIF_HH_MAX_HID; i++) {
    if (btif_hh_cb.devices[i].dev_status == BTHH_CONN_STATE_CONNECTED &&
        btif_hh_cb.devices[i].link_spec == link_spec) {
      return &btif_hh_cb.devices[i];
    }
  }
  return NULL;
}

/*******************************************************************************
 *
 * Function      btif_hh_stop_vup_timer
 *
 * Description  stop vitual unplug timer
 *
 * Returns      void
 ******************************************************************************/
static void btif_hh_stop_vup_timer(const tAclLinkSpec& link_spec) {
  btif_hh_device_t* p_dev = btif_hh_find_connected_dev_by_link_spec(link_spec);

  if (p_dev != NULL) {
    log::verbose("stop VUP timer");
    alarm_free(p_dev->vup_timer);
    p_dev->vup_timer = NULL;
  }
}
/*******************************************************************************
 *
 * Function      btif_hh_start_vup_timer
 *
 * Description  start virtual unplug timer
 *
 * Returns      void
 ******************************************************************************/
static void btif_hh_start_vup_timer(const tAclLinkSpec& link_spec) {
  log::verbose("");

  btif_hh_device_t* p_dev = btif_hh_find_connected_dev_by_link_spec(link_spec);
  log::assert_that(p_dev != NULL, "assert failed: p_dev != NULL");

  alarm_free(p_dev->vup_timer);
  p_dev->vup_timer = alarm_new("btif_hh.vup_timer");
  alarm_set_on_mloop(p_dev->vup_timer, BTIF_TIMEOUT_VUP_MS,
                     btif_hh_timer_timeout, p_dev);
}

static bthh_connection_state_t hh_get_state_on_disconnect(
    tAclLinkSpec& link_spec) {
  if (!com::android::bluetooth::flags::allow_switching_hid_and_hogp()) {
    return BTHH_CONN_STATE_ACCEPTING;
  }

  btif_hh_added_device_t* added_dev = btif_hh_find_added_dev(link_spec);
  if (added_dev != nullptr) {
    return added_dev->reconnect_allowed ? BTHH_CONN_STATE_ACCEPTING
                                        : BTHH_CONN_STATE_DISCONNECTED;
  } else {
    return BTHH_CONN_STATE_DISCONNECTED;
  }
}

static void hh_connect_complete(uint8_t handle, tAclLinkSpec& link_spec,
                                BTIF_HH_STATUS status) {
  bthh_connection_state_t state = BTHH_CONN_STATE_CONNECTED;
  btif_hh_cb.status = status;

  if (status != BTIF_HH_DEV_CONNECTED) {
    state = BTHH_CONN_STATE_DISCONNECTED;
    BTA_HhClose(handle);
  }
  BTHH_STATE_UPDATE(link_spec, state);
}

static void hh_open_handler(tBTA_HH_CONN& conn) {
  log::debug("link spec = {}, status = {}, handle = {}",
             conn.link_spec.ToRedactedStringForLogging(), conn.status,
             conn.handle);

  if (com::android::bluetooth::flags::allow_switching_hid_and_hogp()) {
    // Initialize with disconnected/accepting state based on reconnection policy
    bthh_connection_state_t dev_status =
        hh_get_state_on_disconnect(conn.link_spec);

    // Use current state if the device instance already exists
    btif_hh_device_t* p_dev = btif_hh_find_dev_by_link_spec(conn.link_spec);
    if (p_dev != nullptr) {
      log::debug("Device instance found: {}, state: {}",
                 p_dev->link_spec.ToRedactedStringForLogging(),
                 bthh_connection_state_text(p_dev->dev_status));
      dev_status = p_dev->dev_status;
    } else {
      auto bdStr = conn.link_spec.addrt.bda.ToString();
      if (btif_in_fetch_bonded_device(bdStr) == BT_STATUS_SUCCESS) {
        dev_status = BTHH_CONN_STATE_ACCEPTING;
      }
    }

    if (btif_hh_cb.pending_link_spec == conn.link_spec) {
      log::verbose("Device connection was pending for: {}, status: {}",
                   conn.link_spec.ToRedactedStringForLogging(),
                   btif_hh_status_text(btif_hh_cb.status));
      dev_status = BTHH_CONN_STATE_CONNECTING;
    }

    if (dev_status != BTHH_CONN_STATE_ACCEPTING &&
        dev_status != BTHH_CONN_STATE_CONNECTING) {
      log::warn("Reject Incoming HID Connection, device: {}, state: {}",
                conn.link_spec.ToRedactedStringForLogging(),
                bthh_connection_state_text(dev_status));
      log_counter_metrics_btif(android::bluetooth::CodePathCounterKeyEnum::
                                   HIDH_COUNT_INCOMING_CONNECTION_REJECTED,
                               1);

      if (p_dev != nullptr) {
        p_dev->dev_status = BTHH_CONN_STATE_DISCONNECTED;
      }

      if (!com::android::bluetooth::flags::suppress_hid_rejection_broadcast()) {
        hh_connect_complete(conn.handle, conn.link_spec,
                            BTIF_HH_DEV_DISCONNECTED);
        return;
      }
      BTA_HhClose(conn.handle);
      return;
    }
  }

  if (!com::android::bluetooth::flags::allow_switching_hid_and_hogp()) {
    BTHH_STATE_UPDATE(conn.link_spec, BTHH_CONN_STATE_CONNECTING);
  }

  btif_hh_cb.pending_link_spec = {};

  if (conn.status != BTA_HH_OK) {
    btif_dm_hh_open_failed(&conn.link_spec.addrt.bda);
    btif_hh_device_t* p_dev = btif_hh_find_dev_by_link_spec(conn.link_spec);
    if (p_dev != NULL) {
      btif_hh_stop_vup_timer(p_dev->link_spec);

      p_dev->dev_status = hh_get_state_on_disconnect(p_dev->link_spec);
    }
    hh_connect_complete(conn.handle, conn.link_spec, BTIF_HH_DEV_DISCONNECTED);
    return;
  }

  /* Initialize device driver */
  if (!bta_hh_co_open(conn.handle, conn.sub_class, conn.attr_mask, conn.app_id,
                      conn.link_spec)) {
    log::warn("Failed to find the uhid driver");
    hh_connect_complete(conn.handle, conn.link_spec, BTIF_HH_DEV_DISCONNECTED);
    return;
  }

  btif_hh_device_t* p_dev = btif_hh_find_connected_dev_by_handle(conn.handle);
  if (p_dev == NULL) {
    /* The connect request must have come from device side and exceeded the
     * connected HID device number. */
    log::warn("Cannot find device with handle {}", conn.handle);
    hh_connect_complete(conn.handle, conn.link_spec, BTIF_HH_DEV_DISCONNECTED);
    return;
  }

  log::info("Found device, getting dscp info for handle {}", conn.handle);

  p_dev->link_spec = conn.link_spec;
  p_dev->dev_status = BTHH_CONN_STATE_CONNECTED;
  hh_connect_complete(conn.handle, conn.link_spec, BTIF_HH_DEV_CONNECTED);
  // Send set_idle if the peer_device is a keyboard
  if (check_cod_hid_major(conn.link_spec.addrt.bda, COD_HID_KEYBOARD) ||
      check_cod_hid_major(conn.link_spec.addrt.bda, COD_HID_COMBO)) {
    BTA_HhSetIdle(conn.handle, 0);
  }
  BTA_HhGetDscpInfo(conn.handle);
}

/*******************************************************************************
 *
 * Function         hh_add_device
 *
 * Description      Add a new device to the added device list.
 *
 * Returns          true if add successfully, otherwise false.
 ******************************************************************************/
static bool hh_add_device(const tAclLinkSpec& link_spec,
                          tBTA_HH_ATTR_MASK attr_mask, bool reconnect_allowed) {
  int i;

  // Check if already added
  if (btif_hh_find_added_dev(link_spec) != nullptr) {
    log::warn("Device {} already added",
              link_spec.ToRedactedStringForLogging());
    return false;
  }

  // Use an empty slot for the new device
  for (i = 0; i < BTIF_HH_MAX_ADDED_DEV; i++) {
    btif_hh_added_device_t& dev = btif_hh_cb.added_devices[i];
    if (dev.link_spec.addrt.bda.IsEmpty()) {
      log::info("Added device {}", link_spec.ToRedactedStringForLogging());
      dev.link_spec = link_spec;
      dev.dev_handle = BTA_HH_INVALID_HANDLE;
      dev.attr_mask = attr_mask;
      dev.reconnect_allowed = reconnect_allowed;
      return true;
    }
  }

  log::error("Out of space to add device");
  log_counter_metrics_btif(android::bluetooth::CodePathCounterKeyEnum::
                               HIDH_COUNT_MAX_ADDED_DEVICE_LIMIT_REACHED,
                           1);
  return false;
}

void btif_hh_load_bonded_dev(const tAclLinkSpec& link_spec_ref,
                             tBTA_HH_ATTR_MASK attr_mask, uint8_t sub_class,
                             uint8_t app_id, tBTA_HH_DEV_DSCP_INFO dscp_info,
                             bool reconnect_allowed) {
  btif_hh_device_t* p_dev;
  uint8_t i;
  tAclLinkSpec link_spec = link_spec_ref;

  if (com::android::bluetooth::flags::allow_switching_hid_and_hogp() &&
      link_spec.transport == BT_TRANSPORT_AUTO) {
    log::warn("Resolving link spec {} transport to BREDR/LE",
              link_spec.ToRedactedStringForLogging());
    btif_hh_transport_select(link_spec);
    reconnect_allowed = true;
    btif_storage_set_hid_connection_policy(link_spec, reconnect_allowed);

    // remove and re-write the hid info
    btif_storage_remove_hid_info(link_spec);
    btif_storage_add_hid_device_info(
        link_spec, attr_mask, sub_class, app_id, dscp_info.vendor_id,
        dscp_info.product_id, dscp_info.version, dscp_info.ctry_code,
        dscp_info.ssr_max_latency, dscp_info.ssr_min_tout,
        dscp_info.descriptor.dl_len, dscp_info.descriptor.dsc_list);
  }

  if (hh_add_device(link_spec, attr_mask, reconnect_allowed)) {
    if (com::android::bluetooth::flags::allow_switching_hid_and_hogp() &&
        reconnect_allowed) {
      BTHH_STATE_UPDATE(link_spec, BTHH_CONN_STATE_ACCEPTING);
    }
    BTA_HhAddDev(link_spec, attr_mask, sub_class, app_id, dscp_info);
  }
}

/*******************************************************************************
 **
 ** Function         btif_hh_remove_device
 **
 ** Description      Remove an added device from the stack.
 **
 ** Returns          void
 ******************************************************************************/
void btif_hh_remove_device(const tAclLinkSpec& link_spec) {
  int i;
  btif_hh_device_t* p_dev;
  btif_hh_added_device_t* p_added_dev;

  BTHH_LOG_LINK(link_spec);

  for (i = 0; i < BTIF_HH_MAX_ADDED_DEV; i++) {
    p_added_dev = &btif_hh_cb.added_devices[i];
    if (p_added_dev->link_spec == link_spec) {
      BTA_HhRemoveDev(p_added_dev->dev_handle);
      btif_storage_remove_hid_info(p_added_dev->link_spec);
      p_added_dev->link_spec = {};
      p_added_dev->dev_handle = BTA_HH_INVALID_HANDLE;

      /* Look for other instances only if AUTO transport was used */
      if (link_spec.transport != BT_TRANSPORT_AUTO) {
        break;
      }
    }
  }

  /* Remove all connections instances related to link_spec. If AUTO transport is
   * used, btif_hh_find_dev_by_link_spec() finds both HID and HOGP instances */
  while ((p_dev = btif_hh_find_dev_by_link_spec(link_spec)) != NULL) {

    if (btif_hh_cb.device_num > 0) {
      btif_hh_cb.device_num--;
    } else {
      log::warn("device_num = 0");
    }
    bta_hh_co_close(p_dev);
    p_dev->dev_status = BTHH_CONN_STATE_UNKNOWN;
    p_dev->dev_handle = BTA_HH_INVALID_HANDLE;
    p_dev->uhid.ready_for_data = false;
    /* need to notify up-layer device is disconnected to avoid state out of sync
     * with up-layer */

    do_in_jni_thread(base::Bind(
        [](tAclLinkSpec link_spec) {
          BTHH_STATE_UPDATE(link_spec, BTHH_CONN_STATE_DISCONNECTED);
        },
         p_dev->link_spec));
  }
  if ((btif_hh_cb.pending_link_spec.addrt.bda == link_spec.addrt.bda) &&
         (btif_hh_cb.status == BTIF_HH_DEV_CONNECTING)) {
       log::warn("reset pending connection status");
       btif_hh_cb.status = (BTIF_HH_STATUS)BTIF_HH_DEV_DISCONNECTED;
       btif_hh_cb.pending_link_spec = {};
       /* need to notify up-layer device is disconnected to avoid
        * state out of sync with up-layer */
       do_in_jni_thread(base::Bind(
           [](tAclLinkSpec link_spec) {
             BTHH_STATE_UPDATE(link_spec, BTHH_CONN_STATE_DISCONNECTED);
           },
           link_spec));
   }

}

bool btif_hh_copy_hid_info(tBTA_HH_DEV_DSCP_INFO* dest,
                           tBTA_HH_DEV_DSCP_INFO* src) {
  memset(dest, 0, sizeof(tBTA_HH_DEV_DSCP_INFO));
  dest->descriptor.dl_len = 0;
  if (src->descriptor.dl_len > 0) {
    dest->descriptor.dsc_list = (uint8_t*)osi_malloc(src->descriptor.dl_len);
  }
  memcpy(dest->descriptor.dsc_list, src->descriptor.dsc_list,
         src->descriptor.dl_len);
  dest->descriptor.dl_len = src->descriptor.dl_len;
  dest->vendor_id = src->vendor_id;
  dest->product_id = src->product_id;
  dest->version = src->version;
  dest->ctry_code = src->ctry_code;
  dest->ssr_max_latency = src->ssr_max_latency;
  dest->ssr_min_tout = src->ssr_min_tout;
  return true;
}

/*******************************************************************************
 *
 * Function         btif_hh_virtual_unplug
 *
 * Description      Virtual unplug initiated from the BTIF thread context
 *                  Special handling for HID mouse-
 *
 * Returns          void
 *
 ******************************************************************************/

bt_status_t btif_hh_virtual_unplug(const tAclLinkSpec& link_spec_input) {
  BTHH_LOG_LINK(link_spec_input);
  tAclLinkSpec link_spec = link_spec_input;
  btif_hh_device_t* p_dev;
  if (com::android::bluetooth::flags::allow_switching_hid_and_hogp() &&
        link_spec.transport == BT_TRANSPORT_AUTO) {
      log::warn("Resolving link spec {} transport to BREDR/LE",
            link_spec.ToRedactedStringForLogging());
      btif_hh_transport_select(link_spec);
  }
  p_dev = btif_hh_find_dev_by_link_spec(link_spec);
  if ((p_dev != NULL) && (p_dev->dev_status == BTHH_CONN_STATE_CONNECTED) &&
      (p_dev->attr_mask & HID_VIRTUAL_CABLE)) {
    log::verbose("Sending BTA_HH_CTRL_VIRTUAL_CABLE_UNPLUG for: {}",
                 link_spec.ToRedactedStringForLogging());
    /* start the timer */
    btif_hh_start_vup_timer(link_spec);
    p_dev->local_vup = true;
    BTA_HhSendCtrl(p_dev->dev_handle, BTA_HH_CTRL_VIRTUAL_CABLE_UNPLUG);
    return BT_STATUS_SUCCESS;
  } else if ((p_dev != NULL) &&
             (p_dev->dev_status == BTHH_CONN_STATE_CONNECTED)) {
    log::error("Virtual unplug not supported, disconnecting device: {}",
               link_spec.ToRedactedStringForLogging());
    /* start the timer */
    btif_hh_start_vup_timer(link_spec);
    p_dev->local_vup = true;
    BTA_HhClose(p_dev->dev_handle);
    return BT_STATUS_SUCCESS;
  } else {
    log::error("Error, device {} not opened, status = {}",
               link_spec.ToRedactedStringForLogging(),
               btif_hh_status_text(btif_hh_cb.status));
    if ((btif_hh_cb.pending_link_spec.addrt.bda == link_spec.addrt.bda) &&
        (btif_hh_cb.status == BTIF_HH_DEV_CONNECTING)) {
      btif_hh_cb.status = (BTIF_HH_STATUS)BTIF_HH_DEV_DISCONNECTED;
      btif_hh_cb.pending_link_spec = {};

      /* need to notify up-layer device is disconnected to avoid
       * state out of sync with up-layer */
      do_in_jni_thread(base::Bind(
          [](tAclLinkSpec link_spec) {
            BTHH_STATE_UPDATE(link_spec, BTHH_CONN_STATE_DISCONNECTED);
          },
          link_spec));
    }
    return BT_STATUS_DEVICE_NOT_FOUND;
  }
}

/*******************************************************************************
 *
 * Function         btif_hh_connect
 *
 * Description      connection initiated from the BTIF thread context
 *
 * Returns          int status
 *
 ******************************************************************************/

bt_status_t btif_hh_connect(const tAclLinkSpec& link_spec) {
  CHECK_BTHH_INIT();
  log::verbose("BTHH");
  btif_hh_device_t* p_dev = btif_hh_find_dev_by_link_spec(link_spec);
  if (!p_dev && btif_hh_cb.device_num >= BTIF_HH_MAX_HID) {
    // No space for more HID device now.
    log::warn("Error, exceeded the maximum supported HID device number {}",
              BTIF_HH_MAX_HID);
    log_counter_metrics_btif(
        android::bluetooth::CodePathCounterKeyEnum::
            HIDH_COUNT_CONNECT_REQ_WHEN_MAX_DEVICE_LIMIT_REACHED,
        1);
    return BT_STATUS_NOMEM;
  }

  btif_hh_added_device_t* added_dev = btif_hh_find_added_dev(link_spec);
  if (added_dev != nullptr) {
    log::info("Device {} already added, attr_mask = 0x{:x}",
              link_spec.ToRedactedStringForLogging(), added_dev->attr_mask);

    if (added_dev->dev_handle == BTA_HH_INVALID_HANDLE) {
      // No space for more HID device now.
      log::error("Device {} added but addition failed",
                 link_spec.ToRedactedStringForLogging());
      added_dev->link_spec = {};
      added_dev->dev_handle = BTA_HH_INVALID_HANDLE;
      return BT_STATUS_NOMEM;
    }

    // Reset the connection policy to allow incoming reconnections
    if (com::android::bluetooth::flags::allow_switching_hid_and_hogp()) {
      added_dev->reconnect_allowed = true;
      btif_storage_set_hid_connection_policy(link_spec, true);
    }
  }

  if (p_dev && p_dev->dev_status == BTHH_CONN_STATE_CONNECTED) {
    log::debug("HidHost profile already connected for {}",
               link_spec.ToRedactedStringForLogging());
    return BT_STATUS_SUCCESS;
  }

  if (p_dev) {
    p_dev->dev_status = BTHH_CONN_STATE_CONNECTING;
  }

  /* Not checking the NORMALLY_Connectible flags from sdp record, and anyways
   sending this request from host, for subsequent user initiated connection.
   If the remote is not in pagescan mode, we will do 2 retries to connect before
   giving up */
  btif_hh_cb.status = BTIF_HH_DEV_CONNECTING;
  btif_hh_cb.pending_link_spec = link_spec;
  do_in_jni_thread(base::Bind(
      [](tAclLinkSpec link_spec) {
        BTHH_STATE_UPDATE(link_spec, BTHH_CONN_STATE_CONNECTING);
      },
      link_spec));
  BTA_HhOpen(btif_hh_cb.pending_link_spec);

  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         btif_hh_disconnect
 *
 * Description      disconnection initiated from the BTIF thread context
 *
 * Returns          void
 *
 ******************************************************************************/
void btif_hh_disconnect(const tAclLinkSpec& link_spec) {
  btif_hh_device_t* p_dev = btif_hh_find_connected_dev_by_link_spec(link_spec);
  if (p_dev == nullptr) {
    log::warn("Unable to disconnect unknown HID device:{}",
              link_spec.ToRedactedStringForLogging());
    return;
  }
  log::debug("Disconnect and close request for HID device:{}",
             link_spec.ToRedactedStringForLogging());
  BTA_HhClose(p_dev->dev_handle);
}

/*******************************************************************************
 *
 * Function         btif_btif_hh_setreport
 *
 * Description      setreport initiated from the UHID thread context
 *
 * Returns          void
 *
 ******************************************************************************/
void btif_hh_setreport(btif_hh_uhid_t* p_uhid, bthh_report_type_t r_type,
                       uint16_t size, uint8_t* report) {
  BT_HDR* p_buf = create_pbuf(size, report);
  if (p_buf == NULL) {
    log::error("Error, failed to allocate RPT buffer, size = {}", size);
    return;
  }
  BTA_HhSetReport(p_uhid->dev_handle, r_type, p_buf);
}

/*******************************************************************************
 *
 * Function         btif_btif_hh_senddata
 *
 * Description      senddata initiated from the UHID thread context
 *
 * Returns          void
 *
 ******************************************************************************/
void btif_hh_senddata(btif_hh_uhid_t* p_uhid, uint16_t size, uint8_t* report) {
  BT_HDR* p_buf = create_pbuf(size, report);
  if (p_buf == NULL) {
    log::error("Error, failed to allocate RPT buffer, size = {}", size);
    return;
  }
  p_buf->layer_specific = BTA_HH_RPTT_OUTPUT;
  BTA_HhSendData(p_uhid->dev_handle, p_uhid->link_spec, p_buf);
}

/*******************************************************************************
 *
 * Function         btif_hh_service_registration
 *
 * Description      Registers or derigisters the hid host service
 *
 * Returns          none
 *
 ******************************************************************************/
void btif_hh_service_registration(bool enable) {
  log::verbose("");

  log::verbose("enable = {}", enable);
  if (bt_hh_callbacks == NULL) {
    // The HID Host service was never initialized (it is either disabled or not
    // available in this build). We should proceed directly to changing the HID
    // Device service state (if needed).
    if (!enable) {
      btif_hd_service_registration();
    }
  } else if (enable) {
    BTA_HhEnable(bte_hh_evt, bt_hh_enable_type.hidp_enabled,
                 bt_hh_enable_type.hogp_enabled);
  } else {
    btif_hh_cb.service_dereg_active = TRUE;
    BTA_HhDisable();
  }
}

/*******************************************************************************
 *
 *
 * Function         btif_hh_getreport
 *
 * Description      getreport initiated from the UHID thread context
 *
 * Returns          void
 *
 ******************************************************************************/
void btif_hh_getreport(btif_hh_uhid_t* p_uhid, bthh_report_type_t r_type,
                       uint8_t reportId, uint16_t bufferSize) {
  BTA_HhGetReport(p_uhid->dev_handle, r_type, reportId, bufferSize);
}

/*****************************************************************************
 *   Section name (Group of functions)
 ****************************************************************************/

/*****************************************************************************
 *
 *   btif hh api functions (no context switch)
 *
 ****************************************************************************/

/*******************************************************************************
 *
 * Function         btif_hh_upstreams_evt
 *
 * Description      Executes HH UPSTREAMS events in btif context
 *
 * Returns          void
 *
 ******************************************************************************/
static void btif_hh_upstreams_evt(uint16_t event, char* p_param) {
  tBTA_HH* p_data = (tBTA_HH*)p_param;
  btif_hh_device_t* p_dev = NULL;

  log::verbose("event={} dereg = {}", bta_hh_event_text(event),
               btif_hh_cb.service_dereg_active);

  switch (event) {
    case BTA_HH_ENABLE_EVT:
      log::verbose("BTA_HH_ENABLE_EVT: status ={}", p_data->status);
      if (p_data->status == BTA_HH_OK) {
        btif_hh_cb.status = BTIF_HH_ENABLED;
        log::verbose("Loading added devices");
        /* Add hid descriptors for already bonded hid devices*/
        btif_storage_load_bonded_hid_info();
      } else {
        btif_hh_cb.status = BTIF_HH_DISABLED;
        log::warn("BTA_HH_ENABLE_EVT: HH enabling failed, status = {}",
                  p_data->status);
      }
      break;

    case BTA_HH_DISABLE_EVT:
      if (btif_hh_cb.status == BTIF_HH_DISABLING) {
        bt_hh_callbacks = NULL;
      }

      btif_hh_cb.status = BTIF_HH_DISABLED;
      if (btif_hh_cb.service_dereg_active) {
        log::verbose("BTA_HH_DISABLE_EVT: enabling HID Device service");
        btif_hd_service_registration();
        btif_hh_cb.service_dereg_active = FALSE;
      }
      if (p_data->status == BTA_HH_OK) {
        int i;
        // Clear the control block
        for (i = 0; i < BTIF_HH_MAX_HID; i++) {
          alarm_free(btif_hh_cb.devices[i].vup_timer);
        }
        memset(&btif_hh_cb, 0, sizeof(btif_hh_cb));
        for (i = 0; i < BTIF_HH_MAX_HID; i++) {
          btif_hh_cb.devices[i].dev_status = BTHH_CONN_STATE_UNKNOWN;
        }
      } else {
        log::warn("BTA_HH_DISABLE_EVT: HH disabling failed, status = {}",
                  p_data->status);
      }
      break;

    case BTA_HH_OPEN_EVT:
      hh_open_handler(p_data->conn);
      break;

    case BTA_HH_CLOSE_EVT:
      log::verbose("BTA_HH_CLOSE_EVT: status = {}, handle = {}",
                   p_data->dev_status.status, p_data->dev_status.handle);
      p_dev = btif_hh_find_connected_dev_by_handle(p_data->dev_status.handle);
      if (p_dev != NULL) {
        BTHH_STATE_UPDATE(p_dev->link_spec, BTHH_CONN_STATE_DISCONNECTING);
        log::verbose("uhid fd={} local_vup={}", p_dev->uhid.fd,
                     p_dev->local_vup);
        btif_hh_stop_vup_timer(p_dev->link_spec);
        /* If this is a locally initiated VUP, remove the bond as ACL got
         *  disconnected while VUP being processed.
         */
        if (p_dev->local_vup) {
          p_dev->local_vup = false;
          BTA_DmRemoveDevice(p_dev->link_spec.addrt.bda);
        } else if (p_data->dev_status.status == BTA_HH_HS_SERVICE_CHANGED) {
          /* Local disconnection due to service change in the HOGP device.
             HID descriptor would be read again, so remove it from cache. */
          log::warn(
              "Removing cached descriptor due to service change, handle = {}",
              p_data->dev_status.handle);
          btif_storage_remove_hid_info(p_dev->link_spec);
        }

        btif_hh_cb.status = (BTIF_HH_STATUS)BTIF_HH_DEV_DISCONNECTED;
        p_dev->dev_status = hh_get_state_on_disconnect(p_dev->link_spec);

        bta_hh_co_close(p_dev);
        BTHH_STATE_UPDATE(p_dev->link_spec, p_dev->dev_status);
      } else {
        log::warn("Error: cannot find device with handle {}",
                  p_data->dev_status.handle);
      }
      break;

    case BTA_HH_GET_RPT_EVT: {
      log::verbose("BTA_HH_GET_RPT_EVT: status = {}, handle = {}",
                   p_data->hs_data.status, p_data->hs_data.handle);
      p_dev = btif_hh_find_connected_dev_by_handle(p_data->hs_data.handle);
      if (p_dev) {
        BT_HDR* hdr = p_data->hs_data.rsp_data.p_rpt_data;
        tBTA_HH_STATUS status = p_data->hs_data.status;

        if (status == BTA_HH_OK && hdr) { /* Get report response */
          uint8_t* data = (uint8_t*)(hdr + 1) + hdr->offset;
          uint16_t len = hdr->len;
          HAL_CBACK(bt_hh_callbacks, get_report_cb,
                    (RawAddress*)&(p_dev->link_spec.addrt.bda),
                    p_dev->link_spec.addrt.type, p_dev->link_spec.transport,
                    (bthh_status_t)p_data->hs_data.status, data, len);

          bta_hh_co_get_rpt_rsp(p_dev->dev_handle,
                                (tBTA_HH_STATUS)p_data->hs_data.status, data,
                                len);
        } else { /* Handshake */
          HAL_CBACK(bt_hh_callbacks, handshake_cb,
                    (RawAddress*)&(p_dev->link_spec.addrt.bda),
                    p_dev->link_spec.addrt.type, p_dev->link_spec.transport,
                    (bthh_status_t)p_data->hs_data.status);
        }
      } else {
        log::warn("Error: cannot find device with handle {}",
                  p_data->hs_data.handle);
      }
      break;
    }

    case BTA_HH_SET_RPT_EVT:
      log::verbose("BTA_HH_SET_RPT_EVT: status = {}, handle = {}",
                   p_data->dev_status.status, p_data->dev_status.handle);
      p_dev = btif_hh_find_connected_dev_by_handle(p_data->dev_status.handle);
      if (p_dev != NULL) {
        HAL_CBACK(bt_hh_callbacks, handshake_cb,
                  (RawAddress*)&(p_dev->link_spec.addrt.bda),
                  p_dev->link_spec.addrt.type, p_dev->link_spec.transport,
                  (bthh_status_t)p_data->hs_data.status);

        bta_hh_co_set_rpt_rsp(p_dev->dev_handle, p_data->dev_status.status);
      }
      break;

    case BTA_HH_GET_PROTO_EVT:
      p_dev = btif_hh_find_connected_dev_by_handle(p_data->hs_data.handle);
      if (p_dev == NULL) {
        log::warn("BTA_HH_GET_PROTO_EVT: cannot find device with handle {}",
                  p_data->hs_data.handle);
        return;
      }
      log::warn(
          "BTA_HH_GET_PROTO_EVT: status = {}, handle = {}, proto = [{}], {}",
          p_data->hs_data.status, p_data->hs_data.handle,
          p_data->hs_data.rsp_data.proto_mode,
          (p_data->hs_data.rsp_data.proto_mode == BTA_HH_PROTO_RPT_MODE)
              ? "Report Mode"
          : (p_data->hs_data.rsp_data.proto_mode == BTA_HH_PROTO_BOOT_MODE)
              ? "Boot Mode"
              : "Unsupported");
      if (p_data->hs_data.rsp_data.proto_mode != BTA_HH_PROTO_UNKNOWN) {
        HAL_CBACK(bt_hh_callbacks, protocol_mode_cb,
                  (RawAddress*)&(p_dev->link_spec.addrt.bda),
                  p_dev->link_spec.addrt.type, p_dev->link_spec.transport,
                  (bthh_status_t)p_data->hs_data.status,
                  (bthh_protocol_mode_t)p_data->hs_data.rsp_data.proto_mode);
      } else {
        HAL_CBACK(bt_hh_callbacks, handshake_cb,
                  (RawAddress*)&(p_dev->link_spec.addrt.bda),
                  p_dev->link_spec.addrt.type, p_dev->link_spec.transport,
                  (bthh_status_t)p_data->hs_data.status);
      }
      break;

    case BTA_HH_SET_PROTO_EVT:
      log::verbose("BTA_HH_SET_PROTO_EVT: status = {}, handle = {}",
                   p_data->dev_status.status, p_data->dev_status.handle);
      p_dev = btif_hh_find_connected_dev_by_handle(p_data->dev_status.handle);
      if (p_dev) {
        HAL_CBACK(bt_hh_callbacks, handshake_cb,
                  (RawAddress*)&(p_dev->link_spec.addrt.bda),
                  p_dev->link_spec.addrt.type, p_dev->link_spec.transport,
                  (bthh_status_t)p_data->hs_data.status);
      }
      break;

    case BTA_HH_GET_IDLE_EVT:
      log::verbose("BTA_HH_GET_IDLE_EVT: handle = {}, status = {}, rate = {}",
                   p_data->hs_data.handle, p_data->hs_data.status,
                   p_data->hs_data.rsp_data.idle_rate);
      p_dev = btif_hh_find_connected_dev_by_handle(p_data->hs_data.handle);
      if (p_dev) {
        HAL_CBACK(bt_hh_callbacks, idle_time_cb,
                  (RawAddress*)&(p_dev->link_spec.addrt.bda),
                  p_dev->link_spec.addrt.type, p_dev->link_spec.transport,
                  (bthh_status_t)p_data->hs_data.status,
                  p_data->hs_data.rsp_data.idle_rate);
      }
      break;

    case BTA_HH_SET_IDLE_EVT:
      log::verbose("BTA_HH_SET_IDLE_EVT: status = {}, handle = {}",
                   p_data->dev_status.status, p_data->dev_status.handle);
      break;

    case BTA_HH_GET_DSCP_EVT: {
      uint8_t hid_handle = p_data->dscp_info.hid_handle;
      int len = p_data->dscp_info.descriptor.dl_len;
      log::verbose("BTA_HH_GET_DSCP_EVT: len = {}, handle = {}", len,
                   hid_handle);

      p_dev = btif_hh_find_connected_dev_by_handle(hid_handle);
      if (p_dev == NULL) {
        log::error("BTA_HH_GET_DSCP_EVT: No HID device is currently connected");
        p_data->dscp_info.hid_handle = BTA_HH_INVALID_HANDLE;
        return;
      }

      if (p_dev->uhid.fd < 0) {
        log::error("BTA_HH_GET_DSCP_EVT: failed to find the uhid driver...");
        return;
      }

      const char* cached_name = NULL;
      bt_bdname_t bdname;
      bt_property_t prop_name;
      BTIF_STORAGE_FILL_PROPERTY(&prop_name, BT_PROPERTY_BDNAME,
                                 sizeof(bt_bdname_t), &bdname);
      if (btif_storage_get_remote_device_property(
              &p_dev->link_spec.addrt.bda, &prop_name) == BT_STATUS_SUCCESS) {
        cached_name = (char*)bdname.name;
      } else {
        cached_name = "Bluetooth HID";
      }

      log::warn("name = {}", cached_name);
      bta_hh_co_send_hid_info(p_dev, cached_name, p_data->dscp_info.vendor_id,
                              p_data->dscp_info.product_id,
                              p_data->dscp_info.version,
                              p_data->dscp_info.ctry_code, len,
                              p_data->dscp_info.descriptor.dsc_list);
      if (hh_add_device(p_dev->link_spec, p_dev->attr_mask, true)) {
        tBTA_HH_DEV_DSCP_INFO dscp_info;
        bt_status_t ret;
        btif_hh_copy_hid_info(&dscp_info, &p_data->dscp_info);
        log::verbose("BTA_HH_GET_DSCP_EVT: link spec = {}",
                     p_dev->link_spec.ToRedactedStringForLogging());
        BTA_HhAddDev(p_dev->link_spec, p_dev->attr_mask, p_dev->sub_class,
                     p_dev->app_id, dscp_info);
        // write hid info to nvram
        ret = btif_storage_add_hid_device_info(
            p_dev->link_spec, p_dev->attr_mask, p_dev->sub_class, p_dev->app_id,
            p_data->dscp_info.vendor_id, p_data->dscp_info.product_id,
            p_data->dscp_info.version, p_data->dscp_info.ctry_code,
            p_data->dscp_info.ssr_max_latency, p_data->dscp_info.ssr_min_tout,
            len, p_data->dscp_info.descriptor.dsc_list);

        // Allow incoming connections
        if (com::android::bluetooth::flags::allow_switching_hid_and_hogp() &&
            com::android::bluetooth::flags::
                save_initial_hid_connection_policy()) {
          btif_storage_set_hid_connection_policy(p_dev->link_spec, true);
        }

        ASSERTC(ret == BT_STATUS_SUCCESS, "storing hid info failed", ret);
        log::warn("BTA_HH_GET_DSCP_EVT: Called add device");

        // Free buffer created for dscp_info;
        if (dscp_info.descriptor.dl_len > 0 &&
            dscp_info.descriptor.dsc_list != NULL) {
          osi_free_and_reset((void**)&dscp_info.descriptor.dsc_list);
          dscp_info.descriptor.dl_len = 0;
        }
      } else {
        // Device already added.
        log::warn("BTA_HH_GET_DSCP_EVT: Device {} already added",
                  p_dev->link_spec.ToRedactedStringForLogging());
      }
      /*Sync HID Keyboard lockstates */
      int tmplen = sizeof(hid_kb_numlock_on_list) / sizeof(tHID_KB_LIST);
      for (int i = 0; i < tmplen; i++) {
        if (p_data->dscp_info.vendor_id ==
                hid_kb_numlock_on_list[i].version_id &&
            p_data->dscp_info.product_id ==
                hid_kb_numlock_on_list[i].product_id) {
          log::verbose("idx[{}] Enabling NUMLOCK for device :: {}", i,
                       hid_kb_numlock_on_list[i].kb_name);
          /* Enable NUMLOCK by default so that numeric
              keys work from first keyboard connect */
          set_keylockstate(BTIF_HH_KEYSTATE_MASK_NUMLOCK, true);
          sync_lockstate_on_connect(p_dev);
          /* End Sync HID Keyboard lockstates */
          break;
        }
      }
    } break;

    case BTA_HH_ADD_DEV_EVT: {
      log::info("BTA_HH_ADD_DEV_EVT: status = {}, handle = {}",
                p_data->dev_info.status, p_data->dev_info.handle);
      btif_hh_added_device_t* added_dev =
          btif_hh_find_added_dev(p_data->dev_info.link_spec);
      if (added_dev != nullptr) {
        if (p_data->dev_info.status == BTA_HH_OK) {
          added_dev->dev_handle = p_data->dev_info.handle;
        } else {
          added_dev->link_spec = {};
          added_dev->dev_handle = BTA_HH_INVALID_HANDLE;
        }
      }
    } break;
    case BTA_HH_RMV_DEV_EVT:
      log::verbose(
          "BTA_HH_RMV_DEV_EVT: status = {}, handle = {}, link spec = {}",
          p_data->dev_info.status, p_data->dev_info.handle,
          p_data->dev_info.link_spec.ToRedactedStringForLogging());
      break;

    case BTA_HH_VC_UNPLUG_EVT:
      log::verbose("BTA_HH_VC_UNPLUG_EVT: status = {}, handle = {}",
                   p_data->dev_status.status, p_data->dev_status.handle);
      p_dev = btif_hh_find_connected_dev_by_handle(p_data->dev_status.handle);

      if (p_dev == NULL) {
        log::error("BTA_HH_VC_UNPLUG_EVT: device not found handle {}",
                   p_data->dev_status.handle);
        return;
      }

      if (p_dev->link_spec.transport == BT_TRANSPORT_LE) {
        log::error("BTA_HH_VC_UNPLUG_EVT: not expected for {}",
                   p_dev->link_spec.ToRedactedStringForLogging());
        return;
      }
      btif_hh_cb.status = (BTIF_HH_STATUS)BTIF_HH_DEV_DISCONNECTED;
      log::verbose("BTA_HH_VC_UNPLUG_EVT: link_spec = {}",
                   p_dev->link_spec.ToRedactedStringForLogging());

      /* Stop the VUP timer */
      btif_hh_stop_vup_timer(p_dev->link_spec);
      p_dev->dev_status = hh_get_state_on_disconnect(p_dev->link_spec);
      log::verbose("--Sending connection state change");
      BTHH_STATE_UPDATE(p_dev->link_spec, p_dev->dev_status);
      log::verbose("--Removing HID bond");
      /* If it is locally initiated VUP or remote device has its major COD as
      Peripheral removed the bond.*/
      if (p_dev->local_vup || check_cod_hid(&(p_dev->link_spec.addrt.bda))) {
        p_dev->local_vup = false;
        BTA_DmRemoveDevice(p_dev->link_spec.addrt.bda);
      } else {
        log_counter_metrics_btif(
            android::bluetooth::CodePathCounterKeyEnum::
                HIDH_COUNT_VIRTUAL_UNPLUG_REQUESTED_BY_REMOTE_DEVICE,
            1);
        btif_hh_remove_device(p_dev->link_spec);
      }
      HAL_CBACK(bt_hh_callbacks, virtual_unplug_cb,
                &(p_dev->link_spec.addrt.bda), p_dev->link_spec.addrt.type,
                p_dev->link_spec.transport,
                (bthh_status_t)p_data->dev_status.status);
      break;

    case BTA_HH_API_ERR_EVT:
      log::error("BTA_HH API_ERR");
      break;

    default:
      log::warn("Unhandled event: {}", event);
      break;
  }
}

/*******************************************************************************
 *
 * Function         btif_hh_hsdata_rpt_copy_cb
 *
 * Description      Deep copies the tBTA_HH_HSDATA structure
 *
 * Returns          void
 *
 ******************************************************************************/

static void btif_hh_hsdata_rpt_copy_cb(uint16_t event, char* p_dest,
                                       const char* p_src) {
  tBTA_HH_HSDATA* p_dst_data = (tBTA_HH_HSDATA*)p_dest;
  tBTA_HH_HSDATA* p_src_data = (tBTA_HH_HSDATA*)p_src;
  BT_HDR* hdr;

  if (!p_src) {
    log::error("Nothing to copy");
    return;
  }

  memcpy(p_dst_data, p_src_data, sizeof(tBTA_HH_HSDATA));

  hdr = p_src_data->rsp_data.p_rpt_data;
  if (hdr != NULL) {
    uint8_t* p_data = ((uint8_t*)p_dst_data) + sizeof(tBTA_HH_HSDATA);
    memcpy(p_data, hdr, BT_HDR_SIZE + hdr->offset + hdr->len);

    p_dst_data->rsp_data.p_rpt_data = (BT_HDR*)p_data;
  }
}

/*******************************************************************************
 *
 * Function         bte_hh_evt
 *
 * Description      Switches context from BTE to BTIF for all HH events
 *
 * Returns          void
 *
 ******************************************************************************/

static void bte_hh_evt(tBTA_HH_EVT event, tBTA_HH* p_data) {
  bt_status_t status;
  int param_len = 0;
  tBTIF_COPY_CBACK* p_copy_cback = NULL;

  if (BTA_HH_ENABLE_EVT == event)
    param_len = sizeof(tBTA_HH_STATUS);
  else if (BTA_HH_OPEN_EVT == event)
    param_len = sizeof(tBTA_HH_CONN);
  else if (BTA_HH_DISABLE_EVT == event)
    param_len = sizeof(tBTA_HH_STATUS);
  else if (BTA_HH_CLOSE_EVT == event)
    param_len = sizeof(tBTA_HH_CBDATA);
  else if (BTA_HH_GET_DSCP_EVT == event)
    param_len = sizeof(tBTA_HH_DEV_DSCP_INFO);
  else if ((BTA_HH_GET_PROTO_EVT == event) || (BTA_HH_GET_IDLE_EVT == event))
    param_len = sizeof(tBTA_HH_HSDATA);
  else if (BTA_HH_GET_RPT_EVT == event) {
    BT_HDR* hdr = p_data->hs_data.rsp_data.p_rpt_data;
    param_len = sizeof(tBTA_HH_HSDATA);

    if (hdr != NULL) {
      p_copy_cback = btif_hh_hsdata_rpt_copy_cb;
      param_len += BT_HDR_SIZE + hdr->offset + hdr->len;
    }
  } else if ((BTA_HH_SET_PROTO_EVT == event) || (BTA_HH_SET_RPT_EVT == event) ||
             (BTA_HH_VC_UNPLUG_EVT == event) || (BTA_HH_SET_IDLE_EVT == event))
    param_len = sizeof(tBTA_HH_CBDATA);
  else if ((BTA_HH_ADD_DEV_EVT == event) || (BTA_HH_RMV_DEV_EVT == event))
    param_len = sizeof(tBTA_HH_DEV_INFO);
  else if (BTA_HH_API_ERR_EVT == event)
    param_len = 0;
  /* switch context to btif task context (copy full union size for convenience)
   */
  status = btif_transfer_context(btif_hh_upstreams_evt, (uint16_t)event,
                                 (char*)p_data, param_len, p_copy_cback);

  /* catch any failed context transfers */
  ASSERTC(status == BT_STATUS_SUCCESS, "context transfer failed", status);
}

/*******************************************************************************
 *
 * Function         btif_hh_handle_evt
 *
 * Description      Switches context for immediate callback
 *
 * Returns          void
 *
 ******************************************************************************/

static void btif_hh_handle_evt(uint16_t event, char* p_param) {
  log::assert_that(p_param != nullptr, "assert failed: p_param != nullptr");
  tAclLinkSpec link_spec = *(tAclLinkSpec*)p_param;

  switch (event) {
    case BTIF_HH_CONNECT_REQ_EVT: {
      log::debug("BTIF_HH_CONNECT_REQ_EVT: link spec:{}",
                 link_spec.ToRedactedStringForLogging());
      if (btif_hh_connect(link_spec) == BT_STATUS_SUCCESS) {
        BTHH_STATE_UPDATE(link_spec, BTHH_CONN_STATE_CONNECTING);
      } else {
        BTHH_STATE_UPDATE(link_spec, BTHH_CONN_STATE_DISCONNECTED);
      }
    } break;

    case BTIF_HH_DISCONNECT_REQ_EVT: {
      log::debug("BTIF_HH_DISCONNECT_REQ_EVT: link spec:{}",
                 link_spec.ToRedactedStringForLogging());
      btif_hh_disconnect(link_spec);
      BTHH_STATE_UPDATE(link_spec, BTHH_CONN_STATE_DISCONNECTING);
    } break;

    case BTIF_HH_VUP_REQ_EVT: {
      log::debug("BTIF_HH_VUP_REQ_EVT: link spec:{}",
                 link_spec.ToRedactedStringForLogging());
      if (btif_hh_virtual_unplug(link_spec) != BT_STATUS_SUCCESS) {
        log::warn("Unable to virtual unplug device remote:{}",
                  link_spec.ToRedactedStringForLogging());
      }
    } break;

    default: {
      log::warn("Unknown event received:{} remote:{}", event,
                link_spec.ToRedactedStringForLogging());
    } break;
  }
}

/*******************************************************************************
 *
 * Function      btif_hh_timer_timeout
 *
 * Description   Process timer timeout
 *
 * Returns      void
 ******************************************************************************/
void btif_hh_timer_timeout(void* data) {
  btif_hh_device_t* p_dev = (btif_hh_device_t*)data;
  tBTA_HH_EVT event = BTA_HH_VC_UNPLUG_EVT;
  tBTA_HH p_data;
  int param_len = sizeof(tBTA_HH_CBDATA);

  log::verbose("");
  if (p_dev->dev_status != BTHH_CONN_STATE_CONNECTED) return;

  memset(&p_data, 0, sizeof(tBTA_HH));
  p_data.dev_status.status = BTA_HH_ERR;  // tBTA_HH_STATUS
  p_data.dev_status.handle = p_dev->dev_handle;

  /* switch context to btif task context */
  btif_transfer_context(btif_hh_upstreams_evt, (uint16_t)event, (char*)&p_data,
                        param_len, NULL);
}

/*******************************************************************************
 *
 * Function         btif_hh_init
 *
 * Description     initializes the hh interface
 *
 * Returns         bt_status_t
 *
 ******************************************************************************/
static bt_status_t init(bthh_callbacks_t* callbacks) {
  uint32_t i;
  log::verbose("");

  bt_hh_callbacks = callbacks;
  memset(&btif_hh_cb, 0, sizeof(btif_hh_cb));
  for (i = 0; i < BTIF_HH_MAX_HID; i++) {
    btif_hh_cb.devices[i].dev_status = BTHH_CONN_STATE_UNKNOWN;
  }
  /* Invoke the enable service API to the core to set the appropriate service_id
   */
  btif_enable_service(BTA_HID_SERVICE_ID);
  return BT_STATUS_SUCCESS;
}
/*******************************************************************************
 *
 * Function         btif_hh_transport_select
 *
 * Description      Select HID transport based on services available.
 *
 * Returns          void
 *
 ******************************************************************************/
static void btif_hh_transport_select(tAclLinkSpec& link_spec) {
  bool hid_available = false;
  bool hogp_available = false;
  bool headtracker_available = false;
  bool le_preferred = false;
  bluetooth::Uuid remote_uuids[BT_MAX_NUM_UUIDS] = {};
  bt_property_t remote_properties = {BT_PROPERTY_UUIDS, sizeof(remote_uuids),
                                     &remote_uuids};
  const RawAddress& bd_addr = link_spec.addrt.bda;

  // Find the device type
  tBT_DEVICE_TYPE dev_type;
  tBLE_ADDR_TYPE addr_type;
  BTM_ReadDevInfo(bd_addr, &dev_type, &addr_type);

  // Find which transports are already connected
  bool bredr_acl = BTM_IsAclConnectionUp(bd_addr, BT_TRANSPORT_BR_EDR);
  bool le_acl = BTM_IsAclConnectionUp(bd_addr, BT_TRANSPORT_LE);

  // Find which services known to be available
  if (btif_storage_get_remote_device_property(&bd_addr, &remote_properties) ==
      BT_STATUS_SUCCESS) {
    int count = remote_properties.len / sizeof(remote_uuids[0]);
    for (int i = 0; i < count; i++) {
      if (remote_uuids[i].Is16Bit()) {
        if (remote_uuids[i].As16Bit() == UUID_SERVCLASS_HUMAN_INTERFACE) {
          hid_available = true;
        } else if (remote_uuids[i].As16Bit() == UUID_SERVCLASS_LE_HID) {
          hogp_available = true;
        }
      } else if (com::android::bluetooth::flags::
                     android_headtracker_service() &&
                 remote_uuids[i] == ANDROID_HEADTRACKER_SERVICE_UUID) {
        headtracker_available = true;
      }

      if (hid_available && (hogp_available || headtracker_available)) {
        break;
      }
    }
  }

  /* Decide whether to connect HID or HOGP */
  if (bredr_acl && hid_available) {
    le_preferred = false;
  } else if (le_acl && (hogp_available || headtracker_available)) {
    le_preferred = true;
  } else if (hid_available) {
    le_preferred = false;
  } else if (hogp_available || headtracker_available) {
    le_preferred = true;
  } else if (bredr_acl) {
    le_preferred = false;
  } else if (le_acl || dev_type == BT_DEVICE_TYPE_BLE) {
    le_preferred = true;
  } else {
    le_preferred = false;
  }

  link_spec.transport = le_preferred ? BT_TRANSPORT_LE : BT_TRANSPORT_BR_EDR;
  log::info(
      "link_spec:{}, bredr_acl:{}, hid_available:{}, le_acl:{}, "
      "hogp_available:{}, headtracker_available:{}, "
      "dev_type:{}, le_preferred:{}",
      link_spec.ToRedactedStringForLogging(), bredr_acl, hid_available, le_acl,
      hogp_available, headtracker_available, dev_type, le_preferred);
}
/*******************************************************************************
 *
 * Function        connect
 *
 * Description     connect to hid device
 *
 * Returns         bt_status_t
 *
 ******************************************************************************/
static bt_status_t connect(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                           tBT_TRANSPORT transport) {
  tAclLinkSpec link_spec = {};
  link_spec.addrt.bda = *bd_addr;
  link_spec.addrt.type = addr_type;
  link_spec.transport = transport;

  BTHH_LOG_LINK(link_spec);

  if (btif_hh_cb.status == BTIF_HH_DEV_CONNECTING) {
    log::warn("HH status = {}", btif_hh_status_text(btif_hh_cb.status));
    return BT_STATUS_BUSY;
  } else if (btif_hh_cb.status == BTIF_HH_DISABLED ||
             btif_hh_cb.status == BTIF_HH_DISABLING) {
    log::warn("HH status = {}", btif_hh_status_text(btif_hh_cb.status));
    return BT_STATUS_NOT_READY;
  }

  btif_hh_device_t* p_dev = btif_hh_find_connected_dev_by_link_spec(link_spec);
  if (p_dev != nullptr) {
    log::warn("device {} already connected",
              p_dev->link_spec.ToRedactedStringForLogging());
    return BT_STATUS_DONE;
  }

  if (link_spec.transport == BT_TRANSPORT_AUTO) {
    btif_hh_transport_select(link_spec);
  }

  return btif_transfer_context(btif_hh_handle_evt, BTIF_HH_CONNECT_REQ_EVT,
                               (char*)&link_spec, sizeof(tAclLinkSpec), NULL);
}

/*******************************************************************************
 *
 * Function         disconnect
 *
 * Description      disconnect from hid device
 *
 * Returns         bt_status_t
 *
 ******************************************************************************/
static bt_status_t disconnect(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                              tBT_TRANSPORT transport, bool reconnect_allowed) {
  CHECK_BTHH_INIT();
  tAclLinkSpec link_spec = {};
  link_spec.addrt.bda = *bd_addr;
  link_spec.addrt.type = addr_type;
  link_spec.transport = transport;

  BTHH_LOG_LINK(link_spec);

  if (btif_hh_cb.status == BTIF_HH_DISABLED ||
      btif_hh_cb.status == BTIF_HH_DISABLING) {
    log::error("HH status = {}", btif_hh_status_text(btif_hh_cb.status));
    return BT_STATUS_UNHANDLED;
  }

  if (com::android::bluetooth::flags::allow_switching_hid_and_hogp() &&
      !reconnect_allowed) {
    log::info("Incoming reconnections disabled for device {}",
              link_spec.ToRedactedStringForLogging());
    btif_hh_added_device_t* added_dev = btif_hh_find_added_dev(link_spec);
    if (added_dev != nullptr) {
      added_dev->reconnect_allowed = reconnect_allowed;
      btif_storage_set_hid_connection_policy(added_dev->link_spec,
                                             reconnect_allowed);
    }
  }

  btif_hh_device_t* p_dev = btif_hh_find_connected_dev_by_link_spec(link_spec);
  if (p_dev == nullptr) {
    if (com::android::bluetooth::flags::allow_switching_hid_and_hogp()) {
      // Conclude the request if the device is already disconnected
      p_dev = btif_hh_find_dev_by_link_spec(link_spec);
      if (p_dev != nullptr &&
          (p_dev->dev_status == BTHH_CONN_STATE_ACCEPTING ||
           p_dev->dev_status == BTHH_CONN_STATE_CONNECTING)) {
        log::warn("Device {} already not connected, state: {}",
                  p_dev->link_spec.ToRedactedStringForLogging(),
                  bthh_connection_state_text(p_dev->dev_status));
        p_dev->dev_status = BTHH_CONN_STATE_DISCONNECTED;
        return BT_STATUS_DONE;
      }
    }

    BTHH_LOG_UNKNOWN_LINK(link_spec);
    return BT_STATUS_UNHANDLED;
  }

  return btif_transfer_context(btif_hh_handle_evt, BTIF_HH_DISCONNECT_REQ_EVT,
                               (char*)&p_dev->link_spec, sizeof(tAclLinkSpec),
                               NULL);
}

/*******************************************************************************
 *
 * Function         virtual_unplug
 *
 * Description      Virtual UnPlug (VUP) the specified HID device.
 *
 * Returns         bt_status_t
 *
 ******************************************************************************/
static bt_status_t virtual_unplug(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                                  tBT_TRANSPORT transport) {
  CHECK_BTHH_INIT();
  tAclLinkSpec link_spec = {};
  link_spec.addrt.bda = *bd_addr;
  link_spec.addrt.type = addr_type;
  link_spec.transport = transport;

  BTHH_LOG_LINK(link_spec);

  BTHH_CHECK_NOT_DISABLED();

  btif_hh_device_t* p_dev = btif_hh_find_dev_by_link_spec(link_spec);
  if (!p_dev) {
    BTHH_LOG_UNKNOWN_LINK(link_spec);
    return BT_STATUS_DEVICE_NOT_FOUND;
  }
  btif_transfer_context(btif_hh_handle_evt, BTIF_HH_VUP_REQ_EVT,
                        (char*)&link_spec, sizeof(tAclLinkSpec), NULL);
  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
**
** Function         get_idle_time
**
** Description      Get the HID idle time
**
** Returns         bt_status_t
**
*******************************************************************************/
static bt_status_t get_idle_time(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                                 tBT_TRANSPORT transport) {
  CHECK_BTHH_INIT();
  tAclLinkSpec link_spec = {};
  link_spec.addrt.bda = *bd_addr;
  link_spec.addrt.type = addr_type;
  link_spec.transport = transport;

  BTHH_LOG_LINK(link_spec);

  BTHH_CHECK_NOT_DISABLED();

  btif_hh_device_t* p_dev = btif_hh_find_connected_dev_by_link_spec(link_spec);
  if (p_dev == NULL) {
    BTHH_LOG_UNKNOWN_LINK(link_spec);
    return BT_STATUS_DEVICE_NOT_FOUND;
  }

  BTA_HhGetIdle(p_dev->dev_handle);
  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
**
** Function         set_idle_time
**
** Description      Set the HID idle time
**
** Returns         bt_status_t
**
*******************************************************************************/
static bt_status_t set_idle_time(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                                 tBT_TRANSPORT transport, uint8_t idle_time) {
  CHECK_BTHH_INIT();
  tAclLinkSpec link_spec = {};
  link_spec.addrt.bda = *bd_addr;
  link_spec.addrt.type = addr_type;
  link_spec.transport = transport;

  BTHH_LOG_LINK(link_spec);
  log::verbose("idle time: {}", idle_time);

  BTHH_CHECK_NOT_DISABLED();

  btif_hh_device_t* p_dev = btif_hh_find_connected_dev_by_link_spec(link_spec);
  if (p_dev == NULL) {
    BTHH_LOG_UNKNOWN_LINK(link_spec);
    return BT_STATUS_DEVICE_NOT_FOUND;
  }

  BTA_HhSetIdle(p_dev->dev_handle, idle_time);
  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         set_info
 *
 * Description      Set the HID device descriptor for the specified HID device.
 *
 * Returns         bt_status_t
 *
 ******************************************************************************/
static bt_status_t set_info(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                            tBT_TRANSPORT transport, bthh_hid_info_t hid_info) {
  CHECK_BTHH_INIT();
  tBTA_HH_DEV_DSCP_INFO dscp_info = {};
  tAclLinkSpec link_spec = {};
  link_spec.addrt.bda = *bd_addr;
  link_spec.addrt.type = addr_type;
  link_spec.transport = transport;

  BTHH_LOG_LINK(link_spec);
  log::verbose(
      "sub_class = 0x{:02x}, app_id = {}, vendor_id = 0x{:04x}, "
      "product_id = 0x{:04x}, version= 0x{:04x}",
      hid_info.sub_class, hid_info.app_id, hid_info.vendor_id,
      hid_info.product_id, hid_info.version);

  BTHH_CHECK_NOT_DISABLED();

  dscp_info.vendor_id = hid_info.vendor_id;
  dscp_info.product_id = hid_info.product_id;
  dscp_info.version = hid_info.version;
  dscp_info.ctry_code = hid_info.ctry_code;

  dscp_info.descriptor.dl_len = hid_info.dl_len;
  dscp_info.descriptor.dsc_list =
      (uint8_t*)osi_malloc(dscp_info.descriptor.dl_len);
  memcpy(dscp_info.descriptor.dsc_list, &(hid_info.dsc_list), hid_info.dl_len);

  if (transport == BT_TRANSPORT_AUTO) {
    btif_hh_transport_select(link_spec);
  }

  if (hh_add_device(link_spec, hid_info.attr_mask, true)) {
    BTA_HhAddDev(link_spec, hid_info.attr_mask, hid_info.sub_class,
                 hid_info.app_id, dscp_info);
  }

  osi_free_and_reset((void**)&dscp_info.descriptor.dsc_list);

  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         get_protocol
 *
 * Description      Get the HID proto mode.
 *
 * Returns         bt_status_t
 *
 ******************************************************************************/
static bt_status_t get_protocol(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                                tBT_TRANSPORT transport,
                                bthh_protocol_mode_t /* protocolMode */) {
  CHECK_BTHH_INIT();
  tAclLinkSpec link_spec = {};
  link_spec.addrt.bda = *bd_addr;
  link_spec.addrt.type = addr_type;
  link_spec.transport = transport;

  BTHH_LOG_LINK(link_spec);

  BTHH_CHECK_NOT_DISABLED();

  btif_hh_device_t* p_dev = btif_hh_find_connected_dev_by_link_spec(link_spec);
  if (!p_dev) {
    BTHH_LOG_UNKNOWN_LINK(link_spec);
    return BT_STATUS_DEVICE_NOT_FOUND;
  }

  BTA_HhGetProtoMode(p_dev->dev_handle);
  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         set_protocol
 *
 * Description      Set the HID proto mode.
 *
 * Returns         bt_status_t
 *
 ******************************************************************************/
static bt_status_t set_protocol(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                                tBT_TRANSPORT transport,
                                bthh_protocol_mode_t protocolMode) {
  CHECK_BTHH_INIT();
  btif_hh_device_t* p_dev;
  uint8_t proto_mode = protocolMode;
  tAclLinkSpec link_spec = {};
  link_spec.addrt.bda = *bd_addr;
  link_spec.addrt.type = addr_type;
  link_spec.transport = transport;

  BTHH_LOG_LINK(link_spec);
  log::verbose("mode: {}", protocolMode);

  BTHH_CHECK_NOT_DISABLED();

  p_dev = btif_hh_find_connected_dev_by_link_spec(link_spec);
  if (p_dev == NULL) {
    BTHH_LOG_UNKNOWN_LINK(link_spec);
    return BT_STATUS_DEVICE_NOT_FOUND;
  } else if (protocolMode != BTA_HH_PROTO_RPT_MODE &&
             protocolMode != BTA_HH_PROTO_BOOT_MODE) {
    log::warn("device proto_mode = {}", proto_mode);
    return BT_STATUS_PARM_INVALID;
  } else {
    BTA_HhSetProtoMode(p_dev->dev_handle, protocolMode);
  }

  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         get_report
 *
 * Description      Send a GET_REPORT to HID device.
 *
 * Returns         bt_status_t
 *
 ******************************************************************************/
static bt_status_t get_report(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                              tBT_TRANSPORT transport,
                              bthh_report_type_t reportType, uint8_t reportId,
                              int bufferSize) {
  CHECK_BTHH_INIT();
  btif_hh_device_t* p_dev;
  tAclLinkSpec link_spec = {};
  link_spec.addrt.bda = *bd_addr;
  link_spec.addrt.type = addr_type;
  link_spec.transport = transport;

  BTHH_LOG_LINK(link_spec);
  log::verbose("r_type: {}; rpt_id: {}; buf_size: {}", reportType, reportId,
               bufferSize);

  BTHH_CHECK_NOT_DISABLED();

  p_dev = btif_hh_find_connected_dev_by_link_spec(link_spec);
  if (p_dev == NULL) {
    BTHH_LOG_UNKNOWN_LINK(link_spec);
    return BT_STATUS_DEVICE_NOT_FOUND;
  } else if (((int)reportType) <= BTA_HH_RPTT_RESRV ||
             ((int)reportType) > BTA_HH_RPTT_FEATURE) {
    log::error("report type={} not supported", reportType);
    log_counter_metrics_btif(android::bluetooth::CodePathCounterKeyEnum::
                                 HIDH_COUNT_WRONG_REPORT_TYPE,
                             1);
    return BT_STATUS_UNSUPPORTED;
  } else {
    BTA_HhGetReport(p_dev->dev_handle, reportType, reportId, bufferSize);
  }

  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         get_report_reply
 *
 * Description      Send a REPORT_REPLY/FEATURE_ANSWER to HID driver.
 *
 * Returns         bt_status_t
 *
 ******************************************************************************/
static bt_status_t get_report_reply(RawAddress* bd_addr,
                                    tBLE_ADDR_TYPE addr_type,
                                    tBT_TRANSPORT transport,
                                    bthh_status_t status, char* report,
                                    uint16_t size) {
  CHECK_BTHH_INIT();
  tAclLinkSpec link_spec = {};
  link_spec.addrt.bda = *bd_addr;
  link_spec.addrt.type = addr_type;
  link_spec.transport = transport;

  BTHH_LOG_LINK(link_spec);

  BTHH_CHECK_NOT_DISABLED();

  btif_hh_device_t* p_dev = btif_hh_find_connected_dev_by_link_spec(link_spec);
  if (p_dev == NULL) {
    BTHH_LOG_UNKNOWN_LINK(link_spec);
    return BT_STATUS_DEVICE_NOT_FOUND;
  }

  bta_hh_co_get_rpt_rsp(p_dev->dev_handle, (tBTA_HH_STATUS)status,
                        (uint8_t*)report, size);
  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         set_report
 *
 * Description      Send a SET_REPORT to HID device.
 *
 * Returns         bt_status_t
 *
 ******************************************************************************/
static bt_status_t set_report(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                              tBT_TRANSPORT transport,
                              bthh_report_type_t reportType, char* report) {
  CHECK_BTHH_INIT();
  btif_hh_device_t* p_dev;
  tAclLinkSpec link_spec = {};
  link_spec.addrt.bda = *bd_addr;
  link_spec.addrt.type = addr_type;
  link_spec.transport = transport;

  BTHH_LOG_LINK(link_spec);
  log::verbose("reportType: {}", reportType);

  BTHH_CHECK_NOT_DISABLED();

  p_dev = btif_hh_find_connected_dev_by_link_spec(link_spec);
  if (p_dev == NULL) {
    BTHH_LOG_UNKNOWN_LINK(link_spec);
    return BT_STATUS_DEVICE_NOT_FOUND;
  } else if (((int)reportType) <= BTA_HH_RPTT_RESRV ||
             ((int)reportType) > BTA_HH_RPTT_FEATURE) {
    log::error("report type={} not supported", reportType);
    log_counter_metrics_btif(android::bluetooth::CodePathCounterKeyEnum::
                                 HIDH_COUNT_WRONG_REPORT_TYPE,
                             1);
    return BT_STATUS_UNSUPPORTED;
  } else {
    int hex_bytes_filled;
    size_t len = (strlen(report) + 1) / 2;
    uint8_t* hexbuf = (uint8_t*)osi_calloc(len);

    /* Build a SetReport data buffer */
    // TODO
    hex_bytes_filled = ascii_2_hex(report, len, hexbuf);
    log::info("Hex bytes filled, hex value: {}", hex_bytes_filled);
    if (hex_bytes_filled) {
      BT_HDR* p_buf = create_pbuf(hex_bytes_filled, hexbuf);
      if (p_buf == NULL) {
        log::error("failed to allocate RPT buffer, len = {}", hex_bytes_filled);
        osi_free(hexbuf);
        return BT_STATUS_NOMEM;
      }
      BTA_HhSetReport(p_dev->dev_handle, reportType, p_buf);
      osi_free(hexbuf);
      return BT_STATUS_SUCCESS;
    }
    osi_free(hexbuf);
    return BT_STATUS_FAIL;
  }
}

/*******************************************************************************
 *
 * Function         send_data
 *
 * Description      Send a SEND_DATA to HID device.
 *
 * Returns         bt_status_t
 *
 ******************************************************************************/
static bt_status_t send_data(RawAddress* bd_addr, tBLE_ADDR_TYPE addr_type,
                             tBT_TRANSPORT transport, char* data) {
  CHECK_BTHH_INIT();
  tAclLinkSpec link_spec = {};
  link_spec.addrt.bda = *bd_addr;
  link_spec.addrt.type = addr_type;
  link_spec.transport = transport;

  BTHH_LOG_LINK(link_spec);

  BTHH_CHECK_NOT_DISABLED();

  btif_hh_device_t* p_dev = btif_hh_find_connected_dev_by_link_spec(link_spec);
  if (p_dev == NULL) {
    BTHH_LOG_UNKNOWN_LINK(link_spec);
    return BT_STATUS_DEVICE_NOT_FOUND;
  } else {
    int hex_bytes_filled;
    size_t len = (strlen(data) + 1) / 2;
    uint8_t* hexbuf = (uint8_t*)osi_calloc(len);

    /* Build a SendData data buffer */
    hex_bytes_filled = ascii_2_hex(data, len, hexbuf);
    log::info("Hex bytes filled, hex value: {}, {}", hex_bytes_filled, len);

    if (hex_bytes_filled) {
      BT_HDR* p_buf = create_pbuf(hex_bytes_filled, hexbuf);
      if (p_buf == NULL) {
        log::error("failed to allocate RPT buffer, len = {}", hex_bytes_filled);
        osi_free(hexbuf);
        return BT_STATUS_NOMEM;
      }
      p_buf->layer_specific = BTA_HH_RPTT_OUTPUT;
      BTA_HhSendData(p_dev->dev_handle, link_spec, p_buf);
      osi_free(hexbuf);
      return BT_STATUS_SUCCESS;
    }
    osi_free(hexbuf);
    return BT_STATUS_FAIL;
  }
}

/*******************************************************************************
 *
 * Function         cleanup
 *
 * Description      Closes the HH interface
 *
 * Returns          bt_status_t
 *
 ******************************************************************************/
static void cleanup(void) {
  log::verbose("");
  btif_hh_device_t* p_dev;
  int i;
  if (btif_hh_cb.status == BTIF_HH_DISABLED ||
      btif_hh_cb.status == BTIF_HH_DISABLING) {
    log::warn("HH disabling or disabled already, status = {}",
              btif_hh_status_text(btif_hh_cb.status));
    return;
  }
  if (bt_hh_callbacks) {
    btif_hh_cb.status = BTIF_HH_DISABLING;
    /* update flag, not to enable hid device service now as BT is switching off
     */
    btif_hh_cb.service_dereg_active = FALSE;
    btif_disable_service(BTA_HID_SERVICE_ID);
  }
  for (i = 0; i < BTIF_HH_MAX_HID; i++) {
    p_dev = &btif_hh_cb.devices[i];
    if (p_dev->dev_status != BTHH_CONN_STATE_UNKNOWN && p_dev->uhid.fd >= 0) {
      log::verbose("Closing uhid fd = {}", p_dev->uhid.fd);
      bta_hh_co_close(p_dev);
    }
  }
}

/*******************************************************************************
 *
 * Function         configure_enabled_profiles
 *
 * Description      Configure HIDP or HOGP enablement. Require to cleanup and
 *re-init to take effect.
 *
 * Returns          void
 *
 ******************************************************************************/
static void configure_enabled_profiles(bool enable_hidp, bool enable_hogp) {
  bt_hh_enable_type.hidp_enabled = enable_hidp;
  bt_hh_enable_type.hogp_enabled = enable_hogp;
}

static const bthh_interface_t bthhInterface = {
    sizeof(bthhInterface),
    init,
    connect,
    disconnect,
    virtual_unplug,
    set_info,
    get_protocol,
    set_protocol,
    get_idle_time,
    set_idle_time,
    get_report,
    get_report_reply,
    set_report,
    send_data,
    cleanup,
    configure_enabled_profiles,
};

/*******************************************************************************
 *
 * Function         btif_hh_execute_service
 *
 * Description      Initializes/Shuts down the service
 *
 * Returns          BT_STATUS_SUCCESS on success, BT_STATUS_FAIL otherwise
 *
 ******************************************************************************/
bt_status_t btif_hh_execute_service(bool b_enable) {
  if (b_enable) {
    /* Enable and register with BTA-HH */
    BTA_HhEnable(bte_hh_evt, bt_hh_enable_type.hidp_enabled,
                 bt_hh_enable_type.hogp_enabled);
  } else {
    /* Disable HH */
    BTA_HhDisable();
  }
  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
 *
 * Function         btif_hh_get_interface
 *
 * Description      Get the hh callback interface
 *
 * Returns          bthh_interface_t
 *
 ******************************************************************************/
const bthh_interface_t* btif_hh_get_interface() {
  log::verbose("");
  return &bthhInterface;
}

#define DUMPSYS_TAG "shim::legacy::hid"
void DumpsysHid(int fd) {
  LOG_DUMPSYS_TITLE(fd, DUMPSYS_TAG);
  LOG_DUMPSYS(fd, "status:%s num_devices:%u",
              btif_hh_status_text(btif_hh_cb.status).c_str(),
              btif_hh_cb.device_num);
  LOG_DUMPSYS(fd, "status:%s", btif_hh_status_text(btif_hh_cb.status).c_str());
  for (unsigned i = 0; i < BTIF_HH_MAX_HID; i++) {
    const btif_hh_device_t* p_dev = &btif_hh_cb.devices[i];
    if (p_dev->link_spec.addrt.bda != RawAddress::kEmpty) {
      LOG_DUMPSYS(
          fd, "  %u: addr:%s fd:%d state:%s ready:%s thread_id:%d handle:%d", i,
          p_dev->link_spec.ToRedactedStringForLogging().c_str(), p_dev->uhid.fd,
          bthh_connection_state_text(p_dev->dev_status).c_str(),
          (p_dev->uhid.ready_for_data) ? ("T") : ("F"),
          static_cast<int>(p_dev->hh_poll_thread_id), p_dev->dev_handle);
    }
  }
  for (unsigned i = 0; i < BTIF_HH_MAX_ADDED_DEV; i++) {
    const btif_hh_added_device_t* p_dev = &btif_hh_cb.added_devices[i];
    if (p_dev->link_spec.addrt.bda != RawAddress::kEmpty) {
      LOG_DUMPSYS(fd, "  %u: addr:%s reconnect:%s", i,
                  p_dev->link_spec.ToRedactedStringForLogging().c_str(),
                  p_dev->reconnect_allowed ? "T" : "F");
    }
  }
}

namespace bluetooth {
namespace legacy {
namespace testing {

void bte_hh_evt(tBTA_HH_EVT event, tBTA_HH* p_data) {
  ::bte_hh_evt(event, p_data);
}

}  // namespace testing
}  // namespace legacy
}  // namespace bluetooth

#undef DUMPSYS_TAG
