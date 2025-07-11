/*
 * Copyright 2021 HIMSA II K/S - www.himsa.com. Represented by EHIMA -
 * www.ehima.com
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "state_machine.h"

#include <base/functional/bind.h>
#include <base/functional/callback.h>
#include <base/strings/string_number_conversions.h>
#include <bluetooth/log.h>
#include <com_android_bluetooth_flags.h>

#include "bta_gatt_queue.h"
#include "btm_iso_api.h"
#include "client_parser.h"
#include "codec_manager.h"
#include "common/strings.h"
#include "devices.h"
#include "hci/hci_packets.h"
#include "hcimsgs.h"
#include "internal_include/bt_trace.h"
#include "le_audio_health_status.h"
#include "le_audio_log_history.h"
#include "le_audio_types.h"
#include "os/log.h"
#include "osi/include/alarm.h"
#include "osi/include/osi.h"
#include "osi/include/properties.h"
#include "stack/include/hcimsgs.h"

// clang-format off
/* ASCS state machine 1.0
 *
 * State machine manages group of ASEs to make transition from one state to
 * another according to specification and keeping involved necessary externals
 * like: ISO, CIG, ISO data path, audio path form/to upper layer.
 *
 * GroupStream (API): GroupStream method of this le audio implementation class
 *                    object should allow transition from Idle (No Caching),
 *                    Codec Configured (Caching after release) state to
 *                    Streaming for all ASEs in group within time limit. Time
 *                    limit should keep safe whole state machine from being
 *                    stucked in any in-middle state, which is not a destination
 *                    state.
 *
 *                    TODO Second functionality of streaming should be switch
 *                    context which will base on previous state, context type.
 *
 * GroupStop (API): GroupStop method of this le audio implementation class
 *                  object should allow safe transition from any state to Idle
 *                  or Codec Configured (if caching supported).
 *
 * ╔══════════════════╦═════════════════════════════╦══════════════╦══════════════════╦══════╗
 * ║  Current State   ║ ASE Control Point Operation ║    Result    ║    Next State    ║ Note ║
 * ╠══════════════════╬═════════════════════════════╬══════════════╬══════════════════╬══════╣
 * ║ Idle             ║ Config Codec                ║ Success      ║ Codec Configured ║  +   ║
 * ║ Codec Configured ║ Config Codec                ║ Success      ║ Codec Configured ║  -   ║
 * ║ Codec Configured ║ Release                     ║ Success      ║ Releasing        ║  +   ║
 * ║ Codec Configured ║ Config QoS                  ║ Success      ║ QoS Configured   ║  +   ║
 * ║ QoS Configured   ║ Config Codec                ║ Success      ║ Codec Configured ║  -   ║
 * ║ QoS Configured   ║ Config QoS                  ║ Success      ║ QoS Configured   ║  -   ║
 * ║ QoS Configured   ║ Release                     ║ Success      ║ Releasing        ║  +   ║
 * ║ QoS Configured   ║ Enable                      ║ Success      ║ Enabling         ║  +   ║
 * ║ Enabling         ║ Release                     ║ Success      ║ Releasing        ║  +   ║
 * ║ Enabling         ║ Update Metadata             ║ Success      ║ Enabling         ║  -   ║
 * ║ Enabling         ║ Disable                     ║ Success      ║ Disabling        ║  -   ║
 * ║ Enabling         ║ Receiver Start Ready        ║ Success      ║ Streaming        ║  +   ║
 * ║ Streaming        ║ Update Metadata             ║ Success      ║ Streaming        ║  -   ║
 * ║ Streaming        ║ Disable                     ║ Success      ║ Disabling        ║  +   ║
 * ║ Streaming        ║ Release                     ║ Success      ║ Releasing        ║  +   ║
 * ║ Disabling        ║ Receiver Stop Ready         ║ Success      ║ QoS Configured   ║  +   ║
 * ║ Disabling        ║ Release                     ║ Success      ║ Releasing        ║  +   ║
 * ║ Releasing        ║ Released (no caching)       ║ Success      ║ Idle             ║  +   ║
 * ║ Releasing        ║ Released (caching)          ║ Success      ║ Codec Configured ║  -   ║
 * ╚══════════════════╩═════════════════════════════╩══════════════╩══════════════════╩══════╝
 *
 * + - supported transition
 * - - not supported
 */
// clang-format on

constexpr uint16_t  HCI_VS_QBCE_OCF                     = 0xFC51;

constexpr uint8_t  LTV_TYPE_VS_METADATA                 = 0xFF;
constexpr uint8_t  LTV_TYPE_VS_METADATA_FE              = 0xFE;

constexpr uint8_t  LTV_TYPE_MIN_FT                      = 0X00;
constexpr uint8_t  LTV_TYPE_MIN_BIT_RATE                = 0X01;
constexpr uint8_t  LTV_TYPE_MIN_MAX_ERROR_RESILIENCE    = 0X02;
constexpr uint8_t  LTV_TYPE_LATENCY_MODE                = 0X03;
constexpr uint8_t  LTV_TYPE_MAX_FT                      = 0X04;

constexpr uint8_t  LTV_LEN_MIN_FT                       = 0X01;
constexpr uint8_t  LTV_LEN_MIN_BIT_RATE                 = 0X01;
constexpr uint8_t  LTV_LEN_MIN_MAX_ERROR_RESILIENCE     = 0X01;
constexpr uint8_t  LTV_LEN_LATENCY_MODE                 = 0X01;
constexpr uint8_t  LTV_LEN_MAX_FT                       = 0X01;

constexpr uint8_t  ENCODER_LIMITS_SUB_OP                = 0x24;
constexpr uint8_t  HCI_VS_SET_CIG_CONTEXT_TYPE          = 0x3C;

typedef struct {
  uint8_t cig_id;
  uint8_t cis_id;
  std::vector<uint8_t> encoder_params;
  uint8_t encoder_mode;
} tBTM_BLE_SET_ENCODER_LIMITS_PARAM;

bool flag_sendenableLater = false;

bool isRecordReadable(uint8_t total_len, uint8_t processed_len,
                      uint8_t req_len) {
  LOG(WARNING) << __func__ << ": total_len: " << loghex(total_len)
                           << ", processed_len: " << loghex(processed_len)
                           << ", req_len: " << loghex(req_len);
  if((total_len > processed_len) &&
     ((total_len - processed_len) >= req_len)) {
    return true;
  } else {
    return false;
  }
}

uint8_t* PrepareSetEncoderLimitsPayload(tBTM_BLE_SET_ENCODER_LIMITS_PARAM *params,
                                        uint8_t *length, uint8_t *p) {
  uint8_t param_len = 0;
  uint8_t size = params->encoder_params.size();
  uint8_t num_limits = (size == 0) ? 1 : size;
  LOG(INFO) <<__func__  << "num_limits = "<<loghex(num_limits);
  LOG(INFO) <<__func__  << "params->cig_id = "<<loghex(params->cig_id);
  LOG(INFO) <<__func__  << "params->cis_id = "<<loghex(params->cis_id);
  UINT8_TO_STREAM(p, ENCODER_LIMITS_SUB_OP); //sub-opcode
  param_len++;
  UINT8_TO_STREAM(p, params->cig_id);
  param_len++;
  UINT8_TO_STREAM(p, params->cis_id);
  param_len++;
  UINT8_TO_STREAM(p, num_limits); //numlimits
  param_len++;
  UINT8_TO_STREAM(p, LTV_TYPE_MIN_FT);
  param_len++;
  UINT8_TO_STREAM(p, LTV_LEN_MIN_FT);
  param_len++;
  UINT8_TO_STREAM(p, params->encoder_params[0]);
  param_len++;
  UINT8_TO_STREAM(p, LTV_TYPE_MIN_BIT_RATE);
  param_len++;
  UINT8_TO_STREAM(p, LTV_LEN_MIN_BIT_RATE);
  param_len++;
  UINT8_TO_STREAM(p, params->encoder_params[2]);
  param_len++;
  UINT8_TO_STREAM(p, LTV_TYPE_MIN_MAX_ERROR_RESILIENCE);
  param_len++;
  UINT8_TO_STREAM(p, LTV_LEN_MIN_MAX_ERROR_RESILIENCE);
  param_len++;
  UINT8_TO_STREAM(p, params->encoder_params[3]);
  param_len++;
  UINT8_TO_STREAM(p, LTV_TYPE_LATENCY_MODE);
  param_len++;
  UINT8_TO_STREAM(p, LTV_LEN_LATENCY_MODE);
  param_len++;
  UINT8_TO_STREAM(p, params->encoder_params[4]);
  param_len++;
  UINT8_TO_STREAM(p, LTV_TYPE_MAX_FT);
  param_len++;
  UINT8_TO_STREAM(p, LTV_LEN_MAX_FT);
  param_len++;
  UINT8_TO_STREAM(p, params->encoder_params[1]);
  param_len++;
  *length = param_len;
  LOG(INFO) <<__func__  << "param_len = "<<loghex(param_len);
  return p;
}

void UpdateEncoderParams(uint8_t cig_id, uint8_t cis_id,
                                 std::vector<uint8_t> encoder_limit_params,
                                 uint8_t encoder_mode) {
  tBTM_BLE_SET_ENCODER_LIMITS_PARAM encoder_params = {
                                       .cig_id = cig_id,
                                       .cis_id = cis_id,
                                       .encoder_params = encoder_limit_params,
                                       .encoder_mode = encoder_mode};
    uint8_t length = 0;
    uint8_t size = 1;
    if (encoder_params.encoder_params.size())
      size = encoder_params.encoder_params.size();
    uint16_t len = 4 + size * 3;
    LOG(INFO) <<__func__  << "len = "<<loghex(len);
    uint8_t param_arr[len];
    uint8_t *param = param_arr;
    PrepareSetEncoderLimitsPayload(&encoder_params, &length, param);
    bluetooth::legacy::hci::GetInterface().SendVendorSpecificCmd(HCI_VS_QBCE_OCF, length, param, NULL);
}

void send_vs_cmd(const uint16_t content_type, const uint8_t cig_id, const uint8_t cis_cnt,
  const std::vector<uint16_t> cis_conn_handles, bool remote_support) {
  if (osi_property_get_bool("persist.vendor.service.bt.adv_transport", false) && remote_support) {
    std::vector<uint8_t> param;
    param.push_back(HCI_VS_SET_CIG_CONTEXT_TYPE);
    param.push_back(cig_id);
    param.push_back(content_type & 0x00FF);
    param.push_back((content_type & 0xFF00) >> 8);
    param.push_back(cis_cnt);
    for (auto& cis_handle: cis_conn_handles) {
      param.push_back(cis_handle & 0x00FF);
      param.push_back((cis_handle & 0xFF00) >> 8);
    }
    bluetooth::legacy::hci::GetInterface().SendVendorSpecificCmd(HCI_VS_QBCE_OCF,
        param.size(), param.data(), NULL);
  }
}

using bluetooth::common::ToString;
using bluetooth::hci::IsoManager;
using bluetooth::legacy::hci::GetInterface;
using bluetooth::le_audio::CodecManager;
using bluetooth::le_audio::GroupStreamStatus;
using bluetooth::le_audio::LeAudioDevice;
using bluetooth::le_audio::LeAudioDeviceGroup;
using bluetooth::le_audio::LeAudioGroupStateMachine;

using bluetooth::hci::ErrorCode;
using bluetooth::hci::ErrorCodeText;
using bluetooth::le_audio::DsaMode;
using bluetooth::le_audio::DsaModes;
using bluetooth::le_audio::types::ase;
using bluetooth::le_audio::types::AseState;
using bluetooth::le_audio::types::AudioContexts;
using bluetooth::le_audio::types::BidirectionalPair;
using bluetooth::le_audio::types::CigState;
using bluetooth::le_audio::types::CisState;
using bluetooth::le_audio::types::CodecLocation;
using bluetooth::le_audio::types::DataPathState;
using bluetooth::le_audio::types::LeAudioContextType;
using bluetooth::le_audio::types::LeAudioCoreCodecConfig;

void parseVSMetadata(uint8_t total_len, std::vector<uint8_t>& metadata,
                     uint8_t cig_id, uint8_t cis_id, struct ase* ase) {
  LOG(INFO) << __func__ ;
  const uint8_t* p = metadata.data();
  uint8_t ltv_len, ltv_type;
  uint8_t processed_len = 0;
  std::vector<uint8_t> vs_meta_data;
  uint8_t meta_data_len = total_len;

  while(meta_data_len > 0) {
    STREAM_TO_UINT8(ltv_len, p);
    processed_len++;

    if(!ltv_len || !isRecordReadable(total_len, processed_len, ltv_len)) {
      LOG(INFO) << __func__ << ": Data is not readable ";
      break;
    }

    STREAM_TO_UINT8(ltv_type, p);
    processed_len++;

    if(ltv_type == LTV_TYPE_VS_METADATA) {
      uint16_t company_id;
      uint8_t vs_meta_data_len, vs_meta_data_type;
      STREAM_TO_UINT16(company_id, p);
      LOG(INFO) << ": company_id = " << loghex(company_id);
      processed_len += 2;
      ltv_len -= 3; //company id and ltv_type
      while(ltv_len > 0) {
        STREAM_TO_UINT8(vs_meta_data_len, p);
        LOG(INFO) << __func__ << ": vs_meta_data_len = " << loghex(vs_meta_data_len);
        processed_len++;

        if(!vs_meta_data_len || !isRecordReadable(total_len, processed_len, vs_meta_data_len)) {
          LOG(INFO) << __func__ << ": Data is not readable ";
          break;
        }

        STREAM_TO_UINT8(vs_meta_data_type, p);
        LOG(INFO) << __func__ << ": vs_meta_data_type = " << loghex(vs_meta_data_type);
        processed_len++;
        if(vs_meta_data_type == LTV_TYPE_VS_METADATA_FE) {
          vs_meta_data.resize(vs_meta_data_len - 1);
          STREAM_TO_ARRAY(vs_meta_data.data(), p, vs_meta_data_len - 1); // "ltv_len - 1" because 1B for type
          LOG(INFO) << __func__ << ": STREAM_TO_ARRAY done ";
          processed_len += vs_meta_data_len - 1;
          if (ase->state == AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
            LOG(INFO) << __func__ << ": straight away call UpdateEncoderParams ";
            UpdateEncoderParams(cig_id, cis_id, vs_meta_data, 0xFF);
          } else {
            LOG(INFO) << __func__ << ": Cache it untill encoder is up ";
            ase->vs_metadata = vs_meta_data;
            ase->is_vsmetadata_available = true;
          }
        } else {
          (p) += (vs_meta_data_len - 1); //just ignore and increase pointer
          processed_len += (vs_meta_data_len - 1);
        }
        ltv_len -= (vs_meta_data_len + 1);
      }
    } else {
      (p) += (ltv_len - 1);
    }
    meta_data_len -= (ltv_len + 1);
  }
}

std::map<uint32_t, uint8_t> freq_to_ltv_map = {
     {8000,     bluetooth::le_audio::codec_spec_conf::kLeAudioSamplingFreq8000Hz},
     {11025,    bluetooth::le_audio::codec_spec_conf::kLeAudioSamplingFreq11025Hz},
     {16000,    bluetooth::le_audio::codec_spec_conf::kLeAudioSamplingFreq16000Hz},
     {22050,    bluetooth::le_audio::codec_spec_conf::kLeAudioSamplingFreq22050Hz},
     {24000,    bluetooth::le_audio::codec_spec_conf::kLeAudioSamplingFreq24000Hz},
     {32000,    bluetooth::le_audio::codec_spec_conf::kLeAudioSamplingFreq32000Hz},
     {44100,    bluetooth::le_audio::codec_spec_conf::kLeAudioSamplingFreq44100Hz},
     {48000,    bluetooth::le_audio::codec_spec_conf::kLeAudioSamplingFreq48000Hz},
     {88200,    bluetooth::le_audio::codec_spec_conf::kLeAudioSamplingFreq88200Hz},
     {96000,    bluetooth::le_audio::codec_spec_conf::kLeAudioSamplingFreq96000Hz},
     {176400,   bluetooth::le_audio::codec_spec_conf::kLeAudioSamplingFreq176400Hz},
     {192000,   bluetooth::le_audio::codec_spec_conf::kLeAudioSamplingFreq192000Hz},
     {384000,   bluetooth::le_audio::codec_spec_conf::kLeAudioSamplingFreq384000Hz},
};

constexpr int kIsoDataPathPlatformDefault = 0x01;

namespace {

using namespace bluetooth;

constexpr int linkQualityCheckInterval = 4000;
constexpr int kAutonomousTransitionTimeoutMs = 5000;
constexpr int kNumberOfCisRetries = 2;

static void link_quality_cb(void* data) {
  // very ugly, but we need to pass just two bytes
  uint16_t cis_conn_handle = *((uint16_t*)data);

  IsoManager::GetInstance()->ReadIsoLinkQuality(cis_conn_handle);
}

class LeAudioGroupStateMachineImpl;
LeAudioGroupStateMachineImpl* instance;

class LeAudioGroupStateMachineImpl : public LeAudioGroupStateMachine {
 public:
  LeAudioGroupStateMachineImpl(Callbacks* state_machine_callbacks_)
      : state_machine_callbacks_(state_machine_callbacks_),
        watchdog_(alarm_new("LeAudioStateMachineTimer")) {
    log_history_ = LeAudioLogHistory::Get();
  }

  ~LeAudioGroupStateMachineImpl() {
    alarm_free(watchdog_);
    watchdog_ = nullptr;
    log_history_->Cleanup();
    log_history_ = nullptr;
  }

  bool AttachToStream(LeAudioDeviceGroup* group, LeAudioDevice* leAudioDevice,
                      BidirectionalPair<std::vector<uint8_t>> ccids) override {
    log::info("group id: {} device: {}", group->group_id_,
              leAudioDevice->address_);

    /* This function is used to attach the device to the stream.
     * Limitation here is that device should be previously in the streaming
     * group and just got reconnected.
     */
    if (group->GetState() != AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING ||
        group->GetTargetState() != AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
      log::error(
          "group {} no in correct streaming state: {} or target state: {}",
          group->group_id_, ToString(group->GetState()),
          ToString(group->GetTargetState()));
      return false;
    }

    /*Return false if device is not CONNECTED, it will
     *retry AttachToStream after kDeviceAttachDelayMs.
     */
    if (leAudioDevice->GetConnectionState() !=
                       bluetooth::le_audio::DeviceConnectState::CONNECTED) {
      log::error("device: {}, in the state: {}", leAudioDevice->address_,
          bluetooth::common::ToString(leAudioDevice->GetConnectionState()));
      return false;
    }

    /* This is cautious - mostly needed for unit test only */
    auto group_metadata_contexts =
        get_bidirectional(group->GetMetadataContexts());
    auto device_available_contexts = leAudioDevice->GetAvailableContexts();
    if (!group_metadata_contexts.test_any(device_available_contexts)) {
      log::info("{} does is not have required context type",
                leAudioDevice->address_);
      return false;
    }

    /* Invalidate configuration to make sure it is chosen properly when new
     * member connects
     */
    group->InvalidateCachedConfigurations();

    if (!group->Configure(group->GetConfigurationContextType(),
                          group->GetMetadataContexts(), ccids)) {
      log::error("failed to set ASE configuration");
      return false;
    }

    PrepareAndSendCodecConfigure(group, leAudioDevice);
    return true;
  }

  bool StartStream(
      LeAudioDeviceGroup* group, LeAudioContextType context_type,
      const BidirectionalPair<AudioContexts>& metadata_context_types,
      BidirectionalPair<std::vector<uint8_t>> ccid_lists) override {
    log::info("current state: {}", ToString(group->GetState()));

    switch (group->GetState()) {
      case AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED:
        if (group->IsConfiguredForContext(context_type)) {
          if (group->Activate(context_type, metadata_context_types,
                              ccid_lists)) {
            SetTargetState(group, AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING);

            if (CigCreate(group)) {
              return true;
            }
          }
          log::info("Could not activate device, try to configure it again");
        }

        /* Deactivate previousely activated ASEs in case if there were just a
         * reconfiguration (group target state as CODEC CONFIGURED) and no
         * deactivation. Currently activated ASEs cannot be used for different
         * context.
         */
        group->Deactivate();

        /* We are going to reconfigure whole group. Clear Cises.*/
        ReleaseCisIds(group);

        /* If configuration is needed */
        FALLTHROUGH_INTENDED;
      case AseState::BTA_LE_AUDIO_ASE_STATE_IDLE:
        if (!group->Configure(context_type, metadata_context_types,
                              ccid_lists)) {
          log::error("failed to set ASE configuration");
          return false;
        }

        group->cig.GenerateCisIds(context_type);
        /* All ASEs should aim to achieve target state */
        SetTargetState(group, AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING);
        if (!PrepareAndSendCodecConfigToTheGroup(group)) {
          group->PrintDebugState();
          ClearGroup(group, true);
          return false;
        }
        break;

      case AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED: {
        LeAudioDevice* leAudioDevice = group->GetFirstActiveDevice();
        if (!leAudioDevice) {
          log::error("group has no active devices");
          return false;
        }

        /* All ASEs should aim to achieve target state */
        SetTargetState(group, AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING);
        PrepareAndSendEnableToTheGroup(group);
        break;
      }

      case AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING: {
        /* This case just updates the metadata for the stream, in case
         * stream configuration is satisfied. We can do that already for
         * all the devices in a group, without any state transitions.
         */
        static int count = 0;
        if (!group->IsMetadataChanged(metadata_context_types, ccid_lists))
          return true;

        LeAudioDevice* leAudioDevice = group->GetFirstActiveDevice();
        if (!leAudioDevice) {
          log::error("group has no active devices");
          return false;
        }
        if (count == 0) {
           if (osi_property_get_bool("persist.bluetooth.leaudio.bap_enableQoS", false)) {
              log::error("One moved to streaming, processing the other one");
              PrepareAndSendEnable(leAudioDevice);
              count = 1;
           }
        }
        while (leAudioDevice) {
          PrepareAndSendUpdateMetadata(leAudioDevice, metadata_context_types,
                                       ccid_lists);
          leAudioDevice = group->GetNextActiveDevice(leAudioDevice);
        }
        break;
      }

      default:
        log::error("Unable to transit from {}", ToString(group->GetState()));
        return false;
    }

    return true;
  }

  bool ConfigureStream(
      LeAudioDeviceGroup* group, LeAudioContextType context_type,
      const BidirectionalPair<AudioContexts>& metadata_context_types,
      BidirectionalPair<std::vector<uint8_t>> ccid_lists) override {
    if (group->GetState() > AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED) {
      log::error(
          "Stream should be stopped or in configured stream. Current state: {}",
          ToString(group->GetState()));
      return false;
    }

    group->Deactivate();
    ReleaseCisIds(group);

    if (!group->Configure(context_type, metadata_context_types, ccid_lists)) {
      log::error("Could not configure ASEs for group {} content type {}",
                 group->group_id_, int(context_type));

      return false;
    }

    group->cig.GenerateCisIds(context_type);
    SetTargetState(group, AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED);
    return PrepareAndSendCodecConfigToTheGroup(group);
  }

  void SuspendStream(LeAudioDeviceGroup* group) override {
    /* All ASEs should aim to achieve target state */
    SetTargetState(group, AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED);
    auto status = PrepareAndSendDisableToTheGroup(group);
    state_machine_callbacks_->StatusReportCb(group->group_id_, status);
  }

  void StopStream(LeAudioDeviceGroup* group) override {
    if (group->IsReleasingOrIdle()) {
      log::info("group: {} already in releasing process", group->group_id_);
      return;
    }

    /* All Ases should aim to achieve target state */
    SetTargetState(group, AseState::BTA_LE_AUDIO_ASE_STATE_IDLE);

    auto status = PrepareAndSendReleaseToTheGroup(group);
    state_machine_callbacks_->StatusReportCb(group->group_id_, status);
  }

  void notifyLeAudioHealth(
      LeAudioDeviceGroup* group,
      bluetooth::le_audio::LeAudioHealthGroupStatType stat) {
    if (!com::android::bluetooth::flags::
            leaudio_enable_health_based_actions()) {
      return;
    }

    auto leAudioHealthStatus = bluetooth::le_audio::LeAudioHealthStatus::Get();
    if (leAudioHealthStatus) {
      leAudioHealthStatus->AddStatisticForGroup(group, stat);
    }
  }

  void ProcessGattCtpNotification(LeAudioDeviceGroup* group, uint8_t* value,
                                  uint16_t len) {
    auto ntf = std::make_unique<
        struct bluetooth::le_audio::client_parser::ascs::ctp_ntf>();

    bool valid_notification = ParseAseCtpNotification(*ntf, len, value);
    if (group == nullptr) {
      log::warn("Notification received to invalid group");
      return;
    }

    /* State machine looks on ASE state and base on it take decisions.
     * If ASE state is not achieve on time, timeout is reported and upper
     * layer mostlikely drops ACL considers that remote is in bad state.
     * However, it might happen that remote device rejects ASE configuration for
     * some reason and ASCS specification defines tones of different reasons.
     * Maybe in the future we will be able to handle all of them but for now it
     * seems to be important to allow remote device to reject ASE configuration
     * when stream is creating. e.g. Allow remote to reject Enable on unwanted
     * context type.
     */

    auto target_state = group->GetTargetState();
    auto in_transition = group->IsInTransition();
    if (!in_transition ||
        target_state != AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
      log::debug(
          "Not interested in ctp result for group {} inTransistion: {} , "
          "targetState: {}",
          group->group_id_, in_transition, ToString(target_state));
      return;
    }

    if (!valid_notification) {
      /* Do nothing, just allow guard timer to fire */
      log::error("Invalid CTP notification for group {}", group->group_id_);
      return;
    }

    for (auto& entry : ntf->entries) {
      if (entry.response_code !=
          bluetooth::le_audio::client_parser::ascs::kCtpResponseCodeSuccess) {
        /* Gracefully stop the stream */
        log::error(
            "Stoping stream due to control point error for ase: {}, error: "
            "0x{:02x}, reason: 0x{:02x}",
            entry.ase_id, entry.response_code, entry.reason);

        notifyLeAudioHealth(group,
                            bluetooth::le_audio::LeAudioHealthGroupStatType::
                                STREAM_CREATE_SIGNALING_FAILED);
        StopStream(group);
        return;
      }
    }

    log::debug("Ctp result OK for group {} inTransistion: {} , targetState: {}",
               group->group_id_, in_transition, ToString(target_state));
  }

  void ProcessGattNotifEvent(uint8_t* value, uint16_t len, struct ase* ase,
                             LeAudioDevice* leAudioDevice,
                             LeAudioDeviceGroup* group) override {
    struct bluetooth::le_audio::client_parser::ascs::ase_rsp_hdr arh;

    ParseAseStatusHeader(arh, len, value);

    if (ase->id == 0x00) {
      /* Initial state of Ase - update id */
      log::info(", discovered ase id: {}", arh.id);
      ase->id = arh.id;
    }

    auto state = static_cast<AseState>(arh.state);

    log::info("{} , ASE id: {}, state changed {} -> {}",
              leAudioDevice->address_, ase->id, ToString(ase->state),
              ToString(state));

    log_history_->AddLogHistory(
        kLogAseStateNotif, leAudioDevice->group_id_, leAudioDevice->address_,
        "ASE_ID " + std::to_string(arh.id) + ": " + ToString(state),
        "curr: " + ToString(ase->state));

    switch (state) {
      case AseState::BTA_LE_AUDIO_ASE_STATE_IDLE:
        AseStateMachineProcessIdle(arh, ase, group, leAudioDevice);
        break;
      case AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED:
        AseStateMachineProcessCodecConfigured(
            arh, ase,
            value + bluetooth::le_audio::client_parser::ascs::kAseRspHdrMinLen,
            len - bluetooth::le_audio::client_parser::ascs::kAseRspHdrMinLen,
            group, leAudioDevice);
        break;
      case AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED:
        AseStateMachineProcessQosConfigured(arh, ase, group, leAudioDevice);
        break;
      case AseState::BTA_LE_AUDIO_ASE_STATE_ENABLING:
        AseStateMachineProcessEnabling(
            arh, ase, value + le_audio::client_parser::ascs::kAseRspHdrMinLen,
            len - le_audio::client_parser::ascs::kAseRspHdrMinLen, group,
            leAudioDevice);
        break;
      case AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING:
        AseStateMachineProcessStreaming(
            arh, ase,
            value + bluetooth::le_audio::client_parser::ascs::kAseRspHdrMinLen,
            len - bluetooth::le_audio::client_parser::ascs::kAseRspHdrMinLen,
            group, leAudioDevice);
        break;
      case AseState::BTA_LE_AUDIO_ASE_STATE_DISABLING:
        AseStateMachineProcessDisabling(arh, ase, group, leAudioDevice);
        break;
      case AseState::BTA_LE_AUDIO_ASE_STATE_RELEASING:
        AseStateMachineProcessReleasing(arh, ase, group, leAudioDevice);
        break;
      default:
        log::error("Wrong AES status: {}", static_cast<int>(arh.state));
        StopStream(group);
        break;
    }
  }

  void ProcessHciNotifOnCigCreate(LeAudioDeviceGroup* group, uint8_t status,
                                  uint8_t cig_id,
                                  std::vector<uint16_t> conn_handles) override {
    /* TODO: What if not all cises will be configured ?
     * conn_handle.size() != active ases in group
     */

    if (!group) {
      log::error(", group is null");
      return;
    }

    log_history_->AddLogHistory(kLogHciEvent, group->group_id_,
                                RawAddress::kEmpty,
                                kLogCisCreateOp + "STATUS=" + loghex(status));

    if (status != HCI_SUCCESS) {
      if (status == HCI_ERR_COMMAND_DISALLOWED) {
        /*
         * We are here, because stack has no chance to remove CIG when it was
         * shut during streaming. In the same time, controller probably was not
         * Reseted, which creates the issue. Lets remove CIG and try to create
         * it again.
         */
        group->cig.SetState(CigState::RECOVERING);
        IsoManager::GetInstance()->RemoveCig(group->group_id_, true);
        return;
      }

      group->cig.SetState(CigState::NONE);
      log::error(", failed to create CIG, reason: 0x{:02x}, new cig state: {}",
                 status, ToString(group->cig.GetState()));
      StopStream(group);
      return;
    }

    log::assert_that(group->cig.GetState() == CigState::CREATING,
                     "Unexpected CIG creation group id: {}, cig state: {}",
                     group->group_id_, ToString(group->cig.GetState()));

    group->cig.SetState(CigState::CREATED);
    log::info("Group: {}, id: {} cig state: {}, number of cis handles: {}",
              fmt::ptr(group), group->group_id_,
              ToString(group->cig.GetState()),
              static_cast<int>(conn_handles.size()));

    if (group->GetTargetState() != AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
      /* Group is not going to stream. It happen while CIG was creating.
       * Remove CIG in such a case
       */
      log::warn("group_id {} is not going to stream anymore. Remove CIG.",
                group->group_id_);
      group->PrintDebugState();
      RemoveCigForGroup(group);
      return;
    }

    /* Assign all connection handles to CIS ids of the CIG */
    group->cig.AssignCisConnHandles(conn_handles);

    /* Assign all connection handles to multiple device ASEs */
    group->AssignCisConnHandlesToAses();

    /* Last node configured, process group to codec configured state */
    group->SetState(AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED);

    /* As streaming is about to start send HCI configure data path
      * based on codec selected before CIS creation ensuring order
      * Connected_Iso_Group_Create -> HCI_Configure Data path ->
      * Create CIS -> SetUp ISO Datapath
      * (need to ensure HCI Datapath is configure just after group creation
      * and before CIS/ISO Data path creation so that vendor offload path
      *  is configured properly between ADSP and BT Controller
      */
    bool config_host_to_controller_sent = false;
    bool config_controller_to_host_sent = false;
    /* Above flags to ensure HCI Config data path sent only once for Tx only
      * OR Rx only OR Tx/Rx both based on cis type direction
      */
    for (struct bluetooth::le_audio::types::cis& cis : group->cig.cises) {
      if (cis.type == bluetooth::le_audio::types::CisType::CIS_TYPE_BIDIRECTIONAL) {
        if (!config_host_to_controller_sent) {
          std::vector<uint8_t> vendor_config_sink = PrepareVendorConfigPayloadData(
              group, conn_handles, bluetooth::le_audio::types::kLeAudioDirectionSink);
          GetInterface().ConfigureDataPath(hci_data_direction_t::HOST_TO_CONTROLLER,
                                kIsoDataPathPlatformDefault, vendor_config_sink);
          config_host_to_controller_sent = true;
        }
        if (!config_controller_to_host_sent) {
          std::vector<uint8_t> vendor_config_source = PrepareVendorConfigPayloadData(
              group, conn_handles, bluetooth::le_audio::types::kLeAudioDirectionSource);
          GetInterface().ConfigureDataPath(hci_data_direction_t::CONTROLLER_TO_HOST,
                                kIsoDataPathPlatformDefault, vendor_config_source);
          config_controller_to_host_sent = true;
        }
      } else if (cis.type == bluetooth::le_audio::types::CisType::CIS_TYPE_UNIDIRECTIONAL_SINK) {
        if (config_host_to_controller_sent) continue;
        std::vector<uint8_t> vendor_config_sink = PrepareVendorConfigPayloadData(
            group, conn_handles, bluetooth::le_audio::types::kLeAudioDirectionSink);
        GetInterface().ConfigureDataPath(hci_data_direction_t::HOST_TO_CONTROLLER,
                              kIsoDataPathPlatformDefault, vendor_config_sink);
        config_host_to_controller_sent = true;
      } else {
        if (config_controller_to_host_sent) continue;
        std::vector<uint8_t> vendor_config_source = PrepareVendorConfigPayloadData(
            group, conn_handles, bluetooth::le_audio::types::kLeAudioDirectionSource);
        GetInterface().ConfigureDataPath(hci_data_direction_t::CONTROLLER_TO_HOST,
                              kIsoDataPathPlatformDefault, vendor_config_source);
        config_controller_to_host_sent = false;
      }
    }
    send_vs_cmd(static_cast<uint16_t>(group->GetConfigurationContextType()),
        cig_id, group->cig.cises.size(), conn_handles, group->IsLeXDevice());

    PrepareAndSendQoSToTheGroup(group);
  }

  void FreeLinkQualityReports(LeAudioDevice* leAudioDevice) {
    if (leAudioDevice->link_quality_timer == nullptr) return;

    alarm_free(leAudioDevice->link_quality_timer);
    leAudioDevice->link_quality_timer = nullptr;
  }

  void ProcessHciNotifyOnCigRemoveRecovering(uint8_t status,
                                             LeAudioDeviceGroup* group) {
    group->cig.SetState(CigState::NONE);

    log_history_->AddLogHistory(kLogHciEvent, group->group_id_,
                                RawAddress::kEmpty,
                                kLogCigRemoveOp + " STATUS=" + loghex(status));
    if (status != HCI_SUCCESS) {
      log::error(
          "Could not recover from the COMMAND DISALLOAD on CigCreate. Status "
          "on CIG remove is 0x{:02x}",
          status);
      StopStream(group);
      return;
    }
    log::info("Succeed on CIG Recover - back to creating CIG");
    if (!CigCreate(group)) {
      log::error("Could not create CIG. Stop the stream for group {}",
                 group->group_id_);
      StopStream(group);
    }
  }

  void ProcessHciNotifOnCigRemove(uint8_t status,
                                  LeAudioDeviceGroup* group) override {
    if (group->cig.GetState() == CigState::RECOVERING) {
      ProcessHciNotifyOnCigRemoveRecovering(status, group);
      return;
    }

    log_history_->AddLogHistory(kLogHciEvent, group->group_id_,
                                RawAddress::kEmpty,
                                kLogCigRemoveOp + " STATUS=" + loghex(status));

    if (status != HCI_SUCCESS) {
      group->cig.SetState(CigState::CREATED);
      log::error(
          "failed to remove cig, id: {}, status 0x{:02x}, new cig state: {}",
          group->group_id_, status, ToString(group->cig.GetState()));
      return;
    }

    log::assert_that(group->cig.GetState() == CigState::REMOVING,
                     "Unexpected CIG remove group id: {}, cig state {}",
                     group->group_id_, ToString(group->cig.GetState()));

    group->cig.SetState(CigState::NONE);

    LeAudioDevice* leAudioDevice = group->GetFirstDevice();
    if (!leAudioDevice) return;

    do {
      FreeLinkQualityReports(leAudioDevice);

      for (auto& ase : leAudioDevice->ases_) {
        ase.cis_state = CisState::IDLE;
        ase.data_path_state = DataPathState::IDLE;
      }
    } while ((leAudioDevice = group->GetNextDevice(leAudioDevice)));
  }

  void ProcessHciNotifSetupIsoDataPath(LeAudioDeviceGroup* group,
                                       LeAudioDevice* leAudioDevice,
                                       uint8_t status,
                                       uint16_t conn_handle) override {
    log_history_->AddLogHistory(
        kLogHciEvent, group->group_id_, leAudioDevice->address_,
        kLogSetDataPathOp + "cis_h:" + loghex(conn_handle) +
            " STATUS=" + loghex(status));

    if (status) {
      log::error("failed to setup data path");
      StopStream(group);

      return;
    }

    if (com::android::bluetooth::flags::leaudio_dynamic_spatial_audio()) {
      if (group->dsa_.active &&
          (group->dsa_.mode == DsaMode::ISO_SW ||
           group->dsa_.mode == DsaMode::ISO_HW) &&
          leAudioDevice->GetDsaDataPathState() == DataPathState::CONFIGURING) {
        log::info("Datapath configured for headtracking");
        leAudioDevice->SetDsaDataPathState(DataPathState::CONFIGURED);
        return;
      }
    }

    /* Update state for the given cis.*/
    auto ase = leAudioDevice->GetFirstActiveAseByCisAndDataPathState(
        CisState::CONNECTED, DataPathState::CONFIGURING);

    if (!ase || ase->cis_conn_hdl != conn_handle) {
      log::error("Cannot find ase by handle {}", conn_handle);
      return;
    }

    ase->data_path_state = DataPathState::CONFIGURED;

    if (group->GetTargetState() != AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
      log::warn("Group {} is not targeting streaming state any more",
                group->group_id_);
      return;
    }

    AddCisToStreamConfiguration(group, ase);

    if (group->GetState() == AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING &&
        !group->GetFirstActiveDeviceByCisAndDataPathState(
            CisState::CONNECTED, DataPathState::IDLE)) {
      /* No more transition for group. Here we are for the late join device
       * scenario */
      cancel_watchdog_if_needed(group->group_id_);
    }

    if (group->GetNotifyStreamingWhenCisesAreReadyFlag() &&
        group->IsGroupStreamReady()) {
      group->SetNotifyStreamingWhenCisesAreReadyFlag(false);
      log::info("Ready to notify Group Streaming.");
      cancel_watchdog_if_needed(group->group_id_);
      if (group->GetState() != AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
        group->SetState(AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING);
      }
      state_machine_callbacks_->StatusReportCb(group->group_id_,
                                               GroupStreamStatus::STREAMING);
    }
  }

  void ProcessHciNotifRemoveIsoDataPath(LeAudioDeviceGroup* group,
                                        LeAudioDevice* leAudioDevice,
                                        uint8_t status,
                                        uint16_t conn_hdl) override {
    log_history_->AddLogHistory(
        kLogHciEvent, group->group_id_, leAudioDevice->address_,
        kLogRemoveDataPathOp + "STATUS=" + loghex(status));

    if (status != HCI_SUCCESS) {
      log::error(
          "failed to remove ISO data path, reason: 0x{:0x} - contining stream "
          "closing",
          status);
      /* Just continue - disconnecting CIS removes data path as well.*/
    }

    bool do_disconnect = false;

    auto ases_pair = leAudioDevice->GetAsesByCisConnHdl(conn_hdl);
    if (ases_pair.sink &&
        (ases_pair.sink->data_path_state == DataPathState::REMOVING)) {
      ases_pair.sink->data_path_state = DataPathState::IDLE;

      if (ases_pair.sink->cis_state == CisState::CONNECTED) {
        ases_pair.sink->cis_state = CisState::DISCONNECTING;
        do_disconnect = true;
      }
    }

    if (ases_pair.source &&
        (ases_pair.source->data_path_state == DataPathState::REMOVING)) {
      ases_pair.source->data_path_state = DataPathState::IDLE;

      if (ases_pair.source->cis_state == CisState::CONNECTED) {
        ases_pair.source->cis_state = CisState::DISCONNECTING;
        do_disconnect = true;
      }
    } else if (com::android::bluetooth::flags::
                   leaudio_dynamic_spatial_audio()) {
      if (group->dsa_.active &&
          leAudioDevice->GetDsaDataPathState() == DataPathState::REMOVING) {
        log::info("DSA data path removed");
        leAudioDevice->SetDsaDataPathState(DataPathState::IDLE);
        leAudioDevice->SetDsaCisHandle(GATT_INVALID_CONN_ID);
      }
    }

    if (do_disconnect) {
      group->RemoveCisFromStreamIfNeeded(leAudioDevice, conn_hdl);
      IsoManager::GetInstance()->DisconnectCis(conn_hdl, HCI_ERR_PEER_USER);

      log_history_->AddLogHistory(
          kLogStateMachineTag, group->group_id_, leAudioDevice->address_,
          kLogCisDisconnectOp + "cis_h:" + loghex(conn_hdl));
    }
  }

  void ProcessHciNotifIsoLinkQualityRead(
      LeAudioDeviceGroup* group, LeAudioDevice* leAudioDevice,
      uint8_t conn_handle, uint32_t txUnackedPackets, uint32_t txFlushedPackets,
      uint32_t txLastSubeventPackets, uint32_t retransmittedPackets,
      uint32_t crcErrorPackets, uint32_t rxUnreceivedPackets,
      uint32_t duplicatePackets) {
    log::info(
        "conn_handle: 0x{:x}, txUnackedPackets: 0x{:x}, txFlushedPackets: "
        "0x{:x}, txLastSubeventPackets: 0x{:x}, retransmittedPackets: 0x{:x}, "
        "crcErrorPackets: 0x{:x}, rxUnreceivedPackets: 0x{:x}, "
        "duplicatePackets: 0x{:x}",
        conn_handle, txUnackedPackets, txFlushedPackets, txLastSubeventPackets,
        retransmittedPackets, crcErrorPackets, rxUnreceivedPackets,
        duplicatePackets);
  }

  void ReleaseCisIds(LeAudioDeviceGroup* group) {
    if (group == nullptr) {
      log::debug("Group is null.");
      return;
    }
    log::debug("Releasing CIS is for group {}", group->group_id_);

    LeAudioDevice* leAudioDevice = group->GetFirstDevice();
    while (leAudioDevice != nullptr) {
      for (auto& ase : leAudioDevice->ases_) {
        ase.cis_id = bluetooth::le_audio::kInvalidCisId;
        ase.cis_conn_hdl = 0;
      }
      leAudioDevice = group->GetNextDevice(leAudioDevice);
    }

    group->ClearAllCises();
  }

  void RemoveCigForGroup(LeAudioDeviceGroup* group) {
    log::debug("Group: {}, id: {} cig state: {}", fmt::ptr(group),
               group->group_id_, ToString(group->cig.GetState()));
    if (group->cig.GetState() != CigState::CREATED) {
      log::warn("Group: {}, id: {} cig state: {} cannot be removed",
                fmt::ptr(group), group->group_id_,
                ToString(group->cig.GetState()));
      return;
    }

    group->cig.SetState(CigState::REMOVING);
    IsoManager::GetInstance()->RemoveCig(group->group_id_);
    log::debug("Group: {}, id: {} cig state: {}", fmt::ptr(group),
               group->group_id_, ToString(group->cig.GetState()));
    log_history_->AddLogHistory(kLogStateMachineTag, group->group_id_,
                                RawAddress::kEmpty, kLogCigRemoveOp);
  }

  void ProcessHciNotifAclDisconnected(LeAudioDeviceGroup* group,
                                      LeAudioDevice* leAudioDevice) {
    FreeLinkQualityReports(leAudioDevice);
    if (!group) {
      log::error("group is null for device: {} group_id: {}",
                 leAudioDevice->address_, leAudioDevice->group_id_);
      /* mark ASEs as not used. */
      leAudioDevice->DeactivateAllAses();
      return;
    }

    /* It is possible that ACL disconnection came before CIS disconnect event */
    for (auto& ase : leAudioDevice->ases_) {
      group->RemoveCisFromStreamIfNeeded(leAudioDevice, ase.cis_conn_hdl);
    }

    /* mark ASEs as not used. */
    leAudioDevice->DeactivateAllAses();

    /* Update the current group audio context availability which could change
     * due to disconnected group member.
     */
    group->ReloadAudioLocations();
    group->ReloadAudioDirections();
    group->UpdateAudioContextAvailability();
    group->InvalidateCachedConfigurations();
    group->InvalidateGroupStrategy();

    /* If group is in Idle and not transitioning, update the current group
     * audio context availability which could change due to disconnected group
     * member.
     */
    if ((group->GetState() == AseState::BTA_LE_AUDIO_ASE_STATE_IDLE) &&
        !group->IsInTransition()) {
      log::info("group: {} is in IDLE", group->group_id_);

      /* When OnLeAudioDeviceSetStateTimeout happens, group will transition
       * to IDLE, and after that an ACL disconnect will be triggered. We need
       * to check if CIG is created and if it is, remove it so it can be created
       * again after reconnect. Otherwise we will get Command Disallowed on CIG
       * Create when starting stream.
       */
      if (group->cig.GetState() == CigState::CREATED) {
        log::info("CIG is in CREATED state so removing CIG for Group {}",
                  group->group_id_);
        RemoveCigForGroup(group);
      }
      return;
    }

    log::debug(
        "device: {}, group connected: {}, all active ase disconnected:: {}",
        leAudioDevice->address_, group->IsAnyDeviceConnected(),
        group->HaveAllCisesDisconnected());

    if (group->IsAnyDeviceConnected()) {
      /*
       * ACL of one of the device has been dropped. If number of CISes has
       * changed notify upper layer so the CodecManager can be updated with CIS
       * information.
       */
      if (!group->HaveAllCisesDisconnected()) {
        /* some CISes are connected */

        if ((group->GetState() == AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) &&
            (group->GetTargetState() ==
             AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING)) {
          /* We keep streaming but want others to let know user that it might
           * be need to update CodecManager with new CIS configuration
           */
          state_machine_callbacks_->StatusReportCb(
              group->group_id_, GroupStreamStatus::STREAMING);
        } else {
          log::warn("group_id {} not in streaming, CISes are still there",
                    group->group_id_);
          group->PrintDebugState();
        }

        return;
      }
    }

    /* Group is not connected and all the CISes are down.
     * Clean states and destroy HCI group
     */
    ClearGroup(group, true);
  }

  void cancel_watchdog_if_needed(int group_id) {
    if (alarm_is_scheduled(watchdog_)) {
      log_history_->AddLogHistory(kLogStateMachineTag, group_id,
                                  RawAddress::kEmpty, "WATCHDOG STOPPED");
      alarm_cancel(watchdog_);
    }
  }

  void applyDsaDataPath(LeAudioDeviceGroup* group, LeAudioDevice* leAudioDevice,
                        uint16_t conn_hdl) {
    if (!com::android::bluetooth::flags::leaudio_dynamic_spatial_audio()) {
      return;
    }

    if (!group->dsa_.active) {
      log::info("DSA mode not used");
      return;
    }

    DsaModes dsa_modes = leAudioDevice->GetDsaModes();
    if (dsa_modes.empty()) {
      log::warn("DSA mode not supported by this LE Audio device: {}",
                leAudioDevice->address_);
      group->dsa_.active = false;
      return;
    }

    if (std::find(dsa_modes.begin(), dsa_modes.end(), DsaMode::ISO_SW) ==
            dsa_modes.end() &&
        std::find(dsa_modes.begin(), dsa_modes.end(), DsaMode::ISO_HW) ==
            dsa_modes.end()) {
      log::warn("DSA mode not supported by this LE Audio device: {}",
                leAudioDevice->address_);
      group->dsa_.active = false;
      return;
    }

    uint8_t data_path_id = bluetooth::hci::iso_manager::kIsoDataPathHci;
    log::info("DSA mode used: {}", static_cast<int>(group->dsa_.mode));
    switch (group->dsa_.mode) {
      case DsaMode::ISO_HW:
        data_path_id = bluetooth::hci::iso_manager::kIsoDataPathPlatformDefault;
        break;
      case DsaMode::ISO_SW:
        data_path_id = bluetooth::hci::iso_manager::kIsoDataPathHci;
        break;
      default:
        log::warn("Unexpected DsaMode: {}", static_cast<int>(group->dsa_.mode));
        group->dsa_.active = false;
        return;
    }

    leAudioDevice->SetDsaDataPathState(DataPathState::CONFIGURING);
    leAudioDevice->SetDsaCisHandle(conn_hdl);

    log::verbose(
        "DSA mode supported on this LE Audio device: {}, apply data path: {}",
        leAudioDevice->address_, data_path_id);

    LeAudioLogHistory::Get()->AddLogHistory(
        kLogStateMachineTag, group->group_id_, RawAddress::kEmpty,
        kLogSetDataPathOp + "cis_h:" + loghex(conn_hdl),
        "direction: " +
            loghex(bluetooth::hci::iso_manager::kIsoDataPathDirectionOut));

    bluetooth::hci::iso_manager::iso_data_path_params param = {
        .data_path_dir = bluetooth::hci::iso_manager::kIsoDataPathDirectionOut,
        .data_path_id = data_path_id,
        .codec_id_format =
            bluetooth::le_audio::types::kLeAudioCodecHeadtracking.coding_format,
        .codec_id_company =
            bluetooth::le_audio::types::kLeAudioCodecHeadtracking
                .vendor_company_id,
        .codec_id_vendor = bluetooth::le_audio::types::kLeAudioCodecHeadtracking
                               .vendor_codec_id,
        .controller_delay = 0x00000000,
        .codec_conf = std::vector<uint8_t>(),
    };
    IsoManager::GetInstance()->SetupIsoDataPath(conn_hdl, std::move(param));
  }

  void ProcessHciNotifCisEstablished(
      LeAudioDeviceGroup* group, LeAudioDevice* leAudioDevice,
      const bluetooth::hci::iso_manager::cis_establish_cmpl_evt* event)
      override {
    auto ases_pair = leAudioDevice->GetAsesByCisConnHdl(event->cis_conn_hdl);

    log_history_->AddLogHistory(
        kLogHciEvent, group->group_id_, leAudioDevice->address_,
        kLogCisEstablishedOp + "cis_h:" + loghex(event->cis_conn_hdl) +
            " STATUS=" + loghex(event->status));

    if (event->status != HCI_SUCCESS) {
      if (ases_pair.sink) ases_pair.sink->cis_state = CisState::ASSIGNED;
      if (ases_pair.source) ases_pair.source->cis_state = CisState::ASSIGNED;

      log::warn("{}: failed to create CIS 0x{:04x}, status: {} (0x{:02x})",
                leAudioDevice->address_, event->cis_conn_hdl,
                ErrorCodeText((ErrorCode)event->status), event->status);

      if (event->status == HCI_ERR_CONN_FAILED_ESTABLISHMENT &&
          ((leAudioDevice->cis_failed_to_be_established_retry_cnt_++) <
           kNumberOfCisRetries) &&
          (CisCreateForDevice(group, leAudioDevice))) {
        log::info("Retrying ({}) to create CIS for {}",
                  leAudioDevice->cis_failed_to_be_established_retry_cnt_,
                  leAudioDevice->address_);
        return;
      }

      if (event->status == HCI_ERR_UNSUPPORTED_REM_FEATURE &&
          group->asymmetric_phy_for_unidirectional_cis_supported == true &&
          group->GetSduInterval(
              bluetooth::le_audio::types::kLeAudioDirectionSource) == 0) {
        log::info(
            "Remote device may not support asymmetric phy for CIS, retry "
            "symmetric setting again");
        group->asymmetric_phy_for_unidirectional_cis_supported = false;
      }

      log::error("CIS creation failed {} times, stopping the stream",
                 leAudioDevice->cis_failed_to_be_established_retry_cnt_);
      leAudioDevice->cis_failed_to_be_established_retry_cnt_ = 0;

      /* CIS establishment failed. Remove CIG if no other CIS is already created
       * or pending. If CIS is established, this will be handled in disconnected
       * complete event
       */
      if (group->HaveAllCisesDisconnected()) {
        RemoveCigForGroup(group);
      }

      StopStream(group);
      return;
    }

    if (leAudioDevice->cis_failed_to_be_established_retry_cnt_ > 0) {
      /* Reset retry counter */
      leAudioDevice->cis_failed_to_be_established_retry_cnt_ = 0;
    }

    if (group->GetTargetState() != AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
      log::error("Unintended CIS establishement event came for group id: {}",
                 group->group_id_);
      StopStream(group);
      return;
    }

    if (ases_pair.sink) ases_pair.sink->cis_state = CisState::CONNECTED;
    if (ases_pair.source) ases_pair.source->cis_state = CisState::CONNECTED;

    if (ases_pair.sink &&
        (ases_pair.sink->data_path_state == DataPathState::IDLE)) {
      PrepareDataPath(group->group_id_, ases_pair.sink);
    }

    if (ases_pair.source &&
        (ases_pair.source->data_path_state == DataPathState::IDLE)) {
      PrepareDataPath(group->group_id_, ases_pair.source);
    } else {
      applyDsaDataPath(group, leAudioDevice, event->cis_conn_hdl);
    }

    if (osi_property_get_bool("persist.bluetooth.iso_link_quality_report",
                              false)) {
      leAudioDevice->link_quality_timer =
          alarm_new_periodic("le_audio_cis_link_quality");
      leAudioDevice->link_quality_timer_data = event->cis_conn_hdl;
      alarm_set_on_mloop(leAudioDevice->link_quality_timer,
                         linkQualityCheckInterval, link_quality_cb,
                         &leAudioDevice->link_quality_timer_data);
    }

    if (!leAudioDevice->HaveAllActiveAsesCisEst()) {
      /* More cis established events has to come */
      return;
    }

    if (!leAudioDevice->IsReadyToCreateStream() && !flag_sendenableLater) {
      /* Device still remains in ready to create stream state. It means that
       * more enabling status notifications has to come. This may only happen
       * for reconnection scenario for bi-directional CIS.
       */
      return;
    }

    /* All CISes created. Send start ready for source ASE before we can go
     * to streaming state.
     */
    struct ase* ase = leAudioDevice->GetFirstActiveAse();
    if (osi_property_get_bool("persist.bluetooth.leaudio.bap_enableQoS_src", false)) {
       ase= leAudioDevice->GetNextActiveAse(ase);
    }
    log::assert_that(ase != nullptr,
                     "shouldn't be called without an active ASE, device {}, "
                     "group id: {}, cis handle 0x{:04x}",
                     ADDRESS_TO_LOGGABLE_CSTR(leAudioDevice->address_),
                     event->cig_id, event->cis_conn_hdl);

    PrepareAndSendReceiverStartReady(leAudioDevice, ase);
  }

  static void WriteToControlPoint(LeAudioDevice* leAudioDevice,
                                  std::vector<uint8_t> value) {
    tGATT_WRITE_TYPE write_type = GATT_WRITE_NO_RSP;

    if (value.size() > (leAudioDevice->mtu_ - 3)) {
      log::warn("{}, using long write procedure ({} > {})",
                leAudioDevice->address_, static_cast<int>(value.size()),
                leAudioDevice->mtu_ - 3);

      /* Note, that this type is actually LONG WRITE.
       * Meaning all the Prepare Writes plus Execute is handled in the stack
       */
      write_type = GATT_WRITE_PREPARE;
    }

    BtaGattQueue::WriteCharacteristic(leAudioDevice->conn_id_,
                                      leAudioDevice->ctp_hdls_.val_hdl, value,
                                      write_type, NULL, NULL);
  }

  static void RemoveDataPathByCisHandle(LeAudioDevice* leAudioDevice,
                                        uint16_t cis_conn_hdl) {
    auto ases_pair = leAudioDevice->GetAsesByCisConnHdl(cis_conn_hdl);
    uint8_t value = 0;

    if (ases_pair.sink &&
        ases_pair.sink->data_path_state == DataPathState::CONFIGURED) {
      value |= bluetooth::hci::iso_manager::kRemoveIsoDataPathDirectionInput;
      ases_pair.sink->data_path_state = DataPathState::REMOVING;
    }

    if (ases_pair.source &&
        ases_pair.source->data_path_state == DataPathState::CONFIGURED) {
      value |= bluetooth::hci::iso_manager::kRemoveIsoDataPathDirectionOutput;
      ases_pair.source->data_path_state = DataPathState::REMOVING;
    } else {
      if (com::android::bluetooth::flags::leaudio_dynamic_spatial_audio()) {
        if (leAudioDevice->GetDsaDataPathState() == DataPathState::CONFIGURED) {
          value |=
              bluetooth::hci::iso_manager::kRemoveIsoDataPathDirectionOutput;
          leAudioDevice->SetDsaDataPathState(DataPathState::REMOVING);
        }
      }
    }

    if (value == 0) {
      log::info("Data path was not set. Nothing to do here.");
      return;
    }

    IsoManager::GetInstance()->RemoveIsoDataPath(cis_conn_hdl, value);

    LeAudioLogHistory::Get()->AddLogHistory(
        kLogStateMachineTag, leAudioDevice->group_id_, leAudioDevice->address_,
        kLogRemoveDataPathOp + " cis_h:" + loghex(cis_conn_hdl));
  }

  void ProcessHciNotifCisDisconnected(
      LeAudioDeviceGroup* group, LeAudioDevice* leAudioDevice,
      const bluetooth::hci::iso_manager::cis_disconnected_evt* event) override {
    /* Reset the disconnected CIS states */

    FreeLinkQualityReports(leAudioDevice);

    auto ases_pair = leAudioDevice->GetAsesByCisConnHdl(event->cis_conn_hdl);

    log_history_->AddLogHistory(
        kLogHciEvent, group->group_id_, leAudioDevice->address_,
        kLogCisDisconnectedOp + "cis_h:" + loghex(event->cis_conn_hdl) +
            " REASON=" + loghex(event->reason));

    if (ases_pair.sink) {
      ases_pair.sink->cis_state = CisState::ASSIGNED;
    }
    if (ases_pair.source) {
      ases_pair.source->cis_state = CisState::ASSIGNED;
    }

    RemoveDataPathByCisHandle(leAudioDevice, event->cis_conn_hdl);

    /* If this is peer disconnecting CIS, make sure to clear data path */
    if (event->reason != HCI_ERR_CONN_CAUSE_LOCAL_HOST) {
      // Make sure we won't stay in STREAMING state
      if (ases_pair.sink &&
          ases_pair.sink->state == AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
        SetAseState(leAudioDevice, ases_pair.sink,
                    AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED);
      }
      if (ases_pair.source && ases_pair.source->state ==
                                  AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
        SetAseState(leAudioDevice, ases_pair.source,
                    AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED);
      }
    }

    group->RemoveCisFromStreamIfNeeded(leAudioDevice, event->cis_conn_hdl);

    auto target_state = group->GetTargetState();
    switch (target_state) {
      case AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING: {
        /* Something wrong happen when streaming or when creating stream.
         * If there is other device connected and streaming, just leave it as it
         * is, otherwise stop the stream.
         */
        if (!group->HaveAllCisesDisconnected()) {
          /* There is ASE streaming for some device. Continue streaming. */
          log::warn(
              "Group member disconnected during streaming. Cis handle 0x{:04x}",
              event->cis_conn_hdl);
          return;
        }

        /* CISes are disconnected, but it could be a case here, that there is
         * another set member trying to get STREAMING state. Can happen when
         * while streaming user switch buds. In such a case, lets try to allow
         * that device to continue
         */

        LeAudioDevice* attaching_device =
            getDeviceTryingToAttachTheStream(group);
        if (attaching_device != nullptr) {
          /* There is a device willitng to stream. Let's wait for it to start
           * streaming */
          auto active_ase = attaching_device->GetFirstActiveAse();
          group->SetState(active_ase->state);

          /* this is just to start timer */
          group->SetTargetState(AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING);
          log::info(
              "{} is still attaching to stream while other members got "
              "disconnected from the group_id: {}",
              attaching_device->address_, group->group_id_);
          return;
        }

        log::info("Lost all members from the group {}", group->group_id_);
        group->cig.cises.clear();
        RemoveCigForGroup(group);

        group->SetState(AseState::BTA_LE_AUDIO_ASE_STATE_IDLE);
        group->SetTargetState(AseState::BTA_LE_AUDIO_ASE_STATE_IDLE);
        /* If there is no more ase to stream. Notify it is in IDLE. */
        state_machine_callbacks_->StatusReportCb(group->group_id_,
                                                 GroupStreamStatus::IDLE);
        return;
      }

      case AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED:
        /* Intentional group disconnect has finished, but the last CIS in the
         * event came after the ASE notification.
         * If group is already suspended and all CIS are disconnected, we can
         * report SUSPENDED state.
         */
        if ((group->GetState() ==
             AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED) &&
            group->HaveAllCisesDisconnected()) {
          /* No more transition for group */
          cancel_watchdog_if_needed(group->group_id_);

          state_machine_callbacks_->StatusReportCb(
              group->group_id_, GroupStreamStatus::SUSPENDED);
          return;
        }
        break;
      case AseState::BTA_LE_AUDIO_ASE_STATE_IDLE:
      case AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED: {
        /* Those two are used when closing the stream and CIS disconnection is
         * expected */
        if (!group->HaveAllCisesDisconnected()) {
          log::debug(
              "Still waiting for all CISes being disconnected for group:{}",
              group->group_id_);
          return;
        }

        auto current_group_state = group->GetState();
        log::info("group {} current state: {}, target state: {}",
                  group->group_id_,
                  bluetooth::common::ToString(current_group_state),
                  bluetooth::common::ToString(target_state));
        /* It might happen that controller notified about CIS disconnection
         * later, after ASE state already changed.
         * In such an event, there is need to notify upper layer about state
         * from here.
         */
        if (current_group_state == AseState::BTA_LE_AUDIO_ASE_STATE_IDLE) {
          cancel_watchdog_if_needed(group->group_id_);

          log::info(
              "Cises disconnected for group {}, we are good in Idle state.",
              group->group_id_);
          ReleaseCisIds(group);
          state_machine_callbacks_->StatusReportCb(group->group_id_,
                                                   GroupStreamStatus::IDLE);
        } else if (current_group_state ==
                   AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED) {
          cancel_watchdog_if_needed(group->group_id_);
          auto reconfig = group->IsPendingConfiguration();
          log::info(
              "Cises disconnected for group: {}, we are good in Configured "
              "state, reconfig={}.",
              group->group_id_, reconfig);

          /* This is Autonomous change if both, target and current state
           * is CODEC_CONFIGURED
           */
          if (target_state == current_group_state) {
            state_machine_callbacks_->StatusReportCb(
                group->group_id_, GroupStreamStatus::CONFIGURED_AUTONOMOUS);
          }
        }
        RemoveCigForGroup(group);
      } break;
      default:
        break;
    }

    /* We should send Receiver Stop Ready when acting as a source */
    if (ases_pair.source &&
        ases_pair.source->state == AseState::BTA_LE_AUDIO_ASE_STATE_DISABLING) {
      std::vector<uint8_t> ids = {ases_pair.source->id};
      std::vector<uint8_t> value;

      bluetooth::le_audio::client_parser::ascs::
          PrepareAseCtpAudioReceiverStopReady(ids, value);
      WriteToControlPoint(leAudioDevice, value);

      log_history_->AddLogHistory(kLogControlPointCmd, leAudioDevice->group_id_,
                                  leAudioDevice->address_,
                                  kLogAseStopReadyOp + "ASE_ID " +
                                      std::to_string(ases_pair.source->id));
    }

    /* Tear down CIS's data paths within the group */
    struct ase* ase = leAudioDevice->GetFirstActiveAseByCisAndDataPathState(
        CisState::CONNECTED, DataPathState::CONFIGURED);
    if (!ase) {
      leAudioDevice = group->GetNextActiveDevice(leAudioDevice);
      /* No more ASEs to disconnect their CISes */
      if (!leAudioDevice) return;

      ase = leAudioDevice->GetFirstActiveAse();
    }

    log::assert_that(ase, "shouldn't be called without an active ASE");
    if (ase->data_path_state == DataPathState::CONFIGURED) {
      RemoveDataPathByCisHandle(leAudioDevice, ase->cis_conn_hdl);
    }
  }

 private:
  static constexpr uint64_t kStateTransitionTimeoutMs = 3500;
  static constexpr char kStateTransitionTimeoutMsProp[] =
      "persist.bluetooth.leaudio.device.set.state.timeoutms";
  Callbacks* state_machine_callbacks_;
  alarm_t* watchdog_;
  LeAudioLogHistory* log_history_;

  /* This callback is called on timeout during transition to target state */
  void OnStateTransitionTimeout(int group_id) {
    log_history_->AddLogHistory(kLogStateMachineTag, group_id,
                                RawAddress::kEmpty, "WATCHDOG FIRED");
    state_machine_callbacks_->OnStateTransitionTimeout(group_id);
  }

  void SetTargetState(LeAudioDeviceGroup* group, AseState state) {
    auto current_state = ToString(group->GetTargetState());
    auto new_state = ToString(state);

    log::debug("Watchdog watch started for group={} transition from {} to {}",
               group->group_id_, current_state, new_state);

    group->SetTargetState(state);

    /* Group should tie in time to get requested status */
    uint64_t timeoutMs = kStateTransitionTimeoutMs;
    timeoutMs =
        osi_property_get_int32(kStateTransitionTimeoutMsProp, timeoutMs);

    cancel_watchdog_if_needed(group->group_id_);

    alarm_set_on_mloop(
        watchdog_, timeoutMs,
        [](void* data) {
          if (instance) instance->OnStateTransitionTimeout(PTR_TO_INT(data));
        },
        INT_TO_PTR(group->group_id_));

    log_history_->AddLogHistory(kLogStateMachineTag, group->group_id_,
                                RawAddress::kEmpty, "WATCHDOG STARTED");
  }

  void AddCisToStreamConfiguration(LeAudioDeviceGroup* group,
                                   const struct ase* ase) {
    group->stream_conf.codec_id = ase->codec_id;

    auto cis_conn_hdl = ase->cis_conn_hdl;
    auto& params = group->stream_conf.stream_params.get(ase->direction);
    auto context_type = group->GetConfigurationContextType();
    log::info(
        "Adding cis handle 0x{:04x} ({}) to stream list", cis_conn_hdl,
        ase->direction == bluetooth::le_audio::types::kLeAudioDirectionSink
            ? "sink"
            : "source");

    auto iter = std::find_if(
        params.stream_locations.begin(), params.stream_locations.end(),
        [cis_conn_hdl](auto& pair) { return cis_conn_hdl == pair.first; });
    log::assert_that(iter == params.stream_locations.end(),
                     "Stream is already there 0x{:04x}", cis_conn_hdl);

    auto core_config = ase->codec_config.GetAsCoreCodecConfig();

    params.num_of_devices++;
    params.num_of_channels += ase->channel_count;

    if (!core_config.audio_channel_allocation.has_value()) {
      log::warn("ASE has invalid audio location");
    }
    auto ase_audio_channel_allocation =
        core_config.audio_channel_allocation.value_or(0);
    params.audio_channel_allocation |= ase_audio_channel_allocation;
    params.stream_locations.emplace_back(
        std::make_pair(ase->cis_conn_hdl, ase_audio_channel_allocation));

    if (params.sample_frequency_hz == 0) {
      params.sample_frequency_hz = core_config.GetSamplingFrequencyHz();
    } else {
      log::assert_that(
          params.sample_frequency_hz == core_config.GetSamplingFrequencyHz(),
          "sample freq mismatch: {}!={}", params.sample_frequency_hz,
          core_config.GetSamplingFrequencyHz());
    }

    if (params.octets_per_codec_frame == 0) {
      params.octets_per_codec_frame = *core_config.octets_per_codec_frame;
    } else {
      log::assert_that(
          params.octets_per_codec_frame == *core_config.octets_per_codec_frame,
          "octets per frame mismatch: {}!={}", params.octets_per_codec_frame,
          *core_config.octets_per_codec_frame);
    }

    if (params.codec_frames_blocks_per_sdu == 0) {
      params.codec_frames_blocks_per_sdu =
          *core_config.codec_frames_blocks_per_sdu;
    } else {
      log::assert_that(params.codec_frames_blocks_per_sdu ==
                           *core_config.codec_frames_blocks_per_sdu,
                       "codec_frames_blocks_per_sdu: {}!={}",
                       params.codec_frames_blocks_per_sdu,
                       *core_config.codec_frames_blocks_per_sdu);
    }

    if (params.frame_duration_us == 0) {
      params.frame_duration_us = core_config.GetFrameDurationUs();
    } else {
      log::assert_that(
          params.frame_duration_us == core_config.GetFrameDurationUs(),
          "frame_duration_us: {}!={}", params.frame_duration_us,
          core_config.GetFrameDurationUs());
    }

    params.codec_spec_metadata = group->GetCodecVendorMetadata(ase->direction, context_type);

    log::info(
        "Added {} Stream Configuration. CIS Connection Handle: {}, Audio "
        "Channel Allocation: {}, Number Of Devices: {}, Number Of Channels: {}",
        (ase->direction == bluetooth::le_audio::types::kLeAudioDirectionSink
             ? "Sink"
             : "Source"),
        cis_conn_hdl, ase_audio_channel_allocation, params.num_of_devices,
        params.num_of_channels);

    /* Update CodecManager stream configuration */
    state_machine_callbacks_->OnUpdatedCisConfiguration(group->group_id_,
                                                        ase->direction);
  }

  static bool isIntervalAndLatencyProperlySet(uint32_t sdu_interval_us,
                                              uint16_t max_latency_ms) {
    log::verbose("sdu_interval_us: {}, max_latency_ms: {}", sdu_interval_us,
                 max_latency_ms);

    if (sdu_interval_us == 0) {
      return max_latency_ms ==
             bluetooth::le_audio::types::kMaxTransportLatencyMin;
    }
    return ((1000 * max_latency_ms) >= sdu_interval_us);
  }

  void ApplyDsaParams(LeAudioDeviceGroup* group,
                      bluetooth::hci::iso_manager::cig_create_params& param) {
    if (!com::android::bluetooth::flags::leaudio_dynamic_spatial_audio()) {
      return;
    }

    log::info("DSA mode selected: {}", (int)group->dsa_.mode);
    group->dsa_.active = false;

    /* Unidirectional streaming */
    if (param.sdu_itv_stom == 0) {
      log::info("Media streaming, apply DSA parameters");

      switch (group->dsa_.mode) {
        case DsaMode::ISO_HW:
        case DsaMode::ISO_SW: {
          auto& cis_cfgs = param.cis_cfgs;
          auto it = cis_cfgs.begin();

          for (auto dsa_modes : group->GetAllowedDsaModesList()) {
            if (!dsa_modes.empty() && it != cis_cfgs.end()) {
              if (std::find(dsa_modes.begin(), dsa_modes.end(),
                            group->dsa_.mode) != dsa_modes.end()) {
                log::info("Device found with support for selected DsaMode");

                group->dsa_.active = true;

                /* Todo: Replace literal values */
                param.sdu_itv_stom = 20000;
                param.max_trans_lat_stom = 20;
                it->max_sdu_size_stom = 15;
                it->rtn_stom = 2;

                it++;
              }
            }
          }
        } break;

        case DsaMode::ACL:
          /* Todo: Prioritize the ACL */
          break;

        case DsaMode::DISABLED:
        default:
          /* No need to change ISO parameters */
          break;
      }
    } else {
      log::debug("Bidirection streaming, ignore DSA mode");
    }
  }

  bool CigCreate(LeAudioDeviceGroup* group) {
    uint32_t sdu_interval_mtos, sdu_interval_stom;
    uint16_t max_trans_lat_mtos, max_trans_lat_stom;
    uint8_t packing, framing, sca;
    std::vector<EXT_CIS_CFG> cis_cfgs;

    log::debug("Group: {}, id: {} cig state: {}", fmt::ptr(group),
               group->group_id_, ToString(group->cig.GetState()));

    if (group->cig.GetState() != CigState::NONE) {
      log::warn("Group {}, id: {} has invalid cig state: {}", fmt::ptr(group),
                group->group_id_, ToString(group->cig.GetState()));
      return false;
    }

    sdu_interval_mtos = group->GetSduInterval(
        bluetooth::le_audio::types::kLeAudioDirectionSink);
    sdu_interval_stom = group->GetSduInterval(
        bluetooth::le_audio::types::kLeAudioDirectionSource);
    sca = group->GetSCA();
    packing = group->GetPacking();
    framing = group->GetFraming();
    max_trans_lat_mtos = group->GetMaxTransportLatencyMtos();
    max_trans_lat_stom = group->GetMaxTransportLatencyStom();

    uint16_t max_sdu_size_mtos = 0;
    uint16_t max_sdu_size_stom = 0;
    uint8_t phy_mtos =
        group->GetPhyBitmask(bluetooth::le_audio::types::kLeAudioDirectionSink);
    uint8_t phy_stom = group->GetPhyBitmask(
        bluetooth::le_audio::types::kLeAudioDirectionSource);

    if (!isIntervalAndLatencyProperlySet(sdu_interval_mtos,
                                         max_trans_lat_mtos) ||
        !isIntervalAndLatencyProperlySet(sdu_interval_stom,
                                         max_trans_lat_stom)) {
      log::error("Latency and interval not properly set");
      group->PrintDebugState();
      return false;
    }

    // Use 1M Phy for the ACK packet from remote device to phone for better
    // sensitivity
    if (group->asymmetric_phy_for_unidirectional_cis_supported &&
        sdu_interval_stom == 0 &&
        (phy_stom & bluetooth::hci::kIsoCigPhy1M) != 0) {
      log::info("Use asymmetric PHY for unidirectional CIS");
      phy_stom = bluetooth::hci::kIsoCigPhy1M;
    }

    uint8_t rtn_mtos = 0;
    uint8_t rtn_stom = 0;

    /* Currently assumed Sink/Source configuration is same across cis types.
     * If a cis in cises_ is currently associated with active device/ASE(s),
     * use the Sink/Source configuration for the same.
     * If a cis in cises_ is not currently associated with active device/ASE(s),
     * use the Sink/Source configuration for the cis in cises_
     * associated with a active device/ASE(s). When the same cis is associated
     * later, with active device/ASE(s), check if current configuration is
     * supported or not, if not, reconfigure CIG.
     */
    for (struct bluetooth::le_audio::types::cis& cis : group->cig.cises) {
      uint16_t max_sdu_size_mtos_temp = group->GetMaxSduSize(
          bluetooth::le_audio::types::kLeAudioDirectionSink, cis.id);
      uint16_t max_sdu_size_stom_temp = group->GetMaxSduSize(
          bluetooth::le_audio::types::kLeAudioDirectionSource, cis.id);
      uint8_t rtn_mtos_temp = group->GetRtn(
          bluetooth::le_audio::types::kLeAudioDirectionSink, cis.id);
      uint8_t rtn_stom_temp = group->GetRtn(
          bluetooth::le_audio::types::kLeAudioDirectionSource, cis.id);

      max_sdu_size_mtos =
          max_sdu_size_mtos_temp ? max_sdu_size_mtos_temp : max_sdu_size_mtos;
      max_sdu_size_stom =
          max_sdu_size_stom_temp ? max_sdu_size_stom_temp : max_sdu_size_stom;
      rtn_mtos = rtn_mtos_temp ? rtn_mtos_temp : rtn_mtos;
      rtn_stom = rtn_stom_temp ? rtn_stom_temp : rtn_stom;
    }

    for (struct bluetooth::le_audio::types::cis& cis : group->cig.cises) {
      EXT_CIS_CFG cis_cfg = {};

      cis_cfg.cis_id = cis.id;
      cis_cfg.phy_mtos = phy_mtos;
      cis_cfg.phy_stom = phy_stom;
      if (cis.type ==
          bluetooth::le_audio::types::CisType::CIS_TYPE_BIDIRECTIONAL) {
        cis_cfg.max_sdu_size_mtos = max_sdu_size_mtos;
        cis_cfg.rtn_mtos = rtn_mtos;
        cis_cfg.max_sdu_size_stom = max_sdu_size_stom;
        cis_cfg.rtn_stom = rtn_stom;
        cis_cfgs.push_back(cis_cfg);
      } else if (cis.type == bluetooth::le_audio::types::CisType::
                                 CIS_TYPE_UNIDIRECTIONAL_SINK) {
        cis_cfg.max_sdu_size_mtos = max_sdu_size_mtos;
        cis_cfg.rtn_mtos = rtn_mtos;
        cis_cfg.max_sdu_size_stom = 0;
        cis_cfg.rtn_stom = 0;
        cis_cfgs.push_back(cis_cfg);
      } else {
        cis_cfg.max_sdu_size_mtos = 0;
        cis_cfg.rtn_mtos = 0;
        cis_cfg.max_sdu_size_stom = max_sdu_size_stom;
        cis_cfg.rtn_stom = rtn_stom;
        cis_cfgs.push_back(cis_cfg);
      }
    }

    if ((sdu_interval_mtos == 0 && sdu_interval_stom == 0) ||
        (max_trans_lat_mtos ==
             bluetooth::le_audio::types::kMaxTransportLatencyMin &&
         max_trans_lat_stom ==
             bluetooth::le_audio::types::kMaxTransportLatencyMin) ||
        (max_sdu_size_mtos == 0 && max_sdu_size_stom == 0)) {
      log::error("Trying to create invalid group");
      group->PrintDebugState();
      return false;
    }

    bluetooth::hci::iso_manager::cig_create_params param = {
        .sdu_itv_mtos = sdu_interval_mtos,
        .sdu_itv_stom = sdu_interval_stom,
        .sca = sca,
        .packing = packing,
        .framing = framing,
        .max_trans_lat_stom = max_trans_lat_stom,
        .max_trans_lat_mtos = max_trans_lat_mtos,
        .cis_cfgs = std::move(cis_cfgs),
    };

    ApplyDsaParams(group, param);

    log_history_->AddLogHistory(
        kLogStateMachineTag, group->group_id_, RawAddress::kEmpty,
        kLogCigCreateOp + "#CIS: " + std::to_string(param.cis_cfgs.size()));

    group->cig.SetState(CigState::CREATING);
    IsoManager::GetInstance()->CreateCig(group->group_id_, std::move(param));
    log::debug("Group: {}, id: {} cig state: {}", fmt::ptr(group),
               group->group_id_, ToString(group->cig.GetState()));
    return true;
  }

  static bool CisCreateForDevice(LeAudioDeviceGroup* group,
                                 LeAudioDevice* leAudioDevice) {
    std::vector<EXT_CIS_CREATE_CFG> conn_pairs;
    struct ase* ase = leAudioDevice->GetFirstActiveAse();

    /* Make sure CIG is there */
    if (group->cig.GetState() != CigState::CREATED) {
      log::error("CIG is not created for group_id {}", group->group_id_);
      group->PrintDebugState();
      return false;
    }

    std::stringstream extra_stream;
    do {
      /* First in ase pair is Sink, second Source */
      auto ases_pair = leAudioDevice->GetAsesByCisConnHdl(ase->cis_conn_hdl);

      /* Already in pending state - bi-directional CIS or seconde CIS to same
       * device */
      if (ase->cis_state == CisState::CONNECTING ||
          ase->cis_state == CisState::CONNECTED)
        continue;

      if (ases_pair.sink) ases_pair.sink->cis_state = CisState::CONNECTING;
      if (ases_pair.source) ases_pair.source->cis_state = CisState::CONNECTING;

      uint16_t acl_handle =
          BTM_GetHCIConnHandle(leAudioDevice->address_, BT_TRANSPORT_LE);
      conn_pairs.push_back({.cis_conn_handle = ase->cis_conn_hdl,
                            .acl_conn_handle = acl_handle});
      log::info("cis handle: 0x{:04x}, acl handle: 0x{:04x}", ase->cis_conn_hdl,
                acl_handle);
      extra_stream << "cis_h:" << loghex(ase->cis_conn_hdl)
                   << " acl_h:" << loghex(acl_handle) << ";;";
    } while ((ase = leAudioDevice->GetNextActiveAse(ase)));

    LeAudioLogHistory::Get()->AddLogHistory(
        kLogStateMachineTag, leAudioDevice->group_id_, RawAddress::kEmpty,
        kLogCisCreateOp + "#CIS: " + std::to_string(conn_pairs.size()),
        extra_stream.str());

    IsoManager::GetInstance()->EstablishCis(
        {.conn_pairs = std::move(conn_pairs)});

    return true;
  }

  static bool CisCreate(LeAudioDeviceGroup* group) {
    LeAudioDevice* leAudioDevice = group->GetFirstActiveDevice();
    struct ase* ase;
    static int count = 0;
    std::vector<EXT_CIS_CREATE_CFG> conn_pairs;

    log::assert_that(leAudioDevice,
                     "Shouldn't be called without an active device.");

    /* Make sure CIG is there */
    if (group->cig.GetState() != CigState::CREATED) {
      log::error("CIG is not created for group_id {}", group->group_id_);
      group->PrintDebugState();
      return false;
    }

    do {
      ase = leAudioDevice->GetFirstActiveAse();
      if (osi_property_get_bool("persist.bluetooth.leaudio.bap_enableQoS_src", false)) {
         ase = leAudioDevice->GetNextActiveAse(ase);
      }
      if (flag_sendenableLater && count == 1) {
           log::debug("next ase is being called");
           if (osi_property_get_bool("persist.bluetooth.leaudio.bap_enableQoS_src", false)) {
              ase = leAudioDevice->GetFirstActiveAse();
           } else {
              ase = leAudioDevice->GetNextActiveAse(ase);
           }
      }
      count = 1;
      log::assert_that(ase, "shouldn't be called without an active ASE");
      do {
        /* First is ase pair is Sink, second Source */
        auto ases_pair = leAudioDevice->GetAsesByCisConnHdl(ase->cis_conn_hdl);

        /* Already in pending state - bi-directional CIS or second CIS to */
        /* same device*/
        if (ase->cis_state == CisState::CONNECTING ||
            ase->cis_state == CisState::CONNECTED)
          continue;

        if (ases_pair.sink) ases_pair.sink->cis_state = CisState::CONNECTING;
        if (ases_pair.source)
          ases_pair.source->cis_state = CisState::CONNECTING;

        uint16_t acl_handle =
            BTM_GetHCIConnHandle(leAudioDevice->address_, BT_TRANSPORT_LE);
        conn_pairs.push_back({.cis_conn_handle = ase->cis_conn_hdl,
                              .acl_conn_handle = acl_handle});
        log::debug("cis handle: {} acl handle : 0x{:x}", ase->cis_conn_hdl,
                   acl_handle);
        if (flag_sendenableLater) {
           break;
        }
      } while ((ase = leAudioDevice->GetNextActiveAse(ase)));
    } while ((leAudioDevice = group->GetNextActiveDevice(leAudioDevice)));

    bool ignore_cis_create = false;
    for (auto& it : conn_pairs) {
      if (!it.cis_conn_handle) {
        log::error("cis handle 0. Is CreateCig skipped ?");
        ignore_cis_create = true;
        break;
      }
    }
    if (ignore_cis_create) {
      log::error("cannot proceed cis create");
      return false;
    }

    IsoManager::GetInstance()->EstablishCis(
        {.conn_pairs = std::move(conn_pairs)});

    return true;
  }

  static void PrepareDataPath(int group_id, struct ase* ase) {
    bluetooth::hci::iso_manager::iso_data_path_params param = {
        .data_path_dir =
            ase->direction == bluetooth::le_audio::types::kLeAudioDirectionSink
                ? bluetooth::hci::iso_manager::kIsoDataPathDirectionIn
                : bluetooth::hci::iso_manager::kIsoDataPathDirectionOut,
        .data_path_id = ase->data_path_configuration.dataPathId,
        .codec_id_format = ase->data_path_configuration.isoDataPathConfig
                               .codecId.coding_format,
        .codec_id_company = ase->data_path_configuration.isoDataPathConfig
                                .codecId.vendor_company_id,
        .codec_id_vendor = ase->data_path_configuration.isoDataPathConfig
                               .codecId.vendor_codec_id,
        .controller_delay =
            ase->data_path_configuration.isoDataPathConfig.controllerDelayUs,
        .codec_conf =
            ase->data_path_configuration.isoDataPathConfig.configuration,
    };

    if (!ase->is_codec_in_controller) {
      param.codec_id_company = 0x0000;
      param.codec_id_vendor = 0x0000;
    }

    LeAudioLogHistory::Get()->AddLogHistory(
        kLogStateMachineTag, group_id, RawAddress::kEmpty,
        kLogSetDataPathOp + "cis_h:" + loghex(ase->cis_conn_hdl),
        "direction: " + loghex(param.data_path_dir) + ", codecId: " +
            ToString(ase->data_path_configuration.isoDataPathConfig.codecId));

    ase->data_path_state = DataPathState::CONFIGURING;
    IsoManager::GetInstance()->SetupIsoDataPath(ase->cis_conn_hdl,
                                                std::move(param));
  }

  static void ReleaseDataPath(LeAudioDeviceGroup* group) {
    LeAudioDevice* leAudioDevice = group->GetFirstActiveDevice();
    while (leAudioDevice){
      leAudioDevice->HaveAllActiveAsesCisEst();
      leAudioDevice->HaveAllActiveAsesSameDataPathState(DataPathState::CONFIGURED);
      auto ase = leAudioDevice->GetFirstActiveAseByCisAndDataPathState(
        CisState::CONNECTED, DataPathState::CONFIGURED);
      if (ase){
        log::info("{} is the first device with active ase", leAudioDevice->address_);
        RemoveDataPathByCisHandle(leAudioDevice, ase->cis_conn_hdl);
        break;
      } else {
        log::info("{} has no active ase, search next device", leAudioDevice->address_);
        leAudioDevice = group->GetNextActiveDevice(leAudioDevice);
      }
    }
  }

  static uint8_t audioContextToUseCase(const LeAudioContextType& context) {
    switch (context) {
      case LeAudioContextType::UNINITIALIZED:
      case LeAudioContextType::UNSPECIFIED:
      case LeAudioContextType::MEDIA:
      case LeAudioContextType::INSTRUCTIONAL:
      case LeAudioContextType::ALERTS:
      case LeAudioContextType::SOUNDEFFECTS:
      case LeAudioContextType::NOTIFICATIONS:
      case LeAudioContextType::EMERGENCYALARM:
        return HQ_AUDIO_USE_CASE;
      case LeAudioContextType::CONVERSATIONAL:
      case LeAudioContextType::VOICEASSISTANTS:
      case LeAudioContextType::RINGTONE:
        return VOICE_USE_CASE;
      case LeAudioContextType::GAME:
        return GAMING_VBC_USE_CASE;
      case LeAudioContextType::LIVE:
        return STEREO_REC_USE_CASE;
      default:
        return HQ_AUDIO_USE_CASE;
    }
  }

  static std::vector<uint8_t> PrepareVendorConfigPayloadData(
      LeAudioDeviceGroup* group, std::vector<uint16_t> conn_handles,
      uint8_t direction) {
    std::vector<uint8_t> vendor_datapath_config;
    auto context_type = group->GetConfigurationContextType();

    if (CodecManager::GetInstance()->GetCodecLocation() ==
        CodecLocation::HOST) {
      LOG(INFO) << __func__ << "send empty payload for non-offload";
      return vendor_datapath_config;
    }

    LeAudioDevice* leAudioDevice = group->GetFirstActiveDevice();
    LOG_ASSERT(leAudioDevice)
        << __func__ << " Shouldn't be called without an active device.";

    auto datapath_state = CisState::ASSIGNED;
    auto ase = leAudioDevice->GetFirstActiveAseByCisAndDataPathState(
        datapath_state, DataPathState::IDLE);
    if (!ase) {
      LOG(INFO) << __func__ << "Invalid datapath state " << datapath_state;
      return vendor_datapath_config;
    }

    ase = leAudioDevice->GetFirstActiveAseByDirection(direction);
    if (!ase) {
      LOG(INFO) << __func__ << "Inactive ASE for direction " << direction;
      return vendor_datapath_config;
    }

    // Populate codec version number
    uint8_t codec_ver = 0;
    uint8_t len = LTV_LEN_VER_NUM;
    uint8_t type = LTV_TYPE_VER_NUM;
    auto vendor_metadata = group->GetCodecVendorMetadata(direction, context_type);

    if (vendor_metadata.empty())
      codec_ver = 0;
    else
      codec_ver = ase->codec_id.coding_format == bluetooth::le_audio::types::kLeAudioCodingFormatLC3 ?
                  (direction == bluetooth::le_audio::types::kLeAudioDirectionSink ?
                  vendor_metadata[6] : vendor_metadata[7]) : vendor_metadata[7] & 0x0F;
    vendor_datapath_config.insert(vendor_datapath_config.end(), &len, &len + 1);
    vendor_datapath_config.insert(vendor_datapath_config.end(), &type, &type + 1);
    vendor_datapath_config.insert(vendor_datapath_config.end(), &codec_ver, &codec_ver + 1);

    // Populate frequency
    uint32_t frequency =
        ase->codec_config.GetAsCoreCodecConfig().GetSamplingFrequencyHz();
    uint8_t freq = freq_to_ltv_map.count(frequency) ? freq_to_ltv_map.at(frequency) : 0;
    len = LTV_LEN_FREQ;
    type = LTV_TYPE_FREQ;
    vendor_datapath_config.insert(vendor_datapath_config.end(), &len, &len + 1);
    vendor_datapath_config.insert(vendor_datapath_config.end(), &type, &type + 1);
    vendor_datapath_config.insert(vendor_datapath_config.end(), &freq, &freq + 1);

    // Populate usecase number
    uint8_t usecase = HQ_AUDIO_USE_CASE;
    len = LTV_LEN_USE_CASE;
    type = LTV_TYPE_USE_CASE;
    usecase = audioContextToUseCase(context_type);
    vendor_datapath_config.insert(vendor_datapath_config.end(), &len, &len + 1);
    vendor_datapath_config.insert(vendor_datapath_config.end(), &type, &type + 1);
    vendor_datapath_config.insert(vendor_datapath_config.end(), &usecase, &usecase + 1);

    // Populated codec ID
    uint16_t vendor_company_id = ase->codec_id.vendor_company_id;
    uint16_t vendor_codec_id = ase->codec_id.vendor_codec_id;
    len = LTV_LEN_CODEC_ID;
    type = LTV_TYPE_CODEC_ID;
    vendor_datapath_config.insert(vendor_datapath_config.end(), &len, &len + 1);
    vendor_datapath_config.insert(vendor_datapath_config.end(), &type, &type + 1);
    vendor_datapath_config.insert(vendor_datapath_config.end(), &ase->codec_id.coding_format,
                                  &ase->codec_id.coding_format + 1);
    vendor_datapath_config.insert(vendor_datapath_config.end(), vendor_company_id);
    vendor_datapath_config.insert(vendor_datapath_config.end(), vendor_company_id >> 8);
    vendor_datapath_config.insert(vendor_datapath_config.end(), vendor_codec_id);
    vendor_datapath_config.insert(vendor_datapath_config.end(), vendor_codec_id >> 8);

    // Populate connection handle
    uint16_t connection_handle = conn_handles[0];
    len = LTV_LEN_CONN_HANDLE;
    type = LTV_TYPE_CONN_HANDLE;
    vendor_datapath_config.insert(vendor_datapath_config.end(), &len, &len + 1);
    vendor_datapath_config.insert(vendor_datapath_config.end(), &type, &type + 1);
    vendor_datapath_config.insert(vendor_datapath_config.end(), connection_handle);
    vendor_datapath_config.insert(vendor_datapath_config.end(), connection_handle >> 8);

    return vendor_datapath_config;
  }

  void SetAseState(LeAudioDevice* leAudioDevice, struct ase* ase,
                   AseState state) {
    log::info("{}, ase_id: {}, {} -> {}", leAudioDevice->address_, ase->id,
              ToString(ase->state), ToString(state));

    log_history_->AddLogHistory(
        kLogStateMachineTag, leAudioDevice->group_id_, leAudioDevice->address_,
        "ASE_ID " + std::to_string(ase->id) + ": " + kLogStateChangedOp,
        ToString(ase->state) + "->" + ToString(state));

    ase->state = state;
  }

  LeAudioDevice* getDeviceTryingToAttachTheStream(LeAudioDeviceGroup* group) {
    /* Device which is attaching the stream is just an active device not in
     * STREAMING state and NOT in  the RELEASING state.
     * The precondition is, that TargetState is Streaming
     */

    log::debug("group_id: {}, targetState: {}", group->group_id_,
               ToString(group->GetTargetState()));

    if (group->GetTargetState() != AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
      return nullptr;
    }

    for (auto dev = group->GetFirstActiveDevice(); dev != nullptr;
         dev = group->GetNextActiveDevice(dev)) {
      if (!dev->HaveAllActiveAsesSameState(AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) &&
          !dev->HaveAnyReleasingAse()) {
        log::debug("Attaching device {} to group_id: {}", dev->address_,
                   group->group_id_);
        return dev;
      }
    }
    return nullptr;
  }

  void AseStateMachineProcessIdle(
      struct bluetooth::le_audio::client_parser::ascs::ase_rsp_hdr& arh,
      struct ase* ase, LeAudioDeviceGroup* group,
      LeAudioDevice* leAudioDevice) {
    log::info("group_id: {}", group->group_id_);
    log::debug("ase state: {}", static_cast<int>(ase->state));
    switch (ase->state) {
      case AseState::BTA_LE_AUDIO_ASE_STATE_IDLE:
      case AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED:
        break;
      case AseState::BTA_LE_AUDIO_ASE_STATE_RELEASING: {
        SetAseState(leAudioDevice, ase, AseState::BTA_LE_AUDIO_ASE_STATE_IDLE);
        ase->active = false;
        ase->configured_for_context_type =
            bluetooth::le_audio::types::LeAudioContextType::UNINITIALIZED;

        if (!leAudioDevice->HaveAllActiveAsesSameState(
                AseState::BTA_LE_AUDIO_ASE_STATE_IDLE)) {
          /* More ASEs notification from this device has to come for this group
           */
          log::debug("Wait for more ASE to configure for device {}",
                     leAudioDevice->address_);
          return;
        }

        if (!group->HaveAllActiveDevicesAsesTheSameState(
                AseState::BTA_LE_AUDIO_ASE_STATE_IDLE)) {
          log::debug("Waiting for more devices to get into idle state");
          return;
        }

        /* Last node is in releasing state*/
        group->SetState(AseState::BTA_LE_AUDIO_ASE_STATE_IDLE);
        group->PrintDebugState();

        /* If all CISes are disconnected, notify upper layer about IDLE state,
         * otherwise wait for */
        if (!group->HaveAllCisesDisconnected() ||
            getDeviceTryingToAttachTheStream(group) != nullptr) {
          log::warn(
              "Not all CISes removed before going to IDLE for group {}, "
              "waiting...",
              group->group_id_);
          group->PrintDebugState();
          return;
        }

        cancel_watchdog_if_needed(group->group_id_);
        ReleaseCisIds(group);
        RemoveCigForGroup(group);
        group->SetTargetState(AseState::BTA_LE_AUDIO_ASE_STATE_IDLE);
        state_machine_callbacks_->StatusReportCb(group->group_id_,
                                                 GroupStreamStatus::IDLE);

        break;
      }
      case AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED:
      case AseState::BTA_LE_AUDIO_ASE_STATE_DISABLING:
        log::error(
            "Ignore invalid attempt of state transition from  {} to {}, {}, "
            "ase_id: {}",
            ToString(ase->state),
            ToString(AseState::BTA_LE_AUDIO_ASE_STATE_IDLE),
            leAudioDevice->address_, ase->id);
        group->PrintDebugState();
        break;
      case AseState::BTA_LE_AUDIO_ASE_STATE_ENABLING:
      case AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING:
        log::error(
            "Invalid state transition from {} to {}, {}, ase_id: {}. Stopping "
            "the stream.",
            ToString(ase->state),
            ToString(AseState::BTA_LE_AUDIO_ASE_STATE_IDLE),
            leAudioDevice->address_, ase->id);
        group->PrintDebugState();
        StopStream(group);
        break;
    }
  }

  void PrepareAndSendQoSToTheGroup(LeAudioDeviceGroup* group) {
    LeAudioDevice* leAudioDevice = group->GetFirstActiveDevice();
    if (!leAudioDevice) {
      log::error("No active device for the group");
      group->PrintDebugState();
      ClearGroup(group, true);
      return;
    }

    for (; leAudioDevice;
         leAudioDevice = group->GetNextActiveDevice(leAudioDevice)) {
      PrepareAndSendConfigQos(group, leAudioDevice);
    }
  }

  bool PrepareAndSendCodecConfigToTheGroup(LeAudioDeviceGroup* group) {
    log::info("group_id: {}", group->group_id_);
    auto leAudioDevice = group->GetFirstActiveDevice();
    if (!leAudioDevice) {
      log::error("No active device for the group");
      return false;
    }

    for (; leAudioDevice;
         leAudioDevice = group->GetNextActiveDevice(leAudioDevice)) {
      if (!group->cig.AssignCisIds(leAudioDevice)) {
        log::error("unable to assign CIS IDs");
        StopStream(group);
        return false;
      }
      PrepareAndSendCodecConfigure(group, leAudioDevice);
    }
    return true;
  }

  void PrepareAndSendCodecConfigure(LeAudioDeviceGroup* group,
                                    LeAudioDevice* leAudioDevice) {
    struct bluetooth::le_audio::client_parser::ascs::ctp_codec_conf conf;
    std::vector<struct bluetooth::le_audio::client_parser::ascs::ctp_codec_conf>
        confs;
    struct ase* ase;
    bool mFlagGattWriteUpdated = false;
    std::stringstream msg_stream;
    std::stringstream extra_stream;

    if (!group->cig.AssignCisIds(leAudioDevice)) {
      log::error("unable to assign CIS IDs");
      StopStream(group);
      return;
    }

    if (group->cig.GetState() == CigState::CREATED)
      group->AssignCisConnHandlesToAses(leAudioDevice);

    msg_stream << kLogAseConfigOp;

    ase = leAudioDevice->GetFirstActiveAse();
    log::assert_that(ase, "shouldn't be called without an active ASE");
    for (; ase != nullptr; ase = leAudioDevice->GetNextActiveAse(ase)) {
      log::debug("device: {}, ase_id: {}, cis_id: {}, ase state: {}",
                 leAudioDevice->address_, ase->id, ase->cis_id,
                 ToString(ase->state));
      conf.ase_id = ase->id;
      conf.target_latency = ase->target_latency;
      conf.target_phy = group->GetTargetPhy(ase->direction);
      conf.codec_id = ase->codec_id;

      if (!ase->vendor_codec_config.empty()) {
        log::debug("Using vendor codec configuration.");
        conf.codec_config = ase->vendor_codec_config;
      } else {
        conf.codec_config = ase->codec_config.RawPacket();
      }
      confs.push_back(conf);

      msg_stream << "ASE_ID " << +conf.ase_id << ",";
      if (ase->direction == bluetooth::le_audio::types::kLeAudioDirectionSink) {
        extra_stream << "snk,";
      } else {
        extra_stream << "src,";
      }
      extra_stream << +conf.codec_id.coding_format << ","
                   << +conf.target_latency << ";;";

      if (osi_property_get_bool("persist.bluetooth.leaudio.tmap_vrc_05_08", false)) {
         std::vector<uint8_t> value;
         bluetooth::le_audio::client_parser::ascs::PrepareAseCtpCodecConfig(confs,
                                                                            value);
         WriteToControlPoint(leAudioDevice, value);
         confs.pop_back();
         mFlagGattWriteUpdated = true;
      }
    }
    if (!mFlagGattWriteUpdated) {
       std::vector<uint8_t> value;
       bluetooth::le_audio::client_parser::ascs::PrepareAseCtpCodecConfig(confs,
                                                                            value);
       WriteToControlPoint(leAudioDevice, value);
    }
    log_history_->AddLogHistory(kLogControlPointCmd, group->group_id_,
                                leAudioDevice->address_, msg_stream.str(),
                                extra_stream.str());
  }

  void AseStateMachineProcessCodecConfigured(
      struct bluetooth::le_audio::client_parser::ascs::ase_rsp_hdr& arh,
      struct ase* ase, uint8_t* data, uint16_t len, LeAudioDeviceGroup* group,
      LeAudioDevice* leAudioDevice) {
    if (!group) {
      log::error("leAudioDevice doesn't belong to any group");

      return;
    }

    /* Internal helper for filling in the QoS parameters for an ASE, based
     * on the codec configure state and the prefferend ASE QoS parameters.
     * Note: The whole group state dependent parameters (out_cfg.framing, and
     *       out.cfg.presentation_delay) are calculated later, in the
     *       PrepareAndSendConfigQos(), once the whole group transitions to a
     *       proper state.
     */
    auto qos_config_update = [leAudioDevice](
                                 const struct bluetooth::le_audio::
                                     client_parser::ascs::
                                         ase_codec_configured_state_params& rsp,
                                 bluetooth::le_audio::types::AseQosPreferences&
                                     out_qos,
                                 bluetooth::le_audio::types::
                                     AseQosConfiguration& out_cfg) {
      out_qos.supported_framing = rsp.framing;
      out_qos.preferred_phy = rsp.preferred_phy;
      out_qos.preferred_retrans_nb = rsp.preferred_retrans_nb;
      out_qos.pres_delay_min = rsp.pres_delay_min;
      out_qos.pres_delay_max = rsp.pres_delay_max;
      out_qos.preferred_pres_delay_min = rsp.preferred_pres_delay_min;
      out_qos.preferred_pres_delay_max = rsp.preferred_pres_delay_max;

      log::debug(
            "Max Transport Latency: {}, "
            "rsp_mtl: {}, Retransmission Number: {}, Phy: {}, preferred_phy: {}",
            out_cfg.max_transport_latency, rsp.max_transport_latency,
            out_cfg.retrans_nb, out_cfg.phy, rsp.preferred_phy);

      /* Validate and update QoS to be consistent */
      if ((!out_cfg.max_transport_latency ||
           out_cfg.max_transport_latency > rsp.max_transport_latency) ||
          !out_cfg.retrans_nb) {
        out_cfg.max_transport_latency = rsp.max_transport_latency;
        out_cfg.retrans_nb = rsp.preferred_retrans_nb;
        log::info(
            "Using server preferred QoS settings. Max Transport Latency: {}, "
            "Retransmission Number: {}, Phy: {}",
            out_cfg.max_transport_latency, out_cfg.retrans_nb, out_cfg.phy);
      }
      if (!out_cfg.phy) {
        out_cfg.phy = leAudioDevice->GetPreferredPhyBitmask(rsp.preferred_phy);
        log::debug( "Using server preferred Phy: {}", out_cfg.phy);
      }
    };

    log::debug("ase state: {}", static_cast<int>(ase->state));

    /* ase contain current ASE state. New state is in "arh" */
    switch (ase->state) {
      case AseState::BTA_LE_AUDIO_ASE_STATE_IDLE: {
        struct bluetooth::le_audio::client_parser::ascs::
            ase_codec_configured_state_params rsp;

        /* Cache codec configured status values for further
         * configuration/reconfiguration
         */
        if (!ParseAseStatusCodecConfiguredStateParams(rsp, len, data)) {
          StopStream(group);
          return;
        }

        uint16_t cig_curr_max_trans_lat_mtos =
            group->GetMaxTransportLatencyMtos();
        uint16_t cig_curr_max_trans_lat_stom =
            group->GetMaxTransportLatencyStom();

        if (group->GetState() == AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
          /* We are here because of the reconnection of the single device.
           * Reconfigure CIG if current CIG supported Max Transport Latency for
           * a direction, cannot be supported by the newly connected member
           * device's ASE for the direction.
           */
          if ((ase->direction ==
                   bluetooth::le_audio::types::kLeAudioDirectionSink &&
               cig_curr_max_trans_lat_mtos > rsp.max_transport_latency) ||
              (ase->direction ==
                   bluetooth::le_audio::types::kLeAudioDirectionSource &&
               cig_curr_max_trans_lat_stom > rsp.max_transport_latency)) {
            group->SetPendingConfiguration();
            StopStream(group);
            return;
          }
        }

        qos_config_update(rsp, ase->qos_preferences, ase->qos_config);
        SetAseState(leAudioDevice, ase,
                    AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED);

        if (group->GetTargetState() == AseState::BTA_LE_AUDIO_ASE_STATE_IDLE) {
          /* This is autonomus change of the remote device */
          log::debug(
              "Autonomus change for device {}, ase id {}. Just store it.",
              leAudioDevice->address_, ase->id);

          /* Since at least one ASE is in configured state, we should admit
           * group is configured state */
          group->SetState(AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED);
          return;
        }

        if (leAudioDevice->HaveAnyUnconfiguredAses()) {
          /* More ASEs notification from this device has to come for this group
           */
          log::debug("More Ases to be configured for the device {}",
                     leAudioDevice->address_);
          return;
        }

        if (group->GetState() == AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
          /* We are here because of the reconnection of the single device. */
          /* Make sure that device is ready to be configured as we could also
           * get here triggered by the remote device. If device is not connected
           * yet, we should wait for the stack to trigger adding device to the
           * stream */
          if (leAudioDevice->GetConnectionState() ==
              bluetooth::le_audio::DeviceConnectState::CONNECTED) {
            PrepareAndSendConfigQos(group, leAudioDevice);
          } else {
            log::debug(
                "Device {} initiated configured state but it is not yet ready "
                "to be configured",
                leAudioDevice->address_);
          }
          return;
        }

        /* Configure ASEs for next device in group */
        if (group->HaveAnyActiveDeviceInUnconfiguredState()) {
          log::debug("Waiting for all the ASES in the Configured state");
          return;
        }

        /* Last node configured, process group to codec configured state */
        group->SetState(AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED);

        if (group->GetTargetState() ==
            AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
          if (group->cig.GetState() == CigState::CREATED) {
            /* It can happen on the earbuds switch scenario. When one device
             * is getting remove while other is adding to the stream and CIG is
             * already created */
            PrepareAndSendConfigQos(group, leAudioDevice);
          } else if (!CigCreate(group)) {
            log::error("Could not create CIG. Stop the stream for group {}",
                       group->group_id_);
            StopStream(group);
          }
          return;
        }

        if (group->GetTargetState() ==
                AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED &&
            group->IsPendingConfiguration()) {
          log::info("Configured state completed");

          /* If all CISes are disconnected, notify upper layer about IDLE
           * state, otherwise wait for */
          if (!group->HaveAllCisesDisconnected()) {
            log::warn(
                "Not all CISes removed before going to CONFIGURED for group "
                "{}, waiting...",
                group->group_id_);
            group->PrintDebugState();
            return;
          }

          group->ClearPendingConfiguration();
          state_machine_callbacks_->StatusReportCb(
              group->group_id_, GroupStreamStatus::CONFIGURED_BY_USER);

          /* No more transition for group */
          cancel_watchdog_if_needed(group->group_id_);
          return;
        }

        log::error(", invalid state transition, from: {} to {}",
                   ToString(group->GetState()),
                   ToString(group->GetTargetState()));
        StopStream(group);

        break;
      }
      case AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED: {
        /* Received Configured in Configured state. This could be done
         * autonomously because of the reconfiguration done by us
         */

        struct bluetooth::le_audio::client_parser::ascs::
            ase_codec_configured_state_params rsp;

        /* Cache codec configured status values for further
         * configuration/reconfiguration
         */
        if (!ParseAseStatusCodecConfiguredStateParams(rsp, len, data)) {
          StopStream(group);
          return;
        }

        /* This may be a notification from a re-configured ASE */
        ase->reconfigure = false;
        qos_config_update(rsp, ase->qos_preferences, ase->qos_config);

        if (leAudioDevice->HaveAnyUnconfiguredAses()) {
          /* Waiting for others to be reconfigured */
          return;
        }

        if (group->GetState() == AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
          /* We are here because of the reconnection of the single device. */
          /* Make sure that device is ready to be configured as we could also
           * get here triggered by the remote device. If device is not connected
           * yet, we should wait for the stack to trigger adding device to the
           * stream */
          if (leAudioDevice->GetConnectionState() ==
              bluetooth::le_audio::DeviceConnectState::CONNECTED) {
            PrepareAndSendConfigQos(group, leAudioDevice);
          } else {
            log::debug(
                "Device {} initiated configured state but it is not yet ready "
                "to be configured",
                leAudioDevice->address_);
          }
          return;
        }

        if (group->HaveAnyActiveDeviceInUnconfiguredState()) {
          log::debug(
              "Waiting for all the devices to be configured for group id {}",
              group->group_id_);
          return;
        }

        /* Last node configured, process group to codec configured state */
        group->SetState(AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED);

        if (group->GetTargetState() ==
            AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
          if (group->cig.GetState() == CigState::CREATED) {
            /* It can happen on the earbuds switch scenario. When one device
             * is getting remove while other is adding to the stream and CIG is
             * already created */
            PrepareAndSendConfigQos(group, leAudioDevice);
          } else if (!CigCreate(group)) {
            log::error("Could not create CIG. Stop the stream for group {}",
                       group->group_id_);
            StopStream(group);
          }
          return;
        }

        if (group->GetTargetState() ==
                AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED &&
            group->IsPendingConfiguration()) {
          log::info("Configured state completed");
          group->ClearPendingConfiguration();
          state_machine_callbacks_->StatusReportCb(
              group->group_id_, GroupStreamStatus::CONFIGURED_BY_USER);

          /* No more transition for group */
          cancel_watchdog_if_needed(group->group_id_);
          return;
        }

        if (group->GetTargetState() == AseState::BTA_LE_AUDIO_ASE_STATE_IDLE) {
          log::info("Cancel watchdog");
          cancel_watchdog_if_needed(group->group_id_);
          return;
        }

        log::info("Autonomous change, from: {} to {}",
                  ToString(group->GetState()),
                  ToString(group->GetTargetState()));

        break;
      }
      case AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED:
        SetAseState(leAudioDevice, ase,
                    AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED);
        group->PrintDebugState();
        break;
      case AseState::BTA_LE_AUDIO_ASE_STATE_DISABLING:
        log::error(
            "Ignore invalid attempt of state transition from {} to {}, {}, "
            "ase_id: {}",
            ToString(ase->state),
            ToString(AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED),
            leAudioDevice->address_, ase->id);
        group->PrintDebugState();
        break;
      case AseState::BTA_LE_AUDIO_ASE_STATE_RELEASING:
        SetAseState(leAudioDevice, ase,
                    AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED);
        ase->active = false;

        if (!leAudioDevice->HaveAllActiveAsesSameState(
                AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED)) {
          /* More ASEs notification from this device has to come for this group
           */
          log::debug("Wait for more ASE to configure for device {}",
                     leAudioDevice->address_);
          return;
        }

        {
          auto activeDevice = group->GetFirstActiveDevice();
          if (activeDevice) {
            log::debug(
                "There is at least one active device {}, wait to become "
                "inactive",
                activeDevice->address_);
            return;
          }
        }

        /* Last node is in releasing state*/
        group->SetState(AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED);
        /* Remote device has cache and keep staying in configured state after
         * release. Therefore, we assume this is a target state requested by
         * remote device.
         */
        group->SetTargetState(group->GetState());

        if (!group->HaveAllCisesDisconnected()) {
          log::warn(
              "Not all CISes removed before going to IDLE for group {}, "
              "waiting...",
              group->group_id_);
          group->PrintDebugState();
          return;
        }

        cancel_watchdog_if_needed(group->group_id_);
        ReleaseCisIds(group);
        RemoveCigForGroup(group);

        state_machine_callbacks_->StatusReportCb(
            group->group_id_, GroupStreamStatus::CONFIGURED_AUTONOMOUS);
        break;
      case AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING:
      case AseState::BTA_LE_AUDIO_ASE_STATE_ENABLING:
        log::error(
            "Invalid state transition from {} to {}, {}, ase_id: {}. Stopping "
            "the stream",
            ToString(ase->state),
            ToString(AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED),
            leAudioDevice->address_, ase->id);
        group->PrintDebugState();
        StopStream(group);
        break;
    }
  }

  void AseStateMachineProcessQosConfigured(
      struct bluetooth::le_audio::client_parser::ascs::ase_rsp_hdr& arh,
      struct ase* ase, LeAudioDeviceGroup* group,
      LeAudioDevice* leAudioDevice) {
    if (!group) {
      log::error("leAudioDevice doesn't belong to any group");

      return;
    }

    log::debug("ase state: {}", static_cast<int>(ase->state));

    switch (ase->state) {
      case AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED:
        log::info(
            "Unexpected state transition from {} to {}, {}, ase_id: {}, "
            "fallback to transition from {} to {}",
            ToString(ase->state),
            ToString(AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED),
            leAudioDevice->address_, ase->id,
            ToString(AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED),
            ToString(AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED));
        group->PrintDebugState();
        FMT_FALLTHROUGH;
      case AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED: {
        SetAseState(leAudioDevice, ase,
                    AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED);

        if (!leAudioDevice->HaveAllActiveAsesSameState(
                AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED)) {
          /* More ASEs notification from this device has to come for this group
           */
          return;
        }

        if (group->GetState() == AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING ||
            (group->GetTargetState() == AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING &&
                                       group->cig.GetState() == CigState::CREATED)) {
          /* We are here because of the reconnection of the single device. */
          PrepareAndSendEnable(leAudioDevice);
          return;
        }

        if (!group->HaveAllActiveDevicesAsesTheSameState(
                AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED)) {
          log::debug("Waiting for all the devices to be in QoS state");
          return;
        }

        PrepareAndSendEnableToTheGroup(group);

        break;
      }
      case AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING:
        if (ase->direction ==
            bluetooth::le_audio::types::kLeAudioDirectionSource) {
          /* Source ASE cannot go from Streaming to QoS Configured state */
          log::error("invalid state transition, from: {}, to: {}",
                     static_cast<int>(ase->state),
                     static_cast<int>(
                         AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED));
          StopStream(group);
          return;
        }

        SetAseState(leAudioDevice, ase,
                    AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED);

        /* Remote may autonomously bring ASEs to QoS configured state */
        if (group->GetTargetState() !=
            AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED) {
          ProcessAutonomousDisable(leAudioDevice, ase);
        }

        /* Process the Disable Transition of the rest of group members if no
         * more ASE notifications has to come from this device. */
        if (leAudioDevice->IsReadyToSuspendStream()) ProcessGroupDisable(group);

        break;

      case AseState::BTA_LE_AUDIO_ASE_STATE_DISABLING: {
        SetAseState(leAudioDevice, ase,
                    AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED);

        /* More ASEs notification from this device has to come for this group */
        if (!group->HaveAllActiveDevicesAsesTheSameState(
                AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED))
          return;

        group->SetState(AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED);

        if (!group->HaveAllCisesDisconnected()) return;

        if (group->GetTargetState() ==
            AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED) {
          /* No more transition for group */
          cancel_watchdog_if_needed(group->group_id_);

          state_machine_callbacks_->StatusReportCb(
              group->group_id_, GroupStreamStatus::SUSPENDED);
        } else {
          log::error(", invalid state transition, from: {}, to: {}",
                     ToString(group->GetState()),
                     ToString(group->GetTargetState()));
          StopStream(group);
          return;
        }
        break;
      }
      case AseState::BTA_LE_AUDIO_ASE_STATE_IDLE:
      case AseState::BTA_LE_AUDIO_ASE_STATE_RELEASING:
        // Do nothing here, just print an error message
        log::error(
            "Ignore invalid attempt of state transition from {} to {}, {}, "
            "ase_id: {}",
            ToString(ase->state),
            ToString(AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED),
            leAudioDevice->address_, ase->id);
        group->PrintDebugState();
        break;
      case AseState::BTA_LE_AUDIO_ASE_STATE_ENABLING:
        log::error(
            "Invalid state transition from {} to {}, {}, ase_id: {}. Stopping "
            "the stream.",
            ToString(ase->state),
            ToString(AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED),
            leAudioDevice->address_, ase->id);
        StopStream(group);
        break;
    }
  }

  void ClearGroup(LeAudioDeviceGroup* group, bool report_idle_state) {
    log::debug("group_id: {}", group->group_id_);
    group->SetState(AseState::BTA_LE_AUDIO_ASE_STATE_IDLE);
    group->SetTargetState(AseState::BTA_LE_AUDIO_ASE_STATE_IDLE);

    /* Clear group pending status */
    group->ClearPendingAvailableContextsChange();
    group->ClearPendingConfiguration();

    cancel_watchdog_if_needed(group->group_id_);
    ReleaseCisIds(group);
    RemoveCigForGroup(group);

    if (report_idle_state) {
      state_machine_callbacks_->StatusReportCb(group->group_id_,
                                               GroupStreamStatus::IDLE);
    }
  }

  void PrepareAndSendEnableToTheGroup(LeAudioDeviceGroup* group) {
    log::info("group_id: {}", group->group_id_);

    auto leAudioDevice = group->GetFirstActiveDevice();
    if (!leAudioDevice) {
      log::error("No active device for the group");
      group->PrintDebugState();
      ClearGroup(group, true);
      return;
    }

    for (; leAudioDevice;
         leAudioDevice = group->GetNextActiveDevice(leAudioDevice)) {
      PrepareAndSendEnable(leAudioDevice);
    }
  }

  void PrepareAndSendEnable(LeAudioDevice* leAudioDevice) {
    struct bluetooth::le_audio::client_parser::ascs::ctp_enable conf;
    std::vector<struct bluetooth::le_audio::client_parser::ascs::ctp_enable>
        confs;
    std::vector<uint8_t> value;
    struct ase* ase;
    std::stringstream msg_stream;
    std::stringstream extra_stream;

    msg_stream << kLogAseEnableOp;

    ase = leAudioDevice->GetFirstActiveAse();
    if (osi_property_get_bool("persist.bluetooth.leaudio.bap_enableQoS", false)) {
       ase = leAudioDevice->GetNextActiveAse(ase);
    }

    if (flag_sendenableLater) {
      log::debug("sending enable for 2nd ase");
      //ase = leAudioDevice->GetNextActiveAse(ase);
      ase = leAudioDevice->GetFirstActiveAse();
    }

    log::assert_that(ase, "shouldn't be called without an active ASE");
    do {
      log::debug("device: {}, ase_id: {}, cis_id: {}, ase state: {}",
                 leAudioDevice->address_, ase->id, ase->cis_id,
                 ToString(ase->state));
      conf.ase_id = ase->id;
      conf.metadata = ase->metadata;
      confs.push_back(conf);

      /* Below is just for log history */
      msg_stream << "ASE_ID " << +ase->id << ",";
      extra_stream << "meta: "
                   << base::HexEncode(conf.metadata.data(),
                                      conf.metadata.size())
                   << ";;";
      if (osi_property_get_bool("persist.bluetooth.leaudio.bap_enableQoS", false)) {
         flag_sendenableLater = true;
         break;
      }
    } while ((ase = leAudioDevice->GetNextActiveAse(ase)));

    bluetooth::le_audio::client_parser::ascs::PrepareAseCtpEnable(confs, value);
    WriteToControlPoint(leAudioDevice, value);

    log::info("group_id: {}, {}", leAudioDevice->group_id_,
              leAudioDevice->address_);
    log_history_->AddLogHistory(kLogControlPointCmd, leAudioDevice->group_id_,
                                leAudioDevice->address_, msg_stream.str(),
                                extra_stream.str());
  }

  GroupStreamStatus PrepareAndSendDisableToTheGroup(LeAudioDeviceGroup* group) {
    log::info("grop_id: {}", group->group_id_);

    auto leAudioDevice = group->GetFirstActiveDevice();
    if (!leAudioDevice) {
      log::error("No active device for the group");
      group->PrintDebugState();
      ClearGroup(group, false);
      return GroupStreamStatus::IDLE;
    }

    for (; leAudioDevice;
         leAudioDevice = group->GetNextActiveDevice(leAudioDevice)) {
      PrepareAndSendDisable(leAudioDevice);
    }
    return GroupStreamStatus::SUSPENDING;
  }

  void PrepareAndSendDisable(LeAudioDevice* leAudioDevice) {
    ase* ase = leAudioDevice->GetFirstActiveAse();
    log::assert_that(ase, "shouldn't be called without an active ASE");

    std::stringstream msg_stream;
    msg_stream << kLogAseDisableOp;

    std::vector<uint8_t> ids;
    do {
      log::debug("device: {}, ase_id: {}, cis_id: {}, ase state: {}",
                 leAudioDevice->address_, ase->id, ase->cis_id,
                 ToString(ase->state));
      ids.push_back(ase->id);

      msg_stream << "ASE_ID " << +ase->id << ", ";
    } while ((ase = leAudioDevice->GetNextActiveAse(ase)));

    log::info("group_id: {}, {}", leAudioDevice->group_id_,
              leAudioDevice->address_);
    std::vector<uint8_t> value;
    bluetooth::le_audio::client_parser::ascs::PrepareAseCtpDisable(ids, value);

    WriteToControlPoint(leAudioDevice, value);

    log_history_->AddLogHistory(kLogControlPointCmd, leAudioDevice->group_id_,
                                leAudioDevice->address_, msg_stream.str());
  }

  GroupStreamStatus PrepareAndSendReleaseToTheGroup(LeAudioDeviceGroup* group) {
    log::info("group_id: {}", group->group_id_);
    LeAudioDevice* leAudioDevice = group->GetFirstActiveDevice();
    if (!leAudioDevice) {
      log::error("No active device for the group");
      group->PrintDebugState();
      ClearGroup(group, false);
      return GroupStreamStatus::IDLE;
    }

    for (; leAudioDevice;
         leAudioDevice = group->GetNextActiveDevice(leAudioDevice)) {
      PrepareAndSendRelease(leAudioDevice);
    }

    return GroupStreamStatus::RELEASING;
  }

  void PrepareAndSendRelease(LeAudioDevice* leAudioDevice) {
    ase* ase = leAudioDevice->GetFirstActiveAse();
    log::assert_that(ase, "shouldn't be called without an active ASE");

    std::vector<uint8_t> ids;
    std::stringstream stream;
    stream << kLogAseReleaseOp;

    do {
      log::debug("device: {}, ase_id: {}, cis_id: {}, ase state: {}",
                 leAudioDevice->address_, ase->id, ase->cis_id,
                 ToString(ase->state));
      ids.push_back(ase->id);
      stream << "ASE_ID " << +ase->id << ",";
    } while ((ase = leAudioDevice->GetNextActiveAse(ase)));

    std::vector<uint8_t> value;
    bluetooth::le_audio::client_parser::ascs::PrepareAseCtpRelease(ids, value);
    WriteToControlPoint(leAudioDevice, value);

    log::info("group_id: {}, {}", leAudioDevice->group_id_,
              leAudioDevice->address_);
    log_history_->AddLogHistory(kLogControlPointCmd, leAudioDevice->group_id_,
                                leAudioDevice->address_, stream.str());
  }

  void PrepareAndSendConfigQos(LeAudioDeviceGroup* group,
                               LeAudioDevice* leAudioDevice) {
    std::vector<struct bluetooth::le_audio::client_parser::ascs::ctp_qos_conf>
        confs;

    bool validate_transport_latency = false;
    bool validate_max_sdu_size = false;
    bool mFlagGattWriteUpdated = false;

    std::stringstream msg_stream;
    msg_stream << kLogAseQoSConfigOp;

    std::stringstream extra_stream;

    for (struct ase* ase = leAudioDevice->GetFirstActiveAse(); ase != nullptr;
         ase = leAudioDevice->GetNextActiveAse(ase)) {
      log::debug("device: {}, ase_id: {}, cis_id: {}, ase state: {}",
                 leAudioDevice->address_, ase->id, ase->cis_id,
                 ToString(ase->state));

      /* Fill in the whole group dependent ASE parameters */
      if (!group->GetPresentationDelay(&ase->qos_config.presentation_delay,
                                       ase->direction)) {
        log::error("inconsistent presentation delay for group");
        group->PrintDebugState();
        StopStream(group);
        return;
      }
      ase->qos_config.framing = group->GetFraming();

      struct bluetooth::le_audio::client_parser::ascs::ctp_qos_conf conf;
      conf.ase_id = ase->id;
      conf.cig = group->group_id_;
      conf.cis = ase->cis_id;
      conf.framing = ase->qos_config.framing;
      conf.phy = ase->qos_config.phy;
      conf.max_sdu = ase->qos_config.max_sdu_size;
      conf.retrans_nb = ase->qos_config.retrans_nb;
      conf.pres_delay = ase->qos_config.presentation_delay;
      conf.sdu_interval = ase->qos_config.sdu_interval;

      if (!conf.sdu_interval) {
        log::error("unsupported SDU interval for group");
        group->PrintDebugState();
        StopStream(group);
        return;
      }

      msg_stream << "ASE " << +conf.ase_id << ",";
      if (ase->direction == bluetooth::le_audio::types::kLeAudioDirectionSink) {
        conf.max_transport_latency = group->GetMaxTransportLatencyMtos();
        extra_stream << "snk,";
      } else {
        conf.max_transport_latency = group->GetMaxTransportLatencyStom();
        extra_stream << "src,";
      }

      if (conf.max_transport_latency >
          bluetooth::le_audio::types::kMaxTransportLatencyMin) {
        validate_transport_latency = true;
      }

      if (conf.max_sdu > 0) {
        validate_max_sdu_size = true;
      }
      confs.push_back(conf);

      // dir...cis_id,sdu,lat,rtn,phy,frm;;
      extra_stream << +conf.cis << "," << +conf.max_sdu << ","
                   << +conf.max_transport_latency << "," << +conf.retrans_nb
                   << "," << +conf.phy << "," << +conf.framing << ";;";

      if (confs.size() == 0 || !validate_transport_latency ||
          !validate_max_sdu_size) {
        log::error("Invalid configuration or latency or sdu size");
        group->PrintDebugState();
        StopStream(group);
        return;
      }
      if (osi_property_get_bool("persist.bluetooth.leaudio.tmap_vrc_05_08", false)) {
        std::vector<uint8_t> value;
        bluetooth::le_audio::client_parser::ascs::PrepareAseCtpConfigQos(confs,
                                                                       value);
        WriteToControlPoint(leAudioDevice, value);
        confs.pop_back();
        mFlagGattWriteUpdated =  true;
      }
    }

    if (!mFlagGattWriteUpdated) {
       std::vector<uint8_t> value;
       bluetooth::le_audio::client_parser::ascs::PrepareAseCtpConfigQos(confs,
                                                                      value);
       WriteToControlPoint(leAudioDevice, value);
    }
    log::info("group_id: {}, {}", leAudioDevice->group_id_,
              leAudioDevice->address_);
    log_history_->AddLogHistory(kLogControlPointCmd, group->group_id_,
                                leAudioDevice->address_, msg_stream.str(),
                                extra_stream.str());
  }

  void PrepareAndSendUpdateMetadata(
      LeAudioDevice* leAudioDevice,
      const BidirectionalPair<AudioContexts>& context_types,
      const BidirectionalPair<std::vector<uint8_t>>& ccid_lists) {
    std::vector<
        struct bluetooth::le_audio::client_parser::ascs::ctp_update_metadata>
        confs;

    std::stringstream msg_stream;
    msg_stream << kLogAseUpdateMetadataOp;

    std::stringstream extra_stream;

    if (!leAudioDevice->IsMetadataChanged(context_types, ccid_lists)) return;

    std::vector<uint16_t> conn_handles;
    AudioContexts ctx_type;

    /* Request server to update ASEs with new metadata */
    for (struct ase* ase = leAudioDevice->GetFirstActiveAse(); ase != nullptr;
         ase = leAudioDevice->GetNextActiveAse(ase)) {
      log::debug("device: {}, ase_id: {}, cis_id: {}, ase state: {}",
                 leAudioDevice->address_, ase->id, ase->cis_id,
                 ToString(ase->state));

      if (ase->state != AseState::BTA_LE_AUDIO_ASE_STATE_ENABLING &&
          ase->state != AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
        /* This might happen when update metadata happens on late connect */
        log::debug(
            "Metadata for ase_id {} cannot be updated due to invalid ase state "
            "- see log above",
            ase->id);
        continue;
      }

      msg_stream << "ASE_ID " << +ase->id << ",";
      if (ase->direction == bluetooth::le_audio::types::kLeAudioDirectionSink) {
        extra_stream << "snk,";
      } else {
        extra_stream << "src,";
      }

      /* Filter multidirectional audio context for each ase direction */
      auto directional_audio_context =
          context_types.get(ase->direction) &
          leAudioDevice->GetAvailableContexts(ase->direction);

      std::vector<uint8_t> new_metadata;
      if (directional_audio_context.any()) {
        new_metadata = leAudioDevice->GetMetadata(
            directional_audio_context, ccid_lists.get(ase->direction));
      } else {
        new_metadata = leAudioDevice->GetMetadata(
            AudioContexts(LeAudioContextType::UNSPECIFIED),
            std::vector<uint8_t>());
      }

      /* Do not update if metadata did not changed. */
      if (ase->metadata == new_metadata) {
        continue;
      }

      ase->metadata = new_metadata;

      struct bluetooth::le_audio::client_parser::ascs::ctp_update_metadata conf;

      conf.ase_id = ase->id;
      conf.metadata = ase->metadata;
      confs.push_back(conf);
      conn_handles.push_back(ase->cis_conn_hdl);
      ctx_type = directional_audio_context;

      extra_stream << "meta: "
                   << base::HexEncode(conf.metadata.data(),
                                      conf.metadata.size())
                   << ";;";
    }

    if (confs.size() != 0) {
      std::vector<uint8_t> value;
      bluetooth::le_audio::client_parser::ascs::PrepareAseCtpUpdateMetadata(
          confs, value);
      WriteToControlPoint(leAudioDevice, value);

      send_vs_cmd(static_cast<uint16_t>(ctx_type.value()),
          leAudioDevice->group_id_, conn_handles.size(), conn_handles,
          leAudioDevice->isLeXDevice());

      log::info("group_id: {}, {}", leAudioDevice->group_id_,
                leAudioDevice->address_);

      log_history_->AddLogHistory(kLogControlPointCmd, leAudioDevice->group_id_,
                                  leAudioDevice->address_, msg_stream.str(),
                                  extra_stream.str());
    }
  }

  void PrepareAndSendReceiverStartReady(LeAudioDevice* leAudioDevice,
                                        struct ase* ase) {
    std::vector<uint8_t> ids;
    std::vector<uint8_t> value;
    std::stringstream stream;

    stream << kLogAseStartReadyOp;

    do {
      if (ase->direction ==
          bluetooth::le_audio::types::kLeAudioDirectionSource) {
        stream << "ASE_ID " << +ase->id << ",";
        ids.push_back(ase->id);
      }
    } while ((ase = leAudioDevice->GetNextActiveAse(ase)));

    if (ids.size() > 0) {
      bluetooth::le_audio::client_parser::ascs::
          PrepareAseCtpAudioReceiverStartReady(ids, value);
      WriteToControlPoint(leAudioDevice, value);

      log::info("group_id: {}, {}", leAudioDevice->group_id_,
                leAudioDevice->address_);
      log_history_->AddLogHistory(kLogControlPointCmd, leAudioDevice->group_id_,
                                  leAudioDevice->address_, stream.str());
    }
  }

  void AseStateMachineProcessEnabling(
      struct bluetooth::le_audio::client_parser::ascs::ase_rsp_hdr& arh,
      struct ase* ase, uint8_t* data, uint16_t len, LeAudioDeviceGroup* group,
      LeAudioDevice* leAudioDevice) {
    if (!group) {
      log::error("leAudioDevice doesn't belong to any group");
      return;
    }

    log::debug("ase state: {}", static_cast<int>(ase->state));

    switch (ase->state) {
      case AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED:
        SetAseState(leAudioDevice, ase,
                    AseState::BTA_LE_AUDIO_ASE_STATE_ENABLING);

        {
          struct le_audio::client_parser::ascs::ase_transient_state_params rsp;

          if (ParseAseStatusTransientStateParams(rsp, len, data)) {
            parseVSMetadata(rsp.metadata.size(), rsp.metadata, rsp.cig_id,
                rsp.cis_id, ase);
          }
        }

        if (group->GetState() == AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
          if (flag_sendenableLater) {
            if (osi_property_get_bool("persist.bluetooth.leaudio.bap_enableQoS_src", false)) {
              log::debug("Sending Start ready for 2nd ASE");
              PrepareAndSendReceiverStartReady(leAudioDevice, ase);
              return;
            }
          }
          if (ase->cis_state < CisState::CONNECTING) {
            /* We are here because of the reconnection of the single device. */
            if (!CisCreateForDevice(group, leAudioDevice)) {
              StopStream(group);
              return;
            }
          }

          if (!leAudioDevice->HaveAllActiveAsesCisEst()) {
            /* More cis established events has to come */
            return;
          }

          if (!leAudioDevice->IsReadyToCreateStream()) {
            /* Device still remains in ready to create stream state. It means
             * that more enabling status notifications has to come.
             */
            return;
          }

          /* All CISes created. Send start ready for source ASE before we can go
           * to streaming state.
           */
          struct ase* ase = leAudioDevice->GetFirstActiveAse();
          log::assert_that(
              ase != nullptr,
              "shouldn't be called without an active ASE, device {}",
              leAudioDevice->address_.ToString());
          PrepareAndSendReceiverStartReady(leAudioDevice, ase);

          return;
        }

        if (leAudioDevice->IsReadyToCreateStream() || flag_sendenableLater)
          log::error("processing Enable to group");
          ProcessGroupEnable(group);

        break;

      case AseState::BTA_LE_AUDIO_ASE_STATE_ENABLING:
        /* Enable/Switch Content */
        break;
      default:
        log::error("invalid state transition, from: {}, to: {}",
                   static_cast<int>(ase->state),
                   static_cast<int>(AseState::BTA_LE_AUDIO_ASE_STATE_ENABLING));
        StopStream(group);
        break;
    }
  }

  void AseStateMachineProcessStreaming(
      struct bluetooth::le_audio::client_parser::ascs::ase_rsp_hdr& arh,
      struct ase* ase, uint8_t* data, uint16_t len, LeAudioDeviceGroup* group,
      LeAudioDevice* leAudioDevice) {
    if (!group) {
      log::error("leAudioDevice doesn't belong to any group");

      return;
    }

    log::debug("ase state: {}", static_cast<int>(ase->state));

    switch (ase->state) {
      case AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED:
        log::error(
            "{}, ase_id: {}, moving from QoS Configured to Streaming is "
            "impossible.",
            leAudioDevice->address_, ase->id);
        group->PrintDebugState();
        StopStream(group);
        break;

      case AseState::BTA_LE_AUDIO_ASE_STATE_ENABLING: {
        std::vector<uint8_t> value;

        SetAseState(leAudioDevice, ase,
                    AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING);

        if (!group->HaveAllActiveDevicesAsesTheSameState(
                AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) && !flag_sendenableLater) {
          /* More ASEs notification form this device has to come for this group
           */
          return;
        }

        if (group->GetState() == AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
          /* We are here because of the reconnection of the single device */
          log::info("{}, Ase id: {}, ase state: {}", leAudioDevice->address_,
                    ase->id, bluetooth::common::ToString(ase->state));
          cancel_watchdog_if_needed(group->group_id_);
          state_machine_callbacks_->StatusReportCb(
              group->group_id_, GroupStreamStatus::STREAMING);
          return;
        }

        /* Not all CISes establish events will came */
        if (!group->IsGroupStreamReady() && !flag_sendenableLater) {
          log::info("CISes are not yet ready, wait for it.");
          group->SetNotifyStreamingWhenCisesAreReadyFlag(true);
          return;
        }

        if (group->GetTargetState() ==
            AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING || flag_sendenableLater) {
          /* No more transition for group */
          cancel_watchdog_if_needed(group->group_id_);

          /* Last node is in streaming state */
          group->SetState(AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING);

          state_machine_callbacks_->StatusReportCb(
              group->group_id_, GroupStreamStatus::STREAMING);
          return;
        }

        log::error(", invalid state transition, from: {}, to: {}",
                   ToString(group->GetState()),
                   ToString(group->GetTargetState()));
        StopStream(group);

        break;
      }
      case AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING: {
        struct bluetooth::le_audio::client_parser::ascs::
            ase_transient_state_params rsp;

        if (!ParseAseStatusTransientStateParams(rsp, len, data)) {
          StopStream(group);
          return;
        }

        parseVSMetadata(rsp.metadata.size(), rsp.metadata,
            rsp.cig_id, rsp.cis_id, ase);
        /* Cache current set up metadata values for for further possible
         * reconfiguration
         */
        if (!rsp.metadata.empty()) {
          ase->metadata = rsp.metadata;
        }

        break;
      }
      default:
        log::error(
            "invalid state transition, from: {}, to: {}",
            static_cast<int>(ase->state),
            static_cast<int>(AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING));
        StopStream(group);
        break;
    }
  }

  void ScheduleAutonomousOperationTimer(AseState target_state,
                                        LeAudioDevice* leAudioDevice,
                                        struct ase* ase) {
    ase->autonomous_target_state_ = target_state;
    ase->autonomous_operation_timer_ =
        alarm_new("LeAudioAutonomousOperationTimeout");
    alarm_set_on_mloop(
        ase->autonomous_operation_timer_, kAutonomousTransitionTimeoutMs,
        [](void* data) {
          LeAudioDevice* leAudioDevice = static_cast<LeAudioDevice*>(data);
          instance->state_machine_callbacks_
              ->OnDeviceAutonomousStateTransitionTimeout(leAudioDevice);
        },
        leAudioDevice);
  }

  void AseStateMachineProcessDisabling(
      struct bluetooth::le_audio::client_parser::ascs::ase_rsp_hdr& arh,
      struct ase* ase, LeAudioDeviceGroup* group,
      LeAudioDevice* leAudioDevice) {
    if (!group) {
      log::error("leAudioDevice doesn't belong to any group");

      return;
    }

    if (ase->direction == bluetooth::le_audio::types::kLeAudioDirectionSink) {
      /* Sink ASE state machine does not have Disabling state */
      log::error(", invalid state transition, from: {} , to: {}",
                 ToString(group->GetState()),
                 ToString(group->GetTargetState()));
      StopStream(group);
      return;
    }

    log::debug("ase state: {}", static_cast<int>(ase->state));

    switch (ase->state) {
      case AseState::BTA_LE_AUDIO_ASE_STATE_ENABLING:
        /* TODO: Disable */
        break;
      case AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING:
        SetAseState(leAudioDevice, ase,
                    AseState::BTA_LE_AUDIO_ASE_STATE_DISABLING);

        /* Remote may autonomously bring ASEs to QoS configured state */
        if (group->GetTargetState() !=
            AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED) {
          ProcessAutonomousDisable(leAudioDevice, ase);
        }

        /* Process the Disable Transition of the rest of group members if no
         * more ASE notifications has to come from this device. */
        if (leAudioDevice->IsReadyToSuspendStream()) ProcessGroupDisable(group);

        break;

      default:
        log::error(
            "invalid state transition, from: {}, to: {}",
            static_cast<int>(ase->state),
            static_cast<int>(AseState::BTA_LE_AUDIO_ASE_STATE_DISABLING));
        StopStream(group);
        break;
    }
  }

  void DisconnectCisIfNeeded(LeAudioDeviceGroup* group,
                             LeAudioDevice* leAudioDevice, struct ase* ase) {
    log::debug(
        "Group id: {}, {}, ase id: {}, cis_handle: 0x{:04x}, direction: {}, "
        "data_path_state: {}, cis_state: {}",
        group->group_id_, leAudioDevice->address_, ase->id, ase->cis_conn_hdl,
        ase->direction == bluetooth::le_audio::types::kLeAudioDirectionSink
            ? "sink"
            : "source",
        bluetooth::common::ToString(ase->data_path_state),
        bluetooth::common::ToString(ase->cis_state));

    auto bidirection_ase = leAudioDevice->GetAseToMatchBidirectionCis(ase);
    if (bidirection_ase != nullptr &&
        bidirection_ase->cis_state == CisState::CONNECTED &&
        (bidirection_ase->state == AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING ||
         bidirection_ase->state == AseState::BTA_LE_AUDIO_ASE_STATE_ENABLING)) {
      log::info(
          "Still waiting for the bidirectional ase {} to be released ({})",
          bidirection_ase->id,
          bluetooth::common::ToString(bidirection_ase->state));
      return;
    }

    group->RemoveCisFromStreamIfNeeded(leAudioDevice, ase->cis_conn_hdl);
    IsoManager::GetInstance()->DisconnectCis(ase->cis_conn_hdl,
                                             HCI_ERR_PEER_USER);
    log_history_->AddLogHistory(
        kLogStateMachineTag, group->group_id_, leAudioDevice->address_,
        kLogCisDisconnectOp + "cis_h:" + loghex(ase->cis_conn_hdl));
  }

  void AseStateMachineProcessReleasing(
      struct bluetooth::le_audio::client_parser::ascs::ase_rsp_hdr& arh,
      struct ase* ase, LeAudioDeviceGroup* group,
      LeAudioDevice* leAudioDevice) {
    if (!group) {
      log::error("leAudioDevice doesn't belong to any group");

      return;
    }

    log::debug("ase state: {}", static_cast<int>(ase->state));

    switch (ase->state) {
      case AseState::BTA_LE_AUDIO_ASE_STATE_DISABLING:
      case AseState::BTA_LE_AUDIO_ASE_STATE_CODEC_CONFIGURED:
        SetAseState(leAudioDevice, ase,
                    AseState::BTA_LE_AUDIO_ASE_STATE_RELEASING);

        if (group->HaveAllActiveDevicesAsesTheSameState(
                AseState::BTA_LE_AUDIO_ASE_STATE_RELEASING)) {
          group->SetState(AseState::BTA_LE_AUDIO_ASE_STATE_RELEASING);
        }

        if (group->cig.GetState() == CigState::CREATED &&
            group->HaveAllCisesDisconnected() &&
            getDeviceTryingToAttachTheStream(group) == nullptr) {
          RemoveCigForGroup(group);
        }

        break;

      case AseState::BTA_LE_AUDIO_ASE_STATE_ENABLING: {
        SetAseState(leAudioDevice, ase,
                    AseState::BTA_LE_AUDIO_ASE_STATE_RELEASING);

        bool remove_cig = true;

        log::debug("cis_state: {}", static_cast<int>(ase->cis_state));
        log::debug("data_path_state: {}", static_cast<int>(ase->data_path_state));

        /* Happens when bi-directional completive ASE releasing state came */
        if (ase->cis_state == CisState::DISCONNECTING) break;

        if (ase->data_path_state == DataPathState::CONFIGURED) {
          RemoveDataPathByCisHandle(leAudioDevice, ase->cis_conn_hdl);
          remove_cig = false;
        } else if ((ase->cis_state == CisState::CONNECTED ||
             ase->cis_state == CisState::CONNECTING) &&
            ase->data_path_state == DataPathState::IDLE) {
          DisconnectCisIfNeeded(group, leAudioDevice, ase);
          /* CISes are still there. CIG will be removed when CIS is down. */
          remove_cig = false;
        }

        if (!group->HaveAllActiveDevicesAsesTheSameState(
                AseState::BTA_LE_AUDIO_ASE_STATE_RELEASING)) {
          return;
        }
        group->SetState(AseState::BTA_LE_AUDIO_ASE_STATE_RELEASING);

        if (remove_cig) {
          /* In the ENABLING state most probably there was no CISes created.
           * Make sure group is destroyed here */
          RemoveCigForGroup(group);
        }
        break;
      }
      case AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED:
      case AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING: {
        SetAseState(leAudioDevice, ase,
                    AseState::BTA_LE_AUDIO_ASE_STATE_RELEASING);

        log::debug("cis_state: {}", static_cast<int>(ase->cis_state));
        log::debug("data_path_state: {}", static_cast<int>(ase->data_path_state));

        /* Happens when bi-directional completive ASE releasing state came */
        if (ase->cis_state == CisState::DISCONNECTING) break;

        if (ase->data_path_state == DataPathState::CONFIGURED) {
          RemoveDataPathByCisHandle(leAudioDevice, ase->cis_conn_hdl);
        } else if ((ase->cis_state == CisState::CONNECTED ||
                    ase->cis_state == CisState::CONNECTING) &&
                   ase->data_path_state == DataPathState::IDLE) {
          DisconnectCisIfNeeded(group, leAudioDevice, ase);
        } else {
          log::debug("Nothing to do ase data path state: {}",
                     static_cast<int>(ase->data_path_state));
        }

        if (group->HaveAllActiveDevicesAsesTheSameState(
                    AseState::BTA_LE_AUDIO_ASE_STATE_RELEASING)) {
          group->SetState(AseState::BTA_LE_AUDIO_ASE_STATE_RELEASING);
          if (group->GetTargetState() == AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
            log::info("Group {} is doing autonomous release", group->group_id_);
            SetTargetState(group, AseState::BTA_LE_AUDIO_ASE_STATE_IDLE);
            state_machine_callbacks_->StatusReportCb(group->group_id_,
                                                     GroupStreamStatus::RELEASING_AUTONOMOUS);
          }
        }

        break;
      }
      default:
        log::error(
            "invalid state transition, from: {}, to: {}",
            static_cast<int>(ase->state),
            static_cast<int>(AseState::BTA_LE_AUDIO_ASE_STATE_RELEASING));
        break;
    }
  }

  void ProcessGroupEnable(LeAudioDeviceGroup* group) {
    if (group->GetState() != AseState::BTA_LE_AUDIO_ASE_STATE_ENABLING) {
       log::debug("is state not in enabling");
      /* Check if the group is ready to create stream. If not, keep waiting. */
      if (!group->IsGroupReadyToCreateStream() && !flag_sendenableLater) {
        log::debug(
            "Waiting for more ASEs to be in enabling or directly in streaming "
            "state");
        return;
      }

      /* Group can move to Enabling state now. */
      group->SetState(AseState::BTA_LE_AUDIO_ASE_STATE_ENABLING);
    }

    /* If Target State is not streaming, then something is wrong. */
    if (group->GetTargetState() != AseState::BTA_LE_AUDIO_ASE_STATE_STREAMING) {
      log::error(", invalid state transition, from: {} , to: {}",
                 ToString(group->GetState()),
                 ToString(group->GetTargetState()));
      StopStream(group);
      return;
    }

    /* Try to create CISes for the group */
    if (!CisCreate(group)) {
      log::debug("cis creation got the group");
      StopStream(group);
    }
  }

  void ProcessGroupDisable(LeAudioDeviceGroup* group) {
    /* Disable ASEs for next device in group. */
    if (group->GetState() != AseState::BTA_LE_AUDIO_ASE_STATE_DISABLING) {
      if (!group->IsGroupReadyToSuspendStream()) {
        log::info("Waiting for all devices to be in disable state");
        return;
      }
      group->SetState(AseState::BTA_LE_AUDIO_ASE_STATE_DISABLING);
    }

    /* At this point all of the active ASEs within group are disabled. As there
     * is no Disabling state for Sink ASE, it might happen that all of the
     * active ASEs are Sink ASE and will transit to QoS state. So check
     * the group state, because we might be ready to release data path. */
    if (group->HaveAllActiveDevicesAsesTheSameState(
            AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED)) {
      group->SetState(AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED);
    }

    /* Transition to QoS configured is done by CIS disconnection */
    if (group->GetTargetState() ==
        AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED) {
      ReleaseDataPath(group);
    } else {
      log::error(", invalid state transition, from: {} , to: {}",
                 ToString(group->GetState()),
                 ToString(group->GetTargetState()));
      StopStream(group);
    }
  }

  void ProcessAutonomousDisable(LeAudioDevice* leAudioDevice, struct ase* ase) {
    auto bidirection_ase = leAudioDevice->GetAseToMatchBidirectionCis(ase);

    /* ASE is not a part of bi-directional CIS */
    if (!bidirection_ase) return;

    /* ASE is already disabled */
    if (bidirection_ase->state ==
        AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED) {
      /* Bi-direction ASEs are now disabled */
      if ((ase->autonomous_target_state_ ==
           AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED) &&
          alarm_is_scheduled(ase->autonomous_operation_timer_)) {
        alarm_free(ase->autonomous_operation_timer_);
        ase->autonomous_operation_timer_ = NULL;
        ase->autonomous_target_state_ = AseState::BTA_LE_AUDIO_ASE_STATE_IDLE;
      }
      return;
    }

    /* Schedule alarm if first ASE is autonomously disabling */
    if (!alarm_is_scheduled(bidirection_ase->autonomous_operation_timer_)) {
      ScheduleAutonomousOperationTimer(
          AseState::BTA_LE_AUDIO_ASE_STATE_QOS_CONFIGURED, leAudioDevice,
          bidirection_ase);
    }
  }
};
}  // namespace

namespace bluetooth::le_audio {
void LeAudioGroupStateMachine::Initialize(Callbacks* state_machine_callbacks_) {
  if (instance) {
    log::error("Already initialized");
    return;
  }

  instance = new LeAudioGroupStateMachineImpl(state_machine_callbacks_);
}

void LeAudioGroupStateMachine::Cleanup() {
  if (!instance) return;

  LeAudioGroupStateMachineImpl* ptr = instance;
  instance = nullptr;

  delete ptr;
}

LeAudioGroupStateMachine* LeAudioGroupStateMachine::Get() {
  log::assert_that(instance != nullptr, "assert failed: instance != nullptr");
  return instance;
}
}  // namespace bluetooth::le_audio
