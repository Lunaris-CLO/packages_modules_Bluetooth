/******************************************************************************
 * Copyright (c) 2019 The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *        * Redistributions of source code must retain the above copyright
 *            notice, this list of conditions and the following disclaimer.
 *        * Redistributions in binary form must reproduce the above
 *            copyright notice, this list of conditions and the following
 *            disclaimer in the documentation and/or other materials provided
 *            with the distribution.
 *        * Neither the name of The Linux Foundation nor the names of its
 *            contributors may be used to endorse or promote products derived
 *            from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ******************************************************************************/

/******************************************************************************
 * Changes from Qualcomm Innovation Center are provided under the following
 * license:
 *
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 ******************************************************************************/

#ifndef __BT_CONFIG_STORE__
#define __BT_CONFIG_STORE__

#include <cutils/properties.h>
#include <vector>

#define BT_CONFIG_STORE_INTERFACE_STRING "btConfigStoreInterface"
#define MAX_SIZE_OF_SOC_ADD_ON_FEATURES 245
#define MAX_SIZE_OF_HOST_ADD_ON_FEATURES 255

enum {
  BT_PROP_ALL = 0x0000,
  BT_PROP_SOC_TYPE,
  BT_PROP_A2DP_OFFLOAD_CAP,
  BT_PROP_SPILT_A2DP,
  BT_PROP_AAC_FRAME_CTL,
  BT_PROP_WIPOWER,
  BT_PROP_SWB_ENABLE,
  BT_PROP_SWBPM_ENABLE,
  BT_PROP_A2DP_MCAST_TEST,
  BT_PROP_TWSP_STATE,
  BT_PROP_STACK_TIMEOUT,
  BT_PROP_MAX_POWER,
};

typedef enum {
  BT_SOC_TYPE_DEFAULT = 0x0000,
  BT_SOC_TYPE_SMD = BT_SOC_TYPE_DEFAULT,
  BT_SOC_TYPE_AR3K,
  BT_SOC_TYPE_ROME,
  BT_SOC_TYPE_CHEROKEE,
  BT_SOC_TYPE_HASTINGS,
  BT_SOC_TYPE_MOSELLE,
  BT_SOC_TYPE_HAMILTON,
  BT_SOC_TYPE_GANGES,
  BT_SOC_TYPE_ORNE,
  BT_SOC_TYPE_EVROS,
  /* Add chipset type here */
  BT_SOC_TYPE_RESERVED
} bt_soc_type_t;

typedef struct {
  uint16_t product_id;
  uint16_t rsp_version;
  uint8_t feat_mask_len;
  uint8_t features[8];
} add_on_features_list_t;

typedef struct {
  uint32_t type;
  char value[PROPERTY_VALUE_MAX];
} vendor_property_t;

typedef struct {
  uint16_t product_id;
  uint16_t rsp_version;
  uint8_t feat_mask_len;
  uint8_t features[MAX_SIZE_OF_SOC_ADD_ON_FEATURES];
} controller_add_on_features_list_t;

typedef struct {
  uint8_t feat_mask_len;
  uint8_t features[MAX_SIZE_OF_HOST_ADD_ON_FEATURES];
} host_add_on_features_list_t;

typedef struct {
  /** set to sizeof(bt_configstore_interface_t) */
  size_t size;

  /**
   *
   *To get vendor property
   */
  bool (*get_vendor_properties)(uint32_t propType,
                                std::vector<vendor_property_t>& vPropList);

  /**
   *
   * To get BT controller add On features
   */
  bool (*get_controller_add_on_features)(
      controller_add_on_features_list_t* features_list);

  /**
   *
   * To get BT Host add On features
   */
  bool (*get_host_add_on_features)(host_add_on_features_list_t* features_list);

  /**
   * To set ANT/BT/FM Vendor Properties
   */
  bool (*set_vendor_property)(uint32_t type, const char* value);

  /**
   * To convert soc name to soc type
   */
  bt_soc_type_t (*convert_bt_soc_name_to_soc_type)(const char* soc_name);

  /**
   * To convert prop type  to string format
   */
  const char* (*convert_prop_type_to_string_format)(uint32_t propType);

} bt_configstore_interface_t;

bt_configstore_interface_t* get_btConfigStore_interface();

#endif
