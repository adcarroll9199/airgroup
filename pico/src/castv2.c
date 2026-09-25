// Minimal Google Cast (CASTV2) sender: TLS to the device, then length-prefixed protobuf
// CastMessages carrying JSON payloads. Just enough to launch the Default Media Receiver,
// load a URL, answer heartbeats and stop again.

#include "castv2.h"

#include <stdio.h>
#include <string.h>

#include "log.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "pico/time.h"

#define NS_CONNECTION "urn:x-cast:com.google.cast.tp.connection"
#define NS_HEARTBEAT "urn:x-cast:com.google.cast.tp.heartbeat"
#define NS_RECEIVER "urn:x-cast:com.google.cast.receiver"
#define NS_MEDIA "urn:x-cast:com.google.cast.media"
#define SENDER_ID "sender-airgroup"
#define RECEIVER_ID "receiver-0"
#define MEDIA_RECEIVER_APP "CC1AD845"

#define RX_BUF 8192
#define STEP_TIMEOUT_MS 20000
#define PING_INTERVAL_MS 5000

static struct {
    cast_state_t state;
    struct altcp_tls_config *tls;
    struct altcp_pcb *pcb;
    char url[160];
    char content_type[32];
    char transport_id[64];
    char session_id[64];
    int request_id;
    absolute_time_t step_deadline;
    absolute_time_t next_ping;
    uint8_t rx[RX_BUF + 1];
    size_t rx_len;
    size_t skip;  // bytes still to discard of an oversized message
} c;

static const char *const STATE_NAMES[] = {"idle", "connecting", "launching", "loading", "playing", "failed"};

const char *cast_state_name(cast_state_t s) {
    return STATE_NAMES[s];
}

cast_state_t cast_state(void) {
    return c.state;
}

static void set_state(cast_state_t s) {
    if (s != c.state) LOG("cast: %s -> %s", STATE_NAMES[c.state], STATE_NAMES[s]);
    c.state = s;
    c.step_deadline = make_timeout_time_ms(STEP_TIMEOUT_MS);
}

// --- protobuf encoding ------------------------------------------------------------------

static size_t pb_varint(uint8_t *p, uint32_t v) {
    size_t n = 0;
    while (v >= 0x80) {
        p[n++] = (v & 0x7F) | 0x80;
        v >>= 7;
    }
    p[n++] = (uint8_t)v;
    return n;
}

static size_t pb_string(uint8_t *p, int field, const char *s) {
    size_t len = strlen(s), n = 0;
    p[n++] = (uint8_t)((field << 3) | 2);
    n += pb_varint(p + n, len);
    memcpy(p + n, s, len);
    return n + len;
}

static void send_msg(const char *ns, const char *dest, const char *json) {
    if (!c.pcb) return;
    static uint8_t buf[1024];
    size_t n = 4;
    buf[n++] = 0x08, buf[n++] = 0;  // protocol_version = CASTV2_1_0
    n += pb_string(buf + n, 2, SENDER_ID);
    n += pb_string(buf + n, 3, dest);
    n += pb_string(buf + n, 4, ns);
    buf[n++] = 0x28, buf[n++] = 0;  // payload_type = STRING
    n += pb_string(buf + n, 6, json);
    uint32_t body = n - 4;
    buf[0] = body >> 24, buf[1] = body >> 16, buf[2] = body >> 8, buf[3] = body;

    err_t err = altcp_write(c.pcb, buf, n, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK) err = altcp_output(c.pcb);
    if (err != ERR_OK) LOG("cast: send failed (%d)", err);
}

// --- JSON helpers (the payloads are compact JSON; we only need a few string fields) --------

static bool json_string(const char *json, const char *key, char *out, size_t outlen) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t n = 0;
    while (*p && *p != '"' && n < outlen - 1) out[n++] = *p++;
    out[n] = 0;
    return true;
}

// --- message handling -----------------------------------------------------------------------

static void send_load(void) {
    static char json[512];
    snprintf(json, sizeof json,
             "{\"type\":\"LOAD\",\"requestId\":%d,\"autoplay\":true,\"media\":{"
             "\"contentId\":\"%s\",\"contentType\":\"%s\",\"streamType\":\"LIVE\","
             "\"metadata\":{\"metadataType\":0,\"title\":\"airgroup\"}}}",
             ++c.request_id, c.url, c.content_type);
    send_msg(NS_CONNECTION, c.transport_id, "{\"type\":\"CONNECT\"}");
    send_msg(NS_MEDIA, c.transport_id, json);
    set_state(CAST_LOADING);
}

static void handle_payload(const char *ns, const char *src, char *json) {
    char type[32] = "";
    json_string(json, "type", type, sizeof type);

    if (strcmp(ns, NS_HEARTBEAT) == 0) {
        if (strcmp(type, "PING") == 0) send_msg(NS_HEARTBEAT, src, "{\"type\":\"PONG\"}");
        return;
    }
    LOG("cast: <- %s %.160s", type, json);

    if (strcmp(ns, NS_RECEIVER) == 0 && strcmp(type, "RECEIVER_STATUS") == 0) {
        const char *app = strstr(json, "\"appId\":\"" MEDIA_RECEIVER_APP "\"");
        if (app && c.state == CAST_LAUNCHING) {
            json_string(app, "sessionId", c.session_id, sizeof c.session_id);
            if (json_string(app, "transportId", c.transport_id, sizeof c.transport_id)) send_load();
        } else if (!app && (c.state == CAST_LOADING || c.state == CAST_PLAYING)) {
            LOG("cast: our receiver app is no longer running");
            set_state(CAST_FAILED);
        }
    } else if (strcmp(ns, NS_MEDIA) == 0) {
        char player[16] = "";
        if (strcmp(type, "LOAD_FAILED") == 0 || strcmp(type, "LOAD_CANCELLED") == 0 ||
            strcmp(type, "INVALID_REQUEST") == 0) {
            set_state(CAST_FAILED);
        } else if (json_string(json, "playerState", player, sizeof player)) {
            if (strcmp(player, "PLAYING") == 0 || strcmp(player, "BUFFERING") == 0) {
                set_state(CAST_PLAYING);
            } else if (strcmp(player, "IDLE") == 0 && c.state == CAST_PLAYING) {
                set_state(CAST_FAILED);
            }
        }
    } else if (strcmp(ns, NS_CONNECTION) == 0 && strcmp(type, "CLOSE") == 0 &&
               strcmp(src, c.transport_id) == 0) {
        LOG("cast: receiver closed our session");
        set_state(CAST_FAILED);
    }
}

// Decodes one CastMessage (protobuf) and dispatches its payload.
static void handle_message(uint8_t *msg, size_t len) {
    char ns[64] = "", src[64] = "";
    uint8_t *payload = NULL;
    size_t payload_len = 0, i = 0;
    while (i < len) {
        uint32_t key = 0, v = 0;
        for (int shift = 0; i < len; shift += 7) {
            key |= (uint32_t)(msg[i] & 0x7F) << shift;
            if (!(msg[i++] & 0x80)) break;
        }
        int field = key >> 3, wire = key & 7;
        if (wire == 0) {
            while (i < len && (msg[i++] & 0x80)) {
            }
        } else if (wire == 2) {
            for (int shift = 0; i < len; shift += 7) {
                v |= (uint32_t)(msg[i] & 0x7F) << shift;
                if (!(msg[i++] & 0x80)) break;
            }
            if (i + v > len) return;
            if ((field == 2 || field == 4) && v < 64) {
                char *dst = field == 2 ? src : ns;
                memcpy(dst, msg + i, v);
                dst[v] = 0;
            } else if (field == 6) {
                payload = msg + i;
                payload_len = v;
            }
            i += v;
        } else if (wire == 1) {
            i += 8;
        } else if (wire == 5) {
            i += 4;
        } else {
            return;
        }
    }
    if (!payload) return;
    // Terminate the JSON in place for the string helpers; restore the byte afterwards.
    uint8_t saved = payload[payload_len];
    payload[payload_len] = 0;
    handle_payload(ns, src, (char *)payload);
    payload[payload_len] = saved;
}

static void rx_bytes(const uint8_t *data, size_t len) {
    while (len) {
        if (c.skip) {
            size_t n = len < c.skip ? len : c.skip;
            c.skip -= n, data += n, len -= n;
            continue;
        }
        size_t n = RX_BUF - c.rx_len;
        if (n > len) n = len;
        memcpy(c.rx + c.rx_len, data, n);
        c.rx_len += n, data += n, len -= n;

        while (c.rx_len >= 4) {
            uint32_t mlen = (c.rx[0] << 24) | (c.rx[1] << 16) | (c.rx[2] << 8) | c.rx[3];
            if (mlen > RX_BUF - 4) {
                LOG("cast: skipping oversized message (%lu bytes)", (unsigned long)mlen);
                size_t have = c.rx_len - 4;
                if (have >= mlen) {
                    memmove(c.rx, c.rx + 4 + mlen, c.rx_len - 4 - mlen);
                    c.rx_len -= 4 + mlen;
                } else {
                    c.skip = mlen - have;
                    c.rx_len = 0;
                }
                continue;
            }
            if (c.rx_len < 4 + mlen) break;
            handle_message(c.rx + 4, mlen);
            memmove(c.rx, c.rx + 4 + mlen, c.rx_len - 4 - mlen);
            c.rx_len -= 4 + mlen;
        }
    }
}

// --- lwIP callbacks -------------------------------------------------------------------------

static void close_conn(void) {
    if (!c.pcb) return;
    altcp_arg(c.pcb, NULL);
    altcp_recv(c.pcb, NULL);
    altcp_err(c.pcb, NULL);
    if (altcp_close(c.pcb) != ERR_OK) altcp_abort(c.pcb);
    c.pcb = NULL;
}

static err_t on_recv(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    if (!p) {
        LOG("cast: connection closed by device");
        close_conn();
        set_state(CAST_FAILED);
        return ERR_OK;
    }
    for (struct pbuf *q = p; q; q = q->next) rx_bytes(q->payload, q->len);
    altcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    LOG("cast: connection error %d", err);
    c.pcb = NULL;  // already freed by lwIP
    set_state(CAST_FAILED);
}

static err_t on_connected(void *arg, struct altcp_pcb *pcb, err_t err) {
    LOG("cast: TLS connected");
    send_msg(NS_CONNECTION, RECEIVER_ID, "{\"type\":\"CONNECT\"}");
    char json[96];
    snprintf(json, sizeof json, "{\"type\":\"LAUNCH\",\"appId\":\"" MEDIA_RECEIVER_APP "\",\"requestId\":%d}",
             ++c.request_id);
    send_msg(NS_RECEIVER, RECEIVER_ID, json);
    c.next_ping = make_timeout_time_ms(PING_INTERVAL_MS);
    set_state(CAST_LAUNCHING);
    return ERR_OK;
}

// --- public API -----------------------------------------------------------------------------

void cast_start(const ip_addr_t *ip, uint16_t port, const char *url, const char *content_type) {
    close_conn();
    c.rx_len = c.skip = 0;
    c.transport_id[0] = c.session_id[0] = 0;
    strncpy(c.url, url, sizeof c.url - 1);
    strncpy(c.content_type, content_type, sizeof c.content_type - 1);

    if (!c.tls) c.tls = altcp_tls_create_config_client(NULL, 0);
    c.pcb = c.tls ? altcp_tls_new(c.tls, IPADDR_TYPE_V4) : NULL;
    if (!c.pcb) {
        LOG("cast: out of memory creating TLS connection");
        set_state(CAST_FAILED);
        return;
    }
    altcp_recv(c.pcb, on_recv);
    altcp_err(c.pcb, on_err);
    LOG("cast: connecting to %s:%u", ipaddr_ntoa(ip), port);
    set_state(CAST_CONNECTING);
    err_t err = altcp_connect(c.pcb, ip, port, on_connected);
    if (err != ERR_OK) {
        LOG("cast: connect failed (%d)", err);
        close_conn();
        set_state(CAST_FAILED);
    }
}

void cast_stop(void) {
    if (c.pcb && c.session_id[0]) {
        char json[128];
        snprintf(json, sizeof json, "{\"type\":\"STOP\",\"sessionId\":\"%s\",\"requestId\":%d}", c.session_id,
                 ++c.request_id);
        send_msg(NS_RECEIVER, RECEIVER_ID, json);
    }
    close_conn();
    set_state(CAST_IDLE);
}

void cast_poll(void) {
    if (!c.pcb) return;
    if (c.state != CAST_PLAYING && absolute_time_diff_us(get_absolute_time(), c.step_deadline) < 0) {
        LOG("cast: timed out while %s", STATE_NAMES[c.state]);
        close_conn();
        set_state(CAST_FAILED);
        return;
    }
    if (c.state != CAST_CONNECTING && absolute_time_diff_us(get_absolute_time(), c.next_ping) < 0) {
        send_msg(NS_HEARTBEAT, RECEIVER_ID, "{\"type\":\"PING\"}");
        c.next_ping = make_timeout_time_ms(PING_INTERVAL_MS);
    }
}
