/*
 * Copyright (C) 2016 The Linux Foundation. All rights reserved
 * Not a Contribution.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted (subject to the limitations in the
 * disclaimer below) provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *
 * * Neither the name of The Linux Foundation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
 * GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
 * HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 *  Copyright (C) 2009-2012 Broadcom Corporation
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
 * Changes from Qualcomm Innovation Center are provided under the following
 * license:
 *
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 ******************************************************************************/

/************************************************************************************
 *
 *  Filename:      btif_vendor.cc
 *
 *  Description:   Vendor Bluetooth Interface
 *
 *
 ***********************************************************************************/

#if TEST_APP_INTERFACE == TRUE
#include <hardware/bt_vendor.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#undef LOG_TAG
#define LOG_TAG "bt_btif_vendor"

#include <base/bind.h>
#include <base/callback.h>
#include <base/location.h>
#include <bluetooth/log.h>
#include <cutils/properties.h>
#include "btif_api.h"
#include "btif_common.h"
#include "btif_vendor.h"
#include "btm_api.h"
#include "os/log.h"
#include "osi/include/allocator.h"
#include "osi/include/osi.h"
#include "stack/include/btm_client_interface.h"
#if TEST_APP_INTERFACE == TRUE
#include <bt_testapp.h>
#endif
using namespace bluetooth;

extern bool interface_ready(void);
#define SOC_NAME_MAX_SIZE 15

extern const btl2cap_interface_t* btif_l2cap_get_interface(void);
extern const btgatt_test_interface_t* btif_gatt_test_get_interface(void);
extern const btsmp_interface_t* btif_smp_get_interface(void);
extern const btgap_interface_t* btif_gap_get_interface(void);
#endif

btvendor_callbacks_t* bt_vendor_callbacks = NULL;

/*******************************************************************************
** VENDOR INTERFACE FUNCTIONS
*******************************************************************************/

/*******************************************************************************
**
** Function         btif_vendor_init
**
** Description     initializes the vendor interface
**
** Returns         bt_status_t
**
*******************************************************************************/
static bt_status_t init(btvendor_callbacks_t* callbacks) {
  bt_vendor_callbacks = callbacks;
  log::info("init done");
  return BT_STATUS_SUCCESS;
}

void btif_vendor_update_add_on_features_to_jni() {
  uint8_t soc_add_on_features_len = 0;
  uint8_t host_add_on_features_len = 0;
  bt_vendor_property_t vnd_prop;
  char s_buf[SOC_ADD_ON_FEATURES_MAX_SIZE];
  char h_buf[HOST_ADD_ON_FEATURES_MAX_SIZE];
  const bt_device_soc_add_on_features_t* soc_add_on_features =
      get_btm_client_interface().vendor.BTM_GetSocAddOnFeatures(
          &soc_add_on_features_len);
  const bt_device_host_add_on_features_t* host_add_on_features =
      get_btm_client_interface().vendor.BTM_GetHostAddOnFeatures(
          &host_add_on_features_len);

  if (soc_add_on_features && soc_add_on_features_len > 0) {
    vnd_prop.len = soc_add_on_features_len;
    vnd_prop.type = BT_VENDOR_PROPERTY_SOC_ADD_ON_FEATURES;
    vnd_prop.val = (void*)s_buf;
    memcpy(vnd_prop.val, soc_add_on_features, soc_add_on_features_len);

    HAL_CBACK(bt_vendor_callbacks, adapter_vendor_prop_cb, BT_STATUS_SUCCESS, 1,
              &vnd_prop);
  }

  if (host_add_on_features && host_add_on_features_len > 0) {
    vnd_prop.len = host_add_on_features_len;
    vnd_prop.type = BT_VENDOR_PROPERTY_HOST_ADD_ON_FEATURES;
    vnd_prop.val = (void*)h_buf;
    memcpy(vnd_prop.val, host_add_on_features, host_add_on_features_len);
    HAL_CBACK(bt_vendor_callbacks, adapter_vendor_prop_cb, BT_STATUS_SUCCESS, 1,
              &vnd_prop);
  }
}
void btif_vendor_update_add_on_features() {
  do_in_jni_thread(base::BindOnce(btif_vendor_update_add_on_features_to_jni));
}
static void set_wifi_state(bool status) {
  log::info("setWifiState :{}", status);
}

static void set_Power_back_off_state(bool status) {
  log::info("setPowerBackOffState :{}", status);
  get_btm_client_interface().vendor.BTM_SetPowerBackOffState(status);
}

static void cleanup(void) {
  log::info("cleanup");
  if (bt_vendor_callbacks) bt_vendor_callbacks = NULL;
}

/*******************************************************************************
**
** Function         get_testapp_interface
**
** Description      Get the Test interface
**
** Returns          btvendor_interface_t
**
*******************************************************************************/
#if TEST_APP_INTERFACE == TRUE
static const void* get_testapp_interface(int test_app_profile) {
  log::info("{} : ", test_app_profile);
  switch (test_app_profile) {
    case TEST_APP_L2CAP:
      return btif_l2cap_get_interface();
    case TEST_APP_GATT:
      return btif_gatt_test_get_interface();
    case TEST_APP_SMP:
      return btif_smp_get_interface();
    case TEST_APP_GAP:
      return btif_gap_get_interface();
    default:
      return NULL;
  }
  return NULL;
}
#endif

static const btvendor_interface_t btvendorInterface = {
    sizeof(btvendorInterface),
    init,
#if TEST_APP_INTERFACE == TRUE
    get_testapp_interface,
#else
    NULL,
#endif
    set_wifi_state,
    set_Power_back_off_state,
    cleanup,
};

/*******************************************************************************
** LOCAL FUNCTIONS
*******************************************************************************/

/*******************************************************************************
**
** Function         btif_vendor_get_interface
**
** Description      Get the vendor callback interface
**
** Returns          btvendor_interface_t
**
*******************************************************************************/
const btvendor_interface_t* btif_vendor_get_interface() {
  log::info("");
  return &btvendorInterface;
}
