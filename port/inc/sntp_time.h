// SNTP helpers for the ESP port of the Azure IoT SDK.
//
// SNTP is started once by platform_init() and restarted every time the
// station gets an IP address, so a request fired on a half-configured stack
// is never left to lwIP's exponential retry back-off.

#ifndef SNTP_TIME_H
#define SNTP_TIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Configure the NTP server list and start SNTP. Safe to call more than once:
/// a second call restarts an already running client.
void initialize_sntp(void);

/// Restart SNTP polling, e.g. when the network interface got an IP address.
/// No-op when SNTP has not been initialised yet.
void sntp_network_up(void);

/// True once the system clock holds a plausible wall-clock time.
bool sntp_time_is_valid(void);

/// Block until the clock is valid or timeout_ms has elapsed.
/// Returns true when the time is valid.
bool sntp_wait_for_time(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // SNTP_TIME_H
