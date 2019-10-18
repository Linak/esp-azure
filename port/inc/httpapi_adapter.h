/*
 * httpapi_adapter.h
 *
 *  Created on: Oct 10, 2019
 *      Author: kibo
 *
 *      This file provides structures and constants necessary to 'hook in' a callback to be 
 *      used for partial handling of HTTP responses
 *
 */


#ifdef __cplusplus
extern "C" {
#endif

/* @brief Callback for partial response handling
 * @param respArg A caller provided argument.
 * @param data pointer to the received chunk
 * @param len length of chunk
 */
typedef void (*HttpApiResponseCbFuncType)(void* respArg, char* data, size_t len);

/// The option type given to HTTPAPI_SetOption when installing a callback
extern const char* OPTION_RESP_CB_FUNC;

/* brief The argument given to HTTPAPI_SetOption when installing a callback
 * @param respCb The callback
 * @param respArg User provided argument available when callback is called
 * @param statusCode Pointer to storage for statusCode. Will be initialized
 *                   with status code of request before callback is called
 */
typedef struct RespCbCfg {
    HttpApiResponseCbFuncType respCb;
    void*                     respArg;
    unsigned int*             statusCode;
} RespCbCfgType;

#ifdef __cplusplus
}
#endif
