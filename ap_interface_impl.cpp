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

#include "wificond/ap_interface_impl.h"

#include <android-base/logging.h>

#include "wificond/net/netlink_utils.h"

#include "wificond/ap_interface_binder.h"
#include "wificond/logging_utils.h"
#include <sstream>
#include <iomanip>
#include "qsap_api.h"

using android::net::wifi::IApInterface;
using android::wifi_system::HostapdManager;
using android::wifi_system::InterfaceTool;
using std::endl;
using std::string;
using std::unique_ptr;
using std::vector;

using EncryptionType = android::wifi_system::HostapdManager::EncryptionType;

using namespace std::placeholders;

namespace android {
namespace wificond {

ApInterfaceImpl::ApInterfaceImpl(const string& interface_name,
                                 uint32_t interface_index,
                                 NetlinkUtils* netlink_utils,
                                 InterfaceTool* if_tool,
                                 HostapdManager* hostapd_manager)
    : interface_name_(interface_name),
      interface_index_(interface_index),
      netlink_utils_(netlink_utils),
      if_tool_(if_tool),
      hostapd_manager_(hostapd_manager),
      binder_(new ApInterfaceBinder(this)),
      number_of_associated_stations_(0) {
  // This log keeps compiler happy.
  LOG(DEBUG) << "Created ap interface " << interface_name_
             << " with index " << interface_index_;

  netlink_utils_->SubscribeStationEvent(
      interface_index_,
      std::bind(&ApInterfaceImpl::OnStationEvent,
                this,
                _1, _2));
}

ApInterfaceImpl::~ApInterfaceImpl() {
  binder_->NotifyImplDead();
  if_tool_->SetUpState(interface_name_.c_str(), false);
  netlink_utils_->UnsubscribeStationEvent(interface_index_);
}

sp<IApInterface> ApInterfaceImpl::GetBinder() const {
  return binder_;
}

void ApInterfaceImpl::Dump(std::stringstream* ss) const {
  *ss << "------- Dump of AP interface with index: "
      << interface_index_ << " and name: " << interface_name_
      << "-------" << endl;
  *ss << "Number of associated stations: "
      <<  number_of_associated_stations_ << endl;
  *ss << "------- Dump End -------" << endl;
}

bool ApInterfaceImpl::StartHostapd() {
  return hostapd_manager_->StartHostapd();
}

bool ApInterfaceImpl::StopHostapd() {
  // Drop SIGKILL on hostapd.
  if (!hostapd_manager_->StopHostapd()) {
    // Logging was done internally.
    return false;
  }

  // Take down the interface.
  if (!if_tool_->SetUpState(interface_name_.c_str(), false)) {
    // Logging was done internally.
    return false;
  }

  // Since wificond SIGKILLs hostapd, hostapd has no chance to handle
  // the cleanup.
  // Besides taking down the interface, we also need to set the interface mode
  // back to station mode for the cleanup.
  if (!netlink_utils_->SetInterfaceMode(interface_index_,
                                        NetlinkUtils::STATION_MODE)) {
    LOG(ERROR) << "Failed to set interface back to station mode";
    return false;
  }

  return true;
}

#ifndef CONFIG_QSAP_SUPPORT
bool ApInterfaceImpl::WriteHostapdConfig(const vector<uint8_t>& ssid,
                                         bool is_hidden,
                                         int32_t channel,
                                         EncryptionType encryption_type,
                                         const vector<uint8_t>& passphrase) {
  string config = hostapd_manager_->CreateHostapdConfig(
      interface_name_, ssid, is_hidden, channel, encryption_type, passphrase);

  if (config.empty()) {
    return false;
  }

  return hostapd_manager_->WriteHostapdConfig(config);
}
#else

bool ApInterfaceImpl::QcWriteHostapdConfig(const vector<uint8_t>& ssid,
                                           bool is_hidden,
                                           int32_t channel,
                                           EncryptionType encryption_type,
                                           const vector<uint8_t>& passphrase) {
  int argc = 0;
  char *data[10];
  char **argv = data;
  std::stringstream ss;
  char *start,*end;
  char ctrl_interface[255];
  char cmdbuf[255];
  char respbuf[255];
  uint32_t  rlen = 255;

  for (uint8_t b : ssid) {
    ss << b;
  }
  // ASCII ssid string.
  const string ssid_as_string  = ss.str();

  ss.str(string());  // clear ss buffer.
  for (uint8_t b : passphrase) {
    ss << b;
  }
  // ASCII passphrase string.
  const string passphrase_as_string  = ss.str();

  /* softap setsoftap <optional dual2g/5g> <interface> <ssid/ssid2> <hidden/visible> <channel> <open/wep/wpa-psk/wpa2-psk> <wpa_passphrase> <max_num_sta> */
  data[argc++] = strdup("softap");
  data[argc++] = strdup("setsoftap");
  data[argc++] = strdup(interface_name_.c_str());
  data[argc++] = strdup(ssid_as_string.c_str());
  data[argc++] = is_hidden ? strdup("hidden") : strdup("visible");
  data[argc++] = strdup(std::to_string(channel).c_str());

  switch (encryption_type) {
    case EncryptionType::kOpen:
      data[argc++] = strdup("open");
      break;
    case EncryptionType::kWpa:
      data[argc++] = strdup("wpa-psk");
      data[argc++] = strdup(passphrase_as_string.c_str());
      break;
    case EncryptionType::kWpa2:
      data[argc++] = strdup("wpa2-psk");
      data[argc++] = strdup(passphrase_as_string.c_str());
      break;
  }
  bool status = !qsapsetSoftap(argc, argv);

/* Writing ctlr_interface path in config file */
  string config = hostapd_manager_->CreateHostapdConfig(
      interface_name_, ssid, is_hidden, channel, encryption_type, passphrase);
  start = strstr((char*)config.c_str(),"ctrl_interface");
  end = strchr(start,'\n');
  strncpy(ctrl_interface, start, (end - start));
  snprintf(cmdbuf, 255," set %s",ctrl_interface);
  (void) qsap_hostd_exec_cmd(cmdbuf, respbuf, &rlen);
  if(strncmp("success", respbuf, rlen) != 0) {
      LOG(INFO) << "Failed to set ctrl_interface \n";
  }

  // done with data, free it now.
  while (argc--)
    free(data[argc]);

  return status;
}
#endif

void ApInterfaceImpl::OnStationEvent(StationEvent event,
                                     const vector<uint8_t>& mac_address) {
  if (event == NEW_STATION) {
    LOG(INFO) << "New station "
              << LoggingUtils::GetMacString(mac_address)
              << " associated with hotspot";
    number_of_associated_stations_++;
  } else if (event == DEL_STATION) {
    LOG(INFO) << "Station "
              << LoggingUtils::GetMacString(mac_address)
              << " disassociated from hotspot";
    if (number_of_associated_stations_ <= 0) {
      LOG(ERROR) << "Received DEL_STATION event when station counter is: "
                 << number_of_associated_stations_;
    } else {
      number_of_associated_stations_--;
    }
  }
}

int ApInterfaceImpl::GetNumberOfAssociatedStations() const {
  return number_of_associated_stations_;
}

}  // namespace wificond
}  // namespace android
