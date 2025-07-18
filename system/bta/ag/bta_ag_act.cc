/******************************************************************************
 *
 *  Copyright 2003-2012 Broadcom Corporation
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

/******************************************************************************
 *
 *  This file contains action functions for the audio gateway.
 *
 ******************************************************************************/

#include <bluetooth/log.h>
#include <com_android_bluetooth_flags.h>

#include <cstdint>
#include <cstring>

#include "bta/ag/bta_ag_int.h"
#include "bta/include/bta_dm_api.h"
#include "bta/include/bta_hfp_api.h"
#include "bta_ag_swb_aptx.h"
#include "internal_include/bt_trace.h"

#ifdef __ANDROID__
#include "bta/le_audio/devices.h"
#endif

#include "btif/include/btif_config.h"
#include "device/include/interop_config.h"
#include "device/include/device_iot_config.h"
#include "stack/include/bt_uuid16.h"
#include "stack/include/btm_sec_api_types.h"
#include "stack/include/l2c_api.h"
#include "stack/include/port_api.h"
#include "stack/include/sdp_api.h"
#include "storage/config_keys.h"
#include "types/raw_address.h"

using namespace bluetooth;
using namespace bluetooth::legacy::stack::sdp;

/*****************************************************************************
 *  Constants
 ****************************************************************************/

/* maximum length of data to read from RFCOMM */
#define BTA_AG_RFC_READ_MAX 512

/* maximum AT command length */
#define BTA_AG_CMD_MAX 512

/* SLC TIMER exception for IOT devices */
#define SLC_EXCEPTION_TIMEOUT_MS 10000

const uint16_t bta_ag_uuid[BTA_AG_NUM_IDX] = {
    UUID_SERVCLASS_HEADSET_AUDIO_GATEWAY, UUID_SERVCLASS_AG_HANDSFREE};

const uint8_t bta_ag_sec_id[BTA_AG_NUM_IDX] = {BTM_SEC_SERVICE_HEADSET_AG,
                                               BTM_SEC_SERVICE_AG_HANDSFREE};

const tBTA_SERVICE_ID bta_ag_svc_id[BTA_AG_NUM_IDX] = {BTA_HSP_SERVICE_ID,
                                                       BTA_HFP_SERVICE_ID};

const tBTA_SERVICE_MASK bta_ag_svc_mask[BTA_AG_NUM_IDX] = {
    BTA_HSP_SERVICE_MASK, BTA_HFP_SERVICE_MASK};

typedef void (*tBTA_AG_ATCMD_CBACK)(tBTA_AG_SCB* p_scb, uint16_t cmd,
                                    uint8_t arg_type, char* p_arg, char* p_end,
                                    int16_t int_arg);

const tBTA_AG_ATCMD_CBACK bta_ag_at_cback_tbl[BTA_AG_NUM_IDX] = {
    bta_ag_at_hsp_cback, bta_ag_at_hfp_cback};

/*******************************************************************************
 *
 * Function         bta_ag_cback_open
 *
 * Description      Send open callback event to application.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
static void bta_ag_cback_open(tBTA_AG_SCB* p_scb, const RawAddress& bd_addr,
                              tBTA_AG_STATUS status) {
  tBTA_AG_OPEN open = {};

  /* call app callback with open event */
  open.hdr.handle = bta_ag_scb_to_idx(p_scb);
  open.hdr.app_id = p_scb->app_id;
  open.status = status;
  open.service_id = bta_ag_svc_id[p_scb->conn_service];
  open.bd_addr = bd_addr;

  (*bta_ag_cb.p_cback)(BTA_AG_OPEN_EVT, (tBTA_AG*)&open);
}

/*******************************************************************************
 *
 * Function         bta_ag_register
 *
 * Description      This function initializes values of the AG cb and sets up
 *                  the SDP record for the services.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_register(tBTA_AG_SCB* p_scb, const tBTA_AG_DATA& data) {
  /* initialize control block */
  p_scb->reg_services = data.api_register.services;
  p_scb->features = data.api_register.features;
  p_scb->masked_features = data.api_register.features;
  p_scb->app_id = data.api_register.app_id;

  /* create SDP records */
  bta_ag_create_records(p_scb, data);

  /* start RFCOMM servers */
  bta_ag_start_servers(p_scb, p_scb->reg_services);

  /* call app callback with register event */
  tBTA_AG_REGISTER reg = {};
  reg.hdr.handle = bta_ag_scb_to_idx(p_scb);
  reg.hdr.app_id = p_scb->app_id;
  reg.status = BTA_AG_SUCCESS;
  (*bta_ag_cb.p_cback)(BTA_AG_REGISTER_EVT, (tBTA_AG*)&reg);
}

/*******************************************************************************
 *
 * Function         bta_ag_deregister
 *
 * Description      This function removes the sdp records, closes the RFCOMM
 *                  servers, and deallocates the service control block.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_deregister(tBTA_AG_SCB* p_scb, const tBTA_AG_DATA& data) {
  /* set dealloc */
  p_scb->dealloc = true;

  /* remove sdp records */
  bta_ag_del_records(p_scb);

  /* remove rfcomm servers */
  bta_ag_close_servers(p_scb, p_scb->reg_services);

  /* dealloc */
  bta_ag_scb_dealloc(p_scb);
}

/*******************************************************************************
 *
 * Function         bta_ag_start_dereg
 *
 * Description      Start a deregister event.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_start_dereg(tBTA_AG_SCB* p_scb, const tBTA_AG_DATA& data) {
  /* set dealloc */
  p_scb->dealloc = true;

  /* remove sdp records */
  bta_ag_del_records(p_scb);
}

/*******************************************************************************
 *
 * Function         bta_ag_start_open
 *
 * Description      This starts an AG open.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_start_open(tBTA_AG_SCB* p_scb, const tBTA_AG_DATA& data) {
  p_scb->peer_addr = data.api_open.bd_addr;
  p_scb->open_services = p_scb->reg_services;

  /* Check if RFCOMM has any incoming connection to avoid collision. */
  RawAddress pending_bd_addr = RawAddress::kEmpty;
  if (PORT_IsOpening(&pending_bd_addr)) {
    /* Let the incoming connection goes through.                        */
    /* Issue collision for this scb for now.                            */
    /* We will decide what to do when we find incoming connetion later. */
    bta_ag_collision_cback(BTA_SYS_CONN_OPEN, BTA_ID_AG, 0, p_scb->peer_addr);
    return;
  }

  /* close servers */
  bta_ag_close_servers(p_scb, p_scb->reg_services);

  /* set role */
  p_scb->role = BTA_AG_INT;

  /* do service search */
  bta_ag_do_disc(p_scb, p_scb->open_services);
}

/*******************************************************************************
 *
 * Function         bta_ag_disc_int_res
 *
 * Description      This function handles a discovery result when initiator.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_disc_int_res(tBTA_AG_SCB* p_scb, const tBTA_AG_DATA& data) {
  uint16_t event = BTA_AG_DISC_FAIL_EVT;

  log::verbose("bta_ag_disc_int_res: Status: {}", data.disc_result.status);

  /* if found service */
  if (data.disc_result.status == SDP_SUCCESS ||
      data.disc_result.status == SDP_DB_FULL) {
    /* get attributes */
    if (bta_ag_sdp_find_attr(p_scb, p_scb->open_services)) {
      /* set connected service */
      p_scb->conn_service = bta_ag_service_to_idx(p_scb->open_services);

      /* send ourselves sdp ok event */
      event = BTA_AG_DISC_OK_EVT;

      DEVICE_IOT_CONFIG_ADDR_SET_HEX_IF_GREATER(
          p_scb->peer_addr, IOT_CONF_KEY_HFP_VERSION, p_scb->peer_version,
          IOT_CONF_BYTE_NUM_2);
    }
  }

  /* free discovery db */
  bta_ag_free_db(p_scb, data);

  /* if service not found check if we should search for other service */
  if ((event == BTA_AG_DISC_FAIL_EVT) &&
      (data.disc_result.status == SDP_SUCCESS ||
       data.disc_result.status == SDP_DB_FULL ||
       data.disc_result.status == SDP_NO_RECS_MATCH)) {
    if ((p_scb->open_services & BTA_HFP_SERVICE_MASK) &&
        (p_scb->open_services & BTA_HSP_SERVICE_MASK)) {
      /* search for HSP */
      p_scb->open_services &= ~BTA_HFP_SERVICE_MASK;
      bta_ag_do_disc(p_scb, p_scb->open_services);
    } else if ((p_scb->open_services & BTA_HSP_SERVICE_MASK) &&
               (p_scb->hsp_version == HSP_VERSION_1_2)) {
      /* search for UUID_SERVCLASS_HEADSET instead */
      p_scb->hsp_version = HSP_VERSION_1_0;
      bta_ag_do_disc(p_scb, p_scb->open_services);
    } else {
      /* send ourselves sdp ok/fail event */
      bta_ag_sm_execute(p_scb, event, data);
    }
  } else {
    /* send ourselves sdp ok/fail event */
    bta_ag_sm_execute(p_scb, event, data);
  }
}

/*******************************************************************************
 *
 * Function         bta_ag_disc_acp_res
 *
 * Description      This function handles a discovery result when acceptor.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_disc_acp_res(tBTA_AG_SCB* p_scb, const tBTA_AG_DATA& data) {
  /* if found service */
  if (data.disc_result.status == SDP_SUCCESS ||
      data.disc_result.status == SDP_DB_FULL) {
    /* get attributes */
    bta_ag_sdp_find_attr(p_scb, bta_ag_svc_mask[p_scb->conn_service]);
    DEVICE_IOT_CONFIG_ADDR_SET_HEX_IF_GREATER(
        p_scb->peer_addr, IOT_CONF_KEY_HFP_VERSION, p_scb->peer_version,
        IOT_CONF_BYTE_NUM_2);
  }

  /* free discovery db */
  bta_ag_free_db(p_scb, data);
}

/*******************************************************************************
 *
 * Function         bta_ag_disc_fail
 *
 * Description      This function handles a discovery failure.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_disc_fail(tBTA_AG_SCB* p_scb, const tBTA_AG_DATA& /* data */) {
  /* reopen registered servers */
  bta_ag_start_servers(p_scb, p_scb->reg_services);

  /* reinitialize stuff */

  /* clear the remote BD address */
  RawAddress peer_addr = p_scb->peer_addr;
  p_scb->peer_addr = RawAddress::kEmpty;

  /* call open cback w. failure */
  bta_ag_cback_open(p_scb, peer_addr, BTA_AG_FAIL_SDP);
}

/*******************************************************************************
 *
 * Function         bta_ag_open_fail
 *
 * Description      open connection failed.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_open_fail(tBTA_AG_SCB* p_scb, const tBTA_AG_DATA& data) {
  /* call open cback w. failure */
  log::debug("state {}", bta_ag_state_str(p_scb->state));
  bta_ag_cback_open(p_scb, data.api_open.bd_addr, BTA_AG_FAIL_RESOURCES);
}

/*******************************************************************************
 *
 * Function         bta_ag_rfc_fail
 *
 * Description      RFCOMM connection failed.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_rfc_fail(tBTA_AG_SCB* p_scb, const tBTA_AG_DATA& /* data */) {
  log::info("reset p_scb with index={}", bta_ag_scb_to_idx(p_scb));
  RawAddress peer_addr = p_scb->peer_addr;
  /* reinitialize stuff */
  if (com::android::bluetooth::flags::reset_ag_state_on_collision()) {
    p_scb->state = BTA_AG_INIT_ST;
  }
  p_scb->conn_handle = 0;
  p_scb->conn_service = 0;
  p_scb->peer_features = 0;
  p_scb->peer_codecs = BTM_SCO_CODEC_CVSD;
  p_scb->sco_codec = BTM_SCO_CODEC_CVSD;
  p_scb->is_aptx_swb_codec = false;
  p_scb->role = 0;
  p_scb->svc_conn = false;
  p_scb->hsp_version = HSP_VERSION_1_2;
  /*Clear the BD address*/
  p_scb->peer_addr = RawAddress::kEmpty;

  /* reopen registered servers */
  bta_ag_start_servers(p_scb, p_scb->reg_services);

  /* call open cback w. failure */
  bta_ag_cback_open(p_scb, peer_addr, BTA_AG_FAIL_RFCOMM);
}

/*******************************************************************************
 *
 * Function         bta_ag_rfc_close
 *
 * Description      RFCOMM connection closed.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_rfc_close(tBTA_AG_SCB* p_scb, const tBTA_AG_DATA& /* data */) {
  tBTA_AG_CLOSE close = {};
  tBTA_SERVICE_MASK services;
  int i, num_active_conn = 0;

  /* reinitialize stuff */
  p_scb->conn_service = 0;
  p_scb->peer_features = 0;
  p_scb->masked_features = p_scb->features;
  p_scb->peer_codecs = BTM_SCO_CODEC_CVSD;
  p_scb->sco_codec = BTM_SCO_CODEC_CVSD;
  /* Clear these flags upon SLC teardown */
  p_scb->codec_updated = false;
  p_scb->codec_fallback = false;
  p_scb->trying_cvsd_safe_settings = false;
  p_scb->retransmission_effort_retries = 0;
  p_scb->codec_msbc_settings = BTA_AG_SCO_MSBC_SETTINGS_T2;
  p_scb->codec_cvsd_settings = BTA_AG_SCO_CVSD_SETTINGS_S4;
  p_scb->codec_aptx_settings = BTA_AG_SCO_APTX_SWB_SETTINGS_Q0;
  p_scb->is_aptx_swb_codec = false;
  p_scb->codec_lc3_settings = BTA_AG_SCO_LC3_SETTINGS_T2;
  p_scb->role = 0;
  p_scb->svc_conn = false;
  p_scb->hsp_version = HSP_VERSION_1_2;
  bta_ag_at_reinit(&p_scb->at_cb);

  for (auto& peer_hf_indicator : p_scb->peer_hf_indicators) {
    peer_hf_indicator = {};
  }
  for (auto& local_hf_indicator : p_scb->local_hf_indicators) {
    local_hf_indicator = {};
  }

  /* stop timers */
  alarm_cancel(p_scb->ring_timer);
  alarm_cancel(p_scb->codec_negotiation_timer);

  close.hdr.handle = bta_ag_scb_to_idx(p_scb);
  close.hdr.app_id = p_scb->app_id;
  close.bd_addr = p_scb->peer_addr;

  bta_sys_conn_close(BTA_ID_AG, p_scb->app_id, p_scb->peer_addr);

  if (bta_ag_get_active_device() == p_scb->peer_addr) {
    bta_clear_active_device();
  }

  /* call close cback */
  (*bta_ag_cb.p_cback)(BTA_AG_CLOSE_EVT, (tBTA_AG*)&close);

  /* if not deregistering (deallocating) reopen registered servers */
  if (!p_scb->dealloc) {
    /* Clear peer bd_addr so instance can be reused */
    p_scb->peer_addr = RawAddress::kEmpty;

    /* start only unopened server */
    services = p_scb->reg_services;
    for (i = 0; i < BTA_AG_NUM_IDX && services != 0; i++) {
      if (p_scb->serv_handle[i])
        services &= ~((tBTA_SERVICE_MASK)1 << (BTA_HSP_SERVICE_ID + i));
    }
    bta_ag_start_servers(p_scb, services);

    p_scb->conn_handle = 0;

    /* Make sure SCO state is BTA_AG_SCO_SHUTDOWN_ST */
    bta_ag_sco_shutdown(p_scb, tBTA_AG_DATA::kEmpty);

    /* Check if all the SLCs are down */
    for (i = 0; i < BTA_AG_MAX_NUM_CLIENTS; i++) {
      if (bta_ag_cb.scb[i].in_use && bta_ag_cb.scb[i].svc_conn)
        num_active_conn++;
    }

    if (!num_active_conn) {
      bta_sys_sco_unuse(BTA_ID_AG, p_scb->app_id, p_scb->peer_addr);
    }

  }
  /* else close port and deallocate scb */
  else {
    if (RFCOMM_RemoveServer(p_scb->conn_handle) != PORT_SUCCESS) {
      log::warn("Unable to remove RFCOMM server peer:{} handle:{}",
                p_scb->peer_addr, p_scb->conn_handle);
    };
    bta_ag_scb_dealloc(p_scb);
  }
}

/*******************************************************************************
 *
 * Function         bta_ag_rfc_open
 *
 * Description      Handle RFCOMM channel open.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_rfc_open(tBTA_AG_SCB* p_scb, const tBTA_AG_DATA& data) {
  int ag_conn_timeout = p_bta_ag_cfg->conn_tout;
  /* initialize AT feature variables */
  p_scb->clip_enabled = false;
  p_scb->ccwa_enabled = false;
  p_scb->cmer_enabled = false;
  p_scb->cmee_enabled = false;
  p_scb->inband_enabled =
      ((p_scb->features & BTA_AG_FEAT_INBAND) == BTA_AG_FEAT_INBAND);
  if (p_scb->conn_service == BTA_AG_HFP) {
    size_t version_value_size = sizeof(p_scb->peer_version);
    if (!btif_config_get_bin(
            p_scb->peer_addr.ToString(), BTIF_STORAGE_KEY_HFP_VERSION,
            (uint8_t*)&p_scb->peer_version, &version_value_size)) {
      log::warn("Failed read cached peer HFP version for {}", p_scb->peer_addr);
      p_scb->peer_version = HFP_HSP_VERSION_UNKNOWN;
    }
    size_t sdp_features_size = sizeof(p_scb->peer_sdp_features);
    if (btif_config_get_bin(
            p_scb->peer_addr.ToString(), BTIF_STORAGE_KEY_HFP_SDP_FEATURES,
            (uint8_t*)&p_scb->peer_sdp_features, &sdp_features_size)) {
      bool sdp_wbs_support = p_scb->peer_sdp_features & BTA_AG_FEAT_WBS_SUPPORT;
      if (!p_scb->received_at_bac && sdp_wbs_support) {
        p_scb->codec_updated = true;
        p_scb->peer_codecs = BTM_SCO_CODEC_CVSD | BTM_SCO_CODEC_MSBC;
        p_scb->sco_codec = BTM_SCO_CODEC_MSBC;
      }
      bool sdp_swb_support = p_scb->peer_sdp_features & BTA_AG_FEAT_SWB_SUPPORT;
      if (!p_scb->received_at_bac && sdp_swb_support) {
        p_scb->codec_updated = true;
        p_scb->peer_codecs |= BTM_SCO_CODEC_LC3;
        p_scb->sco_codec = BTM_SCO_CODEC_LC3;
      }
    } else {
      log::warn("Failed read cached peer HFP SDP features for {}",
                p_scb->peer_addr);
    }
  }

  /* set up AT command interpreter */
  p_scb->at_cb.p_at_tbl = bta_ag_at_tbl[p_scb->conn_service];
  p_scb->at_cb.p_cmd_cback = bta_ag_at_cback_tbl[p_scb->conn_service];
  p_scb->at_cb.p_err_cback = bta_ag_at_err_cback;
  p_scb->at_cb.p_user = p_scb;
  p_scb->at_cb.cmd_max_len = BTA_AG_CMD_MAX;
  bta_ag_at_init(&p_scb->at_cb);

  bta_sys_conn_open(BTA_ID_AG, p_scb->app_id, p_scb->peer_addr);

  bta_ag_cback_open(p_scb, p_scb->peer_addr, BTA_AG_SUCCESS);

  if (interop_match_addr(INTEROP_INCREASE_AG_CONN_TIMEOUT, &p_scb->peer_addr)) {
    /* use higher value for ag conn timeout */
    ag_conn_timeout = SLC_EXCEPTION_TIMEOUT_MS;
  }

  log::verbose("bta_ag_rfc_open: ag_conn_timeout: {}", ag_conn_timeout);
  if (p_scb->conn_service == BTA_AG_HFP) {
    /* if hfp start timer for service level conn */
    bta_sys_start_timer(p_scb->ring_timer,ag_conn_timeout,
                        BTA_AG_SVC_TIMEOUT_EVT, bta_ag_scb_to_idx(p_scb));
  } else {
    /* else service level conn is open */
    bta_ag_svc_conn_open(p_scb, data);
  }
}

/*******************************************************************************
 *
 * Function         bta_ag_rfc_acp_open
 *
 * Description      Handle RFCOMM channel open when accepting connection.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_rfc_acp_open(tBTA_AG_SCB* p_scb, const tBTA_AG_DATA& data) {
  log::verbose("serv_handle0 = {} serv_handle = {}", p_scb->serv_handle[0],
               p_scb->serv_handle[1]);
  /* set role */
  p_scb->role = BTA_AG_ACP;

  /* get bd addr of peer */
  uint16_t lcid = 0;
  uint16_t hfp_version = 0;
  RawAddress dev_addr = RawAddress::kEmpty;
  int status = PORT_CheckConnection(data.rfc.port_handle, &dev_addr, &lcid);
  if (status != PORT_SUCCESS) {
    log::error("PORT_CheckConnection returned {}", status);
    return;
  }

  /* Collision Handling */
  for (tBTA_AG_SCB& ag_scb : bta_ag_cb.scb) {
    // Cancel any pending collision timers
    if (ag_scb.in_use && alarm_is_scheduled(ag_scb.collision_timer)) {
      log::verbose("cancel collision alarm for {}", ag_scb.peer_addr);
      alarm_cancel(ag_scb.collision_timer);
      if (dev_addr != ag_scb.peer_addr && p_scb != &ag_scb) {
        // Resume outgoing connection if incoming is not on the same device
        bta_ag_resume_open(&ag_scb);
      }
    }
    if (dev_addr == ag_scb.peer_addr && p_scb != &ag_scb) {
      log::info(
          "close outgoing connection before accepting {} with conn_handle={}",
          ag_scb.peer_addr, ag_scb.conn_handle);
      if (!com::android::bluetooth::flags::close_rfcomm_instead_of_reset()) {
        // Fail the outgoing connection to clean up any upper layer states
        bta_ag_rfc_fail(&ag_scb, tBTA_AG_DATA::kEmpty);
      }
      // If client port is opened, close it, state machine will handle rfcomm
      // closed in opening state as failure and pass to upper layer
      if (ag_scb.conn_handle > 0) {
        if(!ag_scb.svc_conn){
           status = RFCOMM_RemoveConnection(ag_scb.conn_handle);
           if (status != PORT_SUCCESS) {
              log::warn(
                 "RFCOMM_RemoveConnection failed for {}, handle {}, error {}",
               dev_addr, ag_scb.conn_handle, status);
            }
         } else {
            log::warn(
                 "No call RFCOMM_RemoveConnection as SLC is on ");
         }
      }
      if (com::android::bluetooth::flags::reset_after_collision()) {
        // As no existing outgoing rfcomm connection, then manual reset current
        // state, and use the incoming one
        bta_ag_rfc_fail(&ag_scb, tBTA_AG_DATA::kEmpty);
      }
    }
    log::info("dev_addr={}, peer_addr={}, in_use={}, index={}", dev_addr,
              ag_scb.peer_addr, ag_scb.in_use, bta_ag_scb_to_idx(p_scb));
  }

  p_scb->peer_addr = dev_addr;

  /* determine connected service from port handle */
  for (uint8_t i = 0; i < BTA_AG_NUM_IDX; i++) {
    log::verbose(
        "bta_ag_rfc_acp_open: i = {} serv_handle = {} port_handle = {}", i,
        p_scb->serv_handle[i], data.rfc.port_handle);

    if (p_scb->serv_handle[i] == data.rfc.port_handle) {
      p_scb->conn_service = i;
      p_scb->conn_handle = data.rfc.port_handle;
      break;
    }
  }

  log::verbose("bta_ag_rfc_acp_open: conn_service = {} conn_handle = {}",
               p_scb->conn_service, p_scb->conn_handle);

  /* close any unopened server */
  bta_ag_close_servers(
      p_scb, (p_scb->reg_services & ~bta_ag_svc_mask[p_scb->conn_service]));

  size_t version_value_size = sizeof(hfp_version);
  bool get_version = btif_config_get_bin(
      p_scb->peer_addr.ToString(), BTIF_STORAGE_KEY_HFP_VERSION,
      (uint8_t*)&hfp_version, &version_value_size);

  if (p_scb->conn_service == BTA_AG_HFP && get_version) {
    DEVICE_IOT_CONFIG_ADDR_SET_HEX_IF_GREATER(p_scb->peer_addr,
                                              IOT_CONF_KEY_HFP_VERSION,
                                              hfp_version, IOT_CONF_BYTE_NUM_2);
  }
  /* do service discovery to get features */
  bta_ag_do_disc(p_scb, bta_ag_svc_mask[p_scb->conn_service]);

  /* continue with common open processing */
  bta_ag_rfc_open(p_scb, data);
}

/*******************************************************************************
 *
 * Function         bta_ag_rfc_data
 *
 * Description      Read and process data from RFCOMM.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_rfc_data(tBTA_AG_SCB* p_scb, const tBTA_AG_DATA& /* data */) {
  uint16_t len;
  char buf[BTA_AG_RFC_READ_MAX] = "";

  log::verbose("");

  /* do the following */
  for (;;) {
    /* read data from rfcomm; if bad status, we're done */
    if (PORT_ReadData(p_scb->conn_handle, buf, BTA_AG_RFC_READ_MAX, &len) !=
        PORT_SUCCESS) {
      log::error("failed to read data {}", p_scb->peer_addr);
      break;
    }

    /* if no data, we're done */
    if (len == 0) {
      log::warn("no data for {}", p_scb->peer_addr);
      break;
    }

    /* run AT command interpreter on data */
    bta_sys_busy(BTA_ID_AG, p_scb->app_id, p_scb->peer_addr);
    bta_ag_at_parse(&p_scb->at_cb, buf, len);
    if ((p_scb->sco_idx != BTM_INVALID_SCO_INDEX) &&
        bta_ag_sco_is_open(p_scb)) {
      log::verbose("change link policy for SCO");
      bta_sys_sco_open(BTA_ID_AG, p_scb->app_id, p_scb->peer_addr);
    } else {
      bta_sys_idle(BTA_ID_AG, p_scb->app_id, p_scb->peer_addr);
    }

    /* no more data to read, we're done */
    if (len < BTA_AG_RFC_READ_MAX) {
      break;
    }
  }
}

/*******************************************************************************
 *
 * Function         bta_ag_start_close
 *
 * Description      Start the process of closing SCO and RFCOMM connection.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_start_close(tBTA_AG_SCB* p_scb, const tBTA_AG_DATA& data) {
  /* Take the link out of sniff and set L2C idle time to 0 */
  bta_dm_pm_active(p_scb->peer_addr);
  if (p_scb->svc_conn){
    log::warn("SLC is up peer:{}", p_scb->peer_addr);
    if (!L2CA_SetIdleTimeoutByBdAddr(p_scb->peer_addr, 0, BT_TRANSPORT_BR_EDR)) {
      log::warn("Unable to set idle timeout peer:{}", p_scb->peer_addr);
    }
  }
  /* if SCO is open close SCO and wait on RFCOMM close */
  if (bta_ag_sco_is_open(p_scb)) {
    p_scb->post_sco = BTA_AG_POST_SCO_CLOSE_RFC;
  } else {
    p_scb->post_sco = BTA_AG_POST_SCO_NONE;
    bta_ag_rfc_do_close(p_scb, data);
  }

  /* always do SCO shutdown to handle all SCO corner cases */
  bta_ag_sco_shutdown(p_scb, data);
}

/*******************************************************************************
 *
 * Function         bta_ag_post_sco_open
 *
 * Description      Perform post-SCO open action, if any
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_post_sco_open(tBTA_AG_SCB* p_scb, const tBTA_AG_DATA& data) {
  switch (p_scb->post_sco) {
    case BTA_AG_POST_SCO_RING:
      bta_ag_send_ring(p_scb, data);
      p_scb->post_sco = BTA_AG_POST_SCO_NONE;
      break;

    case BTA_AG_POST_SCO_CALL_CONN:
      bta_ag_send_call_inds(p_scb, BTA_AG_IN_CALL_CONN_RES);
      p_scb->post_sco = BTA_AG_POST_SCO_NONE;
      break;

    default:
      break;
  }
}

/*******************************************************************************
 *
 * Function         bta_ag_post_sco_close
 *
 * Description      Perform post-SCO close action, if any
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_post_sco_close(tBTA_AG_SCB* p_scb, const tBTA_AG_DATA& data) {
  switch (p_scb->post_sco) {
    case BTA_AG_POST_SCO_CLOSE_RFC:
      bta_ag_rfc_do_close(p_scb, data);
      p_scb->post_sco = BTA_AG_POST_SCO_NONE;
      break;

    case BTA_AG_POST_SCO_CALL_CONN:
      bta_ag_send_call_inds(p_scb, BTA_AG_IN_CALL_CONN_RES);
      p_scb->post_sco = BTA_AG_POST_SCO_NONE;
      break;

    case BTA_AG_POST_SCO_CALL_ORIG:
      bta_ag_send_call_inds(p_scb, BTA_AG_OUT_CALL_ORIG_RES);
      p_scb->post_sco = BTA_AG_POST_SCO_NONE;
      break;

    case BTA_AG_POST_SCO_CALL_END:
      bta_ag_send_call_inds(p_scb, BTA_AG_END_CALL_RES);
      p_scb->post_sco = BTA_AG_POST_SCO_NONE;
      break;

    case BTA_AG_POST_SCO_CALL_END_INCALL:
      bta_ag_send_call_inds(p_scb, BTA_AG_END_CALL_RES);

      /* Sending callsetup IND and Ring were defered to after SCO close. */
      bta_ag_send_call_inds(p_scb, BTA_AG_IN_CALL_RES);

      if (bta_ag_inband_enabled(p_scb) &&
          !(p_scb->features & BTA_AG_FEAT_NOSCO)) {
        p_scb->post_sco = BTA_AG_POST_SCO_RING;
        if (!bta_ag_is_sco_open_allowed(p_scb,
                                        "BTA_AG_POST_SCO_CALL_END_INCALL")) {
          break;
        }
        if (bta_ag_is_sco_managed_by_audio()) {
          // let Audio HAL open the SCO
          break;
        }
        bta_ag_sco_open(p_scb, data);
      } else {
        p_scb->post_sco = BTA_AG_POST_SCO_NONE;
        bta_ag_send_ring(p_scb, data);
      }
      break;

    default:
      break;
  }
}

/*******************************************************************************
 *
 * Function         bta_ag_svc_conn_open
 *
 * Description      Service level connection opened
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_svc_conn_open(tBTA_AG_SCB* p_scb, const tBTA_AG_DATA& /* data */) {
  tBTA_AG_CONN evt = {};

  if (!p_scb->svc_conn) {
    /* set state variable */
    p_scb->svc_conn = true;

    /* Clear AT+BIA mask from previous SLC if any. */
    p_scb->bia_masked_out = 0;

    alarm_cancel(p_scb->ring_timer);

    /* call callback */
    evt.hdr.handle = bta_ag_scb_to_idx(p_scb);
    evt.hdr.app_id = p_scb->app_id;
    evt.peer_feat = p_scb->peer_features;
    evt.bd_addr = p_scb->peer_addr;
    evt.peer_codec = p_scb->peer_codecs;

    if ((p_scb->call_ind != BTA_AG_CALL_INACTIVE) ||
        (p_scb->callsetup_ind != BTA_AG_CALLSETUP_NONE)) {
      bta_sys_sco_use(BTA_ID_AG, p_scb->app_id, p_scb->peer_addr);
    }
    if (bta_ag_get_active_device().IsEmpty()) {
      bta_ag_api_set_active_device(p_scb->peer_addr);
    }
    (*bta_ag_cb.p_cback)(BTA_AG_CONN_EVT, (tBTA_AG*)&evt);
  }
}

/*******************************************************************************
 *
 * Function         bta_ag_setcodec
 *
 * Description      Handle API SetCodec
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_ag_setcodec(tBTA_AG_SCB* p_scb, const tBTA_AG_DATA& data) {
  tBTA_AG_PEER_CODEC codec_type = data.api_setcodec.codec;
  tBTA_AG_VAL val = {};
  const bool aptx_voice = is_hfp_aptx_voice_enabled() &&
                          (codec_type == BTA_AG_SCO_APTX_SWB_SETTINGS_Q0);
  log::verbose("aptx_voice={}, codec_type={:#x}", aptx_voice, codec_type);

  val.hdr.handle = bta_ag_scb_to_idx(p_scb);

  /* Check if the requested codec type is valid */
  if ((codec_type != BTM_SCO_CODEC_NONE) &&
      (codec_type != BTM_SCO_CODEC_CVSD) &&
      (codec_type != BTM_SCO_CODEC_MSBC) && (codec_type != BTM_SCO_CODEC_LC3) &&
      !aptx_voice) {
    val.num = codec_type;
    val.hdr.status = BTA_AG_FAIL_RESOURCES;
    log::error("bta_ag_setcodec error: unsupported codec type {}", codec_type);
    (*bta_ag_cb.p_cback)(BTA_AG_CODEC_EVT, (tBTA_AG*)&val);
    return;
  }

  if ((p_scb->peer_codecs & codec_type) || (codec_type == BTM_SCO_CODEC_NONE) ||
      (codec_type == BTM_SCO_CODEC_CVSD)) {
    p_scb->sco_codec = codec_type;
    p_scb->codec_updated = true;
    val.num = codec_type;
    val.hdr.status = BTA_AG_SUCCESS;
    log::verbose("bta_ag_setcodec: Updated codec type {}", codec_type);
  } else {
    val.num = codec_type;
    val.hdr.status = BTA_AG_FAIL_RESOURCES;
    log::error("bta_ag_setcodec error: unsupported codec type {}", codec_type);
  }

  (*bta_ag_cb.p_cback)(BTA_AG_CODEC_EVT, (tBTA_AG*)&val);
}

void bta_ag_handle_collision(tBTA_AG_SCB* p_scb,
                             const tBTA_AG_DATA& /* data */) {
  /* Cancel SDP if it had been started. */
  if (p_scb->p_disc_db) {
    if (!get_legacy_stack_sdp_api()->service.SDP_CancelServiceSearch(
            p_scb->p_disc_db)) {
      log::warn("Unable to cancel SDP service discovery search peer:{}",
                p_scb->peer_addr);
    }
    bta_ag_free_db(p_scb, tBTA_AG_DATA::kEmpty);
  }

  RawAddress peer_addr = p_scb->peer_addr;
  p_scb->peer_addr = RawAddress::kEmpty;
  log::verbose("sending RFCOMM fail event to btif for dev: {}",
            peer_addr);
  bta_ag_cback_open(p_scb, peer_addr, BTA_AG_FAIL_RFCOMM);

  /* reopen registered servers */
  /* Collision may be detected before or after we close servers. */
  if (bta_ag_is_server_closed(p_scb)) {
    bta_ag_start_servers(p_scb, p_scb->reg_services);
  }

  /* connection is retried from apps, no need for connection attempt again */
}
