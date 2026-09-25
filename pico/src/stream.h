#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STREAM_SAMPLE_RATE 44100
#define STREAM_PATH "/stream.wav"

// Fills up to `frames` stereo frames (interleaved int16) and returns how many it produced.
// Anything it doesn't produce is sent as silence, so the live stream never stalls.
typedef size_t (*pcm_source_fn)(int16_t *dst, size_t frames);

// Starts the HTTP server: GET /stream.wav (live WAV), GET /log, GET /.
void stream_start(uint16_t port, pcm_source_fn source);
// Call as often as possible from the main loop: produces audio in real time and sends it.
void stream_poll(void);
int stream_listener_count(void);
// Largest amount of audio (ms) queued for a listener since the last call.
uint32_t stream_take_max_backlog_ms(void);
void stream_log_tcp_stats(uint32_t interval_ms);
// Set when someone requests GET /test or /test?s=<seconds> (starts the cast test); cleared by the caller.
extern volatile bool stream_test_requested;
// Optional test length from GET /test?s=N (0 = default).
extern volatile int stream_test_seconds;
