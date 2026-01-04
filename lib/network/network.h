#ifndef NETWORK_H_
#define NETWORK_H_

#include <stdint.h>
#include <Client.h>

typedef enum
{
    NET_CONNECT_FAILED,
    NET_DISCONNECTED,
    NET_CONNECTING,
    NET_CONNECTED,
} NetStateEvent_t;

bool network_init(void);
bool network_is_connected(void);
Client *network_get_client(void);

#endif