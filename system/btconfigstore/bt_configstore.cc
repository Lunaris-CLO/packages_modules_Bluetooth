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

/*****************************************************************************
 *
 * This file contains main functions to support BT Config Store interface to
 * send commands and  received events.
 *
 *****************************************************************************/

#include <vector>

#include <cutils/properties.h>
#include "bt_configstore.h"

#include <utils/Log.h>
#include <vendor/qti/hardware/btconfigstore/1.0/IBTConfigStore.h>
#include <vendor/qti/hardware/btconfigstore/1.0/types.h>
#include <vendor/qti/hardware/btconfigstore/2.0/IBTConfigStore.h>
#include <vendor/qti/hardware/btconfigstore/2.0/types.h>
#include "osi/include/compat.h"

#include <cutils/properties.h>
#include <fcntl.h>
#include <hwbinder/IPCThreadState.h>
#include <hwbinder/ProcessState.h>
#include <string.h>

using android::hardware::IPCThreadState;

using ::vendor::qti::hardware::btconfigstore::V1_0::AddOnFeaturesList;
using ::vendor::qti::hardware::btconfigstore::V2_0::ControllerAddOnFeatures;
using ::vendor::qti::hardware::btconfigstore::V2_0::HostAddOnFeatures;

using Result_V1_0 = ::vendor::qti::hardware::btconfigstore::V1_0::Result;
using Result_V2_0 = ::vendor::qti::hardware::btconfigstore::V2_0::Result;
using VendorProperty_V1_0 =
    ::vendor::qti::hardware::btconfigstore::V1_0::VendorProperty;
using VendorProperty_V2_0 =
    ::vendor::qti::hardware::btconfigstore::V2_0::VendorProperty;
using IBTConfigStore_V1_0 =
    ::vendor::qti::hardware::btconfigstore::V1_0::IBTConfigStore;
using IBTConfigStore_V2_0 =
    ::vendor::qti::hardware::btconfigstore::V2_0::IBTConfigStore;

using ::android::hardware::hidl_vec;
using ::android::hardware::ProcessState;
using ::android::hardware::Return;
using ::android::hardware::Void;
using std::vector;

android::sp<IBTConfigStore_V1_0> btConfigStoreHal_1_0 = nullptr;
android::sp<IBTConfigStore_V2_0> btConfigStoreHal_2_0 = nullptr;

const bool IsLazyHalSupported(property_get_bool("ro.vendor.bt.enablelazyhal",
                                                false));

/**
 *
 * To get BT controller add On features
 */
static bool getControllerAddOnFeatures(
    controller_add_on_features_list_t* features_list);
/**
 *
 * To get BT Host add On features
 */
static bool getHostAddOnFeatures(host_add_on_features_list_t* features_list);
/**
 *
 *To get vendor property
 */
static bool getVendorProperties(uint32_t type,
                                std::vector<vendor_property_t>& vPropList);
/**
 * To set BT Vendor Properties
 */
static bool setVendorProperty(uint32_t type, const char* value);

/**
 * To convert soc name to soc type
 */
static bt_soc_type_t convertSocNameToBTSocType(const char* name);
/**
 * To convert prop type  to string format
 */
static const char* convertPropTypeToStringFormat(uint32_t propType);

bt_configstore_interface_t btConfigStoreInterface = {
    sizeof(btConfigStoreInterface),
    getVendorProperties,
    getControllerAddOnFeatures,
    getHostAddOnFeatures,
    setVendorProperty,
    convertSocNameToBTSocType,
    convertPropTypeToStringFormat,
};

bt_configstore_interface_t* get_btConfigStore_interface() {
  return &btConfigStoreInterface;
}

/*******************************************************************************
**
** Function         getVendorProperties
**
** Description      This function is used to read predefined vendor properties
*from vendor module
**                  to system module It makes a binder call to hal daemon for
*reading properties.
**
** Parameters:      vPropType - is a vendor property type
**                  vPropList - is a referance vector of vendor property list
**
** Returns          bool
**
*******************************************************************************/
bool getVendorProperties(uint32_t vPropType,
                         std::vector<vendor_property_t>& vPropList) {
  bool status = false;
  Return<void> ret;

  ALOGI("%s ", __func__);
  btConfigStoreHal_2_0 = IBTConfigStore_V2_0::getService();

  if (btConfigStoreHal_2_0 != nullptr) {
    hidl_vec<VendorProperty_V2_0> vendorPropList;
    auto halResult = Result_V2_0::UNKNOWN_ERROR;
    auto cb = [&](Result_V2_0 result,
                  hidl_vec<VendorProperty_V2_0> vendorPropListCb) {
      halResult = result;
      vendorPropList = vendorPropListCb;
    };

    ret = btConfigStoreHal_2_0->getVendorProperties(vPropType, cb);
    if (!ret.isOk()) {
      ALOGE("%s, HIDL returns error ", __func__);
    }
    if (halResult == Result_V2_0::SUCCESS) {
      for (auto&& vendorProp : vendorPropList) {
        vendor_property_t vProp;

        vProp.type = vendorProp.type;
        strlcpy(vProp.value, vendorProp.value.c_str(), sizeof(vProp.value));
        vPropList.push_back(vProp);
        ALOGI("getVendorProperties: prop type: %s, prop_value: %s",
              convertPropTypeToStringFormat(vProp.type), vProp.value);
      }
      status = true;
    }

  } else {
    btConfigStoreHal_1_0 = IBTConfigStore_V1_0::getService();
    if (btConfigStoreHal_1_0 != nullptr) {
      hidl_vec<VendorProperty_V1_0> vendorPropList;
      auto halResult = Result_V1_0::UNKNOWN_ERROR;
      auto cb = [&](Result_V1_0 result,
                    hidl_vec<VendorProperty_V1_0> vendorPropListCb) {
        halResult = result;
        vendorPropList = vendorPropListCb;
      };

      ret = btConfigStoreHal_1_0->getVendorProperties(vPropType, cb);
      if (!ret.isOk()) {
        ALOGE("%s, HIDL returns error ", __func__);
      }
      if (halResult == Result_V1_0::SUCCESS) {
        for (auto&& vendorProp : vendorPropList) {
          vendor_property_t vProp;

          vProp.type = vendorProp.type;
          strlcpy(vProp.value, vendorProp.value.c_str(), sizeof(vProp.value));
          vPropList.push_back(vProp);
          ALOGI("prop type: %s, prop_value: %s",
                convertPropTypeToStringFormat(vProp.type), vProp.value);
        }
        status = true;
      }
    } else {
      ALOGW("%s btConfigStore hal interface is null", __func__);
    }
  }

  if (IsLazyHalSupported &&
      (btConfigStoreHal_2_0 != nullptr || btConfigStoreHal_1_0 != nullptr))
    IPCThreadState::self()->flushCommands();

  btConfigStoreHal_2_0 = nullptr;
  btConfigStoreHal_1_0 = nullptr;

  return status;
}

/*******************************************************************************
**
** Function         setVendorProperty
**
** Description      This function is used to set predefined vendor properties
*from system module
**                  to vendor module It makes a binder call to hal daemon for
*setting properties.
**
** Parameters:      type - is a vendor property type.
**                  value - is a pointer to vendor property value.
**
** Returns          bool
**
*******************************************************************************/
bool setVendorProperty(uint32_t type, const char* value) {
  bool status = false;
  std::string vPropValue(value);

  ALOGI("%s ", __func__);

  btConfigStoreHal_2_0 = IBTConfigStore_V2_0::getService();

  if (btConfigStoreHal_2_0 != nullptr) {
    VendorProperty_V2_0 vProp = {type, vPropValue};
    Result_V2_0 halResult = Result_V2_0::UNKNOWN_ERROR;

    halResult = btConfigStoreHal_2_0->setVendorProperty(vProp);

    ALOGI("%s:: halResult = %d", __func__, halResult);

    if (halResult == Result_V2_0::SUCCESS) {
      status = true;
    }
  } else {
    btConfigStoreHal_1_0 = IBTConfigStore_V1_0::getService();
    if (btConfigStoreHal_1_0 != nullptr) {
      VendorProperty_V1_0 vProp = {type, vPropValue};
      Result_V1_0 halResult = Result_V1_0::UNKNOWN_ERROR;

      halResult = btConfigStoreHal_1_0->setVendorProperty(vProp);

      ALOGI("%s:: halResult = %d", __func__, halResult);

      if (halResult == Result_V1_0::SUCCESS) {
        status = true;
      }
    } else {
      ALOGW("%s btConfigStore is null", __func__);
    }
  }

  if (IsLazyHalSupported &&
      (btConfigStoreHal_2_0 != nullptr || btConfigStoreHal_1_0 != nullptr))
    IPCThreadState::self()->flushCommands();

  btConfigStoreHal_2_0 = nullptr;
  btConfigStoreHal_1_0 = nullptr;
  return status;
}

/*******************************************************************************
**
** Function         getControllerAddOnFeatures
**
** Description      This function is used to read BT controller add on features
*from bt config store
**                  hidl transport. It makes a binder call to hal daemon
**
** Parameters:      pointer to controller_add_on_features_list_t
**
**
** Returns          bool
**
*******************************************************************************/
bool getControllerAddOnFeatures(
    controller_add_on_features_list_t* features_list) {
  bool status = false;
  ALOGI("%s ", __func__);

  btConfigStoreHal_2_0 = IBTConfigStore_V2_0::getService();

  if (btConfigStoreHal_2_0 != nullptr) {
    ControllerAddOnFeatures featureList;
    auto halResult = Result_V2_0::UNKNOWN_ERROR;
    auto cb = [&](Result_V2_0 result, ControllerAddOnFeatures featureListCb) {
      halResult = result;
      featureList = featureListCb;
    };

    auto hidlResult = btConfigStoreHal_2_0->getControllerAddOnFeatures(cb);

    ALOGI("%s:: halResult = %d", __func__, halResult);

    if (hidlResult.isOk() && halResult == Result_V2_0::SUCCESS) {
      std::stringstream features;

      features_list->product_id = featureList.product_id;
      features_list->rsp_version = featureList.rsp_version;
      features_list->feat_mask_len = featureList.feat_mask_len;
      memcpy(features_list->features, featureList.features.data(),
             featureList.features.size());

      std::copy(features_list->features,
                features_list->features + features_list->feat_mask_len,
                std::ostream_iterator<int>(
                    features << std::showbase << std::hex, " "));

      ALOGI(
          "%s:: product_id = %d, version = %d, feat_mask_len = %d "
          "features data: %s",
          __func__, features_list->product_id, features_list->rsp_version,
          features_list->feat_mask_len, features.str().c_str());
      status = true;
    }

  } else {
    btConfigStoreHal_1_0 = IBTConfigStore_V1_0::getService();
    if (btConfigStoreHal_1_0 != nullptr) {
      AddOnFeaturesList featureList;
      auto halResult = Result_V1_0::UNKNOWN_ERROR;
      auto cb = [&](Result_V1_0 result, AddOnFeaturesList featureListCb) {
        halResult = result;
        featureList = featureListCb;
      };

      auto hidlResult = btConfigStoreHal_1_0->getAddOnFeatures(cb);

      ALOGI("%s:: halResult = %d", __func__, halResult);

      if (hidlResult.isOk() && halResult == Result_V1_0::SUCCESS) {
        std::stringstream features;

        features_list->product_id = featureList.product_id;
        features_list->rsp_version = featureList.rsp_version;
        features_list->feat_mask_len = featureList.feat_mask_len;
        memcpy(features_list->features, &featureList.features,
               features_list->feat_mask_len);

        std::copy(features_list->features,
                  features_list->features + features_list->feat_mask_len,
                  std::ostream_iterator<int>(
                      features << std::showbase << std::hex, " "));

        ALOGI(
            "%s:: product_id = %d, version = %d, feat_mask_len = %d "
            "features data: %s",
            __func__, features_list->product_id, features_list->rsp_version,
            features_list->feat_mask_len, features.str().c_str());

        status = true;
      }
    }
  }

  if (IsLazyHalSupported &&
      (btConfigStoreHal_2_0 != nullptr || btConfigStoreHal_1_0 != nullptr))
    IPCThreadState::self()->flushCommands();

  btConfigStoreHal_2_0 = nullptr;
  btConfigStoreHal_1_0 = nullptr;

  return status;
}

/*******************************************************************************
**
** Function         getHostAddOnFeatures
**
** Description      This function is used to read BT host add on features from
*bt config store
**                  hidl transport. It makes a binder call to hal daemon
**
** Parameters:      pointer to add_on_features_list_t
**
**
** Returns          bool
**
*******************************************************************************/
bool getHostAddOnFeatures(host_add_on_features_list_t* features_list) {
  bool status = false;

  ALOGI("%s ", __func__);

  btConfigStoreHal_2_0 = IBTConfigStore_V2_0::getService();

  if (btConfigStoreHal_2_0 != nullptr) {
    HostAddOnFeatures featureList;
    auto halResult = Result_V2_0::UNKNOWN_ERROR;
    auto cb = [&](Result_V2_0 result, HostAddOnFeatures featureListCb) {
      halResult = result;
      featureList = featureListCb;
    };

    auto hidlResult = btConfigStoreHal_2_0->getHostAddOnFeatures(cb);

    ALOGI("%s:: halResult = %d", __func__, halResult);

    if (hidlResult.isOk() && halResult == Result_V2_0::SUCCESS) {
      std::stringstream features;

      features_list->feat_mask_len = featureList.feat_mask_len;
      memcpy(features_list->features, featureList.features.data(),
             featureList.features.size());

      std::copy(features_list->features,
                features_list->features + features_list->feat_mask_len,
                std::ostream_iterator<int>(
                    features << std::showbase << std::hex, " "));

      ALOGI("%s:: feat_mask_len = %d features data: %s", __func__,
            features_list->feat_mask_len, features.str().c_str());
      status = true;
    }
  } else {
    ALOGW("%s add on features is not avaliable", __func__);
  }

  if (IsLazyHalSupported && (btConfigStoreHal_2_0 != nullptr))
    IPCThreadState::self()->flushCommands();

  btConfigStoreHal_2_0 = nullptr;

  return status;
}

/*******************************************************************************
**
** Function         convertSocNameToBTSocType
**
** Description      This function is used to convert bt soc name to soc type
**
** Parameters:      soc_name - is a pointer to char
**
** Returns          bt_soc_type_t - soc type
**
*******************************************************************************/
bt_soc_type_t convertSocNameToBTSocType(const char* soc_name) {
  bt_soc_type_t soc_type;
  if (!strncasecmp(soc_name, "rome", sizeof("rome"))) {
    soc_type = BT_SOC_TYPE_ROME;
  } else if (!strncasecmp(soc_name, "cherokee", sizeof("cherokee"))) {
    soc_type = BT_SOC_TYPE_CHEROKEE;
  } else if (!strncasecmp(soc_name, "ath3k", sizeof("ath3k"))) {
    soc_type = BT_SOC_TYPE_AR3K;
  } else if (!strncasecmp(soc_name, "hastings", sizeof("hastings"))) {
    soc_type = BT_SOC_TYPE_HASTINGS;
  } else if (!strncasecmp(soc_name, "moselle", sizeof("moselle"))) {
    soc_type = BT_SOC_TYPE_MOSELLE;
  } else if (!strncasecmp(soc_name, "hamilton", sizeof("hamilton"))) {
    soc_type = BT_SOC_TYPE_HAMILTON;
  } else if (!strncasecmp(soc_name, "ganges", sizeof("ganges"))) {
    soc_type = BT_SOC_TYPE_GANGES;
  } else if (!strncasecmp(soc_name, "orne", sizeof("orne"))) {
    soc_type = BT_SOC_TYPE_ORNE;
  } else if (!strncasecmp(soc_name, "evros", sizeof("evros"))) {
    soc_type = BT_SOC_TYPE_EVROS;
  } else if (!strncasecmp(soc_name, "pronto", sizeof("pronto"))) {
    soc_type = BT_SOC_TYPE_DEFAULT;
  } else {
    ALOGI("not set, so using pronto");
    soc_type = BT_SOC_TYPE_DEFAULT;
  }

  ALOGI("soc_name: %s, soc_type = %04x", soc_name, soc_type);
  return soc_type;
}

/*******************************************************************************
**
** Function         convertPropTypeToStringFormat
**
** Description      This function is used to convert property type to string
*format
**
** Parameters:      propType - is a vendor property type
**
** Returns          const char * - pointer to const char
**
*******************************************************************************/
const char* convertPropTypeToStringFormat(uint32_t propType) {
  switch (propType) {
    case BT_PROP_ALL:
      return "BT_PROP_ALL";

    case BT_PROP_SOC_TYPE:
      return "BT_PROP_SOC_TYPE";

    case BT_PROP_A2DP_OFFLOAD_CAP:
      return "BT_PROP_A2DP_OFFLOAD_CAP";

    case BT_PROP_SPILT_A2DP:
      return "BT_PROP_SPILT_A2DP";

    case BT_PROP_AAC_FRAME_CTL:
      return "BT_PROP_AAC_FRAME_CTL";

    default:
      ALOGI(" not handled propType = %d", propType);
      return "not handled";
  }
}
