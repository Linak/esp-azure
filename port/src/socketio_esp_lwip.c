// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "azure_c_shared_utility/socketio.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "azure_c_shared_utility/singlylinkedlist.h"
#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/gbnetwork.h"
#include "azure_c_shared_utility/optimize_size.h"
#include "azure_c_shared_utility/optionhandler.h"
#include "azure_c_shared_utility/shared_util_options.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/const_defines.h"

#define SOCKET_SUCCESS                 0
#define INVALID_SOCKET                -1
#define CONNECT_TIMEOUT               10 // [s]

typedef enum IO_STATE_TAG
{
    IO_STATE_CLOSED,
    IO_STATE_OPENING,
    IO_STATE_OPEN,
    IO_STATE_CLOSING,
    IO_STATE_ERROR
} IO_STATE;

typedef struct PENDING_SOCKET_IO_TAG
{
    unsigned char* bytes;
    size_t size;
    ON_SEND_COMPLETE on_send_complete;
    void* callback_context;
    SINGLYLINKEDLIST_HANDLE pending_io_list;
} PENDING_SOCKET_IO;

typedef struct SOCKET_IO_INSTANCE_TAG
{
    int socket;
    SOCKETIO_ADDRESS_TYPE address_type;
    ON_BYTES_RECEIVED on_bytes_received;
    ON_IO_ERROR on_io_error;
    void* on_bytes_received_context;
    void* on_io_error_context;
    char* hostname;
    int port;
    IO_STATE io_state;
    SINGLYLINKEDLIST_HANDLE pending_io_list;
    unsigned char recv_bytes[RECEIVE_BYTES_VALUE];
} SOCKET_IO_INSTANCE;

typedef struct NETWORK_INTERFACE_DESCRIPTION_TAG
{
    char* name;
    char* mac_address;
    char* ip_address;
    struct NETWORK_INTERFACE_DESCRIPTION_TAG* next;
} NETWORK_INTERFACE_DESCRIPTION;

/* Following is copied from socketio_mbed.c */
/*this function will clone an option given by name and value*/
static void* socketio_CloneOption(const char* name, const void* value)
{
    (void)name;
    (void)value;
    return NULL;
}

/*this function destroys an option previously created*/
static void socketio_DestroyOption(const char* name, const void* value)
{
    (void)name;
    (void)value;
}

static OPTIONHANDLER_HANDLE socketio_retrieveoptions(CONCRETE_IO_HANDLE socket_io)
{
    OPTIONHANDLER_HANDLE result;
    (void)socket_io;
    result = OptionHandler_Create(socketio_CloneOption, socketio_DestroyOption, socketio_setoption);
    if (result == NULL)
    {
        /*return as is*/
    }
    else
    {
        /*insert here work to add the options to "result" handle*/
    }
    return result;
}
/* Copy from socketio_mbed.c - END */


static const IO_INTERFACE_DESCRIPTION socket_io_interface_description =
{
    socketio_retrieveoptions,
    socketio_create,
    socketio_destroy,
    socketio_open,
    socketio_close,
    socketio_send,
    socketio_dowork,
    socketio_setoption
};

static void indicate_error(SOCKET_IO_INSTANCE* socket_io_instance)
{
    socket_io_instance->io_state = IO_STATE_ERROR;
    if (socket_io_instance->on_io_error != NULL)
    {
        socket_io_instance->on_io_error(socket_io_instance->on_io_error_context);
    }
}

static int add_pending_io(SOCKET_IO_INSTANCE* socket_io_instance,
                          const unsigned char* buffer, size_t size,
                          ON_SEND_COMPLETE on_send_complete, void* callback_context)
{
    int result = 0;
    PENDING_SOCKET_IO* pending_socket_io = (PENDING_SOCKET_IO*)malloc(sizeof(PENDING_SOCKET_IO));
    if (pending_socket_io == NULL)
    {
        result = __FAILURE__;
    }
    else
    {
        pending_socket_io->bytes = (unsigned char*)malloc(size);
        if (pending_socket_io->bytes == NULL)
        {
            LogError("Allocation Failure: Unable to allocate pending list.");
            free(pending_socket_io);
            result = __FAILURE__;
        }
        else
        {
            pending_socket_io->size = size;
            pending_socket_io->on_send_complete = on_send_complete;
            pending_socket_io->callback_context = callback_context;
            pending_socket_io->pending_io_list = socket_io_instance->pending_io_list;
            (void)memcpy(pending_socket_io->bytes, buffer, size);

            if (singlylinkedlist_add(socket_io_instance->pending_io_list, pending_socket_io) == NULL)
            {
                LogError("Failure: Unable to add socket to pending list.");
                free(pending_socket_io->bytes);
                free(pending_socket_io);
                result = __FAILURE__;
            }
        }
    }
    return result;
}

static int lookup_address_and_initiate_socket_connection(SOCKET_IO_INSTANCE* socket_io_instance)
{
    int result = 0;
    int err;

    struct addrinfo addrInfoHintIp;
    struct sockaddr* connect_addr = NULL;
    socklen_t connect_addr_len;
    struct addrinfo* addrInfoIp = NULL;

    if (socket_io_instance->address_type == ADDRESS_TYPE_IP)
    {
        char portString[16];

        memset(&addrInfoHintIp, 0, sizeof(addrInfoHintIp));
        addrInfoHintIp.ai_family = AF_INET;
        addrInfoHintIp.ai_socktype = SOCK_STREAM;

        sprintf(portString, "%u", socket_io_instance->port);
        err = lwip_getaddrinfo(socket_io_instance->hostname, portString, &addrInfoHintIp, &addrInfoIp);
        if (err != 0)
        {
            LogError("Failure: getaddrinfo failure %d.", err);
            result = __FAILURE__;
        }
        else
        {
            connect_addr = addrInfoIp->ai_addr;
            connect_addr_len = sizeof(*addrInfoIp->ai_addr);
        }
    }
    else
    {
        LogError("Failure: Only ADDRESS_TYPE_IP is supported");
        result = __FAILURE__;
    }

    if (result == 0)
    {
        int flags;

        if ((-1 == (flags = lwip_fcntl(socket_io_instance->socket, F_GETFL, 0))) ||
            (lwip_fcntl(socket_io_instance->socket, F_SETFL, flags | O_NONBLOCK) == -1))
        {
            LogError("Failure: fcntl failure.");
            result = __FAILURE__;
        }
        else
        {
            err = lwip_connect(socket_io_instance->socket, connect_addr, connect_addr_len);
            if ((err != 0) && (errno != EINPROGRESS))
            {
                LogError("Failure: connect failure %d.", errno);
                result = __FAILURE__;
            }
        }
    }

    if (addrInfoIp != NULL)
    {
        freeaddrinfo(addrInfoIp);
    }

    return result;
}

static int wait_for_connection(SOCKET_IO_INSTANCE* socket_io_instance)
{
    int result = 0;
    int err;
    int retval;
    int select_errno = 0;

    fd_set fdset;
    struct timeval tv;

    FD_ZERO(&fdset);
    FD_SET(socket_io_instance->socket, &fdset);
    tv.tv_sec = CONNECT_TIMEOUT;
    tv.tv_usec = 0;

    do
    {  // Wait for activity on our socket
        retval = lwip_select(socket_io_instance->socket + 1, NULL, &fdset, NULL, &tv);

        if (retval < 0)
        {
            select_errno = errno;
        }
    } while (retval < 0 && select_errno == EINTR);

    if (retval != 1)
    {
        LogError("Failure: select failure.");
        result = __FAILURE__;
    }
    else
    {
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        err = lwip_getsockopt(socket_io_instance->socket, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (err != 0)
        {
            LogError("Failure: getsockopt failure %d.", errno);
            result = __FAILURE__;
        }
        else if (so_error != 0)
        {
            err = so_error;
            LogError("Failure: connect failure %d.", so_error);
            result = __FAILURE__;
        }
    }

    return result;
}




CONCRETE_IO_HANDLE socketio_create(void* io_create_parameters)
{
    SOCKETIO_CONFIG* socket_io_config = io_create_parameters;
    SOCKET_IO_INSTANCE* result;

    if (socket_io_config == NULL)
    {
        LogError("Invalid argument: socket_io_config is NULL");
        result = NULL;
    }
    else
    {
        result = malloc(sizeof(SOCKET_IO_INSTANCE));
        if (result != NULL)
        {
            result->address_type = ADDRESS_TYPE_IP;
            result->pending_io_list = singlylinkedlist_create();
            if (result->pending_io_list == NULL)
            {
                LogError("Failure: singlylinkedlist_create unable to create pending list.");
                free(result);
                result = NULL;
            }
            else
            {
                if (socket_io_config->hostname != NULL)
                {
                    result->hostname = (char*)malloc(strlen(socket_io_config->hostname) + 1);
                    if (result->hostname != NULL)
                    {
                        (void)strcpy(result->hostname, socket_io_config->hostname);
                    }

                    result->socket = INVALID_SOCKET;
                }
                else
                {
                    result->hostname = NULL;
                    result->socket = *((int*)socket_io_config->accepted_socket);
                }

                if ((result->hostname == NULL) && (result->socket == INVALID_SOCKET))
                {
                    LogError("Failure: hostname == NULL and socket is invalid.");
                    singlylinkedlist_destroy(result->pending_io_list);
                    free(result);
                    result = NULL;
                }
                else
                {
                    result->port = socket_io_config->port;
                    result->on_bytes_received = NULL;
                    result->on_io_error = NULL;
                    result->on_bytes_received_context = NULL;
                    result->on_io_error_context = NULL;
                    result->io_state = IO_STATE_CLOSED;
                }
            }
        }
        else
        {
            LogError("Allocation Failure: SOCKET_IO_INSTANCE");
        }
    }
    return result;
}

void socketio_destroy(CONCRETE_IO_HANDLE socket_io)
{
    if (socket_io != NULL)
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
        /* we cannot do much if the close fails, so just ignore the result */
        if (socket_io_instance->socket != INVALID_SOCKET)
        {
            lwip_close(socket_io_instance->socket);
        }

        /* clear allpending IOs */
        LIST_ITEM_HANDLE first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
        while (first_pending_io != NULL)
        {
            PENDING_SOCKET_IO* pending_socket_io = (PENDING_SOCKET_IO*)singlylinkedlist_item_get_value(first_pending_io);
            if (pending_socket_io != NULL)
            {
                free(pending_socket_io->bytes);
                free(pending_socket_io);
            }

            (void)singlylinkedlist_remove(socket_io_instance->pending_io_list, first_pending_io);
            first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
        }

        singlylinkedlist_destroy(socket_io_instance->pending_io_list);
        free(socket_io_instance->hostname);
        free(socket_io);
    }
}

int socketio_open(CONCRETE_IO_HANDLE socket_io,
                  ON_IO_OPEN_COMPLETE on_io_open_complete,
                  void* on_io_open_complete_context,
                  ON_BYTES_RECEIVED on_bytes_received,
                  void* on_bytes_received_context,
                  ON_IO_ERROR on_io_error,
                  void* on_io_error_context)
{
    int result = 0;

    SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
    if (socket_io == NULL)
    {
        LogError("Invalid argument: SOCKET_IO_INSTANCE is NULL");
        result = __FAILURE__;
    }
    else
    {
        if (socket_io_instance->io_state != IO_STATE_CLOSED)
        {
            LogError("Failure: socket state is not closed.");
            return __FAILURE__;
        }
        else if (socket_io_instance->socket != INVALID_SOCKET)
        {   // Opening an accepted socket
            socket_io_instance->on_bytes_received_context = on_bytes_received_context;
            socket_io_instance->on_bytes_received = on_bytes_received;
            socket_io_instance->on_io_error = on_io_error;
            socket_io_instance->on_io_error_context = on_io_error_context;

            socket_io_instance->io_state = IO_STATE_OPEN;
        }
        else
        {
            // Supports only AF_INET
            socket_io_instance->socket = lwip_socket( AF_INET,  SOCK_STREAM, 0);
            if (socket_io_instance->socket < SOCKET_SUCCESS)
            {
                LogError("Failure: socket create failure %d.", socket_io_instance->socket);
                result = __FAILURE__;
            }
            else if ((result = lookup_address_and_initiate_socket_connection(socket_io_instance)) != 0)
            {
                LogError("lookup_address_and_connect_socket failed");
            }
            else if ((result = wait_for_connection(socket_io_instance)) != 0)
            {
                LogError("wait_for_connection failed");
            }

            if (result == 0)
            {
                socket_io_instance->on_bytes_received = on_bytes_received;
                socket_io_instance->on_bytes_received_context = on_bytes_received_context;

                socket_io_instance->on_io_error = on_io_error;
                socket_io_instance->on_io_error_context = on_io_error_context;

                socket_io_instance->io_state = IO_STATE_OPEN;
            }
            else
            {
                if (socket_io_instance->socket >= SOCKET_SUCCESS)
                {
                    close(socket_io_instance->socket);
                }
                socket_io_instance->socket = INVALID_SOCKET;
            }
        }
    }

    if (on_io_open_complete != NULL)
    {
        on_io_open_complete(on_io_open_complete_context, result == 0 ? IO_OPEN_OK : IO_OPEN_ERROR);
    }
    return result;
}

int socketio_close(CONCRETE_IO_HANDLE socket_io, ON_IO_CLOSE_COMPLETE on_io_close_complete, void* callback_context)
{
    int result = 0;

    if (socket_io == NULL)
    {
        result = __FAILURE__;
    }
    else
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
        if ((socket_io_instance->io_state != IO_STATE_CLOSED) && (socket_io_instance->io_state != IO_STATE_CLOSING))
        {
            // Only close if the socket isn't already in the closed or closing state
            (void)lwip_shutdown(socket_io_instance->socket, SHUT_RDWR);
            lwip_close(socket_io_instance->socket);
            socket_io_instance->socket = INVALID_SOCKET;
            socket_io_instance->io_state = IO_STATE_CLOSED;
        }

        if (on_io_close_complete != NULL)
        {
            on_io_close_complete(callback_context);
        }
    }

    return result;
}

int socketio_send(CONCRETE_IO_HANDLE socket_io,
                  const void* buffer,
                  size_t size,
                  ON_SEND_COMPLETE on_send_complete,
                  void* callback_context)
{
    int result = 0;

    if ((socket_io == NULL) ||
        (buffer == NULL) ||
        (size == 0))
    {
        /* Invalid arguments */
        LogError("Invalid argument: send given invalid parameter");
        result = __FAILURE__;
    }
    else
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
        if (socket_io_instance->io_state != IO_STATE_OPEN)
        {
            LogError("Failure: socket state is not opened.");
            result = __FAILURE__;
        }
        else
        {
            LIST_ITEM_HANDLE first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
            if (first_pending_io != NULL)
            {
                if (add_pending_io(socket_io_instance, buffer, size, on_send_complete, callback_context) != 0)
                {
                    LogError("Failure: add_pending_io failed.");
                    result = __FAILURE__;
                }
            }
            else
            {
                ssize_t send_result = lwip_send(socket_io_instance->socket, buffer, size, 0);
                if ((send_result < 0) || ((size_t)send_result != size))
                {
                    if (send_result == INVALID_SOCKET)
                    { // Nothing sent. Check errno
                        if (errno != EAGAIN) /*send says "come back later" with EAGAIN - likely the socket buffer cannot accept more data*/
                        {
                            LogError("Failure: sending socket failed. errno=%d (%s).", errno, strerror(errno));
                            result = __FAILURE__;
                        }
                    }
                    else
                    { // Not all was send. Queue the unsent
                        if (add_pending_io(socket_io_instance, buffer + send_result, size - send_result, on_send_complete, callback_context) != 0)
                        {
                            LogError("Failure: add_pending_io failed.");
                            result = __FAILURE__;
                        }
                    }
                }
                else
                { // Everything was sent.
                    if (on_send_complete != NULL)
                    {
                        on_send_complete(callback_context, IO_SEND_OK);
                    }
                }
            }
        }
    }

    return result;
}

void socketio_dowork(CONCRETE_IO_HANDLE socket_io)
{
    if (socket_io != NULL)
    {
        SOCKET_IO_INSTANCE* socket_io_instance = (SOCKET_IO_INSTANCE*)socket_io;
        LIST_ITEM_HANDLE first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
        while (first_pending_io != NULL)
        {
            PENDING_SOCKET_IO* pending_socket_io = (PENDING_SOCKET_IO*)singlylinkedlist_item_get_value(first_pending_io);
            if (pending_socket_io == NULL)
            {
                indicate_error(socket_io_instance);
                LogError("Failure: retrieving socket from list");
                break;
            }

            ssize_t send_result = lwip_send(socket_io_instance->socket, pending_socket_io->bytes, pending_socket_io->size, 0);
            if ((send_result < 0) || ((size_t)send_result != pending_socket_io->size))
            {
                if (send_result == INVALID_SOCKET)
                { // Nothing sent. Check errno
                    if (errno == EAGAIN) /*send says "come back later" with EAGAIN - likely the socket buffer cannot accept more data*/
                    {
                        /*do nothing until next dowork */
                        break;
                    }
                    else
                    {
                        free(pending_socket_io->bytes);
                        free(pending_socket_io);
                        (void)singlylinkedlist_remove(socket_io_instance->pending_io_list, first_pending_io);

                        LogError("Failure: sending Socket information. errno=%d (%s).", errno, strerror(errno));
                        indicate_error(socket_io_instance);
                    }
                }
                else
                { // Not all data was sent.
                    /* simply wait until next dowork */
                    (void)memmove(pending_socket_io->bytes, pending_socket_io->bytes + send_result, pending_socket_io->size - send_result);
                    pending_socket_io->size -= send_result;
                    break;
                }
            }
            else
            { // Everything was sent - report back and cleanup
                if (pending_socket_io->on_send_complete != NULL)
                {
                    pending_socket_io->on_send_complete(pending_socket_io->callback_context, IO_SEND_OK);
                }

                free(pending_socket_io->bytes);
                free(pending_socket_io);
                if (singlylinkedlist_remove(socket_io_instance->pending_io_list, first_pending_io) != 0)
                {
                    indicate_error(socket_io_instance);
                    LogError("Failure: unable to remove socket from list");
                }
            }

            first_pending_io = singlylinkedlist_get_head_item(socket_io_instance->pending_io_list);
        }

        if (socket_io_instance->io_state == IO_STATE_OPEN)
        {
            ssize_t received = 0;
            do
            {
                received = lwip_recv(socket_io_instance->socket, socket_io_instance->recv_bytes, RECEIVE_BYTES_VALUE, 0);
                if (received > 0)
                {
                    if (socket_io_instance->on_bytes_received != NULL)
                    {
                        /* Explicitly ignoring here the result of the callback */
                        (void)socket_io_instance->on_bytes_received(socket_io_instance->on_bytes_received_context, socket_io_instance->recv_bytes, received);
                    }
                }
                else if (received == 0)
                {
                    // Do not log error here due to this is probably the socket being closed on the other end
                    indicate_error(socket_io_instance);
                }
                else if (received < 0 && errno != EAGAIN)
                {
                    LogError("Socketio_Failure: Receiving data from endpoint: errno=%d.", errno);
                    indicate_error(socket_io_instance);
                }

            } while (received > 0 && socket_io_instance->io_state == IO_STATE_OPEN);
        }
    }
}

int socketio_setoption(CONCRETE_IO_HANDLE socket_io, const char* optionName, const void* value)
{
    /* Not implementing any options */
    return __FAILURE__;
}

const IO_INTERFACE_DESCRIPTION* socketio_get_interface_description(void)
{
    return &socket_io_interface_description;
}

