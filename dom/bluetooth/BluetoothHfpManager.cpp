/* -*- Mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; tab-width: 40 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BluetoothHfpManager.h"

#include "BluetoothReplyRunnable.h"
#include "BluetoothService.h"
#include "BluetoothServiceUuid.h"

#include "mozilla/Services.h"
#include "nsIObserverService.h"
#include "nsIRadioInterfaceLayer.h"

#include <unistd.h> /* usleep() */

USING_BLUETOOTH_NAMESPACE

static BluetoothHfpManager* sInstance = nullptr;
static nsCOMPtr<nsIThread> sHfpCommandThread;
static bool sStopSendingRingFlag = true;

static int kRingInterval = 3000000;  //unit: us

class SendRingIndicatorTask : public nsRunnable
{
public:
  SendRingIndicatorTask()
  {
    MOZ_ASSERT(NS_IsMainThread());
  }

  NS_IMETHOD Run()
  {
    MOZ_ASSERT(!NS_IsMainThread());

    while (!sStopSendingRingFlag) {
      sInstance->SendLine("RING");

      usleep(kRingInterval);
    }

    return NS_OK;
  }
};

BluetoothHfpManager::BluetoothHfpManager()
  : mCurrentVgs(-1)
  , mCurrentCallIndex(0)
  , mCurrentCallState(nsIRadioInterfaceLayer::CALL_STATE_DISCONNECTED)
{
}

BluetoothHfpManager::~BluetoothHfpManager()
{
}

//static
BluetoothHfpManager*
BluetoothHfpManager::Get()
{
  if (sInstance == nullptr) {
    sInstance = new BluetoothHfpManager();
  }

  return sInstance;
}

// Virtual function of class SocketConsumer
void
BluetoothHfpManager::ReceiveSocketData(mozilla::ipc::UnixSocketRawData* aMessage)
{
  const char* msg = (const char*)aMessage->mData;

  // For more information, please refer to 4.34.1 "Bluetooth Defined AT
  // Capabilities" in Bluetooth hands-free profile 1.6
  if (!strncmp(msg, "AT+BRSF=", 8)) {
    SendLine("+BRSF: 23");
    SendLine("OK");
  } else if (!strncmp(msg, "AT+CIND=?", 9)) {
    nsAutoCString cindRange;

    cindRange += "+CIND: ";
    cindRange += "(\"battchg\",(0-5)),";
    cindRange += "(\"signal\",(0-5)),";
    cindRange += "(\"service\",(0,1)),";
    cindRange += "(\"call\",(0,1)),";
    cindRange += "(\"callsetup\",(0-3)),";
    cindRange += "(\"callheld\",(0-2)),";
    cindRange += "(\"roam\",(0,1))";

    SendLine(cindRange.get());
    SendLine("OK");
  } else if (!strncmp(msg, "AT+CIND", 7)) {
    // TODO(Eric)
    // This value reflects current status of telephony, roaming, battery ...,
    // so obviously fixed value must be wrong here. Need a patch for this.
    SendLine("+CIND: 5,5,1,0,0,0,0");
    SendLine("OK");
  } else if (!strncmp(msg, "AT+CMER=", 8)) {
    SendLine("OK");
  } else if (!strncmp(msg, "AT+CHLD=?", 9)) {
    SendLine("+CHLD: (0,1,2,3)");
    SendLine("OK");
  } else if (!strncmp(msg, "AT+CHLD=", 8)) {
    SendLine("OK");
  } else if (!strncmp(msg, "AT+VGS=", 7)) {
    // VGS range: [0, 15]
    int newVgs = msg[7] - '0';

    if (strlen(msg) > 8) {
      newVgs *= 10;
      newVgs += (msg[8] - '0');
    }

#ifdef DEBUG
    NS_ASSERTION(newVgs >= 0 && newVgs <= 15, "Received invalid VGS value");
#endif

    // Currently, we send volume up/down commands to represent that
    // volume has been changed by Bluetooth headset, and that will affect
    // the main stream volume of our device. In the future, we may want to
    // be able to set volume by stream.
    nsCOMPtr<nsIObserverService> os = mozilla::services::GetObserverService();
    if (newVgs > mCurrentVgs) {
      os->NotifyObservers(nullptr, "bluetooth-volume-up", nullptr);
    } else if (newVgs < mCurrentVgs) {
      os->NotifyObservers(nullptr, "bluetooth-volume-down", nullptr);
    }

    mCurrentVgs = newVgs;

    SendLine("OK");
  } else {
#ifdef DEBUG
    nsCString warningMsg;
    warningMsg.AssignLiteral("Not handling HFP message, reply ok: ");
    warningMsg.Append(msg);
    NS_WARNING(warningMsg.get());
#endif
    SendLine("OK");
  }
}

bool
BluetoothHfpManager::Connect(const nsAString& aDeviceObjectPath,
                             BluetoothReplyRunnable* aRunnable)
{
  MOZ_ASSERT(NS_IsMainThread());

  BluetoothService* bs = BluetoothService::Get();
  if (!bs) {
    NS_WARNING("BluetoothService not available!");
    return false;
  }

  nsString serviceUuidStr =
    NS_ConvertUTF8toUTF16(mozilla::dom::bluetooth::BluetoothServiceUuidStr::Handsfree);

  nsRefPtr<BluetoothReplyRunnable> runnable = aRunnable;

  nsresult rv = bs->GetSocketViaService(aDeviceObjectPath,
                                        serviceUuidStr,
                                        BluetoothSocketType::RFCOMM,
                                        true,
                                        false,
                                        this,
                                        runnable);

  runnable.forget();
  return NS_FAILED(rv) ? false : true;
}

bool
BluetoothHfpManager::Disconnect(BluetoothReplyRunnable* aRunnable)
{
  MOZ_ASSERT(NS_IsMainThread());

  // BluetoothService* bs = BluetoothService::Get();
  // if (!bs) {
  //   NS_WARNING("BluetoothService not available!");
  //   return false;
  // }

  // nsRefPtr<BluetoothReplyRunnable> runnable = aRunnable;

  nsresult rv = NS_OK;
  CloseSocket();
  return NS_FAILED(rv) ? false : true;
}

void
BluetoothHfpManager::SendLine(const char* aMessage)
{
  const char* kHfpCrlf = "\xd\xa";
  nsAutoCString msg;

  msg += kHfpCrlf;
  msg += aMessage;
  msg += kHfpCrlf;

  SendSocketData(msg);
}

/*
 * CallStateChanged will be called whenever call status is changed, and it 
 * also means we need to notify HS about the change. For more information, 
 * please refer to 4.13 ~ 4.15 in Bluetooth hands-free profile 1.6.
 */
void
BluetoothHfpManager::CallStateChanged(int aCallIndex, int aCallState,
                                      const char* aNumber, bool aIsActive)
{
  nsRefPtr<nsRunnable> sendRingTask;

  switch (aCallState) {
    case nsIRadioInterfaceLayer::CALL_STATE_INCOMING:
      // Send "CallSetup = 1"
      SendLine("+CIEV: 5,1");

      // Start sending RING indicator to HF
      sStopSendingRingFlag = false;
      sendRingTask = new SendRingIndicatorTask();
      sHfpCommandThread->Dispatch(sendRingTask, NS_DISPATCH_NORMAL);
      break;
    case nsIRadioInterfaceLayer::CALL_STATE_DIALING:
      // Send "CallSetup = 2"
      SendLine("+CIEV: 5,2");
      break;
    case nsIRadioInterfaceLayer::CALL_STATE_ALERTING:
      // Send "CallSetup = 3"
      if (mCurrentCallIndex == nsIRadioInterfaceLayer::CALL_STATE_DIALING) {
        SendLine("+CIEV: 5,3");
      } else {
#ifdef DEBUG
        NS_WARNING("%s: Impossible state changed from %d to %d",
                   __FUNCTION__, mCurrentCallState, aCallState);
#endif
      }
      break;
    case nsIRadioInterfaceLayer::CALL_STATE_CONNECTED:
      switch (mCurrentCallState) {
        case nsIRadioInterfaceLayer::CALL_STATE_INCOMING:
          sStopSendingRingFlag = true;
          // Continue executing, no break
        case nsIRadioInterfaceLayer::CALL_STATE_DIALING:
          // Send "Call = 1, CallSetup = 0"
          SendLine("+CIEV: 4,1");
          SendLine("+CIEV: 5,0");
          break;
        default:
#ifdef DEBUG
          NS_WARNING("%s: Impossible state changed from %d to %d",
                     __FUNCTION__, mCurrentCallState, aCallState);
#endif
          break;
      }

      break;
    case nsIRadioInterfaceLayer::CALL_STATE_DISCONNECTED:
      switch (mCurrentCallState) {
        case nsIRadioInterfaceLayer::CALL_STATE_INCOMING:
          sStopSendingRingFlag = true;
          // Continue executing, no break
        case nsIRadioInterfaceLayer::CALL_STATE_DIALING:
        case nsIRadioInterfaceLayer::CALL_STATE_ALERTING:
          // Send "CallSetup = 0"
          SendLine("+CIEV: 5,0");
          break;
        case nsIRadioInterfaceLayer::CALL_STATE_CONNECTED:
          // Send "Call = 0"
          SendLine("+CIEV: 4,0");
          break;
        default:
#ifdef DEBUG
          NS_WARNING("%s: Impossible state changed from %d to %d",
                     __FUNCTION__, mCurrentCallState, aCallState);
#endif
          break;
      }
      break;

    default:
#ifdef DEBUG
      NS_WARNING("%s: current state:%d, not handling state: %d",
                 __FUNCTION__, mCurrentCallState, aCallState);
#endif
      break;
  }

  mCurrentCallIndex = aCallIndex;
  mCurrentCallState = aCallState;
}
