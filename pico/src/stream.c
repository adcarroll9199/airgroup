// Live WAV-over-HTTP server on raw lwIP TCP.
//
// The stream is mono: Google Home/Nest speakers are single-driver, and half the bytes means
// our limited RAM holds twice as many seconds of backlog for a listener that pauses reading.
//
// Audio is produced at exactly real-time rate (paced by the system timer) into a ring
// buffer. Each listener reads from the ring at its own position; a listener that falls more
// than a ring's worth behind skips ahead instead of stalling everyone else.

#include "stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "lwip/tcp.h"
#include "pico/time.h"

#define FRAME_BYTES 2                  // mono int16
#define RING_BYTES (64 * 1024)          // ~740 ms of 44.1 kHz mono
#define BYTES_PER_MS (STREAM_SAMPLE_RATE * FRAME_BYTES / 1000)
#define MAX_CLIENTS 3
#define MAX_REQUEST 512

typedef struct {
    struct tcp_pcb *pcb;
    bool streaming;
    uint64_t pos;  // absolute byte position in the audio stream
    uint32_t bench_left;  // /bench: bytes still to send as fast as possible
    char req[MAX_REQUEST];
    size_t req_len;
} client_t;

static uint8_t ring[RING_BYTES];
static uint64_t produced;  // bytes of audio produced since start
static uint64_t start_us;
static pcm_source_fn source;
static client_t clients[MAX_CLIENTS];
static uint32_t max_backlog;  // most audio (bytes) buffered on our side for a listener
static uint32_t sent_bytes;   // audio handed to TCP since the last stats call
static uint32_t write_errors;
static err_t last_write_error;
volatile bool stream_test_requested;
volatile int stream_test_seconds;

static const char STREAM_HEADERS[] =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: audio/wav\r\n"
    "Cache-Control: no-cache, no-store\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Connection: close\r\n\r\n";

// 44-byte WAV header with "unknown/huge" sizes, as used for live WAV streams.
static const uint8_t WAV_HEADER[44] = {
    'R', 'I', 'F', 'F', 0xFF, 0xFF, 0xFF, 0xFF, 'W', 'A', 'V', 'E',
    'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,              // PCM, 1 channel
    0x44, 0xAC, 0, 0,                                         // 44100 Hz
    0x88, 0x58, 0x01, 0,                                      // byte rate 88200
    2, 0, 16, 0,                                              // block align 2, 16 bits
    'd', 'a', 't', 'a', 0xDB, 0xFF, 0xFF, 0xFF,
};

uint32_t stream_take_max_backlog_ms(void) {
    uint32_t ms = max_backlog / BYTES_PER_MS;
    max_backlog = 0;
    return ms;
}

void stream_log_tcp_stats(uint32_t interval_ms) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *cl = &clients[i];
        if (!cl->pcb || !cl->streaming) continue;
        struct tcp_pcb *p = cl->pcb;
        LOG("tcp: sent %lu B/s (need %d), peer window %u, cwnd %u, sndbuf free %u, queued segs %u, srtt ~%d ms, retransmits %u",
            (unsigned long)(sent_bytes * 1000ULL / interval_ms), STREAM_SAMPLE_RATE * FRAME_BYTES, (unsigned)p->snd_wnd,
            (unsigned)p->cwnd, (unsigned)tcp_sndbuf(p), (unsigned)tcp_sndqueuelen(p), (p->sa >> 3) * 500,
            (unsigned)p->nrtx);
    }
    if (write_errors) LOG("tcp: %lu write errors (last %d)", (unsigned long)write_errors, last_write_error);
    sent_bytes = write_errors = 0;
}

int stream_listener_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) n += clients[i].streaming;
    return n;
}

static void client_close(client_t *cl) {
    if (!cl->pcb) return;
    tcp_arg(cl->pcb, NULL);
    tcp_recv(cl->pcb, NULL);
    tcp_err(cl->pcb, NULL);
    if (tcp_close(cl->pcb) != ERR_OK) tcp_abort(cl->pcb);
    if (cl->streaming) LOG("http: listener disconnected");
    cl->pcb = NULL;
    cl->streaming = false;
}

static void send_text(client_t *cl, const char *status, const char *ctype, const char *body, size_t len) {
    char hdr[160];
    int n = snprintf(hdr, sizeof hdr, "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                     status, ctype, (unsigned)len);
    tcp_write(cl->pcb, hdr, n, TCP_WRITE_FLAG_COPY);
    if (len) tcp_write(cl->pcb, body, len, TCP_WRITE_FLAG_COPY);
    tcp_output(cl->pcb);
    client_close(cl);
}

static void handle_request(client_t *cl) {
    char method[8] = "", path[64] = "";
    sscanf(cl->req, "%7s %63s", method, path);
    bool head = strcmp(method, "HEAD") == 0;
    LOG("http: %s %s", method, path);

    if (strcmp(path, STREAM_PATH) == 0) {
        tcp_write(cl->pcb, STREAM_HEADERS, sizeof STREAM_HEADERS - 1, TCP_WRITE_FLAG_COPY);
        if (head) {
            tcp_output(cl->pcb);
            client_close(cl);
            return;
        }
        tcp_write(cl->pcb, WAV_HEADER, sizeof WAV_HEADER, TCP_WRITE_FLAG_COPY);
        tcp_output(cl->pcb);
        cl->pos = produced;  // start live, at the newest audio
        cl->streaming = true;
        LOG("http: listener connected");
    } else if (strcmp(path, "/bench") == 0) {
        // Throughput test: 2 MB of zeros as fast as TCP allows.
        static const char hdr[] = "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 2097152\r\n\r\n";
        tcp_write(cl->pcb, hdr, sizeof hdr - 1, TCP_WRITE_FLAG_COPY);
        cl->bench_left = 2 * 1024 * 1024;
    } else if (strncmp(path, "/test", 5) == 0 && (path[5] == 0 || path[5] == '?')) {
        int secs = 0;
        const char *q = strstr(path, "s=");
        if (q) secs = atoi(q + 2);
        stream_test_seconds = secs > 0 && secs <= 3600 ? secs : 0;
        stream_test_requested = true;
        send_text(cl, "200 OK", "text/plain", "starting test\n", 14);
    } else if (strcmp(path, "/log") == 0) {
        static char buf[4096];
        send_text(cl, "200 OK", "text/plain; charset=utf-8", buf, log_snapshot(buf, sizeof buf));
    } else if (strcmp(path, "/") == 0) {
        char body[128];
        int n = snprintf(body, sizeof body, "airgroup-pico\nlisteners: %d\nlog: /log\n", stream_listener_count());
        send_text(cl, "200 OK", "text/plain", body, n);
    } else {
        send_text(cl, "404 Not Found", "text/plain", "not found\n", 10);
    }
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    client_t *cl = arg;
    if (!p) {
        client_close(cl);
        return ERR_OK;
    }
    tcp_recved(pcb, p->tot_len);
    if (!cl->streaming && cl->req_len < MAX_REQUEST - 1) {
        cl->req_len += pbuf_copy_partial(p, cl->req + cl->req_len, MAX_REQUEST - 1 - cl->req_len, 0);
        cl->req[cl->req_len] = 0;
        if (strstr(cl->req, "\r\n\r\n") || cl->req_len == MAX_REQUEST - 1) handle_request(cl);
    }
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    client_t *cl = arg;
    if (cl->streaming) LOG("http: listener dropped (%d)", err);
    cl->pcb = NULL;
    cl->streaming = false;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    if (err != ERR_OK || !pcb) return ERR_VAL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *cl = &clients[i];
        if (cl->pcb) continue;
        memset(cl, 0, sizeof *cl);
        cl->pcb = pcb;
        tcp_arg(pcb, cl);
        tcp_recv(pcb, on_recv);
        tcp_err(pcb, on_err);
        return ERR_OK;
    }
    tcp_abort(pcb);
    return ERR_ABRT;
}

void stream_start(uint16_t port, pcm_source_fn src) {
    source = src;
    start_us = time_us_64();
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    tcp_bind(pcb, IP_ANY_TYPE, port);
    pcb = tcp_listen_with_backlog(pcb, MAX_CLIENTS);
    tcp_accept(pcb, on_accept);
    LOG("http: listening on port %u", port);
}

static void produce(void) {
    uint64_t due_frames = (time_us_64() - start_us) * STREAM_SAMPLE_RATE / 1000000;
    uint64_t have_frames = produced / FRAME_BYTES;
    if (due_frames - have_frames > RING_BYTES / FRAME_BYTES) {
        // We were stalled for longer than the ring holds (e.g. TLS handshake); jump ahead.
        have_frames = due_frames - 256;
        produced = have_frames * FRAME_BYTES;
    }
    while (have_frames < due_frames) {
        size_t idx = produced % RING_BYTES;
        size_t frames = (RING_BYTES - idx) / FRAME_BYTES;
        if (frames > due_frames - have_frames) frames = due_frames - have_frames;
        if (frames > 256) frames = 256;
        int16_t stereo[2 * 256];
        size_t got = source ? source(stereo, frames) : 0;
        int16_t *dst = (int16_t *)(ring + idx);
        for (size_t i = 0; i < got; i++) dst[i] = (int16_t)((stereo[2 * i] + stereo[2 * i + 1]) / 2);
        memset(dst + got, 0, (frames - got) * FRAME_BYTES);
        produced += frames * FRAME_BYTES;
        have_frames += frames;
    }
}

static void send_to(client_t *cl) {
    uint32_t backlog = (uint32_t)(produced - cl->pos) + (TCP_SND_BUF - tcp_sndbuf(cl->pcb));
    if (backlog > max_backlog) max_backlog = backlog;
    if (produced - cl->pos > RING_BYTES - 4096) {
        LOG("http: listener fell behind; skipping ahead");
        cl->pos = produced - RING_BYTES / 2;
    }
    bool wrote = false;
    while (cl->pos < produced) {
        size_t idx = cl->pos % RING_BYTES;
        size_t n = produced - cl->pos;
        if (n > RING_BYTES - idx) n = RING_BYTES - idx;
        if (n > TCP_MSS) n = TCP_MSS;  // one segment per write: small allocations, partial progress
        if (n > tcp_sndbuf(cl->pcb) || tcp_sndqueuelen(cl->pcb) >= TCP_SND_QUEUELEN - 2) break;
        err_t err = tcp_write(cl->pcb, ring + idx, n, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) {
            write_errors++;
            last_write_error = err;
            break;
        }
        cl->pos += n;
        sent_bytes += n;
        wrote = true;
    }
    if (wrote) tcp_output(cl->pcb);
}

static void send_bench(client_t *cl) {
    static const uint8_t zeros[TCP_MSS];
    bool wrote = false;
    while (cl->bench_left) {
        size_t n = cl->bench_left < TCP_MSS ? cl->bench_left : TCP_MSS;
        if (tcp_sndbuf(cl->pcb) < n || tcp_write(cl->pcb, zeros, n, 0) != ERR_OK) break;
        cl->bench_left -= n;
        wrote = true;
    }
    if (wrote) tcp_output(cl->pcb);
    if (!cl->bench_left) client_close(cl);
}

void stream_poll(void) {
    produce();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].pcb) continue;
        if (clients[i].streaming) send_to(&clients[i]);
        else if (clients[i].bench_left) send_bench(&clients[i]);
    }
}
