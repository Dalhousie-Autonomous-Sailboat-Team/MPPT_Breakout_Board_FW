/** @file uart.c
 *
 * @brief This module contains code related to servicing the on-board debug
 * UART. This module will include a basic command line processor and a buffered
 * debug logging interface.
 *
 * @par
 * COPYRIGHT NOTICE: (c) 2024 DalMAST.  All rights reserved.
 */

// This module's header file:
#include "uart.h"

// STM32 HAL header files:
#include "usart.h"

// Standard C header files:
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

// Our header files:
#include "ring_buffer.h"
#include "assert.h"
#include "uart.h"
#include "blink.h"

//
// The general format for trace messages printed to the Debug UART:
//      [<HAL TICK> ms]<SPACE><HEADER><MESSAGE><TERMINATOR>
//

// Select Uart Channels
#define DEBUG_UART_HANDLE huart4
#define INPUT_1_UART_HANDLE huart1
#define INPUT_2_UART_HANDLE huart2
// Send to Debug (huart4) or Output UART (huart3)
#define OUTPUT_UART_HANDLE DEBUG_UART_HANDLE

#define ENTER_CHAR '\r'
#define SPACE_CHAR ' '

// The maximum length of the message
#define MESSAGE_MAX_LEN 245

//
// The headers prepended to every message before it's sent to the output UART.
// ATTENTION: These must all have the same fixed length!
//
#define MESSAGE_HEAD_BMS_1    "BMS_1:  "
#define MESSAGE_HEAD_BMS_2    "BMS_2:  "
#define MESSAGE_HEAD_MPPT_1   "MPPT_1: "
#define MESSAGE_HEAD_MPPT_2   "MPPT_2: "
#define MESSAGE_HEAD_DEBUG    "DEBUG:  "
#define MESSAGE_HEAD_UNKNOWN  "?????:  "

// The common length of all message headers:
#define MESSAGE_HEAD_LEN (sizeof(MESSAGE_HEAD_UNKNOWN) - 1)

// The length of the null terminator character:
#define NULL_TERMINATOR_LEN 1

// Define the buffer size required to store an entire message:
#define MESSAGE_BUFFER_LEN (MESSAGE_HEAD_LEN + MESSAGE_MAX_LEN + NULL_TERMINATOR_LEN)

// Define the number of message buffers we will make available:
// ATTENTION: This needs to be one less than a power of 2 due to the way the
// ringbuffers work.
#define MESSAGE_BUFFER_COUNT 31

// Define the size of the ringbuffer that will store pointers into the message
// buffer: ATTENTION: This needs to be a power of 2.
#define MESSAGE_BUFFER_PTR_COUNT (MESSAGE_BUFFER_COUNT + 1)

// This 2D buffer stores all trace messages waiting to be transmitted, pointers
// into this buffer should be managed by the free and busy FIFOs.
static char message_bufs[MESSAGE_BUFFER_COUNT][MESSAGE_BUFFER_LEN];

// A FIFO of buffers available to be populated:
static ring_buffer_ptr_t free_bufs;
static char             *free_bufs_mem[MESSAGE_BUFFER_PTR_COUNT];

// A FIFO of buffers waiting to be transmitted via the UART:
static ring_buffer_ptr_t busy_bufs;
static char             *busy_bufs_mem[MESSAGE_BUFFER_PTR_COUNT];

// Store the buffer we are currently transmitting via DMA here
static char *tx_buf = NULL;

// Message Head Lookup Table
static const char *message_head_lookup[] =
{
    MESSAGE_HEAD_BMS_1,
    MESSAGE_HEAD_BMS_2,
    MESSAGE_HEAD_MPPT_1,
    MESSAGE_HEAD_MPPT_2,
    MESSAGE_HEAD_DEBUG,
    MESSAGE_HEAD_UNKNOWN
};

/*!
 * @brief Initialize the UART module.
 */
void
uart_init (void)
{
    // Set up the two pointer FIFOs
    ring_buffer_ptr_setup(
        &free_bufs, (void *)&free_bufs_mem[0], MESSAGE_BUFFER_PTR_COUNT);
    ring_buffer_ptr_setup(
        &busy_bufs, (void *)&busy_bufs_mem[0], MESSAGE_BUFFER_PTR_COUNT);

    // Populate the free FIFO with all the available buffers
    for (uint32_t index = 0; index < MESSAGE_BUFFER_COUNT; index++)
    {
        // Get a pointer to the first character in each message buffer:
        void *const pointer = &message_bufs[index][0];

        // Push each message buffer in to the free FIFO:
        if (!ring_buffer_ptr_push(&free_bufs, pointer))
        {
            // Neither the free or busy FIFOs can ever be full, unless the
            //     programmer gave them the wrong size.
            ASSERT(false, "UART free buffers FIFO is full!");
            return;
        }
    }
}

/*!
 * @brief Uart Superloop: Check for pending UART RX and TX operations.
 */
void
uart_superloop (void)
{
    // First check if we are currently transmitting a buffer:
    if (tx_buf != NULL)
    {
        // Our DMA channel is busy, retry later.
        return;
    }

    // Try popping a trace message from the busy buffer:
    if (!ring_buffer_ptr_pop(&busy_bufs, (void **)&tx_buf))
    {
        // No buffers to print out, retry later.
        return;
    }

    // Transmit the buffer:
    const unsigned int tx_buf_len = strlen(tx_buf);

    if (HAL_UART_Transmit_DMA(&OUTPUT_UART_HANDLE, (uint8_t *)tx_buf, tx_buf_len) != HAL_OK)
    {
        // Failed to transmit, the message will get lost...
        ASSERT(false, "Failed to start UART DMA!");

        // Try putting back the buffer:
        if (!ring_buffer_ptr_push(&free_bufs, tx_buf))
        {
            // Neither the free or busy FIFOs can ever be full, unless the
            //     programmer gave them the wrong size.
            ASSERT(false, "UART free buffers FIFO is full!");
        }

        tx_buf = NULL;
    }
}

/*!
 * @brief Put a buffer of data on the output UART.
 */
void
uart_putbuf (message_origin_t origin, const uint8_t *buf, uint32_t buf_len)
{
    // Obtain a buffer to format the message into, give up if we don't
    // have available buffers:
    char *message_buf;

    if (!ring_buffer_ptr_pop(&free_bufs, (void **)&message_buf))
    {
        return;
    }

    // Truncate the message if needed:
    const int buf_len_trunc
        = (buf_len < MESSAGE_MAX_LEN) ? (int)buf_len : MESSAGE_MAX_LEN;

    // Get the message header:
    const char *message_head = message_head_lookup[origin];

    // Format the string to transmit over UART:
    const int tx_len = snprintf(message_buf,
                                MESSAGE_BUFFER_LEN,
                                "%.*s%.*s\r\n",
                                MESSAGE_HEAD_LEN,
                                message_head,
                                buf_len_trunc,
                                buf);

    // Check for errors from snprintf:
    if (tx_len < 0)
    {
        ASSERT(false, "Got error from snprintf!");
        return;
    }

    // Push the message in to the busy FIFO
    if (!ring_buffer_ptr_push(&busy_bufs, message_buf))
    {
        // Neither the free or busy FIFOs can ever be full, unless the
        //     programmer gave them the wrong size.
        ASSERT(false, "UART busy buffers FIFO is full!");
        return;
    }
}

/*!
 * @brief Put a string on the output UART.
 */
void
uart_putstring (message_origin_t origin, const char *str)
{
    const unsigned int len = strlen(str);
    uart_putbuf(origin, (uint8_t *)str, len);
}

/*!
 * @brief Print a formatted message to the output UART.
 */
void
uart_printf (message_origin_t origin, const char *format, ...)
{
    // ATTENTION: The formatting buffer for the message must have space for a
    // null terminator which will be placed by vsnprintf().
    static char message_format_buffer[MESSAGE_MAX_LEN + 1];
    va_list     arg_ptr;

    va_start(arg_ptr, format);
    const int message_len
        = vsnprintf(message_format_buffer, MESSAGE_MAX_LEN + 1, format, arg_ptr);
    va_end(arg_ptr);

    // Check for errors from snprintf:
    if (message_len < 0)
    {
        ASSERT(false, "Got an error from snprintf!");
        return;
    }

    uart_putbuf(origin, (uint8_t *)message_format_buffer, message_len);
}

/*!
 * @brief UART Transmit Complete Callback
 */
void
HAL_UART_TxCpltCallback (UART_HandleTypeDef *huart)
{
    if ((huart == &OUTPUT_UART_HANDLE) && (tx_buf != NULL))
    {
        // Done transferring the buffer, put it back
        if (!ring_buffer_ptr_push(&free_bufs, tx_buf))
        {
            // Neither the free or busy FIFOs can ever be full, unless the
            //     programmer gave them the wrong size.
            ASSERT(false, "UART free buffers FIFO is full!");
        }

        tx_buf = NULL;
    }
}

/*!
 * @brief UART Error Callback
 */
void
HAL_UART_ErrorCallback (UART_HandleTypeDef *huart)
{
    if ((huart == &OUTPUT_UART_HANDLE) && (tx_buf != NULL))
    {
        // Done transferring the buffer, put it back
        if (!ring_buffer_ptr_push(&free_bufs, tx_buf))
        {
            // Neither the free or busy FIFOs can ever be full, unless the
            //     programmer gave them the wrong size.
            ASSERT(false, "UART free buffers FIFO is full!");
        }

        tx_buf = NULL;
    }
}

/*!
 * @brief UART Receive Complete Callback
 */
void
HAL_UART_RxCpltCallback (UART_HandleTypeDef *UartHandle)
{
    (void) UartHandle;
}