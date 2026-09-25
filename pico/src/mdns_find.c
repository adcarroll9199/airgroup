// One-shot mDNS lookup for Google Cast devices.
//
// Queries are sent from an ephemeral port ("legacy unicast", RFC 6762 §6.7), so responders
// reply directly to us and we don't need to join the multicast group. Replies usually carry
// the SRV/TXT/A records alongside the PTR; anything missing is asked for explicitly on the
// next round.

#include "mdns_find.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "pico/cyw43_arch.h"

#define MAX_NAME 128
#define MAX_SERVICES 12
#define MAX_HOSTS 12

#define TYPE_A 1
#define TYPE_PTR 12
#define TYPE_TXT 16
#define TYPE_SRV 33
#define TYPE_ANY 255

static const char SERVICE[] = "_googlecast._tcp.local";

typedef struct {
    char instance[MAX_NAME];
    char target[MAX_NAME];
    char friendly[64];
    uint16_t port;
    bool have_srv, have_txt;
} service_t;

typedef struct {
    char name[MAX_NAME];
    ip4_addr_t addr;
} host_t;

static service_t services[MAX_SERVICES];
static host_t hosts[MAX_HOSTS];
static int n_services, n_hosts;

static bool name_eq(const char *a, const char *b) {
    return strcasecmp(a, b) == 0;
}

static service_t *service_for(const char *instance) {
    for (int i = 0; i < n_services; i++)
        if (name_eq(services[i].instance, instance)) return &services[i];
    if (n_services == MAX_SERVICES) return NULL;
    service_t *s = &services[n_services++];
    memset(s, 0, sizeof *s);
    snprintf(s->instance, MAX_NAME, "%s", instance);
    return s;
}

static host_t *host_named(const char *name) {
    for (int i = 0; i < n_hosts; i++)
        if (name_eq(hosts[i].name, name)) return &hosts[i];
    return NULL;
}

// Reads a (possibly compressed) DNS name at *off into out as dotted text. Advances *off past it.
static bool read_name(const uint8_t *msg, size_t len, size_t *off, char *out, size_t outlen) {
    size_t pos = *off, o = 0;
    bool jumped = false;
    int hops = 0;
    for (;;) {
        if (pos >= len) return false;
        uint8_t l = msg[pos];
        if ((l & 0xC0) == 0xC0) {
            if (pos + 1 >= len || ++hops > 16) return false;
            if (!jumped) *off = pos + 2;
            jumped = true;
            pos = ((l & 0x3F) << 8) | msg[pos + 1];
            continue;
        }
        pos++;
        if (l == 0) break;
        if (pos + l > len) return false;
        if (o && o < outlen - 1) out[o++] = '.';
        for (int i = 0; i < l && o < outlen - 1; i++) out[o++] = (char)msg[pos + i];
        pos += l;
    }
    out[o] = 0;
    if (!jumped) *off = pos;
    return true;
}

static void parse_txt(service_t *s, const uint8_t *p, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t l = p[i++];
        if (i + l > len) break;
        if (l > 3 && memcmp(p + i, "fn=", 3) == 0) {
            size_t n = (size_t)(l - 3) < sizeof s->friendly - 1 ? (size_t)(l - 3) : sizeof s->friendly - 1;
            memcpy(s->friendly, p + i + 3, n);
            s->friendly[n] = 0;
            s->have_txt = true;
        }
        i += l;
    }
}

static void parse_response(const uint8_t *msg, size_t len) {
    if (len < 12 || !(msg[2] & 0x80)) return;  // not a response
    int qd = (msg[4] << 8) | msg[5];
    int rr = ((msg[6] << 8) | msg[7]) + ((msg[8] << 8) | msg[9]) + ((msg[10] << 8) | msg[11]);
    size_t off = 12;
    char name[MAX_NAME], data[MAX_NAME];
    for (int i = 0; i < qd; i++) {
        if (!read_name(msg, len, &off, name, sizeof name)) return;
        off += 4;
    }
    for (int i = 0; i < rr; i++) {
        if (!read_name(msg, len, &off, name, sizeof name) || off + 10 > len) return;
        uint16_t type = (msg[off] << 8) | msg[off + 1];
        uint16_t rdlen = (msg[off + 8] << 8) | msg[off + 9];
        size_t rd = off + 10;
        off = rd + rdlen;
        if (off > len) return;

        if (type == TYPE_PTR && name_eq(name, SERVICE)) {
            size_t p = rd;
            if (read_name(msg, len, &p, data, sizeof data)) service_for(data);
        } else if (type == TYPE_SRV && rdlen > 6) {
            service_t *s = service_for(name);
            size_t p = rd + 6;
            if (s && read_name(msg, len, &p, s->target, sizeof s->target)) {
                s->port = (msg[rd + 4] << 8) | msg[rd + 5];
                s->have_srv = true;
            }
        } else if (type == TYPE_TXT) {
            service_t *s = service_for(name);
            if (s) parse_txt(s, msg + rd, rdlen);
        } else if (type == TYPE_A && rdlen == 4) {
            host_t *h = host_named(name);
            if (!h && n_hosts < MAX_HOSTS) {
                h = &hosts[n_hosts++];
                snprintf(h->name, MAX_NAME, "%s", name);
            }
            if (h) IP4_ADDR(&h->addr, msg[rd], msg[rd + 1], msg[rd + 2], msg[rd + 3]);
        }
    }
}

static void on_udp(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    static uint8_t buf[1500];
    size_t len = pbuf_copy_partial(p, buf, sizeof buf, 0);
    pbuf_free(p);
    parse_response(buf, len);
}

static size_t put_name(uint8_t *p, const char *name) {
    size_t n = 0;
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t l = dot ? (size_t)(dot - name) : strlen(name);
        p[n++] = (uint8_t)l;
        memcpy(p + n, name, l);
        n += l;
        name += l + (dot ? 1 : 0);
    }
    p[n++] = 0;
    return n;
}

static size_t put_question(uint8_t *p, const char *name, uint16_t type) {
    size_t n = put_name(p, name);
    p[n++] = type >> 8;
    p[n++] = type & 0xFF;
    p[n++] = 0x80;  // QU: ask for a unicast reply
    p[n++] = 0x01;  // class IN
    return n;
}

static void send_query(struct udp_pcb *pcb) {
    uint8_t q[512];
    int count = 0;
    size_t n = 12;
    memset(q, 0, 12);
    q[1] = 0x42;  // query ID (legacy unicast replies echo it)
    n += put_question(q + n, SERVICE, TYPE_PTR), count++;
    for (int i = 0; i < n_services && n < 380; i++) {
        service_t *s = &services[i];
        if (!s->have_srv || !s->have_txt) n += put_question(q + n, s->instance, TYPE_ANY), count++;
        else if (!host_named(s->target)) n += put_question(q + n, s->target, TYPE_A), count++;
    }
    q[5] = count;

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, n, PBUF_RAM);
    if (!p) return;
    memcpy(p->payload, q, n);
    ip_addr_t group;
    IP4_ADDR(ip_2_ip4(&group), 224, 0, 0, 251);
    udp_sendto(pcb, p, &group, 5353);
    pbuf_free(p);
}

bool cast_find(const char *friendly_name, ip_addr_t *ip, uint16_t *port, uint32_t timeout_ms) {
    n_services = n_hosts = 0;
    struct udp_pcb *pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) return false;
    udp_bind(pcb, IP_ANY_TYPE, 0);
    udp_recv(pcb, on_udp, NULL);

    bool found = false;
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    absolute_time_t next_query = get_absolute_time();
    while (!found && absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        if (absolute_time_diff_us(get_absolute_time(), next_query) <= 0) {
            send_query(pcb);
            next_query = make_timeout_time_ms(700);
        }
        cyw43_arch_poll();
        for (int i = 0; i < n_services && !found; i++) {
            service_t *s = &services[i];
            host_t *h = s->have_srv ? host_named(s->target) : NULL;
            if (s->have_txt && h && strcmp(s->friendly, friendly_name) == 0) {
                ip_addr_copy_from_ip4(*ip, h->addr);
                *port = s->port;
                found = true;
            }
        }
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(10));
    }
    udp_remove(pcb);

    if (!found) {
        LOG("mDNS: \"%s\" not found. Cast devices seen:", friendly_name);
    }
    for (int i = 0; i < n_services && !found; i++)
        LOG("  \"%s\"%s", services[i].friendly, services[i].have_txt ? "" : " (no details)");
    return found;
}
