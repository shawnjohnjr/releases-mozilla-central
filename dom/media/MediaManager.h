/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaEngine.h"
#include "mozilla/Services.h"
#include "nsIMediaManager.h"

#include "nsHashKeys.h"
#include "nsGlobalWindow.h"
#include "nsClassHashtable.h"
#include "nsRefPtrHashtable.h"
#include "nsObserverService.h"

#include "nsPIDOMWindow.h"
#include "nsIDOMNavigatorUserMedia.h"
#include "mozilla/Attributes.h"
#include "mozilla/StaticPtr.h"
#include "prlog.h"

namespace mozilla {

#ifdef PR_LOGGING
extern PRLogModuleInfo* GetMediaManagerLog();
#define MM_LOG(msg) PR_LOG(GetMediaManagerLog(), PR_LOG_DEBUG, msg)
#else
#define MM_LOG(msg)
#endif

class GetUserMediaNotificationEvent: public nsRunnable
{
  public:
    enum GetUserMediaStatus {
      STARTING,
      STOPPING
    };
    GetUserMediaNotificationEvent(GetUserMediaStatus aStatus)
    : mStatus(aStatus) {}

    NS_IMETHOD
    Run()
    {
      nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
      if (!obs) {
        NS_WARNING("Could not get the Observer service for GetUserMedia recording notification.");
        return NS_ERROR_FAILURE;
      }
      if (mStatus) {
        obs->NotifyObservers(nullptr,
            "recording-device-events",
            NS_LITERAL_STRING("starting").get());
      } else {
        obs->NotifyObservers(nullptr,
            "recording-device-events",
            NS_LITERAL_STRING("shutdown").get());
      }
      return NS_OK;
    }

  protected:
    GetUserMediaStatus mStatus;
};

/**
 * This class is an implementation of MediaStreamListener. This is used
 * to Start() and Stop() the underlying MediaEngineSource when MediaStreams
 * are assigned and deassigned in content.
 */
class GetUserMediaCallbackMediaStreamListener : public MediaStreamListener
{
public:
  GetUserMediaCallbackMediaStreamListener(nsIThread *aThread,
    nsDOMMediaStream* aStream,
    MediaEngineSource* aAudioSource,
    MediaEngineSource* aVideoSource)
    : mMediaThread(aThread)
    , mAudioSource(aAudioSource)
    , mVideoSource(aVideoSource)
    , mStream(aStream)
    , mSourceStream(aStream->GetStream()->AsSourceStream())
    , mLastEndTimeAudio(0)
    , mLastEndTimeVideo(0) { MOZ_ASSERT(mSourceStream); }

  ~GetUserMediaCallbackMediaStreamListener()
  {
    // In theory this could be released from the MediaStreamGraph thread (RemoveListener)
    if (mStream) {
      nsCOMPtr<nsIThread> mainThread = do_GetMainThread();
      nsDOMMediaStream *stream;
      mStream.forget(&stream);
      // Releases directly if on MainThread already
      NS_ProxyRelease(mainThread, stream, false);
    }
  }

  SourceMediaStream *GetSourceStream()
  {
    return mStream->GetStream()->AsSourceStream();
  }

  void
  Invalidate(); // implement in .cpp to avoid circular dependency with MediaOperationRunnable

  void
  Remove()
  {
    NS_ASSERTION(NS_IsMainThread(), "Only call on main thread");
    // Caller holds strong reference to us, so no death grip required
    mStream->GetStream()->RemoveListener(this);
  }

  // Proxy NotifyPull() to sources
  void
  NotifyPull(MediaStreamGraph* aGraph, StreamTime aDesiredTime)
  {
    // Currently audio sources ignore NotifyPull, but they could
    // watch it especially for fake audio.
    if (mAudioSource) {
      mAudioSource->NotifyPull(aGraph, mSourceStream, kAudioTrack, aDesiredTime, mLastEndTimeAudio);
    }
    if (mVideoSource) {
      mVideoSource->NotifyPull(aGraph, mSourceStream, kVideoTrack, aDesiredTime, mLastEndTimeVideo);
    }
  }

  void
  NotifyFinished(MediaStreamGraph* aGraph)
  {
    Invalidate();
    // XXX right now this calls Finish, which isn't ideal but doesn't hurt
  }

private:
  nsCOMPtr<nsIThread> mMediaThread;
  nsRefPtr<MediaEngineSource> mAudioSource;
  nsRefPtr<MediaEngineSource> mVideoSource;
  nsRefPtr<nsDOMMediaStream> mStream;
  SourceMediaStream *mSourceStream; // mStream controls ownership
  TrackTicks mLastEndTimeAudio;
  TrackTicks mLastEndTimeVideo;
};

typedef enum {
  MEDIA_START,
  MEDIA_STOP
} MediaOperation;

// Generic class for running long media operations like Start off the main
// thread, and then (because nsDOMMediaStreams aren't threadsafe),
// ProxyReleases mStream since it's cycle collected.
class MediaOperationRunnable : public nsRunnable
{
public:
  MediaOperationRunnable(MediaOperation aType,
    nsDOMMediaStream* aStream,
    MediaEngineSource* aAudioSource,
    MediaEngineSource* aVideoSource)
    : mType(aType)
    , mAudioSource(aAudioSource)
    , mVideoSource(aVideoSource)
    , mStream(aStream)
    {}

  MediaOperationRunnable(MediaOperation aType,
    GetUserMediaCallbackMediaStreamListener* aListener,
    MediaEngineSource* aAudioSource,
    MediaEngineSource* aVideoSource)
    : mType(aType)
    , mAudioSource(aAudioSource)
    , mVideoSource(aVideoSource)
    , mStream(nullptr)
    , mListener(aListener)
    {}

  ~MediaOperationRunnable()
  {
    // nsDOMMediaStreams are cycle-collected and thus main-thread-only for
    // refcounting and releasing
    if (mStream) {
      nsCOMPtr<nsIThread> mainThread = do_GetMainThread();
      nsDOMMediaStream *stream;
      mStream.forget(&stream);
      NS_ProxyRelease(mainThread, stream, true);
    }
  }

  NS_IMETHOD
  Run()
  {
    SourceMediaStream *source;
    // No locking between these is required as all the callbacks for the
    // same MediaStream will occur on the same thread.
    if (mStream) {
      source = mStream->GetStream()->AsSourceStream();
    } else {
      source = mListener->GetSourceStream();
    }
    MOZ_ASSERT(source);
    if (!source)  // paranoia
      return NS_ERROR_FAILURE;

    switch (mType) {
      case MEDIA_START:
        {
          NS_ASSERTION(!NS_IsMainThread(), "Never call on main thread");
          nsresult rv;

          source->SetPullEnabled(true);

          if (mAudioSource) {
            rv = mAudioSource->Start(source, kAudioTrack);
            if (NS_FAILED(rv)) {
              MM_LOG(("Starting audio failed, rv=%d",rv));
            }
          }
          if (mVideoSource) {
            rv = mVideoSource->Start(source, kVideoTrack);
            if (NS_FAILED(rv)) {
              MM_LOG(("Starting video failed, rv=%d",rv));
            }
          }

          MM_LOG(("started all sources"));
          nsRefPtr<GetUserMediaNotificationEvent> event =
            new GetUserMediaNotificationEvent(GetUserMediaNotificationEvent::STARTING);


          NS_DispatchToMainThread(event, NS_DISPATCH_NORMAL);
        }
        break;

      case MEDIA_STOP:
        {
          NS_ASSERTION(!NS_IsMainThread(), "Never call on main thread");
          if (mAudioSource) {
            mAudioSource->Stop(source, kAudioTrack);
            mAudioSource->Deallocate();
          }
          if (mVideoSource) {
            mVideoSource->Stop(source, kVideoTrack);
            mVideoSource->Deallocate();
          }
          // Do this after stopping all tracks with EndTrack()
          source->Finish();

          nsRefPtr<GetUserMediaNotificationEvent> event =
            new GetUserMediaNotificationEvent(GetUserMediaNotificationEvent::STOPPING);

          NS_DispatchToMainThread(event, NS_DISPATCH_NORMAL);
        }
        break;

      default:
        MOZ_ASSERT(false,"invalid MediaManager operation");
        break;
    }
    return NS_OK;
  }

private:
  MediaOperation mType;
  nsRefPtr<MediaEngineSource> mAudioSource; // threadsafe
  nsRefPtr<MediaEngineSource> mVideoSource; // threadsafe
  nsRefPtr<nsDOMMediaStream> mStream;       // not threadsafe
  nsRefPtr<GetUserMediaCallbackMediaStreamListener> mListener; // threadsafe
};

typedef nsTArray<nsRefPtr<GetUserMediaCallbackMediaStreamListener> > StreamListeners;
typedef nsClassHashtable<nsUint64HashKey, StreamListeners> WindowTable;

class MediaDevice : public nsIMediaDevice
{
public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIMEDIADEVICE

  MediaDevice(MediaEngineVideoSource* aSource) {
    mSource = aSource;
    mType.Assign(NS_LITERAL_STRING("video"));
    mSource->GetName(mName);
    mSource->GetUUID(mID);
  }
  MediaDevice(MediaEngineAudioSource* aSource) {
    mSource = aSource;
    mType.Assign(NS_LITERAL_STRING("audio"));
    mSource->GetName(mName);
    mSource->GetUUID(mID);
  }
  virtual ~MediaDevice() {}

  MediaEngineSource* GetSource();
private:
  nsString mName;
  nsString mType;
  nsString mID;
  nsRefPtr<MediaEngineSource> mSource;
};

class MediaManager MOZ_FINAL : public nsIMediaManagerService,
                               public nsIObserver
{
public:
  static already_AddRefed<MediaManager> GetInstance();

  static MediaManager* Get() {
    if (!sSingleton) {
      sSingleton = new MediaManager();

      NS_NewThread(getter_AddRefs(sSingleton->mMediaThread));
      MM_LOG(("New Media thread for gum"));

      NS_ASSERTION(NS_IsMainThread(), "Only create MediaManager on main thread");
      nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
      obs->AddObserver(sSingleton, "xpcom-shutdown", false);
      obs->AddObserver(sSingleton, "getUserMedia:response:allow", false);
      obs->AddObserver(sSingleton, "getUserMedia:response:deny", false);
    }
    return sSingleton;
  }
  static nsIThread* GetThread() {
    return Get()->mMediaThread;
  }

  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER
  NS_DECL_NSIMEDIAMANAGERSERVICE

  MediaEngine* GetBackend();
  StreamListeners *GetWindowListeners(uint64_t aWindowId) {
    NS_ASSERTION(NS_IsMainThread(), "Only access windowlist on main thread");

    return mActiveWindows.Get(aWindowId);
  }
  bool IsWindowStillActive(uint64_t aWindowId) {
    return !!GetWindowListeners(aWindowId);
  }

  nsresult GetUserMedia(bool aPrivileged, nsPIDOMWindow* aWindow,
    nsIMediaStreamOptions* aParams,
    nsIDOMGetUserMediaSuccessCallback* onSuccess,
    nsIDOMGetUserMediaErrorCallback* onError);
  nsresult GetUserMediaDevices(nsPIDOMWindow* aWindow,
    nsIGetUserMediaDevicesSuccessCallback* onSuccess,
    nsIDOMGetUserMediaErrorCallback* onError);
  void OnNavigation(uint64_t aWindowID);

private:
  WindowTable *GetActiveWindows() {
    NS_ASSERTION(NS_IsMainThread(), "Only access windowlist on main thread");
    return &mActiveWindows;
  }

  // Make private because we want only one instance of this class
  MediaManager()
  : mMediaThread(nullptr)
  , mMutex("mozilla::MediaManager")
  , mBackend(nullptr) {
    mActiveWindows.Init();
    mActiveCallbacks.Init();
  }

  ~MediaManager() {
    delete mBackend;
  }

  // ONLY access from MainThread so we don't need to lock
  WindowTable mActiveWindows;
  nsRefPtrHashtable<nsStringHashKey, nsRunnable> mActiveCallbacks;
  // Always exists
  nsCOMPtr<nsIThread> mMediaThread;

  Mutex mMutex;
  // protected with mMutex:
  MediaEngine* mBackend;

  static StaticRefPtr<MediaManager> sSingleton;
};

} // namespace mozilla
