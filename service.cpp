#define LOG_TAG "hwservicemanager"

#include <utils/Log.h>

#include <inttypes.h>
#include <unistd.h>
#include <sys/timerfd.h>

#include <android/hidl/token/1.0/ITokenManager.h>
#include <cutils/properties.h>
#include <hidl/HidlBinderSupport.h>
#include <hidl/HidlTransportSupport.h>
#include <hidl/Status.h>
#include <hwbinder/IPCThreadState.h>
#include <hwbinder/ProcessState.h>
#include <utils/Errors.h>
#include <utils/Looper.h>
#include <utils/StrongPointer.h>

#include "ServiceManager.h"
#include "TokenManager.h"

// libutils:
using android::sp;
using android::Looper;
using android::LooperCallback;

// libhwbinder:
using android::hardware::BHwBinder;
using android::hardware::IBinder;
using android::hardware::IPCThreadState;
using android::hardware::ProcessState;

// libhidl
using android::hardware::handleTransportPoll;
using android::hardware::setRequestingSid;
using android::hardware::setupTransportPolling;
using android::hardware::toBinder;

// implementations
using android::hidl::manager::implementation::ServiceManager;
using android::hidl::manager::V1_0::IServiceManager;
using android::hidl::token::V1_0::implementation::TokenManager;

static std::string serviceName = "default";

class HwBinderCallback : public LooperCallback {
public:
    static sp<HwBinderCallback> setupTo(const sp<Looper>& looper) {
        sp<HwBinderCallback> cb = new HwBinderCallback;

        int fdHwBinder = setupTransportPolling();
        LOG_ALWAYS_FATAL_IF(fdHwBinder < 0, "Failed to setupTransportPolling: %d", fdHwBinder);

        // Flush after setupPolling(), to make sure the binder driver
        // knows about this thread handling commands.
        IPCThreadState::self()->flushCommands();

        int ret = looper->addFd(fdHwBinder,
                                Looper::POLL_CALLBACK,
                                Looper::EVENT_INPUT,
                                cb,
                                nullptr /*data*/);
        LOG_ALWAYS_FATAL_IF(ret != 1, "Failed to add binder FD to Looper");

        return cb;
    }

    int handleEvent(int fd, int /*events*/, void* /*data*/) override {
        handleTransportPoll(fd);
        return 1;  // Continue receiving callbacks.
    }
};

// LooperCallback for IClientCallback
class ClientCallbackCallback : public LooperCallback {
public:
    static sp<ClientCallbackCallback> setupTo(const sp<Looper>& looper, const sp<ServiceManager>& manager) {
        sp<ClientCallbackCallback> cb = new ClientCallbackCallback(manager);

        int fdTimer = timerfd_create(CLOCK_MONOTONIC, 0 /*flags*/);
        LOG_ALWAYS_FATAL_IF(fdTimer < 0, "Failed to timerfd_create: fd: %d err: %d", fdTimer, errno);

        itimerspec timespec {
            .it_interval = {
                .tv_sec = 5,
                .tv_nsec = 0,
            },
            .it_value = {
                .tv_sec = 5,
                .tv_nsec = 0,
            },
        };

        int timeRes = timerfd_settime(fdTimer, 0 /*flags*/, &timespec, nullptr);
        LOG_ALWAYS_FATAL_IF(timeRes < 0, "Failed to timerfd_settime: res: %d err: %d", timeRes, errno);

        int addRes = looper->addFd(fdTimer,
                                   Looper::POLL_CALLBACK,
                                   Looper::EVENT_INPUT,
                                   cb,
                                   nullptr);
        LOG_ALWAYS_FATAL_IF(addRes != 1, "Failed to add client callback FD to Looper");

        return cb;
    }

    int handleEvent(int fd, int /*events*/, void* /*data*/) override {
        uint64_t expirations;
        int ret = read(fd, &expirations, sizeof(expirations));
        if (ret != sizeof(expirations)) {
            ALOGE("Read failed to callback FD: ret: %d err: %d", ret, errno);
        }

        mManager->handleClientCallbacks();
        return 1;  // Continue receiving callbacks.
    }
private:
    ClientCallbackCallback(const sp<ServiceManager>& manager) : mManager(manager) {}
    sp<ServiceManager> mManager;
};

int main() {
    sp<ServiceManager> manager = new ServiceManager();
    setRequestingSid(manager, true);

    if (!manager->add(serviceName, manager)) {
        ALOGE("Failed to register hwservicemanager with itself.");
    }

    sp<TokenManager> tokenManager = new TokenManager();
    if (!manager->add(serviceName, tokenManager)) {
        ALOGE("Failed to register ITokenManager with hwservicemanager.");
    }

    // Tell IPCThreadState we're the service manager
    sp<IBinder> binder = toBinder<IServiceManager>(manager);
    sp<BHwBinder> service = static_cast<BHwBinder*>(binder.get()); // local binder object
    IPCThreadState::self()->setTheContextObject(service);
    // Then tell the kernel
    ProcessState::self()->becomeContextManager(nullptr, nullptr);

    int rc = property_set("hwservicemanager.ready", "true");
    if (rc) {
        ALOGE("Failed to set \"hwservicemanager.ready\" (error %d). "\
              "HAL services will not start!\n", rc);
    }

    sp<Looper> looper = Looper::prepare(0 /* opts */);

    (void)HwBinderCallback::setupTo(looper);
    (void)ClientCallbackCallback::setupTo(looper, manager);

    ALOGI("hwservicemanager is ready now.");

    while (true) {
        looper->pollAll(-1 /* timeoutMillis */);
    }

    return 0;
}
