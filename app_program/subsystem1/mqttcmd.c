/**
 * @file mqttcmd.c
 * @brief One-byte commands checked by pattern and code; no allocation.
 * NOTE: Early error returns preserve shallow validation (BARR-C 6.2.c).
 */
#include "mqttcmd.h"

#include <string.h>

#define MQTTCMD_CODE_MASK    (0x07U)

/** @brief Pattern in bits 3-7, code in bits 0-2. */
uint8_t mqttcmd_byte (mqttcmd_code_t code)
{
    uint32_t value = (uint32_t)code & MQTTCMD_CODE_MASK;

    if (MQTTCMD_NONE == code)
    {
        return 0U;
    }
    /* Five pattern bits above three code bits fill exactly one byte. */
    return (uint8_t)((MQTTCMD_PATTERN << MQTTCMD_CODE_BITS) | value);
}

/** @brief The validity pattern must match before the code is read. */
mqttcmd_code_t mqttcmd_code (uint8_t command)
{
    static const mqttcmd_code_t codes[] =
    {
        MQTTCMD_NONE, MQTTCMD_STOP, MQTTCMD_START, MQTTCMD_RESET,
        MQTTCMD_LEFT, MQTTCMD_RIGHT, MQTTCMD_STRAIGHT, MQTTCMD_UTURN
    };

    if (MQTTCMD_PATTERN != ((uint32_t)command >> MQTTCMD_CODE_BITS))
    {
        return MQTTCMD_NONE;
    }
    return codes[(uint32_t)command & MQTTCMD_CODE_MASK];
}

/** @brief Clear old state so rejected publications cannot reuse a byte. */
void mqttcmd_begin (mqttcmd_t * p_state, uint32_t length,
                    bool b_topic_matches)
{
    if (NULL == p_state)
    {
        return;
    }
    memset(p_state, 0, sizeof(*p_state));
    p_state->b_accepting = b_topic_matches && (MQTTCMD_LENGTH == length);
}

/** @brief Accept the single byte once, then validate it on the last part. */
bool mqttcmd_feed (mqttcmd_t * p_state, const uint8_t * p_data,
                   uint32_t length, bool b_last)
{
    if ((NULL == p_state) || (!p_state->b_accepting))
    {
        return false;
    }
    if (((NULL == p_data) && (length > 0U)) ||
        (length > (p_state->b_received ? 0U : MQTTCMD_LENGTH)))
    {
        p_state->b_accepting = false;
        return false;
    }
    if (length > 0U)
    {
        p_state->command = p_data[0];
        p_state->b_received = true;
    }
    if (!b_last)
    {
        return false;
    }
    p_state->b_accepting = false;
    return p_state->b_received &&
           (MQTTCMD_NONE != mqttcmd_code(p_state->command));
}

/* End of file. */

