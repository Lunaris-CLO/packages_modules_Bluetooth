/******************************************************************************
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 ******************************************************************************/

/******************************************************************************
 *
 * This file contains functions that handle BTM Vendor interface functions for
 * the Bluetooth Add ON feature, etc.
 *
 ******************************************************************************/

#include <base/logging.h>
#include <bluetooth/log.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <include/hardware/bt_av.h>
#include "acl_api.h"
#include "btconfigstore/bt_configstore.h"
#include "btif/include/btif_config.h"
#include "btm_api.h"
#include "btm_int_types.h"
#include "os/log.h"
#include "stack/acl/acl.h"
#include "stack/acl/peer_packet_types.h"
#include "stack/include/bt_types.h"
#include "stack/include/btm_iso_api.h"
#include "stack/include/btm_vendor_api.h"
#include "stack/include/btm_vendor_types.h"

#define QHS_TRANSPORT_BREDR 0
#define QHS_TRANSPORT_LE 1
#define QHS_TRANSPORT_LE_ISO 2

/* Disable QHS */
#define QHS_HOST_MODE_HOST_DISABLE 0
/* Enable QHS support */
#define QHS_HOST_MODE_HOST_AWARE 3
/* Disable QHS, QLL and QLMP modes */
#define QHS_HOST_DISABLE_ALL 4

#define QHS_BREDR_MASK 0x01
#define QHS_LE_MASK 0x02
#define QHS_LE_ISO_MASK 0x04

#define QBCE_QLL_MULTI_CONFIG_CIS_PARAMETER_UPDATE_HOST_BIT 58

#define HCI_VS_SET_MAX_RADIATED_POWER_SUB_OPCODE 0x0E
#define BR_TECH_VALUE 0x00
#define EDR_TECH_VALUE 0x01
#define BLE_TECH_VALUE 0x02
#define BT_DEFAULT_POWER (0x80)

constexpr uint16_t  HCI_VS_QBCE_OCF = 0xFC51;
constexpr uint16_t  HCI_VS_GET_ADDON_FEATURES_SUPPORT = 0xFC1D;
constexpr uint16_t  HCI_VS_LINK_POWER_CTRL_REQ_OPCODE = 0xFCDA;

using namespace bluetooth;

typedef struct {
  const uint8_t as_array[8];
} bt_event_mask_t;

typedef struct {
  uint8_t BR_max_pow_support;
  uint8_t EDR_max_pow_support;
  uint8_t BLE_max_pow_support;
  bool BR_max_pow_feature = false;
  bool EDR_max_pow_feature = false;
  bool BLE_max_pow_feature = false;
} max_pow_feature_t;

const bt_event_mask_t QBCE_QLM_AND_QLL_EVENT_MASK = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x4A}};

// BT features related defines
static bt_soc_type_t soc_type = BT_SOC_TYPE_DEFAULT;
static char a2dp_offload_Cap[PROPERTY_VALUE_MAX] = {'\0'};
static bool spilt_a2dp_supported = true;
static bool aac_frame_ctl_enabled = false;
static bool max_power_prop_enabled = false;
static uint8_t max_power_prop_value[3];
static uint8_t scrambling_supported_freqs[MAX_SUPPORTED_SCRAMBLING_FREQ_SIZE];
static uint8_t number_of_scrambling_supported_freqs = 0;
static bt_device_soc_add_on_features_t soc_add_on_features;
static uint8_t soc_add_on_features_length = 0;
static uint16_t product_id, response_version;
static bt_device_host_add_on_features_t host_add_on_features;
static uint8_t host_add_on_features_length = 0;
char qhs_value[PROPERTY_VALUE_MAX] = "0";
uint8_t qhs_support_mask = 0;
static bt_device_qll_local_supported_features_t qll_features;
static uint8_t qll_local_supported_features_length = 0;
static bt_configstore_interface_t* bt_configstore_intf = NULL;
static bool is_power_backoff_enabled = false;

extern tBTM_CB btm_cb;

void BTM_ConfigQHS();
static void btm_vendor_set_tech_based_max_power(bool status);

static inline bool is_byte_valid(char ch) {
  return ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
          (ch >= 'A' && ch <= 'F'));
}

bool decode_max_power_values(char* power_val) {
  bool status = false;
  char *token, *reset = power_val;
  int i;

  if (!strcmp(power_val, "false")) {
    log::info(": MAX POW property is not set");
    return false;
  } else if (!strchr(power_val, '-') ||
             (!strchr(power_val, 'x') && !strchr(power_val, 'X')) ||
             strlen(power_val) != 14) {
    log::warn(": MAX POW property is not in required order");
    return false;
  } else {
    status = true;
    for (i = 0; (i < 3 && status); i++) {
      token = strtok_r(reset, "-", &reset);
      if (token && strlen(token) == 4 &&
          (token[0] == '0' && (token[1] == 'x' || token[1] == 'X') &&
           (is_byte_valid(token[2]) && is_byte_valid(token[3])))) {
        max_power_prop_value[i] = (uint8_t)strtoul(token, NULL, 16);
      } else {
        status = false;
      }
    }
  }

  if (status) {
    log::debug(": MAX_POW_ID: BR MAX POW:{:02x}, EDR MAX POW:{:02x}, BLE MAX POW:{:02x}", max_power_prop_value[0], max_power_prop_value[1], max_power_prop_value[2]);
    max_power_prop_enabled = true;
  } else {
    log::error(": MAX POW property is not in required order");
  }

  return status;
}

char* BTM_GetA2dpOffloadCapablity() { return &a2dp_offload_Cap[0]; }

bool BTM_IsSpiltA2dpSupported() { return spilt_a2dp_supported; }

bool BTM_IsAACFrameCtrlEnabled() { return aac_frame_ctl_enabled; }

uint8_t* BTM_GetScramblingSupportedFreqs(uint8_t* number_of_freqs) {
  if (number_of_scrambling_supported_freqs) {
    *number_of_freqs = number_of_scrambling_supported_freqs;
    return scrambling_supported_freqs;
  }
  return NULL;
}

/*******************************************************************************
 *
 * Function         BTM_GetHostAddOnFeatures
 *
 * Description      BTM_GetHostAddOnFeatures
 *
 *
 * Returns          host add on features array
 *
 ******************************************************************************/
bt_device_host_add_on_features_t* BTM_GetHostAddOnFeatures(
    uint8_t* host_add_on_features_len) {
  *host_add_on_features_len = host_add_on_features_length;
  return &host_add_on_features;
}

/*******************************************************************************
 *
 * Function         BTM_GetSocAddOnFeatures
 *
 * Description      BTM_GetSocAddOnFeatures
 *
 *
 * Returns          soc add on features array
 *
 ******************************************************************************/
bt_device_soc_add_on_features_t* BTM_GetSocAddOnFeatures(
    uint8_t* soc_add_on_features_len) {
  *soc_add_on_features_len = soc_add_on_features_length;
  return &soc_add_on_features;
}

/*******************************************************************************
 *
 * Function         BTM_GetQllLocalSupportedFeatures
 *
 * Description      BTM_GetQllLocalSupportedFeatures
 *
 *
 * Returns          get QLL Local Supported add on features array
 *
 ******************************************************************************/
bt_device_qll_local_supported_features_t* BTM_GetQllLocalSupportedFeatures(
    uint8_t* qll_local_supported_features_len) {
  *qll_local_supported_features_len = qll_local_supported_features_length;
  return &qll_features;
}

/*******************************************************************************
 *
 * Function         BTM_BleIsCisParamUpdateLocalHostSupported
 *
 * Description      This function is called to determine if
 *CIS_Parameter_Update_Host feature is supported by local host.
 *
 * Returns          bool true if supported, false otherwise
 *
 ******************************************************************************/
bool BTM_BleIsCisParamUpdateLocalHostSupported() {
  bool supported = false;

  char value[PROPERTY_VALUE_MAX] = "true";
  property_get("persist.vendor.service.bt.cis_param_update_enabled", value,
               "true");
  if (!strncmp("true", value, 4)) supported = true;

  log::info(": supported={}", supported);

  return supported;
}

/*******************************************************************************
 *
 * Function         BTM_GetRemoteQLLFeatures
 *
 * Description      This function is called to get remote QLL features
 *
 * Parameters       features - 8 bytes array for features value
 *
 * Returns          true if feature value is available
 *
 ******************************************************************************/
bool BTM_GetRemoteQLLFeatures(uint16_t handle, uint8_t* features) {
  int idx;
  bool res = false;

  tACL_CONN* p_acl;

  if (!BTM_QBCE_QLE_HCI_SUPPORTED(soc_add_on_features.as_array)) {
    log::info("QHS not support");
    return false;
  }

  const RawAddress remote_bd_addr = acl_address_from_handle(handle);
  if (remote_bd_addr == RawAddress::kEmpty) {
    log::error("can't find acl for handle: 0x{:04x}", handle);
    return false;
  }

  p_acl = btm_acl_for_bda(remote_bd_addr, BT_TRANSPORT_LE);

  if (p_acl == nullptr) {
    log::error("can't find acl for handle: 0x{:04x}", handle);
    return false;
  }

  log::info(": qll_features_state = {:x}", p_acl->qll_features_state);

  if (p_acl->qll_features_state != BTM_QLL_FEATURES_STATE_FEATURE_COMPLETE) {
    BD_FEATURES value;
    size_t length = sizeof(value);

    if (btif_config_get_bin(p_acl->remote_addr.ToString().c_str(),
                            "QLL_FEATURES", value, &length)) {
      log::info("reading feature from config file");
      p_acl->qll_features_state = BTM_QLL_FEATURES_STATE_FEATURE_COMPLETE;
      memcpy(p_acl->remote_qll_features, value, BD_FEATURES_LEN);
      res = true;
    }
  } else {
    res = true;
  }

  if (res && features) {
    memcpy(features, p_acl->remote_qll_features, BD_FEATURES_LEN);
  }
  return res;
}

static void qbce_set_qhs_host_mode_hci_cmd_complete(tBTM_VSC_CMPL* p_data) {
  uint8_t *stream, status, subcmd;
  uint16_t opcode, length;

  if (p_data && (stream = (uint8_t*)p_data->p_param_buf)) {
    opcode = p_data->opcode;
    length = p_data->param_len;
    STREAM_TO_UINT8(status, stream);
    STREAM_TO_UINT8(subcmd, stream);
    log::info(": opcode = 0x{:04X}, length = {}, status = {}, subcmd = {}", opcode, length, status, subcmd);
    if (status == HCI_SUCCESS) {
      log::info(": status success");
    }
  }
}

static void qbce_set_qll_event_mask_hci_cmd_complete(tBTM_VSC_CMPL* p_data) {
  uint8_t *stream, status, subcmd;
  uint16_t opcode, length;

  if (p_data && (stream = (uint8_t*)p_data->p_param_buf)) {
    opcode = p_data->opcode;
    length = p_data->param_len;
    STREAM_TO_UINT8(status, stream);
    STREAM_TO_UINT8(subcmd, stream);
    log::info(": opcode = 0x{:04X}, length = {}, status = {}, subcmd = {}", opcode, length, status, subcmd);
    if (status == HCI_SUCCESS) {
      log::info(": status success");
    }
  }
}

static void qbce_set_qlm_event_mask_hci_cmd_complete(tBTM_VSC_CMPL* p_data) {
  uint8_t *stream, status, subcmd;
  uint16_t opcode, length;

  if (p_data && (stream = (uint8_t*)p_data->p_param_buf)) {
    opcode = p_data->opcode;
    length = p_data->param_len;
    STREAM_TO_UINT8(status, stream);
    STREAM_TO_UINT8(subcmd, stream);

    log::info(": opcode = 0x{:04X}, length = {}, status = {}, subcmd = {}", opcode, length, status, subcmd);
    if (status == HCI_SUCCESS) {
      log::info(": status success");
    }
  }
}

static void qbce_qle_set_host_feature_hci_cmd_complete(tBTM_VSC_CMPL* p_data) {
  uint8_t *stream, status, subcmd;
  uint16_t opcode, length;

  if (p_data && (stream = (uint8_t*)p_data->p_param_buf)) {
    opcode = p_data->opcode;
    length = p_data->param_len;
    STREAM_TO_UINT8(status, stream);
    STREAM_TO_UINT8(subcmd, stream);
    log::info(": opcode = 0x{:04X}, length = {}, status = {}, subcmd = {}", opcode, length, status, subcmd);
    if (status == HCI_SUCCESS) {
      log::info(": Status success");
    }
  }
}
static void parse_qll_read_local_supported_features_response(
    tBTM_VSC_CMPL* p_data) {
  uint8_t *stream, status, subcmd;
  uint16_t opcode, length;

  if (p_data && (stream = (uint8_t*)p_data->p_param_buf)) {
    opcode = p_data->opcode;
    length = p_data->param_len;
    STREAM_TO_UINT8(status, stream);
    STREAM_TO_UINT8(subcmd, stream);
    STREAM_TO_ARRAY(qll_features.as_array, stream,
                    (int)sizeof(bt_device_qll_local_supported_features_t));
    qll_local_supported_features_length = length - 2;
    log::info(": opcode = 0x{:04X}, length = {}, status = {}, subcmd = {}", opcode, length, status, subcmd);
    if (status == HCI_SUCCESS) {
      log::info(": status success");
      if (BTM_QBCE_QLL_MULTI_CONFIG_CIS_PARAMETER_UPDATE_CONTROLLER(
              qll_features.as_array) &&
          BTM_BleIsCisParamUpdateLocalHostSupported()) {
        uint8_t cmd[3];
        cmd[0] = QBCE_QLE_SET_HOST_FEATURE;
        cmd[1] = QBCE_QLL_MULTI_CONFIG_CIS_PARAMETER_UPDATE_HOST_BIT;
        cmd[2] = 1;
        BTM_VendorSpecificCommand(HCI_VS_QBCE_OCF, sizeof(cmd), cmd,
                                  qbce_qle_set_host_feature_hci_cmd_complete);
      }
    }
  }
}

static void parse_controller_addon_features_response(tBTM_VSC_CMPL* p_data) {
  uint8_t *stream, status;
  uint16_t opcode, length;

  if (p_data && (stream = (uint8_t*)p_data->p_param_buf)) {
    opcode = p_data->opcode;
    length = p_data->param_len;
    STREAM_TO_UINT8(status, stream);

    if (stream && (length > 8)) {
      STREAM_TO_UINT16(product_id, stream);
      STREAM_TO_UINT16(response_version, stream);

      soc_add_on_features_length = length - 5;
      STREAM_TO_ARRAY(soc_add_on_features.as_array, stream,
                      soc_add_on_features_length);
      soc_add_on_features.as_array[soc_add_on_features_length] = '\0';
    }

    log::info("::opcode = 0x{:04X}, length = {}, soc_add_on_features_length={} status = {}, product_id:{}, feature={}", opcode, length, soc_add_on_features_length, status, product_id, fmt::ptr(soc_add_on_features.as_array));
    if (status == HCI_SUCCESS) {
      log::info(": status success");

      if (BTM_SPLIT_A2DP_SCRAMBLING_DATA_REQUIRED(
              soc_add_on_features.as_array)) {
        if (BTM_SPLIT_A2DP_44P1KHZ_SAMPLE_FREQ(soc_add_on_features.as_array)) {
          scrambling_supported_freqs[number_of_scrambling_supported_freqs++] =
              BTAV_A2DP_CODEC_SAMPLE_RATE_44100;
          scrambling_supported_freqs[number_of_scrambling_supported_freqs++] =
              BTAV_A2DP_CODEC_SAMPLE_RATE_88200;
        }
        if (BTM_SPLIT_A2DP_48KHZ_SAMPLE_FREQ(soc_add_on_features.as_array)) {
          scrambling_supported_freqs[number_of_scrambling_supported_freqs++] =
              BTAV_A2DP_CODEC_SAMPLE_RATE_48000;
          scrambling_supported_freqs[number_of_scrambling_supported_freqs++] =
              BTAV_A2DP_CODEC_SAMPLE_RATE_96000;
        }
      }
      BTM_ConfigQHS();
    }
  }
}

void btm_ble_read_remote_supported_qll_features_status_cback(
    tBTM_VSC_CMPL* param) {
  uint8_t status;

  log::info(":: op: {:x}, param_len: {}", param->opcode, param->param_len);
  if (param->param_len == 1) {
    status = *param->p_param_buf;
    log::info(":: status = {}", status);
  }
}

/*******************************************************************************
 *
 * Function         btm_ble_qll_connection_complete
 *
 * Description      This function process the QLL connection complete event.
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_qll_connection_complete(uint8_t* p) {
  uint16_t handle;
  uint8_t status, param[3] = {0}, *p_param = param;
  int idx;
  tACL_CONN* p_acl;

  STREAM_TO_UINT8(status, p);
  STREAM_TO_UINT16(handle, p);
  handle = handle & 0x0FFF;

  const RawAddress remote_bd_addr = acl_address_from_handle(handle);
  if (remote_bd_addr == RawAddress::kEmpty) {
    log::error(":: can't find acl for handle: 0x{:04x}", handle);
    return;
  }

  p_acl = btm_acl_for_bda(remote_bd_addr, BT_TRANSPORT_LE);

  if (p_acl == nullptr) {
    p_acl = btm_acl_for_bda(remote_bd_addr, BT_TRANSPORT_BR_EDR);
    if (p_acl == nullptr) {
      log::error(":: can't find acl for handle: 0x{:04x}", handle);
      return;
    }
  }

  if (status != HCI_SUCCESS) {
    log::error(":: failed for handle: 0x{:04x}, status 0x{:02x}", handle, status);
    p_acl->qll_features_state = BTM_QLL_FEATURES_STATE_ERROR;
    return;
  }

  p_acl->qll_features_state = BTM_QLL_FEATURES_STATE_CONN_COMPLETE;

  UINT8_TO_STREAM(p_param, QBCE_READ_REMOTE_QLL_SUPPORTED_FEATURE);
  UINT16_TO_STREAM(p_param, handle);
  BTM_VendorSpecificCommand(
      HCI_VS_QBCE_OCF, BTM_QBCE_READ_REMOTE_QLL_SUPPORTED_FEATURE_LEN, param,
      btm_ble_read_remote_supported_qll_features_status_cback);
}

/*******************************************************************************
 *
 * Function         btm_ble_read_remote_supported_qll_features_complete
 *
 * Description      This function process the read remote supported QLL features
 *                  complete event.
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_read_remote_supported_qll_features_complete(uint8_t* p) {
  uint16_t handle;
  uint8_t status;
  int idx;
  tACL_CONN* p_acl;

  STREAM_TO_UINT8(status, p);
  STREAM_TO_UINT16(handle, p);
  handle = handle & 0x0FFF;

  const RawAddress remote_bd_addr = acl_address_from_handle(handle);
  if (remote_bd_addr == RawAddress::kEmpty) {
    log::error(":: can't find acl for handle: 0x{:04x}", handle);
    return;
  }

  p_acl = btm_acl_for_bda(remote_bd_addr, BT_TRANSPORT_LE);

  if (p_acl == nullptr) {
    log::error(":: can't find acl for handle: 0x{:04x}", handle);
    return;
  }

  if (status != HCI_SUCCESS) {
    log::error(":: failed for handle: 0x{:04x}, status 0x{:02x}", handle, status);
    p_acl->qll_features_state = BTM_QLL_FEATURES_STATE_ERROR;
    return;
  }

  p_acl->qll_features_state = BTM_QLL_FEATURES_STATE_FEATURE_COMPLETE;
  STREAM_TO_ARRAY(p_acl->remote_qll_features, p, BD_FEATURES_LEN);
  btif_config_set_bin(p_acl->remote_addr.ToString(), "QLL_FEATURES",
                      p_acl->remote_qll_features, BD_FEATURES_LEN);
}

/*******************************************************************************
 *
 * Function        BTM_GetQcmPhyState
 *
 * Description     This function returns the phy state of ACL connection.
 *
 *
 * Parameters      bda : BD address of the remote device
 *
 * Returns         Returns qcm phy state of ACL connection.
 *                 Returns default value as BR/EDR if it fails.
 *
 *
 ******************************************************************************/
uint8_t BTM_GetQcmPhyState(const RawAddress& bda) {
  bool ret;
  // Default value for QCM PHY state
  int qcm_phy_state = QCM_PHY_STATE_BR_EDR;

  ret = btif_config_get_int(bda.ToString(), "QCM_PHY_STATE", &qcm_phy_state);
  if (ret == 0) {
    log::error(":: can't find phy state for BdAddr {} in btconfig file", bda.ToString());
  }
  return (uint8_t)qcm_phy_state;
}


void btm_update_codec_settings(uint8_t* p) {
  uint16_t handle, delay = 0xFFFF;
  uint8_t types, mode = 0xFF;
  uint64_t bdAddr = 0xFFFFFFFFFFFFFFFF;

  STREAM_TO_UINT16(handle, p);
  STREAM_TO_UINT8(types, p);

  log::info(" :: handle = 0x{:02x}, types = 0x{:02x}", handle, types);

  while(types--) {
    uint8_t len = 0x00, type = 0xFF;
    STREAM_TO_UINT8(len, p);
    STREAM_TO_UINT8(type, p);
    if (len > 2 && type == 0x01) {
      STREAM_TO_UINT24(delay, p);
    } else if (len > 0 && type == 0x00) {
      STREAM_TO_UINT8(mode, p);
    } else if (len > 5 && type == 0x02) {
      STREAM_TO_UINT48(bdAddr, p);
    }
  }

  bluetooth::hci::IsoManager::GetInstance()->HandleVSCodecSettingsEvent(mode,
    delay, bdAddr);
}

/*******************************************************************************
 *
 * Function        btm_acl_update_qcm_phy_state
 *
 * Description     This function updates the qcm phy state of ACL connection.
 *
 * Returns         void
 *
 ******************************************************************************/
void btm_acl_update_qcm_phy_state(uint8_t* p) {
  uint16_t handle;
  uint8_t status, qcm_phy_state;
  int idx;
  tACL_CONN* p_acl;

  STREAM_TO_UINT8(status, p);
  STREAM_TO_UINT16(handle, p);

  handle = handle & 0x0FFF;

  const RawAddress remote_bd_addr = acl_address_from_handle(handle);
  if (remote_bd_addr == RawAddress::kEmpty) {
    log::error(":: can't find acl for handle: 0x{:04x}", handle);
    return;
  }

  p_acl = btm_acl_for_bda(remote_bd_addr, BT_TRANSPORT_LE);

  if (p_acl == nullptr) {
    p_acl = btm_acl_for_bda(remote_bd_addr, BT_TRANSPORT_BR_EDR);
    if (p_acl == nullptr) {
      log::error(":: can't find acl for handle: 0x{:04x}", handle);
      return;
    }
  }

  if (status != HCI_SUCCESS) {
    log::error(":: failed for handle: 0x{:04x}, status 0x{:02x}", handle, status);
    // Setting qcm phy state to default value: 0x00 BR/EDR
    btif_config_set_int(p_acl->remote_addr.ToString(), "QCM_PHY_STATE",
                        QCM_PHY_STATE_BR_EDR);
    return;
  }

  STREAM_TO_UINT8(qcm_phy_state, p);
  // Setting qcm phy state as 0x00 BR/EDR, 0x01 QHS
  btif_config_set_int(p_acl->remote_addr.ToString(), "QCM_PHY_STATE",
                      qcm_phy_state);
}

/*******************************************************************************
 *
 * Function        BTM_IsQHSPhySupported
 *
 * Description     This function is called to determine if QHS is supported or
 *not.
 *
 * Parameters      bda : BD address of the remote device
 *                 transport : Physical transport used for ACL connection
 *                 (BR/EDR or LE)
 *
 * Returns         True if qhs phy can be used, false otherwise.
 *
 ******************************************************************************/
bool BTM_IsQHSPhySupported(const RawAddress& bda, tBT_TRANSPORT transport) {
  bool qhs_phy = false;
  if (transport == BT_TRANSPORT_LE) {
    tACL_CONN* p_acl = btm_acl_for_bda(bda, BT_TRANSPORT_LE);
    if (p_acl == NULL) {
      log::error("invalid bda {}", bda.ToString());
      qhs_phy = false;
    } else {
      bool ret;
      BD_FEATURES features;

      ret = BTM_GetRemoteQLLFeatures(p_acl->hci_handle, (uint8_t*)&features);
      if (ret && (features[2] & 0x40)) qhs_phy = true;
    }
  }

  if (transport == BT_TRANSPORT_BR_EDR) {
    uint8_t qcm_phy_state = BTM_GetQcmPhyState(bda);
    if (qcm_phy_state == QCM_PHY_STATE_QHS) {
      qhs_phy = true;
    }
  }
  if (qhs_phy == false) {
    log::debug(": QHS not supported for transport = {} and BdAddr = {}", transport, bda.ToString());
  }
  return qhs_phy;
}

/*******************************************************************************
 *
 * Function         btm_vendor_link_power_control_event
 *
 * Description      This function process the link power control event.
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_vendor_link_power_control_event(uint8_t* p) {
  uint16_t handle;
  uint8_t subopcode;
  uint8_t NumOftechnologiesWithRestrictedPower = 0;

  STREAM_TO_UINT8(subopcode, p);

  if (subopcode == HCI_VS_SET_MAX_RADIATED_POWER_SUB_OPCODE) {
    STREAM_TO_UINT8(NumOftechnologiesWithRestrictedPower, p);

    for (int i = 0; i < NumOftechnologiesWithRestrictedPower; i++) {
      uint8_t tech;
      uint8_t max_allowed_power_index;
      uint8_t fine_back_off;
      uint8_t max_support_power;
      STREAM_TO_UINT8(tech, p);
      STREAM_TO_UINT8(max_allowed_power_index, p);
      STREAM_TO_UINT8(fine_back_off, p);
      STREAM_TO_UINT8(max_support_power, p);
      log::info(":: tech = 0x{:02x}, max_allowed_power_index = 0x{:02x}, fine_back_off = 0x{:02x}, max_support_power = 0x{:02x}", tech, max_allowed_power_index, fine_back_off, max_support_power);
    }
  }
}

/*******************************************************************************
 *
 * Function         btm_vendor_vse_cback
 *
 * Description      Process event VENDOR_SPECIFIC_EVT
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_vendor_vse_cback(uint8_t vse_subcode, uint8_t evt_len, uint8_t* p) {
  uint8_t i;
  uint8_t* pp = p;

  log::info(":: VSE event received, vse_subcode = 0x{:02x}, evt_len = 0x{:02x}", vse_subcode, evt_len);
  if (evt_len >= 1) {
    if (HCI_VSE_SUBCODE_QBCE == vse_subcode) {
      uint8_t vse_msg_type;

      STREAM_TO_UINT8(vse_msg_type, pp);
      log::info(":: QBCE VSE event received, msg = 0x{:02x}", vse_msg_type);
      switch (vse_msg_type) {
        case MSG_QBCE_QLL_CONNECTION_COMPLETE:
          btm_ble_qll_connection_complete(pp);
          break;
        case MSG_QBCE_REMOTE_SUPPORTED_QLL_FEATURES_COMPLETE:
          btm_ble_read_remote_supported_qll_features_complete(pp);
          break;
        case MSG_QBCE_QCM_PHY_CHANGE:
          btm_acl_update_qcm_phy_state(pp);
          break;
        case MSG_QBCE_VS_CODEC_SETTINGS:
          btm_update_codec_settings(pp);
          break;
        case MSG_QBCE_QLE_CIG_LATENCY_CHANGED:
          break;
        default:
          log::info(":: unknown msg type: {}", vse_msg_type);
          break;
      }
      return;
    } else if (HCI_VS_LINK_POWER_CTRL_EVENT == vse_subcode) {
      btm_vendor_link_power_control_event(pp);
    }
  }
  log::debug("BTM Event: Vendor Specific event from controller");
}

void BTM_ConfigQHS() {
  if (BTM_QBCE_QLE_HCI_SUPPORTED(soc_add_on_features.as_array)) {
    BT_HDR* response;
    char qhs_iso[PROPERTY_VALUE_MAX] = "false";
    property_get("persist.vendor.btstack.qhs_enable", qhs_iso, "true");
    uint8_t cmd[3];
    uint8_t sub_cmd = QBCE_SET_QHS_HOST_MODE;

    memset(cmd, 0, 3);

    cmd[0] = sub_cmd;
    cmd[1] = QHS_TRANSPORT_LE_ISO;

    if (!strncmp("true", qhs_iso, 4)) {
      cmd[2] = QHS_HOST_MODE_HOST_AWARE;
    } else {
      cmd[2] = QHS_HOST_DISABLE_ALL;
    }
    BTM_VendorSpecificCommand(HCI_VS_QBCE_OCF, sizeof(cmd), cmd,
                              qbce_set_qhs_host_mode_hci_cmd_complete);
    /* This property is for test/debug purpose only */
    property_get("persist.vendor.btstack.qhs_support", qhs_value, "255");
    log::info(": qhs property value= {}", qhs_value);
    qhs_support_mask = (uint8_t)atoi(qhs_value);
    log::info(": qhs support mask={}", qhs_support_mask);
    if (qhs_support_mask != 0xFF) {
      if (qhs_support_mask & QHS_BREDR_MASK) {
        cmd[1] = QHS_TRANSPORT_BREDR;
        cmd[2] = QHS_HOST_MODE_HOST_AWARE;
      } else {
        cmd[1] = QHS_TRANSPORT_BREDR;
        cmd[2] = QHS_HOST_DISABLE_ALL;
      }
      BTM_VendorSpecificCommand(HCI_VS_QBCE_OCF, sizeof(cmd), cmd,
                                qbce_set_qhs_host_mode_hci_cmd_complete);
      if (qhs_support_mask & QHS_LE_MASK) {
        cmd[1] = QHS_TRANSPORT_LE;
        cmd[2] = QHS_HOST_MODE_HOST_AWARE;
      } else {
        cmd[1] = QHS_TRANSPORT_LE;
        cmd[2] = QHS_HOST_DISABLE_ALL;
      }
      BTM_VendorSpecificCommand(HCI_VS_QBCE_OCF, sizeof(cmd), cmd,
                                qbce_set_qhs_host_mode_hci_cmd_complete);
      if (qhs_support_mask & QHS_LE_MASK) {
        cmd[1] = QHS_TRANSPORT_LE_ISO;
        cmd[2] = QHS_HOST_MODE_HOST_AWARE;
      } else {
        cmd[1] = QHS_TRANSPORT_LE_ISO;
        cmd[2] = QHS_HOST_DISABLE_ALL;
      }
      BTM_VendorSpecificCommand(HCI_VS_QBCE_OCF, sizeof(cmd), cmd,
                                qbce_set_qhs_host_mode_hci_cmd_complete);
    }
    uint8_t cmd_qll[9];
    uint8_t* stream = &cmd_qll[0];
    UINT8_TO_STREAM(stream, QBCE_SET_QLL_EVENT_MASK);
    ARRAY8_TO_STREAM(stream, (&QBCE_QLM_AND_QLL_EVENT_MASK)->as_array);

    BTM_VendorSpecificCommand(HCI_VS_QBCE_OCF, sizeof(cmd_qll), cmd_qll,
                              qbce_set_qll_event_mask_hci_cmd_complete);
  }

  if (BTM_QBCE_QCM_HCI_SUPPORTED(soc_add_on_features.as_array)) {
    uint8_t cmd_qlm[9];
    uint8_t* stream = &cmd_qlm[0];
    ;
    UINT8_TO_STREAM(stream, QBCE_SET_QLM_EVENT_MASK);
    ARRAY8_TO_STREAM(stream, (&QBCE_QLM_AND_QLL_EVENT_MASK)->as_array);

    BTM_VendorSpecificCommand(HCI_VS_QBCE_OCF, sizeof(cmd_qlm), cmd_qlm,
                              qbce_set_qlm_event_mask_hci_cmd_complete);
  }

  if (BTM_QBCE_QLE_HCI_SUPPORTED(soc_add_on_features.as_array)) {
    uint8_t cmd[1];
    cmd[0] = QBCE_READ_LOCAL_QLL_SUPPORTED_FEATURES;

    BTM_VendorSpecificCommand(HCI_VS_QBCE_OCF, sizeof(cmd), cmd,
                              parse_qll_read_local_supported_features_response);
  }
}

void BTM_ReadVendorAddOnFeaturesInternal() {
  bt_configstore_intf = get_btConfigStore_interface();
  if (bt_configstore_intf != NULL) {
    std::vector<vendor_property_t> vPropList;
    bt_configstore_intf->get_vendor_properties(BT_PROP_ALL, vPropList);

    for (auto&& vendorProp : vPropList) {
      switch (vendorProp.type) {
        case BT_PROP_SOC_TYPE:
          char soc_name[32];

          strlcpy(soc_name, vendorProp.value, sizeof(soc_name));
          soc_type =
              bt_configstore_intf->convert_bt_soc_name_to_soc_type(soc_name);
          break;

        case BT_PROP_A2DP_OFFLOAD_CAP:
          strlcpy(a2dp_offload_Cap, vendorProp.value, sizeof(a2dp_offload_Cap));
          log::info(": a2dp_offload_Cap = {}", a2dp_offload_Cap);
          break;

        case BT_PROP_SPILT_A2DP:
          if (!strncasecmp(vendorProp.value, "true", sizeof("true"))) {
            spilt_a2dp_supported = true;
          } else {
            spilt_a2dp_supported = false;
          }

          log::info(":: spilt_a2dp_supported = {}", spilt_a2dp_supported);
          break;

        case BT_PROP_AAC_FRAME_CTL:
          if (!strncasecmp(vendorProp.value, "true", sizeof("true"))) {
            aac_frame_ctl_enabled = true;
          } else {
            aac_frame_ctl_enabled = false;
          }

          log::info(": aac_frame_ctl_enabled = {}", aac_frame_ctl_enabled);
          break;

        case BT_PROP_MAX_POWER:
          decode_max_power_values((char*)vendorProp.value);
          log::info(": max_power_prop_enabled = {}", max_power_prop_enabled);
          break;
        default:
          break;
      }
    }
    host_add_on_features_list_t features_list;

    if (bt_configstore_intf->get_host_add_on_features(&features_list)) {
      host_add_on_features_length = features_list.feat_mask_len;
      if (host_add_on_features_length != 0 &&
          host_add_on_features_length <= HOST_ADD_ON_FEATURES_MAX_SIZE)
        memcpy(host_add_on_features.as_array, features_list.features,
               host_add_on_features_length);
    }

    // Read HCI_VS_GET_ADDON_FEATURES_SUPPORT
    if (soc_type >= BT_SOC_TYPE_CHEROKEE) {
      controller_add_on_features_list_t features_list;

      if (bt_configstore_intf->get_controller_add_on_features(&features_list)) {
        product_id = features_list.product_id;
        response_version = features_list.rsp_version;
        soc_add_on_features_length = features_list.feat_mask_len;
        if (soc_add_on_features_length != 0) {
          if (soc_add_on_features_length <= SOC_ADD_ON_FEATURES_MAX_SIZE) {
            memcpy(soc_add_on_features.as_array, features_list.features,
                   soc_add_on_features_length);
            if (BTM_SPLIT_A2DP_SCRAMBLING_DATA_REQUIRED(
                    soc_add_on_features.as_array)) {
              if (BTM_SPLIT_A2DP_44P1KHZ_SAMPLE_FREQ(
                      soc_add_on_features.as_array)) {
                scrambling_supported_freqs
                    [number_of_scrambling_supported_freqs++] =
                        BTAV_A2DP_CODEC_SAMPLE_RATE_44100;
                scrambling_supported_freqs
                    [number_of_scrambling_supported_freqs++] =
                        BTAV_A2DP_CODEC_SAMPLE_RATE_88200;
              }
              if (BTM_SPLIT_A2DP_48KHZ_SAMPLE_FREQ(
                      soc_add_on_features.as_array)) {
                scrambling_supported_freqs
                    [number_of_scrambling_supported_freqs++] =
                        BTAV_A2DP_CODEC_SAMPLE_RATE_48000;
                scrambling_supported_freqs
                    [number_of_scrambling_supported_freqs++] =
                        BTAV_A2DP_CODEC_SAMPLE_RATE_96000;
              }
            }
            BTM_ConfigQHS();
          } else {
            log::fatal("invalid soc add on features length: {}", soc_add_on_features_length);
          }
        }
      }
    }
  }
}

/*******************************************************************************
 *
 * Function         BTM_ReadVendorAddOnFeatures
 *
 * Description      BTM_ReadVendorAddOnFeatures
 *
 * Parameters:      None
 *
 ******************************************************************************/
void BTM_ReadVendorAddOnFeatures() {
  bool btConfigStore = true;
  char bt_config_store_prop[PROPERTY_VALUE_MAX] = {'\0'};
  int ret = 0;

  is_power_backoff_enabled = false;
  ret = property_get("ro.vendor.bluetooth.btconfigstore", bt_config_store_prop,
                     "true");

  if (ret != 0) {
    if (!strncasecmp(bt_config_store_prop, "true", sizeof("true"))) {
      btConfigStore = true;
    } else {
      btConfigStore = false;
    }
    log::info(":: btConfigStore = {}", btConfigStore);
  }

  if (btConfigStore) {
    BTM_ReadVendorAddOnFeaturesInternal();
  } else {
    log::info(": Soc Add On");

    char soc_name[PROPERTY_VALUE_MAX] = {'\0'};
    char splita2dp[PROPERTY_VALUE_MAX];
    char aac_frame_ctl[PROPERTY_VALUE_MAX];
    char max_pow_support[PROPERTY_VALUE_MAX];

    ret = property_get("persist.vendor.qcom.bluetooth.soc", soc_name, "");
    log::info(":: Bluetooth soc type set to: {}, ret: {}", soc_name, ret);

    if (ret != 0) {
      bt_configstore_intf = get_btConfigStore_interface();
      soc_type = bt_configstore_intf->convert_bt_soc_name_to_soc_type(soc_name);
    }

    ret = property_get("persist.vendor.qcom.bluetooth.enable.splita2dp",
                       splita2dp, "true");
    log::info(":: persist.vendor.qcom.bluetooth.enable.splita2dp: {}, ret: {}", splita2dp, ret);

    if (ret != 0) {
      if (!strncasecmp(splita2dp, "true", sizeof("true"))) {
        spilt_a2dp_supported = true;
      } else {
        spilt_a2dp_supported = false;
      }
      log::info(":: spilt_a2dp_supported = {}", spilt_a2dp_supported);
    }

    ret = property_get("persist.vendor.qcom.bluetooth.a2dp_offload_cap",
                       a2dp_offload_Cap, "");
    log::info(":: a2dp_offload_Cap = {}", a2dp_offload_Cap);

    ret = property_get("persist.vendor.qcom.bluetooth.aac_frm_ctl.enabled",
                       aac_frame_ctl, "false");
    log::info(":: persist.vendor.qcom.bluetooth.aac_frm_ctl.enabled: {}, ret: {}", aac_frame_ctl, ret);

    if (ret != 0) {
      if (!strncasecmp(aac_frame_ctl, "true", sizeof("true"))) {
        aac_frame_ctl_enabled = true;
      } else {
        aac_frame_ctl_enabled = false;
      }
    }

    ret = property_get("persist.vendor.qcom.bluetooth.max_power_support",
                       max_pow_support, "false");
    log::info(":: persist.vendor.qcom.bluetooth.max_power_support: {}, ret: {}", max_pow_support, ret);

    if (ret != 0) {
      decode_max_power_values((char*)max_pow_support);
      log::info(": max_power_prop_enabled = {}", max_power_prop_enabled);
    }

    if (soc_type >= BT_SOC_TYPE_CHEROKEE) {
      BTM_VendorSpecificCommand(HCI_VS_GET_ADDON_FEATURES_SUPPORT, 0, NULL,
                                parse_controller_addon_features_response);
    }

    /*if (!HCI_LE_CIS_MASTER_SUPPORT(features_ble.as_array)) {
      adv_audio_support_mask &= ~ADV_AUDIO_UNICAST_FEAT_MASK;
    }
    if (!HCI_LE_PERIODIC_SYNC_TRANSFER_SEND_SUPPORTED(features_ble.as_array)) {
      adv_audio_support_mask &= ~ADV_AUDIO_BCA_FEAT_MASK;
    }
    if (!HCI_LE_ISO_BROADCASTER_SUPPORTED(features_ble.as_array)) {
      adv_audio_support_mask &= ~ADV_AUDIO_BCS_FEAT_MASK;
    }
    snprintf(adv_audio_property, 2, "%d", adv_audio_support_mask);
    osi_property_set("persist.vendor.service.bt.adv_audio_mask",
    adv_audio_property); */
  }
}

/*******************************************************************************
 *
 * Function         BTM_SetPowerBackOffState
 *
 * Description      This function set PowerBackOff state.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTM_SetPowerBackOffState(bool status) {
  if (max_power_prop_enabled) {
    log::info("is_power_backoff_enabled: {} status = {}", is_power_backoff_enabled, status);
    if (is_power_backoff_enabled != status) {
      btm_vendor_set_tech_based_max_power(status);
      is_power_backoff_enabled = status;
    }
  } else {
    log::info("power back off config is not enabled");
  }
}

/*******************************************************************************
 *
 * Function         btm_vendor_link_power_cntrl_callback
 *
 * Description      Callback to notify link_power_cntrl cmd status
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_vendor_link_power_ctrl_callback(tBTM_VSC_CMPL* param) {
  uint8_t status = 0xFF;
  uint8_t* p;

  /* Check status of command complete event */
  CHECK(param->opcode == HCI_VS_LINK_POWER_CTRL_REQ_OPCODE);
  CHECK(param->param_len > 0);

  p = param->p_param_buf;
  STREAM_TO_UINT8(status, p);
  if (status != HCI_SUCCESS) {
    log::info(": Status = 0x{:02x} (0 is success)", status);
    return;
  }

  log::info(": param->opcode=0x{:02x} subopcode=0x{:02x} status=0x{:02x}", param->opcode, param->p_param_buf[1], param->p_param_buf[0]);
}

bool get_max_power_values(max_pow_feature_t* tech_based_max_power) {
  if (max_power_prop_value[0] != BT_DEFAULT_POWER) {
    log::debug("using BR MAX POW from property");
    tech_based_max_power->BR_max_pow_feature = true;
    tech_based_max_power->BR_max_pow_support = max_power_prop_value[0];
  } else {
    log::debug("discarding BR MAX POW from property as it set to default");
  }

  if (max_power_prop_value[1] != BT_DEFAULT_POWER) {
    log::debug("using EDR MAX POW from property");
    tech_based_max_power->EDR_max_pow_feature = true;
    tech_based_max_power->EDR_max_pow_support = max_power_prop_value[1];
  } else {
    log::debug("discarding EDR MAX POW from property as it set to default");
  }

  if (max_power_prop_value[2] != BT_DEFAULT_POWER) {
    log::debug("using BLE MAX POW from property");
    tech_based_max_power->BLE_max_pow_feature = true;
    tech_based_max_power->BLE_max_pow_support = max_power_prop_value[2];
  } else {
    log::debug("discarding BLE MAX POW from property as it set to default");
  }

  return true;
}

/*******************************************************************************
 *
 * Function         btm_vendor_set_tech_based_max_power
 *
 * Description      limit the maximum output power for particular technology
 *
 *
 * Returns          void
 *
 ******************************************************************************/
static void btm_vendor_set_tech_based_max_power(bool status) {
  uint8_t param[8] = {0};
  uint8_t HCI_VS_LINK_POWER_CTRL_PARAM_SIZE;

  param[0] = HCI_VS_SET_MAX_RADIATED_POWER_SUB_OPCODE;
  if (status == true) {
    static max_pow_feature_t tech_based_max_power;
    uint8_t tech_count = 0, i = 2;
    log::info(": entry");

    /* check whether the property is set, if property is enabled
     * use those power values, else fall back to values set in config
     * file.
     */
    get_max_power_values(&tech_based_max_power);

    if (tech_based_max_power.BR_max_pow_feature == true) {
      param[1] = ++tech_count;
      param[i++] = BR_TECH_VALUE;
      param[i++] = tech_based_max_power.BR_max_pow_support;

      log::info("BR_max_power fetch from property 0x{:02x}", tech_based_max_power.BR_max_pow_support);
    }

    if (tech_based_max_power.EDR_max_pow_feature == true) {
      param[1] = ++tech_count;
      param[i++] = EDR_TECH_VALUE;
      param[i++] = tech_based_max_power.EDR_max_pow_support;

      log::info("EDR_max_power fetch from property: 0x{:02x}", tech_based_max_power.EDR_max_pow_support);
    }

    if (tech_based_max_power.BLE_max_pow_feature == true) {
      param[1] = ++tech_count;
      param[i++] = BLE_TECH_VALUE;
      param[i++] = tech_based_max_power.BLE_max_pow_support;

      log::info("BLE_max_power fetch from property: 0x{:02x}", tech_based_max_power.BLE_max_pow_support);
    }

    if (!tech_count) {
      log::info(": max power not configured for any tech");
      return;
    } else {
      HCI_VS_LINK_POWER_CTRL_PARAM_SIZE = 2 + (tech_count * 2);
    }

  } else {
    HCI_VS_LINK_POWER_CTRL_PARAM_SIZE = 8;
    param[1] = 0x03;
    param[2] = BR_TECH_VALUE;
    param[3] = 0x80;
    param[4] = EDR_TECH_VALUE;
    param[5] = 0x80;
    param[6] = BLE_TECH_VALUE;
    param[7] = 0x80;
  }

  BTM_VendorSpecificCommand(HCI_VS_LINK_POWER_CTRL_REQ_OPCODE,
                            HCI_VS_LINK_POWER_CTRL_PARAM_SIZE, param,
                            btm_vendor_link_power_ctrl_callback);
}
