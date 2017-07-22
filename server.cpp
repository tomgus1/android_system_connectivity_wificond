/*
 * Copyright (C) 2016 The Android Open Source Project
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
 */

#include "wificond/server.h"

#include <sstream>
#include <iomanip>
#include <string.h>
#include <net/if.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <binder/IPCThreadState.h>
#include <binder/PermissionCache.h>

#include "qsap_api.h"
#include "wificond/logging_utils.h"
#include "wificond/net/netlink_utils.h"
#include "wificond/scanning/scan_utils.h"

using android::base::WriteStringToFd;
using android::binder::Status;
using android::sp;
using android::IBinder;
using android::net::wifi::IApInterface;
using android::net::wifi::IClientInterface;
using android::net::wifi::IInterfaceEventCallback;
using android::wifi_system::HostapdManager;
using android::wifi_system::InterfaceTool;
using android::wifi_system::SupplicantManager;

using std::endl;
using std::placeholders::_1;
using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;

namespace android {
namespace wificond {

namespace {

constexpr const char* kPermissionDump = "android.permission.DUMP";
constexpr const char* kBaseIfName = "wlan0";

}  // namespace

Server::Server(unique_ptr<InterfaceTool> if_tool,
               unique_ptr<SupplicantManager> supplicant_manager,
               unique_ptr<HostapdManager> hostapd_manager,
               NetlinkUtils* netlink_utils,
               ScanUtils* scan_utils)
    : base_ifname_(kBaseIfName),
      if_tool_(std::move(if_tool)),
      supplicant_manager_(std::move(supplicant_manager)),
      hostapd_manager_(std::move(hostapd_manager)),
      netlink_utils_(netlink_utils),
      scan_utils_(scan_utils) {
}

Status Server::RegisterCallback(const sp<IInterfaceEventCallback>& callback) {
  for (auto& it : interface_event_callbacks_) {
    if (IInterface::asBinder(callback) == IInterface::asBinder(it)) {
      LOG(WARNING) << "Ignore duplicate interface event callback registration";
      return Status::ok();
    }
  }
  LOG(INFO) << "New interface event callback registered";
  interface_event_callbacks_.push_back(callback);
  return Status::ok();
}

Status Server::UnregisterCallback(const sp<IInterfaceEventCallback>& callback) {
  for (auto it = interface_event_callbacks_.begin();
       it != interface_event_callbacks_.end();
       it++) {
    if (IInterface::asBinder(callback) == IInterface::asBinder(*it)) {
      interface_event_callbacks_.erase(it);
      LOG(INFO) << "Unregister interface event callback";
      return Status::ok();
    }
  }
  LOG(WARNING) << "Failed to find registered interface event callback"
               << " to unregister";
  return Status::ok();
}

Status Server::createApInterface(sp<IApInterface>* created_interface) {
  InterfaceInfo interface;
  if (!SetupInterface(&interface)) {
    return Status::ok();  // Logging was done internally
  }

  unique_ptr<ApInterfaceImpl> ap_interface(new ApInterfaceImpl(
      interface.name,
      interface.index,
      netlink_utils_,
      if_tool_.get(),
      hostapd_manager_.get(),
      this));
  *created_interface = ap_interface->GetBinder();
  ap_interfaces_.push_back(std::move(ap_interface));
  BroadcastApInterfaceReady(ap_interfaces_.back()->GetBinder());

  return Status::ok();
}

Status Server::createClientInterface(sp<IClientInterface>* created_interface) {
  InterfaceInfo interface;
  if (!SetupInterface(&interface)) {
    return Status::ok();  // Logging was done internally
  }

  unique_ptr<ClientInterfaceImpl> client_interface(new ClientInterfaceImpl(
      wiphy_index_,
      interface.name,
      interface.index,
      interface.mac_address,
      if_tool_.get(),
      supplicant_manager_.get(),
      netlink_utils_,
      scan_utils_));
  *created_interface = client_interface->GetBinder();
  client_interfaces_.push_back(std::move(client_interface));
  BroadcastClientInterfaceReady(client_interfaces_.back()->GetBinder());

  return Status::ok();
}

Status Server::tearDownInterfaces() {
  for (auto& it : client_interfaces_) {
    BroadcastClientInterfaceTornDown(it->GetBinder());
  }
  client_interfaces_.clear();

  for (auto& it : ap_interfaces_) {
    BroadcastApInterfaceTornDown(it->GetBinder());
  }
  ap_interfaces_.clear();

  MarkDownAllInterfaces();

  netlink_utils_->UnsubscribeRegDomainChange(wiphy_index_);

  return Status::ok();
}

Status Server::tearDownStaInterfaces() {
  for (auto& it : client_interfaces_) {
    BroadcastClientInterfaceTornDown(it->GetBinder());
  }
  client_interfaces_.clear();

  return Status::ok();
}

Status Server::tearDownApInterfaces() {
  for (auto& it : ap_interfaces_) {
    BroadcastApInterfaceTornDown(it->GetBinder());
  }
  ap_interfaces_.clear();

  return Status::ok();
}

Status Server::GetClientInterfaces(vector<sp<IBinder>>* out_client_interfaces) {
  vector<sp<android::IBinder>> client_interfaces_binder;
  for (auto& it : client_interfaces_) {
    out_client_interfaces->push_back(asBinder(it->GetBinder()));
  }
  return binder::Status::ok();
}

Status Server::GetApInterfaces(vector<sp<IBinder>>* out_ap_interfaces) {
  vector<sp<IBinder>> ap_interfaces_binder;
  for (auto& it : ap_interfaces_) {
    out_ap_interfaces->push_back(asBinder(it->GetBinder()));
  }
  return binder::Status::ok();
}

Status Server::setHostapdParam(
    const std::vector<uint8_t>& cmd,
    bool* out_success) {
  *out_success = false;
  const int max_arg_size = 10;
  char *data[max_arg_size];
  char **argv = data;
  int argc = 0;
  static sp<IApInterface> ap_interface = nullptr;
  stringstream ss;

  for (uint8_t b : cmd) {
    ss << b;
  }
  LOG(INFO) << "Command: " << ss.str() << " (len=" << cmd.size() << ")";

  /* Tokenize command to char array */
  std::istringstream buf(ss.str());
  std::istream_iterator<std::string> beg(buf), end;
  std::vector<std::string> tokens(beg, end);

  for(auto& s: tokens) {
    if (argc >= max_arg_size) {
        LOG(ERROR) << "Command too long";
        return binder::Status::ok();
    }
    data[argc] = strdup(s.c_str());
    argc++;
  }

  if (argc > 2) {
    if (!strcmp(argv[1], "qccmd")) {
      *out_success = qsap_hostd_exec(argc, argv) ? false : true;
    } else if (!strcmp(argv[1], "create") &&
               qsap_add_or_remove_interface(argv[2], 1)) {
      *out_success = true;
    } else if (!strcmp(argv[1], "remove") &&
               qsap_add_or_remove_interface(argv[2], 0)) {
      *out_success = true;
    } else if (!strcmp(argv[1], "bridge")) {
      *out_success = !qsap_control_bridge(argc, argv) ? true : false;
    } else if (!strcmp(argv[1], "startap") &&
               !strcmp(argv[2], "dual")) {
      if (argc < 6) {
        LOG(ERROR) << "Need additional args <bridge, sap0, sap1>";
        *out_success = false;
      } else {
        std::string bridge = (std::string) argv[3];
        std::string sap0 = (std::string) argv[4];
        std::string sap1 = (std::string) argv[5];
        sp<IApInterface> dual_ap_interface = nullptr;
        if (QcCreateApInterface(vector<uint8_t>(bridge.begin(), bridge.end()),
                                &ap_interface).isOk() &&
            QcCreateApInterface(vector<uint8_t>(sap0.begin(), sap0.end()),
                                &dual_ap_interface).isOk() &&
            QcCreateApInterface(vector<uint8_t>(sap1.begin(), sap1.end()),
                                &dual_ap_interface).isOk() &&
            ap_interface != nullptr &&
            ap_interface->startHostapd(true, out_success).isOk()) {
          *out_success = true;
          LOG(INFO) << "dual hostapd started";
        } else {
          *out_success = false;
          LOG(ERROR) << "Failed to start dual hostapd";
        }
      }
    } else if (!strcmp(argv[1], "stopap") &&
               !strcmp(argv[2], "dual")) {
      if (!ap_interfaces_.empty() &&
          ap_interface != nullptr &&
          ap_interface->stopHostapd(true, out_success).isOk()) {
        ap_interfaces_.clear();
        ap_interface = nullptr;
        *out_success = true;
        LOG(INFO) << "hostapd stopped";
      } else {
        *out_success = false;
        LOG(INFO) << "Failed to stop hostapd";
      }
    } else if (!strcmp(argv[1], "setsoftap")) {
      *out_success = qsapsetSoftap(argc, argv) ? false : true;
    }
  } else if (argc > 1) {
    if(!strcmp(argv[1], "startap") &&
        Server::createApInterface(&ap_interface).isOk() &&
        ap_interface != nullptr &&
        ap_interface->startHostapd(false, out_success).isOk()) {
      *out_success = true;
      LOG(INFO) << "hostapd started";
    } else if (!strcmp(argv[1], "stopap")) {
        if (!ap_interfaces_.empty() &&
            ap_interface != nullptr &&
            ap_interface->stopHostapd(false, out_success).isOk()) {
          ap_interfaces_.clear();
          ap_interface = nullptr;
          *out_success = true;
          LOG(INFO) << "hostapd stopped";
        } else {
          *out_success = false;
          LOG(INFO) << "Failed to stop hostapd";
        }
    }
  } else {
    LOG(ERROR) << "Wrong/Unknown command: " << ss.str();
  }

  return binder::Status::ok();
}

bool Server::QcSetupInterface(InterfaceInfo* interface, const char* ifname) {
  if (!RefreshWiphyIndex()) {
    return false;
  }

  netlink_utils_->SubscribeRegDomainChange(
          wiphy_index_,
          std::bind(&Server::OnRegDomainChanged,
          this,
          _1));

  interfaces_.clear();
  if (!netlink_utils_->GetInterfaces(wiphy_index_, &interfaces_)) {
    LOG(ERROR) << "Failed to get interfaces info from kernel";
    return false;
  }

  for (const auto& iface : interfaces_) {
    LOG(ERROR) << iface.name;
    if (android::base::StartsWith(iface.name, ifname)) {
      *interface = iface;
      return true;
    }
  }

  // Bridge interface is not part of GetInterfaces. Check this explicitly.
  uint32_t br_index = if_nametoindex(ifname);
  char mac_addr[7];
  if (!br_index) {
    LOG(ERROR) << "Failed to get requested interface " << strerror(errno);
  } else if (!linux_get_ifhwaddr(ifname, mac_addr)) {
    mac_addr[6] = '\0';
    std::string addr = std::string(mac_addr);
    *interface = InterfaceInfo(br_index, std::string(ifname),
                                  vector<uint8_t>(addr.begin(), addr.end()));
    LOG(INFO) << "Bridged iface found " << ifname;
    return true;
  }

  LOG(ERROR) << "No usable interface found with name " << ifname;
  return false;
}

Status Server::QcCreateApInterface(
    const std::vector<uint8_t>& ifname,
    sp<IApInterface>* created_interface) {
  InterfaceInfo interface;
  stringstream ss;

  for (uint8_t b : ifname) {
    ss << b;
  }
  if (!QcSetupInterface(&interface, (ss.str()).c_str())) {
    return Status::ok();  // Logging was done internally
  }

  unique_ptr<ApInterfaceImpl> ap_interface(new ApInterfaceImpl(
      interface.name,
      interface.index,
      netlink_utils_,
      if_tool_.get(),
      hostapd_manager_.get(),
      this));
  *created_interface = ap_interface->GetBinder();
  ap_interfaces_.push_back(std::move(ap_interface));
  BroadcastApInterfaceReady(ap_interfaces_.back()->GetBinder());

  return Status::ok();
}

status_t Server::dump(int fd, const Vector<String16>& /*args*/) {
  if (!PermissionCache::checkCallingPermission(String16(kPermissionDump))) {
    IPCThreadState* ipc = android::IPCThreadState::self();
    LOG(ERROR) << "Caller (uid: " << ipc->getCallingUid()
               << ") is not permitted to dump wificond state";
    return PERMISSION_DENIED;
  }

  stringstream ss;
  ss << "Current wiphy index: " << wiphy_index_ << endl;
  ss << "Cached interfaces list from kernel message: " << endl;
  for (const auto& iface : interfaces_) {
    ss << "Interface index: " << iface.index
       << ", name: " << iface.name
       << ", mac address: "
       << LoggingUtils::GetMacString(iface.mac_address) << endl;
  }

  for (const auto& iface : client_interfaces_) {
    iface->Dump(&ss);
  }

  for (const auto& iface : ap_interfaces_) {
    iface->Dump(&ss);
  }

  if (!WriteStringToFd(ss.str(), fd)) {
    PLOG(ERROR) << "Failed to dump state to fd " << fd;
    return FAILED_TRANSACTION;
  }

  return OK;
}

void Server::MarkDownAllInterfaces() {
  uint32_t wiphy_index;
  vector<InterfaceInfo> interfaces;
  if (netlink_utils_->GetWiphyIndexWithInterfaceName(base_ifname_, &wiphy_index) &&
      netlink_utils_->GetInterfaces(wiphy_index, &interfaces)) {
    for (InterfaceInfo& interface : interfaces) {
      if_tool_->SetUpState(interface.name.c_str(), false);
    }
  }
}

void Server::CleanUpSystemState() {
  supplicant_manager_->StopSupplicant();
  hostapd_manager_->StopHostapd(false);
  hostapd_manager_->StopHostapd(true);
  MarkDownAllInterfaces();
}

bool Server::SetupInterface(InterfaceInfo* interface) {
  if (!client_interfaces_.empty()) {
    // In the future we may support multiple interfaces at once.  However,
    // today, we support just one.
    LOG(ERROR) << "Cannot create STA interface when other interfaces exist";
    return false;
  }

  if (!RefreshWiphyIndex()) {
    return false;
  }

  netlink_utils_->SubscribeRegDomainChange(
          wiphy_index_,
          std::bind(&Server::OnRegDomainChanged,
          this,
          _1));

  interfaces_.clear();
  if (!netlink_utils_->GetInterfaces(wiphy_index_, &interfaces_)) {
    LOG(ERROR) << "Failed to get interfaces info from kernel";
    return false;
  }

  for (const auto& iface : interfaces_) {
    // Some kernel/driver uses station type for p2p interface.
    // In that case we can only rely on hard-coded name to exclude
    // p2p interface from station interfaces.
    // Currently NAN interfaces also use station type.
    // We should blacklist NAN interfaces as well.
    // Also we would not prefer to use softap* interfaces for Station
    // mode.
    if (iface.name != "p2p0" &&
        !android::base::StartsWith(iface.name, "aware_data") &&
        !android::base::StartsWith(iface.name, "softap")) {
      *interface = iface;
      return true;
    }
  }

  LOG(ERROR) << "No usable interface found";
  return false;
}

bool Server::RefreshWiphyIndex() {
  if (!netlink_utils_->GetWiphyIndexWithInterfaceName(base_ifname_, &wiphy_index_)) {
    LOG(ERROR) << "Failed to get wiphy index";
    return false;
  }
  return true;
}

void Server::OnRegDomainChanged(std::string& country_code) {
  if (country_code.empty()) {
    LOG(INFO) << "Regulatory domain changed";
  } else {
    LOG(INFO) << "Regulatory domain changed to country: " << country_code;
  }
  LogSupportedBands();
}

void Server::LogSupportedBands() {
  BandInfo band_info;
  ScanCapabilities scan_capabilities;
  WiphyFeatures wiphy_features;
  netlink_utils_->GetWiphyInfo(wiphy_index_,
                               &band_info,
                               &scan_capabilities,
                               &wiphy_features);

  stringstream ss;
  for (unsigned int i = 0; i < band_info.band_2g.size(); i++) {
    ss << " " << band_info.band_2g[i];
  }
  LOG(INFO) << "2.4Ghz frequencies:"<< ss.str();
  ss.str("");

  for (unsigned int i = 0; i < band_info.band_5g.size(); i++) {
    ss << " " << band_info.band_5g[i];
  }
  LOG(INFO) << "5Ghz non-DFS frequencies:"<< ss.str();
  ss.str("");

  for (unsigned int i = 0; i < band_info.band_dfs.size(); i++) {
    ss << " " << band_info.band_dfs[i];
  }
  LOG(INFO) << "5Ghz DFS frequencies:"<< ss.str();
}

void Server::BroadcastClientInterfaceReady(
    sp<IClientInterface> network_interface) {
  for (auto& it : interface_event_callbacks_) {
    it->OnClientInterfaceReady(network_interface);
  }
}

void Server::BroadcastApInterfaceReady(
    sp<IApInterface> network_interface) {
  for (auto& it : interface_event_callbacks_) {
    it->OnApInterfaceReady(network_interface);
  }
}

void Server::BroadcastClientInterfaceTornDown(
    sp<IClientInterface> network_interface) {
  for (auto& it : interface_event_callbacks_) {
    it->OnClientTorndownEvent(network_interface);
  }
}

void Server::BroadcastApInterfaceTornDown(
    sp<IApInterface> network_interface) {
  for (auto& it : interface_event_callbacks_) {
    it->OnApTorndownEvent(network_interface);
  }
}
void Server::BroadcastSoftApClientConnectStatus(const vector<uint8_t>& mac_address, bool connect_status) {
  for (auto& it : interface_event_callbacks_) {
    it->OnSoftApClientEvent(mac_address, connect_status);
  }
}


}  // namespace wificond
}  // namespace android
