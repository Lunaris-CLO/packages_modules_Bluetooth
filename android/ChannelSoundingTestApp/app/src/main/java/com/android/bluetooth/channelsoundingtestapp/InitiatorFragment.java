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

import android.bluetooth.BluetoothGatt;
import android.os.Bundle;
import android.text.TextUtils;
import android.util.Log;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.TextView;
import androidx.annotation.NonNull;
import androidx.fragment.app.Fragment;
import androidx.fragment.app.FragmentTransaction;
import androidx.lifecycle.ViewModelProvider;
import com.android.bluetooth.channelsoundingtestapp.InitiatorViewModel.FileAppender;
import java.text.DecimalFormat;
import java.time.LocalDateTime;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** The fragment holds the initiator of channel sounding. */
@SuppressWarnings("SetTextI18n")
public class InitiatorFragment extends Fragment {
  private static final DecimalFormat DISTANCE_DECIMAL_FMT = new DecimalFormat("0.0");

  private ArrayAdapter<String> mDmMethodArrayAdapter;
  private ArrayAdapter<String> msecurityModeAdapter;
  private ArrayAdapter<String> msetfrequencyAdapter;
  private ArrayList<String> securityModes;
  private ArrayList<String> Conn_Interval;
  private Button seclevelset;
  private Button freqset;
  private Button durset;
  private Button methoddist;
  private Button distancemarker;        // for storing the distance in the file
  private Button distancemarkerlog;    //for adding marker in log
  private double curr_distance;
  private EditText dur_text;
  private EditText dis_meas;
  private Spinner mSpinnerSecurityMode;
  private Spinner mSpinnersetfrequency;
  private ArrayList<String> frequency;
  private TextView mDistanceText;
  private CanvasView mDistanceCanvasView;
  private Spinner mSpinnerDmMethod;
  private Button mButtonCs;
  private LinearLayout mDistanceViewLayout;
  private Spinner mConnUpSpinner;
  private Button mConnUpButton;
  private TextView mLogText;
  private BleConnectionViewModel mBleConnectionViewModel;
  private InitiatorViewModel mInitiatorViewModel;

  @Override
  public View onCreateView(
      @NonNull LayoutInflater inflater, ViewGroup container, Bundle savedInstanceState) {
    View root = inflater.inflate(R.layout.fragment_initiator, container, false);
    Fragment bleConnectionFragment = new BleConnectionFragment();
    FragmentTransaction transaction = getChildFragmentManager().beginTransaction();
    transaction.replace(R.id.init_ble_connection_container, bleConnectionFragment).commit();
    mSpinnerSecurityMode = (Spinner) root.findViewById(R.id.spinner_security_mode);
    distancemarker = (Button) root.findViewById(R.id.marker_dist);
    distancemarkerlog = (Button) root.findViewById(R.id.marker_log);
    mSpinnersetfrequency = (Spinner) root.findViewById(R.id.spinner_frequency);
    dur_text = (EditText) root.findViewById(R.id.edittext_duration);
    dis_meas = (EditText) root.findViewById(R.id.distance_meas);
    mButtonCs = (Button) root.findViewById(R.id.btn_cs);
    mSpinnerDmMethod = (Spinner) root.findViewById(R.id.spinner_dm_method);
    mDistanceViewLayout = (LinearLayout) root.findViewById(R.id.layout_distance_view);
    mDistanceText = new TextView(getContext());
    mDistanceViewLayout.addView(mDistanceText);
    mDistanceText.setText("0.00 m");
    mDistanceText.setTextSize(96);
    mDistanceText.setGravity(Gravity.END);
    mDistanceCanvasView = new CanvasView(getContext(), "Distance");
    mDistanceViewLayout.addView(mDistanceCanvasView);
    mDistanceViewLayout.setPadding(0, 0, 0, 600);
    mLogText = (TextView) root.findViewById(R.id.text_log);
    mConnUpSpinner = (Spinner) root.findViewById(R.id.conn_up_spinner);
    mConnUpButton = (Button) root.findViewById(R.id.conn_up_button);
    return root;
    }

    public void onViewCreated(@NonNull View view, Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);

        mDmMethodArrayAdapter =
                new ArrayAdapter<String>(
                        getContext(), android.R.layout.simple_spinner_item, new ArrayList<>());
        mDmMethodArrayAdapter.setDropDownViewResource(
                android.R.layout.simple_spinner_dropdown_item);
        mSpinnerDmMethod.setAdapter(mDmMethodArrayAdapter);

        mInitiatorViewModel = new ViewModelProvider(this).get(InitiatorViewModel.class);
        mBleConnectionViewModel = new ViewModelProvider(this).get(BleConnectionViewModel.class);
        mBleConnectionViewModel
                .getLogText()
                .observe(
                        getActivity(),
                        log -> {
                            mLogText.setText(log);
                        });
        mBleConnectionViewModel
                .getTargetDevice()
                .observe(
                        getActivity(),
                        targetDevice -> {
                            mInitiatorViewModel.setTargetDevice(targetDevice);
                        });

        List<String> securityModes = Arrays.asList("1", "2", "3", "4");
        mSpinnerSecurityMode.setAdapter(
            new ArrayAdapter<>(getContext(), android.R.layout.simple_spinner_item, securityModes));

        List<String> frequency = Arrays.asList(
            "REPORT_FREQUENCY_LOW", "REPORT_FREQUENCY_MEDIUM", "REPORT_FREQUENCY_HIGH");
        mSpinnersetfrequency.setAdapter(
            new ArrayAdapter<>(getContext(), android.R.layout.simple_spinner_item, frequency));

        Conn_Interval = new ArrayList<>();
        Conn_Interval.add("Balanced");
        Conn_Interval.add("High Priority");
        Conn_Interval.add("Low Power");

        ArrayAdapter<String> adapter = new ArrayAdapter<String>(
            getContext(), android.R.layout.simple_spinner_item, Conn_Interval);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        mConnUpSpinner.setAdapter(adapter);

        mConnUpButton.setOnClickListener(new View.OnClickListener() {
          @Override
          public void onClick(View view) {
          }
        });

        mInitiatorViewModel
                .getCsStarted()
                .observe(
                        getActivity(),
                        started -> {
                            if (started) {
                                mButtonCs.setText("Stop Distance Measurement");
                                mDistanceCanvasView.cleanUp();
                            } else {
                                mButtonCs.setText("Start Distance Measurement");
                            }
                        });
        mInitiatorViewModel
                .getLogText()
                .observe(
                        getActivity(),
                        log -> {
                            mLogText.setText(log);
                        });

        mInitiatorViewModel
                .getDistanceResult()
                .observe(
                        getActivity(),
                        distanceMeters -> {
                            mDistanceCanvasView.addNode(distanceMeters, /* abort= */ false);
                            mDistanceText.setText(
                                    DISTANCE_DECIMAL_FMT.format(distanceMeters) + " m");
                            curr_distance = distanceMeters;
                            String timestamp = LocalDateTime.now().format(
                                DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss"));
                            FileAppender.appendToFile(getActivity(), "myfile.csv",
                                distanceMeters + "," + timestamp + "\n");
                        });

        mDmMethodArrayAdapter.addAll(mInitiatorViewModel.getSupportedDmMethods());

        mButtonCs.setOnClickListener(v -> {
        if(!mBleConnectionViewModel.isconnected()) {
            printLog("Do Gatt Connect First");
            return;
        }
          String methodName = mSpinnerDmMethod.getSelectedItem().toString();
          String opt_frequency = mSpinnersetfrequency.getSelectedItem().toString();
          String sec_mode_selected = mSpinnerSecurityMode.getSelectedItem().toString();
          String duration_selected = dur_text.getText().toString();
          int conn_state = (int) mConnUpSpinner.getSelectedItemId();
          String conn_priority = mConnUpSpinner.getSelectedItem().toString();
          mBleConnectionViewModel.updateconnectioninterval(conn_priority);
          if (!duration_selected.isEmpty()) {
            if (Integer.parseInt(duration_selected) >= 60
                && Integer.parseInt(duration_selected) <= 3600) {
              if (TextUtils.isEmpty(methodName)) {
                printLog("the device doesn't support any distance measurement methods.");
              }
              mInitiatorViewModel.toggleCsStartStop(
                  methodName, sec_mode_selected, opt_frequency, duration_selected);
            } else {
              printLog("Please enter the duration between 60 sec to 3600 sec");
            }
          } else {
            if (TextUtils.isEmpty(methodName)) {
              printLog("the device doesn't support any distance measurement methods.");
            }
            mInitiatorViewModel.toggleCsStartStop(
                methodName, sec_mode_selected, opt_frequency, duration_selected);
          }
        });

        distancemarker.setOnClickListener(v -> {
          String dist_meas = dis_meas.getText().toString();
          mInitiatorViewModel.actualDistance(dist_meas);
          FileAppender.appendToFile(
              getActivity(), "myfile.csv", "changing distance to " + dist_meas + "\n");
        });

        distancemarkerlog.setOnClickListener(v -> {
          mInitiatorViewModel.logMarker();
        });
    }

    private void printLog(String logMessage) {
        mLogText.setText("LOG: " + logMessage);
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
    }
}
