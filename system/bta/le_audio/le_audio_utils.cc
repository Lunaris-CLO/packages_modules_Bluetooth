/*
 * Copyright 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "le_audio_utils.h"

#include <bluetooth/log.h>

#include "common/init_flags.h"
#include "common/strings.h"
#include "le_audio_types.h"
#include "os/log.h"
#include "codec_manager.h"

using bluetooth::common::ToString;
using bluetooth::le_audio::types::AudioContexts;
using bluetooth::le_audio::types::LeAudioContextType;

namespace fmt {
template <>
struct formatter<audio_usage_t> : enum_formatter<audio_usage_t> {};
template <>
struct formatter<audio_content_type_t> : enum_formatter<audio_content_type_t> {
};
template <>
struct formatter<audio_source_t> : enum_formatter<audio_source_t> {};
template <>
struct formatter<audio_devices_t> : enum_formatter<audio_devices_t> {};
}  // namespace fmt

namespace bluetooth::le_audio {
namespace utils {

/* The returned LeAudioContextType should have its entry in the
 * AudioSetConfigurationProvider's ContextTypeToScenario mapping table.
 * Otherwise the AudioSetConfigurationProvider will fall back
 * to default scenario.
 */
LeAudioContextType AudioContentToLeAudioContext(
    audio_content_type_t content_type, audio_usage_t usage) {
  /* Check audio attribute usage of stream */
  switch (usage) {
    case AUDIO_USAGE_MEDIA:
      return LeAudioContextType::MEDIA;
    case AUDIO_USAGE_ASSISTANCE_ACCESSIBILITY:
    case AUDIO_USAGE_ASSISTANT:
      return LeAudioContextType::VOICEASSISTANTS;
    case AUDIO_USAGE_VOICE_COMMUNICATION:
    case AUDIO_USAGE_CALL_ASSISTANT:
      return LeAudioContextType::CONVERSATIONAL;
    case AUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING:
      if (content_type == AUDIO_CONTENT_TYPE_SPEECH) {
        return LeAudioContextType::CONVERSATIONAL;
      }

      if (content_type == AUDIO_CONTENT_TYPE_SONIFICATION) {
        return LeAudioContextType::RINGTONE;
      }

      return LeAudioContextType::MEDIA;
    case AUDIO_USAGE_GAME:
      return LeAudioContextType::GAME;
    case AUDIO_USAGE_NOTIFICATION:
      return LeAudioContextType::NOTIFICATIONS;
    case AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE:
      return LeAudioContextType::RINGTONE;
    case AUDIO_USAGE_ALARM:
      return LeAudioContextType::ALERTS;
    case AUDIO_USAGE_EMERGENCY:
      return LeAudioContextType::EMERGENCYALARM;
    case AUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE:
      return LeAudioContextType::INSTRUCTIONAL;
    case AUDIO_USAGE_ASSISTANCE_SONIFICATION:
      return LeAudioContextType::SOUNDEFFECTS;
    default:
      break;
  }

  return LeAudioContextType::MEDIA;
}

static std::string usageToString(audio_usage_t usage) {
  switch (usage) {
    case AUDIO_USAGE_UNKNOWN:
      return "USAGE_UNKNOWN";
    case AUDIO_USAGE_MEDIA:
      return "USAGE_MEDIA";
    case AUDIO_USAGE_VOICE_COMMUNICATION:
      return "USAGE_VOICE_COMMUNICATION";
    case AUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING:
      return "USAGE_VOICE_COMMUNICATION_SIGNALLING";
    case AUDIO_USAGE_ALARM:
      return "USAGE_ALARM";
    case AUDIO_USAGE_NOTIFICATION:
      return "USAGE_NOTIFICATION";
    case AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE:
      return "USAGE_NOTIFICATION_TELEPHONY_RINGTONE";
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_REQUEST:
      return "USAGE_NOTIFICATION_COMMUNICATION_REQUEST";
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_INSTANT:
      return "USAGE_NOTIFICATION_COMMUNICATION_INSTANT";
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_DELAYED:
      return "USAGE_NOTIFICATION_COMMUNICATION_DELAYED";
    case AUDIO_USAGE_NOTIFICATION_EVENT:
      return "USAGE_NOTIFICATION_EVENT";
    case AUDIO_USAGE_ASSISTANCE_ACCESSIBILITY:
      return "USAGE_ASSISTANCE_ACCESSIBILITY";
    case AUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE:
      return "USAGE_ASSISTANCE_NAVIGATION_GUIDANCE";
    case AUDIO_USAGE_ASSISTANCE_SONIFICATION:
      return "USAGE_ASSISTANCE_SONIFICATION";
    case AUDIO_USAGE_GAME:
      return "USAGE_GAME";
    case AUDIO_USAGE_ASSISTANT:
      return "USAGE_ASSISTANT";
    case AUDIO_USAGE_CALL_ASSISTANT:
      return "USAGE_CALL_ASSISTANT";
    case AUDIO_USAGE_EMERGENCY:
      return "USAGE_EMERGENCY";
    case AUDIO_USAGE_SAFETY:
      return "USAGE_SAFETY";
    case AUDIO_USAGE_VEHICLE_STATUS:
      return "USAGE_VEHICLE_STATUS";
    case AUDIO_USAGE_ANNOUNCEMENT:
      return "USAGE_ANNOUNCEMENT";
    default:
      return "unknown usage ";
  }
}

static std::string contentTypeToString(audio_content_type_t content_type) {
  switch (content_type) {
    case AUDIO_CONTENT_TYPE_UNKNOWN:
      return "CONTENT_TYPE_UNKNOWN";
    case AUDIO_CONTENT_TYPE_SPEECH:
      return "CONTENT_TYPE_SPEECH";
    case AUDIO_CONTENT_TYPE_MUSIC:
      return "CONTENT_TYPE_MUSIC";
    case AUDIO_CONTENT_TYPE_MOVIE:
      return "CONTENT_TYPE_MOVIE";
    case AUDIO_CONTENT_TYPE_SONIFICATION:
      return "CONTENT_TYPE_SONIFICATION";
    default:
      return "unknown content type ";
  }
}

static const char* audioSourceToStr(audio_source_t source) {
  const char* strArr[] = {
      "AUDIO_SOURCE_DEFAULT",           "AUDIO_SOURCE_MIC",
      "AUDIO_SOURCE_VOICE_UPLINK",      "AUDIO_SOURCE_VOICE_DOWNLINK",
      "AUDIO_SOURCE_VOICE_CALL",        "AUDIO_SOURCE_CAMCORDER",
      "AUDIO_SOURCE_VOICE_RECOGNITION", "AUDIO_SOURCE_VOICE_COMMUNICATION",
      "AUDIO_SOURCE_REMOTE_SUBMIX",     "AUDIO_SOURCE_UNPROCESSED",
      "AUDIO_SOURCE_VOICE_PERFORMANCE"};

  if (static_cast<uint32_t>(source) < (sizeof(strArr) / sizeof(strArr[0])))
    return strArr[source];
  return "UNKNOWN";
}

static bool isMetadataTagPresent(const char* tags, const char* tag) {
  std::istringstream iss(tags);
  std::string t;
  while (std::getline(iss, t, AUDIO_ATTRIBUTES_TAGS_SEPARATOR)) {
    log::verbose("Tag {}", t);
    if (t.compare(tag) == 0) {
      return true;
    }
  }
  return false;
}

AudioContexts GetAudioContextsFromSourceMetadata(
    const std::vector<struct playback_track_metadata_v7>& source_metadata) {
  AudioContexts track_contexts;
  for (const auto& entry : source_metadata) {
    auto track = entry.base;
    if (track.content_type == 0 && track.usage == 0) continue;

    log::info("usage={}({}), content_type={}({}), gain={:f}, tag:{}",
              usageToString(track.usage), track.usage,
              contentTypeToString(track.content_type),
              track.content_type, track.gain, entry.tags);

    if (isMetadataTagPresent(entry.tags, "VX_AOSP_SAMPLESOUND")) {
      track_contexts.set(LeAudioContextType::SOUNDEFFECTS);
    } else {
      track_contexts.set(
          AudioContentToLeAudioContext(track.content_type, track.usage));
    }
  }
  return track_contexts;
}

AudioContexts GetAudioContextsFromSinkMetadata(
    const std::vector<struct record_track_metadata_v7>& sink_metadata) {
  AudioContexts all_track_contexts;

  for (const auto& entry : sink_metadata) {
    auto track = entry.base;
    if (track.source == AUDIO_SOURCE_INVALID) continue;
    LeAudioContextType track_context;

    log::debug(
        "source={}(0x{:02x}), gain={:f}, destination device=0x{:08x}, "
        "destination device address={:32s}",
        audioSourceToStr(track.source), track.source, track.gain,
        track.dest_device, track.dest_device_address);

    if (track.source == AUDIO_SOURCE_MIC ||
        track.source == AUDIO_SOURCE_CAMCORDER ) {
      track_context = LeAudioContextType::LIVE;

    } else if (track.source == AUDIO_SOURCE_VOICE_COMMUNICATION ||
               track.source == AUDIO_SOURCE_VOICE_CALL) {
      track_context = LeAudioContextType::CONVERSATIONAL;
    } else {
      /* Fallback to voice assistant
       * This will handle also a case when the device is
       * AUDIO_SOURCE_VOICE_RECOGNITION
       */
      track_context = LeAudioContextType::VOICEASSISTANTS;
      log::warn(
          "Could not match the recording track type to group available "
          "context. Using context {}.",
          ToString(track_context));
    }

    all_track_contexts.set(track_context);
  }

  if (all_track_contexts.none()) {
    all_track_contexts = AudioContexts(
        static_cast<std::underlying_type<LeAudioContextType>::type>(
            LeAudioContextType::UNSPECIFIED));
    log::debug(
        "Unable to find supported audio source context for the remote audio "
        "sink device. This may result in voice back channel malfunction.");
  }

  log::info("Allowed contexts from sink metadata: {} (0x{:08x})",
            bluetooth::common::ToString(all_track_contexts),
            all_track_contexts.value());
  return all_track_contexts;
}

bluetooth::le_audio::btle_audio_codec_index_t
translateBluetoothCodecFormatToCodecType(uint8_t codec_format, uint16_t codec_id) {
  switch (codec_format) {
    case types::kLeAudioCodingFormatLC3:
      return bluetooth::le_audio::LE_AUDIO_CODEC_INDEX_SOURCE_LC3;
    case types::kLeAudioCodingFormatVendorSpecific:
      if (codec_id == types::kLeAudioCodingFormatAptxLe) {
        return bluetooth::le_audio::LE_AUDIO_CODEC_INDEX_SOURCE_APTX_LE;
      } else if (codec_id == types::kLeAudioCodingFormatAptxLeX) {
        return bluetooth::le_audio::LE_AUDIO_CODEC_INDEX_SOURCE_APTX_LEX;
      }
  }
  return bluetooth::le_audio::LE_AUDIO_CODEC_INDEX_SOURCE_INVALID;
}

bluetooth::le_audio::btle_audio_sample_rate_index_t
translateToBtLeAudioCodecConfigSampleRate(uint32_t sample_rate) {
  log::info("{}", sample_rate);
  switch (sample_rate) {
    case LeAudioCodecConfiguration::kSampleRate8000:
      return LE_AUDIO_SAMPLE_RATE_INDEX_8000HZ;
    case LeAudioCodecConfiguration::kSampleRate16000:
      return LE_AUDIO_SAMPLE_RATE_INDEX_16000HZ;
    case LeAudioCodecConfiguration::kSampleRate24000:
      return LE_AUDIO_SAMPLE_RATE_INDEX_24000HZ;
    case LeAudioCodecConfiguration::kSampleRate32000:
      return LE_AUDIO_SAMPLE_RATE_INDEX_32000HZ;
    case LeAudioCodecConfiguration::kSampleRate44100:
      return LE_AUDIO_SAMPLE_RATE_INDEX_44100HZ;
    case LeAudioCodecConfiguration::kSampleRate48000:
      return LE_AUDIO_SAMPLE_RATE_INDEX_48000HZ;
    case LeAudioCodecConfiguration::kSampleRate96000:
      return LE_AUDIO_SAMPLE_RATE_INDEX_96000HZ;
  }

  return LE_AUDIO_SAMPLE_RATE_INDEX_NONE;
}

bluetooth::le_audio::btle_audio_bits_per_sample_index_t
translateToBtLeAudioCodecConfigBitPerSample(uint8_t bits_per_sample) {
  switch (bits_per_sample) {
    case 16:
      return bluetooth::le_audio::LE_AUDIO_BITS_PER_SAMPLE_INDEX_16;
    case 24:
      return bluetooth::le_audio::LE_AUDIO_BITS_PER_SAMPLE_INDEX_24;
    case 32:
      return bluetooth::le_audio::LE_AUDIO_BITS_PER_SAMPLE_INDEX_32;
  }
  return bluetooth::le_audio::LE_AUDIO_BITS_PER_SAMPLE_INDEX_NONE;
}

bluetooth::le_audio::btle_audio_channel_count_index_t
translateToBtLeAudioCodecConfigChannelCount(uint8_t channel_count) {
  switch (channel_count) {
    case 1:
      return bluetooth::le_audio::LE_AUDIO_CHANNEL_COUNT_INDEX_1;
    case 2:
      return bluetooth::le_audio::LE_AUDIO_CHANNEL_COUNT_INDEX_2;
  }
  return bluetooth::le_audio::LE_AUDIO_CHANNEL_COUNT_INDEX_NONE;
}

bluetooth::le_audio::btle_audio_frame_duration_index_t
translateToBtLeAudioCodecConfigFrameDuration(int frame_duration) {
  switch (frame_duration) {
    case 7500:
      return bluetooth::le_audio::LE_AUDIO_FRAME_DURATION_INDEX_7500US;
    case 10000:
      return bluetooth::le_audio::LE_AUDIO_FRAME_DURATION_INDEX_10000US;
  }
  return bluetooth::le_audio::LE_AUDIO_FRAME_DURATION_INDEX_NONE;
}

void fillStreamParamsToBtLeAudioCodecConfig(
    const std::vector<struct set_configurations::AseConfiguration>& confs,
    bluetooth::le_audio::btle_audio_codec_config_t& out_config) {
  if (confs.size() == 0) {
    log::warn("Stream params are null");
    return;
  }

  auto config = confs.at(0).codec;

  out_config.codec_type = translateBluetoothCodecFormatToCodecType(
      config.id.coding_format, config.id.vendor_codec_id);
  out_config.sample_rate = translateToBtLeAudioCodecConfigSampleRate(
      config.GetSamplingFrequencyHz());
  out_config.bits_per_sample = translateToBtLeAudioCodecConfigBitPerSample(16);
  out_config.frame_duration =
      translateToBtLeAudioCodecConfigFrameDuration(config.GetDataIntervalUs());
  out_config.octets_per_frame = config.GetOctectsPerFrame();
  out_config.channel_count = translateToBtLeAudioCodecConfigChannelCount(
      config.GetChannelCountPerIsoStream());
}

static bool is_known_codec(const types::LeAudioCodecId& codec_id) {
  switch (codec_id.coding_format) {
    case types::kLeAudioCodingFormatLC3:
      return true;
    case types::kLeAudioCodingFormatVendorSpecific:
      if (codec_id.vendor_codec_id == types::kLeAudioCodingFormatAptxLe) {
        return true;
      } else if (codec_id.vendor_codec_id == types::kLeAudioCodingFormatAptxLeX) {
        return true;
      }
  }
  return false;
}

static void fillRemotePacsCapabitiliesToBtLeAudioCodecConfig(
    const struct types::acs_ac_record& record,
    std::vector<bluetooth::le_audio::btle_audio_codec_config_t>& vec) {
  if (!utils::IsCodecUsingLtvFormat(record.codec_id)) {
    log::warn(
        "Unknown codec capability format. Unable to report known codec "
        "parameters.");
    return;
  }
  log::assert_that(!record.codec_spec_caps.IsEmpty(),
                   "Codec specific capabilities are not parsed approprietly.");

  const struct types::LeAudioCoreCodecCapabilities capa =
      record.codec_spec_caps.GetAsCoreCodecCapabilities();
  for (uint8_t freq_bit = codec_spec_conf::kLeAudioSamplingFreq8000Hz;
       freq_bit <= codec_spec_conf::kLeAudioSamplingFreq384000Hz; freq_bit++) {
    if (!capa.IsSamplingFrequencyConfigSupported(freq_bit)) continue;
    for (uint8_t fd_bit = codec_spec_conf::kLeAudioCodecFrameDur7500us;
         fd_bit <= codec_spec_conf::kLeAudioCodecFrameDur10000us; fd_bit++) {
      if (!capa.IsFrameDurationConfigSupported(fd_bit)) {
        if ((record.codec_id.vendor_codec_id != types::kLeAudioCodingFormatAptxLe) &&
            (record.codec_id.vendor_codec_id != types::kLeAudioCodingFormatAptxLeX)) continue;
      }
      if (!capa.HasSupportedAudioChannelCounts()) {
        bluetooth::le_audio::btle_audio_codec_config_t config = {
            .codec_type= utils::translateBluetoothCodecFormatToCodecType(
                record.codec_id.coding_format, record.codec_id.vendor_codec_id),
            .sample_rate = utils::translateToBtLeAudioCodecConfigSampleRate(
                types::LeAudioCoreCodecConfig::GetSamplingFrequencyHz(
                    freq_bit)),
            .bits_per_sample =
                utils::translateToBtLeAudioCodecConfigBitPerSample(16),
            .channel_count =
                utils::translateToBtLeAudioCodecConfigChannelCount(1),
            .frame_duration =
                utils::translateToBtLeAudioCodecConfigFrameDuration(
                    types::LeAudioCoreCodecConfig::GetFrameDurationUs(fd_bit)),
        };
        vec.push_back(config);
      } else {
        for (int chan_bit = 1; chan_bit <= 2; chan_bit++) {
          if (!capa.IsAudioChannelCountsSupported(chan_bit)) continue;

          bluetooth::le_audio::btle_audio_codec_config_t config = {
              .codec_type= utils::translateBluetoothCodecFormatToCodecType(
                  record.codec_id.coding_format, record.codec_id.vendor_codec_id),
              .sample_rate = utils::translateToBtLeAudioCodecConfigSampleRate(
                  types::LeAudioCoreCodecConfig::GetSamplingFrequencyHz(
                      freq_bit)),
              .bits_per_sample =
                  utils::translateToBtLeAudioCodecConfigBitPerSample(16),
              .channel_count =
                  utils::translateToBtLeAudioCodecConfigChannelCount(chan_bit),
              .frame_duration =
                  utils::translateToBtLeAudioCodecConfigFrameDuration(
                      types::LeAudioCoreCodecConfig::GetFrameDurationUs(
                          fd_bit)),
          };
          vec.push_back(config);
        }
      }
    }
  }
}

std::vector<bluetooth::le_audio::btle_audio_codec_config_t>
GetRemoteBtLeAudioCodecConfigFromPac(
    const types::PublishedAudioCapabilities& group_pacs) {
  std::vector<bluetooth::le_audio::btle_audio_codec_config_t> vec;

  for (auto& [handles, pacs_record] : group_pacs) {
    for (auto& pac : pacs_record) {
      if (!is_known_codec(pac.codec_id)) continue;

      fillRemotePacsCapabitiliesToBtLeAudioCodecConfig(pac, vec);
    }
  }
  return vec;
}

bool IsCodecUsingLtvFormat(const types::LeAudioCodecId& codec_id) {
  if (codec_id == set_configurations::LeAudioCodecIdLc3 ||
      codec_id == set_configurations::LeAudioCodecIdAptxLe ||
      codec_id == set_configurations::LeAudioCodecIdAptxLeX) {
    return true;
  }
  return false;
}

::bluetooth::le_audio::LeAudioCodecConfiguration
GetAudioSessionCodecConfigFromAudioSetConfiguration(
    const bluetooth::le_audio::set_configurations::AudioSetConfiguration&
        audio_set_conf,
    uint8_t remote_direction) {
  /* Note: For now we expect that each ASE in a particular direction needs
   *       exactly the same audio codec parameters.
   */

  LeAudioCodecConfiguration group_config = {{0, 0, 0}, 0, 0, 0, 0, 0};
  for (const auto& conf : audio_set_conf.confs.get(remote_direction)) {
    if (group_config.sample_rate != 0 &&
        conf.codec.GetSamplingFrequencyHz() != group_config.sample_rate) {
      log::warn(
          "Stream configuration could not be determined (multiple, different "
          "sampling frequencies) for remote_direction: {:#x}",
          remote_direction);
      break;
    }
    group_config.sample_rate = conf.codec.GetSamplingFrequencyHz();

    if (group_config.data_interval_us != 0 &&
        conf.codec.GetDataIntervalUs() != group_config.data_interval_us) {
      log::warn(
          "Stream configuration could not be determined (multiple, different "
          "data intervals) for remote_direction: {:#x}",
          remote_direction);
      break;
    }
    group_config.data_interval_us = conf.codec.GetDataIntervalUs();

    if (group_config.bits_per_sample != 0 &&
        conf.codec.GetBitsPerSample() != group_config.bits_per_sample) {
      log::warn(
          "Stream configuration could not be determined (multiple, different "
          "bits per sample) for remote_direction: {:#x}",
          remote_direction);
      break;
    }
    group_config.bits_per_sample = conf.codec.GetBitsPerSample();
    group_config.num_channels += conf.codec.GetChannelCountPerIsoStream();
  }
  if (group_config.num_channels > 2) group_config.num_channels = 2;

  return group_config;
}

types::LeAudioConfigurationStrategy GetStrategyForAseConfig(
    const std::vector<le_audio::set_configurations::AseConfiguration>& cfgs,
    uint8_t device_cnt) {
  if (cfgs.size() == 0) {
    return types::LeAudioConfigurationStrategy::RFU;
  }

  /* Banded headphones or the Classic TWS style topology (a single device) */
  if (device_cnt == 1) {
    if (cfgs.at(0).codec.GetChannelCountPerIsoStream() == 1) {
      /* One mono ASE - could be a single channel microphone */
      if (cfgs.size() == 1) {
        log::debug("Strategy set to '0' ");
        return types::LeAudioConfigurationStrategy::MONO_ONE_CIS_PER_DEVICE;
      }

      log::debug("Strategy set to '1' ");
      /* Each channel on a dedicated ASE - TWS style split channel re-routing */
      return types::LeAudioConfigurationStrategy::STEREO_TWO_CISES_PER_DEVICE;
    }

    log::debug("Strategy set to '2' ");
    /* Banded headphones with 1 ASE - requires two channels per CIS */
    return types::LeAudioConfigurationStrategy::STEREO_ONE_CIS_PER_DEVICE;
  }

  // We need at least 2 ASEs in the group config to set up more than one device
  if (cfgs.size() == 1) {
    return types::LeAudioConfigurationStrategy::RFU;
  }

  log::debug("Strategy set to one channel per device topology ");
  /* The common one channel per device topology */
  return types::LeAudioConfigurationStrategy::MONO_ONE_CIS_PER_DEVICE;
}

static bool IsCodecConfigSupported(const types::LeAudioCodecId codec,
                                   const types::LeAudioLtvMap& pacs,
                                   const types::LeAudioLtvMap& reqs,
                                   uint8_t channel_cnt_per_ase,
                                   const types::LeAudioLtvMap& pacs_metadata,
                                   std::optional<set_configurations::CodecMetadataSetting> vendor_metadata,
                                   LeAudioContextType context_type) {
  log::info("codec.coding_format: {}, codec.vendor_company_id: {}, codec.vendor_codec_id: {}",
             codec.coding_format, codec.vendor_company_id, codec.vendor_codec_id);
  log::debug(" Requested context: {}", ToHexString(context_type).c_str());

  if (!bluetooth::common::init_flags::leaudio_multicodec_support_is_enabled() &&
      codec.coding_format == types::kLeAudioCodingFormatVendorSpecific) {
    log::debug("Disallow Vendor Codec negotiation for multi-codec feature disabled");
    return false;
  }

  // AptX LEX
  if (codec.coding_format == types::kLeAudioCodingFormatVendorSpecific &&
      codec.vendor_company_id == types::kLeAudioVendorCompanyIdQualcomm &&
      codec.vendor_codec_id == types::kLeAudioCodingFormatAptxLeX) {
    uint8_t u8_req_val, u8_pac_val;
    uint16_t u16_pac_val;
    auto req = reqs.Find(codec_spec_conf::qcom_codec_spec_conf::kLeAudioCodecAptxLeTypeSamplingFreq);
    auto pac = pacs.Find(codec_spec_caps::kLeAudioLtvTypeSupportedSamplingFrequencies);
    if (!req || !pac) {
      log::debug(", lack of sampling frequency fields");
      return false;
    }
    u8_req_val = VEC_UINT8_TO_UINT8(req.value());
    u16_pac_val = VEC_UINT8_TO_UINT16(pac.value());

    if (!(u16_pac_val &
          codec_spec_caps::SamplingFreqConfig2Capability(u8_req_val))) {
      log::debug("Req:SamplFreq= 0x{:04x} (Assigned Numbers: Codec_Specific_Configuration)", u8_req_val);
      log::debug("Pac:SamplFreq= 0x{:04x}  (Assigned numbers: Codec_Specific_Capabilities - bitfield)", u16_pac_val);
      log::debug(", sampling frequency not supported");
      return false;
    }

    pac = pacs.Find(codec_spec_caps::kLeAudioLtvTypeSupportedAudioChannelCounts);

    if (!pac) {
      log::debug(", no Audio_Channel_Counts field in PAC, using default 0x01");
      u8_pac_val = 0x01;
    } else {
      u8_pac_val = VEC_UINT8_TO_UINT8(pac.value());
    }

    if (!((1 << (channel_cnt_per_ase - 1)) & u8_pac_val)) {
      log::debug("Req:AudioChanCnt=0x{:04x}", 1 << (channel_cnt_per_ase - 1));
      log::debug("Pac:AudioChanCnt=0x{:04x}", u8_pac_val);
      log::debug(", channel count warning");
      return false;
    }

    log::debug("APTX_LEX codec config matched");
    return true;
  }

  // Aptx LE
  if (codec.coding_format == types::kLeAudioCodingFormatVendorSpecific &&
      codec.vendor_company_id == types::kLeAudioVendorCompanyIdQualcomm &&
      codec.vendor_codec_id == types::kLeAudioCodingFormatAptxLe &&
      CodecManager::GetInstance()->IsAptxAdaptiveLeSupported()) {
    log::debug("APTX_LE");
    uint8_t u8_req_val, u8_pac_val;
    uint16_t u16_pac_val;
    auto req = reqs.Find(codec_spec_conf::qcom_codec_spec_conf::kLeAudioCodecAptxLeTypeSamplingFreq);
    auto pac = pacs.Find(codec_spec_caps::kLeAudioLtvTypeSupportedSamplingFrequencies);
    if (!req || !pac) {
      log::debug(", lack of sampling frequency fields");
      return false;
    }
    u8_req_val = VEC_UINT8_TO_UINT8(req.value());
    u16_pac_val = VEC_UINT8_TO_UINT16(pac.value());

    if (!(u16_pac_val &
          codec_spec_caps::SamplingFreqConfig2Capability(u8_req_val))) {
      log::debug("Req:SamplFreq= 0x{:04x} (Assigned Numbers: Codec_Specific_Configuration)", u8_req_val);
      log::debug("Pac:SamplFreq= 0x{:04x}  (Assigned numbers: Codec_Specific_Capabilities - bitfield)", u16_pac_val);
      log::debug(", sampling frequency not supported");
      return false;
    }

    pac = pacs.Find(codec_spec_caps::kLeAudioLtvTypeSupportedAudioChannelCounts);

    if (!pac) {
      log::debug(", no Audio_Channel_Counts field in PAC, using default 0x01");
      u8_pac_val = 0x01;
    } else {
      u8_pac_val = VEC_UINT8_TO_UINT8(pac.value());
    }

    if (!((1 << (channel_cnt_per_ase - 1)) & u8_pac_val)) {
      log::debug("Req:AudioChanCnt=0x{:04x}", 1 << (channel_cnt_per_ase - 1));
      log::debug("Pac:AudioChanCnt=0x{:04x}", u8_pac_val);
      log::debug(", channel count warning");
      return false;
    }

    auto pac_preferred_audio_context =
        pacs_metadata.Find(types::kLeAudioMetadataTypePreferredAudioContext);
    u16_pac_val = VEC_UINT8_TO_UINT16(pac_preferred_audio_context.value());
    types::AudioContexts pac_preferred_context = types::AudioContexts(u16_pac_val);

    log::debug("Preferred Audio context: {}", pac_preferred_context.to_string());

    auto pac_vendor_metadata = pacs_metadata.Find(types::kLeAudioMetadataTypeVendorSpecific);
    /* codec metadata */
    if (vendor_metadata.has_value() && pac_vendor_metadata.has_value()) {
      std::vector<uint8_t> pac_vndr_metadata;
      pac_vndr_metadata = pac_vendor_metadata.value_or(std::vector<uint8_t>(0x00));
      std::vector<uint8_t> req_metadata;
      req_metadata.assign(vendor_metadata.value().vs_metadata.begin(),
                          vendor_metadata.value().vs_metadata.end());

      uint16_t pac_company_id = VEC_UINT8_TO_UINT16(pac_vndr_metadata);
      uint16_t req_company_id = vendor_metadata.value().vendor_company_id;
      if (pac_company_id != req_company_id) {
        log::debug("Company ID mismatch, as per pac record {} != requested {}", pac_company_id, req_company_id);
        return false;
      }

      uint8_t pac_metadata_len = pac_vendor_metadata.value()[2];
      uint8_t req_metadata_len = vendor_metadata.value().vs_metadata.size() + 1 /*type*/;
      if (pac_metadata_len != req_metadata_len) {
        log::debug("Vendor Metadata length mismatch, as per pac record {} != requested {}", pac_metadata_len, req_metadata_len);
        return false;
      }

      if (pac_company_id == types::kLeAudioVendorCompanyIdQualcomm &&
          pac_metadata_len != types::qcom_codec_metadata::kLeAudioCodecAptxLeSupportedFeaturesMetadataLen) {
        log::debug("Invalid Metadata length");
        return false;
      }

      uint8_t pac_metadata_type = pac_vendor_metadata.value()[3];
      uint8_t req_metadata_type = vendor_metadata.value().vendor_metadata_type;
      if (pac_metadata_type != req_metadata_type) {
        log::debug("Vendor Metadata type mismatch, as per pac record {} != requested {}", pac_metadata_type, req_metadata_type);
        return false;
      }

      if (pac_company_id == types::kLeAudioVendorCompanyIdQualcomm &&
          pac_metadata_type != types::qcom_codec_metadata::kLeAudioCodecAptxLeSupportedFeaturesMetadataType) {
        log::debug("Invalid Metadata type");
        return false;
      }

      /* codec version */
      uint8_t pac_codec_version = pac_vendor_metadata.value()[5];
      uint8_t req_codec_version = req_metadata[1];
      if (pac_codec_version < req_codec_version) {
        log::debug("Codec version unspported pac_codec_version {} < req_codec_version {}", pac_codec_version, req_codec_version);
        return false;
      }
    }
    log::debug("APTX_LE codec matched");
    return true;
  }

  // Core LC3 codec
  auto caps = pacs.GetAsCoreCodecCapabilities();
  auto config = reqs.GetAsCoreCodecConfig();

  /* Sampling frequency */
  if (!caps.HasSupportedSamplingFrequencies() || !config.sampling_frequency) {
    log::debug("Missing supported sampling frequencies capability");
    return false;
  }
  if (!caps.IsSamplingFrequencyConfigSupported(
          config.sampling_frequency.value())) {
    log::debug("Cfg: SamplingFrequency= {:#x}",
               config.sampling_frequency.value());
    log::debug("Cap: SupportedSamplingFrequencies= {:#x}",
               caps.supported_sampling_frequencies.value());
    log::debug("Sampling frequency not supported");
    return false;
  }

  /* Channel counts */
  if (!caps.IsAudioChannelCountsSupported(channel_cnt_per_ase)) {
    log::debug("Cfg: Allocated channel count= {:#x}", channel_cnt_per_ase);
    log::debug("Cap: Supported channel counts= {:#x}",
               caps.supported_audio_channel_counts.value_or(1));
    log::debug("Channel count not supported");
    return false;
  }

  /* Frame duration */
  if (!caps.HasSupportedFrameDurations() || !config.frame_duration) {
    log::debug("Missing supported frame durations capability");
    return false;
  }
  if (!caps.IsFrameDurationConfigSupported(config.frame_duration.value())) {
    log::debug("Cfg: FrameDuration= {:#x}", config.frame_duration.value());
    log::debug("Cap: SupportedFrameDurations= {:#x}",
               caps.supported_frame_durations.value());
    log::debug("Frame duration not supported");
    return false;
  }

  /* Octets per frame */
  if (!caps.HasSupportedOctetsPerCodecFrame() ||
      !config.octets_per_codec_frame) {
    log::debug("Missing supported octets per codec frame");
    return false;
  }
  if (!caps.IsOctetsPerCodecFrameConfigSupported(
          config.octets_per_codec_frame.value())) {
    log::debug("Cfg: Octets per frame={}",
               config.octets_per_codec_frame.value());
    log::debug("Cap: Min octets per frame={}",
               caps.supported_min_octets_per_codec_frame.value());
    log::debug("Cap: Max octets per frame={}",
               caps.supported_max_octets_per_codec_frame.value());
    log::debug("Octets per codec frame outside the capabilities");
    return false;
  }

  if (!bluetooth::common::init_flags::leaudio_multicodec_support_is_enabled()) {
    if (vendor_metadata.value().vs_metadata.empty()) {
      log::debug(" Multicodec support disabled, LC3 codec config matched");
      return true;
    } else {
      log::debug(" Multicodec support disabled, disallow LC3Q codec config match");
      return false;
    }
  }

  /* below logic is for LC3Q match */
  auto pac_vendor_metadata = pacs_metadata.Find(types::kLeAudioMetadataTypeVendorSpecific);
  std::vector<uint8_t> pac_vndr_metadata;
  pac_vndr_metadata = pac_vendor_metadata.value_or(std::vector<uint8_t>(0x00));
  auto pac_preferred_audio_context =
      pacs_metadata.Find(types::kLeAudioMetadataTypePreferredAudioContext);
  if (pac_vendor_metadata.has_value()) {
    uint16_t u16_pac_val;
    uint16_t pac_company_id = VEC_UINT8_TO_UINT16(pac_vndr_metadata);
    uint8_t pac_metadata_type = pac_vendor_metadata.value()[3];
    if (pac_metadata_type == types::qcom_codec_metadata::kLeAudioCodecLC3QSupportedFeaturesMetadataType
        && pac_company_id == types::kLeAudioVendorCompanyIdQualcomm) {
      log::debug(" Context checks for LC3Q codecs types only");
      u16_pac_val = VEC_UINT8_TO_UINT16(pac_preferred_audio_context.value());
      types::AudioContexts pac_preferred_context = types::AudioContexts(u16_pac_val);
      log::debug(" Preferred Audio context: {}", pac_preferred_context.to_string().c_str());
      log::debug(" Requested context: {}", ToHexString(context_type).c_str());
      if (context_type != LeAudioContextType::UNSPECIFIED &&
          !pac_preferred_context.test(context_type)) {
        log::debug(" Current requested context {} not part of Preferred Audio context: {}",
            ToHexString(context_type).c_str(), pac_preferred_context.to_string().c_str());
        return false;
      }
    }
  }

  /* codec metadata negotiated against PACS Vendor metadata and local Codec metadata cfg in JSON*/
  if (!vendor_metadata.value().vs_metadata.empty() && pac_vendor_metadata.has_value()) {
    std::vector<uint8_t> pac_vndr_metadata;
    pac_vndr_metadata = pac_vendor_metadata.value_or(std::vector<uint8_t>(0x00));
    std::vector<uint8_t> req_metadata;
    req_metadata.assign(vendor_metadata.value().vs_metadata.begin(),
                        vendor_metadata.value().vs_metadata.end());
    uint16_t pac_company_id = VEC_UINT8_TO_UINT16(pac_vndr_metadata);
    uint16_t req_company_id = vendor_metadata.value().vendor_company_id;
    if (pac_company_id != req_company_id) {
      log::debug(" Company ID mismatch, as per pac record {} != requested {} ",
          pac_company_id, req_company_id);
      return false;
    }

    uint8_t pac_metadata_len = pac_vendor_metadata.value()[2];
    uint8_t req_metadata_len = vendor_metadata.value().vs_metadata.size() + 1 /*type*/;
    if (pac_metadata_len != req_metadata_len) {
      log::debug(" Vendor Metadata length mismatch, as per pac record {} != requested {} ",
          pac_metadata_len, req_metadata_len);
      return false;
    }

    if (pac_company_id == types::kLeAudioVendorCompanyIdQualcomm &&
        pac_metadata_len != types::qcom_codec_metadata::kLeAudioCodecLC3QSupportedFeaturesMetadataLen) {
      log::debug(" Invalid Metadata length ");
      return false;
    }

    uint8_t pac_metadata_type = pac_vendor_metadata.value()[3];
    uint8_t req_metadata_type = vendor_metadata.value().vendor_metadata_type;
    if (pac_metadata_type != req_metadata_type) {
      log::debug(" Vendor Metadata type mismatch, as per pac record {} != requested {} ",
          pac_metadata_type, req_metadata_type);
      return false;
    }

    if (pac_company_id == types::kLeAudioVendorCompanyIdQualcomm &&
        pac_metadata_type != types::qcom_codec_metadata::kLeAudioCodecLC3QSupportedFeaturesMetadataType) {
      log::debug(" Invalid Metadata length ");
      return false;
    }

    uint8_t pac_decoder_version = pac_vendor_metadata.value()[5];
    uint8_t req_encoder_version = req_metadata[0];
    if (pac_decoder_version < req_encoder_version) {
      log::debug(" Encoder version cfg couldn't be negotiated as corresponding peer decoder couldn't"
          " be matched as pac_decoder_version {} < req_encoder_version {} ",
          pac_decoder_version, req_encoder_version);
      return false;
    }

    uint8_t pac_encoder_version = pac_vendor_metadata.value()[4];
    uint8_t req_decoder_version = req_metadata[1];
    if (pac_encoder_version < req_decoder_version) {
      log::debug(" Decoder version cfg couldn't be negotiated as corresponding peer encoder couldn't"
          " be matched as pac_encoder_version {} < req_decoder_version {} ",
          pac_encoder_version, req_encoder_version);
      return false;
    }
  }

  if (bluetooth::common::init_flags::leaudio_multicodec_support_is_enabled()) {
    if (!vendor_metadata.value().vs_metadata.empty() && !pac_vendor_metadata.has_value()) {
      log::debug(" Disallow LC3Q match against LC3 codec ");
      return false;
    }
  }

  if (vendor_metadata.value().vs_metadata.empty())
    log::debug("LC3 codec matched");
  else
    log::debug("LC3Q codec matched");

  return true;
}

static bool IsCodecConfigSettingSupported(
    const types::acs_ac_record& pac,
    const set_configurations::CodecConfigSetting& codec_config_setting,
    std::optional<set_configurations::CodecMetadataSetting> vendor_metadata,
    LeAudioContextType context_type) {
  const auto& codec_id = codec_config_setting.id;
  if (codec_id != pac.codec_id) return false;

  log::debug(": Settings for format: 0x{} ", codec_id.coding_format);

  if (utils::IsCodecUsingLtvFormat(codec_id)) {
    log::assert_that(
        !pac.codec_spec_caps.IsEmpty(),
        "Codec specific capabilities are not parsed approprietly.");
    return IsCodecConfigSupported(codec_id,
        pac.codec_spec_caps, codec_config_setting.params,
        codec_config_setting.GetChannelCountPerIsoStream(),
        pac.metadata, vendor_metadata, context_type);
  }

  log::error("Codec {}, seems to be not supported here.",
             bluetooth::common::ToString(codec_id));
  return false;
}

const struct types::acs_ac_record* GetConfigurationSupportedPac(
    const types::PublishedAudioCapabilities& pacs,
    const set_configurations::CodecConfigSetting& codec_config_setting,
    std::optional<set_configurations::CodecMetadataSetting> vendor_metadata,
    LeAudioContextType context_type) {
  for (const auto& pac_tuple : pacs) {
    for (const auto& pac : std::get<1>(pac_tuple)) {
      if (utils::IsCodecConfigSettingSupported(pac, codec_config_setting,
          vendor_metadata, context_type))
        return &pac;
    };
  }
  /* Doesn't match required configuration with any PAC */
  if (pacs.size() == 0) log::error("No PAC records");
  return nullptr;
}
}  // namespace utils
}  // namespace bluetooth::le_audio
