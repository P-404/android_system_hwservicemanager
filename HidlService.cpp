#define LOG_TAG "hwservicemanager"
#include "HidlService.h"

#include <android-base/logging.h>
#include <hidl/HidlTransportSupport.h>
#include <hwbinder/BpHwBinder.h>
#include <sstream>

using ::android::hardware::interfacesEqual;

namespace android {
namespace hidl {
namespace manager {
namespace implementation {

HidlService::HidlService(
    const std::string &interfaceName,
    const std::string &instanceName,
    const sp<IBase> &service,
    pid_t pid)
: mInterfaceName(interfaceName),
  mInstanceName(instanceName),
  mService(service),
  mPid(pid)
{}

sp<IBase> HidlService::getService() const {
    return mService;
}
void HidlService::setService(sp<IBase> service, pid_t pid) {
    mService = service;
    mPid = pid;

    mClientCallbacks.clear();

    sendRegistrationNotifications();
}

pid_t HidlService::getPid() const {
    return mPid;
}
const std::string &HidlService::getInterfaceName() const {
    return mInterfaceName;
}
const std::string &HidlService::getInstanceName() const {
    return mInstanceName;
}

void HidlService::addListener(const sp<IServiceNotification> &listener) {
    if (mService != nullptr) {
        auto ret = listener->onRegistration(
            mInterfaceName, mInstanceName, true /* preexisting */);
        if (!ret.isOk()) {
            LOG(ERROR) << "Not adding listener for " << mInterfaceName << "/"
                       << mInstanceName << ": transport error when sending "
                       << "notification for already registered instance.";
            return;
        }
    }
    mListeners.push_back(listener);
}

bool HidlService::removeListener(const wp<IBase>& listener) {
    bool found = false;

    for (auto it = mListeners.begin(); it != mListeners.end();) {
        if (interfacesEqual(*it, listener.promote())) {
            it = mListeners.erase(it);
            found = true;
        } else {
            ++it;
        }
    }

    return found;
}

void HidlService::registerPassthroughClient(pid_t pid) {
    mPassthroughClients.insert(pid);
}

const std::set<pid_t> &HidlService::getPassthroughClients() const {
    return mPassthroughClients;
}

void HidlService::addClientCallback(const sp<IClientCallback>& callback) {
    mClientCallbacks.push_back(callback);
}

bool HidlService::removeClientCallback(const sp<IClientCallback>& callback) {
    bool found = false;

    for (auto it = mClientCallbacks.begin(); it != mClientCallbacks.end();) {
        if (interfacesEqual(*it, callback)) {
            it = mClientCallbacks.erase(it);
            found = true;
        } else {
            ++it;
        }
    }

    return found;
}

void HidlService::handleClientCallbacks() {
    using ::android::hardware::toBinder;
    using ::android::hardware::BpHwBinder;
    using ::android::hardware::IBinder;

    if (mClientCallbacks.empty()) return;
    if (mService == nullptr) return;

    // this justifies the bp cast below, no in-process HALs need this
    if (!mService->isRemote()) return;

    sp<IBinder> binder = toBinder(mService);
    if (binder == nullptr) return;

    sp<BpHwBinder> bpBinder = static_cast<BpHwBinder*>(binder.get());
    ssize_t count = bpBinder->getNodeStrongRefCount();

    // binder driver doesn't support this feature
    if (count == -1) return;

    bool hasClients = count > 1; // this process holds a strong count

    if (hasClients != mHasClients) {
        LOG(INFO) << "Notifying " << string() << " they have clients: " << hasClients;

        for (const auto& cb : mClientCallbacks) {
            Return<void> ret = cb->onClients(getService(), hasClients);
            if (!ret.isOk()) {
                LOG(WARNING) << "onClients callback failed for " << string() << ": " << ret.description();
            }
        }
    }

    mHasClients = hasClients;
}

std::string HidlService::string() const {
    std::stringstream ss;
    ss << mInterfaceName << "/" << mInstanceName;
    return ss.str();
}

void HidlService::sendRegistrationNotifications() {
    if (mListeners.size() == 0 || mService == nullptr) {
        return;
    }

    hidl_string iface = mInterfaceName;
    hidl_string name = mInstanceName;

    for (auto it = mListeners.begin(); it != mListeners.end();) {
        auto ret = (*it)->onRegistration(iface, name, false /* preexisting */);
        if (ret.isOk()) {
            ++it;
        } else {
            LOG(ERROR) << "Dropping registration callback for " << iface << "/" << name
                       << ": transport error.";
            it = mListeners.erase(it);
        }
    }
}

}  // namespace implementation
}  // namespace manager
}  // namespace hidl
}  // namespace android
