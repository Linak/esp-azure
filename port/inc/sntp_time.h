// SNTP helpers for the ESP port of the Azure IoT SDK.
//
// SNTP is started from the STA_GOT_IP handler (sntp_network_up()), i.e. as
// soon as the stack has an address and a DNS server. This puts the first NTP
// request on the air before the BLE client starts scanning and connecting;
// a request lost to Wi-Fi/BLE coexistence costs lwIP's fixed 15 s receive
// timeout before the next server is tried. Later IP events restart polling,
// so a request fired on a half-configured stack is never left to lwIP's
// exponential retry back-off. platform_init() only waits for the clock.

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

/// Call when the network interface got an IP address. Starts SNTP on the
/// first call and restarts polling on every later one.
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
