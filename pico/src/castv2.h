#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "lwip/ip_addr.h"

typedef enum {
    CAST_IDLE,        // not connected
    CAST_CONNECTING,  // TCP + TLS handshake
    CAST_LAUNCHING,   // asked the device to start the Default Media Receiver
    CAST_LOADING,     // asked the receiver to play our URL
    CAST_PLAYING,     // receiver reports PLAYING or BUFFERING
    CAST_FAILED,
} cast_state_t;

// Connects to a Cast device/group and makes it play `url` (a live stream of `content_type`).
void cast_start(const ip_addr_t *ip, uint16_t port, const char *url, const char *content_type);
// Stops our app on the device and closes the connection.
void cast_stop(void);
// Call regularly from the main loop (heartbeats, timeouts).
void cast_poll(void);
cast_state_t cast_state(void);
const char *cast_state_name(cast_state_t s);
