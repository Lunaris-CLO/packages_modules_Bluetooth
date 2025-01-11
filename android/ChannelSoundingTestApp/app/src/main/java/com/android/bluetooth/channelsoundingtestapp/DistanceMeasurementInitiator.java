/*
 * Copyright 2024 The Android Open Source Project
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
 *
 * Changes from Qualcomm Innovation Center, Inc. are provided under the following license:
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

package com.android.bluetooth.channelsoundingtestapp;

import android.annotation.SuppressLint;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothStatusCodes;
import android.bluetooth.le.ChannelSoundingParams;
import android.bluetooth.le.DistanceMeasurementManager;
import android.bluetooth.le.DistanceMeasurementMethod;
import android.bluetooth.le.DistanceMeasurementParams;
import android.bluetooth.le.DistanceMeasurementResult;
import android.bluetooth.le.DistanceMeasurementSession;
import android.content.Context;
import android.text.TextUtils;
import android.util.Log;
import android.util.Pair;
import androidx.annotation.Nullable;
import java.io.FileOutputStream;
import java.io.IOException;
import java.math.BigDecimal;
import java.math.RoundingMode;
import java.text.DecimalFormat;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;

class DistanceMeasurementInitiator {
    private static final int DISTANCE_MEASUREMENT_DURATION_SEC = 3600;
    private int mode_int = 0;
    private int duration_int = 0;
    private int freq_int = 0;
    private static final List<Pair<Integer, String>> mDistanceMeasurementMethodMapping =
            List.of(
                    new Pair<>(DistanceMeasurementMethod.DISTANCE_MEASUREMENT_METHOD_AUTO, "AUTO"),
                    new Pair<>(DistanceMeasurementMethod.DISTANCE_MEASUREMENT_METHOD_RSSI, "RSSI"),
                    new Pair<>(
                            DistanceMeasurementMethod.DISTANCE_MEASUREMENT_METHOD_CHANNEL_SOUNDING,
                            "Channel Sounding"));

    private final BluetoothAdapter mBluetoothAdapter;
    private final LoggingListener mLoggingListener;

    private final Context mApplicationContext;
    private final Executor mBtExecutor;
    private final BtDistanceMeasurementCallback mBtDistanceMeasurementCallback;
    @Nullable private DistanceMeasurementSession mSession = null;
    @Nullable private BluetoothDevice mTargetDevice = null;

    DistanceMeasurementInitiator(
            Context applicationContext,
            BtDistanceMeasurementCallback btDistanceMeasurementCallback,
            LoggingListener loggingListener) {
        mApplicationContext = applicationContext;
        mBtDistanceMeasurementCallback = btDistanceMeasurementCallback;
        mLoggingListener = loggingListener;

        BluetoothManager bluetoothManager =
                mApplicationContext.getSystemService(BluetoothManager.class);
        mBluetoothAdapter = bluetoothManager.getAdapter();

        mBtExecutor = Executors.newSingleThreadExecutor();
    }

    void setTargetDevice(BluetoothDevice targetDevice) {
        mTargetDevice = targetDevice;
    }
    private void printLog(String log) {
        mLoggingListener.onLog(log);
    }

    private String getDistanceMeasurementMethodName(int methodId) {
        for (Pair<Integer, String> methodMapping : mDistanceMeasurementMethodMapping) {
            if (methodMapping.first == methodId) {
                return methodMapping.second;
            }
        }
        throw new IllegalArgumentException("unknown distance measurement method id" + methodId);
    }

    private int getDistanceMeasurementMethodId(String methodName) {
        for (Pair<Integer, String> methodMapping : mDistanceMeasurementMethodMapping) {
            if (methodMapping.second.equals(methodName)) {
                return methodMapping.first;
            }
        }
        throw new IllegalArgumentException("unknown distance measurement method name" + methodName);
    }

    @SuppressLint("MissingPermission") // permissions are checked upfront
    List<String> getDistanceMeasurementMethods() {
        List<String> methods = new ArrayList<>();
        if (mBluetoothAdapter.isDistanceMeasurementSupported()
                != BluetoothStatusCodes.FEATURE_SUPPORTED) {
            printLog("No distance measurement is supported");
            return methods;
        }
        DistanceMeasurementManager distanceMeasurementManager =
                mBluetoothAdapter.getDistanceMeasurementManager();
        List<DistanceMeasurementMethod> list = distanceMeasurementManager.getSupportedMethods();

        StringBuilder dbgMessage = new StringBuilder("getDistanceMeasurementMethods: ");
        for (DistanceMeasurementMethod method : list) {
            String methodName = getDistanceMeasurementMethodName((int) method.getId());
            dbgMessage.append(methodName).append(", ");
            methods.add(methodName);
        }
        printLog(dbgMessage.toString());
        return methods;
    }

    @SuppressLint("MissingPermission") // permissions are checked upfront
    void startDistanceMeasurement(
        String distanceMeasurementMethodName, String sec_mode, String freq, String duration) {
      if (mTargetDevice == null) {
        printLog("do Gatt connect first");
        return;
      }

      printLog("start CS with device: " + mTargetDevice.getName());

      ChannelSoundingParams csParams = new ChannelSoundingParams.Builder()
                                           .setSightType(0)
                                           .setLocationType(0)
                                           .setCsSecurityLevel(Integer.parseInt(sec_mode))
                                           .build();

      int freqint = 0;
      switch (freq) {
        case "REPORT_FREQUENCY_LOW":
          freqint = 0;
          break;
        case "REPORT_FREQUENCY_MEDIUM":
          freqint = 1;
          break;
        case "REPORT_FREQUENCY_HIGH":
          freqint = 2;
          break;
      }

      freq_int = freqint;
      mode_int = getDistanceMeasurementMethodId(distanceMeasurementMethodName);
      if (TextUtils.isEmpty(duration)) {
        duration = "60";
      }

      duration_int = Integer.parseInt(duration);

      DistanceMeasurementParams params =
          new DistanceMeasurementParams.Builder(mTargetDevice)
              .setDurationSeconds(duration_int)
              .setFrequency(freqint)
              .setMethodId(getDistanceMeasurementMethodId(distanceMeasurementMethodName))
              .setChannelSoundingParams(csParams)
              .build();

      DistanceMeasurementManager distanceMeasurementManager =
          mBluetoothAdapter.getDistanceMeasurementManager();
      distanceMeasurementManager.startMeasurementSession(params, mBtExecutor, mTestcallback);
    }

    void stopDistanceMeasurement() {
        if (mSession == null) {
            return;
        }
        mSession.stopSession();
        mSession = null;
    }

    private DistanceMeasurementSession.Callback mTestcallback =
            new DistanceMeasurementSession.Callback() {
                public void onStarted(DistanceMeasurementSession session) {
                    printLog("DistanceMeasurement onStarted ! ");
                    mSession = session;
                    mBtDistanceMeasurementCallback.onStartSuccess();
                }

                public void onStartFail(int reason) {
                    printLog("DistanceMeasurement onStartFail ! reason " + reason);
                    mBtDistanceMeasurementCallback.onStartFail();
                }

                public void onStopped(DistanceMeasurementSession session, int reason) {
                    printLog("DistanceMeasurement onStopped ! reason " + reason);
                    mBtDistanceMeasurementCallback.onStop();
                    mSession = null;
                }

                public void onResult(BluetoothDevice device, DistanceMeasurementResult result) {
                  double Result = result.getResultMeters();
                  BigDecimal bd = new BigDecimal(Double.toString(Result));
                  bd = bd.setScale(1, RoundingMode.HALF_UP);
                  double roundedResult = bd.doubleValue();
                  double Err_Result = result.getErrorMeters();
                  BigDecimal bd_err = new BigDecimal(Double.toString(Err_Result));
                  bd_err = bd_err.setScale(1, RoundingMode.HALF_UP);
                  double rounded_errResult = bd_err.doubleValue();
                  printLog(
                      "DistanceMeasurement onResult ! " + roundedResult + ", " + rounded_errResult);
                  mBtDistanceMeasurementCallback.onDistanceResult(result.getResultMeters());
                }
            };

    interface BtDistanceMeasurementCallback {

        void onStartSuccess();

        void onStartFail();

        void onStop();

        void onDistanceResult(double distanceMeters);
    }
}
