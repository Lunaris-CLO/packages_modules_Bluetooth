/*
 * Copyright 2019 The Android Open Source Project
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
 * Changes from Qualcomm Innovation Center are provided under the following license:
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.

    * Redistribution and use in source and binary forms, with or without
      modification, are permitted (subject to the limitations in the
      disclaimer below) provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.

    * Neither the name of Qualcomm Innovation Center, Inc. nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE
*/

#pragma once

#include "audio_a2dp_hw/include/audio_a2dp_hw.h"
#include <vendor/qti/hardware/bluetooth_audio/2.0/types.h>
#include <vendor/qti/hardware/bluetooth_audio/2.1/types.h>
#include "osi/include/thread.h"
#include "bta_av_api.h"
#include "internal_include/bt_target.h"

#include "common/message_loop_thread.h"

using vendor::qti::hardware::bluetooth_audio::V2_0::SessionType;
using vendor::qti::hardware::bluetooth_audio::V2_0::SessionParamType;

namespace bluetooth {
namespace audio {
namespace qti_hidl {
namespace a2dp {

bool is_hal_2_0_supported();

// Check if new bluetooth_audio is enabled
bool is_hal_2_0_enabled();

bool is_qc_hal_enabled();

bool is_hal_2_0_offloading();

bool is_hal_2_0_offloading_session_unknown();

enum class BluetoothAudioHalVersion : uint8_t {
  VERSION_2_0 = 0,
  VERSION_2_1,
  VERSION_UNAVAILABLE,
};

// Initialize BluetoothAudio HAL: openProvider
bool init( bluetooth::common::MessageLoopThread* message_loop);
// Set up the codec into BluetoothAudio HAL
bool setup_codec();

// Clean up BluetoothAudio HAL
void cleanup();

// Send command to the BluetoothAudio HAL: StartSession, EndSession,
// StreamStarted, StreamSuspended
void start_session();
void end_session();
void ack_stream_started(const tA2DP_CTRL_ACK& status);
void ack_stream_suspended(const tA2DP_CTRL_ACK& status);

// Read from the FMQ of BluetoothAudio HAL
size_t read(uint8_t* p_buf, uint32_t len);

// Update A2DP delay report to BluetoothAudio HAL
void set_remote_delay(uint16_t delay_report);
void update_session_params(SessionParamType param_type);

}  // namespace a2dp
}  // namespace qti_hidl
}  // namespace audio
}  // namespace bluetooth
