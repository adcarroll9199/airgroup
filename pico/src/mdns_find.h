#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "lwip/ip_addr.h"

// Finds a Google Cast device or speaker group by its friendly name (as shown in the
// Google Home app) using mDNS. Blocks, polling the network, for up to timeout_ms.
bool cast_find(const char *friendly_name, ip_addr_t *ip, uint16_t *port, uint32_t timeout_ms);
