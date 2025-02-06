/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
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
 */
/*
 * Changes from Qualcomm Innovation Center, Inc. are provided under the following license:
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
package com.android.bluetooth.le_audio;

import static android.Manifest.permission.BLUETOOTH_CONNECT;
import static com.android.bluetooth.Utils.enforceBluetoothPrivilegedPermission;

import android.annotation.RequiresPermission;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothUuid;
import android.bluetooth.BluetoothHeadset;

import android.content.Context;
import android.content.Intent;
import android.os.UserHandle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.Message;
import android.os.SystemProperties;
import android.util.Log;

import com.android.bluetooth.Utils;
import com.android.bluetooth.BluetoothStatsLog;
import com.android.bluetooth.hfp.HeadsetService;
import com.android.bluetooth.btservice.AdapterService;
import com.android.bluetooth.btservice.ServiceFactory;
import com.android.bluetooth.vc.VolumeControlService;


import java.lang.Integer;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;


public class CallAudio {

    private static CallAudio mCallAudio;
    private static final String TAG = "CallAudio";
    Map<String, CallDevice> mCallDevicesMap;
    private Context mContext;
    private AdapterService mAdapterService;
    public static final String BLUETOOTH_PERM = android.Manifest.permission.BLUETOOTH;
    public static final String BLUETOOTH_ADMIN_PERM = android.Manifest.permission.BLUETOOTH_ADMIN;
    public static final String BLUETOOTH_PRIVILEGED =
            android.Manifest.permission.BLUETOOTH_PRIVILEGED;
    private static final int MAX_DEVICES = 200;
    public boolean mVirtualCallStarted;
    private boolean mIsVoipLeaWarEnabled = false;
    private CallAudioMessageHandler mHandler;
    private BluetoothDevice mActiveDevice = null;
    private int mActiveProfile = 0;

    public static int UNKNOWPROFILE = 0;
    public static int HFP = 1;
    public static int LE_AUDIO_VOICE = 2;

    ServiceFactory mServiceFactory = new ServiceFactory();

        // CallAudio handler messages
    public static final int MESSAGE_VOIP_CALL_STARTED = 1;

    public static final int MESSAGE_ACTIVE_HFP_DEVICE_CHANGE = 2;

    private final class CallAudioMessageHandler extends Handler {
        private CallAudioMessageHandler(Looper looper) {
            super(looper);
        }

        @Override
        public void handleMessage(Message msg)
        {
            switch (msg.what) {
               case MESSAGE_VOIP_CALL_STARTED:
                   Log.d(TAG, "MESSAGE_VOIP_CALL_STARTED");
                   if (mVirtualCallStarted && mActiveDevice != null
	                   && mActiveProfile == LE_AUDIO_VOICE) {
                       if (getConnectionState(mActiveDevice) == BluetoothProfile.STATE_CONNECTED) {
                           broadcastAudioState(mActiveDevice,
                                               BluetoothHeadset.STATE_AUDIO_CONNECTING,
                                               BluetoothHeadset.STATE_AUDIO_CONNECTED);
                       }
                   }
                   break;
               case MESSAGE_ACTIVE_HFP_DEVICE_CHANGE:
                   Log.d(TAG, "MESSAGE_ACTIVE_HFP_DEVICE_CHANGE");
                   if (mActiveDevice != null) {
                       broadcastActiveDevice(mActiveDevice);
                   }
                   break;
               default:
                   break;
            }
        }
    }

    private CallAudio(Context context) {
        Log.d(TAG, "Initialization");
        mContext = context;
        mCallDevicesMap = new ConcurrentHashMap<String, CallDevice>();
        mAdapterService = AdapterService.getAdapterService();

        mIsVoipLeaWarEnabled =
                SystemProperties.getBoolean("persist.enable.bluetooth.voipleawar", false)
                && !Utils.isDualModeAudioEnabled();
        Log.i(TAG, "mIsVoipLeaWarEnabled: " + mIsVoipLeaWarEnabled);
        if ( mIsVoipLeaWarEnabled) {
            HandlerThread thread = new HandlerThread("CallAudioHandler");
            thread.start();
            Looper looper = thread.getLooper();
            mHandler = new CallAudioMessageHandler(looper);
        }
    }

    public static CallAudio init(Context context) {
        if(mCallAudio == null) {
            mCallAudio = new CallAudio(context);
        }
        return mCallAudio;
    }

    public static CallAudio get() {
        return mCallAudio;
    }

    protected void cleanup() {
        Log.d(TAG, " cleanup ");
        mCallDevicesMap.clear();
        mActiveDevice= null;
        mActiveProfile = UNKNOWPROFILE;
    }

    public boolean isVirtualCallStarted() {
        Log.d(TAG, "isVirtualCallStarted " + mVirtualCallStarted);
        return mVirtualCallStarted;
    }

    public boolean isVoipLeaWarEnabled() {
        Log.d(TAG, "isVoipLeaWarEnabled " + mIsVoipLeaWarEnabled);
        return mIsVoipLeaWarEnabled;
    }

    @RequiresPermission(android.Manifest.permission.MODIFY_PHONE_STATE)
    public boolean startScoUsingVirtualVoiceCall() {
        Log.i(TAG, "startScoUsingVirtuallCall, mActiveDevice: " + mActiveDevice +
                   ", mActiveProfile:" + mActiveProfile);

        HeadsetService headsetService = mServiceFactory.getHeadsetService();
        if (isVoipLeaWarEnabled() && mActiveDevice != null && mActiveProfile == LE_AUDIO_VOICE) {
            Log.i(TAG, "start VoIP call over LE AUDIO.");

            if (headsetService != null && headsetService.isScoOrCallActive()) {
                Log.w(TAG, "startScoUsingVirtuallCall: telecom call is ongoing, return false directly.");
                return false;
            }

            if (getConnectionState(mActiveDevice) != BluetoothProfile.STATE_CONNECTED) {
                Log.i(TAG, "startScoUsingVirtuallCall, active device not connected, return false directly");
                return false;
            }

            if (mVirtualCallStarted) {
                Log.w(TAG, "startScoUsingVirtualVoiceCall: VoIP call had been started.");
                return false;
            }
            mVirtualCallStarted = true;

            // fake SCO audio connecting event once receive startScoUsingVirtualCall request
            Log.i(TAG, "startScoUsingVirtuallCall, broadcast audio connected ");
            broadcastAudioState(mActiveDevice, BluetoothHeadset.STATE_AUDIO_DISCONNECTED,
                                BluetoothHeadset.STATE_AUDIO_CONNECTING);

            // fake SCO audio connected event if voice audio not connected in 1000ms
            Message msg = mHandler.obtainMessage(MESSAGE_VOIP_CALL_STARTED);
            mHandler.sendMessageDelayed(msg, 1000);
            return true;
        } else if (headsetService != null) {
            Log.i(TAG, "start VoIP call over HFP.");
            return headsetService.startScoUsingVirtualVoiceCall();
        }
        Log.e(TAG, "startScoUsingVirtualVoiceCall failed. Device: " + mActiveDevice);
        mVirtualCallStarted = false;
        return false;
    }

    @RequiresPermission(android.Manifest.permission.MODIFY_PHONE_STATE)
    public boolean stopScoUsingVirtualVoiceCall() {
        Log.d(TAG, "stopScoUsingVirtualVoiceCall, mActiveDevice: " + mActiveDevice +
                   ", mActiveProfile:" + mActiveProfile);

        HeadsetService headsetService = mServiceFactory.getHeadsetService();
        if(mActiveDevice == null) {
            if(mVirtualCallStarted) {
                mVirtualCallStarted = false;
                if(headsetService != null) {
                    headsetService.stopScoUsingVirtualVoiceCall();
                }
            }
            Log.e(TAG, "stopScoUsingVirtualVoiceCall failed. Active Device is null");
            return false;
        }

        if (mActiveDevice != null && mActiveProfile == HFP
                                  && headsetService.isVirtualCallStarted()) {
            Log.i(TAG, "VoIP call over HFP. ");
            return headsetService.stopScoUsingVirtualVoiceCall();
        }

        if (isVoipLeaWarEnabled() && isVirtualCallStarted()) {
            mVirtualCallStarted = false;

            if (mHandler.hasMessages(MESSAGE_VOIP_CALL_STARTED)) {
                Log.w(TAG, "stopScoUsingVirtualVoiceCall when start sco");
                mHandler.removeMessages(MESSAGE_VOIP_CALL_STARTED);
            }

            CallDevice mCallDevice = mCallDevicesMap.get(mActiveDevice.getAddress());
            if (mCallDevice != null) {
                // fake SCO audio disconnected event while stopScoUsingVirtualCall is invoked
                broadcastAudioState(mActiveDevice, mCallDevice.broadcastScoStatus,
                        BluetoothHeadset.STATE_AUDIO_DISCONNECTED);
                Log.i(TAG, "stopScoUsingVirtuallCall return true directly ");
                return true;
            } else {
                Log.i(TAG, "mCallDevice is null.");
                return false;
            }
        } else {
            if(headsetService != null) {
                mVirtualCallStarted = false;
                return headsetService.stopScoUsingVirtualVoiceCall();
            }
        }

        Log.e(TAG, "stopScoUsingVirtualVoiceCall failed. Device: " + mActiveDevice);
        return false;
    }

    public List<BluetoothDevice> getConnectedDevices() {
        Log.i(TAG, "getConnectedDevices: ");
        if(mCallDevicesMap.size() == 0) {
            Log.i(TAG, "no device is Connected:");
            return new ArrayList<>(0);
        }

        List<BluetoothDevice> connectedDevices = new ArrayList<>();
        for(CallDevice mCallDevice : mCallDevicesMap.values()) {
            if(mCallDevice.deviceConnStatus == BluetoothProfile.STATE_CONNECTED) {
                connectedDevices.add(mCallDevice.mDevice);
            }
        }
        Log.i(TAG, "ConnectedDevices: = " + connectedDevices.size());
        return connectedDevices;
    }

    public List<BluetoothDevice> getDevicesMatchingConnectionStates(int[] states) {
        mContext.enforceCallingOrSelfPermission(BLUETOOTH_PERM, "Need BLUETOOTH permission");

        Log.i(TAG, "getDevicesMatchingConnectionStates: ");
        List<BluetoothDevice> devices = new ArrayList<>();
        if (states == null) {
            return devices;
        }

        BluetoothDevice [] bondedDevices = null;
        bondedDevices = mAdapterService.getBondedDevices();
        if(bondedDevices == null) {
            return devices;
        }

        for (BluetoothDevice device : bondedDevices) {
            CallDevice mCallDevice;
            int state = BluetoothProfile.STATE_DISCONNECTED;

            mCallDevice = mCallDevicesMap.get(device.getAddress());
            if(mCallDevice != null)
                state = mCallDevice.deviceConnStatus;

            for(int s: states) {
                if(s == state) {
                    devices.add(device);
                    break;
                }
            }
        }
        return devices;
    }

    public int getConnectionState(BluetoothDevice device) {
        if(device == null)
            return BluetoothProfile.STATE_DISCONNECTED;
        CallDevice mCallDevice;
        mCallDevice = mCallDevicesMap.get(device.getAddress());
        if(mCallDevice != null) {
            Log.d(TAG, "getConnectionState: mCallDevice.deviceConnStatus " +
                    mCallDevice.deviceConnStatus);
            return mCallDevice.deviceConnStatus;
        }
        Log.d(TAG, "getConnectionState: return disconnected");
        return BluetoothProfile.STATE_DISCONNECTED;
    }

    private void broadcastConnStateChange(BluetoothDevice device, int fromState, int toState) {
         Log.d(TAG,"broadcastConnectionState " + device + ": " + fromState + "->" + toState);

         HeadsetService headsetService = mServiceFactory.getHeadsetService();
        if(headsetService == null) {
            Log.w(TAG,"broadcastConnectionState: HeadsetService not initialized. Return!");
            return;
        }

        if (device.equals(mActiveDevice) && fromState != toState &&
                                         toState == BluetoothProfile.STATE_DISCONNECTED) {
            if (mHandler.hasMessages(MESSAGE_VOIP_CALL_STARTED)) {
                mHandler.removeMessages(MESSAGE_VOIP_CALL_STARTED);
            }
            if (mHandler.hasMessages(MESSAGE_ACTIVE_HFP_DEVICE_CHANGE)) {
                mHandler.removeMessages(MESSAGE_ACTIVE_HFP_DEVICE_CHANGE);
            }
        }
        //AOSP specific
        headsetService.onConnectionStateChangedFromStateMachine(device, fromState, toState);

        Intent intent = new Intent(BluetoothHeadset.ACTION_CONNECTION_STATE_CHANGED);
        intent.putExtra(BluetoothProfile.EXTRA_PREVIOUS_STATE, fromState);
        intent.putExtra(BluetoothProfile.EXTRA_STATE, toState);
        intent.putExtra(BluetoothDevice.EXTRA_DEVICE, device);
        intent.addFlags(Intent.FLAG_RECEIVER_INCLUDE_BACKGROUND);
        headsetService.sendBroadcastAsUser(intent, UserHandle.ALL,
                BLUETOOTH_CONNECT, Utils.getTempBroadcastOptions().toBundle());
    }

    public void broadcastActiveDevice(BluetoothDevice device) {
        Log.d(TAG, "broadcast active device changed " + device);

        HeadsetService headsetService = mServiceFactory.getHeadsetService();
        if(headsetService == null) {
            Log.w(TAG,"broadcastConnectionState: HeadsetService not initialized. Return!");
            return;
        }

        synchronized (headsetService) {
            BluetoothStatsLog.write(
                BluetoothStatsLog.BLUETOOTH_ACTIVE_DEVICE_CHANGED,
                BluetoothProfile.HEADSET,
                mAdapterService.obfuscateAddress(device),
                mAdapterService.getMetricId(device));

            Intent intent = new Intent(BluetoothHeadset.ACTION_ACTIVE_DEVICE_CHANGED);
            intent.putExtra(BluetoothDevice.EXTRA_DEVICE, device);
            intent.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY_BEFORE_BOOT
                            | Intent.FLAG_RECEIVER_INCLUDE_BACKGROUND);
            headsetService.sendBroadcastAsUser(intent, UserHandle.ALL, BLUETOOTH_CONNECT,
                                               Utils.getTempBroadcastOptions().toBundle());
        }
    }

    private void broadcastAudioState(BluetoothDevice device, int fromState, int toState) {
        Log.d(TAG,"broadcastAudioState " + device + ": " + fromState + "->" + toState);

        HeadsetService headsetService = HeadsetService.getHeadsetService();
        if(headsetService == null) {
            Log.d(TAG,"broadcastAudioState: HeadsetService not initialized. Return!");
            return;
        }

        if (!mVirtualCallStarted && toState == BluetoothHeadset.STATE_AUDIO_CONNECTED) {
            Log.i(TAG, "broadcastAudioState(): ignore audio connected if VOIP is not started");
            return;
        }
        if (mActiveDevice != null && !device.equals(mActiveDevice)) {
            Log.d(TAG,"broadcastAudioState: non-active device. Return!");
            return;
        }
        CallDevice mCallDevice = mCallDevicesMap.get(device.getAddress());
        if (mCallDevice.broadcastScoStatus == toState) {
            Log.d(TAG,"broadcastAudioState: broadcastScoStatus is same. Return!");
            return;
        }

        mCallDevice.broadcastScoStatus = toState;
        Intent intent = new Intent(BluetoothHeadset.ACTION_AUDIO_STATE_CHANGED);
        intent.putExtra(BluetoothProfile.EXTRA_PREVIOUS_STATE, fromState);
        intent.putExtra(BluetoothProfile.EXTRA_STATE, toState);
        intent.putExtra(BluetoothDevice.EXTRA_DEVICE, device);
        headsetService.sendBroadcastAsUser(intent, UserHandle.ALL,
                BLUETOOTH_CONNECT, Utils.getTempBroadcastOptions().toBundle());
    }

    private List<BluetoothDevice> getNonIdleAudioDevices() {
        if(mCallDevicesMap.size() == 0) {
            return new ArrayList<>(0);
        }

        ArrayList<BluetoothDevice> devices = new ArrayList<>();
        for (CallDevice mCallDevice : mCallDevicesMap.values()) {
            if (mCallDevice.broadcastScoStatus != BluetoothHeadset.STATE_AUDIO_DISCONNECTED) {
                devices.add(mCallDevice.mDevice);
            }
        }
        return devices;
    }

    public boolean isAudioOn() {
        int numConnectedAudioDevices = getNonIdleAudioDevices().size();
        Log.d(TAG," isAudioOn: The number of audio connected devices "
                      + numConnectedAudioDevices);
             return numConnectedAudioDevices > 0;
    }

    public BluetoothDevice getActiveDevice() {
        Log.d(TAG, "getActiveDevice, mActiveDevice:" + mActiveDevice);
        return mActiveDevice;
    }

    public int getActiveProfile() {
        Log.d(TAG, "getActiveProfile, active device: " + mActiveDevice +
                   " active profile:" + mActiveProfile);
        if (mActiveDevice != null)
            return mActiveProfile;
        else
            return UNKNOWPROFILE;
    }

    public void updateActiveDevice(BluetoothDevice device, int profile) {
        Log.d(TAG,"updateActiveDevice, current active device: " + mActiveDevice +
                  ", current active profile: " + mActiveProfile + ", -> device: " +
                  device + ", profile: " + profile);

        if (profile == UNKNOWPROFILE) {
            mActiveDevice = null;
            mActiveProfile = UNKNOWPROFILE;
            return;
        }
        if (device != null && mActiveDevice != null &&
            device.equals(mActiveDevice) && mActiveProfile == profile) {
            Log.d(TAG,"updateActiveDevice, same device & profile.");
            return;
        }
        if (device != null) {
            HeadsetService headsetService = mServiceFactory.getHeadsetService();
            if (headsetService != null && !headsetService.isVirtualCallStarted() &&
                                          (headsetService.isInCall() ||
                                           (headsetService.isRinging() &&
                                            headsetService.isInbandRingingEnabled()))) {
                broadcastActiveDevice(device);
            } else {
                if (mHandler.hasMessages(MESSAGE_ACTIVE_HFP_DEVICE_CHANGE)) {
                    Log.d(TAG,"updateActiveDevice, remove MESSAGE_ACTIVE_HFP_DEVICE_CHANGE first.");
                    mHandler.removeMessages(MESSAGE_ACTIVE_HFP_DEVICE_CHANGE);
                }
                if (mActiveDevice != null && (mActiveProfile != profile ||
                                              mActiveProfile == HFP)) {
                    Log.d(TAG,"updateActiveDevice, broadcast previous hfp device to null.");
                    stopScoUsingVirtualVoiceCall();
                    broadcastActiveDevice(null);
                }
                Message msg = mHandler.obtainMessage(MESSAGE_ACTIVE_HFP_DEVICE_CHANGE);
                mHandler.sendMessageDelayed(msg, 3000);
            }
            mActiveDevice = device;
            mActiveProfile = profile;
        } else if (mActiveProfile == profile){
            Log.d(TAG,"updateActiveDevice, update device to null.");
            if (mHandler.hasMessages(MESSAGE_ACTIVE_HFP_DEVICE_CHANGE)) {
                mHandler.removeMessages(MESSAGE_ACTIVE_HFP_DEVICE_CHANGE);
            }
            broadcastActiveDevice(null);
            mActiveDevice = device;
        }
        return;
    }

    //Todo: CSIP use-case and profiles siwtch for not dual mode in same device
    public synchronized void onConnStateChange(BluetoothDevice device, Integer state, Integer profile) {
        int prevState;
        Log.d(TAG, "onConnStateChange: profile: " + profile + " state: "
                                                  + state + " for device " + device);
        if(device == null)
            return;

        CallDevice mCallDevice = mCallDevicesMap.get(device.getAddress());
        Log.d(TAG, "onConnStateChange: mCallDevice:" + mCallDevice);

        if(mCallDevice == null) {
            if (state == BluetoothProfile.STATE_DISCONNECTED)
                return;
            if (mCallDevicesMap.size() >= MAX_DEVICES) {
                Log.w(TAG, "onConnStateChange: Reached max devices.");
                return;
            }
            mCallDevice = new CallDevice(device, profile, state);
            mCallDevicesMap.put(device.getAddress(), mCallDevice);
            broadcastConnStateChange(device, BluetoothProfile.STATE_DISCONNECTED, state);
            return;
        }

        int profileIndex = mCallDevice.getProfileIndex(profile);
        prevState = mCallDevice.deviceConnStatus;
        Log.d(TAG, "onConnStateChange: prevState: " + prevState);
        mCallDevice.profileConnStatus[profileIndex] = state;

        int otherProfileConnectionState = mCallDevice.profileConnStatus[(profileIndex+1)%2];
        Log.w(TAG, " otherProfileConnectionState: " + otherProfileConnectionState);

        LeAudioService leAudioService = mServiceFactory.getLeAudioService();
        if (leAudioService != null) {
            int currentGroupId = leAudioService.getGroupId(mActiveDevice);
            int groupId = leAudioService.getGroupId(device);
            if (currentGroupId == groupId
                && leAudioService.getConnectedPeerDevices(groupId).isEmpty()
                && mActiveProfile == LE_AUDIO_VOICE && isVirtualCallStarted()
                && (state == BluetoothProfile.STATE_DISCONNECTING
                    || state == BluetoothProfile.STATE_DISCONNECTED)) {
                Log.d(TAG, "onConnStateChange:"
                           + " all group members got disconnecting/disconnected");
                stopScoUsingVirtualVoiceCall();
            }
        }

        //Reset HFP/LE device and active profile when disconnecting/disconnected
        if (mActiveDevice != null && mActiveDevice.equals(device)
            && prevState == BluetoothProfile.STATE_CONNECTED
            && (state == BluetoothProfile.STATE_DISCONNECTING
                || state == BluetoothProfile.STATE_DISCONNECTED)) {
            Log.d(TAG, "onConnStateChange: mActiveDevice disconnecting/disconnected");
            updateActiveDevice(null, profile);
        }

        switch(otherProfileConnectionState) {
        /*Send Broadcast based on state of other profile*/
            case BluetoothProfile.STATE_DISCONNECTED:
                broadcastConnStateChange(device, prevState, state);
                mCallDevice.deviceConnStatus = state;
                break;
            case BluetoothProfile.STATE_DISCONNECTING:
                if(state == BluetoothProfile.STATE_CONNECTING) {
                    Log.w(TAG, "onConnStateChange"
                               + " Profile is connecting while otherProfile is disconnecting ");
                    mCallDevice.profileSwitching = true;
                    mCallDevice.profileConnStatus[(profileIndex+1)%2]
                                        = BluetoothProfile.STATE_DISCONNECTED;
                    broadcastConnStateChange(device, prevState, BluetoothProfile.STATE_DISCONNECTED);

                    mCallDevice.profileConnStatus[profileIndex] = state;
                    mCallDevice.deviceConnStatus = state;
                    broadcastConnStateChange(device, BluetoothProfile.STATE_DISCONNECTED, state);
                } else if (!mCallDevice.profileSwitching){
                    broadcastConnStateChange(device, prevState, state);
                    mCallDevice.deviceConnStatus = state;
                }
                break;
            case BluetoothProfile.STATE_CONNECTING:
                if (state == BluetoothProfile.STATE_DISCONNECTING) {
                    Log.w(TAG, "onConnStateChange Profile is disconnecting while otherProfile is connecting ");
                    mCallDevice.profileSwitching = true;
                    broadcastConnStateChange(device, prevState, state);

                    mCallDevice.profileConnStatus[profileIndex] = BluetoothProfile.STATE_DISCONNECTED;
                    mCallDevice.deviceConnStatus = BluetoothProfile.STATE_DISCONNECTED;
                    broadcastConnStateChange(device, state, BluetoothProfile.STATE_DISCONNECTED);
                } else if (!mCallDevice.profileSwitching){
                    broadcastConnStateChange(device, prevState, state);
                    mCallDevice.deviceConnStatus = state;
                }
                break;
            case BluetoothProfile.STATE_CONNECTED:
                //Is it possible for non-dual mode ?
                break;
        }

        Log.d(TAG, "onConnStateChange: deviceConnStatus:" + mCallDevice.deviceConnStatus
                   + ", profileSwitching:" + mCallDevice.profileSwitching);
        if (state == BluetoothProfile.STATE_CONNECTED && mCallDevice.profileSwitching) {
            mCallDevice.profileSwitching = false;
        }
        return;
    }

   class CallDevice {
        BluetoothDevice mDevice;
        int[] profileConnStatus = new int[2];
        int deviceConnStatus;
        int scoStatus;
        int broadcastScoStatus;
        boolean profileSwitching;
        //int pendingUpdateDiscState;

        public static final int SCO_STREAM = 0;
        public static final int LE_STREAM = 1;

        CallDevice(BluetoothDevice device, int profile, int state) {
            mDevice = device;
            if(profile == HFP) {
                profileConnStatus[SCO_STREAM] = state;
                profileConnStatus[LE_STREAM] = BluetoothProfile.STATE_DISCONNECTED;
            } else {
                profileConnStatus[LE_STREAM] = state;
                profileConnStatus[SCO_STREAM] = BluetoothProfile.STATE_DISCONNECTED;
            }
            deviceConnStatus = state;
            scoStatus = BluetoothHeadset.STATE_AUDIO_DISCONNECTED;;
            broadcastScoStatus = BluetoothHeadset.STATE_AUDIO_DISCONNECTED;;
            profileSwitching = false;
            //pendingUpdateDiscState = -1;
        }

        CallDevice(BluetoothDevice device, int profile) {
            this(device, profile, BluetoothProfile.STATE_DISCONNECTED);
        }

        public int getProfileIndex(int profile) {
            if(profile == HFP)
                return SCO_STREAM;
            else
                return LE_STREAM;
        }
   }
}
