/** @file uart.h
 *
 * @brief UART handler for forwarding uart messages from channels 1 and 2 onto an output channel.
 *  The output channel can be set to either channel 3 (output) or channel 4 (USB Debug).
 *
 * @par
 * COPYRIGHT NOTICE: (c) 2024 DalMAST.  All rights reserved.
 */

#ifndef UART_H
#define UART_H

typedef enum message_origin_t
{
    MESSAGE_ORIGIN_BMS_1,
    MESSAGE_ORIGIN_BMS_2,
    MESSAGE_ORIGIN_MPPT_1,
    MESSAGE_ORIGIN_MPPT_2,
    MESSAGE_ORIGIN_DEBUG,
    MESSAGE_ORIGIN_UNKNOWN
} message_origin_t;

void uart_init(void);
void uart_superloop(void);

void uart_putstring(message_origin_t origin, const char *str);
void uart_printf(message_origin_t origin, const char *format, ...);

#define DEBUG_PRINTF(...) uart_printf(MESSAGE_ORIGIN_DEBUG, __VA_ARGS__)
#define DEBUG_PUTSTRING(str) uart_putstring(MESSAGE_ORIGIN_DEBUG, str)

#endif /* UART_H */

/*** end of file ***/