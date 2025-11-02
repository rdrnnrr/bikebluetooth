#include "cts_ble_client.h"

#include "BLEClient.h"
#include "BLEUtils.h"

#include <Arduino.h>

const BLEUUID ctsServiceUUID("1805");
// const BLEUUID ctsCharacteristicLocalTimeInformationUUID("00002a0f-0000-1000-8000-00805f9b34fb");
const BLEUUID ctsCharacteristicCurrentTimeUUID("00002a2b-0000-1000-8000-00805f9b34fb");

static char LOG_TAG[] = "CTSBLEClient";

void CTSBLEClient::setup(BLEClient *bleClient)
{
    ESP_LOGW(LOG_TAG, "CTSBLEClient setup");
    BLERemoteService *ctsService = bleClient->getService(BLEUUID("1805"));
    if (ctsService == nullptr)
    {
        ESP_LOGW(LOG_TAG, "Failed to find service UUID: ctsServiceUUID");
        return;
    }
    remoteCurrentTimeCharacteristic = ctsService->getCharacteristic(ctsCharacteristicCurrentTimeUUID);
    if (remoteCurrentTimeCharacteristic == nullptr)
    {
        ESP_LOGW(LOG_TAG, "Failed to find characteristic UUID: ctsCharacteristicCurrentTimeUUID");
        return;
    }
    // BLERemoteCharacteristic *localTimeInformation = ctsService->getCharacteristic(ctsCharacteristicLocalTimeInformationUUID);
}

ble_cts_current_time_char_t *CTSBLEClient::readTime()
{
    if (!remoteCurrentTimeCharacteristic)
        return nullptr;
    std::string data = remoteCurrentTimeCharacteristic->readValue();
    assert(data.length() == 10);
    memcpy(&lastCurrentTimeRead, data.data(), min((size_t)sizeof(ble_cts_current_time_char_t), data.length()));
    return &lastCurrentTimeRead;
}
