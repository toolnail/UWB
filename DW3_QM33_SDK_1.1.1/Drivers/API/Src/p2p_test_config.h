/* Shared configuration for the protocol-free TX/RX experiment and PC scripts. */
#ifndef P2P_TEST_CONFIG_H
#define P2P_TEST_CONFIG_H

/* Edit this decimal integer only; rebuild both firmwares after changing it. */
#ifdef TEST_PACKET_TARGET
#error Define TEST_PACKET_TARGET only in p2p_test_config.h
#endif
#define TEST_PACKET_TARGET 300000UL

#if (TEST_PACKET_TARGET < 1UL) || (TEST_PACKET_TARGET > 2147483647UL)
#error TEST_PACKET_TARGET must be between 1 and 2147483647
#endif

#endif
