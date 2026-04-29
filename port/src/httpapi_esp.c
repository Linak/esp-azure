/*
 * httpapi_esp.h
 *
 *  Created on: Aug 15, 2019
 *      Author: kibo
 *
 *      This file provides a wrapper between the azure HTTPAPI and the esp_http_client API.
 *
 *      There are certain limitations:
 *      - Supports only POST and GET
 *      - SetOption supports the following option(s):
 *           OPTION_RESP_CB_FUNC  - Install a callback for partial handling of responses
 *      - CloneOption not supported
 *      - Only https is supported
 */

#include "azure_c_shared_utility/strings.h"
#include "azure_c_shared_utility/httpapi.h"
#include "azure_c_shared_utility/httpheaders.h"
#include "azure_c_shared_utility/xlogging.h"
#include "certs.h"
#include "esp_http_client.h"
#include "esp_err.h"
#include <stdio.h>
#include "httpapi_adapter.h"
#include <string.h>
#include <azure_c_shared_utility/xlogging.h>

/// The chunksize we want to use for HTTP requests
#define HTTP_BUFFER_SIZE 2048

/// The time we will wait before giving up on HTTP
#define HTTP_TIMEOUT_MS  30000

/// Instance data
typedef struct HTTP_HANDLE_DATA_TAG
{
    /// handle to esp api
    esp_http_client_handle_t espHdl;

    /// Name of the http(s) server
    char* server;

    /// Handle to response headers (Azure HTTPAPI)
    HTTP_HEADERS_HANDLE respHdr;

    /// Handle to response body
    BUFFER_HANDLE       respBody;

    /// Pointer to status code of request
    unsigned int* statusCode;

    /// Callback for partial handling of responses
    HttpApiResponseCbFuncType    respCb;

    /// Argument given to callback. User defined content
    void*                        respArg;

} EspHttpApiHandle;

/* @brief Helper for building the request
 * @param handle - the instance returned bt HTTPAPI_CreateConnection
 * @param requestType - post or get
 * @param relativePath - the path part of the URL
 * @param httpHeadersHandle - Any optional headers added by the application
 * @param content - body to post
 * @param contentLength - length of body
 *
 * @return false on error
 *
 * @detail
 *    If any content is provided, requestType will be converted to a POST
 */
static bool buildRequest(HTTP_HANDLE handle,
        HTTPAPI_REQUEST_TYPE requestType,
        const char* relativePath,
        HTTP_HEADERS_HANDLE httpHeadersHandle,
        const unsigned char* content,
        size_t contentLength);

/// Callback from esp_http_client
static esp_err_t httpEventHandler(esp_http_client_event_t *evt);

#if defined(__GNUC__)
__attribute__ ((format (printf, 6, 7)))
#endif
void httpconsolelogger_log(LOG_CATEGORY log_category, const char* file, const char* func, int line, unsigned int options, const char* format, ...)
{
    time_t t;
    va_list args;
    va_start(args, format);

    switch (log_category)
    {
    case AZ_LOG_INFO:
        (void)printf("Info: ");
        break;
    case AZ_LOG_ERROR:
        (void)printf("Error: File:%s Func:%s Line:%d ", file, func, line);
        break;
    default:
        break;
    }

    (void)vprintf(format, args);
    va_end(args);

    (void)log_category;
    if (options & LOG_LINE)
    {
        (void)printf("\r\n");
    }
}

HTTPAPI_RESULT HTTPAPI_Init(void)
{
    xlogging_set_log_function(httpconsolelogger_log);
    return HTTPAPI_OK;
}

void HTTPAPI_Deinit(void)
{
}

HTTP_HANDLE HTTPAPI_CreateConnection(const char* hostName)
{
    HTTP_HANDLE hdl = (HTTP_HANDLE)malloc(sizeof(EspHttpApiHandle));
    if (!hdl)
    {
        LogError("HTTPAPI_CreateConnection: malloc error");
        return NULL;
    }
    memset(hdl, '\0', sizeof(EspHttpApiHandle));

    hdl->server = strdup(hostName);
    if (! hdl->server) {
        LogError("HTTPAPI_CreateConnection: malloc error");
        goto exit1;
    }

    esp_http_client_config_t esp_cfg = {0};

    esp_cfg.event_handler = httpEventHandler;
    esp_cfg.user_data = hdl; // Reference back to ourself
    esp_cfg.host = hdl->server;
    esp_cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
    esp_cfg.cert_pem = certificates;
    esp_cfg.buffer_size_tx = HTTP_BUFFER_SIZE;
    esp_cfg.buffer_size = HTTP_BUFFER_SIZE;
    esp_cfg.timeout_ms  = HTTP_TIMEOUT_MS;

    char* url = (char*) malloc(strlen(hostName) + 8 + 1); // len https:// + '\0'
    if (!url) {
        LogError("HTTPAPI_CreateConnection: malloc error");
        goto exit2;
    }
    sprintf(url, "https://%s", hdl->server);
    esp_cfg.url = url;

    hdl->espHdl = esp_http_client_init(&esp_cfg);
    if (NULL == hdl->espHdl)
    {
        LogError("HTTPAPI_CreateConnection: Client init failed");
        goto exit3;
    }

    free(url);
    return hdl;

exit3:
    free(url);
exit2:
    free(hdl->server);
exit1:
    free(hdl);

    return NULL;
}

void HTTPAPI_CloseConnection(HTTP_HANDLE handle)
{
    free(handle->server);

    esp_http_client_cleanup(handle->espHdl);

    free(handle);
}

HTTPAPI_RESULT HTTPAPI_ExecuteRequest(HTTP_HANDLE handle,
                                      HTTPAPI_REQUEST_TYPE requestType,
                                      const char* relativePath,
                                      HTTP_HEADERS_HANDLE httpHeadersHandle,
                                      const unsigned char* content,
                                      size_t contentLength,
                                      unsigned int* statusCode,
                                      HTTP_HEADERS_HANDLE responseHeadersHandle,
                                      BUFFER_HANDLE responseContent)
{
    // Sanity check
    if ((handle == NULL) ||
        (relativePath == NULL) ||
        (httpHeadersHandle == NULL) ||
        ((content == NULL) && (contentLength > 0))
    )
    {
        return HTTPAPI_INVALID_ARG;
    }

    size_t headersCount;
    if (HTTPHeaders_GetHeaderCount(httpHeadersHandle, &headersCount) != HTTP_HEADERS_OK)
    {
        return HTTPAPI_INVALID_ARG;
    }

    // Prepare parser callback
    handle->respHdr = responseHeadersHandle;
    handle->respBody = responseContent;

    // Do the work
    buildRequest(handle, requestType, relativePath, httpHeadersHandle, content, contentLength);

    esp_err_t err = esp_http_client_perform(handle->espHdl);
    if (ESP_OK != err)
    {
        LogError("HTTPAPI: esp_http_client_perform failed: %s (0x%x)", esp_err_to_name(err), err);
        return HTTPAPI_SEND_REQUEST_FAILED;
    }

    // Process result
    if (statusCode)
    {
        *statusCode = (unsigned int)esp_http_client_get_status_code(handle->espHdl);
    }

    return HTTPAPI_OK;
}

HTTPAPI_RESULT HTTPAPI_SetOption(HTTP_HANDLE handle, const char* optionName, const void* value)
{
    if ((NULL == handle ) ||
        (NULL == optionName) ||
        (NULL == value))
    {
        return HTTPAPI_INVALID_ARG;
    }

    if (0 == strcmp(optionName, OPTION_RESP_CB_FUNC))
    {
        RespCbCfgType* respCbCfg = (RespCbCfgType*)value;
        handle->respCb = respCbCfg->respCb;
        handle->respArg = respCbCfg->respArg;
        handle->statusCode = respCbCfg->statusCode;

        return HTTPAPI_OK;
    }

    return HTTPAPI_INVALID_ARG; // No options are supported
}


HTTPAPI_RESULT HTTPAPI_CloneOption(const char* optionName, const void* value, const void** savedValue)
{
    return HTTPAPI_INVALID_ARG; // No options are supported
}


static bool buildRequest(HTTP_HANDLE handle,
                         HTTPAPI_REQUEST_TYPE requestType,
                         const char* relativePath,
                         HTTP_HEADERS_HANDLE httpHeadersHandle,
                         const unsigned char* content,
                         size_t contentLength)
{
    if (contentLength)
    {
        // ESP-IDF v6.0: esp_http_client_set_post_field internally calls esp_http_client_get_header
        // to check for an existing Content-Type. In v6.0 that returns ESP_ERR_NOT_FOUND (instead
        // of ESP_OK + NULL) when the header is absent, causing set_post_field to bail early.
        // Pre-apply Content-Type from the caller's headers so the check succeeds.
        const char* contentType = HTTPHeaders_FindHeaderValue(httpHeadersHandle, "Content-Type");
        if (contentType)
        {
            esp_http_client_set_header(handle->espHdl, "Content-Type", contentType);
        }

        // Set body
        esp_err_t pfErr = esp_http_client_set_post_field(handle->espHdl, (const char*)content, contentLength);
        if (ESP_OK != pfErr)
        {
            LogError("HTTPAPI: set_post_field failed: %s (0x%x)", esp_err_to_name(pfErr), pfErr);
            return false;
        }

        LogInfo("HTTPAPI: Forced request to POST");
        requestType = HTTPAPI_REQUEST_POST;
    }

    // Set Request method
    switch( requestType )
    {
    case HTTPAPI_REQUEST_GET:
        esp_http_client_set_method(handle->espHdl, HTTP_METHOD_GET);
        break;

    case HTTPAPI_REQUEST_POST:
        esp_http_client_set_method(handle->espHdl, HTTP_METHOD_POST);
        break;

    default:
        return false;
    }

    // set url
    esp_err_t urlErr = esp_http_client_set_url(handle->espHdl, relativePath);
    if (ESP_OK != urlErr)
    {
        LogError("HTTPAPI: set_url failed: %s (0x%x)", esp_err_to_name(urlErr), urlErr);
        return false;
    }

    // Define some mandatory headers in HTTPAPI style
    char contentLengthStr[20];
    sprintf(contentLengthStr,"%d", contentLength);
    HTTP_HEADERS_HANDLE localHeadersHandle = HTTPHeaders_Clone(httpHeadersHandle);
    if (!localHeadersHandle)
    {
        localHeadersHandle = HTTPHeaders_Alloc();
        if (!localHeadersHandle)
        {
            LogError("HTTPAPI: Failed cloning headers");
            return false;
        }
    }

    HTTPHeaders_AddHeaderNameValuePair(localHeadersHandle,"Host", handle->server);
    HTTPHeaders_AddHeaderNameValuePair(localHeadersHandle,"User-Agent", "LINAK-GW/1.0 esp32");
    HTTPHeaders_AddHeaderNameValuePair(localHeadersHandle,"Content-Length", contentLengthStr);

    // Convert headers to esp_http_client style
    size_t hdCnt;
    if (HTTP_HEADERS_OK != HTTPHeaders_GetHeaderCount(localHeadersHandle, &hdCnt))
    {
        return false;
    }
    for(size_t i=0; i < hdCnt; i++)
    {
        char* tmpBuf;
        if (HTTP_HEADERS_OK == HTTPHeaders_GetHeader(localHeadersHandle, i, &tmpBuf))
        {
            char* key = strtok(tmpBuf, ":");
            if (key)
            {
                char* val = strtok(NULL, ":");
                if (val)
                {
                    val++; // key and val are separated by ": "
                    esp_http_client_set_header(handle->espHdl, key, val);
                }
            }
            free(tmpBuf);
        }
    }
    HTTPHeaders_Free(localHeadersHandle);

    return true;
}


static esp_err_t httpEventHandler(esp_http_client_event_t *evt)
{
    HTTP_HANDLE hdl = (HTTP_HANDLE)evt->user_data;

    switch(evt->event_id) {
    case HTTP_EVENT_ERROR:
        LogError("httpEventHandler: HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_HEADER: // Called on reception of a header line
    {
        HTTP_HEADERS_HANDLE respHdr = hdl->respHdr;

        if (respHdr)
        {
            if (HTTP_HEADERS_OK != HTTPHeaders_AddHeaderNameValuePair(respHdr, evt->header_key, evt->header_value))
            {
                LogError("httpEventHandler: Failded adding key %s", evt->header_key);
            }
        }
        break;
    }
    case HTTP_EVENT_ON_DATA: // Called on reception of body
    {
        if (hdl->respCb)
        { // Custom callback
            if (hdl->statusCode && !*hdl->statusCode) {
                // Fetch status code as fast as possible
                *hdl->statusCode = (unsigned int)esp_http_client_get_status_code(hdl->espHdl);
            }
            hdl->respCb(hdl->respArg, evt->data, evt->data_len);
        }
        else
        {
            BUFFER_HANDLE respBody = hdl->respBody;
            if (respBody)
            {
                if (evt->data_len)
                {
                    if (0 != BUFFER_append_build(respBody, evt->data, evt->data_len))
                    {
                        LogError("httpEventHandler: BUFFER_append_build failed");
                    }
                }
            }
        }
        break;
    }
    default: // Other events are of no interest
        break;
    }

    return ESP_OK;
}
