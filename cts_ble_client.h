#ifndef CTS_BLE_CLIENT_H_
#define CTS_BLE_CLIENT_H_

#include <stdint.h>

class BLEClient;

typedef struct __attribute__((__packed__))
{
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hours;
    uint8_t  minutes;
    uint8_t  seconds;
    uint8_t  day_of_week;
    uint8_t  fractions256;
} ble_cts_date_time_t;

typedef struct
{
    uint8_t manual_time_update : 1;
    uint8_t external_reference_time_update : 1;
    uint8_t change_of_time_zone : 1;
    uint8_t change_of_daylight_savings_time : 1;
} ble_cts_adjust_reason_t;

typedef struct __attribute__((__packed__))
{
    ble_cts_date_time_t exact_time_256;
    ble_cts_adjust_reason_t adjust_reason;
} ble_cts_current_time_char_t;

typedef struct __attribute__((__packed__))
{
    int8_t timeZone;
    uint8_t dst;
} ble_cts_local_time_information_t;

class CTSBLEClient
{
private:
    class BLERemoteCharacteristic *remoteCurrentTimeCharacteristic = nullptr;
    ble_cts_current_time_char_t lastCurrentTimeRead = {};
public:
    CTSBLEClient() {};
    ~CTSBLEClient() {};
    void setup(BLEClient *bleClient);
    ble_cts_current_time_char_t *readTime();
    bool ready() { return remoteCurrentTimeCharacteristic != nullptr; }
};


#endif // CTS_BLE_CLIENT_H_
