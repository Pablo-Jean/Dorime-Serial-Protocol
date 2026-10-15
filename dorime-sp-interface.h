/*
 * dorime-sp-interface.h
 *
 *  Created on: Mar 6, 2024
 *      Author: pablo-jean
 */

#ifndef DORIME_SP_INTERFACE_H_
#define DORIME_SP_INTERFACE_H_

/** Includes **/

#include <stdint.h>
#include <stddef.h>

/** Typedefs **/

typedef enum{
	DORIME_INTERFACE_DIRECTION_TX,
	DORIME_INTERFACE_DIRECTION_RX,
	DORIME_INTERFACE_DIRECTION_SLEEP
}dorime_interface_dir_e;

/** Function Typedefs **/

/*
 * Interface contract
 *
 * - tx / rx MUST be non-blocking. They only start the operation and return
 *   0 on success (any other value = failure).
 * - When the transmission of ALL bytes finishes, the port must call
 *   dorime_tx_event(). On RS485, call it from the "Transmission Complete"
 *   interrupt (TC), NOT from "TX register empty", otherwise the driver
 *   direction is released before the last byte leaves the line.
 * - When EXACTLY `len` bytes were received, the port must call
 *   dorime_rx_event(). Both events are ISR-safe (they only set a flag).
 * - abort (mandatory) must cancel any pending tx/rx and flush the RX FIFO.
 * - dir (optional) switches the transceiver (RS485 DE/RE, RF module TX/RX).
 * - getCnt must return a free-running 32-bit counter (wraps are handled).
 * - lock/unlock (optional) protect the data shared between the task that
 *   calls dorime_send_data() and the one that calls dorime_handler().
 */
typedef uint8_t  (*dorime_tx_f)(uint8_t *data, uint16_t len);
typedef uint8_t  (*dorime_rx_f)(uint8_t *data, uint16_t len);
typedef uint8_t  (*dorime_dir_f)(dorime_interface_dir_e dir);
typedef uint32_t (*dorime_getcnt_f)(void);
typedef void     (*dorime_abort_f)(void);
typedef void     (*dorime_lock_f)(void);
typedef void     (*dorime_unlock_f)(void);
typedef void*    (*dorime_alloc_f)(size_t size);
typedef void     (*dorime_free_f)(void *p);

/** Publics **/

#endif /* DORIME_SP_INTERFACE_H_ */
