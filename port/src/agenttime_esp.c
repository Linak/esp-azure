// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/opt.h"
#include "esp_sntp.h"
#include "esp_log.h"

#include "azure_c_shared_utility/agenttime.h"
#include "azure_c_shared_utility/xlogging.h"
#include "sntp_time.h"

static const char* TAG = "agentime_esp";

/// Any time before this year means the clock still runs from the 1970 epoch.
#define SNTP_MIN_VALID_YEAR   2026
#define SNTP_WAIT_POLL_MS     500

void initialize_sntp(void)
{
    if (esp_sntp_enabled())
    {
        ESP_LOGI(TAG, "SNTP already running, restarting");
        esp_sntp_restart();
        return;
    }

    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
#if LWIP_DHCP_GET_NTP_SRV
    esp_sntp_servermode_dhcp(true);              /* accept router-provided NTP (DHCP option 42) */
#endif
    esp_sntp_setservername(0, "0.pool.ntp.org");
    esp_sntp_setservername(1, "1.pool.ntp.org");
    esp_sntp_setservername(2, "time.windows.com");
    esp_sntp_init();
}

void sntp_network_up(void)
{
    if (!esp_sntp_enabled())
    {
        /* First IP address after boot: start SNTP right away so the first
         * request goes out before the BLE client occupies the radio. */
        initialize_sntp();
        return;
    }
    if (esp_sntp_restart())
    {
        ESP_LOGI(TAG, "Network up, SNTP request restarted");
    }
}

bool sntp_time_is_valid(void)
{
    time_t now = 0;
    struct tm timeinfo = { 0 };

    time(&now);
    localtime_r(&now, &timeinfo);
    return timeinfo.tm_year >= (SNTP_MIN_VALID_YEAR - 1900);
}

bool sntp_wait_for_time(uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    int retry = 0;

    while (!sntp_time_is_valid())
    {
        if ((xTaskGetTickCount() - start) >= timeout)
        {
            ESP_LOGW(TAG, "System time not set after %u ms, giving up for now", (unsigned)timeout_ms);
            return false;
        }
        if ((retry++ % 4) == 0)
        {
            ESP_LOGI(TAG, "Waiting for system time to be set... [times:%d]", retry / 4 + 1);
        }
        vTaskDelay(pdMS_TO_TICKS(SNTP_WAIT_POLL_MS));
    }
    return true;
}

/*
 * get_time() is called from many places inside the Azure SDK (logging, SAS token
 * expiry, retry timers). It must never block: callers that need a valid clock
 * check sntp_time_is_valid() before using the SDK.
 */
time_t get_time(time_t* currentTime)
{
    time_t now = time(NULL);
    if (currentTime != NULL)
    {
        *currentTime = now;
    }
    return now;
}

double get_difftime(time_t stopTime, time_t startTime)
{
    return (double)stopTime - (double)startTime;
}

struct tm* get_gmtime(time_t* currentTime)
{
    return NULL;
}

char* get_ctime(time_t* timeToGet)
{
    return NULL;
}
