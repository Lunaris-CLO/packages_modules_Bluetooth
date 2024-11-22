/*
 * Copyright (c) 2013,2020, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *        * Redistributions of source code must retain the above copyright
 *          notice, this list of conditions and the following disclaimer.
 *        * Redistributions in binary form must reproduce the above copyright
 *            notice, this list of conditions and the following disclaimer in
 * the documentation and/or other materials provided with the distribution.
 *        * Neither the name of The Linux Foundation nor
 *            the names of its contributors may be used to endorse or promote
 *            products derived from this software without specific prior written
 *            permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.    IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Changes from Qualcomm Innovation Center are provided under the following
 * license:
 *
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 ******************************************************************************/
#if TEST_APP_INTERFACE == TRUE
#include <bluetooth/log.h>
#include <hardware/bluetooth.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "btif_api.h"
#include "btif_config.h"
#include "l2c_api.h"
#include "l2cdefs.h"
#include "osi/include/allocator.h"
#include "stack/include/btm_client_interface.h"
#include "stack/l2cap/l2c_int.h"

using namespace bluetooth;

#include <bt_testapp.h>
#define L2CA_REGISTER_COC(a, b, c) L2CA_RegisterLECoc(a, (tL2CAP_APPL_INFO*)(b))
#define L2CA_DEREGISTER_COC(a) L2CA_DeregisterCoc(a)
#define L2CA_CONNECT_COC_REQ(a, b, c) L2CA_ConnectLECocReq(a, b, c)
#define L2CA_CONNECT_COC_RSP(a, b, c, d, e, f) \
  L2CA_ConnectLECocRsp(a, b, c, d, e, f)
#define L2CA_GET_PEER_COC_CONFIG(a, b) L2CA_GetPeerLECocConfig(a, b)
#define BTM_SEC_PROTO_L2CAP 0
#define BTM_SEC_SERVICE_ATT 50

bool btif_get_address_type(const RawAddress& bda, tBLE_ADDR_TYPE* p_addr_type);
bool btif_get_device_type(const RawAddress& bda, int* p_device_type);

static tL2CAP_APPL_INFO* pl2test_l2c_appl = NULL;
static bt_status_t L2cap_Init(tL2CAP_APPL_INFO* p);
static bt_status_t L2cap_Register(uint16_t psm, bool conn_type,
                                  tL2CAP_ERTM_INFO* p_ertm_info,
                                  uint16_t my_mtu, uint16_t required_remote_mtu,
                                  uint16_t sec_level);
static bt_status_t L2cap_DeRegister(uint16_t psm);
static uint16_t L2cap_AllocatePSM(void);
static uint16_t L2cap_Connect(uint16_t psm, RawAddress* bd_addr);
static bool L2cap_ConnectRsp(RawAddress p_bd_addr, uint8_t id, uint16_t lcid,
                             uint16_t result, uint16_t status);
static uint16_t L2cap_ErtmConnect(uint16_t psm, RawAddress p_bd_addr,
                                  tL2CAP_ERTM_INFO* p_ertm_info);
static bool L2cap_ErtmConnectRsp(RawAddress p_bd_addr, uint8_t id,
                                 uint16_t lcid, uint16_t result,
                                 uint16_t status,
                                 tL2CAP_ERTM_INFO* p_ertm_info);
static bool L2cap_ConfigReq(uint16_t cid, tL2CAP_CFG_INFO* p_cfg);
static bool L2cap_ConfigRsp(uint16_t cid, tL2CAP_CFG_INFO* p_cfg);
static bool L2cap_DisconnectReq(uint16_t cid);
static bool L2cap_DisconnectRsp(uint16_t cid);
static uint8_t L2cap_DataWrite(uint16_t cid, char* p_data, uint32_t len);
static bool L2cap_Ping(RawAddress p_bd_addr, tL2CA_ECHO_RSP_CB* p_cb);
static bool L2cap_SetIdleTimeout(uint16_t cid, uint16_t timeout,
                                 bool is_global);
static bool L2cap_SetIdleTimeoutByBdAddr(RawAddress bd_addr, uint16_t timeout);
static void L2cap_SetSecConnOnlyMode(bool secvalue);
static uint8_t L2cap_SetDesireRole(uint8_t new_role);
static uint16_t L2cap_LocalLoopbackReq(uint16_t psm, uint16_t handle,
                                       RawAddress p_bd_addr);
static uint16_t L2cap_FlushChannel(uint16_t lcid, uint16_t num_to_flush);
static bool L2cap_SetAclPriority(RawAddress bd_addr, uint8_t priority);
static bool L2cap_FlowControl(uint16_t cid, bool data_enabled);
static bool L2cap_SendTestSFrame(uint16_t cid, bool rr_or_rej,
                                 uint8_t back_track);
static bool L2cap_SetTxPriority(uint16_t cid, tL2CAP_CHNL_PRIORITY priority);
static bool L2cap_SetChnlDataRate(uint16_t cid, tL2CAP_CHNL_DATA_RATE tx,
                                  tL2CAP_CHNL_DATA_RATE rx);
static bool L2cap_SetFlushTimeout(RawAddress bd_addr, uint16_t flush_tout);
static uint8_t L2cap_DataWriteEx(uint16_t cid, BT_HDR* p_data, uint16_t flags);
static bool L2cap_SetChnlFlushability(uint16_t cid, bool is_flushable);
static bool L2cap_GetPeerFeatures(RawAddress bd_addr, uint32_t* p_ext_feat,
                                  uint8_t* p_chnl_mask);
static bool L2cap_GetBDAddrbyHandle(uint16_t handle, RawAddress bd_addr);
static uint8_t L2cap_GetChnlFcrMode(uint16_t lcid);
static uint16_t L2cap_SendFixedChnlData(uint16_t fixed_cid, RawAddress rem_bda,
                                        BT_HDR* p_buf);
static bt_status_t L2cap_LE_Register(uint16_t le_psm, bool ConnType,
                                     uint16_t SecLevel, uint8_t enc_key_size,
                                     const tL2CAP_APPL_INFO& p_cb_info,
                                     tL2CAP_LE_CFG_INFO cfg);
static bt_status_t L2cap_LE_DeRegister(uint16_t psm);
static uint16_t L2cap_LE_Connect(uint16_t le_psm, RawAddress address,
                                 tL2CAP_LE_CFG_INFO* p_cfg);
static bool L2cap_LE_ConnectRsp(RawAddress p_bd_addr, uint8_t id, uint16_t lcid,
                                uint16_t result, uint16_t status,
                                tL2CAP_LE_CFG_INFO* p_cfg);
static bool L2cap_LE_FlowControl(uint16_t lcid, uint16_t credits);
static void L2cap_LE_freebuf(BT_HDR* p_buf);
static std::vector<uint16_t> L2cap_ConnectCocReq(uint16_t psm,
                                                 const RawAddress& p_bd_addr,
                                                 tL2CAP_LE_CFG_INFO* p_cfg);
static bool L2cap_ConnectCocRsp(const RawAddress& p_bd_addr, uint8_t id,
                                std::vector<uint16_t>& accepted_lcids,
                                uint16_t result, tL2CAP_LE_CFG_INFO* p_cfg);
static bt_status_t L2cap_coc_register(uint16_t psm,
                                      const tL2CAP_APPL_INFO& p_cb_info,
                                      uint16_t secLevel,
                                      tL2CAP_LE_CFG_INFO cfg);
static BT_HDR* L2cap_ReadData(uint16_t cid);

static const btl2cap_interface_t btl2capInterface = {
    sizeof(btl2cap_interface_t),
    L2cap_Init,
    L2cap_Register,
    L2cap_DeRegister,
    L2cap_AllocatePSM,
    L2cap_Connect,
    L2cap_ConnectRsp,
    L2cap_ErtmConnect,
    L2cap_ErtmConnectRsp,
    L2cap_ConfigReq,
    L2cap_ConfigRsp,
    L2cap_DisconnectReq,
    L2cap_DisconnectRsp,
    L2cap_DataWrite,
    L2cap_Ping,
    L2cap_SetIdleTimeout,
    L2cap_SetIdleTimeoutByBdAddr,
    L2cap_SetDesireRole,
    L2cap_SetSecConnOnlyMode,
    L2cap_LocalLoopbackReq,
    L2cap_FlushChannel,
    L2cap_SetAclPriority,
    L2cap_FlowControl,
    L2cap_SendTestSFrame,
    L2cap_SetTxPriority,
    L2cap_SetChnlDataRate,
    L2cap_SetFlushTimeout,
    L2cap_DataWriteEx,
    L2cap_SetChnlFlushability,
    L2cap_GetPeerFeatures,
    L2cap_GetBDAddrbyHandle,
    L2cap_GetChnlFcrMode,
    L2cap_SendFixedChnlData,
    NULL,
    L2cap_LE_Register,
    L2cap_LE_DeRegister,
    L2cap_LE_Connect,
    L2cap_LE_FlowControl,
    L2cap_LE_freebuf,
    L2cap_ConnectCocReq,
    L2cap_ConnectCocRsp,
    L2cap_coc_register,
    L2cap_ReadData};

const btl2cap_interface_t* btif_l2cap_get_interface(void) {
  log::info("{}", __FUNCTION__);
  return &btl2capInterface;
}

/*
- Take PSM only once during the Register func call.
- Rest of the functions (connect, dereg) uses the same PSM. This way user need
not pass it again. This will also avoid additional error checks like
unregistered psm is passed etc.
 */

static uint16_t g_Psm = 0;
static uint16_t g_lcid = 0;

static bt_status_t L2cap_Init(tL2CAP_APPL_INFO* p) {
  pl2test_l2c_appl = p;
  return BT_STATUS_SUCCESS;
}
/*******************************************************************************
**
** Function         L2cap_Register
**
** Description      This function is called during the task startup
**                  to register interface functions with L2CAP.
**
*******************************************************************************/
static bt_status_t L2cap_Register(uint16_t psm, bool conn_type,
                                  tL2CAP_ERTM_INFO* p_ertm_info,
                                  uint16_t my_mtu, uint16_t required_remote_mtu,
                                  uint16_t sec_level) {
  log::debug("L2cap_Register :: psm={}", psm);
  if (!get_btm_client_interface().security.BTM_SetSecurityLevel(
          conn_type, "l2test", BTM_SEC_PROTO_L2CAP, sec_level, psm, 0, 0)) {
    log::debug("Error:: BTM_SetSecurityLevel failed");
    return BT_STATUS_FAIL;
  }
#if 1
  if (4113 == psm) {
    if (!get_btm_client_interface().security.BTM_SetSecurityLevel(
            conn_type, "l2test 4113", BTM_SEC_PROTO_L2CAP, sec_level, psm, 0,
            0)) {
      log::debug("Error:: BTM_SetSecurityLevel failed");
      return BT_STATUS_FAIL;
    }
  }
#endif
  g_Psm = L2CA_Register(psm, *pl2test_l2c_appl, true, p_ertm_info, my_mtu,
                        required_remote_mtu, sec_level);
  if (0 == g_Psm) {
    log::debug("Error:: L2CA_Register failed");
    return BT_STATUS_FAIL;
  }
  return BT_STATUS_SUCCESS;
}

/*******************************************************************************
**
** Function L2cap_LE_Register
**
** Description This function is called during the task startup
** to register interface functions with L2CAP.
**
*******************************************************************************/
static bt_status_t L2cap_LE_Register(uint16_t le_psm, bool ConnType,
                                     uint16_t SecLevel, uint8_t enc_key_size,
                                     const tL2CAP_APPL_INFO& p_cb_info,
                                     tL2CAP_LE_CFG_INFO cfg) {
  log::debug("LE-L2CAP: {} le_psm={}, SecLevel={} ", __FUNCTION__, le_psm,
             SecLevel);
#if 0
    if (!get_btm_client_interface().security.BTM_SetSecurityLevel (ConnType, "l2c_le_test", BTM_SEC_SERVICE_ATT,
            SecLevel, le_psm))
    {
        log::error("LE-L2CAP: BTM_SetSecurityLevel failed");
        return BT_STATUS_FAIL;
    }
    if (!BTM_SetBleEncKeySize ("l2c_le_test", enc_key_size, le_psm))
    {
        log::error("LE-L2CAP: BTM_SetBleEncKeySize failed");
        return BT_STATUS_FAIL;
    }
#endif
  uint16_t vpsm = L2CA_RegisterLECoc(le_psm, p_cb_info, SecLevel, cfg);
  if (0 == g_Psm) {
    log::error("LE-L2CAP: L2cap_LE_Register failed");
    return BT_STATUS_FAIL;
  }

  if (!get_btm_client_interface().security.BTM_SetSecurityLevel(
          ConnType, "l2c_le_test", BTM_SEC_SERVICE_ATT, SecLevel, le_psm, 0,
          0)) {
    log::error("LE-L2CAP: BTM_SetSecurityLevel failed");
    return BT_STATUS_FAIL;
  }
  return BT_STATUS_SUCCESS;
}

static bt_status_t L2cap_coc_register(uint16_t psm,
                                      const tL2CAP_APPL_INFO& p_cb_info,
                                      uint16_t secLevel,
                                      tL2CAP_LE_CFG_INFO cfg) {
  log::debug("ECFC-L2CAP: {} psm={}, SecLevel={} ", __FUNCTION__, psm,
             secLevel);

  g_Psm = L2CA_RegisterLECoc(psm, p_cb_info, secLevel, cfg);

  if (0 == g_Psm) {
    log::error("ECFC-L2CAP: L2cap_Register failed");
    return BT_STATUS_FAIL;
  }

  if (!get_btm_client_interface().security.BTM_SetSecurityLevel(
          true, "ecfc_test", BTM_SEC_SERVICE_EATT, secLevel, psm, 0, 0)) {
    log::error("ECFC-L2CAP: BTM_SetSecurityLevel failed");
    return BT_STATUS_FAIL;
  }
  return BT_STATUS_SUCCESS;
}

static std::vector<uint16_t> L2cap_ConnectCocReq(uint16_t psm,
                                                 const RawAddress& p_bd_addr,
                                                 tL2CAP_LE_CFG_INFO* p_cfg) {
  log::debug("ECFC-L2CAP: {} ", __FUNCTION__);

  return (L2CA_ConnectCreditBasedReq(psm, p_bd_addr, p_cfg));
}

static bool L2cap_ConnectCocRsp(const RawAddress& p_bd_addr, uint8_t id,
                                std::vector<uint16_t>& accepted_lcids,
                                uint16_t result, tL2CAP_LE_CFG_INFO* p_cfg) {
  log::debug("ECFC-L2CAP: {} ", __FUNCTION__);

  return (
      L2CA_ConnectCreditBasedRsp(p_bd_addr, id, accepted_lcids, result, p_cfg));
}
static BT_HDR* L2cap_ReadData(uint16_t cid) { return NULL; }

static uint16_t L2cap_LE_Connect(uint16_t le_psm, RawAddress address,
                                 tL2CAP_LE_CFG_INFO* p_cfg) {
  log::debug("LE-L2CAP: {}:: {}", __FUNCTION__, address.ToString().c_str());

  tBLE_ADDR_TYPE addr_type = 0;
  int device_type = 0;
  uint16_t sec_level = 0;
  if (btif_get_address_type(address, &addr_type) &&
      btif_get_device_type(address, &device_type) &&
      device_type != BT_DEVICE_TYPE_BREDR) {
    BTA_DmAddBleDevice(address, addr_type, device_type);
  }

  if (0 == (g_lcid = L2CA_ConnectLECocReq(le_psm, address, p_cfg, sec_level))) {
    log::error("LE-L2CAP: L2CA_LE_CreditBasedConn_Req failed for le_psm ");
  }
  return g_lcid;
}

static bool L2cap_LE_ConnectRsp(RawAddress p_bd_addr, uint8_t id, uint16_t lcid,
                                uint16_t result, uint16_t status,
                                tL2CAP_LE_CFG_INFO* p_cfg) {
  p_cfg->credits = 100;
  p_cfg->mtu = L2CAP_LE_MIN_MTU;
  p_cfg->mps = L2CAP_LE_MIN_MPS;
#if 0
    if (!L2CA_LE_CreditBasedConn_Rsp (p_bd_addr, id, lcid, conn_info)) {
        log::error("LE-L2CAP: L2CA_LE_CreditBasedConn_Rsp failed");
        return BT_STATUS_FAIL;
    }
#endif
  return BT_STATUS_SUCCESS;
}

static bool L2cap_LE_FlowControl(uint16_t lcid, uint16_t credits) {
  return BT_STATUS_SUCCESS;
}
static void L2cap_LE_freebuf(BT_HDR* p_buf) { osi_free(p_buf); }

static bt_status_t L2cap_LE_DeRegister(uint16_t psm) {
  L2CA_DeregisterLECoc(psm);
  return BT_STATUS_SUCCESS;
}

static bt_status_t L2cap_DeRegister(uint16_t psm) {
  L2CA_Deregister(psm);
  return BT_STATUS_SUCCESS;
}

static uint16_t L2cap_AllocatePSM(void) {
  log::debug("L2cap_AllocatePSM");
  return L2CA_AllocateLePSM();
}

static uint16_t L2cap_Connect(uint16_t psm, RawAddress* bd_addr) {
  log::debug("L2cap_Connect:: {}", bd_addr->ToString().c_str());

  if (0 == (g_lcid = L2CA_ConnectReq(psm, bd_addr->address))) {
    log::debug("Error:: L2CA_ConnectReq failed for psm {}", psm);
  }
  return g_lcid;
}

static bool L2cap_ConnectRsp(RawAddress p_bd_addr, uint8_t id, uint16_t lcid,
                             uint16_t result, uint16_t status) {
  return BT_STATUS_SUCCESS;
}

static uint16_t L2cap_ErtmConnect(uint16_t psm, RawAddress address,
                                  tL2CAP_ERTM_INFO* p_ertm_info) {
  log::debug("L2cap_ErtmConnect:: {}", address.ToString().c_str());
  if (0 == (g_lcid = L2CA_ConnectReq(psm, address.address))) {
    log::debug("Error:: L2CA_ConnectReq failed for psm {}", psm);
  }
  return g_lcid;
}

static bool L2cap_ErtmConnectRsp(RawAddress p_bd_addr, uint8_t id,
                                 uint16_t lcid, uint16_t result,
                                 uint16_t status,
                                 tL2CAP_ERTM_INFO* p_ertm_info) {
  return BT_STATUS_SUCCESS;
}

static bool L2cap_ConfigReq(uint16_t cid, tL2CAP_CFG_INFO* p_cfg) {
  log::debug("L2cap_ConfigReq:: Invoked\n");
  if (p_cfg->fcr_present) {
    log::debug(
        "L2cap_ConfigReq::  mode %u, txwinsz %u, max_trans %u, rtrans_tout %u, "
        "mon_tout %u, mps %u\n",
        p_cfg->fcr.mode, p_cfg->fcr.tx_win_sz, p_cfg->fcr.max_transmit,
        p_cfg->fcr.rtrans_tout, p_cfg->fcr.mon_tout, p_cfg->fcr.mps);
  }
  return false;
}

static bool L2cap_ConfigRsp(uint16_t cid, tL2CAP_CFG_INFO* p_cfg) {
  log::debug("L2cap_ConfigRsp:: Invoked");
  return false;
}

static bool L2cap_DisconnectReq(uint16_t cid) {
  log::debug("L2cap_DisconnectReq:: cid={}", cid);
  return L2CA_DisconnectReq(cid);
}
static bool L2cap_DisconnectRsp(uint16_t cid) {
  log::debug("L2cap_DisconnectRsp:: Invoked");
  return false;
}

static uint8_t L2cap_DataWrite(uint16_t cid, char* p_data, uint32_t len) {
  log::debug("L2cap_DataWrite:: Invoked");
  BT_HDR* p_msg = NULL;
  uint8_t *ptr, *p_start;

  p_msg = (BT_HDR*)osi_malloc(BT_DEFAULT_BUFFER_SIZE);
  log::debug("osi_malloc");
  if (!p_msg) {
    log::debug("No resource to allocate");
    return BT_STATUS_FAIL;
  }
  p_msg->offset = L2CAP_MIN_OFFSET;
  ptr = p_start = (uint8_t*)(p_msg + 1) + L2CAP_MIN_OFFSET;
  p_msg->len =
      len;  // Sends len bytes, irrespective of what you copy to the buffer
  memcpy(ptr, p_data, len);
  return L2CA_DataWrite(cid, p_msg);
}

static bool L2cap_Ping(RawAddress p_bd_addr, tL2CA_ECHO_RSP_CB* p_cb) {
  log::debug("L2cap_Ping:: Invoked");
  return L2CA_Ping(p_bd_addr, p_cb);
}

static bool L2cap_SetIdleTimeout(uint16_t cid, uint16_t timeout,
                                 bool is_global) {
  log::debug("L2cap_SetIdleTimeout:: Invoked: not implemented");
  return false;
}

static bool L2cap_SetIdleTimeoutByBdAddr(RawAddress bd_addr, uint16_t timeout) {
  log::debug("L2cap_SetIdleTimeoutByBdAddr:: Invoked");
  return L2CA_SetIdleTimeoutByBdAddr(bd_addr, timeout, BT_TRANSPORT_BR_EDR);
}

static void L2cap_SetSecConnOnlyMode(bool secvalue) {
  log::debug("L2cap_SetSecConnOnlyMode:: Invoked not implemeted");
}

static uint8_t L2cap_SetDesireRole(uint8_t new_role) {
  log::debug("L2CA_SetDesireRole:: Invoked : not implemeted");
  return 0;
}

static uint16_t L2cap_LocalLoopbackReq(uint16_t psm, uint16_t handle,
                                       RawAddress p_bd_addr) {
  log::debug("L2cap_LocalLoopbackReq:: Invoked");
  return 0;
}

static uint16_t L2cap_FlushChannel(uint16_t lcid, uint16_t num_to_flush) {
  log::debug("L2cap_FlushChannel:: Invoked");
  return L2CA_FlushChannel(lcid, num_to_flush);
}

static bool L2cap_SetAclPriority(RawAddress bd_addr, uint8_t priority) {
  log::debug("L2cap_SetAclPriority:: Invoked");
  return false;
}

static bool L2cap_FlowControl(uint16_t cid, bool data_enabled) {
  log::debug("L2cap_FlowControl:: Invoked with LocalBusy={}\n",
             (data_enabled) ? "FALSE" : "TRUE");
  return false;
}

static bool L2cap_SendTestSFrame(uint16_t cid, bool rr_or_rej,
                                 uint8_t back_track) {
  log::debug("L2cap_SendTestSFrame:: Invoked");
  return false;
}

static bool L2cap_SetTxPriority(uint16_t cid, tL2CAP_CHNL_PRIORITY priority) {
  log::debug("L2cap_SetTxPriority:: Invoked");
  return L2CA_SetTxPriority(cid, priority);
}

static bool L2cap_SetChnlDataRate(uint16_t cid, tL2CAP_CHNL_DATA_RATE tx,
                                  tL2CAP_CHNL_DATA_RATE rx) {
  log::debug("L2cap_SetChnlDataRate:: Invoked");
  return false;
}

static bool L2cap_SetFlushTimeout(RawAddress bd_addr, uint16_t flush_tout) {
  log::debug("L2cap_SetFlushTimeout:: Invoked");
  return false;
}

static uint8_t L2cap_DataWriteEx(uint16_t cid, BT_HDR* p_data, uint16_t flags) {
  log::debug("L2cap_DataWriteEx:: Invoked");
  return false;
}
static bool L2cap_SetChnlFlushability(uint16_t cid, bool is_flushable) {
  log::debug("L2cap_SetChnlFlushability:: Invoked");
  return false;
}
static bool L2cap_GetPeerFeatures(RawAddress bd_addr, uint32_t* p_ext_feat,
                                  uint8_t* p_chnl_mask) {
  log::debug("L2cap_GetPeerFeatures:: Invoked");
  return false;
}
static bool L2cap_GetBDAddrbyHandle(uint16_t handle, RawAddress bd_addr) {
  log::debug("L2cap_GetBDAddrbyHandle:: Invoked");
  return false;
}
static uint8_t L2cap_GetChnlFcrMode(uint16_t lcid) {
  log::debug("L2cap_GetChnlFcrMode:: Invoked");
  return false;
}

//---------------------FIXED CHANNEL API ---------------------
static uint16_t L2cap_SendFixedChnlData(uint16_t fixed_cid, RawAddress rem_bda,
                                        BT_HDR* p_buf) {
  log::debug("L2cap_SendFixedChnlData:: Invoked");
  p_buf->event = 20;
  return L2CA_SendFixedChnlData(fixed_cid, rem_bda, p_buf);
}
#endif  // TEST_APP_INTERFACE
