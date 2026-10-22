/**
 * @file net_mqtt_config.example.h
 * @brief Copy to upstream config/net_mqtt_config.h and set local values.
 * This profile is MQTT 3.1.1 over TCP without TLS, for the classroom LAN.
 * Do not use production credentials. Use broker ACLs for this client's
 * exact command and telemetry topics. Command publications must be QoS0.
 */
#ifndef NET_MQTT_CONFIG_H
#define NET_MQTT_CONFIG_H

#define NET_MQTT_CONFIGURED 0
#if !NET_MQTT_CONFIGURED
#error "Set MQTT broker/topics, then set NET_MQTT_CONFIGURED to 1"
#endif

/* Replace this documentation address with your laboratory broker IPv4. */
#define NET_MQTT_BROKER_IPV4 "192.0.2.1"
#define NET_MQTT_PORT 1883U
#define NET_MQTT_CLIENT_ID "inf2004-team-replace-me"
/* No topic may contain '+' or '#'. The command topic is matched with
 * strcmp, so a wildcard filter compiles and subscribes but silently ignores
 * every command, remote STOP included. The telemetry topic is a PUBLISH
 * topic and the status topic is also the Last Will topic in CONNECT: a
 * wildcard in either makes the broker close the connection. Topics are at
 * most 96 characters.
 */
#define NET_MQTT_COMMAND_TOPIC "inf2004/team-replace-me/command"
#define NET_MQTT_TELEMETRY_TOPIC "inf2004/team-replace-me/telemetry"
#define NET_MQTT_STATUS_TOPIC "inf2004/team-replace-me/status"

/* Use string literals if the broker requires credentials; NULL disables.
 * A user name and a password, if set, must each be non-empty: the pinned
 * lwIP rejects "" for either, and the client then retries forever without
 * connecting. Never set a password without a user name (MQTT 3.1.1
 * [MQTT-3.1.2-22]). Each is at most 64 characters. These rules and the
 * topic rules above are not yet checked at compile time (task T-68).
 */
#define NET_MQTT_USERNAME NULL
#define NET_MQTT_PASSWORD NULL

#endif /* NET_MQTT_CONFIG_H */

/* End of file. */

