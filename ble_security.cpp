
#include "ble_security.h"

#include "esp_log.h"
#include <esp_gap_ble_api.h>

static char LOG_TAG[] = "NotificationSecurityCallbacks";



uint32_t NotificationSecurityCallbacks::onPassKeyRequest(){
    ESP_LOGW(LOG_TAG, "PassKeyRequest received without IO capabilities; returning 0");
    return 0;
}

void NotificationSecurityCallbacks::onPassKeyNotify(uint32_t pass_key){
    ESP_LOGI(LOG_TAG, "On passkey Notify number:%d", pass_key);
}

bool NotificationSecurityCallbacks::onSecurityRequest(){
    ESP_LOGI(LOG_TAG, "On Security Request");
    return true;
}

bool NotificationSecurityCallbacks::onConfirmPIN(unsigned int){
    ESP_LOGI(LOG_TAG, "On Confirmed Pin Request");
    return true;
}

void NotificationSecurityCallbacks::onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl){
    if(cmpl.success){
        uint16_t length;
        esp_ble_gap_get_whitelist_size(&length);
        ESP_LOGI(LOG_TAG, "Authentication successful, whitelist size: %d", length);
    } else {
        ESP_LOGE(LOG_TAG, "Authentication failed, reason: 0x%02x", cmpl.fail_reason);
    }
}
