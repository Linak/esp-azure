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
#include <stdio.h>
#include "httpapi_adapter.h"
#include <string.h>

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

HTTPAPI_RESULT HTTPAPI_Init(void)
{
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
        // Set body
        if (ESP_OK != esp_http_client_set_post_field(handle->espHdl, (const char*)content, contentLength))
        {
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
    if (ESP_OK != esp_http_client_set_url(handle->espHdl, relativePath))
    {
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

    return true;
}


static esp_err_t httpEventHandler(esp_http_client_event_t *evt)
{
    HTTP_HANDLE hdl = (HTTP_HANDLE)evt->user_data;

    switch(evt->event_id) {
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
