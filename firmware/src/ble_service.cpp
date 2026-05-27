#include "ble_service.h"
#include <NimBLEDevice.h>
#include <ArduinoJson.h>

// Match upstream Clawdmeter GATT UUIDs so the daemon stays compatible.
static const char* DATA_SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001";
static const char* RX_CHAR_UUID      = "4c41555a-4465-7669-6365-000000000002";
static const char* TX_CHAR_UUID      = "4c41555a-4465-7669-6365-000000000003";

static UsageCallback s_callback = nullptr;
static NimBLECharacteristic* s_tx_char = nullptr;

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        std::string val = c->getValue();
        Serial.printf("[BLE] RX %u bytes: %s\n", (unsigned)val.size(), val.c_str());

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, val);
        if (err) {
            Serial.printf("[BLE] JSON parse error: %s\n", err.c_str());
            return;
        }

        UsageState st;
        st.session_pct       = doc["s"]  | -1;
        st.session_reset_min = doc["sr"] | -1;
        st.session_reset_at  = String((const char*)(doc["sa"] | ""));
        st.weekly_pct        = doc["w"]  | -1;
        st.weekly_reset_min  = doc["wr"] | -1;
        st.weekly_reset_at   = String((const char*)(doc["wa"] | ""));
        st.status            = String((const char*)(doc["st"] | ""));
        st.ok                = doc["ok"] | false;

        if (s_callback) s_callback(st);
    }
};

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*) override   { Serial.println("[BLE] client connected"); }
    void onDisconnect(NimBLEServer* srv) override {
        Serial.println("[BLE] client disconnected, restarting advertising");
        srv->startAdvertising();
    }
};

void ble_begin(const char* device_name, UsageCallback on_state) {
    s_callback = on_state;

    NimBLEDevice::init(device_name);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());

    NimBLEService* svc = server->createService(DATA_SERVICE_UUID);

    NimBLECharacteristic* rx = svc->createCharacteristic(
        RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(new RxCallbacks());

    s_tx_char = svc->createCharacteristic(
        TX_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    s_tx_char->setValue("ready");

    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(DATA_SERVICE_UUID);
    adv->setScanResponse(true);
    NimBLEDevice::startAdvertising();

    Serial.printf("[BLE] advertising as \"%s\"\n", device_name);
    Serial.printf("[BLE] MAC: %s\n", NimBLEDevice::getAddress().toString().c_str());
}

void ble_loop() {
    // NimBLE runs on its own task; nothing to do here for now.
    // Hook for future heartbeat / TX notify if we ever want the firmware
    // to report status back to the daemon.
}
