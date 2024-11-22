/******************************************************************************
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
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ANDROID_INCLUDE_BT_VENDOR_H
#define ANDROID_INCLUDE_BT_VENDOR_H

#include <hardware/bluetooth.h>
#include <vector>

__BEGIN_DECLS

#define BT_PROFILE_VENDOR_ID "vendor"

typedef enum {
  BT_VENDOR_PROPERTY_HOST_ADD_ON_FEATURES = 0x01,
  BT_VENDOR_PROPERTY_SOC_ADD_ON_FEATURES,
} bt_vendor_property_type_t;

typedef struct {
  bt_vendor_property_type_t type;
  int len;
  void* val;
} bt_vendor_property_t;

/** Callback to handle SSR */
typedef void (*ssr_vendor_callback)(void);

/** Callback to notify the remote device vendor properties.
 */
typedef void (*adapter_vendor_prop_callback)(bt_status_t status,
                                             int num_properties,
                                             bt_vendor_property_t* properties);

/** BT-Vendor callback structure. */
typedef struct {
  /** set to sizeof(BtVendorCallbacks) */
  size_t size;
  adapter_vendor_prop_callback adapter_vendor_prop_cb;
  ssr_vendor_callback ssr_vendor_cb;
} btvendor_callbacks_t;

/** Represents the standard BT-Vendor interface.
 */
typedef struct {
  /** set to sizeof(BtVendorInterface) */
  size_t size;

  /**
   * Register the BtVendor callbacks
   */
  bt_status_t (*init)(btvendor_callbacks_t* callbacks);

  /** test interface. */
  const void* (*get_testapp_interface)(int);

  /** set wifi state */
  void (*set_wifi_state)(bool);

  /** set Power_back_off state */
  void (*set_Power_back_off_state)(bool);

  /** Closes the interface. */
  void (*cleanup)(void);
} btvendor_interface_t;

__END_DECLS

#endif /* ANDROID_INCLUDE_BT_VENDOR_H */
