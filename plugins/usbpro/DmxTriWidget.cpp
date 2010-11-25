/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * DmxTriWidget.h
 * The Jese DMX TRI device.
 * Copyright (C) 2010 Simon Newton
 */

#include <string.h>
#include <algorithm>
#include <map>
#include <string>
#include "ola/BaseTypes.h"
#include "ola/Logging.h"
#include "ola/network/NetworkUtils.h"
#include "ola/rdm/RDMCommand.h"
#include "ola/rdm/RDMEnums.h"
#include "ola/rdm/UID.h"
#include "ola/rdm/UIDSet.h"
#include "plugins/usbpro/DmxTriWidget.h"
#include "plugins/usbpro/UsbWidget.h"

namespace ola {
namespace plugin {
namespace usbpro {

using std::map;
using std::string;
using ola::network::NetworkToHost;
using ola::network::HostToNetwork;
using ola::rdm::RDMCommand;
using ola::rdm::RDMRequest;
using ola::rdm::UID;
using ola::rdm::UIDSet;


/*
 * New DMX TRI device
 */
DmxTriWidget::DmxTriWidget(ola::network::SelectServerInterface *ss,
                           UsbWidget *widget):
    m_ss(ss),
    m_widget(widget),
    m_rdm_timeout_id(ola::network::INVALID_TIMEOUT),
    m_uid_count(0),
    m_rdm_request_pending(false),
    m_last_esta_id(UID::ALL_MANUFACTURERS),
    m_rdm_response(NULL),
    m_uid_set_callback(NULL),
    m_rdm_response_callback(NULL) {
  m_widget->SetMessageHandler(
      NewCallback(this, &DmxTriWidget::HandleMessage));
}


/*
 * Shutdown
 */
DmxTriWidget::~DmxTriWidget() {
  // delete all outstanding requests
  while (!m_pending_requests.empty()) {
    const RDMRequest *request = m_pending_requests.front();
    delete request;
    m_pending_requests.pop();
  }

  if (m_uid_set_callback)
    delete m_uid_set_callback;

  if (m_rdm_response_callback)
    delete m_rdm_response_callback;
}


/**
 * Set the callback used when the UIDSet changes
 */
void DmxTriWidget::SetUIDListCallback(
    ola::Callback1<void, const ola::rdm::UIDSet&> *callback) {
  if (m_uid_set_callback)
    delete m_uid_set_callback;
  m_uid_set_callback = callback;
}


/**
 * Set the callback used when a RDM response is received
 */
void DmxTriWidget::SetRDMResponseCallback(
        ola::Callback1<bool, const ola::rdm::RDMResponse*> *callback) {
  if (m_rdm_response_callback)
    delete m_rdm_response_callback;
  m_rdm_response_callback = callback;
}


/*
 * Stop the widget.
 */
void DmxTriWidget::Stop() {
  if (m_rdm_timeout_id != ola::network::INVALID_TIMEOUT) {
    m_ss->RemoveTimeout(m_rdm_timeout_id);
    m_rdm_timeout_id = ola::network::INVALID_TIMEOUT;
  }
}


/*
 * Send a DMX message
 * @returns true if we sent ok, false otherwise
 */
bool DmxTriWidget::SendDMX(const DmxBuffer &buffer) const {
  struct {
    uint8_t start_code;
    uint8_t dmx[DMX_UNIVERSE_SIZE];
  } widget_dmx;

  widget_dmx.start_code = 0;
  unsigned int length = DMX_UNIVERSE_SIZE;
  buffer.Get(widget_dmx.dmx, &length);
  return m_widget->SendMessage(UsbWidget::DMX_LABEL,
                               length + 1,
                               reinterpret_cast<uint8_t*>(&widget_dmx));
}


/*
 * Handle an RDM Request, ownership of the request object is transferred to us.
 */
bool DmxTriWidget::HandleRDMRequest(const ola::rdm::RDMRequest *request) {
  // if we can't find this UID, fail now. While in discovery mode the
  // m_uid_index_map will be clear so skip the check in this case.
  const UID &dest_uid = request->DestinationUID();
  if (!dest_uid.IsBroadcast() &&
      !InDiscoveryMode() &&
      m_uid_index_map.find(dest_uid) == m_uid_index_map.end()) {
    delete request;
    return false;
  }
  m_pending_requests.push(request);
  MaybeSendRDMRequest();
  return true;
}


/*
 * Kick off the discovery process if it's not already running
 */
void DmxTriWidget::RunRDMDiscovery() {
  if (InDiscoveryMode())
    // process already running
    return;

  if (!SendDiscoveryStart()) {
    OLA_WARN << "Failed to begin RDM discovery";
    return;
  }

  // setup a stat every RDM_STATUS_INTERVAL_MS until we're done
  m_rdm_timeout_id = m_ss->RegisterRepeatingTimeout(
      RDM_STATUS_INTERVAL_MS,
      NewCallback(this, &DmxTriWidget::CheckDiscoveryStatus));
}


/**
 * Call the UIDSet handler with the latest UID list.
 */
void DmxTriWidget::SendUIDUpdate() {
  UIDSet uid_set;
  map<UID, uint8_t>::iterator iter = m_uid_index_map.begin();
  for (; iter != m_uid_index_map.end(); ++iter) {
    uid_set.AddUID(iter->first);
  }
  if (m_uid_set_callback)
    m_uid_set_callback->Run(uid_set);
}


/*
 * Check the status of the RDM discovery process.
 * This is called periodically while discovery is running
 */
bool DmxTriWidget::CheckDiscoveryStatus() {
  return SendDiscoveryStat();
}


/*
 * Handle a message received from the widget
 */
void DmxTriWidget::HandleMessage(uint8_t label,
                                 unsigned int length,
                                 const uint8_t *data) {
  if (label == EXTENDED_COMMAND_LABEL) {
    if (length < DATA_OFFSET) {
      OLA_WARN << "DMX-TRI frame too small";
      return;
    }

    uint8_t command_id = data[0];
    uint8_t return_code = data[1];
    length -= DATA_OFFSET;
    data = length ? data + DATA_OFFSET: NULL;

    switch (command_id) {
      case DISCOVER_AUTO_COMMAND_ID:
        HandleDiscoveryAutoResponse(return_code, data, length);
        break;
      case DISCOVER_STATUS_COMMAND_ID:
        HandleDiscoverStatResponse(return_code, data, length);
        break;
      case REMOTE_UID_COMMAND_ID:
        HandleRemoteUIDResponse(return_code, data, length);
        break;
      case REMOTE_GET_COMMAND_ID:
        HandleRemoteRDMResponse(return_code, data, length);
        break;
      case REMOTE_SET_COMMAND_ID:
        HandleRemoteRDMResponse(return_code, data, length);
        break;
      case QUEUED_GET_COMMAND_ID:
        HandleQueuedGetResponse(return_code, data, length);
        break;
      case SET_FILTER_COMMAND_ID:
        HandleSetFilterResponse(return_code, data, length);
        break;
      default:
        OLA_WARN << "Unknown DMX-TRI CI: " << static_cast<int>(command_id);
    }
  } else {
    OLA_INFO << "DMX-TRI got response " << static_cast<int>(label);
  }
}


/*
 * Return true if discovery is running
 */
bool DmxTriWidget::InDiscoveryMode() const {
  return (m_rdm_timeout_id != ola::network::INVALID_TIMEOUT ||
          m_uid_count);
}


/*
 * Send a DiscoAuto message to begin the discovery process.
 */
bool DmxTriWidget::SendDiscoveryStart() {
  uint8_t command_id = DISCOVER_AUTO_COMMAND_ID;

  return m_widget->SendMessage(EXTENDED_COMMAND_LABEL,
                               sizeof(command_id),
                               &command_id);
}


/*
 * Send a DiscoAuto message to begin the discovery process.
 */
void DmxTriWidget::FetchNextUID() {
  if (!m_uid_count)
    return;

  OLA_INFO << "fetching index  " << static_cast<int>(m_uid_count);
  uint8_t data[] = {REMOTE_UID_COMMAND_ID, m_uid_count};
  m_widget->SendMessage(EXTENDED_COMMAND_LABEL,
                        sizeof(data),
                        data);
}


/*
 * Send a SetFilter command
 */
bool DmxTriWidget::SendSetFilter(uint16_t esta_id) {
  uint8_t data[] = {SET_FILTER_COMMAND_ID, esta_id >> 8, esta_id & 0xff};
  return m_widget->SendMessage(EXTENDED_COMMAND_LABEL,
                               sizeof(data),
                               reinterpret_cast<uint8_t*>(&data));
}


/*
 * Send a DiscoStat message to begin the discovery process.
 */
bool DmxTriWidget::SendDiscoveryStat() {
  uint8_t command_id = DISCOVER_STATUS_COMMAND_ID;

  return m_widget->SendMessage(EXTENDED_COMMAND_LABEL,
                               sizeof(command_id),
                               &command_id);
}


/*
 * If we're not in discovery mode, send the next request. This will call
 * SetFilter and defer the send if it's a broadcast UID.
 */
void DmxTriWidget::MaybeSendRDMRequest() {
  if (InDiscoveryMode() || m_pending_requests.empty() || m_rdm_request_pending)
    return;

  m_rdm_request_pending = true;
  const RDMRequest *request = m_pending_requests.front();
  if (request->DestinationUID().IsBroadcast() &&
      request->DestinationUID().ManufacturerId() != m_last_esta_id) {
    SendSetFilter(request->DestinationUID().ManufacturerId());
  } else {
    DispatchNextRequest();
  }
}


/*
 * Send the next RDM request, this assumes that SetFilter has been called
 */
void DmxTriWidget::DispatchNextRequest() {
  const RDMRequest *request = m_pending_requests.front();
  if (request->ParamId() == ola::rdm::PID_QUEUED_MESSAGE &&
      request->CommandClass() == RDMCommand::GET_COMMAND) {
    // these are special
    if (request->ParamDataSize())
      DispatchQueuedGet(request);
    else
      OLA_WARN << "Missing param data in queued message get";
    return;
  }

  struct rdm_message {
    uint8_t command;
    uint8_t index;
    uint16_t sub_device;
    uint16_t param_id;
    uint8_t data[ola::rdm::RDMCommand::MAX_PARAM_DATA_LENGTH];
  } __attribute__((packed));

  rdm_message message;

  if (request->CommandClass() == RDMCommand::GET_COMMAND) {
    message.command = REMOTE_GET_COMMAND_ID;
  } else if (request->CommandClass() == RDMCommand::SET_COMMAND) {
    message.command = REMOTE_SET_COMMAND_ID;
  } else {
    OLA_WARN << "Request was not get or set: " <<
      static_cast<int>(request->CommandClass());
    return;
  }

  if (request->DestinationUID().IsBroadcast()) {
    message.index = 0;
  } else {
    map<UID, uint8_t>::const_iterator iter =
      m_uid_index_map.find(request->DestinationUID());
    if (iter == m_uid_index_map.end()) {
      OLA_WARN << request->DestinationUID() << " not found in uid map";
      return;
    }
    message.index = iter->second;
  }
  message.sub_device = HostToNetwork(request->SubDevice());
  message.param_id = HostToNetwork(request->ParamId());
  if (request->ParamDataSize())
    memcpy(message.data,
           request->ParamData(),
           request->ParamDataSize());

  unsigned int size = sizeof(message) -
    ola::rdm::RDMCommand::MAX_PARAM_DATA_LENGTH + request->ParamDataSize();

  OLA_INFO << "Sending request to " << request->DestinationUID() <<
    " with command " << std::hex << request->CommandClass() << " and param " <<
    std::hex << request->ParamId();

  m_widget->SendMessage(EXTENDED_COMMAND_LABEL,
                        size,
                        reinterpret_cast<uint8_t*>(&message));
}


/*
 * Send a queued get message
 */
void DmxTriWidget::DispatchQueuedGet(const ola::rdm::RDMRequest* request) {
  map<UID, uint8_t>::const_iterator iter =
    m_uid_index_map.find(request->DestinationUID());
  if (iter == m_uid_index_map.end()) {
    OLA_WARN << request->DestinationUID() << " not found in uid map";
    return;
  }
  uint8_t data[] = {QUEUED_GET_COMMAND_ID,
                    iter->second,
                    request->ParamData()[0]};

  m_widget->SendMessage(EXTENDED_COMMAND_LABEL,
                        sizeof(data),
                        reinterpret_cast<uint8_t*>(&data));
}


/*
 * Stop the discovery process
 */
void DmxTriWidget::StopDiscovery() {
  if (m_rdm_timeout_id != ola::network::INVALID_TIMEOUT) {
    m_ss->RemoveTimeout(m_rdm_timeout_id);
    m_rdm_timeout_id = ola::network::INVALID_TIMEOUT;
  }
}


/*
 * Handle the response from calling DiscoAuto
 */
void DmxTriWidget::HandleDiscoveryAutoResponse(uint8_t return_code,
                                               const uint8_t *data,
                                               unsigned int length) {
  if (return_code != EC_NO_ERROR) {
    OLA_WARN << "DMX_TRI discovery returned error " <<
      static_cast<int>(return_code);
    StopDiscovery();
  }
  (void) data;
  (void) length;
}


/*
 * Handle the response from calling discovery stat
 */
void DmxTriWidget::HandleDiscoverStatResponse(uint8_t return_code,
                                              const uint8_t *data,
                                              unsigned int length) {
  switch (return_code) {
    case EC_NO_ERROR:
      break;
    case EC_RESPONSE_MUTE:
      OLA_WARN << "Failed to mute device, aborting discovery";
      StopDiscovery();
      return;
    case EC_RESPONSE_DISCOVERY:
      OLA_WARN <<
        "Duplicated or erroneous device detected, aborting discovery";
      StopDiscovery();
      return;
    case EC_RESPONSE_UNEXPECTED:
      OLA_INFO << "Got an unexpected RDM response during discovery";
      break;
    default:
      OLA_WARN << "DMX_TRI discovery returned error " <<
        static_cast<int>(return_code);
      StopDiscovery();
      return;
  }

  if (length < 1) {
    OLA_WARN << "DiscoStat command too short, was " << length;
    return;
  }

  if (data[1] == 0) {
    OLA_DEBUG << "Discovery process has completed, " <<
      static_cast<int>(data[0]) << " devices found";
    StopDiscovery();
    m_uid_count = data[0];
    m_uid_index_map.clear();
    if (m_uid_count)
      FetchNextUID();
  }
}


/*
 * Handle the response to a RemoteGet command
 */
void DmxTriWidget::HandleRemoteUIDResponse(uint8_t return_code,
                                           const uint8_t *data,
                                           unsigned int length) {
  if (!m_uid_count) {
    // not expecting any responses
    OLA_INFO << "Got an unexpected RemoteUID response";
    return;
  }

  if (return_code == EC_NO_ERROR) {
    if (length < ola::rdm::UID::UID_SIZE) {
      OLA_INFO << "Short RemoteUID response, was " << length;
    } else {
      const UID uid(data);
      m_uid_index_map[uid] = m_uid_count;
    }
  } else if (return_code == EC_CONSTRAINT) {
    // this is returned if the index is wrong
    OLA_INFO << "RemoteUID returned RC_Constraint, some how we botched the"
      << " discovery process, subtracting 1 and attempting to continue";
  } else {
    OLA_INFO << "RemoteUID returned " << static_cast<int>(return_code);
  }

  m_uid_count--;

  if (m_uid_count) {
    FetchNextUID();
  } else {
    // notify the universe
    SendUIDUpdate();

    // start sending rdm commands again
    MaybeSendRDMRequest();
  }
}


/*
 * Handle the response to a RemoteGet command
 */
void DmxTriWidget::HandleRemoteRDMResponse(uint8_t return_code,
                                           const uint8_t *data,
                                           unsigned int length) {
  const RDMRequest *request = m_pending_requests.front();

  OLA_INFO << "Received RDM response with code 0x" <<
    static_cast<int>(return_code) << ", " << length << " bytes, param " <<
    std::hex << request->ParamId();

  if (return_code == EC_NO_ERROR ||
      return_code == EC_RESPONSE_WAIT ||
      return_code == EC_RESPONSE_MORE) {
    ola::rdm::RDMResponse *response = ola::rdm::GetResponseWithData(
        request,
        data,
        length,
        // this is a hack, there is no way to expose # of queues messages
        return_code == EC_RESPONSE_WAIT ? 1 : 0);

    if (m_rdm_response) {
      // if this is part of an overflowed response we need to combine it
      ola::rdm::RDMResponse *combined_response =
        ola::rdm::RDMResponse::CombineResponses(m_rdm_response, response);
      delete m_rdm_response;
      delete response;
      m_rdm_response = combined_response;
    } else {
      m_rdm_response = response;
    }

    if (m_rdm_response) {
      if (return_code == EC_RESPONSE_MORE) {
        // send the same command again;
        DispatchNextRequest();
        return;
      } else {
        if (m_rdm_response_callback)
          m_rdm_response_callback->Run(m_rdm_response);
        m_rdm_response = NULL;
      }
    }
  } else if (return_code >= EC_UNKNOWN_PID &&
             return_code <= EC_SUBDEVICE_UNKNOWN) {
    if (m_rdm_response) {
      delete m_rdm_response;
      m_rdm_response = NULL;
    }

    // avoid compiler warnings
    ola::rdm::rdm_nack_reason reason = ola::rdm::NR_UNKNOWN_PID;

    switch (return_code) {
      case EC_UNKNOWN_PID:
        reason = ola::rdm::NR_UNKNOWN_PID;
        break;
      case EC_FORMAT_ERROR:
        reason = ola::rdm::NR_FORMAT_ERROR;
        break;
      case EC_HARDWARE_FAULT:
        reason = ola::rdm::NR_HARDWARE_FAULT;
        break;
      case EC_PROXY_REJECT:
        reason = ola::rdm::NR_PROXY_REJECT;
        break;
      case EC_WRITE_PROTECT:
        reason = ola::rdm::NR_WRITE_PROTECT;
        break;
      case EC_UNSUPPORTED_COMMAND_CLASS:
        reason = ola::rdm::NR_UNSUPPORTED_COMMAND_CLASS;
        break;
      case EC_OUT_OF_RANGE:
        reason = ola::rdm::NR_DATA_OUT_OF_RANGE;
        break;
      case EC_BUFFER_FULL:
        reason = ola::rdm::NR_BUFFER_FULL;
        break;
      case EC_FRAME_OVERFLOW:
        reason = ola::rdm::NR_PACKET_SIZE_UNSUPPORTED;
        break;
      case EC_SUBDEVICE_UNKNOWN:
        reason = ola::rdm::NR_SUB_DEVICE_OUT_OF_RANGE;
        break;
    }
    ola::rdm::RDMResponse *response =
      ola::rdm::NackWithReason(request, reason);
    m_rdm_response_callback->Run(response);

  } else {
    // TODO(simonn): Implement the correct response here when we error out
    // case 0x15
    // case 0x18
    OLA_WARN << "Response was returned with 0x" << std::hex <<
      static_cast<int>(return_code);
    if (m_rdm_response) {
      delete m_rdm_response;
      m_rdm_response = NULL;
    }
  }
  delete request;
  m_pending_requests.pop();
  m_rdm_request_pending = false;
  // send the next one
  MaybeSendRDMRequest();
}


/*
 * Handle the response to a QueuedGet command
 */
void DmxTriWidget::HandleQueuedGetResponse(uint8_t return_code,
                                           const uint8_t *data,
                                           unsigned int length) {
  OLA_INFO << "got queued message response";
  // TODO(simon): implement this
  (void) return_code;
  (void) data;
  (void) length;
}


/*
 * Handle a setfilter response
 */
void DmxTriWidget::HandleSetFilterResponse(uint8_t return_code,
                                           const uint8_t *data,
                                           unsigned int length) {
  if (return_code == EC_NO_ERROR) {
    m_last_esta_id =
      m_pending_requests.front()->DestinationUID().ManufacturerId();
    DispatchNextRequest();
  } else {
    OLA_WARN << "SetFilter returned " << static_cast<int>(return_code) <<
      ", we have no option but to drop the rdm request";
    delete m_pending_requests.front();
    m_pending_requests.pop();
    m_rdm_request_pending = false;
    MaybeSendRDMRequest();
  }
  (void) data;
  (void) length;
}
}  // usbpro
}  // plugin
}  // ola
