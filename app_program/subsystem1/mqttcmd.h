/**
 * @file mqttcmd.h
 * @brief One-byte binary MQTT commands, independent of lwIP.
 * @details A command is exactly one payload byte: bits 0-2 hold the command
 * code 1..7 and bits 3-7 the fixed validity pattern 10101b, so a stray,
 * empty, longer or zero byte is never a command. Seven codes need three
 * bits; the pattern makes a random byte valid with probability 7/256.
 * The radio routes MQTTCMD_STOP_BYTE to its own priority channel.
 */
#ifndef MQTTCMD_H
#define MQTTCMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MQTTCMD_LENGTH       (1U)
#define MQTTCMD_CODE_BITS    (3U)
#define MQTTCMD_PATTERN      (0x15U)
#define MQTTCMD_STOP_BYTE    (0xA9U)

typedef enum
{
    MQTTCMD_NONE = 0,
    MQTTCMD_STOP,
    MQTTCMD_START,
    MQTTCMD_RESET,
    MQTTCMD_LEFT,
    MQTTCMD_RIGHT,
    MQTTCMD_STRAIGHT,
    MQTTCMD_UTURN
} mqttcmd_code_t;

/** One publication being assembled: the byte and two state bits. */
typedef struct
{
    uint8_t command;
    bool    b_received : 1;
    bool    b_accepting : 1;
} mqttcmd_t;

/** @brief Command byte for a code; MQTTCMD_NONE gives 0, never valid. */
uint8_t mqttcmd_byte (mqttcmd_code_t code);

/** @brief Decode a byte; MQTTCMD_NONE for any byte without the pattern. */
mqttcmd_code_t mqttcmd_code (uint8_t command);

/** @brief Reset assembly; accept only a matching topic and 1 byte. */
void mqttcmd_begin (mqttcmd_t * p_state, uint32_t length,
                    bool b_topic_matches);

/**
 * Append a fragment. Return true only for one exact complete valid command.
 * Invalid lengths/pointers permanently reject the current message. A final
 * empty fragment is allowed only after the byte was delivered. On success
 * p_state->command holds the byte; a later feed cannot repeat the command.
 */
bool mqttcmd_feed (mqttcmd_t * p_state, const uint8_t * p_data,
                   uint32_t length, bool b_last);

#endif /* MQTTCMD_H */

/* End of file. */

