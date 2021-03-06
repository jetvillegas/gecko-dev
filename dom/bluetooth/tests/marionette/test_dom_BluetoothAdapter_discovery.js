/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: sw=2 ts=2 sts=2 et filetype=javascript
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

///////////////////////////////////////////////////////////////////////////////
// Test Purpose:
//   To verify that discovery process of BluetoothAdapter is correct.
//   Use B2G emulator commands to add/remote remote devices to simulate
//   discovering behavior.
//
// Test Coverage:
//   - BluetoothAdapter.startDiscovery()
//   - BluetoothAdapter.stopDiscovery()
//   - BluetoothAdapter.ondevicefound()
//   - BluetoothAdapter.discovering [Temporarily turned off until BT API update]
//
///////////////////////////////////////////////////////////////////////////////

MARIONETTE_TIMEOUT = 60000;
MARIONETTE_HEAD_JS = 'head.js';

startBluetoothTest(true, function testCaseMain(aAdapter) {
  log("Testing the discovery process of BluetoothAdapter ...");

  // The properties of remote device.
  let theProperties = {
    "name": REMOTE_DEVICE_NAME,
    "discoverable": true
  };

  return Promise.resolve()
    .then(() => removeEmulatorRemoteDevice(BDADDR_ALL))
    .then(() => addEmulatorRemoteDevice(/*theProperties*/ null))
    .then(function(aRemoteAddress) {
      let promises = [];
      promises.push(waitForAdapterEvent(aAdapter, "devicefound"));
      promises.push(startDiscovery(aAdapter));
      return Promise.all(promises)
        .then(function(aResults) {
          is(aResults[0].device.address, aRemoteAddress, "BluetoothDevice.address");
        });
    })
    .then(() => stopDiscovery(aAdapter))
    .then(() => removeEmulatorRemoteDevice(BDADDR_ALL));
});
