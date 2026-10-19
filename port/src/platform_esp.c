// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#ifdef _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif
#include "azure_c_shared_utility/platform.h"
#include "azure_c_shared_utility/xio.h"
#include "azure_c_shared_utility/tlsio_openssl.h"
#include "azure_c_shared_utility/wsio.h"
#include "azure_c_shared_utility/xlogging.h"
#include "esp_log.h"
#include "tlsio_pal.h"

#include "esp_sntp.h"
#include "sntp_time.h"

static const char* TAG = "platform";

/// Upper bound for the initial time sync wait. The cloud connection is gated
/// on sntp_time_is_valid() anyway, so this only limits how long startup logs
/// stay quiet, it does not decide whether the gateway ever connects.
#define PLATFORM_SNTP_INITIAL_WAIT_MS   (30 * 1000)

int platform_init(void)
{
    initialize_sntp();
    ESP_LOGI(TAG, "ESP platform sntp inited!");

    if (!sntp_wait_for_time(PLATFORM_SNTP_INITIAL_WAIT_MS))
    {
        ESP_LOGW(TAG, "Time not synced yet, SNTP keeps retrying in the background");
        return 0;
    }

    time_t now = time(NULL);
    char strftime_buf[64];
    struct tm timeinfo;

    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "The current date/time is: %s", strftime_buf);

    return 0;
}

const IO_INTERFACE_DESCRIPTION* platform_get_default_tlsio(void)
{
    return tlsio_pal_get_interface_description();
}

const IO_INTERFACE_DESCRIPTION* socketio_get_interface_description(void)
{
    return wsio_get_interface_description();
}

void platform_deinit(void)
{
    esp_sntp_stop();
}

STRING_HANDLE platform_get_platform_info(PLATFORM_INFO_OPTION options)
{
    // Expected format: "(<runtime name>; <operating system name>; <platform>)"

    return STRING_construct("(native; freertos; esp platform)");
}
