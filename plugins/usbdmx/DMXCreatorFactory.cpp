/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * DMXCreatorFactory.cpp
 * The factory for DMXCreator widgets.
 * Copyright (C) 2016 Florian Edelmann
 */

#include "plugins/usbdmx/DMXCreatorFactory.h"

#include "libs/usb/LibUsbAdaptor.h"
#include "ola/Logging.h"
#include "ola/base/Flags.h"
#include "plugins/usbdmx/DMXCreator.h"

DECLARE_bool(use_async_libusb);

namespace ola {
namespace plugin {
namespace usbdmx {

using ola::usb::LibUsbAdaptor;

const char DMXCreatorFactory::EXPECTED_MANUFACTURER[] = "http://www.dmx512.ch/";
const char DMXCreatorFactory::EXPECTED_PRODUCT[] = "DMXCreator 512 Basic";
const uint16_t DMXCreatorFactory::VENDOR_ID = 0x0a30;
const uint16_t DMXCreatorFactory::PRODUCT_ID = 0x0002;

bool DMXCreatorFactory::DeviceAdded(
    WidgetObserver *observer,
    libusb_device *usb_device,
    const struct libusb_device_descriptor &descriptor) {
  if (descriptor.idVendor != VENDOR_ID || descriptor.idProduct != PRODUCT_ID) {
    return false;
  }

  OLA_INFO << "Found a new DMXCreator device";
  LibUsbAdaptor::DeviceInformation info;
  /*if (!m_adaptor->GetDeviceInfo(usb_device, descriptor, &info)) {
    return false;
  }

  if (!m_adaptor->CheckManufacturer(EXPECTED_MANUFACTURER, info)) {
    return false;
  }

  if (!m_adaptor->CheckProduct(EXPECTED_PRODUCT, info)) {
    return false;
  }*/

  // Some DMXCreator devices don't have serial numbers. Since there isn't another
  // good way to uniquely identify a USB device, we only support one of these
  // types of devices per host.
  if (info.serial.empty()) {
    if (m_missing_serial_number) {
      OLA_WARN << "Failed to read serial number or serial number empty. "
               << "We can only support one device without a serial number.";
      return false;
    } else {
      OLA_WARN << "Failed to read serial number from " << info.manufacturer
               << " : " << info.product
               << " the device probably doesn't have one";
      m_missing_serial_number = true;
    }
  }

  DMXCreator *widget = NULL;
  if (FLAGS_use_async_libusb) {
    widget = new AsynchronousDMXCreator(m_adaptor, usb_device, info.serial);
  } else {
    widget = new SynchronousDMXCreator(m_adaptor, usb_device, info.serial);
  }
  return AddWidget(observer, widget);
}
}  // namespace usbdmx
}  // namespace plugin
}  // namespace ola
