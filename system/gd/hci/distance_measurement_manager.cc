/*
 * Copyright 2022 The Android Open Source Project
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

/*
 * Changes from Qualcomm Innovation Center, Inc. are provided under the following license:
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "hci/distance_measurement_manager.h"

#include <bluetooth/log.h>
#include <com_android_bluetooth_flags.h>
#include <math.h>

#include <utils/SystemClock.h>
#include <chrono>
#include <complex>
#include <unordered_map>
#include <sys/time.h>
#include <cutils/properties.h>
#include "acl_manager/assembler.h"
#include "common/strings.h"
#include "hal/ranging_hal.h"
#include "hci/acl_manager.h"
#include "hci/controller.h"
#include "hci/distance_measurement_interface.h"
#include "hci/event_checkers.h"
#include "hci/hci_layer.h"
#include "module.h"
#include "os/handler.h"
#include "os/log.h"
#include "os/repeating_alarm.h"
#include "packet/packet_view.h"
#include "ras/ras_packets.h"
#include "device/include/csconfig.h"

using namespace bluetooth::ras;
using bluetooth::hci::acl_manager::PacketViewForRecombination;

namespace bluetooth {
namespace hci {
const ModuleFactory DistanceMeasurementManager::Factory =
        ModuleFactory([]() { return new DistanceMeasurementManager(); });
static constexpr uint16_t kIllegalConnectionHandle = 0xffff;
static constexpr uint8_t kTxPowerNotAvailable = 0xfe;
static constexpr int8_t kRSSIDropOffAt1M = 41;
static constexpr uint8_t kCsMaxTxPower = 20;  // 10 dBm
static constexpr CsSyncAntennaSelection kCsSyncAntennaSelection =
         CsSyncAntennaSelection::ANTENNA_2;
static constexpr uint8_t kConfigId = 0x01;  // Use 0x01 to create config and enable procedure
static constexpr uint8_t kMinMainModeSteps = 0x02;
static constexpr uint8_t kMaxMainModeSteps = 0x05;
static constexpr uint8_t kMainModeRepetition = 0x00;  // No repetition
static constexpr uint8_t kMode0Steps =
        0x03;  // Maximum number of mode-0 steps to increase success subevent rate
static constexpr uint8_t kChannelMapRepetition = 0x01;  // No repetition
static constexpr uint8_t kCh3cJump = 0x03;              // Skip 3 Channels
static constexpr uint16_t kMaxProcedureLen = 0x2710;    // 6.25s
static constexpr uint16_t kMinProcedureInterval = 0x01;
static constexpr uint16_t kMaxProcedureInterval = 0xFF;
static constexpr uint16_t kMaxProcedureCount = 0x01;
static constexpr uint32_t kMinSubeventLen = 0x0004E2;         // 1250us
static constexpr uint32_t kMaxSubeventLen = 0x3d0900;         // 4s
static constexpr uint8_t kTxPwrDelta = 0x00;
static constexpr uint8_t kProcedureDataBufferSize = 0x10;  // Buffer size of Procedure data
static constexpr uint16_t kMtuForRasData = 507;            // 512 - 5
static constexpr uint16_t kRangingCounterMask = 0x0FFF;
static constexpr uint8_t kInvalidConfigId = 0xFF;
static constexpr uint16_t kDefaultIntervalMs = 1000;  // 1s
static constexpr uint32_t kMaxIntervalMs = INT_MAX;  // INT_MAX
static constexpr uint8_t kMaxRetryCounterForCreateConfig = 0x03;
long long proc_start_timestampMs;
long long curr_proc_complete_timestampMs;
static constexpr uint16_t kInvalidConnInterval = 0;  // valid value is from 0x0006 to 0x0C80

struct DistanceMeasurementManager::impl : bluetooth::hal::RangingHalCallback {
  struct CsProcedureData {
    CsProcedureData(uint16_t procedure_counter, uint8_t num_antenna_paths, uint8_t configuration_id,
                    uint8_t selected_tx_power)
        : counter(procedure_counter), num_antenna_paths(num_antenna_paths) {
      local_status = CsProcedureDoneStatus::PARTIAL_RESULTS;
      remote_status = CsProcedureDoneStatus::PARTIAL_RESULTS;
      // In ascending order of antenna position with tone extension data at the end
      uint16_t num_tone_data = num_antenna_paths + 1;
      for (uint8_t i = 0; i < num_tone_data; i++) {
        std::vector<std::complex<double>> empty_complex_vector;
        tone_pct_initiator.push_back(empty_complex_vector);
        tone_pct_reflector.push_back(empty_complex_vector);
        std::vector<uint8_t> empty_vector;
        tone_quality_indicator_initiator.push_back(empty_vector);
        tone_quality_indicator_reflector.push_back(empty_vector);
      }
      // RAS data
      segmentation_header_.first_segment_ = 1;
      segmentation_header_.last_segment_ = 0;
      segmentation_header_.rolling_segment_counter_ = 0;
      ranging_header_.ranging_counter_ = counter;
      ranging_header_.configuration_id_ = configuration_id;
      ranging_header_.selected_tx_power_ = selected_tx_power;
      ranging_header_.antenna_paths_mask_ = 0;
      for (uint8_t i = 0; i < num_antenna_paths; i++) {
        ranging_header_.antenna_paths_mask_ |= (1 << i);
      }
      ranging_header_.pct_format_ = PctFormat::IQ;
    }
    // Procedure counter
    uint16_t counter;
    // Number of antenna paths (1 to 4) reported in the procedure
    uint8_t num_antenna_paths;
    // Frequency Compensation indicates fractional frequency offset (FFO) value of initiator, in
    // 0.01ppm
    std::vector<uint16_t> frequency_compensation;
    // The channel indices of every step in a CS procedure (in time order)
    std::vector<uint8_t> step_channel;
    std::vector<uint16_t> step_mode;
    // Measured Frequency Offset from mode 0, relative to the remote device, in 0.01ppm
    std::vector<int16_t> measured_freq_offset;
    // Initiator's PCT (complex value) measured from mode-2 or mode-3 steps in a CS procedure (in
    // time order)
    std::vector<std::vector<std::complex<double>>> tone_pct_initiator;
    // Reflector's PCT (complex value) measured from mode-2 or mode-3 steps in a CS procedure (in
    // time order)
    std::vector<std::vector<std::complex<double>>> tone_pct_reflector;
    std::vector<std::vector<uint8_t>> tone_quality_indicator_initiator;
    std::vector<std::vector<uint8_t>> tone_quality_indicator_reflector;
	  std::vector<uint8_t> antenna_permutation_index_initiator;
    std::vector<uint8_t> antenna_permutation_index_reflector;
    std::vector<int8_t> packet_quality_initiator;
    std::vector<int8_t> packet_quality_reflector;
    std::vector<int16_t> toa_tod_initiators;
    std::vector<int16_t> tod_toa_reflectors;
    std::vector<int8_t> rssi_initiator;
    std::vector<int8_t> rssi_reflector;
    std::vector<int8_t> packet_nadm_initiator;
    std::vector<int8_t> packet_nadm_reflector;
    std::vector<int8_t> vendor_specific_cs_single_side_data;
    bool contains_sounding_sequence_local_;
    bool contains_sounding_sequence_remote_;
    CsProcedureDoneStatus local_status;
    CsProcedureDoneStatus remote_status;
    // If any subevent is received with a Subevent_Done_Status of 0x0 (All results complete for the
    // CS subevent)
    bool contains_complete_subevent_ = false;
    // RAS data
    SegmentationHeader segmentation_header_;
    RangingHeader ranging_header_;
    std::vector<uint8_t> ras_raw_data_;  // raw data for multi_subevents;
    uint16_t ras_raw_data_index_ = 0;
    RasSubeventHeader ras_subevent_header_;
    std::vector<uint8_t> ras_subevent_data_;
    uint8_t ras_subevent_counter_ = 0;
    int8_t initiator_reference_power_level;
    int8_t reflector_reference_power_level;
  };

  struct RSSITracker {
    uint16_t handle;
    uint16_t interval_ms;
    uint8_t remote_tx_power;
    bool started;
    std::unique_ptr<os::RepeatingAlarm> repeating_alarm;
  };

  // TODO: use state machine to manage the tracker.
  enum class CsTrackerState : uint8_t {
    UNSPECIFIED = 0x00,
    STOPPED = 1 << 0,
    INIT = 1 << 1,
    RAS_CONNECTED = 1 << 2,
    WAIT_FOR_CONFIG_COMPLETE = 1 << 3,
    WAIT_FOR_SECURITY_ENABLED = 1 << 4,
    WAIT_FOR_PROCEDURE_ENABLED = 1 << 5,
    STARTED = 1 << 6,
    HOLD = 1 << 7,
  };

  struct CsTracker {
    CsTrackerState state = CsTrackerState::STOPPED;
    Address address;
    hci::Role local_hci_role = hci::Role::CENTRAL;
    uint16_t procedure_counter = 0;
    CsRole role = CsRole::INITIATOR;
    bool local_start = false;  // If the CS was started by the local device.
    // TODO: clean up, replace the measurement_ongoing with STOPPED
    bool measurement_ongoing = false;
    bool ras_connected = false;
    bool setup_complete = false;
    bool config_set = false;
    uint8_t retry_counter_for_create_config = 0;
    uint16_t n_procedure_count = 0;
    CsMainModeType main_mode_type = CsMainModeType::MODE_2;
    CsSubModeType sub_mode_type = CsSubModeType::UNUSED;
    CsRttType rtt_type = CsRttType::RTT_AA_ONLY;
    bool remote_support_phase_based_ranging = false;
    uint8_t remote_num_antennas_supported_ = 0x01;
    uint8_t remote_num_config_supported_ = 0x01;
    uint8_t config_id = kInvalidConfigId;
    uint8_t selected_tx_power = 0;
    std::vector<CsProcedureData> procedure_data_list = {};
    uint32_t interval_ms = kDefaultIntervalMs;
    uint16_t max_procedure_count = 1;
    bool waiting_for_start_callback = false;
    std::unique_ptr<os::RepeatingAlarm> repeating_alarm = nullptr;
    uint8_t min_main_mode_steps = 0;
    uint8_t max_main_mode_steps = 0;
    uint8_t main_mode_repetition = 0;
    uint8_t mode_0_steps = 0;
    uint8_t cs_sync_phy = 0;
    std::array<uint8_t,10> channel_map;
    uint8_t channel_map_repetition = 0;
    uint8_t channel_selection_type = 0;
    uint8_t ch3c_shape = 0;
    uint8_t ch3c_jump = 0;
    uint8_t t_ip1_time = 0;
    uint8_t t_ip2_time = 0;
    uint8_t t_fcs_time = 0;
    uint8_t t_pm_time = 0;
    uint8_t tone_antenna_config_selection = 0;
    uint32_t subevent_len = 0;
    uint8_t subevents_per_event = 0;
    uint16_t subevent_interval = 0;
    uint16_t event_interval = 0;
    uint16_t procedure_interval = 0;
    uint16_t max_procedure_len = 0;
    // RAS data
    RangingHeader ranging_header_;
    PacketViewForRecombination segment_data_;
    uint16_t conn_interval_ = kInvalidConnInterval;
  };

  void OnOpened(
          uint16_t connection_handle,
          const std::vector<bluetooth::hal::VendorSpecificCharacteristic>& vendor_specific_reply) {
    log::info("connection_handle:0x{:04x}, vendor_specific_reply size:{}", connection_handle,
              vendor_specific_reply.size());
    if (cs_requester_trackers_.find(connection_handle) == cs_requester_trackers_.end()) {
      log::error("Can't find CS tracker for connection_handle {}", connection_handle);
      return;
    }

    auto& tracker = cs_requester_trackers_[connection_handle];
    if (!vendor_specific_reply.empty()) {
      // Send reply to remote
      distance_measurement_callbacks_->OnVendorSpecificReply(tracker.address,
                                                             vendor_specific_reply);
      return;
    }

    start_distance_measurement_with_cs(tracker.address, connection_handle);
  }

  void OnOpenFailed(uint16_t connection_handle) {
    log::info("connection_handle:0x{:04x}", connection_handle);
    if (cs_requester_trackers_.find(connection_handle) == cs_requester_trackers_.end()) {
      log::error("Can't find CS tracker for connection_handle {}", connection_handle);
      return;
    }
    distance_measurement_callbacks_->OnDistanceMeasurementStopped(
            cs_requester_trackers_[connection_handle].address, REASON_INTERNAL_ERROR, METHOD_CS);
  }

  void OnHandleVendorSpecificReplyComplete(uint16_t connection_handle, bool success) {
    log::info("connection_handle:0x{:04x}, success:{}", connection_handle, success);
    auto it = cs_responder_trackers_.find(connection_handle);
    if (it == cs_responder_trackers_.end()) {
      log::error("Can't find CS tracker for connection_handle {}", connection_handle);
      return;
    }
    distance_measurement_callbacks_->OnHandleVendorSpecificReplyComplete(it->second.address,
                                                                         success);
  }

  void OnResult(uint16_t connection_handle, const bluetooth::hal::RangingResult& ranging_result) {
    if (cs_requester_trackers_.find(connection_handle) == cs_requester_trackers_.end()) {
      log::warn("Can't find CS tracker for connection_handle {}", connection_handle);
      return;
    }
    log::debug("address {}, resultMeters {}", cs_requester_trackers_[connection_handle].address,
               ranging_result.result_meters_);
    using namespace std::chrono;
    uint64_t elapsedRealtimeNanos = ::android::elapsedRealtimeNano();
    distance_measurement_callbacks_->OnDistanceMeasurementResult(
            cs_requester_trackers_[connection_handle].address, ranging_result.result_meters_ * 100,
            0.0, -1, -1, -1, -1, elapsedRealtimeNanos, ranging_result.confidence_level_,
            DistanceMeasurementMethod::METHOD_CS);
  }

  ~impl() {}
  void start(os::Handler* handler, hci::Controller* controller, hal::RangingHal* ranging_hal,
             hci::HciLayer* hci_layer, hci::AclManager* acl_manager) {
    handler_ = handler;
    controller_ = controller;
    ranging_hal_ = ranging_hal;
    hci_layer_ = hci_layer;
    acl_manager_ = acl_manager;

    hci_layer_->RegisterLeEventHandler(hci::SubeventCode::TRANSMIT_POWER_REPORTING,
                                       handler_->BindOn(this, &impl::on_transmit_power_reporting));
    if (!controller_->SupportsBleChannelSounding()) {
      log::info("The controller doesn't support Channel Sounding feature.");
      return;
    }
    distance_measurement_interface_ = hci_layer_->GetDistanceMeasurementInterface(
            handler_->BindOn(this, &DistanceMeasurementManager::impl::handle_event));
    distance_measurement_interface_->EnqueueCommand(
            LeCsReadLocalSupportedCapabilitiesBuilder::Create(),
            handler_->BindOnceOn(this, &impl::on_cs_read_local_supported_capabilities));
    if (ranging_hal_->IsBound()) {
      ranging_hal_->RegisterCallback(this);
    }
  }

  void stop() { hci_layer_->UnregisterLeEventHandler(hci::SubeventCode::TRANSMIT_POWER_REPORTING); }

  void register_distance_measurement_callbacks(DistanceMeasurementCallbacks* callbacks) {
    distance_measurement_callbacks_ = callbacks;
    if (ranging_hal_->IsBound()) {
      auto vendor_specific_data = ranging_hal_->GetVendorSpecificCharacteristics();
      if (!vendor_specific_data.empty()) {
        distance_measurement_callbacks_->OnVendorSpecificCharacteristics(vendor_specific_data);
      }
    }
  }

  void set_cs_params(const Address& cs_remote_address, uint16_t connection_handle, int mSightType,
             int mLocationType, int mCsSecurityLevel, int mFrequency, int mDuration) {
    log::info("Address:{}, connection_handle:{}, CsSecurityLevel:{} frequency:{}",
               cs_remote_address, connection_handle, mCsSecurityLevel, mFrequency);
    if (set_cs_params_.find(connection_handle) != set_cs_params_.end() &&
        set_cs_params_[connection_handle].address != cs_remote_address) {
      log::warn("Remove old tracker for {}", cs_remote_address);
      set_cs_params_.erase(connection_handle);
    }

    if(cs_requester_trackers_.find(connection_handle) != cs_requester_trackers_.end()) {
      auto it = cs_requester_trackers_.find(connection_handle);
      if(it->second.state == CsTrackerState::HOLD) {
          log::warn("Cs tracker on hold and params removed");
          set_cs_params_.erase(connection_handle);
      }
    }
    tCS_PROCEDURE_PARAM cs_proc_setting;
    tCS_CONFIG cs_config_setting;
    mCsSecurityLevel = mCsSecurityLevel-1;
    if ((mCsSecurityLevel >=0  && mCsSecurityLevel <= 3) &&
	(mFrequency >= 0 && mFrequency <= 2)) {
       if (get_cs_procedure_settings(mFrequency, &cs_proc_setting) &&
           get_cs_config_settings(mCsSecurityLevel, &cs_config_setting) &&
	   (set_cs_params_.find(connection_handle) == set_cs_params_.end())) {
         set_cs_params_[connection_handle].address = cs_remote_address;
	 set_cs_params_[connection_handle].cs_conf_settings.push_back(cs_config_setting);
	 set_cs_params_[connection_handle].cs_proc_settings.push_back(cs_proc_setting);
       }
    } else {
      log::warn("using default configs");
    }
  }

  void start_distance_measurement(const Address address, uint16_t connection_handle,
                                  hci::Role local_hci_role, uint16_t interval,
                                  DistanceMeasurementMethod method) {
    log::info("Address:{}, method:{}", address, method);

    // Remove this check if we support any connection less method
    if (connection_handle == kIllegalConnectionHandle) {
      log::warn("Can't find any LE connection for {}", address);
      distance_measurement_callbacks_->OnDistanceMeasurementStopped(
              address, REASON_NO_LE_CONNECTION, method);
      return;
    }

    switch (method) {
      case METHOD_AUTO:
      case METHOD_RSSI: {
        if (rssi_trackers.find(address) == rssi_trackers.end()) {
          rssi_trackers[address].handle = connection_handle;
          rssi_trackers[address].interval_ms = interval;
          rssi_trackers[address].remote_tx_power = kTxPowerNotAvailable;
          rssi_trackers[address].started = false;
          rssi_trackers[address].repeating_alarm = std::make_unique<os::RepeatingAlarm>(handler_);
          hci_layer_->EnqueueCommand(
                  LeReadRemoteTransmitPowerLevelBuilder::Create(connection_handle, 0x01),
                  handler_->BindOnceOn(this, &impl::on_read_remote_transmit_power_level_status,
                                       address));
        } else {
          rssi_trackers[address].interval_ms = interval;
        }
      } break;
      case METHOD_CS: {
        init_cs_requester_tracker(address, connection_handle, local_hci_role, interval);
        start_distance_measurement_with_cs(address, connection_handle);
      } break;
    }
  }

  void init_cs_requester_tracker(const Address& cs_remote_address, uint16_t connection_handle,
                                 hci::Role local_hci_role, uint16_t interval) {
    auto it = cs_requester_trackers_.find(connection_handle);
    if (it != cs_requester_trackers_.end()) {
      if (it->second.address != cs_remote_address) {
        log::debug("replace old tracker as {}", cs_remote_address);
        it->second = CsTracker();
      }
    } else {
      cs_requester_trackers_[connection_handle] = CsTracker();
      it = cs_requester_trackers_.find(connection_handle);
    }
    it->second.address = cs_remote_address;
    // make sure the repeating_alarm is initialized.
    if (it->second.repeating_alarm == nullptr) {
      it->second.repeating_alarm = std::make_unique<os::RepeatingAlarm>(handler_);
    }
    it->second.state = CsTrackerState::INIT;
    // If the interval is less than 1 second, update it to 1 second and increase the
    // max_procedure_count
    if (interval < 1000) {
      it->second.max_procedure_count = 1000 / interval;
      interval = 1000;
      log::info("Update interval to 1s and max_procedure_count to {}",
                it->second.max_procedure_count);
    }

    /* Set timer based on max procedure counter */
    if (set_cs_params_.find(connection_handle) != set_cs_params_.end()) {
      tCS_PROCEDURE_PARAM procedure_setting =
                     set_cs_params_[connection_handle].cs_proc_settings[0];
      if (!procedure_setting.max_proc_count) {
        it->second.interval_ms = kMaxIntervalMs;
      } else if(procedure_setting.max_proc_count == 1) {
        it->second.interval_ms = interval;
      } else {
        uint32_t interval;
	uint32_t max_subevent_len = ((procedure_setting.max_subevent_len[0] |
		                     procedure_setting.max_subevent_len[1] <<8 |
                                     procedure_setting.max_subevent_len[2]<<16) * 0.001);
        interval = (uint32_t) (procedure_setting.max_proc_duration *
		               procedure_setting.max_proc_count);
        it->second.interval_ms = interval;
      }
    } else {
      it->second.interval_ms = interval;
    }

    it->second.local_start = true;
    it->second.measurement_ongoing = true;
    it->second.waiting_for_start_callback = true;
    it->second.local_hci_role = local_hci_role;
  }

  void start_distance_measurement_with_cs(const Address& cs_remote_address,
                                          uint16_t connection_handle) {
    log::info("connection_handle: {}, address: {}", connection_handle, cs_remote_address);
    if (!cs_requester_trackers_[connection_handle].ras_connected) {
      log::info("Waiting for RAS connected");
      return;
    }

    if (!cs_requester_trackers_[connection_handle].setup_complete) {
      send_le_cs_read_remote_supported_capabilities(connection_handle);
      return;
    }
    if (!cs_requester_trackers_[connection_handle].config_set) {
      send_le_cs_create_config(connection_handle,
                               cs_requester_trackers_[connection_handle].config_id);
      return;
    }
    log::info("enable cs procedure regularly with interval: {} ms",
              cs_requester_trackers_[connection_handle].interval_ms);
    cs_requester_trackers_[connection_handle].repeating_alarm->Cancel();
    send_le_cs_procedure_enable(connection_handle, Enable::ENABLED);
    cs_requester_trackers_[connection_handle].repeating_alarm->Schedule(
            common::Bind(&impl::send_le_cs_procedure_enable, common::Unretained(this),
                         connection_handle, Enable::ENABLED),
            std::chrono::milliseconds(cs_requester_trackers_[connection_handle].interval_ms));
  }

  void stop_distance_measurement(const Address address, uint16_t connection_handle,
                                 DistanceMeasurementMethod method) {
    log::info("Address:{}, method:{}", address, method);
    switch (method) {
      case METHOD_AUTO:
      case METHOD_RSSI: {
        auto it = rssi_trackers.find(address);
        if (it == rssi_trackers.end()) {
          log::warn("Can't find rssi tracker for {}", address);
        } else {
          hci_layer_->EnqueueCommand(
                  LeSetTransmitPowerReportingEnableBuilder::Create(it->second.handle, 0x00, 0x00),
                  handler_->BindOnce(
                          check_complete<LeSetTransmitPowerReportingEnableCompleteView>));
          it->second.repeating_alarm->Cancel();
          it->second.repeating_alarm.reset();
          rssi_trackers.erase(address);
        }
      } break;
      case METHOD_CS: {
        auto it = cs_requester_trackers_.find(connection_handle);
        if (it == cs_requester_trackers_.end()) {
          log::warn("Can't find CS tracker for {}", address);
        } else if (it->second.measurement_ongoing) {
          it->second.repeating_alarm->Cancel();
          send_le_cs_procedure_enable(connection_handle, Enable::DISABLED);
          reset_tracker_on_stopped(it->second);
          it->second.config_set = false;
          it->second.state = CsTrackerState::HOLD;
          it->second.config_id = kInvalidConfigId;
	  /*
	  if (ranging_hal_->IsBound())
	    ranging_hal_->close(connection_handle);
          */
	     }
      } break;
    }
  }

  void handle_ras_client_connected_event(
          const Address address, uint16_t connection_handle, uint16_t att_handle,
          const std::vector<hal::VendorSpecificCharacteristic> vendor_specific_data,
          uint16_t conn_interval) {
    log::info(
            "address:{}, connection_handle 0x{:04x}, att_handle 0x{:04x}, size of "
            "vendor_specific_data {}, conn_interval {}",
            address, connection_handle, att_handle, vendor_specific_data.size(), conn_interval);

    auto it = cs_requester_trackers_.find(connection_handle);
    if (it == cs_requester_trackers_.end()) {
      log::warn("can't find tracker for 0x{:04x}", connection_handle);
      return;
    }
    if (it->second.ras_connected) {
      log::debug("Already connected");
      return;
    }
    it->second.conn_interval_ = conn_interval;
    it->second.ras_connected = true;
    it->second.state = CsTrackerState::RAS_CONNECTED;

    if (ranging_hal_->IsBound()) {
      ranging_hal_->OpenSession(connection_handle, att_handle, vendor_specific_data);
      return;
    }
    start_distance_measurement_with_cs(it->second.address, connection_handle);
  }

  void handle_conn_interval_updated(const Address& address, uint16_t connection_handle,
                                    uint16_t conn_interval) {
    auto it = cs_requester_trackers_.find(connection_handle);
    if (it == cs_requester_trackers_.end()) {
      log::warn("can't find tracker for 0x{:04x}, address - {} ", connection_handle, address);
      return;
    }
    log::info("interval updated as {}", conn_interval);
    it->second.conn_interval_ = conn_interval;
  }

  void handle_ras_client_disconnected_event(const Address address) {
    log::info("address:{}", address);
    for (auto it = cs_requester_trackers_.begin(); it != cs_requester_trackers_.end();) {
      if (it->second.address == address) {
        if (it->second.repeating_alarm != nullptr) {
          it->second.repeating_alarm->Cancel();
          it->second.repeating_alarm.reset();
        }
        distance_measurement_callbacks_->OnDistanceMeasurementStopped(
                address, REASON_NO_LE_CONNECTION, METHOD_CS);
        it = cs_requester_trackers_.erase(it);  // erase and get the next iterator
      } else {
        ++it;
      }
    }
  }

  void handle_ras_server_vendor_specific_reply(
          const Address& address, uint16_t connection_handle,
          const std::vector<hal::VendorSpecificCharacteristic> vendor_specific_reply) {
    auto it = cs_responder_trackers_.find(connection_handle);
    if (it == cs_responder_trackers_.end()) {
      log::info("no cs tracker found for {}", connection_handle);
      return;
    }
    if (it->second.address != address) {
      log::info("the cs tracker address was changed as {}, not {}.", it->second.address, address);
      return;
    }
    if (ranging_hal_->IsBound()) {
      ranging_hal_->HandleVendorSpecificReply(connection_handle, vendor_specific_reply);
      return;
    }
  }

  void handle_ras_server_connected(const Address& identity_address, uint16_t connection_handle,
                                   hci::Role local_hci_role) {
    log::info("initialize the responder tracker for {} with {}", connection_handle,
              identity_address);
    // create CS tracker to serve the ras_server
    auto it = cs_responder_trackers_.find(connection_handle);
    if (it != cs_responder_trackers_.end()) {
      if (it->second.address != identity_address) {
        log::debug("Remove old tracker for {}", identity_address);
        it->second = CsTracker();
      }
    } else {
      cs_responder_trackers_[connection_handle] = CsTracker();
      it = cs_responder_trackers_.find(connection_handle);
    }
    it->second.state = CsTrackerState::RAS_CONNECTED;
    it->second.address = identity_address;
    char value[92];
    if (property_get("persist.vendor.service.bt.config.role", value, "false")) {
      if (strncmp(value, "true", 92) == 0) {
          it->second.local_start = true;
      } else {
          it->second.local_start = false;
      }
    }
    it->second.local_hci_role = local_hci_role;
  }

  void handle_ras_server_disconnected(const Address& identity_address, uint16_t connection_handle) {
    auto it = cs_responder_trackers_.find(connection_handle);
    if (it == cs_responder_trackers_.end()) {
      log::info("no CS tracker available.");
      return;
    }
    if (it->second.address != identity_address) {
      log::info("cs tracker connection is associated with device {}, not device {}",
                it->second.address, identity_address);
      return;
    }

    cs_responder_trackers_.erase(connection_handle);
  }

  void handle_vendor_specific_reply_complete(const Address address, uint16_t connection_handle,
                                             bool success) {
    log::info("address:{}, connection_handle:0x{:04x}, success:{}", address, connection_handle,
              success);
    auto it = cs_requester_trackers_.find(connection_handle);
    if (it == cs_requester_trackers_.end()) {
      log::warn("can't find tracker for 0x{:04x}", connection_handle);
      distance_measurement_callbacks_->OnDistanceMeasurementStopped(address, REASON_INTERNAL_ERROR,
                                                                    METHOD_CS);
      return;
    }

    if (!success) {
      distance_measurement_callbacks_->OnDistanceMeasurementStopped(address, REASON_INTERNAL_ERROR,
                                                                    METHOD_CS);
      return;
    }

    start_distance_measurement_with_cs(it->second.address, connection_handle);
  }

  void send_read_rssi(const Address& address, uint16_t connection_handle) {
    if (rssi_trackers.find(address) == rssi_trackers.end()) {
      log::warn("Can't find rssi tracker for {}", address);
      return;
    }
    Address connection_address = acl_manager_->HACK_GetLeAddress(connection_handle);
    if (connection_address.IsEmpty()) {
      log::warn("Can't find connection for {}", address);
      if (rssi_trackers.find(address) != rssi_trackers.end()) {
        distance_measurement_callbacks_->OnDistanceMeasurementStopped(
                address, REASON_NO_LE_CONNECTION, METHOD_RSSI);
        rssi_trackers[address].repeating_alarm->Cancel();
        rssi_trackers[address].repeating_alarm.reset();
        rssi_trackers.erase(address);
      }
      return;
    }

    hci_layer_->EnqueueCommand(ReadRssiBuilder::Create(connection_handle),
                               handler_->BindOnceOn(this, &impl::on_read_rssi_complete, address));
  }

  void handle_event(LeMetaEventView event) {
    if (!event.IsValid()) {
      log::error("Received invalid LeMetaEventView");
      return;
    }
    switch (event.GetSubeventCode()) {
      case hci::SubeventCode::LE_CS_TEST_END_COMPLETE:
      case hci::SubeventCode::LE_CS_READ_REMOTE_FAE_TABLE_COMPLETE: {
        log::warn("Unhandled subevent {}", hci::SubeventCodeText(event.GetSubeventCode()));
      } break;
      case hci::SubeventCode::LE_CS_SUBEVENT_RESULT_CONTINUE:
      case hci::SubeventCode::LE_CS_SUBEVENT_RESULT: {
        on_cs_subevent(event);
      } break;
      case hci::SubeventCode::LE_CS_PROCEDURE_ENABLE_COMPLETE: {
        on_cs_procedure_enable_complete(LeCsProcedureEnableCompleteView::Create(event));
      } break;
      case hci::SubeventCode::LE_CS_CONFIG_COMPLETE: {
        on_cs_config_complete(LeCsConfigCompleteView::Create(event));
      } break;
      case hci::SubeventCode::LE_CS_SECURITY_ENABLE_COMPLETE: {
        on_cs_security_enable_complete(LeCsSecurityEnableCompleteView::Create(event));
      } break;
      case hci::SubeventCode::LE_CS_READ_REMOTE_SUPPORTED_CAPABILITIES_COMPLETE: {
        on_cs_read_remote_supported_capabilities_complete(
                LeCsReadRemoteSupportedCapabilitiesCompleteView::Create(event));
      } break;
      default:
        log::info("Unknown subevent {}", hci::SubeventCodeText(event.GetSubeventCode()));
    }
  }

  void send_le_cs_read_remote_supported_capabilities(uint16_t connection_handle) {
    hci_layer_->EnqueueCommand(
            LeCsReadRemoteSupportedCapabilitiesBuilder::Create(connection_handle),
            handler_->BindOnceOn(this, &impl::on_cs_setup_command_status_cb, connection_handle));
  }

  void send_le_cs_security_enable(uint16_t connection_handle) {
    if (cs_requester_trackers_.find(connection_handle) == cs_requester_trackers_.end()) {
      log::warn("no cs tracker found for {}", connection_handle);
    }
    cs_requester_trackers_[connection_handle].state = CsTrackerState::WAIT_FOR_SECURITY_ENABLED;
    hci_layer_->EnqueueCommand(
            LeCsSecurityEnableBuilder::Create(connection_handle),
            handler_->BindOnceOn(this, &impl::on_cs_setup_command_status_cb, connection_handle));
  }

  void send_le_cs_set_default_settings(uint16_t connection_handle) {
    uint8_t role_enable = (1 << (uint8_t)CsRole::INITIATOR) | 1 << ((uint8_t)CsRole::REFLECTOR);
    hci_layer_->EnqueueCommand(
            LeCsSetDefaultSettingsBuilder::Create(connection_handle, role_enable,
                                                  kCsSyncAntennaSelection, kCsMaxTxPower),
            handler_->BindOnceOn(this, &impl::on_cs_set_default_settings_complete));
  }

  void send_le_cs_remove_config(uint16_t conn_handle,
                                       uint8_t config_id) {
    log::info("remove_config conn_handle: {} config_id: {}", conn_handle, config_id);
    hci_layer_->EnqueueCommand(
         LeCsRemoveConfigBuilder::Create(conn_handle, config_id),
         handler_->BindOnceOn(this, &impl::on_le_cs_remove_config_complete));
  }
  void on_le_cs_remove_config_complete(CommandStatusView status_view){
    log::info("remove_config complete");
    ErrorCode status = status_view.GetStatus();
    if (status != ErrorCode::SUCCESS) {
      log::error("Invalid LeCsRemoveConfigStatus: {}", status);
    }
  }
  void send_le_cs_create_config(uint16_t connection_handle, uint8_t config_id) {

    uint8_t KMaxAllowedConfigID = 4;
    if (cs_requester_trackers_.find(connection_handle) == cs_requester_trackers_.end()) {
      log::warn("no cs tracker found for {}", connection_handle);
    }
    log::debug("send cs create config {}", config_id);
    cs_requester_trackers_[connection_handle].state = CsTrackerState::WAIT_FOR_CONFIG_COMPLETE;

    bool config_avb = false;
    if (set_cs_params_.find(connection_handle) != set_cs_params_.end()) {
      config_avb = true;
    }
    uint8_t remote_config_supports = cs_requester_trackers_[connection_handle].remote_num_config_supported_;
    KMaxAllowedConfigID = (remote_config_supports > local_num_config_supported_) ?
                                               local_num_config_supported_ : remote_config_supports;
    if (config_avb) {
      tCS_CONFIG config_settings;
      config_settings = set_cs_params_[connection_handle].cs_conf_settings[0];
      std::array<uint8_t, 10> channel_map;
      for (int i = 0; i < CS_CHANNEL_MAP_SIZE; i++) {
        channel_map[i] = config_settings.channel_map[i];
      }

      if (config_settings.config_id >= KMaxAllowedConfigID) {
        log::info("config_settings.config_id: {} KMaxAllowedConfigID: {} config_avb: {}",
                              config_settings.config_id, KMaxAllowedConfigID, config_avb);
        config_settings.config_id = 0;
      }

      cs_requester_trackers_[connection_handle].config_id = config_settings.config_id;
      hci_layer_->EnqueueCommand(
            LeCsCreateConfigBuilder::Create(
            connection_handle,
            config_settings.config_id,
            CsCreateContext::BOTH_LOCAL_AND_REMOTE_CONTROLLER,
            (CsMainModeType)config_settings.main_mode_type,
            (CsSubModeType)config_settings.sub_mode_type,
            config_settings.main_mode_min_steps,
            config_settings.main_mode_max_steps,
            config_settings.main_mode_rep,
            config_settings.mode_0_steps,
            (CsRole)config_settings.role,
            (CsConfigRttType)config_settings.rtt_types,
            (CsSyncPhy)config_settings.cs_sync_phy,
            channel_map,
            config_settings.channel_map_rep,
            (CsChannelSelectionType)config_settings.hop_algo_type,
            (CsCh3cShape)config_settings.user_shape,
            config_settings.user_channel_jump),
            handler_->BindOnceOn(this, &impl::on_cs_setup_command_status_cb, connection_handle));
      return;
    }

    auto channel_vector = common::FromHexString("1FFFFFFFFFFFFC7FFFFC");  // use all 72 Channels
    // If the interval is less than or equal to 1 second, then use half channels
    if (cs_requester_trackers_[connection_handle].interval_ms <= 1000) {
      channel_vector = common::FromHexString("15555555555554555554");
    }
    std::array<uint8_t, 10> channel_map;
    std::copy(channel_vector->begin(), channel_vector->end(), channel_map.begin());
    std::reverse(channel_map.begin(), channel_map.end());

    if (config_id >= KMaxAllowedConfigID) {
      log::info("config_id: {} KMaxAllowedConfigID: {} config_avb: {}",
                            config_id, KMaxAllowedConfigID, config_avb);
      config_id = 0;
    }

    hci_layer_->EnqueueCommand(
            LeCsCreateConfigBuilder::Create(
                    connection_handle, config_id, CsCreateContext::BOTH_LOCAL_AND_REMOTE_CONTROLLER,
                    CsMainModeType::MODE_2, CsSubModeType::UNUSED, kMinMainModeSteps,
                    kMaxMainModeSteps, kMainModeRepetition, kMode0Steps, CsRole::INITIATOR,
                    CsConfigRttType::RTT_AA_ONLY, CsSyncPhy::LE_1M_PHY, channel_map,
                    kChannelMapRepetition, CsChannelSelectionType::TYPE_3B, CsCh3cShape::HAT_SHAPE,
                    kCh3cJump),
            handler_->BindOnceOn(this, &impl::on_cs_setup_command_status_cb, connection_handle));
  }

  void send_le_cs_set_procedure_parameters(uint16_t connection_handle, uint8_t config_id,
                                           uint8_t remote_num_antennas_supported) {
    uint8_t tone_antenna_config_selection =
            cs_tone_antenna_config_mapping_table_[num_antennas_supported_ - 1]
                                                 [remote_num_antennas_supported - 1];
    uint8_t preferred_peer_antenna_value =
            cs_preferred_peer_antenna_mapping_table_[tone_antenna_config_selection];

    bool config_avb = false;
    uint8_t KMaxAllowedConfigID = 4;
    uint8_t remote_config_supports = cs_requester_trackers_[connection_handle].remote_num_config_supported_;
    KMaxAllowedConfigID = (remote_config_supports > local_num_config_supported_) ?
                                         local_num_config_supported_ : remote_config_supports;
    if (config_id >= KMaxAllowedConfigID) {
      log::info("config_id: {} KMaxAllowedConfigID: {} config_avb: {}",
                           config_id, KMaxAllowedConfigID, config_avb);
      config_id = 0;
   }
    if (set_cs_params_.find(connection_handle) != set_cs_params_.end()) {
      log::info("address {} ", set_cs_params_[connection_handle].address);
      config_avb = true;
   }

   if (config_avb) {
     tCS_PROCEDURE_PARAM procedure_setting =
                     set_cs_params_[connection_handle].cs_proc_settings[0];
     uint32_t min_subevent_len;
     uint32_t max_subevent_len;
     CsPreferredPeerAntenna preferred_peer_antenna;

     min_subevent_len = (procedure_setting.min_subevent_len[0] |
		         procedure_setting.min_subevent_len[1]<<8 |
                         procedure_setting.min_subevent_len[2]<<16);
     max_subevent_len = (procedure_setting.max_subevent_len[0] |
		         procedure_setting.max_subevent_len[1] <<8 |
                         procedure_setting.max_subevent_len[2]<<16);
     log::info("min_subevent_len {} max_subevent_len {} preferred_peer_antenna {}",
	        min_subevent_len, max_subevent_len, procedure_setting.preferred_peer_antenna);

     if (procedure_setting.preferred_peer_antenna & 0x01)
       preferred_peer_antenna.use_first_ordered_antenna_element_ = 1;
     if (procedure_setting.preferred_peer_antenna & 0x02)
       preferred_peer_antenna.use_second_ordered_antenna_element_ = 1;
     if (procedure_setting.preferred_peer_antenna & 0x04)
       preferred_peer_antenna.use_third_ordered_antenna_element_ = 1;
     if (procedure_setting.preferred_peer_antenna & 0x08)
       preferred_peer_antenna.use_fourth_ordered_antenna_element_ = 1;

      hci_layer_->EnqueueCommand(
            LeCsSetProcedureParametersBuilder::Create(
            connection_handle,
            config_id,
            procedure_setting.max_proc_duration,
            procedure_setting.min_period_between_proc,
            procedure_setting.max_period_between_proc,
            procedure_setting.max_proc_count,
            min_subevent_len,
	    max_subevent_len,
           // kToneAntennaConfigSelection,
	    procedure_setting.tone_ant_cfg_selection,
            (CsPhy)procedure_setting.phy,
            procedure_setting.tx_pwr_delta,
            preferred_peer_antenna,
	    (CsSnrControl)procedure_setting.snr_control_initiator,
	    (CsSnrControl)procedure_setting.snr_control_reflector),
            handler_->BindOnceOn(this, &impl::on_cs_set_procedure_parameters));
      return;
    }

    log::info(
            "num_antennas_supported:{}, remote_num_antennas_supported:{}, "
            "tone_antenna_config_selection:{}, preferred_peer_antenna:{}",
            num_antennas_supported_, remote_num_antennas_supported, tone_antenna_config_selection,
            preferred_peer_antenna_value);
    CsPreferredPeerAntenna preferred_peer_antenna;
    preferred_peer_antenna.use_first_ordered_antenna_element_ = preferred_peer_antenna_value & 0x01;
    preferred_peer_antenna.use_second_ordered_antenna_element_ =
            (preferred_peer_antenna_value >> 1) & 0x01;
    preferred_peer_antenna.use_third_ordered_antenna_element_ =
            (preferred_peer_antenna_value >> 2) & 0x01;
    preferred_peer_antenna.use_fourth_ordered_antenna_element_ =
            (preferred_peer_antenna_value >> 3) & 0x01;
    hci_layer_->EnqueueCommand(
            LeCsSetProcedureParametersBuilder::Create(
                    connection_handle, config_id, kMaxProcedureLen, kMinProcedureInterval,
                    kMaxProcedureInterval,
                    cs_requester_trackers_[connection_handle].max_procedure_count, kMinSubeventLen,
                    kMaxSubeventLen, tone_antenna_config_selection, CsPhy::LE_1M_PHY, kTxPwrDelta,
                    preferred_peer_antenna, CsSnrControl::NOT_APPLIED, CsSnrControl::NOT_APPLIED),
            handler_->BindOnceOn(this, &impl::on_cs_set_procedure_parameters));
  }

  static void reset_tracker_on_stopped(CsTracker& cs_tracker) {
    cs_tracker.measurement_ongoing = false;
    cs_tracker.state = CsTrackerState::STOPPED;
  }

  void handle_cs_setup_failure(uint16_t connection_handle, DistanceMeasurementErrorCode errorCode) {
    // responder is stateless. only requester needs to handle the set up failure.
    auto it = cs_requester_trackers_.find(connection_handle);
    if (it == cs_requester_trackers_.end()) {
      log::info("no requester tracker is found for {}.", connection_handle);
      return;
    }
    if (it->second.measurement_ongoing) {
      distance_measurement_callbacks_->OnDistanceMeasurementStopped(it->second.address, errorCode,
                                                                    METHOD_CS);
      it->second.repeating_alarm->Cancel();
      it->second.repeating_alarm.reset();
    }
    reset_tracker_on_stopped(it->second);
    // the cs_tracker should be kept until the connection is disconnected
  }

  void send_le_cs_procedure_enable(uint16_t connection_handle, Enable enable) {
    log::debug("cmd {}", enable);
    auto it = cs_requester_trackers_.find(connection_handle);
    if (it == cs_requester_trackers_.end()) {
      log::warn("Can't find cs tracker for connection {}", connection_handle);
      return;
    }

    if (enable == Enable::ENABLED) {
      if (it->second.state == CsTrackerState::STOPPED) {
        log::error("safe guard, error state, no local measurement request.");
        if (it->second.repeating_alarm) {
          it->second.repeating_alarm->Cancel();
        }
        return;
      }
      it->second.state = CsTrackerState::WAIT_FOR_PROCEDURE_ENABLED;
    } else {  // Enable::DISABLE
      if (it->second.state != CsTrackerState::WAIT_FOR_PROCEDURE_ENABLED &&
          it->second.state != CsTrackerState::STARTED) {
        log::info("no procedure disable command needed for state {}.", (int)it->second.state);
        return;
      }
    }

    hci_layer_->EnqueueCommand(
            LeCsProcedureEnableBuilder::Create(connection_handle, it->second.config_id, enable),
            handler_->BindOnceOn(this, &impl::on_cs_procedure_enable_command_status_cb,
                                 connection_handle, enable));
  }

  void on_cs_procedure_enable_command_status_cb(uint16_t connection_handle, Enable enable,
                                                CommandStatusView status_view) {
    ErrorCode status = status_view.GetStatus();
    // controller may send error if the procedure instance has finished all scheduled procedures.
    if (enable == Enable::DISABLED && status == ErrorCode::COMMAND_DISALLOWED) {
      log::info("ignored the procedure disable command disallow error.");
      if (cs_requester_trackers_.find(connection_handle) != cs_requester_trackers_.end()) {
        reset_tracker_on_stopped(cs_requester_trackers_[connection_handle]);
      }
    } else {
      on_cs_setup_command_status_cb(connection_handle, status_view);
    }
  }

  void on_cs_setup_command_status_cb(uint16_t connection_handle, CommandStatusView status_view) {
    ErrorCode status = status_view.GetStatus();
    OpCode op_code = status_view.GetCommandOpCode();
    if (status != ErrorCode::SUCCESS) {
      log::error("Error code {}, opcode {} for connection-{}", ErrorCodeText(status),
                 OpCodeText(op_code), connection_handle);
      handle_cs_setup_failure(connection_handle, REASON_INTERNAL_ERROR);
    }
  }

  void on_cs_read_local_supported_capabilities(CommandCompleteView view) {
    auto complete_view = LeCsReadLocalSupportedCapabilitiesCompleteView::Create(view);
    if (!complete_view.IsValid()) {
      log::warn("Get invalid LeCsReadLocalSupportedCapabilitiesComplete");
      return;
    } else if (complete_view.GetStatus() != ErrorCode::SUCCESS) {
      std::string error_code = ErrorCodeText(complete_view.GetStatus());
      log::warn("Received LeCsReadLocalSupportedCapabilitiesComplete with error code {}",
                error_code);
      return;
    }
    cs_subfeature_supported_ = complete_view.GetOptionalSubfeaturesSupported();
    num_antennas_supported_ = complete_view.GetNumAntennasSupported();
    local_num_config_supported_ = complete_view.GetNumConfigSupported();
    local_support_phase_based_ranging_ = cs_subfeature_supported_.phase_based_ranging_ == 0x01;
  }

  void on_cs_read_remote_supported_capabilities_complete(
          LeCsReadRemoteSupportedCapabilitiesCompleteView event_view) {
    if (!event_view.IsValid()) {
      log::warn("Get invalid LeCsReadRemoteSupportedCapabilitiesCompleteView");
      return;
    }
    uint16_t connection_handle = event_view.GetConnectionHandle();
    if (event_view.GetStatus() != ErrorCode::SUCCESS) {
      std::string error_code = ErrorCodeText(event_view.GetStatus());
      log::warn("Received LeCsReadRemoteSupportedCapabilitiesCompleteView with error code {}",
                error_code);
      handle_cs_setup_failure(connection_handle, REASON_INTERNAL_ERROR);
      return;
    }
    auto res_it = cs_responder_trackers_.find(connection_handle);
    if (res_it != cs_responder_trackers_.end()) {
      res_it->second.remote_support_phase_based_ranging =
              event_view.GetOptionalSubfeaturesSupported().phase_based_ranging_ == 0x01;
    }
    send_le_cs_set_default_settings(connection_handle);

    auto req_it = cs_requester_trackers_.find(connection_handle);
    if (req_it != cs_requester_trackers_.end() && req_it->second.measurement_ongoing) {
      req_it->second.remote_support_phase_based_ranging =
              event_view.GetOptionalSubfeaturesSupported().phase_based_ranging_ == 0x01;
      req_it->second.remote_num_antennas_supported_ = event_view.GetNumAntennasSupported();
      req_it->second.setup_complete = true;
      log::info("Setup phase complete, connection_handle: {}, address: {}", connection_handle,
                cs_requester_trackers_[connection_handle].address);
      req_it->second.retry_counter_for_create_config = 0;
      req_it->second.remote_num_config_supported_ = event_view.GetNumConfigSupported();
      send_le_cs_create_config(connection_handle,
                               cs_requester_trackers_[connection_handle].config_id);
    }
    log::info(
            "connection_handle:{}, num_antennas_supported:{},  num_config_supported:{},"
            "max_antenna_paths_supported:{}, roles_supported:{}, phase_based_ranging_supported: {}",
            event_view.GetConnectionHandle(), event_view.GetNumAntennasSupported(),
            event_view.GetNumConfigSupported(), event_view.GetMaxAntennaPathsSupported(),
            event_view.GetRolesSupported().ToString(),
            event_view.GetOptionalSubfeaturesSupported().phase_based_ranging_);
  }

  void on_cs_set_default_settings_complete(CommandCompleteView view) {
    auto complete_view = LeCsSetDefaultSettingsCompleteView::Create(view);
    if (!complete_view.IsValid()) {
      log::warn("Get invalid LeCsSetDefaultSettingsComplete");
      return;
    }
    if (complete_view.GetStatus() != ErrorCode::SUCCESS) {
      std::string error_code = ErrorCodeText(complete_view.GetStatus());
      log::warn("Received LeCsSetDefaultSettingsComplete with error code {}", error_code);
      uint16_t connection_handle = complete_view.GetConnectionHandle();
      handle_cs_setup_failure(connection_handle, REASON_INTERNAL_ERROR);
      return;
    }
  }

  void on_cs_security_enable_complete(LeCsSecurityEnableCompleteView event_view) {
    if (!event_view.IsValid()) {
      log::warn("Get invalid LeCsSecurityEnableCompleteView");
      return;
    }
    uint16_t connection_handle = event_view.GetConnectionHandle();
    if (event_view.GetStatus() != ErrorCode::SUCCESS) {
      std::string error_code = ErrorCodeText(event_view.GetStatus());
      log::warn("Received LeCsSecurityEnableCompleteView with error code {}", error_code);
      handle_cs_setup_failure(connection_handle, REASON_INTERNAL_ERROR);
      return;
    }
    auto req_it = cs_requester_trackers_.find(connection_handle);
    if (req_it != cs_requester_trackers_.end() && req_it->second.measurement_ongoing &&
        !req_it->second.config_set) {
      req_it->second.config_set = true;
      send_le_cs_set_procedure_parameters(event_view.GetConnectionHandle(),
                                          req_it->second.config_id,
                                          req_it->second.remote_num_antennas_supported_);
    }
    auto res_it = cs_responder_trackers_.find(connection_handle);
    if (res_it != cs_responder_trackers_.end()) {

      if (res_it->second.state == CsTrackerState::WAIT_FOR_SECURITY_ENABLED) {
        res_it->second.state = CsTrackerState::WAIT_FOR_PROCEDURE_ENABLED;
      } else {
        res_it->second.state = CsTrackerState::WAIT_FOR_SECURITY_ENABLED;
      }
    }
  }

  void on_cs_config_complete(LeCsConfigCompleteView event_view) {
    if (!event_view.IsValid()) {
      log::warn("Get invalid LeCsConfigCompleteView");
      return;
    }

    uint16_t connection_handle = event_view.GetConnectionHandle();
    if (event_view.GetStatus() != ErrorCode::SUCCESS) {
      std::string error_code = ErrorCodeText(event_view.GetStatus());
      log::warn("Received LeCsConfigCompleteView with error code {}", error_code);
      // The Create Config LL packet may arrive before the remote side has finished setting default
      // settings, which will result in create config failure. Retry to ensure the remote side has
      // completed its setup.
      if (cs_requester_trackers_.find(connection_handle) != cs_requester_trackers_.end() &&
          cs_requester_trackers_[connection_handle].retry_counter_for_create_config <
                  kMaxRetryCounterForCreateConfig) {
        cs_requester_trackers_[connection_handle].retry_counter_for_create_config++;
        log::info("send_le_cs_create_config, retry counter {}",
                  cs_requester_trackers_[connection_handle].retry_counter_for_create_config);
        send_le_cs_create_config(connection_handle,
                                 cs_requester_trackers_[connection_handle].config_id);
      } else {
        handle_cs_setup_failure(connection_handle, REASON_INTERNAL_ERROR);
      }
      return;
    }
    uint8_t config_id = event_view.GetConfigId();
    check_and_handle_conflict(connection_handle, config_id,
                              CsTrackerState::WAIT_FOR_CONFIG_COMPLETE);
    uint8_t valid_requester_states = static_cast<uint8_t>(CsTrackerState::WAIT_FOR_CONFIG_COMPLETE);
    // any state, as the remote can start over at any time.
    uint8_t valid_responder_states = static_cast<uint8_t>(CsTrackerState::UNSPECIFIED);
    if(cs_responder_trackers_.find(connection_handle) != cs_responder_trackers_.end()) {
      if(cs_responder_trackers_[connection_handle].config_id != 0xff && config_id != cs_responder_trackers_[connection_handle].config_id) {
        cs_responder_trackers_[connection_handle].config_id = config_id;
      }
    }
    CsTracker* live_tracker = get_live_tracker(connection_handle, config_id, valid_requester_states,
                                               valid_responder_states);
    if (live_tracker == nullptr) {
      log::warn("Can't find cs tracker for connection_handle {}", connection_handle);
      return;
    }
    // assign the config_id, as this may be the 1st time to get the config_id from responder.
    live_tracker->config_id = config_id;
    if (event_view.GetAction() == CsAction::CONFIG_REMOVED) {
      return;
    }
    log::verbose("Get {}", event_view.ToString());
    live_tracker->role = event_view.GetRole();
    live_tracker->main_mode_type = event_view.GetMainModeType();
    live_tracker->sub_mode_type = event_view.GetSubModeType();
    live_tracker->rtt_type = event_view.GetRttType();
    live_tracker->min_main_mode_steps = event_view.GetMinMainModeSteps();
    live_tracker->max_main_mode_steps = event_view.GetMaxMainModeSteps();
    live_tracker->main_mode_repetition = event_view.GetMainModeRepetition();
    live_tracker->mode_0_steps = event_view.GetMode0Steps();
    live_tracker->cs_sync_phy = (uint8_t)event_view.GetCsSyncPhy();
    live_tracker->channel_map = event_view.GetChannelMap();
    live_tracker->channel_map_repetition = event_view.GetChannelMapRepetition();
    live_tracker->channel_selection_type = (uint8_t)event_view.GetChannelSelectionType();
    live_tracker->ch3c_shape = (uint8_t)event_view.GetCh3cShape();
    live_tracker->ch3c_jump = event_view.GetCh3cJump();
    live_tracker->t_ip1_time = event_view.GetTIp1Time();
    live_tracker->t_ip2_time = event_view.GetTIp2Time();
    live_tracker->t_fcs_time = event_view.GetTFcsTime();
    live_tracker->t_pm_time = event_view.GetTPmTime();



    if (live_tracker->local_hci_role == hci::Role::CENTRAL) {
      // send the cmd from the BLE central only.
      send_le_cs_security_enable(connection_handle);
    }
    // TODO: else set a timeout alarm to make sure the remote would trigger the cmd.
    if (!live_tracker->local_start) {
      if (live_tracker->state == CsTrackerState::WAIT_FOR_SECURITY_ENABLED) {
        live_tracker->state = CsTrackerState::WAIT_FOR_PROCEDURE_ENABLED;
      } else {
        live_tracker->state = CsTrackerState::WAIT_FOR_SECURITY_ENABLED;
      }
    }
  }

  void on_cs_set_procedure_parameters(CommandCompleteView view) {
    auto complete_view = LeCsSetProcedureParametersCompleteView::Create(view);
    if (!complete_view.IsValid()) {
      log::warn("Get Invalid LeCsSetProcedureParametersCompleteView");
      return;
    }
    uint16_t connection_handle = complete_view.GetConnectionHandle();
    if (complete_view.GetStatus() != ErrorCode::SUCCESS) {
      std::string error_code = ErrorCodeText(complete_view.GetStatus());
      log::warn("Received LeCsSetProcedureParametersCompleteView with error code {}", error_code);
      handle_cs_setup_failure(connection_handle, REASON_INTERNAL_ERROR);
      return;
    }
    auto it = cs_requester_trackers_.find(connection_handle);
    if (it == cs_requester_trackers_.end()) {
      log::warn("Can't find cs tracker for connection_handle {}", connection_handle);
      return;
    }

    if (it->second.measurement_ongoing) {
      log::info("enable cs procedure regularly with interval: {} ms", it->second.interval_ms);
      it->second.repeating_alarm->Cancel();
      send_le_cs_procedure_enable(connection_handle, Enable::ENABLED);
      it->second.repeating_alarm->Schedule(
              common::Bind(&impl::send_le_cs_procedure_enable, common::Unretained(this),
                           connection_handle, Enable::ENABLED),
              std::chrono::milliseconds(it->second.interval_ms));
    }
  }

  CsTracker* get_live_tracker(uint16_t connection_handle, uint8_t config_id,
                              uint8_t valid_requester_states, uint8_t valid_responder_states) {
    // CAVEAT: if the remote is sending request with the same config id, the behavior is undefined.
    auto req_it = cs_requester_trackers_.find(connection_handle);
    if (req_it != cs_requester_trackers_.end() && req_it->second.state != CsTrackerState::STOPPED &&
        req_it->second.config_id == config_id &&
        (valid_requester_states & static_cast<uint8_t>(req_it->second.state)) != 0) {
      return &cs_requester_trackers_[connection_handle];
    }

    auto res_it = cs_responder_trackers_.find(connection_handle);
    if (res_it != cs_responder_trackers_.end() &&
        (res_it->second.config_id == kInvalidConfigId || res_it->second.config_id == config_id) &&
        (valid_responder_states == static_cast<uint8_t>(CsTrackerState::UNSPECIFIED) ||
         (valid_responder_states & static_cast<uint8_t>(res_it->second.state)) != 0)) {
      return &cs_responder_trackers_[connection_handle];
    }
    log::error("no valid tracker to handle the event.");
    return nullptr;
  }

  void check_and_handle_conflict(uint16_t connection_handle, uint8_t event_config_id,
                                 CsTrackerState expected_requester_state) {
    // If the local and remote were triggering the event at the same time, and the controller
    // allows that happen, the things may still get messed; If the spec can differentiate the
    // local event or remote event, that would be clearer.
    auto it = cs_requester_trackers_.find(connection_handle);
    if (it == cs_requester_trackers_.end()) {
      return;
    }
    if (event_config_id != it->second.config_id) {
      return;
    }
    if (it->second.state == expected_requester_state) {
      return;
    }
    log::warn("unexpected request from remote, which is conflict with the local measurement.");
    it->second.config_set = false;
    if (it->second.state != CsTrackerState::STOPPED) {
      stop_distance_measurement(it->second.address, connection_handle,
                                DistanceMeasurementMethod::METHOD_CS);
      // TODO: clean up the stopped callback, it should be called within stop_distance_measurement.
      distance_measurement_callbacks_->OnDistanceMeasurementStopped(
              it->second.address, REASON_REMOTE_REQUEST, METHOD_CS);
    }
  }

  void on_cs_procedure_enable_complete(LeCsProcedureEnableCompleteView event_view) {
    log::assert_that(event_view.IsValid(), "assert failed: event_view.IsValid()");
    uint16_t connection_handle = event_view.GetConnectionHandle();
    log::debug("on cs procedure enabled complete");
    if (event_view.GetStatus() != ErrorCode::SUCCESS) {
      std::string error_code = ErrorCodeText(event_view.GetStatus());
      log::warn("Received LeCsProcedureEnableCompleteView with error code {}", error_code);
      handle_cs_setup_failure(connection_handle, REASON_INTERNAL_ERROR);
      return;
    }
    uint8_t config_id = event_view.GetConfigId();

    CsTracker* live_tracker = nullptr;
    if (event_view.GetState() == Enable::ENABLED) {
      check_and_handle_conflict(connection_handle, config_id,
                                CsTrackerState::WAIT_FOR_PROCEDURE_ENABLED);
      uint8_t valid_requester_states =
              static_cast<uint8_t>(CsTrackerState::WAIT_FOR_PROCEDURE_ENABLED);
      uint8_t valid_responder_states =
              static_cast<uint8_t>(CsTrackerState::STOPPED) |
              static_cast<uint8_t>(CsTrackerState::INIT) |
              static_cast<uint8_t>(CsTrackerState::STARTED) |
              static_cast<uint8_t>(CsTrackerState::WAIT_FOR_PROCEDURE_ENABLED);
      live_tracker = get_live_tracker(connection_handle, config_id, valid_requester_states,
                                      valid_responder_states);
      if (live_tracker == nullptr) {
        log::error("no tracker is available for {}", connection_handle);
        return;
      }
      log::info("Procedure enabled, {}", event_view.ToString());
      live_tracker->state = CsTrackerState::STARTED;
      // assign the config_id, as this is may be the 1st time to get it for responder;
      live_tracker->config_id = config_id;
      live_tracker->selected_tx_power = event_view.GetSelectedTxPower();
      live_tracker->n_procedure_count = event_view.GetProcedureCount();
      live_tracker->tone_antenna_config_selection = event_view.GetToneAntennaConfigSelection();
      live_tracker->subevent_len = event_view.GetSubeventLen();
      live_tracker->subevents_per_event = event_view.GetSubeventsPerEvent();
      live_tracker->subevent_interval = event_view.GetSubeventInterval();
      live_tracker->event_interval = event_view.GetEventInterval();
      live_tracker->procedure_interval = event_view.GetProcedureInterval();
      live_tracker->max_procedure_len = event_view.GetMaxProcedureLen();

      if (live_tracker->local_start && live_tracker->waiting_for_start_callback) {
        live_tracker->waiting_for_start_callback = false;
        distance_measurement_callbacks_->OnDistanceMeasurementStarted(live_tracker->address,
                                                                      METHOD_CS);
      }
      struct timeval tv;
      gettimeofday(&tv, NULL);
      proc_start_timestampMs = tv.tv_sec*1e6*1ll + tv.tv_usec*1ll;
    } else if (event_view.GetState() == Enable::DISABLED) {
      uint8_t valid_requester_states = static_cast<uint8_t>(CsTrackerState::STARTED);
      uint8_t valid_responder_states = static_cast<uint8_t>(CsTrackerState::STARTED);
      live_tracker = get_live_tracker(connection_handle, config_id, valid_requester_states,
                                      valid_responder_states);
      if (live_tracker == nullptr) {
        log::error("no tracker is available for {}", connection_handle);
        return;
      }
      reset_tracker_on_stopped(*live_tracker);
    }
    // reset the procedure data list.
    std::vector<CsProcedureData>& data_list = live_tracker->procedure_data_list;
    while (!data_list.empty()) {
      data_list.erase(data_list.begin());
    }
  }

  void on_cs_subevent(LeMetaEventView event) {
    if (!event.IsValid()) {
      log::error("Received invalid LeMetaEventView");
      return;
    }

    // Common data for LE_CS_SUBEVENT_RESULT and LE_CS_SUBEVENT_RESULT_CONTINUE,
    uint16_t connection_handle = 0;
    int8_t reference_power_level = 0;
    CsProcedureDoneStatus procedure_done_status;
    CsSubeventDoneStatus subevent_done_status;
    ProcedureAbortReason procedure_abort_reason;
    SubeventAbortReason subevent_abort_reason;
    std::vector<LeCsResultDataStructure> result_data_structures;
    CsTracker* live_tracker = nullptr;
    uint8_t valid_requester_states = static_cast<uint8_t>(CsTrackerState::STARTED);
    uint8_t valid_responder_states = static_cast<uint8_t>(CsTrackerState::STARTED);
    if (event.GetSubeventCode() == SubeventCode::LE_CS_SUBEVENT_RESULT) {
      auto cs_event_result = LeCsSubeventResultView::Create(event);
      if (!cs_event_result.IsValid()) {
        log::warn("Get invalid LeCsSubeventResultView");
        return;
      }
      connection_handle = cs_event_result.GetConnectionHandle();
      live_tracker = get_live_tracker(connection_handle, cs_event_result.GetConfigId(),
                                      valid_requester_states, valid_responder_states);
      if (live_tracker == nullptr) {
        log::error("no live tracker is available for {}", connection_handle);
        return;
      }
      procedure_done_status = cs_event_result.GetProcedureDoneStatus();
      subevent_done_status = cs_event_result.GetSubeventDoneStatus();
      procedure_abort_reason = cs_event_result.GetProcedureAbortReason();
      subevent_abort_reason = cs_event_result.GetSubeventAbortReason();
      result_data_structures = cs_event_result.GetResultDataStructures();

      CsProcedureData* procedure_data =
              init_cs_procedure_data(live_tracker, cs_event_result.GetProcedureCounter(),
                                     cs_event_result.GetNumAntennaPaths());
      if (live_tracker->role == CsRole::INITIATOR) {
        procedure_data->frequency_compensation.push_back(
                cs_event_result.GetFrequencyCompensation());
      }
      // RAS
      log::debug("RAS Update subevent_header counter:{}", procedure_data->ras_subevent_counter_++);
      auto& ras_subevent_header = procedure_data->ras_subevent_header_;
      ras_subevent_header.start_acl_conn_event_ = cs_event_result.GetStartAclConnEvent();
      ras_subevent_header.frequency_compensation_ = cs_event_result.GetFrequencyCompensation();
      ras_subevent_header.reference_power_level_ = cs_event_result.GetReferencePowerLevel();
      ras_subevent_header.num_steps_reported_ = 0;
      reference_power_level = cs_event_result.GetReferencePowerLevel();
      log::verbose("reference power level: {}", reference_power_level);

      log::info(
          "LE_CS_SUBEVENT_RESULT: start_acl_conn_event {} procedure_counter {} acl_handle {}",
          cs_event_result.GetStartAclConnEvent(),
          cs_event_result.GetProcedureCounter(),
          cs_event_result.GetConnectionHandle());
    } else {
      auto cs_event_result = LeCsSubeventResultContinueView::Create(event);
      if (!cs_event_result.IsValid()) {
        log::warn("Get invalid LeCsSubeventResultContinueView");
        return;
      }
      connection_handle = cs_event_result.GetConnectionHandle();
      live_tracker = get_live_tracker(connection_handle, cs_event_result.GetConfigId(),
                                      valid_requester_states, valid_responder_states);
      procedure_done_status = cs_event_result.GetProcedureDoneStatus();
      subevent_done_status = cs_event_result.GetSubeventDoneStatus();
      procedure_abort_reason = cs_event_result.GetProcedureAbortReason();
      subevent_abort_reason = cs_event_result.GetSubeventAbortReason();
      result_data_structures = cs_event_result.GetResultDataStructures();

      if (live_tracker == nullptr) {
        log::warn("Can't find any tracker for {}", connection_handle);
        return;
      }
    }

    uint16_t counter = live_tracker->procedure_counter;
    log::debug(
            "Connection_handle {}, procedure_done_status: {}, subevent_done_status: {}, counter: "
            "{}",
            connection_handle, CsProcedureDoneStatusText(procedure_done_status),
            CsSubeventDoneStatusText(subevent_done_status), counter);

    if (procedure_done_status == CsProcedureDoneStatus::ABORTED ||
        subevent_done_status == CsSubeventDoneStatus::ABORTED) {
      log::info(
              "Received CS Subevent with procedure_abort_reason:{}, subevent_abort_reason:{}, "
              "connection_handle:{}, counter:{}",
              ProcedureAbortReasonText(procedure_abort_reason),
              SubeventAbortReasonText(subevent_abort_reason), connection_handle, counter);
    }

    CsProcedureData* procedure_data = get_procedure_data(live_tracker, counter);
    if (procedure_data == nullptr) {
      log::warn("no procedure data for counter {} of connection {}", counter, connection_handle);
      return;
    }
    procedure_data->ras_subevent_header_.num_steps_reported_ += result_data_structures.size();
    if (subevent_done_status == CsSubeventDoneStatus::ALL_RESULTS_COMPLETE) {
      procedure_data->contains_complete_subevent_ = true;
    }

    if (procedure_abort_reason != ProcedureAbortReason::NO_ABORT ||
        subevent_abort_reason != SubeventAbortReason::NO_ABORT) {
      // Even the procedure is aborted, we should keep following process and
      // handle it when all corresponding remote data received.
      procedure_data->ras_subevent_header_.ranging_abort_reason_ =
              static_cast<RangingAbortReason>(procedure_abort_reason);
      procedure_data->ras_subevent_header_.subevent_abort_reason_ =
              static_cast<bluetooth::ras::SubeventAbortReason>(subevent_abort_reason);
    }
    parse_cs_result_data(result_data_structures, *procedure_data, live_tracker->role);
    if (live_tracker->role == CsRole::INITIATOR) {
      procedure_data->initiator_reference_power_level = reference_power_level;
    }
    else if (live_tracker->role == CsRole::REFLECTOR) {
      procedure_data->reflector_reference_power_level = reference_power_level;
    }
    // Update procedure status
    procedure_data->local_status = procedure_done_status;
    check_cs_procedure_complete(live_tracker, procedure_data, connection_handle);

    if (live_tracker->local_start) {
      // Skip to send remote
      return;
    }

    // Send data to RAS server
    if (subevent_done_status != CsSubeventDoneStatus::PARTIAL_RESULTS) {
      procedure_data->ras_subevent_header_.ranging_done_status_ =
              static_cast<RangingDoneStatus>(procedure_done_status);
      procedure_data->ras_subevent_header_.subevent_done_status_ =
              static_cast<SubeventDoneStatus>(subevent_done_status);
      auto builder = RasSubeventBuilder::Create(procedure_data->ras_subevent_header_,
                                                procedure_data->ras_subevent_data_);
      auto subevent_raw = builder_to_bytes(std::move(builder));
      append_vector(procedure_data->ras_raw_data_, subevent_raw);
      // erase buffer
      procedure_data->ras_subevent_data_.clear();
      send_on_demand_data(live_tracker->address, procedure_data);
      // remove procedure data sent previously
      if (procedure_done_status == CsProcedureDoneStatus::ALL_RESULTS_COMPLETE) {
        delete_consumed_procedure_data(live_tracker, live_tracker->procedure_counter);
      }
    }
  }

  void send_on_demand_data(Address address, CsProcedureData* procedure_data) {
    // Check is last segment or not.
    uint16_t unsent_data_size =
            procedure_data->ras_raw_data_.size() - procedure_data->ras_raw_data_index_;
    if (procedure_data->local_status != CsProcedureDoneStatus::PARTIAL_RESULTS &&
        unsent_data_size <= kMtuForRasData) {
      procedure_data->segmentation_header_.last_segment_ = 1;
    } else if (unsent_data_size < kMtuForRasData) {
      log::verbose("waiting for more data, current unsent data size {}", unsent_data_size);
      return;
    }

    // Create raw data for segment_data;
    uint16_t copy_size = unsent_data_size < kMtuForRasData ? unsent_data_size : kMtuForRasData;
    auto copy_start = procedure_data->ras_raw_data_.begin() + procedure_data->ras_raw_data_index_;
    auto copy_end = copy_start + copy_size;
    std::vector<uint8_t> subevent_data(copy_start, copy_end);
    procedure_data->ras_raw_data_index_ += copy_size;

    auto builder =
            RangingDataSegmentBuilder::Create(procedure_data->segmentation_header_, subevent_data);
    auto segment_data = builder_to_bytes(std::move(builder));

    log::debug("counter: {}, size:{}", procedure_data->counter, (uint16_t)segment_data.size());
    distance_measurement_callbacks_->OnRasFragmentReady(
            address, procedure_data->counter, procedure_data->segmentation_header_.last_segment_,
            segment_data);

    procedure_data->segmentation_header_.first_segment_ = 0;
    procedure_data->segmentation_header_.rolling_segment_counter_++;
    procedure_data->segmentation_header_.rolling_segment_counter_ %= 64;
    if (procedure_data->segmentation_header_.last_segment_) {
      // last segment sent, clear buffer
      procedure_data->ras_raw_data_.clear();
    } else if (unsent_data_size > kMtuForRasData) {
      send_on_demand_data(address, procedure_data);
    }
  }

  void handle_remote_data(const Address address, uint16_t connection_handle,
                          const std::vector<uint8_t> raw_data) {
    log::debug("address:{}, connection_handle 0x{:04x}, size:{}", address.ToString(),
               connection_handle, raw_data.size());

    if (cs_requester_trackers_.find(connection_handle) == cs_requester_trackers_.end()) {
      log::warn("can't find tracker for 0x{:04x}", connection_handle);
      return;
    }
    if (cs_requester_trackers_[connection_handle].state != CsTrackerState::STARTED) {
      log::warn("The measurement for {} is stopped, ignore the remote data.", connection_handle);
      return;
    }
    auto& tracker = cs_requester_trackers_[connection_handle];

    SegmentationHeader segmentation_header;
    PacketView<kLittleEndian> packet_bytes_view(std::make_shared<std::vector<uint8_t>>(raw_data));
    auto after = SegmentationHeader::Parse(&segmentation_header, packet_bytes_view.begin());
    if (after == packet_bytes_view.begin()) {
      log::warn("Invalid segment data");
      return;
    }

    log::verbose(
        "Receive segment for segment counter {}, size {}",
        segmentation_header.rolling_segment_counter_,
        raw_data.size());

    PacketView<kLittleEndian> segment_data(std::make_shared<std::vector<uint8_t>>(raw_data));
    if (segmentation_header.first_segment_) {
      auto segment = FirstRangingDataSegmentView::Create(segment_data);
      if (!segment.IsValid()) {
        log::warn("Invalid segment data");
        return;
      }
      tracker.ranging_header_ = segment.GetRangingHeader();

      auto begin = segment.GetSegmentationHeader().size() + segment.GetRangingHeader().size();
      tracker.segment_data_ =
              PacketViewForRecombination(segment.GetLittleEndianSubview(begin, segment.size()));
    } else {
      auto segment = RangingDataSegmentView::Create(segment_data);
      if (!segment.IsValid()) {
        log::warn("Invalid segment data");
        return;
      }
      tracker.segment_data_.AppendPacketView(
              segment.GetLittleEndianSubview(segmentation_header.size(), segment.size()));
    }

    if (segmentation_header.last_segment_) {
      parse_ras_segments(tracker.ranging_header_, tracker.segment_data_, connection_handle);
    }
  }

  void handle_remote_data_timeout(const Address address, uint16_t connection_handle) {
    log::warn("address:{}, connection_handle 0x{:04x}", address.ToString(), connection_handle);

    if (cs_requester_trackers_.find(connection_handle) == cs_requester_trackers_.end()) {
      log::error("Can't find CS tracker for connection_handle {}", connection_handle);
      return;
    }
    auto& tracker = cs_requester_trackers_[connection_handle];
    if (tracker.measurement_ongoing && tracker.local_start) {
      cs_requester_trackers_[connection_handle].repeating_alarm->Cancel();
      send_le_cs_procedure_enable(connection_handle, Enable::DISABLED);
      distance_measurement_callbacks_->OnDistanceMeasurementStopped(
              tracker.address, REASON_INTERNAL_ERROR, METHOD_CS);
    }
    reset_tracker_on_stopped(tracker);
  }

  void parse_ras_segments(RangingHeader ranging_header, PacketViewForRecombination& segment_data,
                          uint16_t connection_handle) {
    log::verbose("Data size {}, Ranging_header {}", segment_data.size(), ranging_header.ToString());
    auto procedure_data =
            get_procedure_data_for_ras(connection_handle, ranging_header.ranging_counter_);
    if (procedure_data == nullptr) {
      return;
    }

    uint8_t num_antenna_paths = 0;
    for (uint8_t i = 0; i < 4; i++) {
      if ((ranging_header.antenna_paths_mask_ & (1 << i)) != 0) {
        num_antenna_paths++;
      }
    }

    // Get role of the remote device
    CsRole remote_role = cs_requester_trackers_[connection_handle].role == CsRole::INITIATOR
                                 ? CsRole::REFLECTOR
                                 : CsRole::INITIATOR;

    auto parse_index = segment_data.begin();
    uint16_t remaining_data_size = std::distance(parse_index, segment_data.end());

    // Parse subevents
    while (remaining_data_size > 0) {
      RasSubeventHeader subevent_header;
      // Parse header
      auto after = RasSubeventHeader::Parse(&subevent_header, parse_index);
      if (after == parse_index) {
        log::warn("Received invalid subevent_header data");
        return;
      }
      parse_index = after;
      log::debug("remote_role:{} subevent_header: {}", remote_role, subevent_header.ToString());

      // Parse step data
      for (uint8_t i = 0; i < subevent_header.num_steps_reported_; i++) {
        StepMode step_mode;
        after = StepMode::Parse(&step_mode, parse_index);
        if (after == parse_index) {
          log::warn("Received invalid step_mode data");
          return;
        }
        parse_index = after;
        log::verbose("step:{}, {}", (uint16_t)i, step_mode.ToString());
        if (step_mode.aborted_) {
          continue;
        }

        switch (step_mode.mode_type_) {
          case 0: {
            if (remote_role == CsRole::INITIATOR) {
              LeCsMode0InitatorData tone_data;
              after = LeCsMode0InitatorData::Parse(&tone_data, parse_index);
              if (after == parse_index) {
                log::warn("Error invalid mode {} data, role:{}", step_mode.mode_type_,
                          CsRoleText(remote_role));
                return;
              }
              parse_index = after;
            } else {
              LeCsMode0ReflectorData tone_data;
              after = LeCsMode0ReflectorData::Parse(&tone_data, parse_index);
              if (after == parse_index) {
                log::warn("Error invalid mode {} data, role:{}", step_mode.mode_type_,
                          CsRoleText(remote_role));
                return;
              }
              log::verbose("step_data: {}", tone_data.ToString());
              procedure_data->packet_quality_reflector.push_back(tone_data.packet_quality_);
              procedure_data->rssi_reflector.push_back(tone_data.packet_rssi_);
            }
            parse_index = after;
          } break;
          case 1: {
            if (remote_role == CsRole::INITIATOR) {
              if (procedure_data->contains_sounding_sequence_remote_) {
                LeCsMode1InitatorDataWithPacketPct tone_data;
                after = LeCsMode1InitatorDataWithPacketPct::Parse(&tone_data, parse_index);
                if (after == parse_index) {
                  log::warn("Error invalid mode {} data, role:{}", step_mode.mode_type_,
                            CsRoleText(remote_role));
                  return;
                }
                parse_index = after;
                procedure_data->toa_tod_initiators.emplace_back(tone_data.toa_tod_initiator_);
                procedure_data->packet_quality_initiator.emplace_back(tone_data.packet_quality_);
                procedure_data->rssi_initiator.push_back(tone_data.packet_rssi_);
                procedure_data->packet_nadm_initiator.push_back((int8_t)tone_data.packet_nadm_);
              } else {
                LeCsMode1InitatorData tone_data;
                after = LeCsMode1InitatorData::Parse(&tone_data, parse_index);
                if (after == parse_index) {
                  log::warn("Error invalid mode {} data, role:{}", step_mode.mode_type_,
                            CsRoleText(remote_role));
                  return;
                }
                parse_index = after;
                procedure_data->toa_tod_initiators.emplace_back(tone_data.toa_tod_initiator_);
                procedure_data->packet_quality_initiator.emplace_back(tone_data.packet_quality_);
                procedure_data->rssi_initiator.push_back(tone_data.packet_rssi_);
                procedure_data->packet_nadm_initiator.push_back((int8_t)tone_data.packet_nadm_);
              }
            } else {
              if (procedure_data->contains_sounding_sequence_remote_) {
                LeCsMode1ReflectorDataWithPacketPct tone_data;
                after = LeCsMode1ReflectorDataWithPacketPct::Parse(&tone_data, parse_index);
                if (after == parse_index) {
                  log::warn("Error invalid mode {} data, role:{}", step_mode.mode_type_,
                            CsRoleText(remote_role));
                  return;
                }
                parse_index = after;
                procedure_data->tod_toa_reflectors.emplace_back(tone_data.tod_toa_reflector_);
                procedure_data->packet_quality_reflector.emplace_back(tone_data.packet_quality_);
                procedure_data->rssi_reflector.push_back(tone_data.packet_rssi_);
                procedure_data->packet_nadm_reflector.push_back((int8_t)tone_data.packet_nadm_);
              } else {
                LeCsMode1ReflectorData tone_data;
                after = LeCsMode1ReflectorData::Parse(&tone_data, parse_index);
                if (after == parse_index) {
                  log::warn("Error invalid mode {} data, role:{}", step_mode.mode_type_,
                            CsRoleText(remote_role));
                  return;
                }
                parse_index = after;
                procedure_data->tod_toa_reflectors.emplace_back(tone_data.tod_toa_reflector_);
                procedure_data->packet_quality_reflector.emplace_back(tone_data.packet_quality_);
                procedure_data->rssi_reflector.push_back(tone_data.packet_rssi_);
                procedure_data->packet_nadm_reflector.push_back((int8_t)tone_data.packet_nadm_);
              }
            }
          } break;
          case 2: {
            uint8_t num_tone_data = num_antenna_paths + 1;
            uint8_t data_len = 1 + (4 * num_tone_data);
            remaining_data_size = std::distance(parse_index, segment_data.end());
            if (remaining_data_size < data_len) {
              log::warn(
                      "insufficient length for LeCsMode2Data, num_tone_data {}, "
                      "remaining_data_size {}",
                      num_tone_data, remaining_data_size);
              return;
            }
            std::vector<uint8_t> vector_for_num_tone_data = {num_tone_data};
            PacketView<kLittleEndian> packet_view_for_num_tone_data(
                    std::make_shared<std::vector<uint8_t>>(vector_for_num_tone_data));
            PacketViewForRecombination packet_bytes_view =
                    PacketViewForRecombination(packet_view_for_num_tone_data);
            auto subview_begin = std::distance(segment_data.begin(), parse_index);
            packet_bytes_view.AppendPacketView(
                    segment_data.GetLittleEndianSubview(subview_begin, subview_begin + data_len));
            LeCsMode2Data tone_data;
            after = LeCsMode2Data::Parse(&tone_data, packet_bytes_view.begin());
            if (after == packet_bytes_view.begin()) {
              log::warn("Error invalid mode {} data, role:{}", step_mode.mode_type_,
                        CsRoleText(remote_role));
              return;
            }
            parse_index += data_len;
            uint8_t permutation_index = tone_data.antenna_permutation_index_;
            if (remote_role == CsRole::INITIATOR) {
              procedure_data->antenna_permutation_index_initiator.push_back(permutation_index);
            } else {
              procedure_data->antenna_permutation_index_reflector.push_back(permutation_index);
            }
            // Parse in ascending order of antenna position with tone extension data at the end
            for (uint8_t k = 0; k < num_tone_data; k++) {
              uint8_t antenna_path =
                      k == num_antenna_paths
                              ? num_antenna_paths
                              : cs_antenna_permutation_array_[permutation_index][k] - 1;
              double i_value = get_iq_value(tone_data.tone_data_[k].i_sample_);
              double q_value = get_iq_value(tone_data.tone_data_[k].q_sample_);
              uint8_t tone_quality_indicator = tone_data.tone_data_[k].tone_quality_indicator_;
              log::verbose(
                  "antenna_path {}, {:f}, {:f}", (uint16_t)(antenna_path + 1), i_value, q_value);
              if (remote_role == CsRole::INITIATOR) {
                procedure_data->tone_pct_initiator[antenna_path].emplace_back(i_value, q_value);
                procedure_data->tone_quality_indicator_initiator[antenna_path].emplace_back(
                        tone_quality_indicator);
              } else {
                procedure_data->tone_pct_reflector[antenna_path].emplace_back(i_value, q_value);
                procedure_data->tone_quality_indicator_reflector[antenna_path].emplace_back(
                        tone_quality_indicator);
              }
            }
          } break;
          case 3: {
            uint8_t num_tone_data = num_antenna_paths + 1;
            uint8_t data_len = 7 + (4 * num_tone_data);
            if (procedure_data->contains_sounding_sequence_local_) {
              data_len += 3;  // 3 bytes for packet_pct1, packet_pct2
            }
            remaining_data_size = std::distance(parse_index, segment_data.end());
            if (remaining_data_size < data_len) {
              log::warn(
                      "insufficient length for LeCsMode2Data, num_tone_data {}, "
                      "remaining_data_size {}",
                      num_tone_data, remaining_data_size);
              return;
            }
            std::vector<uint8_t> vector_for_num_tone_data = {num_tone_data};
            PacketView<kLittleEndian> packet_view_for_num_tone_data(
                    std::make_shared<std::vector<uint8_t>>(vector_for_num_tone_data));
            PacketViewForRecombination packet_bytes_view =
                    PacketViewForRecombination(packet_view_for_num_tone_data);
            auto subview_begin = std::distance(segment_data.begin(), parse_index);
            packet_bytes_view.AppendPacketView(
                    segment_data.GetLittleEndianSubview(subview_begin, subview_begin + data_len));
            uint8_t permutation_index = 0;
            std::vector<LeCsToneDataWithQuality> view_tone_data = {};
            if (remote_role == CsRole::INITIATOR) {
              if (procedure_data->contains_sounding_sequence_local_) {
                LeCsMode3InitatorDataWithPacketPct tone_data_view;
                after = LeCsMode3InitatorDataWithPacketPct::Parse(&tone_data_view,
                                                                  packet_bytes_view.begin());
                if (after == packet_bytes_view.begin()) {
                  log::warn("Error invalid mode {} data, role:{}", step_mode.mode_type_,
                            CsRoleText(remote_role));
                  return;
                }
                parse_index += data_len;
                log::verbose("step_data: {}", tone_data_view.ToString());
                permutation_index = tone_data_view.antenna_permutation_index_;
                procedure_data->antenna_permutation_index_reflector.push_back(permutation_index);
                procedure_data->rssi_initiator.emplace_back(tone_data_view.packet_rssi_);
                procedure_data->toa_tod_initiators.emplace_back(tone_data_view.toa_tod_initiator_);
                procedure_data->packet_nadm_initiator.push_back((int8_t)tone_data_view.packet_nadm_);
                procedure_data->packet_quality_initiator.emplace_back(
                        tone_data_view.packet_quality_);
                auto tone_data = tone_data_view.tone_data_;
                view_tone_data.reserve(tone_data.size());
                view_tone_data.insert(view_tone_data.end(), tone_data.begin(), tone_data.end());
              } else {
                LeCsMode3InitatorData tone_data_view;
                after = LeCsMode3InitatorData::Parse(&tone_data_view, packet_bytes_view.begin());
                if (after == packet_bytes_view.begin()) {
                  log::warn("Error invalid mode {} data, role:{}", step_mode.mode_type_,
                            CsRoleText(remote_role));
                  return;
                }
                parse_index += data_len;
                log::verbose("step_data: {}", tone_data_view.ToString());
                permutation_index = tone_data_view.antenna_permutation_index_;
                procedure_data->rssi_initiator.emplace_back(tone_data_view.packet_rssi_);
                procedure_data->toa_tod_initiators.emplace_back(tone_data_view.toa_tod_initiator_);
                procedure_data->packet_nadm_initiator.push_back((int8_t)tone_data_view.packet_nadm_);
                procedure_data->packet_quality_initiator.emplace_back(
                        tone_data_view.packet_quality_);
                auto tone_data = tone_data_view.tone_data_;
                view_tone_data.reserve(tone_data.size());
                view_tone_data.insert(view_tone_data.end(), tone_data.begin(), tone_data.end());
              }
            } else {
              if (procedure_data->contains_sounding_sequence_local_) {
                LeCsMode3ReflectorDataWithPacketPct tone_data_view;
                after = LeCsMode3ReflectorDataWithPacketPct::Parse(&tone_data_view,
                                                                   packet_bytes_view.begin());
                if (after == packet_bytes_view.begin()) {
                  log::warn("Error invalid mode {} data, role:{}", step_mode.mode_type_,
                            CsRoleText(remote_role));
                  return;
                }
                parse_index += data_len;
                log::verbose("step_data: {}", tone_data_view.ToString());
                permutation_index = tone_data_view.antenna_permutation_index_;
                procedure_data->rssi_reflector.emplace_back(tone_data_view.packet_rssi_);
                procedure_data->tod_toa_reflectors.emplace_back(tone_data_view.tod_toa_reflector_);
                procedure_data->packet_nadm_reflector.push_back((int8_t)tone_data_view.packet_nadm_);
                procedure_data->packet_quality_reflector.emplace_back(
                        tone_data_view.packet_quality_);
                auto tone_data = tone_data_view.tone_data_;
                view_tone_data.reserve(tone_data.size());
                view_tone_data.insert(view_tone_data.end(), tone_data.begin(), tone_data.end());
              } else {
                LeCsMode3ReflectorData tone_data_view;
                after = LeCsMode3ReflectorData::Parse(&tone_data_view, packet_bytes_view.begin());
                if (after == packet_bytes_view.begin()) {
                  log::warn("Error invalid mode {} data, role:{}", step_mode.mode_type_,
                            CsRoleText(remote_role));
                  return;
                }
                parse_index += data_len;
                log::verbose("step_data: {}", tone_data_view.ToString());
                permutation_index = tone_data_view.antenna_permutation_index_;
                procedure_data->rssi_reflector.emplace_back(tone_data_view.packet_rssi_);
                procedure_data->tod_toa_reflectors.emplace_back(tone_data_view.tod_toa_reflector_);
                procedure_data->packet_nadm_reflector.push_back((int8_t)tone_data_view.packet_nadm_);
                procedure_data->packet_quality_reflector.emplace_back(
                        tone_data_view.packet_quality_);
                auto tone_data = tone_data_view.tone_data_;
                view_tone_data.reserve(tone_data.size());
                view_tone_data.insert(view_tone_data.end(), tone_data.begin(), tone_data.end());
              }
            }
            // Parse in ascending order of antenna position with tone extension data at the end
            for (uint16_t k = 0; k < num_tone_data; k++) {
              uint8_t antenna_path =
                      k == num_antenna_paths
                              ? num_antenna_paths
                              : cs_antenna_permutation_array_[permutation_index][k] - 1;
              double i_value = get_iq_value(view_tone_data[k].i_sample_);
              double q_value = get_iq_value(view_tone_data[k].q_sample_);
              uint8_t tone_quality_indicator = view_tone_data[k].tone_quality_indicator_;
              log::verbose("antenna_path {}, {:f}, {:f}", (uint16_t)(antenna_path + 1), i_value,
                           q_value);
              if (remote_role == CsRole::INITIATOR) {
                procedure_data->tone_pct_initiator[antenna_path].emplace_back(i_value, q_value);
                procedure_data->tone_quality_indicator_initiator[antenna_path].emplace_back(
                        tone_quality_indicator);
              } else {
                procedure_data->tone_pct_reflector[antenna_path].emplace_back(i_value, q_value);
                procedure_data->tone_quality_indicator_reflector[antenna_path].emplace_back(
                        tone_quality_indicator);
              }
            }
          } break;
          default:
            log::error("Unexpect mode: {}", step_mode.mode_type_);
            return;
        }
      }
      remaining_data_size = std::distance(parse_index, segment_data.end());
      log::debug("Parse subevent done with remaining data size {}", remaining_data_size);
      procedure_data->remote_status = (CsProcedureDoneStatus)subevent_header.ranging_done_status_;
    }
    check_cs_procedure_complete(&cs_requester_trackers_[connection_handle], procedure_data,
                                connection_handle);
  }

  CsProcedureData* init_cs_procedure_data(CsTracker* live_tracker, uint16_t procedure_counter,
                                          uint8_t num_antenna_paths) {
    // Update procedure count
    live_tracker->procedure_counter = procedure_counter;

    std::vector<CsProcedureData>& data_list = live_tracker->procedure_data_list;
    for (auto& data : data_list) {
      if (data.counter == procedure_counter) {
        // Data already exists, return
        log::warn("duplicated procedure counter - {}.", procedure_counter);
        return &data;
      }
    }
    log::verbose("Create data for procedure_counter: {}", procedure_counter);
    data_list.emplace_back(procedure_counter, num_antenna_paths, live_tracker->config_id,
                           live_tracker->selected_tx_power);

    // Check if sounding phase-based ranging is supported, and RTT type contains a sounding
    // sequence
    bool rtt_contains_sounding_sequence = false;
    if (live_tracker->rtt_type == CsRttType::RTT_WITH_32_BIT_SOUNDING_SEQUENCE ||
        live_tracker->rtt_type == CsRttType::RTT_WITH_96_BIT_SOUNDING_SEQUENCE) {
      rtt_contains_sounding_sequence = true;
    }
    data_list.back().contains_sounding_sequence_local_ =
            local_support_phase_based_ranging_ && rtt_contains_sounding_sequence;
    data_list.back().contains_sounding_sequence_remote_ =
            live_tracker->remote_support_phase_based_ranging && rtt_contains_sounding_sequence;

    // Append ranging header raw data
    std::vector<uint8_t> ranging_header_raw = {};
    BitInserter bi(ranging_header_raw);
    data_list.back().ranging_header_.Serialize(bi);
    append_vector(data_list.back().ras_raw_data_, ranging_header_raw);

    if (data_list.size() > kProcedureDataBufferSize) {
      log::warn("buffer full, drop procedure data with counter: {}", data_list.front().counter);
      data_list.erase(data_list.begin());
    }
    return &data_list.back();
  }

  CsProcedureData* get_procedure_data(CsTracker* live_tracker, uint16_t counter) {
    std::vector<CsProcedureData>& data_list = live_tracker->procedure_data_list;
    CsProcedureData* procedure_data = nullptr;
    for (uint8_t i = 0; i < data_list.size(); i++) {
      if (data_list[i].counter == counter) {
        procedure_data = &data_list[i];
        // break;
      }
    }
    if (procedure_data == nullptr) {
      log::warn("Can't find data for counter: {}", counter);
    }
    return procedure_data;
  }

  CsProcedureData* get_procedure_data_for_ras(uint16_t connection_handle,
                                              uint16_t ranging_counter) {
    std::vector<CsProcedureData>& data_list =
            cs_requester_trackers_[connection_handle].procedure_data_list;
    CsProcedureData* procedure_data = nullptr;
    for (auto& i : data_list) {
      if ((i.counter & kRangingCounterMask) == ranging_counter) {
        procedure_data = &i;
        break;
      }
    }
    if (procedure_data == nullptr) {
      log::warn("Can't find data for connection_handle:{}, ranging_counter: {}", connection_handle,
                ranging_counter);
    }
    return procedure_data;
  }

  void check_cs_procedure_complete(CsTracker* live_tracker, CsProcedureData* procedure_data,
                                   uint16_t connection_handle) const {
    if (live_tracker->local_start &&
        procedure_data->local_status == CsProcedureDoneStatus::ALL_RESULTS_COMPLETE &&
        procedure_data->remote_status == CsProcedureDoneStatus::ALL_RESULTS_COMPLETE &&
        procedure_data->contains_complete_subevent_) {
      log::debug("Procedure complete counter:{} data size:{}, main_mode_type:{}, sub_mode_type:{}",
                 (uint16_t)procedure_data->counter, (uint16_t)procedure_data->step_channel.size(),
                 (uint16_t)live_tracker->main_mode_type, (uint16_t)live_tracker->sub_mode_type);

      if (ranging_hal_->IsBound()) {
        // Use algorithm in the HAL
        bluetooth::hal::ChannelSoundingRawData raw_data;
        raw_data.procedure_counter_ = live_tracker->procedure_counter;
        raw_data.step_mode_ = procedure_data->step_mode;
        raw_data.measured_freq_offset_= procedure_data->measured_freq_offset;
        raw_data.init_packet_rssi_ = procedure_data->rssi_initiator;
	    raw_data.packet_quality_initiator_ = procedure_data->packet_quality_initiator;
	    raw_data.refl_packet_rssi_ = procedure_data->rssi_reflector;
	    raw_data.packet_quality_reflector_ = procedure_data->packet_quality_reflector;
	    raw_data.frequency_compensation_= procedure_data->frequency_compensation;
        raw_data.num_antenna_paths_ = procedure_data->num_antenna_paths;
        raw_data.step_channel_ = procedure_data->step_channel;
        raw_data.tone_pct_initiator_ = procedure_data->tone_pct_initiator;
        raw_data.tone_quality_indicator_initiator_ =
                procedure_data->tone_quality_indicator_initiator;
        raw_data.antenna_permutation_index_initiator_ =
        procedure_data->antenna_permutation_index_initiator;
        raw_data.antenna_permutation_index_reflector_ =
        procedure_data->antenna_permutation_index_reflector;
        raw_data.tone_pct_reflector_ = procedure_data->tone_pct_reflector;
        raw_data.tone_quality_indicator_reflector_ =
                procedure_data->tone_quality_indicator_reflector;
        raw_data.toa_tod_initiators_ = procedure_data->toa_tod_initiators;
        raw_data.tod_toa_reflectors_ = procedure_data->tod_toa_reflectors;
        raw_data.packet_quality_initiator_ = procedure_data->packet_quality_initiator;
        raw_data.packet_quality_reflector_ = procedure_data->packet_quality_reflector;
        raw_data.initiator_reference_power_level = procedure_data->initiator_reference_power_level;
        raw_data.reflector_reference_power_level = procedure_data->reflector_reference_power_level;
        raw_data.packet_nadm_initiator_ = procedure_data->packet_nadm_initiator;
        raw_data.packet_nadm_reflector_ = procedure_data->packet_nadm_reflector;
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->main_mode_type);
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->sub_mode_type);
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->min_main_mode_steps);
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->max_main_mode_steps);
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->main_mode_repetition);
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->mode_0_steps);
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->role);
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->rtt_type);
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->cs_sync_phy);
        for (int i=0;i<10;i++) {
          raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->channel_map[i]);
        }
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->channel_map_repetition);
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->channel_selection_type);
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->ch3c_shape);
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->ch3c_jump);
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->t_ip1_time);
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->t_ip2_time);
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->t_fcs_time);
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->t_pm_time);
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->tone_antenna_config_selection);
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->selected_tx_power);
        raw_data.vendor_specific_cs_single_side_data.push_back((live_tracker->subevent_len >> 16) & 0xFF);
        raw_data.vendor_specific_cs_single_side_data.push_back((live_tracker->subevent_len >> 8) & 0xFF);
        raw_data.vendor_specific_cs_single_side_data.push_back(live_tracker->subevent_len & 0xFF);
        raw_data.vendor_specific_cs_single_side_data.push_back((uint8_t)live_tracker->subevents_per_event);
        raw_data.vendor_specific_cs_single_side_data.push_back((live_tracker->subevent_interval >> 8) & 0xFF);
        raw_data.vendor_specific_cs_single_side_data.push_back(live_tracker->subevent_interval & 0xFF);
        raw_data.vendor_specific_cs_single_side_data.push_back((live_tracker->event_interval >> 8) & 0xFF);
        raw_data.vendor_specific_cs_single_side_data.push_back(live_tracker->event_interval & 0xFF);
        raw_data.vendor_specific_cs_single_side_data.push_back((live_tracker->procedure_interval >> 8) & 0xFF);
        raw_data.vendor_specific_cs_single_side_data.push_back(live_tracker->procedure_interval & 0xFF);
        raw_data.vendor_specific_cs_single_side_data.push_back((live_tracker->n_procedure_count >> 8) & 0xFF);
        raw_data.vendor_specific_cs_single_side_data.push_back(live_tracker->n_procedure_count & 0xFF);
        raw_data.vendor_specific_cs_single_side_data.push_back((live_tracker->max_procedure_len >> 8) & 0xFF);
        raw_data.vendor_specific_cs_single_side_data.push_back(live_tracker->max_procedure_len & 0xFF);
        raw_data.vendor_specific_cs_single_side_data.push_back((live_tracker->conn_interval_ >> 8) & 0xFF);
        raw_data.vendor_specific_cs_single_side_data.push_back(live_tracker->conn_interval_ & 0xFF);
        uint32_t num_AntPIs = procedure_data->antenna_permutation_index_initiator.size();
        raw_data.vendor_specific_cs_single_side_data.push_back((num_AntPIs >> 24) & 0xFF);
        raw_data.vendor_specific_cs_single_side_data.push_back((num_AntPIs >> 16) & 0xFF);
        raw_data.vendor_specific_cs_single_side_data.push_back((num_AntPIs >> 8) & 0xFF);
        raw_data.vendor_specific_cs_single_side_data.push_back(num_AntPIs & 0xFF);
        for (uint32_t i=0;i<num_AntPIs;i++) {
          raw_data.vendor_specific_cs_single_side_data.push_back(procedure_data->antenna_permutation_index_initiator[i]);
        }


        struct timeval tv;
	    gettimeofday(&tv, NULL);
	    curr_proc_complete_timestampMs  = tv.tv_sec*1e6*1ll + tv.tv_usec*1ll;
        raw_data.timestampMs_ = (long)(curr_proc_complete_timestampMs - proc_start_timestampMs);
        log::info(
            "timestampMs_: {} current_proc : {} proc start :{}",
            raw_data.timestampMs_,
            curr_proc_complete_timestampMs,
            proc_start_timestampMs);

        log::verbose("Vendor Specific data : [{}]",
            fmt::join(raw_data.vendor_specific_cs_single_side_data, ", "));
        ranging_hal_->WriteRawData(connection_handle, raw_data);
      }
    }

    // If the procedure is completed or aborted, delete all previous data
    if (procedure_data->local_status != CsProcedureDoneStatus::PARTIAL_RESULTS &&
        procedure_data->remote_status != CsProcedureDoneStatus::PARTIAL_RESULTS) {
      delete_consumed_procedure_data(live_tracker, procedure_data->counter);
    }
  }

  static void delete_consumed_procedure_data(CsTracker* live_tracker, uint16_t current_counter) {
    std::vector<CsProcedureData>& data_list = live_tracker->procedure_data_list;
    while (data_list.begin()->counter < current_counter) {
      log::debug("Delete obsolete procedure data, counter:{}", data_list.begin()->counter);
      data_list.erase(data_list.begin());
    }
  }

  void parse_cs_result_data(const std::vector<LeCsResultDataStructure>& result_data_structures,
                            CsProcedureData& procedure_data, CsRole role) {
    uint8_t num_antenna_paths = procedure_data.num_antenna_paths;
    uint16_t frequency_compensation = procedure_data.ras_subevent_header_.frequency_compensation_;
    auto& ras_data = procedure_data.ras_subevent_data_;
    for (auto& result_data_structure : result_data_structures) {
      uint16_t mode = result_data_structure.step_mode_;
      uint16_t step_channel = result_data_structure.step_channel_;
      uint16_t data_length = result_data_structure.step_data_.size();
      log::verbose("mode: {}, channel: {}, data_length: {}", mode, step_channel,
                   (uint16_t)result_data_structure.step_data_.size());
      ras_data.emplace_back(mode);
      if (data_length == 0) {
        ras_data.back() |= (1 << 7);  // set step aborted
        continue;
      }
      append_vector(ras_data, result_data_structure.step_data_);

      // Parse data into structs from an iterator
      auto bytes = std::make_shared<std::vector<uint8_t>>();
      if (mode == 0x02 || mode == 0x03) {
        // Add one byte for the length of Tone_PCT[k], Tone_Quality_Indicator[k]
        bytes->emplace_back(num_antenna_paths + 1);
      }
      bytes->reserve(bytes->size() + result_data_structure.step_data_.size());
      bytes->insert(bytes->end(), result_data_structure.step_data_.begin(),
                    result_data_structure.step_data_.end());
      Iterator<packet::kLittleEndian> iterator(bytes);
      procedure_data.step_mode.push_back(mode);
      switch (mode) {
        case 0: {
          if (role == CsRole::INITIATOR) {
            LeCsMode0InitatorData tone_data_view;
            auto after = LeCsMode0InitatorData::Parse(&tone_data_view, iterator);
            if (after == iterator) {
              log::warn("Received invalid mode {} data, role:{}", mode, CsRoleText(role));
              print_raw_data(result_data_structure.step_data_);
              continue;
            }
            log::verbose("step_data: {}", tone_data_view.ToString());
            procedure_data.measured_freq_offset.push_back(tone_data_view.measured_freq_offset_);
            procedure_data.packet_quality_initiator.push_back(tone_data_view.packet_quality_);
            procedure_data.rssi_initiator.push_back(tone_data_view.packet_rssi_);
            procedure_data.step_channel.push_back(step_channel);
            procedure_data.frequency_compensation.push_back(frequency_compensation);
          } else {
            LeCsMode0ReflectorData tone_data_view;
            auto after = LeCsMode0ReflectorData::Parse(&tone_data_view, iterator);
            if (after == iterator) {
              log::warn("Received invalid mode {} data, role:{}", mode, CsRoleText(role));
              print_raw_data(result_data_structure.step_data_);
              continue;
            }
            log::verbose("step_data: {}", tone_data_view.ToString());
          }
        } break;
        case 1: {
          if (role == CsRole::INITIATOR) {
            if (procedure_data.contains_sounding_sequence_local_) {
              LeCsMode1InitatorDataWithPacketPct tone_data_view;
              auto after = LeCsMode1InitatorDataWithPacketPct::Parse(&tone_data_view, iterator);
              if (after == iterator) {
                log::warn("Received invalid mode {} data, role:{}", mode, CsRoleText(role));
                print_raw_data(result_data_structure.step_data_);
                continue;
              }
              log::verbose("step_data: {}", tone_data_view.ToString());
              procedure_data.rssi_initiator.emplace_back(tone_data_view.packet_rssi_);
              procedure_data.toa_tod_initiators.emplace_back(tone_data_view.toa_tod_initiator_);
              procedure_data.packet_quality_initiator.emplace_back(tone_data_view.packet_quality_);
              procedure_data.packet_nadm_initiator.push_back((int8_t)tone_data_view.packet_nadm_);
            } else {
              LeCsMode1InitatorData tone_data_view;
              auto after = LeCsMode1InitatorData::Parse(&tone_data_view, iterator);
              if (after == iterator) {
                log::warn("Received invalid mode {} data, role:{}", mode, CsRoleText(role));
                print_raw_data(result_data_structure.step_data_);
                continue;
              }
              log::verbose("step_data: {}", tone_data_view.ToString());
              procedure_data.rssi_initiator.emplace_back(tone_data_view.packet_rssi_);
              procedure_data.toa_tod_initiators.emplace_back(tone_data_view.toa_tod_initiator_);
              procedure_data.packet_quality_initiator.emplace_back(tone_data_view.packet_quality_);
              procedure_data.packet_nadm_initiator.push_back((int8_t)tone_data_view.packet_nadm_);
            }
            procedure_data.step_channel.push_back(step_channel);
          } else {
            if (procedure_data.contains_sounding_sequence_local_) {
              LeCsMode1ReflectorDataWithPacketPct tone_data_view;
              auto after = LeCsMode1ReflectorDataWithPacketPct::Parse(&tone_data_view, iterator);
              if (after == iterator) {
                log::warn("Received invalid mode {} data, role:{}", mode, CsRoleText(role));
                print_raw_data(result_data_structure.step_data_);
                continue;
              }
              log::verbose("step_data: {}", tone_data_view.ToString());
              procedure_data.rssi_reflector.emplace_back(tone_data_view.packet_rssi_);
              procedure_data.tod_toa_reflectors.emplace_back(tone_data_view.tod_toa_reflector_);
              procedure_data.packet_quality_reflector.emplace_back(tone_data_view.packet_quality_);
              procedure_data.packet_nadm_reflector.push_back((int8_t)tone_data_view.packet_nadm_);
            } else {
              LeCsMode1ReflectorData tone_data_view;
              auto after = LeCsMode1ReflectorData::Parse(&tone_data_view, iterator);
              if (after == iterator) {
                log::warn("Received invalid mode {} data, role:{}", mode, CsRoleText(role));
                print_raw_data(result_data_structure.step_data_);
                continue;
              }
              log::verbose("step_data: {}", tone_data_view.ToString());
              procedure_data.rssi_reflector.emplace_back(tone_data_view.packet_rssi_);
              procedure_data.tod_toa_reflectors.emplace_back(tone_data_view.tod_toa_reflector_);
              procedure_data.packet_quality_reflector.emplace_back(tone_data_view.packet_quality_);
              procedure_data.packet_nadm_reflector.push_back((int8_t)tone_data_view.packet_nadm_);
            }
          }
        } break;
        case 2: {
          LeCsMode2Data tone_data_view;
          auto after = LeCsMode2Data::Parse(&tone_data_view, iterator);
          if (after == iterator) {
            log::warn("Received invalid mode {} data, role:{}", mode, CsRoleText(role));
            print_raw_data(result_data_structure.step_data_);
            continue;
          }
          log::verbose("step_data: {}", tone_data_view.ToString());
          if (role == CsRole::INITIATOR) {
            procedure_data.step_channel.push_back(step_channel);
          }
          auto tone_data = tone_data_view.tone_data_;
          uint8_t permutation_index = tone_data_view.antenna_permutation_index_;
          if (role == CsRole::INITIATOR) {
            procedure_data.antenna_permutation_index_initiator.push_back(permutation_index);
          } else {
            procedure_data.antenna_permutation_index_reflector.push_back(permutation_index);
          }
          // Parse in ascending order of antenna position with tone extension data at the end
          uint16_t num_tone_data = num_antenna_paths + 1;
          for (uint16_t k = 0; k < num_tone_data; k++) {
            uint8_t antenna_path =
                    k == num_antenna_paths
                            ? num_antenna_paths
                            : cs_antenna_permutation_array_[permutation_index][k] - 1;
            double i_value = get_iq_value(tone_data[k].i_sample_);
            double q_value = get_iq_value(tone_data[k].q_sample_);
            uint8_t tone_quality_indicator = tone_data[k].tone_quality_indicator_;
            log::verbose("antenna_path {}, {:f}, {:f}", (uint16_t)(antenna_path + 1), i_value,
                         q_value);
            if (role == CsRole::INITIATOR) {
              procedure_data.tone_pct_initiator[antenna_path].emplace_back(i_value, q_value);
              procedure_data.tone_quality_indicator_initiator[antenna_path].emplace_back(
                      tone_quality_indicator);
            } else {
              procedure_data.tone_pct_reflector[antenna_path].emplace_back(i_value, q_value);
              procedure_data.tone_quality_indicator_reflector[antenna_path].emplace_back(
                      tone_quality_indicator);
            }
          }
        } break;
        case 3: {
          uint8_t permutation_index = 0;
          std::vector<LeCsToneDataWithQuality> view_tone_data = {};
          if (role == CsRole::INITIATOR) {
            if (procedure_data.contains_sounding_sequence_local_) {
              LeCsMode3InitatorDataWithPacketPct tone_data_view;
              auto after = LeCsMode3InitatorDataWithPacketPct::Parse(&tone_data_view, iterator);
              if (after == iterator) {
                log::warn("Received invalid mode {} data, role:{}", mode, CsRoleText(role));
                print_raw_data(result_data_structure.step_data_);
                continue;
              }
              log::verbose("step_data: {}", tone_data_view.ToString());
              permutation_index = tone_data_view.antenna_permutation_index_;
              procedure_data.rssi_initiator.emplace_back(tone_data_view.packet_rssi_);
              procedure_data.toa_tod_initiators.emplace_back(tone_data_view.toa_tod_initiator_);
              procedure_data.packet_quality_initiator.emplace_back(tone_data_view.packet_quality_);
              procedure_data.packet_nadm_initiator.push_back((int8_t)tone_data_view.packet_nadm_);
              auto tone_data = tone_data_view.tone_data_;
              view_tone_data.reserve(tone_data.size());
              view_tone_data.insert(view_tone_data.end(), tone_data.begin(), tone_data.end());
            } else {
              LeCsMode3InitatorData tone_data_view;
              auto after = LeCsMode3InitatorData::Parse(&tone_data_view, iterator);
              if (after == iterator) {
                log::warn("Received invalid mode {} data, role:{}", mode, CsRoleText(role));
                print_raw_data(result_data_structure.step_data_);
                continue;
              }
              log::verbose("step_data: {}", tone_data_view.ToString());
              permutation_index = tone_data_view.antenna_permutation_index_;
              procedure_data.rssi_initiator.emplace_back(tone_data_view.packet_rssi_);
              procedure_data.toa_tod_initiators.emplace_back(tone_data_view.toa_tod_initiator_);
              procedure_data.packet_quality_initiator.emplace_back(tone_data_view.packet_quality_);
              procedure_data.packet_nadm_initiator.push_back((int8_t)tone_data_view.packet_nadm_);
              auto tone_data = tone_data_view.tone_data_;
              view_tone_data.reserve(tone_data.size());
              view_tone_data.insert(view_tone_data.end(), tone_data.begin(), tone_data.end());
            }
            procedure_data.step_channel.push_back(step_channel);
          } else {
            if (procedure_data.contains_sounding_sequence_local_) {
              LeCsMode3ReflectorDataWithPacketPct tone_data_view;
              auto after = LeCsMode3ReflectorDataWithPacketPct::Parse(&tone_data_view, iterator);
              if (after == iterator) {
                log::warn("Received invalid mode {} data, role:{}", mode, CsRoleText(role));
                print_raw_data(result_data_structure.step_data_);
                continue;
              }
              log::verbose("step_data: {}", tone_data_view.ToString());
              permutation_index = tone_data_view.antenna_permutation_index_;
              procedure_data.rssi_reflector.emplace_back(tone_data_view.packet_rssi_);
              procedure_data.tod_toa_reflectors.emplace_back(tone_data_view.tod_toa_reflector_);
              procedure_data.packet_quality_reflector.emplace_back(tone_data_view.packet_quality_);
              procedure_data.packet_nadm_reflector.push_back((int8_t)tone_data_view.packet_nadm_);
              auto tone_data = tone_data_view.tone_data_;
              view_tone_data.reserve(tone_data.size());
              view_tone_data.insert(view_tone_data.end(), tone_data.begin(), tone_data.end());
            } else {
              LeCsMode3ReflectorData tone_data_view;
              auto after = LeCsMode3ReflectorData::Parse(&tone_data_view, iterator);
              if (after == iterator) {
                log::warn("Received invalid mode {} data, role:{}", mode, CsRoleText(role));
                print_raw_data(result_data_structure.step_data_);
                continue;
              }
              log::verbose("step_data: {}", tone_data_view.ToString());
              permutation_index = tone_data_view.antenna_permutation_index_;
              procedure_data.antenna_permutation_index_reflector.push_back(permutation_index);
              procedure_data.rssi_reflector.emplace_back(tone_data_view.packet_rssi_);
              procedure_data.tod_toa_reflectors.emplace_back(tone_data_view.tod_toa_reflector_);
              procedure_data.packet_quality_reflector.emplace_back(tone_data_view.packet_quality_);
              procedure_data.packet_nadm_reflector.push_back((int8_t)tone_data_view.packet_nadm_);
              auto tone_data = tone_data_view.tone_data_;
              view_tone_data.reserve(tone_data.size());
              view_tone_data.insert(view_tone_data.end(), tone_data.begin(), tone_data.end());
            }
          }
          // Parse in ascending order of antenna position with tone extension data at the end
          uint16_t num_tone_data = num_antenna_paths + 1;
          for (uint16_t k = 0; k < num_tone_data; k++) {
            uint8_t antenna_path =
                    k == num_antenna_paths
                            ? num_antenna_paths
                            : cs_antenna_permutation_array_[permutation_index][k] - 1;
            double i_value = get_iq_value(view_tone_data[k].i_sample_);
            double q_value = get_iq_value(view_tone_data[k].q_sample_);
            uint8_t tone_quality_indicator = view_tone_data[k].tone_quality_indicator_;
            log::verbose("antenna_path {}, {:f}, {:f}", (uint16_t)(antenna_path + 1), i_value,
                         q_value);
            if (role == CsRole::INITIATOR) {
              procedure_data.tone_pct_initiator[antenna_path].emplace_back(i_value, q_value);
              procedure_data.tone_quality_indicator_initiator[antenna_path].emplace_back(
                      tone_quality_indicator);
            } else {
              procedure_data.tone_pct_reflector[antenna_path].emplace_back(i_value, q_value);
              procedure_data.tone_quality_indicator_reflector[antenna_path].emplace_back(
                      tone_quality_indicator);
            }
          }
        } break;
        default: {
          log::warn("Invalid mode {}", mode);
        }
      }
    }
  }

  double get_iq_value(uint16_t sample) {
    int16_t signed_sample = convert_to_signed(sample, 12);
    double value = 1.0 * signed_sample / 2048;
    return value;
  }

  int16_t convert_to_signed(uint16_t num_unsigned, uint8_t bits) {
    unsigned msb_mask = 1 << (bits - 1);  // setup a mask for most significant bit
    int16_t num_signed = num_unsigned;
    if ((num_signed & msb_mask) != 0) {
      num_signed |= ~(msb_mask - 1);  // extend the MSB
    }
    return num_signed;
  }

  void print_raw_data(std::vector<uint8_t> raw_data) {
    std::string raw_data_str = "";
    auto for_end = raw_data.size() - 1;
    for (size_t i = 0; i < for_end; i++) {
      char buff[10];
      snprintf(buff, sizeof(buff), "%02x ", (uint8_t)raw_data[i]);
      std::string buffAsStdStr = buff;
      raw_data_str.append(buffAsStdStr);
      if (i % 100 == 0 && i != 0) {
        log::verbose("{}", raw_data_str);
        raw_data_str = "";
      }
    }
    char buff[10];
    snprintf(buff, sizeof(buff), "%02x", (uint8_t)raw_data[for_end]);
    std::string buffAsStdStr = buff;
    raw_data_str.append(buffAsStdStr);
    log::verbose("{}", raw_data_str);
  }

  void on_read_remote_transmit_power_level_status(Address address, CommandStatusView view) {
    auto status_view = LeReadRemoteTransmitPowerLevelStatusView::Create(view);
    if (!status_view.IsValid()) {
      log::warn("Invalid LeReadRemoteTransmitPowerLevelStatus event");
      distance_measurement_callbacks_->OnDistanceMeasurementStopped(address, REASON_INTERNAL_ERROR,
                                                                    METHOD_RSSI);
      rssi_trackers.erase(address);
    } else if (status_view.GetStatus() != ErrorCode::SUCCESS) {
      std::string error_code = ErrorCodeText(status_view.GetStatus());
      log::warn("Received LeReadRemoteTransmitPowerLevelStatus with error code {}", error_code);
      distance_measurement_callbacks_->OnDistanceMeasurementStopped(address, REASON_INTERNAL_ERROR,
                                                                    METHOD_RSSI);
      rssi_trackers.erase(address);
    }
  }

  void on_transmit_power_reporting(LeMetaEventView event) {
    auto event_view = LeTransmitPowerReportingView::Create(event);
    if (!event_view.IsValid()) {
      log::warn("Dropping invalid LeTransmitPowerReporting event");
      return;
    }

    if (event_view.GetReason() == ReportingReason::LOCAL_TRANSMIT_POWER_CHANGED) {
      log::warn("Dropping local LeTransmitPowerReporting event");
      return;
    }

    Address address = Address::kEmpty;
    for (auto& rssi_tracker : rssi_trackers) {
      if (rssi_tracker.second.handle == event_view.GetConnectionHandle()) {
        address = rssi_tracker.first;
      }
    }

    if (address.IsEmpty()) {
      log::warn("Can't find rssi tracker for connection {}", event_view.GetConnectionHandle());
      return;
    }

    auto status = event_view.GetStatus();
    if (status != ErrorCode::SUCCESS) {
      log::warn("Received LeTransmitPowerReporting with error code {}", ErrorCodeText(status));
    } else {
      rssi_trackers[address].remote_tx_power = event_view.GetTransmitPowerLevel();
    }

    if (event_view.GetReason() == ReportingReason::READ_COMMAND_COMPLETE &&
        !rssi_trackers[address].started) {
      if (status == ErrorCode::SUCCESS) {
        hci_layer_->EnqueueCommand(
                LeSetTransmitPowerReportingEnableBuilder::Create(event_view.GetConnectionHandle(),
                                                                 0x00, 0x01),
                handler_->BindOnceOn(this, &impl::on_set_transmit_power_reporting_enable_complete,
                                     address, event_view.GetConnectionHandle()));
      } else {
        log::warn("Read remote transmit power level fail");
        distance_measurement_callbacks_->OnDistanceMeasurementStopped(
                address, REASON_INTERNAL_ERROR, METHOD_RSSI);
        rssi_trackers.erase(address);
      }
    }
  }

  void on_set_transmit_power_reporting_enable_complete(Address address, uint16_t connection_handle,
                                                       CommandCompleteView view) {
    auto complete_view = LeSetTransmitPowerReportingEnableCompleteView::Create(view);
    if (!complete_view.IsValid()) {
      log::warn("Invalid LeSetTransmitPowerReportingEnableComplete event");
      distance_measurement_callbacks_->OnDistanceMeasurementStopped(address, REASON_INTERNAL_ERROR,
                                                                    METHOD_RSSI);
      rssi_trackers.erase(address);
      return;
    } else if (complete_view.GetStatus() != ErrorCode::SUCCESS) {
      std::string error_code = ErrorCodeText(complete_view.GetStatus());
      log::warn("Received LeSetTransmitPowerReportingEnableComplete with error code {}",
                error_code);
      distance_measurement_callbacks_->OnDistanceMeasurementStopped(address, REASON_INTERNAL_ERROR,
                                                                    METHOD_RSSI);
      rssi_trackers.erase(address);
      return;
    }

    if (rssi_trackers.find(address) == rssi_trackers.end()) {
      log::warn("Can't find rssi tracker for {}", address);
      distance_measurement_callbacks_->OnDistanceMeasurementStopped(address, REASON_INTERNAL_ERROR,
                                                                    METHOD_RSSI);
      rssi_trackers.erase(address);
    } else {
      log::info("Track rssi for address {}", address);
      rssi_trackers[address].started = true;
      distance_measurement_callbacks_->OnDistanceMeasurementStarted(address, METHOD_RSSI);
      rssi_trackers[address].repeating_alarm->Schedule(
              common::Bind(&impl::send_read_rssi, common::Unretained(this), address,
                           connection_handle),
              std::chrono::milliseconds(rssi_trackers[address].interval_ms));
    }
  }

  void on_read_rssi_complete(Address address, CommandCompleteView view) {
    auto complete_view = ReadRssiCompleteView::Create(view);
    if (!complete_view.IsValid()) {
      log::warn("Dropping invalid read RSSI complete event");
      return;
    }
    if (rssi_trackers.find(address) == rssi_trackers.end()) {
      log::warn("Can't find rssi tracker for {}", address);
      return;
    }
    double remote_tx_power = (int8_t)rssi_trackers[address].remote_tx_power;
    int8_t rssi = complete_view.GetRssi();
    double pow_value = (remote_tx_power - rssi - kRSSIDropOffAt1M) / 20.0;
    double distance = pow(10.0, pow_value);

    using namespace std::chrono;
    long elapsedRealtimeNanos =
            duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    distance_measurement_callbacks_->OnDistanceMeasurementResult(
            address, distance * 100, distance * 100, -1, -1, -1, -1, elapsedRealtimeNanos, -1,
            DistanceMeasurementMethod::METHOD_RSSI);
  }

  std::vector<uint8_t> builder_to_bytes(std::unique_ptr<PacketBuilder<true>> builder) {
    std::shared_ptr<std::vector<uint8_t>> bytes = std::make_shared<std::vector<uint8_t>>();
    BitInserter bi(*bytes);
    builder->Serialize(bi);
    return *bytes;
  }

  void append_vector(std::vector<uint8_t>& v1, const std::vector<uint8_t>& v2) {
    v1.reserve(v2.size());
    v1.insert(v1.end(), v2.begin(), v2.end());
  }

  struct CsSelectedParams {
    Address address;
    std::vector<tCS_PROCEDURE_PARAM> cs_proc_settings;
    std::vector<tCS_CONFIG> cs_conf_settings;
  };

  os::Handler* handler_;
  hal::RangingHal* ranging_hal_;
  hci::Controller* controller_;
  hci::HciLayer* hci_layer_;
  hci::AclManager* acl_manager_;
  hci::DistanceMeasurementInterface* distance_measurement_interface_;
  std::unordered_map<Address, RSSITracker> rssi_trackers;
  std::unordered_map<uint16_t, CsTracker> cs_requester_trackers_;
  std::unordered_map<uint16_t, CsTracker> cs_responder_trackers_;
  std::unordered_map<uint16_t, CsSelectedParams> set_cs_params_;
  DistanceMeasurementCallbacks* distance_measurement_callbacks_;
  CsOptionalSubfeaturesSupported cs_subfeature_supported_;
  uint8_t num_antennas_supported_ = 0x01;
  uint8_t local_num_config_supported_ = 0x01;
  bool local_support_phase_based_ranging_ = false;
  // A table that maps num_antennas_supported and remote_num_antennas_supported to Antenna
  // Configuration Index.
  uint8_t cs_tone_antenna_config_mapping_table_[4][4] = {
          {0, 4, 5, 6}, {1, 7, 7, 7}, {2, 7, 7, 7}, {3, 7, 7, 7}};
  // A table that maps Antenna Configuration Index to Preferred Peer Antenna.
  uint8_t cs_preferred_peer_antenna_mapping_table_[8] = {1, 1, 1, 1, 3, 7, 15, 3};
  // Antenna path permutations. See Channel Sounding CR_PR for the details.
  uint8_t cs_antenna_permutation_array_[24][4] = {
          {1, 2, 3, 4}, {2, 1, 3, 4}, {1, 3, 2, 4}, {3, 1, 2, 4}, {3, 2, 1, 4}, {2, 3, 1, 4},
          {1, 2, 4, 3}, {2, 1, 4, 3}, {1, 4, 2, 3}, {4, 1, 2, 3}, {4, 2, 1, 3}, {2, 4, 1, 3},
          {1, 4, 3, 2}, {4, 1, 3, 2}, {1, 3, 4, 2}, {3, 1, 4, 2}, {3, 4, 1, 2}, {4, 3, 1, 2},
          {4, 2, 3, 1}, {2, 4, 3, 1}, {4, 3, 2, 1}, {3, 4, 2, 1}, {3, 2, 4, 1}, {2, 3, 4, 1}};
};

DistanceMeasurementManager::DistanceMeasurementManager() { pimpl_ = std::make_unique<impl>(); }

DistanceMeasurementManager::~DistanceMeasurementManager() = default;

void DistanceMeasurementManager::ListDependencies(ModuleList* list) const {
  list->add<hal::RangingHal>();
  list->add<hci::Controller>();
  list->add<hci::HciLayer>();
  list->add<hci::AclManager>();
}

void DistanceMeasurementManager::Start() {
  pimpl_->start(GetHandler(), GetDependency<hci::Controller>(), GetDependency<hal::RangingHal>(),
                GetDependency<hci::HciLayer>(), GetDependency<AclManager>());
}

void DistanceMeasurementManager::Stop() { pimpl_->stop(); }

std::string DistanceMeasurementManager::ToString() const { return "Distance Measurement Manager"; }

void DistanceMeasurementManager::RegisterDistanceMeasurementCallbacks(
        DistanceMeasurementCallbacks* callbacks) {
  CallOn(pimpl_.get(), &impl::register_distance_measurement_callbacks, callbacks);
}

void DistanceMeasurementManager::StartDistanceMeasurement(const Address& address,
                                                          uint16_t connection_handle,
                                                          hci::Role local_hci_role,
                                                          uint16_t interval,
                                                          DistanceMeasurementMethod method) {
  CallOn(pimpl_.get(), &impl::start_distance_measurement, address, connection_handle,
         local_hci_role, interval, method);
}

void DistanceMeasurementManager::SetCsParams(const Address& address, uint16_t connection_handle,
		int mSightType, int mLocationType, int mCsSecurityLevel, int mFrequency, int mDuration) {
	log::info("address {} mSightType {}, mLocationType {} mCsSecurityLevel {} mFrequency {} mDuration {}",
		  address, mSightType, mLocationType,
		  mCsSecurityLevel, mFrequency, mDuration);
	CallOn(pimpl_.get(), &impl::set_cs_params, address, connection_handle, mSightType,
               mLocationType, mCsSecurityLevel, mFrequency, mDuration);
}

void DistanceMeasurementManager::StopDistanceMeasurement(const Address& address,
                                                         uint16_t connection_handle,
                                                         DistanceMeasurementMethod method) {
  CallOn(pimpl_.get(), &impl::stop_distance_measurement, address, connection_handle, method);
}

void DistanceMeasurementManager::HandleRasClientConnectedEvent(
        const Address& address, uint16_t connection_handle, uint16_t att_handle,
        const std::vector<hal::VendorSpecificCharacteristic>& vendor_specific_data,
        uint16_t conn_interval) {
  CallOn(pimpl_.get(), &impl::handle_ras_client_connected_event, address, connection_handle,
         att_handle, vendor_specific_data, conn_interval);
}

void DistanceMeasurementManager::HandleConnIntervalUpdated(const Address& address,
                                                           uint16_t connection_handle,
                                                           uint16_t conn_interval) {
  CallOn(pimpl_.get(), &impl::handle_conn_interval_updated, address, connection_handle,
         conn_interval);
}

void DistanceMeasurementManager::HandleRasClientDisconnectedEvent(const Address& address) {
  CallOn(pimpl_.get(), &impl::handle_ras_client_disconnected_event, address);
}

void DistanceMeasurementManager::HandleVendorSpecificReply(
        const Address& address, uint16_t connection_handle,
        const std::vector<hal::VendorSpecificCharacteristic>& vendor_specific_reply) {
  CallOn(pimpl_.get(), &impl::handle_ras_server_vendor_specific_reply, address, connection_handle,
         vendor_specific_reply);
}

void DistanceMeasurementManager::HandleRasServerConnected(const Address& identity_address,
                                                          uint16_t connection_handle,
                                                          hci::Role local_hci_role) {
  CallOn(pimpl_.get(), &impl::handle_ras_server_connected, identity_address, connection_handle,
         local_hci_role);
}

void DistanceMeasurementManager::HandleRasServerDisconnected(
        const bluetooth::hci::Address& identity_address, uint16_t connection_handle) {
  CallOn(pimpl_.get(), &impl::handle_ras_server_disconnected, identity_address, connection_handle);
}

void DistanceMeasurementManager::HandleVendorSpecificReplyComplete(const Address& address,
                                                                   uint16_t connection_handle,
                                                                   bool success) {
  CallOn(pimpl_.get(), &impl::handle_vendor_specific_reply_complete, address, connection_handle,
         success);
}

void DistanceMeasurementManager::HandleRemoteData(const Address& address,
                                                  uint16_t connection_handle,
                                                  const std::vector<uint8_t>& raw_data) {
  CallOn(pimpl_.get(), &impl::handle_remote_data, address, connection_handle, raw_data);
}

void DistanceMeasurementManager::HandleRemoteDataTimeout(const Address& address,
                                                         uint16_t connection_handle) {
  CallOn(pimpl_.get(), &impl::handle_remote_data_timeout, address, connection_handle);
}

}  // namespace hci
}  // namespace bluetooth
