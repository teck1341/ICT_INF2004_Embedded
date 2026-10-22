/**
 * @file net_mqtt_ipc.h
 * @brief Kernel/ISO-C boundary: fixed-width lengths avoid size_t conflicts.
 * Calls are nonblocking task-context APIs, never ISR APIs. Start once before
 * starting application or radio tasks. STOP is a separate priority channel.
 * Payloads are binary: telemetry frames of at most NET_MQTT_IPC_TX_MAX bytes
 * (telemetry.h) and one-byte commands (mqttcmd.h). This header cannot
 * include either (they use size_t), so net_mqtt.c checks the sizes and the
 * STOP byte against them at compile time.
 */
#ifndef NET_MQTT_IPC_H
#define NET_MQTT_IPC_H

#include <stdbool.h>
#include <stdint.h>

/** Largest frame is 17 bytes; 20 is the kernel's 4-byte message unit. */
#define NET_MQTT_IPC_TX_MAX 20U
#define NET_MQTT_IPC_RX_MAX 1U
#define NET_MQTT_IPC_STOP_BYTE 0xA9U

/** Create three bounded message buffers; clean up on partial failure. */
bool net_mqtt_ipc_start (void);

/** Queue 1..20 telemetry bytes. Return false on invalid input/full queue. */
bool net_mqtt_ipc_send (const uint8_t * p_frame, uint32_t length);

/** Receive one command byte, STOP first. True when a byte was written. */
bool net_mqtt_ipc_receive (uint8_t * p_command);

/** Radio-owner only: dequeue one frame; capacity must be >= 20 bytes. */
uint32_t net_mqtt_ipc_take_tx (uint8_t * p_frame, uint32_t capacity);

/** Radio-owner only: enqueue one validated command byte. */
bool net_mqtt_ipc_put_rx (uint8_t command);

#endif /* NET_MQTT_IPC_H */

/* End of file. */

