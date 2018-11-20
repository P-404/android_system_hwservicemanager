#pragma once

#include <vintf/Transport.h>

#include <set>
#include <string>

namespace android {
namespace hardware {

// Get transport method from vendor interface manifest.
// interfaceName has the format "android.hardware.foo@1.0::IFoo"
// instanceName is "default", "ashmem", etc.
// If it starts with "android.hidl.", a static map is looked up instead.
vintf::Transport getTransport(const std::string &interfaceName,
                              const std::string &instanceName);

// All HALs on the device in manifests.
std::set<std::string> getInstances(const std::string& interfaceName);

}  // hardware
}  // android
