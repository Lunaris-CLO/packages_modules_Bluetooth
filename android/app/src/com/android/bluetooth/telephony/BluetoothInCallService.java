/*
 * Copyright (C) 2020 The Android Open Source Project
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

 * ​​​​​Changes from Qualcomm Innovation Center are provided under the following license:
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 *  SPDX-License-Identifier: BSD-3-Clause-Clear
 */

package com.android.bluetooth.telephony;

import android.annotation.NonNull;
import android.annotation.RequiresPermission;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothHeadset;
import android.bluetooth.BluetoothLeCall;
import android.bluetooth.BluetoothLeCallControl;
import android.bluetooth.BluetoothProfile;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.telecom.BluetoothCallQualityReport;
import android.telecom.Call;
import android.telecom.CallAudioState;
import android.telecom.Connection;
import android.telecom.DisconnectCause;
import android.telecom.InCallService;
import android.telecom.PhoneAccount;
import android.telecom.PhoneAccountHandle;
import android.telecom.TelecomManager;
import android.telecom.VideoProfile;
import android.telephony.PhoneNumberUtils;
import android.telephony.TelephonyManager;
import android.text.TextUtils;
import android.os.SystemProperties;
import android.util.Log;

import android.os.Handler;
import android.os.Message;
import android.os.Bundle;
import android.os.HandlerThread;
import android.os.Looper;


import androidx.annotation.VisibleForTesting;

import com.android.bluetooth.hfp.BluetoothHeadsetProxy;
import com.android.bluetooth.tbs.BluetoothLeCallControlProxy;
import com.android.bluetooth.le_audio.LeAudioService;
import com.android.bluetooth.btservice.ServiceFactory;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Objects;
import java.util.Queue;
import java.util.SortedMap;
import java.util.SortedSet;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Used to receive updates about calls from the Telecom component. This service is bound to Telecom
 * while there exist calls which potentially require UI. This includes ringing (incoming), dialing
 * (outgoing), and active calls. When the last BluetoothCall is disconnected, Telecom will unbind to
 * the service triggering InCallActivity (via CallList) to finish soon after.
 */
public class BluetoothInCallService extends InCallService {

    private static final String TAG = "BluetoothInCallService";
    // match up with bthf_call_state_t of bt_hf.h
    private static final int CALL_STATE_ACTIVE = 0;
    private static final int CALL_STATE_HELD = 1;
    private static final int CALL_STATE_DIALING = 2;
    private static final int CALL_STATE_ALERTING = 3;
    private static final int CALL_STATE_INCOMING = 4;
    private static final int CALL_STATE_WAITING = 5;
    private static final int CALL_STATE_IDLE = 6;
    private static final int CALL_STATE_DISCONNECTED = 7;

    private InCallHandler mHandler = null;
    private HandlerThread mHandlerThread;

    private static final int OUTGOING_INCOMING = 0;
    private static final int MULTI_INCOMING = 1;
    private static final int MULTI_HELD = 2;
    private static final int OUTGOING_INCOMING_DISCONNECTION = 3;
    private static final int MULTI_RINGING_DISCONNECTION = 4;
    private static final int OUTGOING_DISCONNECTION = 5;
    private static final int MULTI_HELD_ACTIVE = 6;
    private static final int DSDS_EVENT = 7;

    public int mLastBtHeadsetState = CALL_STATE_IDLE;

    // match up with bthf_call_state_t of bt_hf.h
    // Terminate all held or set UDUB("busy") to a waiting call
    private static final int CHLD_TYPE_RELEASEHELD = 0;
    // Terminate all active calls and accepts a waiting/held call
    private static final int CHLD_TYPE_RELEASEACTIVE_ACCEPTHELD = 1;
    // Hold all active calls and accepts a waiting/held call
    private static final int CHLD_TYPE_HOLDACTIVE_ACCEPTHELD = 2;
    // Add all held calls to a conference
    private static final int CHLD_TYPE_ADDHELDTOCONF = 3;

    // Indicates that no BluetoothCall is ringing
    private static final int DEFAULT_RINGING_ADDRESS_TYPE = 128;

    private int mNumActiveCalls = 0;
    private int mNumHeldCalls = 0;
    private int mNumChildrenOfActiveCall = 0;
    private int mBluetoothCallState = CALL_STATE_IDLE;
    private String mRingingAddress = "";
    private int mRingingAddressType = DEFAULT_RINGING_ADDRESS_TYPE;
    private BluetoothCall mOldHeldCall = null;
    private boolean mHeadsetUpdatedRecently = false;
    private boolean mIsDisconnectedTonePlaying = false;

    //count variables for held and active calls for dsda
    public int mDsdaTotalcalls = 0;
    public int mDsdaActiveCalls = 0;
    public int mDsdaIncomingCalls = 0;
    public int mDsDaHeldCalls = 0;
    public int mDsDaOutgoingCalls = 0;
    public int mDsDaCallState = CALL_STATE_IDLE;
    public String mDsDaRingingAddress = null;
    public int mDsDaRingingAddressType = DEFAULT_RINGING_ADDRESS_TYPE;
    public String mDsDaRingingName = null;
    private static final int DELAY_DSDA_CALL_INDICATORS = 60;

    //flag for newCall Not updated
    private int mDsDaTwoIncomingCallsFlag = 0;
    private int mdsDaSelectPhoneAccountFlag = 0;
    private int mCallSwapPending = 0;
    private int mDelayOutgoingUpdate = 0;
    private int conferenceCallInitiated = 0;
    public int mFirstIncomingCallId = -1;
    public int mSecondIncomingCallId = -1;
    public int mSelectPhoneAccountId = -1;
    public int mDialingCallId = -1;

    public static int UNKNOWN = 0;
    public static int DSDS = 1;
    public static int PSEUDO_DSDA = 2;
    public static int DSDA = 3;
    public int currentMode = DSDA;
    public boolean dsdsTransition = false;

    @VisibleForTesting
    boolean mIsTerminatedByClient = false;

    private static final Object LOCK = new Object();

    private final ServiceFactory mFactory = new ServiceFactory();

    private LeAudioService mLeAudioService;

    @VisibleForTesting BluetoothHeadsetProxy mBluetoothHeadset;

    @VisibleForTesting BluetoothLeCallControlProxy mBluetoothLeCallControl;
    private ExecutorService mExecutor;

    @VisibleForTesting public TelephonyManager mTelephonyManager;

    @VisibleForTesting public TelecomManager mTelecomManager;

    @VisibleForTesting
    public final HashMap<Integer, CallStateCallback> mCallbacks = new HashMap<>();

    @VisibleForTesting
    public final HashMap<Integer, BluetoothCall> mBluetoothCallHashMap = new HashMap<>();

    private final HashMap<Integer, BluetoothCall> mBluetoothConferenceCallInference =
            new HashMap<>();

    // A queue record the removal order of bluetooth calls
    private final Queue<Integer> mBluetoothCallQueue = new ArrayDeque<>();

    private static BluetoothInCallService sInstance = null;

    public CallInfo mCallInfo = new CallInfo();

    protected boolean mOnCreateCalled = false;

    private int mMaxNumberOfCalls = 0;

    private boolean mEnableDsdaMode = false;

    private boolean mDsDaEventsHadlingInProgress = false;

    private static final String ENABLE_DSDA_SUPPORT =
          "persist.bluetooth.init.dsda.support";

    private final String EXTRAS_MSIM_VOICE_CAPABILITY = "MsimVoiceCapability";

    private static final String ACTION_MSIM_VOICE_CAPABILITY_CHANGED =
        "org.codeaurora.intent.action.MSIM_VOICE_CAPABILITY_CHANGED";

    private static final String PROPERTY_MULTISIM_VOICE_CAPABILITY =
        "ril.multisim.voice_capability";

    /**
     * Listens to connections and disconnections of bluetooth headsets. We need to save the current
     * bluetooth headset so that we know where to send BluetoothCall updates.
     */
    @VisibleForTesting
    public BluetoothProfile.ServiceListener mProfileListener =
            new BluetoothProfile.ServiceListener() {
                @Override
                public void onServiceConnected(int profile, BluetoothProfile proxy) {
                    Log.d(TAG, "onServiceConnected: profile: " + profile);
                    synchronized (LOCK) {
                        if (profile == BluetoothProfile.HEADSET) {
                            setBluetoothHeadset(
                                    new BluetoothHeadsetProxy((BluetoothHeadset) proxy));
                            if (!mEnableDsdaMode) {
                              updateHeadsetWithCallState(true /* force */);
                            } else {
                              updateHeadsetWithDSDACallState(true, DSDS_EVENT);
                            }
                        } else {
                            setBluetoothLeCallControl(
                                    new BluetoothLeCallControlProxy(
                                            (BluetoothLeCallControl) proxy));
                            sendTbsCurrentCallsList();
                        }
                    }
                }

                @Override
                public void onServiceDisconnected(int profile) {
                    Log.d(TAG, "onServiceDisconnected: profile: " + profile);
                    synchronized (LOCK) {
                        if (profile == BluetoothProfile.HEADSET) {
                            setBluetoothHeadset(null);
                        } else {
                            setBluetoothLeCallControl(null);
                        }
                    }
                }
            };

    public class BluetoothAdapterReceiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context context, Intent intent) {
            synchronized (LOCK) {
                if (intent.getAction() != BluetoothAdapter.ACTION_STATE_CHANGED) {
                    Log.w(TAG, "BluetoothAdapterReceiver: Intent action " + intent.getAction());
                    return;
                }
                int state =
                        intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR);
                Log.d(TAG, "Bluetooth Adapter state: " + state);
                if (state == BluetoothAdapter.STATE_ON) {
                    queryPhoneState();
                } else if (state == BluetoothAdapter.STATE_TURNING_OFF) {
                    clear();
                }
               String action = intent.getAction();
                if (action.equals(ACTION_MSIM_VOICE_CAPABILITY_CHANGED)) {
                    Log.d(TAG, "ACTION_MSIM_VOICE_CAPABILITY_CHANGED intent received");
                    currentMode = intent.getIntExtra(EXTRAS_MSIM_VOICE_CAPABILITY, currentMode);
                    if (mTelephonyManager != null) {
                        if (currentMode == DSDS) {
                            Log.w(TAG, "In DSDS mode");
                            for (int i=0; i < mTelephonyManager.getActiveModemCount(); i++){
                                int subId = mTelephonyManager.getSubscriptionId();
                                TelephonyManager tm = mTelephonyManager.createForSubscriptionId(subId);
                                if (tm.getCallStateForSubscription() == CALL_STATE_IDLE) {
                                    dsdsTransition = true;
                                }
                            }
                            if(dsdsTransition) {
                                Log.w(TAG, "In DSDS transition mode");
                            }
                            else {
                                Log.w(TAG, "Not in DSDS transition mode");
                            }
                        }
                        else if (currentMode == PSEUDO_DSDA || currentMode == DSDA) {
                            Log.w(TAG, "In DSDA mode");
                        }
                    }//null check
                    else {
                        Log.e(TAG, "mTelephonyManager is null when "
                                    +"ACTION_MSIM_VOICE_CAPABILITY_CHANGED intent received");
                    }
                }
            }
        }
    };

    /** Receives events for global state changes of the bluetooth adapter. */
    // TODO: The code is moved from Telecom stack. Since we're running in the BT process itself,
    // we may be able to simplify this in a future patch.
    @VisibleForTesting
    public BluetoothAdapterReceiver mBluetoothAdapterReceiver;
    public BluetoothAdapterReceiver mVoiceCapabilityChangeReceiver = null;

    @VisibleForTesting
    public class CallStateCallback extends Call.Callback {
        public int mLastState;

        public CallStateCallback(int initialState) {
            mLastState = initialState;
        }

        public int getLastState() {
            return mLastState;
        }

        public void onStateChanged(BluetoothCall call, int state) {
            if (mCallInfo.isNullCall(call)) {
                return;
            }
            if (call.isExternalCall()) {
                return;
            }
            if (state == Call.STATE_DISCONNECTING) {
                mLastState = state;
                return;
            }

            Integer tbsCallState = getTbsCallState(call);
            if (mBluetoothLeCallControl != null && tbsCallState != null) {
                mBluetoothLeCallControl.onCallStateChanged(call.getTbsCallId(), tbsCallState);
            }

            // If a BluetoothCall is being put on hold because of a new connecting call, ignore the
            // CONNECTING since the BT state update needs to send out the numHeld = 1 + dialing
            // state atomically.
            // When the BluetoothCall later transitions to DIALING/DISCONNECTED we will then
            // send out the aggregated update.
            if (getLastState() == Call.STATE_ACTIVE && state == Call.STATE_HOLDING) {
                for (BluetoothCall otherCall : mCallInfo.getBluetoothCalls()) {
                    if (otherCall.getState() == Call.STATE_CONNECTING) {
                        mLastState = state;
                        return;
                    }
                }
            }

            // To have an active BluetoothCall and another dialing at the same time is an invalid BT
            // state. We can assume that the active BluetoothCall will be automatically held
            // which will send another update at which point we will be in the right state.
            BluetoothCall activeCall = mCallInfo.getActiveCall();
            if (!mCallInfo.isNullCall(activeCall)
                    && getLastState() == Call.STATE_CONNECTING
                    && (state == Call.STATE_DIALING || state == Call.STATE_PULLING_CALL)) {
                mLastState = state;
                return;
            }
            mLastState = state;
            if (!mEnableDsdaMode) {
               updateHeadsetWithCallState(false /* force */);
            } else {
              processOnStateChanged(call);
            }
        }

        @Override
        public void onStateChanged(Call call, int state) {
            super.onStateChanged(call, state);
            onStateChanged(getBluetoothCallById(System.identityHashCode(call)), state);
        }

        public void onDetailsChanged(BluetoothCall call, Call.Details details) {
            if (mCallInfo.isNullCall(call)) {
                return;
            }
            if (call.isExternalCall()) {
                onCallRemoved(call, false /* forceRemoveCallback */);
            } else {
                onCallAdded(call);
            }
        }

        @Override
        public void onDetailsChanged(Call call, Call.Details details) {
            super.onDetailsChanged(call, details);
            onDetailsChanged(getBluetoothCallById(System.identityHashCode(call)), details);
        }

        public void onParentChanged(BluetoothCall call) {
            if (mCallInfo.isNullCall(call) || call.isExternalCall()) {
                Log.w(TAG, "null call or external call");
                return;
            }
            if (call.getParentId() != null) {
                // If this BluetoothCall is newly conferenced, ignore the callback.
                // We only care about the one sent for the parent conference call.
                Log.d(
                        TAG,
                        "Ignoring onIsConferenceChanged from child BluetoothCall with new parent");
                return;
            }
            updateHeadsetWithCallState(false /* force */);
        }

        @Override
        public void onParentChanged(Call call, Call parent) {
            super.onParentChanged(call, parent);
            onParentChanged(getBluetoothCallById(System.identityHashCode(call)));
        }

        public void onChildrenChanged(BluetoothCall call, List<BluetoothCall> children) {
            if (mCallInfo.isNullCall(call) || call.isExternalCall()) {
                Log.w(TAG, "null call or external call");
                return;
            }
            if (call.getChildrenIds().size() == 1) {
                // If this is a parent BluetoothCall with only one child,
                // ignore the callback as well since the minimum number of child calls to
                // start a conference BluetoothCall is 2. We expect this to be called again
                // when the parent BluetoothCall has another child BluetoothCall added.
                Log.d(TAG, "Ignoring onIsConferenceChanged from parent with only one child call");
                return;
            }
            updateHeadsetWithCallState(false /* force */);
        }

        @Override
        public void onChildrenChanged(Call call, List<Call> children) {
            super.onChildrenChanged(call, children);
            onChildrenChanged(
                    getBluetoothCallById(System.identityHashCode(call)),
                    getBluetoothCallsByIds(BluetoothCall.getIds(children)));
        }
    }

    @Override
    public IBinder onBind(Intent intent) {
        Log.i(TAG, "onBind. Intent: " + intent);
        IBinder binder = super.onBind(intent);
        return binder;
    }

    @Override
    public boolean onUnbind(Intent intent) {
        Log.i(TAG, "onUnbind. Intent: " + intent);
        return super.onUnbind(intent);
    }

    public BluetoothInCallService() {
        Log.i(TAG, "BluetoothInCallService is created");
        sInstance = this;
        mExecutor = Executors.newSingleThreadExecutor();
    }

    public static BluetoothInCallService getInstance() {
        return sInstance;
    }

    class InCallHandler extends Handler {
        InCallHandler(Looper looper) {
            super(looper);
        }
        @Override
        public void handleMessage(Message msg) {
            Log.d(TAG, "handleMessage(): msg.what: " + msg.what);
            int numRingingCalls       = mCallInfo.getNumRingingCalls();
            int numHeldCalls          = mCallInfo.getNumHeldCalls();
            BluetoothCall activeCall  = mCallInfo.getActiveCall();
            int numActiveCalls        = mCallInfo.isNullCall(activeCall) ? 0 : 1;
            BluetoothCall dailingCall = mCallInfo.getOutgoingCall();
            int numOutgoingCalls      = mCallInfo.isNullCall(dailingCall) ? 0 : 1;
            int updateheldCalls       = 0;
            if (numHeldCalls == 0)
               updateheldCalls = 0;
            else
               updateheldCalls = 1;

            mDsDaEventsHadlingInProgress = true;
            switch (msg.what) {
             case OUTGOING_INCOMING:
                 Log.d(TAG, "OUTGOING_INCOMING event");
                 if (numOutgoingCalls != mDsDaOutgoingCalls) {
                   //Outgoing calls Changed
                   BluetoothCall ringingCall = mCallInfo.getRingingOrSimulatedRingingCall();
                   getDSDARingingAddress(ringingCall);
                   if ((numOutgoingCalls == 0) && (mDsDaOutgoingCalls == 1)) {
                     if (numRingingCalls > 0) {
                       //Outgoing became active and incoming call is present
                       //Send the active call update and then send waiting call update
                       if (mBluetoothHeadset != null) {
                         mBluetoothHeadset.phoneStateChanged(
                            numActiveCalls,
                            updateheldCalls,
                            CALL_STATE_IDLE,
                            mDsDaRingingAddress,
                            mDsDaRingingAddressType,
                            mDsDaRingingName);
                            mDsDaOutgoingCalls--;
                            mDsdaActiveCalls++;
                            handlerThreadSleep(DELAY_DSDA_CALL_INDICATORS);
                         mBluetoothHeadset.phoneStateChanged(
                            numActiveCalls,
                            updateheldCalls,
                            CALL_STATE_INCOMING,
                            mDsDaRingingAddress,
                            mDsDaRingingAddressType,
                            mDsDaRingingName);
                       }
                       mLastBtHeadsetState = CALL_STATE_INCOMING;
                    }
                   }
                 } else if (mDsdaIncomingCalls != numRingingCalls) {
                   //Incoming calls changed
                   //incoming call became active. Outgoing call should be disconnected
                   //if Outgoing disconnected event has not yet received, fake the params
                   BluetoothCall ringingCall = getBluetoothCallById(mFirstIncomingCallId);
                   if (mBluetoothHeadset != null) {
                     getDSDARingingAddress(null);
                     mBluetoothHeadset.phoneStateChanged(
                        0,
                        updateheldCalls,
                        CALL_STATE_IDLE,
                        mDsDaRingingAddress,
                        mDsDaRingingAddressType,
                        mDsDaRingingName);
                     //sending the incoming setup
                     getDSDARingingAddress(ringingCall);
                     handlerThreadSleep(DELAY_DSDA_CALL_INDICATORS);
                     mBluetoothHeadset.phoneStateChanged(
                        0,
                        updateheldCalls,
                        CALL_STATE_INCOMING,
                        mDsDaRingingAddress,
                        mDsDaRingingAddressType,
                        mDsDaRingingName);
                      //sending active update
                      handlerThreadSleep(DELAY_DSDA_CALL_INDICATORS);
                      getDSDARingingAddress(null);
                      mBluetoothHeadset.phoneStateChanged(
                         numActiveCalls,
                         updateheldCalls,
                         CALL_STATE_IDLE,
                         mDsDaRingingAddress,
                         mDsDaRingingAddressType,
                         mDsDaRingingAddress);
                      mDsdaIncomingCalls--;
                      mLastBtHeadsetState = CALL_STATE_IDLE;
                   }
                 }
             break;
             case MULTI_INCOMING:
                 Log.d(TAG, "MULTI_INCOMING event");
                 if ((numRingingCalls == 1) && (mDsdaIncomingCalls == 2)) {
                   Log.d(TAG, "multiple ringing calls, 1 ringing moved to active");
                   if ((numActiveCalls == 1) && (mDsdaActiveCalls == 0)) {
                     if (mFirstIncomingCallId == activeCall.getId()) {
                       BluetoothCall ringingCall = getBluetoothCallById(mSecondIncomingCallId);
                       //1st call moved to active
                       //2nd call need to be updated as waiting call
                       if (mBluetoothHeadset != null) {
                         getDSDARingingAddress(null);
                         mBluetoothHeadset.phoneStateChanged(
                            numActiveCalls,
                            updateheldCalls,
                            CALL_STATE_IDLE,
                            mDsDaRingingAddress,
                            mDsDaRingingAddressType,
                            mDsDaRingingName);
                         handlerThreadSleep(DELAY_DSDA_CALL_INDICATORS);
                         mFirstIncomingCallId = mSecondIncomingCallId;
                         mSecondIncomingCallId = -1;
                         mDsdaIncomingCalls--;
                         mDsDaTwoIncomingCallsFlag = 0;
                         //waiting call of 2nd incoming call
                         getDSDARingingAddress(ringingCall);
                         mBluetoothHeadset.phoneStateChanged(
                            numActiveCalls,
                            updateheldCalls,
                            CALL_STATE_INCOMING,
                            mDsDaRingingAddress,
                            mDsDaRingingAddressType,
                            mDsDaRingingName);
                         mLastBtHeadsetState = CALL_STATE_INCOMING;
                       }
                     } else if (mSecondIncomingCallId == activeCall.getId()) {
                       Log.d(TAG, "2nd incoming call became active");
                       BluetoothCall ringingCall = getBluetoothCallById(mSecondIncomingCallId);
                       mDsDaTwoIncomingCallsFlag = 0;
                       if (mBluetoothHeadset != null) {
                         Log.d(TAG, "1st setup end");
                         getDSDARingingAddress(null);
                         mBluetoothHeadset.phoneStateChanged(
                            0, //No Active calls
                            updateheldCalls,
                            CALL_STATE_IDLE,
                            mDsDaRingingAddress,
                            mDsDaRingingAddressType,
                            mDsDaRingingName);
                         handlerThreadSleep(DELAY_DSDA_CALL_INDICATORS);
                         //incoming call of 2nd incoming call
                         Log.d(TAG, "2nd setup");
                         getDSDARingingAddress(ringingCall);
                         mBluetoothHeadset.phoneStateChanged(
                            0, //No active calls
                            updateheldCalls,
                            CALL_STATE_INCOMING,
                            mDsDaRingingAddress,
                            mDsDaRingingAddressType,
                            mDsDaRingingName);
                         handlerThreadSleep(DELAY_DSDA_CALL_INDICATORS);
                         Log.d(TAG, "2nd active");
                         mDsdaActiveCalls = 1;
                         getDSDARingingAddress(null);
                         mBluetoothHeadset.phoneStateChanged(
                            numActiveCalls,
                            updateheldCalls,
                            CALL_STATE_IDLE,
                            mDsDaRingingAddress,
                            mDsDaRingingAddressType,
                            mDsDaRingingName);
                         Log.d(TAG, "waiting 1st");
                         ringingCall = getBluetoothCallById(mFirstIncomingCallId);
                         getDSDARingingAddress(ringingCall);
                         handlerThreadSleep(DELAY_DSDA_CALL_INDICATORS);
                         mBluetoothHeadset.phoneStateChanged(
                            numActiveCalls,
                            updateheldCalls,
                            CALL_STATE_INCOMING,
                            mDsDaRingingAddress,
                            mDsDaRingingAddressType,
                            mDsDaRingingName);
                          mLastBtHeadsetState = CALL_STATE_INCOMING;
                            mDsdaIncomingCalls--;
                            mSecondIncomingCallId = -1;
                       }
                     }
                   } else if ((mNumActiveCalls ==0)  && (mDsdaActiveCalls == 0)) {
                     //one of the 2 ringing calls would have been disconnected
                     BluetoothCall ringingCall = mCallInfo.getRingingOrSimulatedRingingCall();
                     if (ringingCall.getId() == mSecondIncomingCallId) {
                       if (mBluetoothHeadset != null) {
                         getDSDARingingAddress(null);
                         mBluetoothHeadset.phoneStateChanged(
                            0,
                            updateheldCalls,
                            CALL_STATE_IDLE,
                            mDsDaRingingAddress,
                            mDsDaRingingAddressType,
                            mDsDaRingingName);
                         handlerThreadSleep(DELAY_DSDA_CALL_INDICATORS);
                         //incoming call of 2nd incoming call
                         getDSDARingingAddress(ringingCall);
                         mBluetoothHeadset.phoneStateChanged(
                            0,
                            updateheldCalls,
                            CALL_STATE_INCOMING,
                            mDsDaRingingAddress,
                            mDsDaRingingAddressType,
                            mDsDaRingingName);
                         mLastBtHeadsetState = CALL_STATE_INCOMING;
                       }
                     } else if (ringingCall.getId() == mFirstIncomingCallId) {
                       //2nd incoming call state is not updated.
                       //No need to update the disconnection state
                     }
                   }
                 }
             break;
             case MULTI_HELD:
                 Log.d(TAG, "MULTI_HELD event");
                 if (numHeldCalls > mDsDaHeldCalls) {
                   if (numHeldCalls >= 2) {
                     //when multi held calls come,
                     //we have to fake the indicators in such a way as if,
                     //they are conferencely held.
                     int temp_callState = CALL_STATE_IDLE;
                     if (numRingingCalls > 0) {
                       temp_callState = CALL_STATE_INCOMING;
                       BluetoothCall ringingCall = mCallInfo.getRingingOrSimulatedRingingCall();
                       getDSDARingingAddress(ringingCall);
                     }
                     if (numActiveCalls == 0) {
                       mBluetoothHeadset.phoneStateChanged(
                          1,
                          0,
                          temp_callState,
                          mDsDaRingingAddress,
                          mDsDaRingingAddressType,
                          mDsDaRingingName);
                       handlerThreadSleep(DELAY_DSDA_CALL_INDICATORS);
                       mBluetoothHeadset.phoneStateChanged(
                           0,
                           1,
                           temp_callState,
                           mDsDaRingingAddress,
                           mDsDaRingingAddressType,
                           mDsDaRingingName);
                           mDsDaHeldCalls++;
                           mDsdaActiveCalls = numActiveCalls;
                     } else {
                       mBluetoothHeadset.phoneStateChanged(
                          1,
                          0,
                          temp_callState,
                          mDsDaRingingAddress,
                          mDsDaRingingAddressType,
                          mDsDaRingingName);
                       handlerThreadSleep(DELAY_DSDA_CALL_INDICATORS);
                       mBluetoothHeadset.phoneStateChanged(
                          0,
                          1,
                          temp_callState,
                          mDsDaRingingAddress,
                          mDsDaRingingAddressType,
                          mDsDaRingingName);
                       handlerThreadSleep(DELAY_DSDA_CALL_INDICATORS);
                       mBluetoothHeadset.phoneStateChanged(
                          1,
                          1,
                          temp_callState,
                          mDsDaRingingAddress,
                          mDsDaRingingAddressType,
                          mDsDaRingingName);
                          mDsDaHeldCalls++;
                          mDsdaActiveCalls = numActiveCalls;
                     }
                     mLastBtHeadsetState = temp_callState;
                     if (mDelayOutgoingUpdate == 1) {
                        mBluetoothHeadset.phoneStateChanged(
                          0,
                          1,
                          CALL_STATE_DIALING,
                          mDsDaRingingAddress,
                          mDsDaRingingAddressType,
                          mDsDaRingingName);
                        mBluetoothHeadset.phoneStateChanged(
                          0,
                          1,
                          CALL_STATE_ALERTING,
                          mDsDaRingingAddress,
                          mDsDaRingingAddressType,
                          mDsDaRingingName);
                          mDsDaOutgoingCalls++;
                        mLastBtHeadsetState =  CALL_STATE_ALERTING;
                        mDelayOutgoingUpdate = 0;
                     }
                   }
                 }
             break;
             case OUTGOING_INCOMING_DISCONNECTION:
                 Log.d(TAG, "OUTGOING_INCOMING_DISCONNECTION event");
                 if (numOutgoingCalls == 1) {
                   //No need to update here.
                   //since outgoing call is updated.
                   mDsdaIncomingCalls--;
                   if (mDsDaTwoIncomingCallsFlag == 1) {
                     mFirstIncomingCallId = mSecondIncomingCallId;
                     mSecondIncomingCallId = -1;
                     mDsDaTwoIncomingCallsFlag = 0;
                   } else {
                     mFirstIncomingCallId = -1;
                   }
                 }
             break;
             case MULTI_RINGING_DISCONNECTION:
                 Log.d(TAG, "MULTI_RINGING_DISCONNECTION event");
                 getDSDARingingAddress(null);
                 if (mBluetoothHeadset != null) {
                   mBluetoothHeadset.phoneStateChanged(
                      numActiveCalls,
                      updateheldCalls,
                      CALL_STATE_IDLE,
                      mDsDaRingingAddress,
                      mDsDaRingingAddressType,
                      mDsDaRingingName);
                   getDSDARingingAddress(getBluetoothCallById(mSecondIncomingCallId));
                   mFirstIncomingCallId = mSecondIncomingCallId;
                   mSecondIncomingCallId = -1;
                   mDsDaTwoIncomingCallsFlag = 0;
                   mDsdaIncomingCalls--;
                   handlerThreadSleep(DELAY_DSDA_CALL_INDICATORS);
                   mBluetoothHeadset.phoneStateChanged(
                      numActiveCalls,
                      updateheldCalls,
                      CALL_STATE_INCOMING,
                      mDsDaRingingAddress,
                      mDsDaRingingAddressType,
                      mDsDaRingingName);
                   mLastBtHeadsetState = CALL_STATE_INCOMING;
                 }
             break;
             case OUTGOING_DISCONNECTION:
                Log.d(TAG, "OUTGOING_DISCONNECTION event");
                 if (numRingingCalls > 0) {
                   getDSDARingingAddress(getBluetoothCallById(null));
                   mDsDaOutgoingCalls--;
                   mBluetoothHeadset.phoneStateChanged(
                      numActiveCalls,
                      updateheldCalls,
                      CALL_STATE_IDLE,
                      mDsDaRingingAddress,
                      mDsDaRingingAddressType,
                      mDsDaRingingName);
                   handlerThreadSleep(DELAY_DSDA_CALL_INDICATORS);
                   getDSDARingingAddress(getBluetoothCallById(mFirstIncomingCallId));
                   mBluetoothHeadset.phoneStateChanged(
                      numActiveCalls,
                      updateheldCalls,
                      CALL_STATE_INCOMING,
                      mDsDaRingingAddress,
                      mDsDaRingingAddressType,
                      mDsDaRingingName);
                   mLastBtHeadsetState = CALL_STATE_INCOMING;
                 }
             break;
             case MULTI_HELD_ACTIVE:
                Log.d(TAG, "MULTI_HELD_ACTIVE event");
                if (mNumHeldCalls < mDsDaHeldCalls) {
                    Log.d(TAG, "one of the held moved to active");
                   if (numActiveCalls > mDsdaActiveCalls) {
                     int temp_callState = CALL_STATE_IDLE;
                     if (numRingingCalls > 0) {
                       temp_callState = CALL_STATE_INCOMING;
                       BluetoothCall ringingCall = mCallInfo.getRingingOrSimulatedRingingCall();
                       getDSDARingingAddress(ringingCall);
                     }
                     mBluetoothHeadset.phoneStateChanged(
                        numActiveCalls,
                        0,
                        temp_callState,
                        mDsDaRingingAddress,
                        mDsDaRingingAddressType,
                        mDsDaRingingName);
                     handlerThreadSleep(DELAY_DSDA_CALL_INDICATORS);
                     mBluetoothHeadset.phoneStateChanged(
                        numActiveCalls,
                        1,
                        temp_callState,
                        mDsDaRingingAddress,
                        mDsDaRingingAddressType,
                        mDsDaRingingName);
                     mDsDaHeldCalls = numHeldCalls;
                     mDsdaActiveCalls = numActiveCalls;
                     mLastBtHeadsetState = temp_callState;
                   }
                }
             break;
             case DSDS_EVENT:
                Log.d(TAG, "DSDS_EVENT event");
                updateHeadsetWithCallState(false);
             break;
             default:
              break;
           }
           mDsDaEventsHadlingInProgress = false;
        }
    };

    private void handlerThreadSleep(long millisecs) {
      try {
        Log.d(TAG, "wait for" + millisecs + "msecs");
        Thread.sleep(millisecs);
        } catch (InterruptedException e) {
          Log.e(TAG, "DsDa Thread was interrupted", e);
        }
    }

    @RequiresPermission(android.Manifest.permission.MODIFY_PHONE_STATE)
    protected void enforceModifyPermission() {
        enforceCallingOrSelfPermission(android.Manifest.permission.MODIFY_PHONE_STATE, null);
    }

    @RequiresPermission(android.Manifest.permission.MODIFY_PHONE_STATE)
    public boolean answerCall() {
        synchronized (LOCK) {
            enforceModifyPermission();
            Log.i(TAG, "BT - answering call");
            BluetoothCall call = mCallInfo.getRingingOrSimulatedRingingCall();
            if (mEnableDsdaMode) {
              BluetoothCall mTempCall = null;
              if (mDsDaTwoIncomingCallsFlag == 1) {
                mTempCall = getBluetoothCallById(mFirstIncomingCallId);
                if (mCallInfo.isNullCall(mTempCall)) {
                   call = mTempCall;
                }
              }
            }
            if (mCallInfo.isNullCall(call)) {
                return false;
            }
            call.answer(VideoProfile.STATE_AUDIO_ONLY);
            return true;
        }
    }

    @RequiresPermission(android.Manifest.permission.MODIFY_PHONE_STATE)
    public boolean hangupCall() {
        synchronized (LOCK) {
            enforceModifyPermission();
            Log.i(TAG, "BT - hanging up call");
            BluetoothCall call = mCallInfo.getForegroundCall();
            BluetoothCall mTempCall = null;
            if (mCallInfo.isNullCall(call)) {
                return false;
            }
            // release the parent if there is a conference call
            BluetoothCall conferenceCall = getBluetoothCallById(call.getParentId());
            if (!mCallInfo.isNullCall(conferenceCall)
                    && conferenceCall.getState() == Call.STATE_ACTIVE) {
                Log.i(TAG, "BT - hanging up conference call");
                call = conferenceCall;
            }
            if (call.getState() == Call.STATE_RINGING) {
              if (mEnableDsdaMode) {
                if (mDsDaTwoIncomingCallsFlag == 1) {
                 mTempCall = getBluetoothCallById(mFirstIncomingCallId);
                 if (mCallInfo.isNullCall(mTempCall)) {
                    call = mTempCall;
                 }
                }
              }
                call.reject(false, "");
            } else {
                call.disconnect();
            }
            return true;
        }
    }

    @RequiresPermission(android.Manifest.permission.MODIFY_PHONE_STATE)
    public boolean sendDtmf(int dtmf) {
        synchronized (LOCK) {
            enforceModifyPermission();
            Log.i(TAG, "BT - sendDtmf " + dtmf);
            BluetoothCall call = mCallInfo.getForegroundCall();
            if (mCallInfo.isNullCall(call)) {
                return false;
            }
            // TODO: Consider making this a queue instead of starting/stopping
            // in quick succession.
            call.playDtmfTone((char) dtmf);
            call.stopDtmfTone();
            return true;
        }
    }

    @RequiresPermission(android.Manifest.permission.MODIFY_PHONE_STATE)
    public String getNetworkOperator() {
        synchronized (LOCK) {
            enforceModifyPermission();
            Log.i(TAG, "getNetworkOperator");
            PhoneAccount account = mCallInfo.getBestPhoneAccount();
            if (account != null && account.getLabel() != null) {
                return account.getLabel().toString();
            }
            // Finally, just get the network name from telephony.
            return mTelephonyManager.getNetworkOperatorName();
        }
    }

    /**
     * Gets the brearer technology.
     *
     * @return bearer technology as defined in Bluetooth Assigned Numbers
     */
    @RequiresPermission(android.Manifest.permission.MODIFY_PHONE_STATE)
    public int getBearerTechnology() {
        synchronized (LOCK) {
            enforceModifyPermission();
            Log.i(TAG, "getBearerTechnology");
            // Get the network name from telephony.
            int dataNetworkType = mTelephonyManager.getDataNetworkType();
            switch (dataNetworkType) {
                case TelephonyManager.NETWORK_TYPE_UNKNOWN:
                case TelephonyManager.NETWORK_TYPE_GSM:
                    return BluetoothLeCallControlProxy.BEARER_TECHNOLOGY_GSM;

                case TelephonyManager.NETWORK_TYPE_GPRS:
                    return BluetoothLeCallControlProxy.BEARER_TECHNOLOGY_2G;

                case TelephonyManager.NETWORK_TYPE_EDGE:
                case TelephonyManager.NETWORK_TYPE_EVDO_0:
                case TelephonyManager.NETWORK_TYPE_EVDO_A:
                case TelephonyManager.NETWORK_TYPE_HSDPA:
                case TelephonyManager.NETWORK_TYPE_HSUPA:
                case TelephonyManager.NETWORK_TYPE_HSPA:
                case TelephonyManager.NETWORK_TYPE_IDEN:
                case TelephonyManager.NETWORK_TYPE_EVDO_B:
                    return BluetoothLeCallControlProxy.BEARER_TECHNOLOGY_3G;

                case TelephonyManager.NETWORK_TYPE_UMTS:
                case TelephonyManager.NETWORK_TYPE_TD_SCDMA:
                    return BluetoothLeCallControlProxy.BEARER_TECHNOLOGY_WCDMA;

                case TelephonyManager.NETWORK_TYPE_LTE:
                    return BluetoothLeCallControlProxy.BEARER_TECHNOLOGY_LTE;

                case TelephonyManager.NETWORK_TYPE_EHRPD:
                case TelephonyManager.NETWORK_TYPE_CDMA:
                case TelephonyManager.NETWORK_TYPE_1xRTT:
                    return BluetoothLeCallControlProxy.BEARER_TECHNOLOGY_CDMA;

                case TelephonyManager.NETWORK_TYPE_HSPAP:
                    return BluetoothLeCallControlProxy.BEARER_TECHNOLOGY_4G;

                case TelephonyManager.NETWORK_TYPE_IWLAN:
                    return BluetoothLeCallControlProxy.BEARER_TECHNOLOGY_WIFI;

                case TelephonyManager.NETWORK_TYPE_NR:
                    return BluetoothLeCallControlProxy.BEARER_TECHNOLOGY_5G;
            }

            return BluetoothLeCallControlProxy.BEARER_TECHNOLOGY_GSM;
        }
    }

    @RequiresPermission(android.Manifest.permission.MODIFY_PHONE_STATE)
    public String getSubscriberNumber() {
        synchronized (LOCK) {
            enforceModifyPermission();
            Log.i(TAG, "getSubscriberNumber");
            String address = null;
            PhoneAccount account = mCallInfo.getBestPhoneAccount();
            if (account != null) {
                Uri addressUri = account.getAddress();
                if (addressUri != null) {
                    address = addressUri.getSchemeSpecificPart();
                }
            }
            if (TextUtils.isEmpty(address)) {
                if (mTelephonyManager == null) {
                    address = null;
                } else {
                    address = mTelephonyManager.getLine1Number();
                }
                if (address == null) address = "";
            }
            return address;
        }
    }

    @RequiresPermission(android.Manifest.permission.MODIFY_PHONE_STATE)
    public boolean listCurrentCalls() {
        synchronized (LOCK) {
            if (!mOnCreateCalled) {
                Log.w(TAG, "listcurrentCalls() is called before onCreate()");
                return false;
            }
            enforceModifyPermission();
            // only log if it is after we recently updated the headset state or else it can
            // clog the android log since this can be queried every second.
            boolean logQuery = mHeadsetUpdatedRecently;
            mHeadsetUpdatedRecently = false;

            if (logQuery) {
                Log.i(TAG, "listcurrentCalls");
            }

            sendListOfCalls(logQuery);
            return true;
        }
    }

    @RequiresPermission(android.Manifest.permission.MODIFY_PHONE_STATE)
    public boolean queryPhoneState() {
        synchronized (LOCK) {
            enforceModifyPermission();
            Log.i(TAG, "queryPhoneState");
            if (!mEnableDsdaMode) {
              updateHeadsetWithCallState(true);
            } else {
              updateHeadsetWithDSDACallState(true, DSDS_EVENT);
            }
            return true;
        }
    }

    /** Check for HD codec for voice call */
    public boolean isHighDefCallInProgress() {
        boolean isHighDef = false;
        /* TODO: Add as an API in TelephonyManager aosp/2679237 */
        int phoneTypeIms = 5;
        int phoneTypeCdmaLte = 6;
        BluetoothCall ringingCall = mCallInfo.getRingingOrSimulatedRingingCall();
        BluetoothCall dialingCall = mCallInfo.getOutgoingCall();
        BluetoothCall activeCall = mCallInfo.getActiveCall();

        /* If it's an incoming call we will have codec info in dialing state */
        if (ringingCall != null) {
            isHighDef = ringingCall.isHighDefAudio();
        } else if (dialingCall != null) {
            /* CS dialing call has codec info in dialing state */
            Bundle extras = dialingCall.getDetails().getExtras();
            if (extras != null) {
                int phoneType = extras.getInt(TelecomManager.EXTRA_CALL_TECHNOLOGY_TYPE);
                if (phoneType == TelephonyManager.PHONE_TYPE_GSM
                        || phoneType == TelephonyManager.PHONE_TYPE_CDMA) {
                    isHighDef = dialingCall.isHighDefAudio();
                    /* For IMS calls codec info is not present in dialing state */
                } else if (phoneType == phoneTypeIms || phoneType == phoneTypeCdmaLte) {
                    isHighDef = true;
                }
            }
        } else if (activeCall != null) {
            isHighDef = activeCall.isHighDefAudio();
        }
        Log.i(TAG, "isHighDefCallInProgress: Call is High Def " + isHighDef);
        return isHighDef;
    }

    @RequiresPermission(android.Manifest.permission.MODIFY_PHONE_STATE)
    public boolean processChld(int chld) {
        synchronized (LOCK) {
            enforceModifyPermission();
            final long token = Binder.clearCallingIdentity();
            try {
                Log.i(TAG, "processChld " + chld);
                return _processChld(chld);
            } finally {
                Binder.restoreCallingIdentity(token);
            }
        }
    }

    public void onCallAdded(BluetoothCall call) {
        if (call.isExternalCall()) {
            return;
        }
        if (!mBluetoothCallHashMap.containsKey(call.getId())) {
            Log.d(TAG, "onCallAdded");
            CallStateCallback callback = new CallStateCallback(call.getState());
            mCallbacks.put(call.getId(), callback);
            call.registerCallback(callback);

            mBluetoothCallHashMap.put(call.getId(), call);
            if (!call.isConference()) {
                mMaxNumberOfCalls = Integer.max(mMaxNumberOfCalls, mBluetoothCallHashMap.size());
            }
            if (!mEnableDsdaMode) {
               updateHeadsetWithCallState(false /* force */);
            } else {
              processOnCallAdded(call);
            }

            BluetoothLeCall tbsCall = createTbsCall(call);
            if (mBluetoothLeCallControl != null && tbsCall != null) {
                mBluetoothLeCallControl.onCallAdded(tbsCall);
            }
        }
    }

    public void sendBluetoothCallQualityReport(
            long timestamp,
            int rssi,
            int snr,
            int retransmissionCount,
            int packetsNotReceiveCount,
            int negativeAcknowledgementCount) {
        BluetoothCall call = mCallInfo.getForegroundCall();
        if (mCallInfo.isNullCall(call)) {
            Log.w(TAG, "No foreground call while trying to send BQR");
            return;
        }
        Bundle b = new Bundle();
        b.putParcelable(
                BluetoothCallQualityReport.EXTRA_BLUETOOTH_CALL_QUALITY_REPORT,
                new BluetoothCallQualityReport.Builder()
                        .setSentTimestampMillis(timestamp)
                        .setChoppyVoice(true)
                        .setRssiDbm(rssi)
                        .setSnrDb(snr)
                        .setRetransmittedPacketsCount(retransmissionCount)
                        .setPacketsNotReceivedCount(packetsNotReceiveCount)
                        .setNegativeAcknowledgementCount(negativeAcknowledgementCount)
                        .build());
        call.sendCallEvent(BluetoothCallQualityReport.EVENT_BLUETOOTH_CALL_QUALITY_REPORT, b);
    }

    @Override
    public void onCallAdded(Call call) {
        super.onCallAdded(call);
        onCallAdded(new BluetoothCall(call));
    }

    /**
     * Called when a {@code BluetoothCall} has been removed from this in-call session.
     *
     * @param call the {@code BluetoothCall} to remove
     * @param forceRemoveCallback if true, this will always unregister this {@code InCallService} as
     *     a callback for the given {@code BluetoothCall}, when false, this will not remove the
     *     callback when the {@code BluetoothCall} is external so that the call can be added back if
     *     no longer external.
     */
    public void onCallRemoved(BluetoothCall call, boolean forceRemoveCallback) {
        Log.d(TAG, "onCallRemoved");
        CallStateCallback callback = getCallback(call);
        if (callback != null && (forceRemoveCallback || !call.isExternalCall())) {
            call.unregisterCallback(callback);
        }

        if (mBluetoothCallHashMap.containsKey(call.getId())) {
            mBluetoothCallHashMap.remove(call.getId());

            DisconnectCause cause = call.getDisconnectCause();
            if (cause != null && cause.getCode() == DisconnectCause.OTHER) {
                Log.d(TAG, "add inference call with reason: " + cause.getReason());
                mBluetoothCallQueue.add(call.getId());
                mBluetoothConferenceCallInference.put(call.getId(), call);
                // queue size limited to 2 because merge operation only happens on 2 calls
                // we are only interested in last 2 calls merged
                if (mBluetoothCallQueue.size() > 2) {
                    Integer callId = mBluetoothCallQueue.peek();
                    mBluetoothCallQueue.remove();
                    mBluetoothConferenceCallInference.remove(callId);
                }
            }
            // As there is at most 1 conference call, so clear inference when parent call ends
            if (call.isConference()) {
                Log.d(TAG, "conference call ends, clear inference");
                mBluetoothConferenceCallInference.clear();
                mBluetoothCallQueue.clear();
            }
        }

        if (!mEnableDsdaMode) {
          updateHeadsetWithCallState(false /* force */);
        } else {
          processOnCallRemoved(call);
        }

        if (mBluetoothLeCallControl != null) {
            mBluetoothLeCallControl.onCallRemoved(
                    call.getTbsCallId(), getTbsTerminationReason(call));
        }
    }

    @Override
    public void onCallRemoved(Call call) {
        super.onCallRemoved(call);
        BluetoothCall bluetoothCall = getBluetoothCallById(System.identityHashCode(call));
        if (bluetoothCall == null) {
            Log.w(TAG, "onCallRemoved, BluetoothCall is removed before registered");
            return;
        }
        onCallRemoved(bluetoothCall, true /* forceRemoveCallback */);
    }

    @Override
    public void onCallAudioStateChanged(CallAudioState audioState) {
        super.onCallAudioStateChanged(audioState);
        Log.d(TAG, "onCallAudioStateChanged, audioState == " + audioState);

        mLeAudioService = mFactory.getLeAudioService();
        if (mLeAudioService == null) {
            Log.e(TAG, "leAudioService not available");
            return;
        }
        mLeAudioService.updateCallAudioRoute(audioState.getRoute());
    }

    @Override
    public void onCreate() {
        Log.d(TAG, "onCreate");
        super.onCreate();
        BluetoothAdapter.getDefaultAdapter()
                .getProfileProxy(this, mProfileListener, BluetoothProfile.HEADSET);
        BluetoothAdapter.getDefaultAdapter()
                .getProfileProxy(this, mProfileListener, BluetoothProfile.LE_CALL_CONTROL);
        mBluetoothAdapterReceiver = new BluetoothAdapterReceiver();
        IntentFilter intentFilter = new IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED);
        intentFilter.setPriority(IntentFilter.SYSTEM_HIGH_PRIORITY);
        registerReceiver(mBluetoothAdapterReceiver, intentFilter);

        if (mVoiceCapabilityChangeReceiver == null) {
            Log.d(TAG, "onCreate(): mVoiceCapabilityChangeReceiver ");
            mVoiceCapabilityChangeReceiver = new BluetoothAdapterReceiver();
            IntentFilter intentfilter = new IntentFilter(
                TelecomManager.ACTION_TTY_PREFERRED_MODE_CHANGED);
            intentFilter.addAction(ACTION_MSIM_VOICE_CAPABILITY_CHANGED);
            registerReceiver(mVoiceCapabilityChangeReceiver, intentfilter,
                android.Manifest.permission.MODIFY_PHONE_STATE, null, Context.RECEIVER_EXPORTED);
        }
        mOnCreateCalled = true;
        mEnableDsdaMode = SystemProperties.getBoolean(ENABLE_DSDA_SUPPORT, true);
        if (mEnableDsdaMode)  {
          Log.d(TAG, "MEnableDsdaMode is: " + mEnableDsdaMode);
          if (mHandler == null) {
            Log.w(TAG, "start InCall Service thread");
            mHandlerThread = new HandlerThread("InCallHandler");
            mHandlerThread.start();
            mHandler = new InCallHandler(mHandlerThread.getLooper());
          }
          Log.w(TAG, "InCall service exit");
        }
        mTelephonyManager = getSystemService(TelephonyManager.class);
        mTelecomManager = getSystemService(TelecomManager.class);
        currentMode = SystemProperties.getInt(PROPERTY_MULTISIM_VOICE_CAPABILITY, UNKNOWN);
        Log.e(TAG, "current mode is: " + currentMode);
        if (currentMode == UNKNOWN || currentMode == DSDS) {
            Log.e(TAG, "setting the default mode to DSDS");
            currentMode = DSDS;
        }
    }

    @Override
    public void onDestroy() {
        Log.d(TAG, "onDestroy");
        clear();
        mOnCreateCalled = false;
        if (mHandler != null) {
           // Shut down the thread
           Log.d(TAG, "cleanup IncallHandler");
           mHandler.removeCallbacksAndMessages(null);
           Looper looper = mHandler.getLooper();
           if (looper != null) {
              looper.quit();
           }
              mHandler = null;
        }
        if (mHandlerThread != null) {
           mHandlerThread.quit();
           mHandlerThread = null;
        }
        super.onDestroy();
    }

    @Override
    @VisibleForTesting
    public void attachBaseContext(Context base) {
        super.attachBaseContext(base);
    }

    @VisibleForTesting
    void clear() {
        Log.d(TAG, "clear");
        if (mBluetoothAdapterReceiver != null) {
            unregisterReceiver(mBluetoothAdapterReceiver);
            mBluetoothAdapterReceiver = null;
        }
        if (mVoiceCapabilityChangeReceiver != null) {
           Log.d(TAG, "clear");
           unregisterReceiver(mVoiceCapabilityChangeReceiver);
           mVoiceCapabilityChangeReceiver = null;
        }
        if (mBluetoothHeadset != null) {
            mBluetoothHeadset.closeBluetoothHeadsetProxy(this);
            mBluetoothHeadset = null;
        }
        if (mBluetoothLeCallControl != null) {
            mBluetoothLeCallControl.unregisterBearer();
            mBluetoothLeCallControl.closeBluetoothLeCallControlProxy(this);
        }
        mProfileListener = null;
        sInstance = null;
        mCallbacks.clear();
        mBluetoothCallHashMap.clear();
        mBluetoothConferenceCallInference.clear();
        mBluetoothCallQueue.clear();
        mMaxNumberOfCalls = 0;
        mDsdaActiveCalls = 0;
        mDsDaCallState = CALL_STATE_IDLE;
        mDsdaIncomingCalls = 0;
        mDsDaHeldCalls = 0;
        mDsDaOutgoingCalls = 0;
        mdsDaSelectPhoneAccountFlag = -1;
        mDsdaTotalcalls = 0;
        mDsDaRingingAddress = null;
        mDsDaRingingAddressType = DEFAULT_RINGING_ADDRESS_TYPE;
        mDsDaRingingName = null;
        mDsDaTwoIncomingCallsFlag = 0;
    }

    private static boolean isConferenceWithNoChildren(BluetoothCall call) {
        return call.isConference()
                && (call.can(Connection.CAPABILITY_CONFERENCE_HAS_NO_CHILDREN)
                        || call.getChildrenIds().isEmpty());
    }

    private void sendListOfCalls(boolean shouldLog) {
        Collection<BluetoothCall> calls = mCallInfo.getBluetoothCalls();

        // either do conference call CLCC index inference or normal conference call
        BluetoothCall conferenceCallChildrenNotReady = null;
        for (BluetoothCall call : calls) {
            // find the conference call parent among calls
            if (call.isConference() && !mBluetoothConferenceCallInference.isEmpty()) {
                Log.d(
                        TAG,
                        "conference call inferred size: "
                                + mBluetoothConferenceCallInference.size()
                                + " current size: "
                                + mBluetoothCallHashMap.size());
                // Do conference call inference until at least 2 children arrive
                // If carrier sends children info, then inference will end when info arrives.
                // If carrier doesn't send children info, then inference won't impact actual value.
                if (call.getChildrenIds().size() >= 2) {
                    mBluetoothConferenceCallInference.clear();
                    break;
                }
                conferenceCallChildrenNotReady = call;
            }
        }
        if (conferenceCallChildrenNotReady != null) {
            SortedMap<Integer, Object[]> clccResponseMap = new TreeMap<>();
            for (BluetoothCall inferredCall : mBluetoothConferenceCallInference.values()) {
                if (inferredCall.isCallNull() || inferredCall.getHandle() == null) {
                    Log.w(TAG, "inferredCall does not have handle");
                    continue;
                }
                // save the index so later on when real children arrive, index is the same
                int index = inferredCall.mClccIndex;
                if (index == -1) {
                    Log.w(TAG, "inferred index is not valid");
                    continue;
                }

                // associate existing bluetoothCall with inferredCall based on call handle
                for (BluetoothCall bluetoothCall : mBluetoothCallHashMap.values()) {
                    if (bluetoothCall.getHandle() == null) {
                        Log.w(TAG, "call id: " + bluetoothCall.getId() + " handle is null");
                        continue;
                    }
                    if (mTelephonyManager == null) {
                        Log.w(TAG, "mTelephonyManager is null");
                        continue;
                    }
                    boolean isSame =
                            PhoneNumberUtils.areSamePhoneNumber(
                                    bluetoothCall.getHandle().toString(),
                                    inferredCall.getHandle().toString(),
                                    mTelephonyManager.getNetworkCountryIso());
                    if (isSame) {
                        Log.d(
                                TAG,
                                "found conference call children that has same call handle, "
                                        + "call id: "
                                        + bluetoothCall.getId());
                        bluetoothCall.mClccIndex = inferredCall.mClccIndex;
                        break;
                    }
                }

                int direction = inferredCall.isIncoming() ? 1 : 0;
                int state = CALL_STATE_ACTIVE;
                boolean isPartOfConference = true;
                final Uri addressUri;
                if (inferredCall.getGatewayInfo() != null) {
                    addressUri = inferredCall.getGatewayInfo().getOriginalAddress();
                } else {
                    addressUri = inferredCall.getHandle();
                }
                String address = addressUri == null ? null : addressUri.getSchemeSpecificPart();
                if (address != null) {
                    address = PhoneNumberUtils.stripSeparators(address);
                }
                int addressType = address == null ? -1 : PhoneNumberUtils.toaFromString(address);
                clccResponseMap.put(
                        index,
                        new Object[] {
                            index, direction, state, 0, isPartOfConference, address, addressType
                        });
            }
            // sort CLCC response based on index
            for (Object[] response : clccResponseMap.values()) {
                if (response.length < 7) {
                    Log.e(TAG, "clccResponseMap entry too short");
                    continue;
                }
                Log.i(
                        TAG,
                        String.format(
                                "sending inferred clcc for BluetoothCall: index %d, direction"
                                        + " %d, state %d, isPartOfConference %b, addressType %d",
                                (int) response[0],
                                (int) response[1],
                                (int) response[2],
                                (boolean) response[4],
                                (int) response[6]));
                mBluetoothHeadset.clccResponse(
                        (int) response[0],
                        (int) response[1],
                        (int) response[2],
                        (int) response[3],
                        (boolean) response[4],
                        (String) response[5],
                        (int) response[6]);
            }
            sendClccEndMarker();
            return;
        }

        for (BluetoothCall call : calls) {
            // We don't send the parent conference BluetoothCall to the bluetooth device.
            // We do, however want to send conferences that have no children to the bluetooth
            // device (e.g. IMS Conference).
            boolean isConferenceWithNoChildren = isConferenceWithNoChildren(call);
            Log.i(
                    TAG,
                    "sendListOfCalls isConferenceWithNoChildren "
                            + isConferenceWithNoChildren
                            + ", call.getChildrenIds() size "
                            + call.getChildrenIds().size());
            if (!call.isConference() || isConferenceWithNoChildren) {
                sendClccForCall(call, shouldLog);
            }
        }
        sendClccEndMarker();
    }

    private void sendClccEndMarker() {
        // End marker is recognized with an index value of 0. All other parameters are ignored.
        if (mBluetoothHeadset != null) {
            mBluetoothHeadset.clccResponse(0 /* index */, 0, 0, 0, false, null, 0);
        }
    }

    /** Sends a single clcc (C* List Current Calls) event for the specified call. */
    private void sendClccForCall(BluetoothCall call, boolean shouldLog) {
        boolean isForeground = mCallInfo.getForegroundCall() == call;
        int state = getBtCallState(call, isForeground);
        boolean isPartOfConference = false;
        boolean isConferenceWithNoChildren = isConferenceWithNoChildren(call);

        if (state == CALL_STATE_IDLE) {
            return;
        }

        BluetoothCall conferenceCall = getBluetoothCallById(call.getParentId());
        if (!mCallInfo.isNullCall(conferenceCall)) {
            isPartOfConference = true;

            if (conferenceCall.hasProperty(Call.Details.PROPERTY_GENERIC_CONFERENCE)) {
                // Run some alternative states for CDMA Conference-level merge/swap support.
                // Basically, if BluetoothCall supports swapping or merging at the conference-level,
                // then we need to expose the calls as having distinct states
                // (ACTIVE vs CAPABILITY_HOLD) or
                // the functionality won't show up on the bluetooth device.

                // Before doing any special logic, ensure that we are dealing with an
                // ACTIVE BluetoothCall and that the conference itself has a notion of
                // the current "active" child call.
                BluetoothCall activeChild =
                        getBluetoothCallById(
                                conferenceCall.getGenericConferenceActiveChildCallId());
                if (state == CALL_STATE_ACTIVE && !mCallInfo.isNullCall(activeChild)) {
                    // Reevaluate state if we can MERGE or if we can SWAP without previously having
                    // MERGED.
                    boolean shouldReevaluateState =
                            conferenceCall.can(Connection.CAPABILITY_MERGE_CONFERENCE)
                                    || (conferenceCall.can(Connection.CAPABILITY_SWAP_CONFERENCE)
                                            && !conferenceCall.wasConferencePreviouslyMerged());

                    if (shouldReevaluateState) {
                        isPartOfConference = false;
                        if (call == activeChild) {
                            state = CALL_STATE_ACTIVE;
                        } else {
                            // At this point we know there is an "active" child and we know that it
                            // is not this call, so set it to HELD instead.
                            state = CALL_STATE_HELD;
                        }
                    }
                }
            }
            if (conferenceCall.getState() == Call.STATE_HOLDING
                    && conferenceCall.can(Connection.CAPABILITY_MANAGE_CONFERENCE)) {
                // If the parent IMS CEP conference BluetoothCall is on hold, we should mark
                // this BluetoothCall as being on hold regardless of what the other
                // children are doing.
                state = CALL_STATE_HELD;
            }
        } else if (isConferenceWithNoChildren) {
            // Handle the special case of an IMS conference BluetoothCall without conference
            // event package support.
            // The BluetoothCall will be marked as a conference, but the conference will not have
            // child calls where conference event packages are not used by the carrier.
            isPartOfConference = true;
        }

        int index = getIndexForCall(call);
        int direction = call.isIncoming() ? 1 : 0;
        final Uri addressUri;
        if (call.getGatewayInfo() != null) {
            addressUri = call.getGatewayInfo().getOriginalAddress();
        } else {
            addressUri = call.getHandle();
        }

        String address = addressUri == null ? null : addressUri.getSchemeSpecificPart();
        if (address != null) {
            address = PhoneNumberUtils.stripSeparators(address);
        }

        // Don't send host call information when IMS calls are conferenced
        String subsNum = getSubscriberNumber();
        if (subsNum != null && address != null) {
           if (subsNum.equals(address)) {
              Log.w(TAG, "return without sending host call in CLCC");
              return;
           }
        }

        int addressType = address == null ? -1 : PhoneNumberUtils.toaFromString(address);

        if (shouldLog) {
            Log.i(
                    TAG,
                    "sending clcc for BluetoothCall "
                            + index
                            + ", "
                            + direction
                            + ", "
                            + state
                            + ", "
                            + isPartOfConference
                            + ", "
                            + addressType);
        }
        if (mEnableDsdaMode) {
          int heldcalls = mCallInfo.getNumHeldCalls();
          if (isPartOfConference == false) {
            if (state == CALL_STATE_HELD) {
              if ((heldcalls > 1) && (mCallSwapPending != 1)) {
                isPartOfConference = true;
              }
            }
          }
        }
        if (mBluetoothHeadset == null) {
            Log.w(
                    TAG,
                    "mBluetoothHeasdset is null when sending clcc for BluetoothCall "
                            + index
                            + ", "
                            + direction
                            + ", "
                            + state
                            + ", "
                            + isPartOfConference
                            + ", "
                            + addressType);
        } else {
            mBluetoothHeadset.clccResponse(
                    index, direction, state, 0, isPartOfConference, address, addressType);
        }
    }

    int getNextAvailableClccIndex(int index) {
        // find the next available smallest index
        SortedSet<Integer> availableIndex = new TreeSet<>();
        for (int i = index; i <= mMaxNumberOfCalls + 1; i++) {
            availableIndex.add(i);
        }
        for (BluetoothCall bluetoothCall : mBluetoothCallHashMap.values()) {
            int callCLCCIndex = bluetoothCall.mClccIndex;
            if (availableIndex.contains(callCLCCIndex)) {
                availableIndex.remove(callCLCCIndex);
            }
        }
        Log.d(TAG, "availableIndex first: " + availableIndex.first());
        return availableIndex.first();
    }

    /**
     * Returns the caches index for the specified call. If no such index exists, then an index is
     * given (the smallest number starting from 1 that isn't already taken).
     */
    private int getIndexForCall(BluetoothCall call) {
        if (mCallInfo.isNullCall(call)) {
            Log.w(TAG, "empty or null call");
            return -1;
        }
        if (call.mClccIndex >= 1) {
            return call.mClccIndex;
        }

        int index = 1; // Indexes for bluetooth clcc are 1-based.
        if (call.isConference()) {
            index = mMaxNumberOfCalls + 1; // The conference call should have a higher index
            Log.i(TAG, "getIndexForCall for conference call starting from " + mMaxNumberOfCalls);
        }

        // NOTE: Indexes are removed in {@link #onCallRemoved}.
        call.mClccIndex = getNextAvailableClccIndex(index);
        Log.d(TAG, "call " + call.getId() + " CLCC index is " + call.mClccIndex);
        return call.mClccIndex;
    }

    private boolean _processChld(int chld) {
        BluetoothCall activeCall = mCallInfo.getActiveCall();
        BluetoothCall ringingCall = mCallInfo.getRingingOrSimulatedRingingCall();
        if (mEnableDsdaMode) {
          int num_ringingCalls = mCallInfo.getNumRingingCalls();
          if (num_ringingCalls >1) {
            Log.i(TAG, "more than 1 ringing call is present");
            ringingCall = getBluetoothCallById(mFirstIncomingCallId);
          }
        }
        if (ringingCall == null) {
            Log.i(TAG, "ringingCall null at processChld");
        } else {
            Log.i(TAG, "ringingCall hashcode: " + ringingCall.hashCode());
        }

        BluetoothCall heldCall = mCallInfo.getHeldCall();

        Log.i(
                TAG,
                "Active: "
                        + activeCall
                        + " Ringing: "
                        + ringingCall
                        + " Held: "
                        + heldCall
                        + " chld: "
                        + chld);

        if (chld == CHLD_TYPE_RELEASEHELD) {
            Log.i(TAG, "chld is CHLD_TYPE_RELEASEHELD");
            if (!mCallInfo.isNullCall(ringingCall)) {
                Log.i(TAG, "reject ringing call " + ringingCall.hashCode());
                ringingCall.reject(false, null);
                return true;
            } else if (!mCallInfo.isNullCall(heldCall)) {
                heldCall.disconnect();
                return true;
            }
        } else if (chld == CHLD_TYPE_RELEASEACTIVE_ACCEPTHELD) {
            if (mCallInfo.isNullCall(activeCall)
                    && mCallInfo.isNullCall(ringingCall)
                    && mCallInfo.isNullCall(heldCall)) {
                return false;
            }
            if (!mCallInfo.isNullCall(activeCall)) {
                BluetoothCall conferenceCall = getBluetoothCallById(activeCall.getParentId());
                if (!mCallInfo.isNullCall(conferenceCall)
                        && conferenceCall.getState() == Call.STATE_ACTIVE) {
                    Log.i(TAG, "CHLD: disconnect conference call");
                    conferenceCall.disconnect();
                } else {
                    activeCall.disconnect();
                }
            }
            if (!mCallInfo.isNullCall(ringingCall)) {
                ringingCall.answer(ringingCall.getVideoState());
            } else if (!mCallInfo.isNullCall(heldCall)) {
                heldCall.unhold();
            }
            return true;
        } else if (chld == CHLD_TYPE_HOLDACTIVE_ACCEPTHELD) {
            if (!mCallInfo.isNullCall(activeCall)
                    && activeCall.can(Connection.CAPABILITY_SWAP_CONFERENCE)) {
                activeCall.swapConference();
                Log.i(TAG, "CDMA calls in conference swapped, updating headset");
                updateHeadsetWithCallState(true /* force */);
                return true;
            } else if (!mCallInfo.isNullCall(ringingCall)) {
                ringingCall.answer(VideoProfile.STATE_AUDIO_ONLY);
                return true;
            } else if (!mCallInfo.isNullCall(heldCall)) {
                // CallsManager will hold any active calls when unhold() is called on a
                // currently-held call.
                heldCall.unhold();
                return true;
            } else if (!mCallInfo.isNullCall(activeCall)
                    && activeCall.can(Connection.CAPABILITY_HOLD)) {
                activeCall.hold();
                return true;
            }
        } else if (chld == CHLD_TYPE_ADDHELDTOCONF) {
            if (!mCallInfo.isNullCall(activeCall)) {
                if (activeCall.can(Connection.CAPABILITY_MERGE_CONFERENCE)) {
                    activeCall.mergeConference();
                    return true;
                } else {
                    List<BluetoothCall> conferenceable =
                            getBluetoothCallsByIds(activeCall.getConferenceableCalls());
                    if (!conferenceable.isEmpty()) {
                        activeCall.conference(conferenceable.get(0));
                        return true;
                    }
                }
            }
        }
        return false;
    }

    /**
     * Sends an update of the current BluetoothCall state to the current Headset.
     *
     * @param force {@code true} if the headset state should be sent regardless if no changes to the
     *     state have occurred, {@code false} if the state should only be sent if the state has
     *     changed.
     */
    private void updateHeadsetWithCallState(boolean force) {
        BluetoothCall activeCall = mCallInfo.getActiveCall();
        BluetoothCall ringingCall = mCallInfo.getRingingOrSimulatedRingingCall();
        BluetoothCall heldCall = mCallInfo.getHeldCall();

        int bluetoothCallState = getBluetoothCallStateForUpdate();

        String ringingAddress = null;
        int ringingAddressType = DEFAULT_RINGING_ADDRESS_TYPE;
        String ringingName = null;
        if (!mCallInfo.isNullCall(ringingCall)
                && ringingCall.getHandle() != null) {
            ringingAddress = ringingCall.getHandle().getSchemeSpecificPart();
            if (ringingAddress != null) {
                ringingAddressType = PhoneNumberUtils.toaFromString(ringingAddress);
            }
            ringingName = ringingCall.getCallerDisplayName();
            if (TextUtils.isEmpty(ringingName)) {
                ringingName = ringingCall.getContactDisplayName();
            }
        }
        if (ringingAddress == null) {
            ringingAddress = "";
        }

        int numActiveCalls = mCallInfo.isNullCall(activeCall) ? 0 : 1;
        int numHeldCalls = mCallInfo.getNumHeldCalls();
        if (mTelephonyManager != null) {
            if (currentMode == DSDA || currentMode == PSEUDO_DSDA
                || dsdsTransition) {
                Log.i(TAG, "Concurrent Calls Possible: DSDA ");
                if (numHeldCalls > 1) {
                    mDsDaHeldCalls = numHeldCalls;
                    numHeldCalls = 1;
                }
            }
        }
        int numChildrenOfActiveCall =
                mCallInfo.isNullCall(activeCall) ? 0 : activeCall.getChildrenIds().size();

        // Intermediate state for GSM calls which are in the process of being swapped.
        // TODO: Should we be hardcoding this value to 2 or should we check if all top level calls
        //       are held?
        boolean callsPendingSwitch = (numHeldCalls == 2);

        // For conference calls which support swapping the active BluetoothCall within the
        // conference (namely CDMA calls) we need to expose that as a held BluetoothCall
        // in order for the BT device to show "swap" and "merge" functionality.
        boolean ignoreHeldCallChange = false;
        if (!mCallInfo.isNullCall(activeCall)
                && activeCall.isConference()
                && !activeCall.can(Connection.CAPABILITY_CONFERENCE_HAS_NO_CHILDREN)) {
            if (activeCall.can(Connection.CAPABILITY_SWAP_CONFERENCE)) {
                // Indicate that BT device should show SWAP command by indicating that there is a
                // BluetoothCall on hold, but only if the conference wasn't previously merged.
                numHeldCalls = activeCall.wasConferencePreviouslyMerged() ? 0 : 1;
            } else if (activeCall.can(Connection.CAPABILITY_MERGE_CONFERENCE)) {
                numHeldCalls = 1; // Merge is available, so expose via numHeldCalls.
            }

            for (Integer id : activeCall.getChildrenIds()) {
                // Held BluetoothCall has changed due to it being combined into a CDMA conference.
                // Keep track of this and ignore any future update since it doesn't really count
                // as a BluetoothCall change.
                if (mOldHeldCall != null && Objects.equals(mOldHeldCall.getId(), id)) {
                    ignoreHeldCallChange = true;
                    break;
                }
            }
        }

        if (mBluetoothHeadset != null
                && (force
                        || (!callsPendingSwitch
                                && (numActiveCalls != mNumActiveCalls
                                        || numChildrenOfActiveCall != mNumChildrenOfActiveCall
                                        || numHeldCalls != mNumHeldCalls
                                        || bluetoothCallState != mBluetoothCallState
                                        || !TextUtils.equals(ringingAddress, mRingingAddress)
                                        || ringingAddressType != mRingingAddressType
                                        || (heldCall != mOldHeldCall && !ignoreHeldCallChange))))) {

            // If the BluetoothCall is transitioning into the alerting state, send DIALING first.
            // Some devices expect to see a DIALING state prior to seeing an ALERTING state
            // so we need to send it first.
            boolean sendDialingFirst =
                    mBluetoothCallState != bluetoothCallState
                            && bluetoothCallState == CALL_STATE_ALERTING;

            mOldHeldCall = heldCall;
            mNumActiveCalls = numActiveCalls;
            mNumChildrenOfActiveCall = numChildrenOfActiveCall;
            mNumHeldCalls = numHeldCalls;
            mBluetoothCallState = bluetoothCallState;
            mRingingAddress = ringingAddress;
            mRingingAddressType = ringingAddressType;

            if (sendDialingFirst) {
                // Log in full to make logs easier to debug.
                Log.i(
                        TAG,
                        "updateHeadsetWithCallState "
                                + "numActive "
                                + mNumActiveCalls
                                + ", "
                                + "numHeld "
                                + mNumHeldCalls
                                + ", "
                                + "callState "
                                + CALL_STATE_DIALING
                                + ", "
                                + "ringing type "
                                + mRingingAddressType);
                mBluetoothHeadset.phoneStateChanged(
                        mNumActiveCalls,
                        mNumHeldCalls,
                        CALL_STATE_DIALING,
                        mRingingAddress,
                        mRingingAddressType,
                        ringingName);
            }

            Log.i(
                    TAG,
                    "updateHeadsetWithCallState "
                            + "numActive "
                            + mNumActiveCalls
                            + ", "
                            + "numHeld "
                            + mNumHeldCalls
                            + ", "
                            + "callState "
                            + mBluetoothCallState
                            + ", "
                            + "ringing type "
                            + mRingingAddressType);

            mBluetoothHeadset.phoneStateChanged(
                    mNumActiveCalls,
                    mNumHeldCalls,
                    mBluetoothCallState,
                    mRingingAddress,
                    mRingingAddressType,
                    ringingName);

            mHeadsetUpdatedRecently = true;
            mLastBtHeadsetState = bluetoothCallState;
            mDsdaActiveCalls = mNumActiveCalls;
            //mDsDaHeldCalls = mNumHeldCalls;
            Log.d(TAG, "LastBt Headset State is : "+ mLastBtHeadsetState);
        }
    }

    private int getBluetoothCallStateForUpdate() {
        BluetoothCall ringingCall = mCallInfo.getRingingOrSimulatedRingingCall();
        BluetoothCall dialingCall = mCallInfo.getOutgoingCall();
        boolean hasOnlyDisconnectedCalls = mCallInfo.hasOnlyDisconnectedCalls();

        //
        // !! WARNING !!
        // You will note that CALL_STATE_WAITING, CALL_STATE_HELD, and CALL_STATE_ACTIVE are not
        // used in this version of the BluetoothCall state mappings.  This is on purpose.
        // phone_state_change() in btif_hf.c is not written to handle these states. Only with the
        // listCalls*() method are WAITING and ACTIVE used.
        // Using the unsupported states here caused problems with inconsistent state in some
        // bluetooth devices (like not getting out of ringing state after answering a call).
        //
        int bluetoothCallState = CALL_STATE_IDLE;
        if (!mCallInfo.isNullCall(ringingCall)) {
            bluetoothCallState = CALL_STATE_INCOMING;
        } else if (!mCallInfo.isNullCall(dialingCall)) {
            bluetoothCallState = CALL_STATE_ALERTING;
        } else if (hasOnlyDisconnectedCalls || mIsDisconnectedTonePlaying) {
            // Keep the DISCONNECTED state until the disconnect tone's playback is done
            bluetoothCallState = CALL_STATE_DISCONNECTED;
        }
        return bluetoothCallState;
    }

    private int getBtCallState(BluetoothCall call, boolean isForeground) {
        switch (call.getState()) {
            case Call.STATE_NEW:
            case Call.STATE_DISCONNECTED:
            case Call.STATE_AUDIO_PROCESSING:
                return CALL_STATE_IDLE;

            case Call.STATE_ACTIVE:
                return CALL_STATE_ACTIVE;

            case Call.STATE_CONNECTING:
            case Call.STATE_SELECT_PHONE_ACCOUNT:
            case Call.STATE_DIALING:
            case Call.STATE_PULLING_CALL:
                // Yes, this is correctly returning ALERTING.
                // "Dialing" for BT means that we have sent information to the service provider
                // to place the BluetoothCall but there is no confirmation that the BluetoothCall
                // is going through. When there finally is confirmation, the ringback is
                // played which is referred to as an "alert" tone, thus, ALERTING.
                // TODO: We should consider using the ALERTING terms in Telecom because that
                // seems to be more industry-standard.
                return CALL_STATE_ALERTING;

            case Call.STATE_HOLDING:
                return CALL_STATE_HELD;

            case Call.STATE_RINGING:
            case Call.STATE_SIMULATED_RINGING:
                if (isForeground) {
                    return CALL_STATE_INCOMING;
                } else {
                    return CALL_STATE_WAITING;
                }
        }
        return CALL_STATE_IDLE;
    }

    private void processOnCallAdded(BluetoothCall call) {

      int numRingingCalls      = mCallInfo.getNumRingingCalls();
      BluetoothCall activeCall = mCallInfo.getActiveCall();

      Log.d(TAG, "processOnCallAdded Events");
      if ((call.getState()  == Call.STATE_CONNECTING) ||
          (call.getState()  == Call.STATE_DIALING)) {
         if (activeCall != null && mDsdaActiveCalls == 1) {
           mDelayOutgoingUpdate = 1;
           mDsDaOutgoingCalls++;
           return;
         }
         mDsDaOutgoingCalls++;
         updateHeadsetWithDSDACallState(true, DSDS_EVENT);

      } else if ((call.getState() == Call.STATE_RINGING) ||
         (call.getState() == Call.STATE_SIMULATED_RINGING)) {
        Log.d(TAG, "incoming call received");
        if ((mDsdaIncomingCalls == 0) && (numRingingCalls == 1)) {

          mFirstIncomingCallId = call.getId();
          mDsdaIncomingCalls++;
          if (mDsDaOutgoingCalls == 0) {
            updateHeadsetWithDSDACallState(true, DSDS_EVENT);
            return;
          } else {
            Log.d(TAG, "incoming call received while outgoing is present");
            return;
          }

        } else if ((mDsdaIncomingCalls == 1) && (numRingingCalls == 2)) {
            mDsDaTwoIncomingCallsFlag = 1;
            mDsdaIncomingCalls++;
            mSecondIncomingCallId = call.getId();
            return;
        }
      } else if ((call.getState() == Call.STATE_ACTIVE)) {
        if (mDsdaActiveCalls == 0) {
           updateHeadsetWithDSDACallState(true, DSDS_EVENT);
           mDsdaActiveCalls = 1;
           return;
        } else if (mDsdaActiveCalls == 1) {
           if (call.getId().equals(activeCall.getId())) {
             return;
           } else {
             //conference call would have been initiated
             conferenceCallInitiated = 1;
             processConferenceCall();
             return;
           }
        } if (conferenceCallInitiated == 1) {
         //directing all the next call added events to the ProcessConferenceCall
         processConferenceCall();
         return;
        }
       }else if ((call.getState() == Call.STATE_HOLDING)) {
        if ((mDsDaHeldCalls == 0) || (mDsDaHeldCalls == 1)) {
           Log.d(TAG, "Call held came when no calls are present");
           updateHeadsetWithDSDACallState(true, DSDS_EVENT);
           mDsDaHeldCalls++;
           return;
        } else if (mDsDaHeldCalls > 1) {
           //multiple dsda held calls are present before headset is connected
           //conference fake need be updated in this case
           Log.d(TAG, "multiple held calls during headset connection"
                       + "need to fake in this case");
           updateHeadsetWithDSDACallState(true, MULTI_HELD);
           return;
        }
      } else if ((call.getState() == Call.STATE_SELECT_PHONE_ACCOUNT)) {
        //When BLDN is triggered and sim needs to be selected,
        //SELECT_PHONE_ACCOUNT will be triggered. We dont send any
        //update over BT until its state turned to connecting or dialing
        mdsDaSelectPhoneAccountFlag = 1;
        mSelectPhoneAccountId = call.getId();
        return;
      }
    }

    private void processOnCallRemoved(BluetoothCall call) {

       BluetoothCall dailingCall  = mCallInfo.getOutgoingCall();
       int numOutgoingCalls       = mCallInfo.isNullCall(dailingCall) ? 0 : 1;
       BluetoothCall activeCall   = mCallInfo.getActiveCall();
       int numActiveCalls         = mCallInfo.isNullCall(activeCall) ? 0 : 1;
       int numHeldCalls           = mCallInfo.getNumHeldCalls();
       int numRingingCalls        = mCallInfo.getNumRingingCalls();

       Log.d(TAG, "processOnCallRemoved Events");

       if (numOutgoingCalls != mDsDaOutgoingCalls) {
         Log.d (TAG, "outgoing call ended before answered");
         if ((mNumHeldCalls <= 1) && (numRingingCalls == 0)) {
           mDsDaOutgoingCalls--;
           updateHeadsetWithDSDACallState(true, DSDS_EVENT);
         } else {
            updateHeadsetWithDSDACallState(true, OUTGOING_DISCONNECTION);
         }
       } else if (mDsdaIncomingCalls != numRingingCalls) {
         if ((mSecondIncomingCallId != -1) &&
             (mSecondIncomingCallId == call.getId())) {
           Log.d(TAG, "Non Updated Incoming call is ended");
           mDsdaIncomingCalls--;
           mSecondIncomingCallId = -1;
           mDsDaTwoIncomingCallsFlag = 0;
         } else if ((mFirstIncomingCallId != -1) &&
            (mFirstIncomingCallId == call.getId())) {
           Log.d(TAG, "Updated incoming call is ended");
           if ((mDsDaTwoIncomingCallsFlag == 0) &&
              (numHeldCalls <= 1) && (numOutgoingCalls == 0)) {
               mDsdaIncomingCalls--;
               updateHeadsetWithDSDACallState(true, DSDS_EVENT);
               return;
           } else {
             if (numOutgoingCalls == 1) {
               updateHeadsetWithDSDACallState(true, OUTGOING_INCOMING_DISCONNECTION);
             } else {
                if (mDsDaTwoIncomingCallsFlag == 1) {
                  updateHeadsetWithDSDACallState(true, MULTI_RINGING_DISCONNECTION);
                }
             }
           }
         }
       } else if ((mDsdaActiveCalls > 0) || (mDsDaHeldCalls > 0)) {
         Log.d(TAG, "active or held call might have been removed");
         if (mDsDaHeldCalls > numHeldCalls) {
           Log.d(TAG, "mismatch in held calls mDsDaHeldCalls:" +
           mDsDaHeldCalls + "numHeldCalls:" + numHeldCalls);
           if (numHeldCalls == 0) {
             //only one held call earlier.
             //so, need to update this as this is normal scenario
             Log.d(TAG, "only one held call present earlier, updating");
             mDsDaHeldCalls = 0;
             updateHeadsetWithDSDACallState(true, DSDS_EVENT);
           }
           else {
             // no need to update this state..but, need to update clcc.
             mDsDaHeldCalls--;
           }
         }
         if ((mDsdaActiveCalls == 1) && (numActiveCalls == 0)) {
           //this means, active call is ended
           Log.d(TAG, "active call is removed");
           mDsdaActiveCalls = 0;
           updateHeadsetWithDSDACallState(true, DSDS_EVENT);
         }
       } else if ((mdsDaSelectPhoneAccountFlag == 1) &&
           (mSelectPhoneAccountId == call.getId())) {
          //this call state will not be updated if call removed
          //from the select Phone account state before dialing
          Log.d(TAG, "Call removed from SelectPhoneAccount State");
          mdsDaSelectPhoneAccountFlag = 0;
          mSelectPhoneAccountId = -1;
       } else {
         updateHeadsetWithDSDACallState(true, DSDS_EVENT);
       }
     }

    private void processOnStateChanged(BluetoothCall call) {

       int bluetoothLastState    = mLastBtHeadsetState;
       int numRingingCalls       = mCallInfo.getNumRingingCalls();
       int numHeldCalls          = mCallInfo.getNumHeldCalls();
       BluetoothCall activeCall  = mCallInfo.getActiveCall();
       int numActiveCalls        = mCallInfo.isNullCall(activeCall) ? 0 : 1;
       BluetoothCall dailingCall = mCallInfo.getOutgoingCall();
       int numOutgoingCalls      = mCallInfo.isNullCall(dailingCall) ? 0 : 1;
       int btCallState           = getBtCallState(call, false);
       Log.d(TAG, "ProcessOnStateChanged events");

      if ((mdsDaSelectPhoneAccountFlag == 1) &&
          (mSelectPhoneAccountId == call.getId())) {
         if ((call.getState() == Call.STATE_CONNECTING) ||
            (call.getState() == Call.STATE_DIALING)) {
           Log.d(TAG, "Dialing state update After SelectPhoneAccount");
           mdsDaSelectPhoneAccountFlag = 0;
           mSelectPhoneAccountId = 0;
           mDsDaOutgoingCalls++;
           updateHeadsetWithCallState(false /*force*/);
         }
         else {
           //If not call moved to Dialing or connecting, Call would
           //Have been disconnected before selecting the sim.
           //Will remove after the call removed callback.
           Log.d(TAG, "Call Disconnect for SelectPhoneAccount");
         }
         return;
      }

       switch (bluetoothLastState) {
          case CALL_STATE_ALERTING:
          case CALL_STATE_INCOMING:
            if ((btCallState == CALL_STATE_ALERTING) ||
                (btCallState == CALL_STATE_INCOMING) ||
                (btCallState == CALL_STATE_WAITING)) {
               //already updated incoming call no need to update
            } else if (btCallState == CALL_STATE_ACTIVE) {
              //either incoming call/alerting call moved to active

              if (mDsDaOutgoingCalls > numOutgoingCalls) {
                 //outgoing call is made active
                 if (mDsdaIncomingCalls == 0) {
                    Log.d(TAG, "outgoing call became active. DSDS");
                    mDsDaOutgoingCalls = 0;
                    mDsdaActiveCalls = 1;
                    updateHeadsetWithDSDACallState(true, DSDS_EVENT);
                 } else {
                    Log.d(TAG, "outgoing call became active.when incoming call is present");
                    updateHeadsetWithDSDACallState(true, OUTGOING_INCOMING);
                }
              }
              else if (mDsdaIncomingCalls > numRingingCalls) {
                if ((numRingingCalls == 0) && (mDsdaIncomingCalls == 1)) {
                   if (numOutgoingCalls == 1) {
                    updateHeadsetWithDSDACallState(true, OUTGOING_INCOMING);
                   } else {
                     Log.d(TAG, "ringing call moved to active. DSDS");
                     mDsdaIncomingCalls--;
                     mDsdaActiveCalls = 1;
                     updateHeadsetWithDSDACallState(true, DSDS_EVENT); 
                   }
                }
                else if((numRingingCalls == 1) && (mDsdaIncomingCalls == 2)) {
                   Log.d(TAG, "multiple ringing calls, 1 ringing moved to active");
                   updateHeadsetWithDSDACallState(true, MULTI_INCOMING);
                }
              }
              else if (mDsDaHeldCalls > numHeldCalls) {
                 if ((mDsDaHeldCalls == 1) && (numHeldCalls == 0)) {
                   Log.d(TAG, "only 1 held call present, moving it to active");
                   mDsDaHeldCalls = 0;
                   mDsdaActiveCalls = 1;
                   updateHeadsetWithDSDACallState(true, DSDS_EVENT);
                 }
                 else if ((mDsDaHeldCalls > 1) && (numHeldCalls > 0)) {
                    Log.d(TAG, "held call made active");
                    //one of the held call made active
                    //CP: need to be checked
                    mDsdaActiveCalls = 1;
                    updateHeadsetWithDSDACallState(true, MULTI_HELD);
                 }
              }
            }
            else if (btCallState == CALL_STATE_HELD) {
              if ((numActiveCalls == 0) && (mDsdaActiveCalls == 1)) {
                Log.d(TAG, "active call is made to held");
                if (numHeldCalls > mDsDaHeldCalls) {
                  if (numHeldCalls >= 2) {
                     updateHeadsetWithDSDACallState(true, MULTI_HELD);
                  }
                  else {
                      mDsDaHeldCalls++;
                      mDsdaActiveCalls = 0;
                      updateHeadsetWithDSDACallState(true, DSDS_EVENT);
                      Log.d(TAG, "1st held call from active");
                  }
                }
              } else {
                Log.d(TAG, "Received Held call event before any active call");
                mDsDaHeldCalls++;
                updateHeadsetWithDSDACallState(true, DSDS_EVENT);
              }
           }
         break;
         case CALL_STATE_IDLE:
            //so it can have active or held or no calls.cannot have incoming/alerting call
            Log.d(TAG, "previous bt state is idle:" + bluetoothLastState);

            if (btCallState == CALL_STATE_HELD) { //received call event state
              Log.d(TAG, "recevied call event as held event");
              if (mDsDaHeldCalls < numHeldCalls) {
                Log.d(TAG, "new held call is received from active");
                if ((mDsDaHeldCalls > 0) && (numHeldCalls>1)) {
                  Log.d(TAG, "Multiple held event came");
                  if ((mDsDaHeldCalls == 1) && (numHeldCalls == 2)) {
                    if (mTelephonyManager != null) {
                      if (!(currentMode == DSDA || currentMode == PSEUDO_DSDA
                        || dsdsTransition)) {
                        Log.i(TAG, "Concurrent Calls Not Possible: Not DSDA ");
                        Log.i(TAG, "Call swapping is in progress ");
                        mCallSwapPending = 1;
                        updateHeadsetWithCallState(false);
                        return;
                      }
                    }
                  }
                  updateHeadsetWithDSDACallState(true, MULTI_HELD);
                }
                else if ((mDsDaHeldCalls == 0) && (numHeldCalls ==1)) {
                  Log.d(TAG, "when only 1 active call and moved to held call");
                  mDsdaActiveCalls = 0;
                  mDsDaHeldCalls++;
                  mDelayOutgoingUpdate = 0;
                  updateHeadsetWithDSDACallState(true, DSDS_EVENT);
                }
              }
            }
            else if (btCallState == CALL_STATE_IDLE) {
               //this means, received call state event can be in ringing state or disconnected
               //will be handled in call removed
               //this might be one of the held call would have been ended
               //check the behaviour of remote update and change accoordingly.
               //this need not be handled here.. would have been handled in oncalladded event or oncallremoved event
               if ((call.getState() == Call.STATE_NEW) ||
                   (call.getState() == Call.STATE_AUDIO_PROCESSING) ||
                   (call.getState() == Call.STATE_SIMULATED_RINGING)) {
                 Log.d(TAG, "ignoring these call state events");
               }
               else if ((call.getState() == Call.STATE_DISCONNECTED) ||
                        (call.getState() == Call.STATE_DISCONNECTING)) {
                 Log.d(TAG, "this event can come for either held or active call");
                 if ((numActiveCalls == 0) && (mDsdaActiveCalls == 1)) {
                   Log.d(TAG, "active call ended event is received. lets remove from oncallremoved");
                   //updateHeadsetWithDSDACallState(true, DSDS_EVENT);
                   //mDsdaActiveCalls = 0;
                 }
                 else if (numHeldCalls < mDsDaHeldCalls) {
                   if ((numHeldCalls > 0) && (mDsDaHeldCalls > 1)) {
                     Log.d(TAG, "multiple held calls..one of the held is ended");
                     //no update here
                   }
                   else if ((numHeldCalls == 0) && (mDsDaHeldCalls == 1)) {
                     Log.d(TAG, "only 1 held call present and is ended");
                     updateHeadsetWithDSDACallState(true, DSDS_EVENT);
                   }
                 }
               }
               else if (call.getState() == Call.STATE_RINGING) {
                   //this should not come because call added should come 1st. so
                   //ignore this command
                   Log.d(TAG, "this ringing should not come will handle only after oncall added");
               }
           }
           else if (btCallState == CALL_STATE_ACTIVE) {
               if (mCallSwapPending == 1) {
                  updateHeadsetWithDSDACallState(true /* force */, DSDS_EVENT);
                  mCallSwapPending = 0;
                  return;
               }
               if (numHeldCalls < mDsDaHeldCalls) {
                   //for this to work, active should have been none earlier
                   if ((numActiveCalls == 1) && (mDsdaActiveCalls == 0)) {
                       if ((mDsDaHeldCalls == 1) && (numHeldCalls == 0)) {
                           Log.d(TAG, "only held call moved to active");
                           mDsdaActiveCalls = 1;
                           mDsDaHeldCalls--;
                           updateHeadsetWithDSDACallState(true /*force */, DSDS_EVENT);
                       }
                       else {
                           //As per the stack implementation, Active call
                           //can come either from held state, incoming or
                           //from outgoing. so, for multiple held to one active
                           //we will send as if held is made as active and
                           //then update the held call info
                           Log.d(TAG, "multiple held, one moved to active");
                           updateHeadsetWithDSDACallState(true, MULTI_HELD_ACTIVE);
                       }
                   }
                   else {
                        Log.d(TAG, "conference call event came");
                        updateHeadsetWithDSDACallState(true /*force */, DSDS_EVENT);
                   }
               } else {
                   Log.d(TAG, "silent ringing call moved to active");
                   mDsdaIncomingCalls--;
                   mFirstIncomingCallId = -1;
                   updateHeadsetWithDSDACallState(true /* force */, DSDS_EVENT);
               }
           }
         break;
         }
      }

     private void processConferenceCall() {
         Log.d(TAG, "process conference call");
         if (conferenceCallInitiated == 1) {
          //Probably no need to update headset in this case
          //confirmation can be done during testing
          Log.d(TAG, "not updating conference call events");
         }
     }

     private void updateHeadsetWithDSDACallState(boolean force, int event) {
      if ((event == DSDS_EVENT) && (!mDsDaEventsHadlingInProgress)) {
          updateHeadsetWithCallState(force);
      } else {
        Log.e(TAG, "handleDSDA events in separate thread.");
        Message msg = mHandler.obtainMessage();
        msg.what = event;
        mHandler.sendMessage(msg);
      }
     }

     private void getDSDARingingAddress(BluetoothCall ringingCall) {

       if (mCallInfo.isNullCall(ringingCall)) {
          Log.i(TAG, "NULL ringing call address");
          mDsDaRingingAddress = null;
          mDsDaRingingAddressType = DEFAULT_RINGING_ADDRESS_TYPE;
          mDsDaRingingName = null;
          return;
       } else {
         if (!mCallInfo.isNullCall(ringingCall) && ringingCall.getHandle() != null) {
            mDsDaRingingAddress = ringingCall.getHandle().getSchemeSpecificPart();
            if (mDsDaRingingAddress != null) {
               mDsDaRingingAddressType = PhoneNumberUtils.toaFromString(mDsDaRingingAddress);
            }
            mDsDaRingingName = ringingCall.getCallerDisplayName();
            if (TextUtils.isEmpty(mDsDaRingingName)) {
               mDsDaRingingName = ringingCall.getContactDisplayName();
            }
          }
          if (mDsDaRingingAddress == null) {
             mDsDaRingingAddress = "";
          }
          return;
      }
     }

    @VisibleForTesting
    public CallStateCallback getCallback(BluetoothCall call) {
        return mCallbacks.get(call.getId());
    }

    @VisibleForTesting
    public void setBluetoothHeadset(BluetoothHeadsetProxy bluetoothHeadset) {
        mBluetoothHeadset = bluetoothHeadset;
    }

    @VisibleForTesting
    public BluetoothCall getBluetoothCallById(Integer id) {
        if (mBluetoothCallHashMap.containsKey(id)) {
            return mBluetoothCallHashMap.get(id);
        }
        return null;
    }

    @VisibleForTesting
    public List<BluetoothCall> getBluetoothCallsByIds(List<Integer> ids) {
        List<BluetoothCall> calls = new ArrayList<>();
        for (Integer id : ids) {
            BluetoothCall call = getBluetoothCallById(id);
            if (!mCallInfo.isNullCall(call)) {
                calls.add(call);
            }
        }
        return calls;
    }

    // extract call information functions out into this part, so we can mock it in testing
    @VisibleForTesting
    public class CallInfo {

        public BluetoothCall getForegroundCall() {
            LinkedHashSet<Integer> states = new LinkedHashSet<Integer>();
            BluetoothCall foregroundCall;

            states.add(Call.STATE_CONNECTING);
            foregroundCall = getCallByStates(states);
            if (!mCallInfo.isNullCall(foregroundCall)) {
                return foregroundCall;
            }

            states.clear();
            states.add(Call.STATE_ACTIVE);
            states.add(Call.STATE_DIALING);
            states.add(Call.STATE_PULLING_CALL);
            foregroundCall = getCallByStates(states);
            if (!mCallInfo.isNullCall(foregroundCall)) {
                return foregroundCall;
            }

            states.clear();
            states.add(Call.STATE_RINGING);
            foregroundCall = getCallByStates(states);
            if (!mCallInfo.isNullCall(foregroundCall)) {
                return foregroundCall;
            }

            return null;
        }

        public BluetoothCall getCallByStates(LinkedHashSet<Integer> states) {
            List<BluetoothCall> calls = getBluetoothCalls();
            for (BluetoothCall call : calls) {
                if (states.contains(call.getState())) {
                    return call;
                }
            }
            return null;
        }

        public BluetoothCall getCallByState(int state) {
            List<BluetoothCall> calls = getBluetoothCalls();
            for (BluetoothCall call : calls) {
                if (state == call.getState() && call.getParentId() == null) {
                    return call;
                }
            }
            return null;
        }

        public int getNumHeldCalls() {
            int number = 0;
            List<BluetoothCall> calls = getBluetoothCalls();
            for (BluetoothCall call : calls) {
                if (call.getState() == Call.STATE_HOLDING) {
                    number++;
                }
            }
            return number;
        }

        public int getNumRingingCalls() {
            int number = 0;
            List<BluetoothCall> calls = getBluetoothCalls();
            for (BluetoothCall call : calls) {
              if (call.getState() == Call.STATE_RINGING) {
                  number++;
              }
            }
            return number;
        }

        public int getNumActiveCalls() {
            int number = 0;
            List<BluetoothCall> calls = getBluetoothCalls();
            for (BluetoothCall call : calls) {
              if (call.getState() == Call.STATE_ACTIVE) {
                  number++;
              }
            }
            return number;
        }



        public boolean hasOnlyDisconnectedCalls() {
            List<BluetoothCall> calls = getBluetoothCalls();
            if (calls.size() == 0) {
                return false;
            }
            for (BluetoothCall call : calls) {
                if (call.getState() != Call.STATE_DISCONNECTED) {
                    return false;
                }
            }
            return true;
        }

        public List<BluetoothCall> getBluetoothCalls() {
            return getBluetoothCallsByIds(BluetoothCall.getIds(getCalls()));
        }

        public BluetoothCall getOutgoingCall() {
            LinkedHashSet<Integer> states = new LinkedHashSet<Integer>();
            states.add(Call.STATE_CONNECTING);
            states.add(Call.STATE_DIALING);
            states.add(Call.STATE_PULLING_CALL);
            return getCallByStates(states);
        }

        public BluetoothCall getRingingOrSimulatedRingingCall() {
            LinkedHashSet<Integer> states = new LinkedHashSet<Integer>();
            states.add(Call.STATE_RINGING);
            states.add(Call.STATE_SIMULATED_RINGING);
            return getCallByStates(states);
        }

        public BluetoothCall getActiveCall() {
            return getCallByState(Call.STATE_ACTIVE);
        }

        public BluetoothCall getHeldCall() {
            return getCallByState(Call.STATE_HOLDING);
        }

        /**
         * Returns the best phone account to use for the given state of all calls. First, tries to
         * return the phone account for the foreground call, second the default phone account for
         * PhoneAccount.SCHEME_TEL.
         */
        public PhoneAccount getBestPhoneAccount() {
            BluetoothCall call = getForegroundCall();

            PhoneAccount account = null;
            if (!mCallInfo.isNullCall(call)) {
                PhoneAccountHandle handle = call.getAccountHandle();
                if (handle != null) {
                    // First try to get the network name of the foreground call.
                    account = mTelecomManager.getPhoneAccount(handle);
                }
            }

            if (account == null) {
                // Second, Try to get the label for the default Phone Account.
                if (mTelecomManager == null) {
                    Log.w(TAG, "mTelecomManager is null");
                    return null;
                }
                List<PhoneAccountHandle> handles =
                        mTelecomManager.getPhoneAccountsSupportingScheme(PhoneAccount.SCHEME_TEL);
                while (handles.iterator().hasNext()) {
                    account = mTelecomManager.getPhoneAccount(handles.iterator().next());
                    if (account != null) {
                        return account;
                    }
                }
            }
            return null;
        }

        public boolean isNullCall(BluetoothCall call) {
            return call == null || call.isCallNull();
        }

        public BluetoothCall getCallByCallId(UUID callId) {
            List<BluetoothCall> calls = getBluetoothCalls();
            for (BluetoothCall call : calls) {
                Log.i(TAG, "getCallByCallId lookingFor=" + callId + " has=" + call.getTbsCallId());
                if (callId.equals(call.getTbsCallId())) {
                    return call;
                }
            }
            return null;
        }
    }
    ;

    @VisibleForTesting
    public void setBluetoothLeCallControl(BluetoothLeCallControlProxy bluetoothTbs) {
        mBluetoothLeCallControl = bluetoothTbs;

        if ((mBluetoothLeCallControl) != null && (mTelecomManager != null)) {
            mBluetoothLeCallControl.registerBearer(
                    TAG,
                    new ArrayList<String>(Arrays.asList("tel")),
                    BluetoothLeCallControl.CAPABILITY_HOLD_CALL,
                    getNetworkOperator(),
                    getBearerTechnology(),
                    mExecutor,
                    mBluetoothLeCallControlCallback);
        }
    }

    private Integer getTbsCallState(BluetoothCall call) {
        switch (call.getState()) {
            case Call.STATE_ACTIVE:
                return BluetoothLeCall.STATE_ACTIVE;

            case Call.STATE_CONNECTING:
            case Call.STATE_SELECT_PHONE_ACCOUNT:
                return BluetoothLeCall.STATE_DIALING;

            case Call.STATE_DIALING:
            case Call.STATE_PULLING_CALL:
                return BluetoothLeCall.STATE_ALERTING;

            case Call.STATE_HOLDING:
                return BluetoothLeCall.STATE_LOCALLY_HELD;

            case Call.STATE_RINGING:
            case Call.STATE_SIMULATED_RINGING:
                return BluetoothLeCall.STATE_INCOMING;
        }
        return null;
    }

    @VisibleForTesting
    int getTbsTerminationReason(BluetoothCall call) {
        DisconnectCause cause = call.getDisconnectCause();
        if (cause == null) {
            Log.w(TAG, " termination cause is null");
            return BluetoothLeCallControl.TERMINATION_REASON_FAIL;
        }

        switch (cause.getCode()) {
            case DisconnectCause.BUSY:
                return BluetoothLeCallControl.TERMINATION_REASON_LINE_BUSY;
            case DisconnectCause.REMOTE:
            case DisconnectCause.REJECTED:
                return BluetoothLeCallControl.TERMINATION_REASON_REMOTE_HANGUP;
            case DisconnectCause.LOCAL:
                if (mIsTerminatedByClient) {
                    mIsTerminatedByClient = false;
                    return BluetoothLeCallControl.TERMINATION_REASON_CLIENT_HANGUP;
                }
                return BluetoothLeCallControl.TERMINATION_REASON_SERVER_HANGUP;
            case DisconnectCause.ERROR:
                return BluetoothLeCallControl.TERMINATION_REASON_NETWORK_CONGESTION;
            case DisconnectCause.CONNECTION_MANAGER_NOT_SUPPORTED:
                return BluetoothLeCallControl.TERMINATION_REASON_INVALID_URI;
            default:
                return BluetoothLeCallControl.TERMINATION_REASON_FAIL;
        }
    }

    private BluetoothLeCall createTbsCall(BluetoothCall call) {
        Integer state = getTbsCallState(call);
        boolean isConferenceWithNoChildren = isConferenceWithNoChildren(call);

        if (state == null) {
            return null;
        }

        BluetoothCall conferenceCall = getBluetoothCallById(call.getParentId());
        if (!mCallInfo.isNullCall(conferenceCall)) {
            // Run some alternative states for Conference-level merge/swap support.
            // Basically, if BluetoothCall supports swapping or merging at the
            // conference-level,
            // then we need to expose the calls as having distinct states
            // (ACTIVE vs CAPABILITY_HOLD) or
            // the functionality won't show up on the bluetooth device.

            // Before doing any special logic, ensure that we are dealing with an
            // ACTIVE BluetoothCall and that the conference itself has a notion of
            // the current "active" child call.
            BluetoothCall activeChild =
                    getBluetoothCallById(conferenceCall.getGenericConferenceActiveChildCallId());
            if (state == BluetoothLeCall.STATE_ACTIVE && !mCallInfo.isNullCall(activeChild)) {
                // Reevaluate state if we can MERGE or if we can SWAP without previously having
                // MERGED.
                boolean shouldReevaluateState =
                        conferenceCall.can(Connection.CAPABILITY_MERGE_CONFERENCE)
                                || (conferenceCall.can(Connection.CAPABILITY_SWAP_CONFERENCE)
                                        && !conferenceCall.wasConferencePreviouslyMerged());

                if (shouldReevaluateState) {
                    if (call == activeChild) {
                        state = BluetoothLeCall.STATE_ACTIVE;
                    } else {
                        // At this point we know there is an "active" child and we know that it is
                        // not this call, so set it to HELD instead.
                        state = BluetoothLeCall.STATE_LOCALLY_HELD;
                    }
                }
            }
            if (conferenceCall.getState() == Call.STATE_HOLDING
                    && conferenceCall.can(Connection.CAPABILITY_MANAGE_CONFERENCE)) {
                // If the parent IMS CEP conference BluetoothCall is on hold, we should mark
                // this BluetoothCall as being on hold regardless of what the other
                // children are doing.
                state = BluetoothLeCall.STATE_LOCALLY_HELD;
            }
        } else if (isConferenceWithNoChildren) {
            // Handle the special case of an IMS conference BluetoothCall without conference
            // event package support.
            // The BluetoothCall will be marked as a conference, but the conference will not
            // have
            // child calls where conference event packages are not used by the carrier.
        }

        final Uri addressUri;
        if (call.getGatewayInfo() != null) {
            addressUri = call.getGatewayInfo().getOriginalAddress();
        } else {
            addressUri = call.getHandle();
        }

        String uri = addressUri == null ? null : addressUri.toString();
        int callFlags = call.isIncoming() ? 0 : BluetoothLeCall.FLAG_OUTGOING_CALL;

        String friendlyName = call.getCallerDisplayName();
        if (TextUtils.isEmpty(friendlyName)) {
            friendlyName = call.getContactDisplayName();
        }

        return new BluetoothLeCall(call.getTbsCallId(), uri, friendlyName, state, callFlags);
    }

    private void sendTbsCurrentCallsList() {
        List<BluetoothLeCall> tbsCalls = new ArrayList<>();

        for (BluetoothCall call : mBluetoothCallHashMap.values()) {
            BluetoothLeCall tbsCall = createTbsCall(call);
            if (tbsCall != null) {
                tbsCalls.add(tbsCall);
            }
        }

        mBluetoothLeCallControl.currentCallsList(tbsCalls);
    }

    @VisibleForTesting
    final BluetoothLeCallControl.Callback mBluetoothLeCallControlCallback =
            new BluetoothLeCallControl.Callback() {

                @Override
                public void onAcceptCall(int requestId, UUID callId) {
                    synchronized (LOCK) {
                        enforceModifyPermission();
                        Log.i(TAG, "TBS - accept call=" + callId);
                        int result = BluetoothLeCallControl.RESULT_SUCCESS;
                        BluetoothCall call = mCallInfo.getCallByCallId(callId);
                        if (mCallInfo.isNullCall(call)) {
                            result = BluetoothLeCallControl.RESULT_ERROR_UNKNOWN_CALL_ID;
                        } else {
                            call.answer(VideoProfile.STATE_AUDIO_ONLY);
                        }
                        mBluetoothLeCallControl.requestResult(requestId, result);
                    }
                }

                @Override
                public void onTerminateCall(int requestId, UUID callId) {
                    synchronized (LOCK) {
                        enforceModifyPermission();
                        Log.i(TAG, "TBS - terminate call=" + callId);
                        int result = BluetoothLeCallControl.RESULT_SUCCESS;
                        BluetoothCall call = mCallInfo.getCallByCallId(callId);
                        if (mCallInfo.isNullCall(call)) {
                            result = BluetoothLeCallControl.RESULT_ERROR_UNKNOWN_CALL_ID;
                        } else {
                            mIsTerminatedByClient = true;
                            call.disconnect();
                        }
                        mBluetoothLeCallControl.requestResult(requestId, result);
                    }
                }

                @Override
                public void onHoldCall(int requestId, UUID callId) {
                    synchronized (LOCK) {
                        enforceModifyPermission();
                        Log.i(TAG, "TBS - hold call=" + callId);
                        int result = BluetoothLeCallControl.RESULT_SUCCESS;
                        BluetoothCall call = mCallInfo.getCallByCallId(callId);
                        if (mCallInfo.isNullCall(call)) {
                            result = BluetoothLeCallControl.RESULT_ERROR_UNKNOWN_CALL_ID;
                        } else {
                            call.hold();
                        }
                        mBluetoothLeCallControl.requestResult(requestId, result);
                    }
                }

                @Override
                public void onUnholdCall(int requestId, UUID callId) {
                    synchronized (LOCK) {
                        enforceModifyPermission();
                        Log.i(TAG, "TBS - unhold call=" + callId);
                        int result = BluetoothLeCallControl.RESULT_SUCCESS;
                        BluetoothCall call = mCallInfo.getCallByCallId(callId);
                        if (mCallInfo.isNullCall(call)) {
                            result = BluetoothLeCallControl.RESULT_ERROR_UNKNOWN_CALL_ID;
                        } else {
                            call.unhold();
                        }
                        mBluetoothLeCallControl.requestResult(requestId, result);
                    }
                }

                @Override
                public void onPlaceCall(int requestId, UUID callId, String uri) {
                    mBluetoothLeCallControl.requestResult(
                            requestId, BluetoothLeCallControl.RESULT_ERROR_APPLICATION);
                }

                @Override
                public void onJoinCalls(int requestId, @NonNull List<UUID> callIds) {
                    synchronized (LOCK) {
                        Log.i(TAG, "TBS - onJoinCalls");
                        int result = BluetoothLeCallControl.RESULT_SUCCESS;
                        List<UUID> alreadyJoinedCalls = new ArrayList<>();
                        BluetoothCall baseCallInstance = null;

                        if (callIds.size() < 2) {
                            Log.e(
                                    TAG,
                                    "TBS - onJoinCalls, join call number is invalid: "
                                            + callIds.size());
                            result = BluetoothLeCallControl.RESULT_ERROR_UNKNOWN_CALL_ID;
                            mBluetoothLeCallControl.requestResult(requestId, result);
                            return;
                        }

                        for (UUID callToJoinUuid : callIds) {
                            BluetoothCall callToJoinInstance =
                                    mCallInfo.getCallByCallId(callToJoinUuid);

                            /* Skip invalid and already add device */
                            if ((callToJoinInstance == null)
                                    || (alreadyJoinedCalls.contains(callToJoinUuid))) {
                                continue;
                            }

                            /* Lets make first valid call the base call */
                            if (baseCallInstance == null) {
                                baseCallInstance = callToJoinInstance;
                                alreadyJoinedCalls.add(callToJoinUuid);
                                continue;
                            }

                            baseCallInstance.conference(callToJoinInstance);
                            alreadyJoinedCalls.add(callToJoinUuid);
                        }

                        if ((baseCallInstance == null) || (alreadyJoinedCalls.size() < 2)) {
                            result = BluetoothLeCallControl.RESULT_ERROR_UNKNOWN_CALL_ID;
                        }

                        mBluetoothLeCallControl.requestResult(requestId, result);
                    }
                }
            };
}
