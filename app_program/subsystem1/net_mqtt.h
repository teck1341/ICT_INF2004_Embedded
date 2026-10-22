/**
 * @file net_mqtt.h
 * @brief ISO-C task API and radio-owner MQTT service.
 * Never include with tk/tkernel.h (the upstream size_t differs). Kernel-only
 * translation units use the equivalent net_mqtt_ipc.h fixed-width API.
 * Telemetry frames (telemetry.h) and commands (mqttcmd.h) are binary.
 */
#ifndef NET_MQTT_H
#define NET_MQTT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct netif;

/** Create bounded queues once, before creating radio/application tasks. */
bool net_mqtt_start (void);

/** Enqueue one 1..20-byte frame, nonblocking; false: invalid input/full. */
bool net_mqtt_send (const uint8_t * p_frame, size_t length);

/** Dequeue one command byte, STOP first; false when none is pending. */
bool net_mqtt_receive (uint8_t * p_command);

/**
 * True while the radio owner holds a subscribed MQTT session, so a remote
 * STOP can arrive. The radio task is the only writer of this one-byte flag
 * and a byte load is a single access, so other tasks read it without a lock.
 * The offline profile has no session and always returns false.
 */
bool net_mqtt_online (void);

/**
 * Command-topic publications rejected as malformed (wrong length, missing
 * validity pattern, a legacy text command), counted modulo 256. The radio
 * task is the only writer of this byte; readers difference two reads.
 * The offline profile always returns 0.
 */
uint8_t net_mqtt_rejected (void);

/**
 * Radio-owner only, after lwip_init and cyw43 initialization. Call every
 * 2..5 ms in the same context as sys_check_timeouts, after it returns: never
 * from an lwIP callback or sys_timeout handler, because connection closes
 * deferred from lwIP callbacks run here. Never from an ISR.
 * Handles Wi-Fi association, MQTT connection, subscription and QoS0 publish.
 */
void net_mqtt_poll (struct netif * p_interface);

#endif /* NET_MQTT_H */

/* End of file. */

