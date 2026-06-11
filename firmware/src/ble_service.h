#pragma once
#include "usage_state.h"

// Same GATT UUIDs as upstream Clawdmeter — so the existing daemon
// (and any future port) can talk to this firmware unchanged.
//
//   Data Service : 4c41555a-4465-7669-6365-000000000001
//   RX (write)   : 4c41555a-4465-7669-6365-000000000002
//   TX (notify)  : 4c41555a-4465-7669-6365-000000000003
//
// Device advertises as "Claude Controller".

using UsageCallback = void (*)(const UsageState&);

void ble_begin(const char* device_name, UsageCallback on_state);
void ble_loop();

// True while a BLE central (the daemon) is connected. The deep-sleep wake
// window uses this to decide whether to keep waiting for a state push or bail
// early when nobody is around.
bool ble_client_connected();

// Publish a short status blob on the TX characteristic for the daemon to read
// on connect (we use it to report battery voltage so we can diagnose remotely
// without a serial cable). Safe to call any time after ble_begin().
void ble_set_status(const char* json);

// Fully tear down the BLE stack and power down the radio. Called right before
// the e-paper refresh: the panel's full BWRY update is a current-heavy burst,
// and on battery the radio + refresh peak together can brown out the rail, so
// we drop the radio first to leave more headroom.
void ble_stop();
