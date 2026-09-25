// airgroup for Raspberry Pi Pico W — step 1: prove the Pico can drive a Cast speaker group.
//
// Joins Wi-Fi, finds CAST_TARGET via mDNS, serves a gentle test chime as a live WAV stream
// and tells the group to play it for TEST_SECONDS. Progress is logged over USB serial and
// at http://airgroup-pico.local:8090/log

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "castv2.h"
#include "config.h"
#include "log.h"
#include "lwip/netif.h"
#include "mdns_find.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "stream.h"

#define HTTP_PORT 8090

// 441 Hz sine: exactly 100 samples per cycle at 44.1 kHz, so a small table loops seamlessly.
static int16_t sine[100];
static uint32_t tone_frame;

// Two soft beeps (0.25 s each) every 2 seconds, at about -14 dBFS.
static size_t tone_source(int16_t *dst, size_t frames) {
    for (size_t i = 0; i < frames; i++, tone_frame++) {
        uint32_t t = tone_frame % (2 * STREAM_SAMPLE_RATE);
        bool on = t < STREAM_SAMPLE_RATE / 4 || (t >= STREAM_SAMPLE_RATE / 2 && t < STREAM_SAMPLE_RATE * 3 / 4);
        int16_t s = on ? sine[tone_frame % 100] : 0;
        dst[2 * i] = dst[2 * i + 1] = s;
    }
    return frames;
}

static void init_sine(void) {
    for (int k = 0; k < 100; k++) sine[k] = (int16_t)(6500.0f * sinf(2.0f * 3.14159265f * k / 100.0f));
}

// Clock for mbedTLS (MBEDTLS_PLATFORM_MS_TIME_ALT).
long long mbedtls_ms_time(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void log_memory(const char *when) {
    struct mallinfo mi = mallinfo();
    LOG("mem (%s): heap in use %d bytes, free in heap %d bytes", when, mi.uordblks, mi.fordblks);
}

static int scan_total, scan_matches;

static int on_scan_result(void *env, const cyw43_ev_scan_result_t *r) {
    scan_total++;
    LOG("wifi scan: \"%.*s\" ch %d, %d dBm, auth 0x%x", r->ssid_len, r->ssid, r->channel, r->rssi, r->auth_mode);
    if (r->ssid_len == strlen(WIFI_SSID) && memcmp(r->ssid, WIFI_SSID, r->ssid_len) == 0) {
        scan_matches++;
        LOG("wifi scan: found \"%s\" channel %d, signal %d dBm, auth 0x%x", WIFI_SSID, r->channel, r->rssi,
            r->auth_mode);
    }
    return 0;
}

// Logs whether our network is visible on 2.4 GHz, and how strong/secured it is.
static void wifi_scan(void) {
    cyw43_wifi_scan_options_t opts = {0};
    scan_total = scan_matches = 0;
    if (cyw43_wifi_scan(&cyw43_state, &opts, NULL, on_scan_result)) {
        LOG("wifi scan: failed to start");
        return;
    }
    while (cyw43_wifi_scan_active(&cyw43_state)) {
        cyw43_arch_poll();
        sleep_ms(10);
    }
    LOG("wifi scan: %d results, %d for \"%s\"", scan_total, scan_matches, WIFI_SSID);
}

static bool wifi_connect(void) {
    static int attempt;
    // Alternate WPA2-only and WPA2/WPA3-mixed auth; routers differ in which they accept.
    uint32_t auth = attempt++ % 2 ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_WPA2_AES_PSK;
    if (attempt % 4 == 1) wifi_scan();
    LOG("wifi: connecting to \"%s\" (%s)", WIFI_SSID, auth == CYW43_AUTH_WPA2_AES_PSK ? "WPA2" : "WPA2/mixed");
    int err = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, auth, 20000);
    if (err) {
        // link status: -1 join failed, -2 network not found, -3 wrong password
        LOG("wifi: failed (%d), link status %d", err, cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA));
        return false;
    }
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);  // power saving causes audio dropouts
    LOG("wifi: connected, IP %s", ip4addr_ntoa(netif_ip4_addr(netif_default)));
    return true;
}

int main(void) {
    stdio_init_all();
    init_sine();
    LOG("airgroup-pico starting (step 1: cast test tone to \"%s\")", CAST_TARGET);

    if (cyw43_arch_init()) {
        LOG("cyw43 init failed");
        return 1;
    }
    cyw43_arch_enable_sta_mode();
    while (!wifi_connect()) sleep_ms(3000);

    stream_start(HTTP_PORT, tone_source);
    log_memory("ready");

    char url[96];
    snprintf(url, sizeof url, "http://%s:%d" STREAM_PATH, ip4addr_ntoa(netif_ip4_addr(netif_default)), HTTP_PORT);

    // The test runs when requested with GET /test, so re-flashing doesn't beep the house.
    bool test_done = true;
    int test_seconds = TEST_SECONDS;
    int attempts = 0;
    absolute_time_t next_attempt = get_absolute_time();
    absolute_time_t play_started = nil_time;
    absolute_time_t next_status = make_timeout_time_ms(5000);
    cast_state_t last_state = CAST_IDLE;

    for (;;) {
        cyw43_arch_poll();
        stream_poll();
        cast_poll();

        if (stream_test_requested) {
            stream_test_requested = false;
            test_done = false;
            test_seconds = stream_test_seconds ? stream_test_seconds : TEST_SECONDS;
            next_status = get_absolute_time();
            attempts = 0;
            play_started = nil_time;
            next_attempt = get_absolute_time();
        }

        cast_state_t st = cast_state();
        if (!test_done && (st == CAST_IDLE || st == CAST_FAILED) &&
            absolute_time_diff_us(get_absolute_time(), next_attempt) <= 0) {
            if (++attempts > 5) {
                LOG("giving up after 5 attempts");
                test_done = true;
                continue;
            }
            ip_addr_t ip;
            uint16_t port;
            LOG("mDNS: looking for \"%s\"", CAST_TARGET);
            if (cast_find(CAST_TARGET, &ip, &port, 6000)) {
                LOG("mDNS: found at %s:%u", ipaddr_ntoa(&ip), port);
                log_memory("before TLS");
                cast_start(&ip, port, url, "audio/wav");
            }
            next_attempt = make_timeout_time_ms(10000);
        }

        if (st != last_state) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, st == CAST_PLAYING);
            if (st == CAST_LAUNCHING) log_memory("after TLS");
            if (st == CAST_PLAYING && is_nil_time(play_started)) play_started = get_absolute_time();
            last_state = st;
        }
        if (!test_done && !is_nil_time(play_started) &&
            absolute_time_diff_us(play_started, get_absolute_time()) > test_seconds * 1000000LL) {
            LOG("test finished after %d seconds; stopping", test_seconds);
            cast_stop();
            test_done = true;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        }
        if (absolute_time_diff_us(get_absolute_time(), next_status) <= 0) {
            LOG("status: cast %s, listeners %d, max backlog %lu ms", cast_state_name(st), stream_listener_count(),
                (unsigned long)stream_take_max_backlog_ms());
            stream_log_tcp_stats(test_done ? 60000 : 5000);
            next_status = make_timeout_time_ms(test_done ? 60000 : 5000);
        }
    }
}
