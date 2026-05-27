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
