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

import android.app.Application;
import android.bluetooth.BluetoothDevice;
import android.content.Context;
import androidx.annotation.NonNull;
import androidx.lifecycle.AndroidViewModel;
import androidx.lifecycle.LiveData;
import androidx.lifecycle.MutableLiveData;
import com.android.bluetooth.channelsoundingtestapp.DistanceMeasurementInitiator.BtDistanceMeasurementCallback;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.List;
import android.util.Log;

/** ViewModel for the Initiator. */
public class InitiatorViewModel extends AndroidViewModel {
    private static final String TAG = "AndroidViewModel";
    public int distance_count = 0;
    private final MutableLiveData<String> mLogText = new MutableLiveData<>();
    private final MutableLiveData<Boolean> mCsStarted = new MutableLiveData<>(false);

    private final MutableLiveData<Double> mDistanceResult = new MutableLiveData<>();

    private final DistanceMeasurementInitiator
            mDistanceMeasurementInitiator; // mDistanceMeasurementInitiator;

    public InitiatorViewModel(@NonNull Application application) {
        super(application);

        mDistanceMeasurementInitiator =
                new DistanceMeasurementInitiator(
                        application,
                        mBtDistanceMeasurementCallback,
                        log -> {
                            mLogText.postValue("BT LOG: " + log);
                        });
    }

    void setTargetDevice(BluetoothDevice targetDevice) {
        mDistanceMeasurementInitiator.setTargetDevice(targetDevice);
    }

    LiveData<String> getLogText() {
        return mLogText;
    }

    LiveData<Boolean> getCsStarted() {
        return mCsStarted;
    }

    LiveData<Double> getDistanceResult() {
        return mDistanceResult;
    }

    List<String> getSupportedDmMethods() {
        return mDistanceMeasurementInitiator.getDistanceMeasurementMethods();
    }

    public class FileAppender {
      public static void appendToFile(Context context, String filename, String data) {
        FileOutputStream fos = null;
        try {
          if (fos != null) {
            fos = context.openFileOutput(filename, Context.MODE_APPEND);
            fos.write(data.getBytes());
            fos.close();
          }
        } catch (IOException e) {
          e.printStackTrace();
        } finally {
          if (fos != null) {
            try {
              fos.close();
            } catch (IOException e) {
              e.printStackTrace();
            }
          }
        }
      }
    }

    void logMarker() {
         Log.d(TAG, "BCS LOG MARKER Count : " + distance_count);
         distance_count++;
    }
    void actualDistance(String distance) {
         Log.d(TAG, "BCS Actual distance : " + distance);
    }

    void toggleCsStartStop(
        String distanceMeasurementMethodName, String security_mode, String freq, String duration) {
      if (!mCsStarted.getValue()) {
        mDistanceMeasurementInitiator.startDistanceMeasurement(
            distanceMeasurementMethodName, security_mode, freq, duration);
      } else {
        distance_count = 0;
        mDistanceMeasurementInitiator.stopDistanceMeasurement();
      }
    }

    private BtDistanceMeasurementCallback mBtDistanceMeasurementCallback =
            new BtDistanceMeasurementCallback() {
                @Override
                public void onStartSuccess() {
                    mCsStarted.postValue(true);
                }

                @Override
                public void onStartFail() {}

                @Override
                public void onStop() {
                    mCsStarted.postValue(false);
                }

                @Override
                public void onDistanceResult(double distanceMeters) {
                    mDistanceResult.postValue(distanceMeters);
                }
            };
}
