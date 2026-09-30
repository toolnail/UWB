/*! ----------------------------------------------------------------------------
 * @file reliable_p2p_protocol.h
 * @brief Shared wire format for the reliable point-to-point data example.
 *
 * The protocol uses IEEE 802.15.4 data frames and an application-level
 * 32-packet selective-repeat window.  Existing simple_tx/simple_rx examples
 * are intentionally not used or modified.
 */
#ifndef RELIABLE_P2P_PROTOCOL_H_
#define RELIABLE_P2P_PROTOCOL_H_

#include <stdint.h>
#include <string.h>

#define RELIABLE_P2P_VERSION             1U
#define RELIABLE_P2P_TARGET              600000UL
#define RELIABLE_P2P_WINDOW_SIZE         32UL
#define RELIABLE_P2P_TX_DELAY_MS         1U
#define RELIABLE_P2P_MAX_WINDOW_ROUNDS   5U
#define RELIABLE_P2P_CONTROL_RETRIES     10U
#define RELIABLE_P2P_SESSION_ID          0x26093001UL

/* A normal IEEE 802.15.4 frame (127-byte maximum) with a 32-byte payload. */
#define RELIABLE_P2P_FRAME_BYTES         59U
#define RELIABLE_P2P_PAYLOAD_BYTES       32U

#define RELIABLE_P2P_MAC_SEQ_IDX         2U
#define RELIABLE_P2P_MAGIC0_IDX          9U
#define RELIABLE_P2P_MAGIC1_IDX          10U
#define RELIABLE_P2P_VERSION_IDX         11U
#define RELIABLE_P2P_TYPE_IDX            12U
#define RELIABLE_P2P_SESSION_IDX         13U
#define RELIABLE_P2P_FIELD_IDX           17U
#define RELIABLE_P2P_ARG0_IDX            21U
#define RELIABLE_P2P_ARG1_IDX            25U
#define RELIABLE_P2P_ARG2_IDX            29U
#define RELIABLE_P2P_ARG3_IDX            33U
#define RELIABLE_P2P_ARG4_IDX            37U
#define RELIABLE_P2P_PAYLOAD_IDX         25U

#define RELIABLE_P2P_MAGIC0              ((uint8_t)'R')
#define RELIABLE_P2P_MAGIC1              ((uint8_t)'P')

#define RELIABLE_P2P_MSG_SESSION_START   0x01U
#define RELIABLE_P2P_MSG_SESSION_ACK     0x02U
#define RELIABLE_P2P_MSG_DATA            0x03U
#define RELIABLE_P2P_MSG_BLOCK_ACK_REQ   0x04U
#define RELIABLE_P2P_MSG_BLOCK_ACK       0x05U
#define RELIABLE_P2P_MSG_SESSION_END     0x06U
#define RELIABLE_P2P_MSG_SUMMARY         0x07U

static inline void reliable_p2p_put_u32(uint8_t *buffer, uint32_t index, uint32_t value)
{
    buffer[index + 0U] = (uint8_t)(value >> 0U);
    buffer[index + 1U] = (uint8_t)(value >> 8U);
    buffer[index + 2U] = (uint8_t)(value >> 16U);
    buffer[index + 3U] = (uint8_t)(value >> 24U);
}

static inline uint32_t reliable_p2p_get_u32(const uint8_t *buffer, uint32_t index)
{
    return ((uint32_t)buffer[index + 0U] << 0U) |
           ((uint32_t)buffer[index + 1U] << 8U) |
           ((uint32_t)buffer[index + 2U] << 16U) |
           ((uint32_t)buffer[index + 3U] << 24U);
}

static inline uint32_t reliable_p2p_valid_mask(uint32_t base)
{
    uint32_t remaining = RELIABLE_P2P_TARGET - base;
    if (remaining >= RELIABLE_P2P_WINDOW_SIZE)
    {
        return 0xFFFFFFFFUL;
    }
    return (1UL << remaining) - 1UL;
}

static inline uint32_t reliable_p2p_popcount(uint32_t value)
{
    uint32_t count = 0U;
    while (value != 0U)
    {
        value &= value - 1U;
        count++;
    }
    return count;
}

static inline void reliable_p2p_prepare_frame(uint8_t *frame,
                                               uint8_t mac_sequence,
                                               uint8_t destination_0,
                                               uint8_t destination_1,
                                               uint8_t source_0,
                                               uint8_t source_1,
                                               uint8_t message_type,
                                               uint32_t session_id,
                                               uint32_t field,
                                               uint32_t arg0,
                                               uint32_t arg1,
                                               uint32_t arg2,
                                               uint32_t arg3,
                                               uint32_t arg4)
{
    memset(frame, 0, RELIABLE_P2P_FRAME_BYTES);

    /* IEEE 802.15.4 data frame, PAN compression, 16-bit source/destination,
     * no immediate MAC ACK. Reliability is provided by block ACKs. */
    frame[0] = 0x41U;
    frame[1] = 0x88U;
    frame[RELIABLE_P2P_MAC_SEQ_IDX] = mac_sequence;
    frame[3] = 0xCAU;
    frame[4] = 0xDEU;
    frame[5] = destination_0;
    frame[6] = destination_1;
    frame[7] = source_0;
    frame[8] = source_1;

    frame[RELIABLE_P2P_MAGIC0_IDX] = RELIABLE_P2P_MAGIC0;
    frame[RELIABLE_P2P_MAGIC1_IDX] = RELIABLE_P2P_MAGIC1;
    frame[RELIABLE_P2P_VERSION_IDX] = RELIABLE_P2P_VERSION;
    frame[RELIABLE_P2P_TYPE_IDX] = message_type;
    reliable_p2p_put_u32(frame, RELIABLE_P2P_SESSION_IDX, session_id);
    reliable_p2p_put_u32(frame, RELIABLE_P2P_FIELD_IDX, field);
    reliable_p2p_put_u32(frame, RELIABLE_P2P_ARG0_IDX, arg0);
    reliable_p2p_put_u32(frame, RELIABLE_P2P_ARG1_IDX, arg1);
    reliable_p2p_put_u32(frame, RELIABLE_P2P_ARG2_IDX, arg2);
    reliable_p2p_put_u32(frame, RELIABLE_P2P_ARG3_IDX, arg3);
    reliable_p2p_put_u32(frame, RELIABLE_P2P_ARG4_IDX, arg4);
}

static inline int reliable_p2p_frame_is_valid(const uint8_t *frame, uint16_t frame_length)
{
    return (frame_length == RELIABLE_P2P_FRAME_BYTES) &&
           (frame[RELIABLE_P2P_MAGIC0_IDX] == RELIABLE_P2P_MAGIC0) &&
           (frame[RELIABLE_P2P_MAGIC1_IDX] == RELIABLE_P2P_MAGIC1) &&
           (frame[RELIABLE_P2P_VERSION_IDX] == RELIABLE_P2P_VERSION);
}

static inline void reliable_p2p_fill_payload(uint8_t *frame, uint32_t sequence)
{
    uint32_t i;
    for (i = 0U; i < RELIABLE_P2P_PAYLOAD_BYTES; i++)
    {
        frame[RELIABLE_P2P_PAYLOAD_IDX + i] =
            (uint8_t)(sequence + (i * 37U) + 0x5AU);
    }
}

static inline int reliable_p2p_payload_is_valid(const uint8_t *frame, uint32_t sequence)
{
    uint32_t i;
    for (i = 0U; i < RELIABLE_P2P_PAYLOAD_BYTES; i++)
    {
        if (frame[RELIABLE_P2P_PAYLOAD_IDX + i] !=
            (uint8_t)(sequence + (i * 37U) + 0x5AU))
        {
            return 0;
        }
    }
    return 1;
}

#endif /* RELIABLE_P2P_PROTOCOL_H_ */
