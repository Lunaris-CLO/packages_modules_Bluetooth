/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#define CS_SUBEVENT_LEN_SIZE 3
#define CS_CHANNEL_MAP_SIZE 10

#ifndef CS_CONFIG_PATH
#if defined(TARGET_FLOSS)
#define CS_CONFIG_PATH "/var/lib/bluetooth/cs_configs.xml"
#elif defined(__ANDROID__)
#define CS_CONFIG_PATH \
  "/apex/com.android.btservices/etc/bluetooth/cs_configs.xml"
#else  // !defined(__ANDROID__)
#define CS_CONFIG_PATH "cs_configs.xml"
#endif  // defined(__ANDROID__)
#endif  // CS_CONFIG_PATH

#define CS_CONFIG_PATH_LOCAL "/data/misc/bluetooth/cs_configs.xml"
static const char CS_CONFIG_MODULE[] = "cs_config_module";
typedef struct {
  uint8_t enable;
  uint8_t config_id;
  uint16_t max_proc_duration;
  uint16_t min_period_between_proc;
  uint16_t max_period_between_proc;
  uint16_t max_proc_count;
  uint8_t min_subevent_len[CS_SUBEVENT_LEN_SIZE];
  uint8_t max_subevent_len[CS_SUBEVENT_LEN_SIZE];
  uint8_t tone_ant_cfg_selection;
  uint8_t phy;
  uint8_t tx_pwr_delta;
  uint8_t preferred_peer_antenna;
  uint8_t snr_control_initiator;
  uint8_t snr_control_reflector;
} tCS_PROCEDURE_PARAM;

typedef struct {
  uint8_t config_id;
  uint8_t main_mode_type;
  uint8_t sub_mode_type;
  uint8_t main_mode_min_steps;
  uint8_t main_mode_max_steps;
  uint8_t main_mode_rep;
  uint8_t mode_0_steps;
  uint8_t role;
  uint8_t rtt_types;
  uint8_t cs_sync_phy;
  uint8_t channel_map[CS_CHANNEL_MAP_SIZE];
  uint8_t channel_map_rep;
  uint8_t hop_algo_type;
  uint8_t user_shape;
  uint8_t user_channel_jump;
  uint8_t comp_signal_enable;
} tCS_CONFIG;

bool get_cs_procedure_settings(int, tCS_PROCEDURE_PARAM *);
bool get_cs_config_settings(int, tCS_CONFIG *);
