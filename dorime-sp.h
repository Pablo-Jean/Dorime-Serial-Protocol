/*
 * dorime-sp.h
 *
 *  Created on: Mar 6, 2024
 *      Author: pablo-jean
 */

#ifndef DORIME_SP_H_
#define DORIME_SP_H_

/**
 * includes
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <useful/crc32.h>

#include "dorime-sp-interface.h"

/*
 * Data Packet Structure (all multi-byte fields are LITTLE-ENDIAN on the wire)
 *
 * | Header | Version | Pck ID | ADDR | Len | Data  | CRC | Tail |
 *    1B        2B        4B      2B    2B     Len    4B     1B
 *
 *    Header: The first byte, indicates the start of a transmission
 *    		and identifies the packet type
 *    Version: Protocol version (MAJOR << 8 | MINOR). A receiver rejects
 *    		packets with a different MAJOR.
 *    Pck ID: Incremented after every successfully acknowledged transmission,
 *    		so a user retry of a failed send reuses the same ID. The ACK
 *    		echoes the ID of the packet it acknowledges.
 *    Addr: The address of the receiver (0x0000 for Controller, 0xFFFF for
 *    		a broadcasting)
 *    Len: Length, in bytes, of the Data. In Start packets, it announces the
 *    		length of the Data packet. In Ack packets, this field carries the
 *    		ACK code (DORIME_PACKET_ACK / DORIME_PACKET_NOT_ACK) and no
 *    		Data is present.
 *    Data: The useful information
 *    CRC: CRC-32 calculation to check if data is not corrupted.
 *    		The calculation starts in Header, ending on the Data
 *    Tail: Signals the end of the packet. Works with CRC to check packet
 *    		integrity
 *
 * Transaction (unicast):
 *
 *   Sender                         Receiver
 *     | --- START (len) ------------> |
 *     | <------------------- ACK ---- |
 *     | --- DATA -------------------> |
 *     | <------------------- ACK ---- |   (ACK or NOT_ACK, after CRC check)
 *
 * Transaction (broadcast): START, guard time, DATA. Nobody answers, to avoid
 * collisions on the shared medium.
 *
 * Since START/ACK have no Data, their size is DORIME_PCK_OVERHEAD (16 bytes).
 * A DATA packet has DORIME_PCK_OVERHEAD + Len bytes.
 */

/**
 * Macros
 */
#define DORIME_VERSION_MAJOR		1
#define DORIME_VERSION_MINOR		0
#define DORIME_PACKET_VERSION		((DORIME_VERSION_MAJOR << 8) | (DORIME_VERSION_MINOR))

#define DORIME_INITIALIZED_FLAG		0xD0

#define DORIME_HEADER_START			0xA0
#define DORIME_HEADER_ACK			0xA2
#define DORIME_HEADER_DATA			0xA4
#define DORIME_HEADER_RESP			0xA6	// reserved
#define DORIME_TAIL					0xED

#define DORIME_PACKET_ACK			0x0000
#define DORIME_PACKET_NOT_ACK		0x0101

#define DORIME_ADDR_CONTROLLER		0x0000
#define DORIME_ADDR_BROADCAST		0xFFFF

#define DORIME_ALIVE_CTR_2_TGT		"DORIME"
#define DORIME_ALIVE_TGT_2_CTR		"AMENO"

// Wire layout
#define DORIME_PCK_OFF_HEADER		0
#define DORIME_PCK_OFF_VERSION		1
#define DORIME_PCK_OFF_ID			3
#define DORIME_PCK_OFF_ADDR			7
#define DORIME_PCK_OFF_LEN			9
#define DORIME_PCK_OFF_DATA			11
#define DORIME_PCK_CRC_SIZE			4
#define DORIME_PCK_TAIL_SIZE		1
#define DORIME_PCK_OVERHEAD			(DORIME_PCK_OFF_DATA + DORIME_PCK_CRC_SIZE + DORIME_PCK_TAIL_SIZE)

// Biggest value accepted by u16MaxDataLen (tx/rx lengths are 16 bits)
#define DORIME_MAX_DATA_LEN_LIMIT	(0xFFFF - DORIME_PCK_OVERHEAD)

#define DORIME_FXN_SET_TX(handle, pTx)			(handle)->fxns._tx = (pTx)
#define DORIME_FXN_SET_RX(handle, pRx)			(handle)->fxns._rx = (pRx)
#define DORIME_FXN_SET_ABORT(handle, pAbort)	(handle)->fxns._abort = (pAbort)
#define DORIME_FXN_SET_DIR(handle, pDir)		(handle)->fxns._dir = (pDir)
#define DORIME_FXN_SET_LOCK(handle, pLock)		(handle)->fxns._lock = (pLock)
#define DORIME_FXN_SET_UNLOCK(handle, pUnlock)	(handle)->fxns._unlock = (pUnlock)
#define DORIME_FXN_SET_TCKCNT(handle, pCnt)		(handle)->fxns._tickCont = (pCnt)
#define DORIME_FXN_SET_MALLOC(handle, pMalloc)	(handle)->fxns._memAlloc = (pMalloc)
#define DORIME_FXN_SET_FREE(handle, pFree)		(handle)->fxns._memFree = (pFree)

// Weak attribute (GCC/Clang/ARMCC/IAR)
#if defined(__ICCARM__)
#define DORIME_WEAK		__weak
#elif defined(__GNUC__) || defined(__clang__) || defined(__ARMCC_VERSION)
#define DORIME_WEAK		__attribute__((weak))
#else
#define DORIME_WEAK
#endif

/**
 * Enumerates
 */

typedef enum{
	DORIME_EVT_ADDR_MATCH,		// params: dorime_evparams_addr_match_t*
	DORIME_EVT_DATA_REC,		// params: dorime_evparams_data_t*
	DORIME_EVT_RESP_REC,		// reserved
	DORIME_EVT_REC_FAILED,		// a reception was discarded (bad CRC, tail...)
	DORIME_EVT_ACK_OK,			// dorime_send_data() finished successfully
	DORIME_EVT_SEND_FAILED,		// dorime_send_data() failed (NOT_ACK, tx error)
	DORIME_EVT_TIMEOUT			// a transaction timed out (see u32Timeout)
}dorime_event_e;

typedef enum{
	DORIME_TYPE_CONTROLLER,
	DORIME_TYPE_TARGET
}dorime_dev_type_e;

typedef enum{
	DORIME_OK,
	DORIME_FAILED,
	DORIME_NOT_ALLOWED,
	DORIME_NOT_INITIALIZED,
	DORIME_INVALID_PARAMETERS,
	DORIME_INVALID_HANDLE,

	DORIME_UNKNOWN = 0xFF
}dorime_err_e;

typedef enum{
	DOR_STATE_IDLE,

	// Receiving
	DOR_STATE_ADDR_CHECK,
	DOR_STATE_SEND_ACK,
	DOR_STATE_WAITING_ACK_SENDED,
	DOR_STATE_WAITING_PACKET,
	DOR_STATE_RECEIVED_PACKET,
	DOR_STATE_PACKET_FAILED,
	DOR_STATE_SEND_FINAL_ACK,
	DOR_STATE_WAITING_FINAL_ACK_SENDED,

	// Sending
	DOR_STATE_START_SEND,
	DOR_STATE_WAITING_START_SENDED,
	DOR_STATE_START_WAITING_ACK,
	DOR_STATE_START_REC_ACK,
	DOR_STATE_SEND_DATA,
	DOR_STATE_WAITING_DATA_SENDED,
	DOR_STATE_SEND_WAITING_ACK,
	DOR_STATE_SEND_REC_ACK,

	// Common
	DOR_STATE_ABORT,
	DOR_STATE_COOLDOWN,
	DOR_STATE_TIMEOUT
}dorime_state_e;

/**
 * Typedefs and Structs
 */

typedef struct{
	uint16_t len;
	uint8_t *data;
}dorime_evparams_data_t;

typedef struct{
	uint16_t resp;
}dorime_evparams_resp_rec_t;

typedef struct{
	uint16_t addr;
}dorime_evparams_addr_match_t;

typedef struct{
	/*
	 * Configuration: fill BEFORE calling dorime_init()
	 */
	uint16_t u16Address;			// 0x0000 = Controller, others = Target (not 0xFFFF)
	uint32_t u32MsPerCount;			// milliseconds per count of fxns._tickCont
	uint32_t u32Timeout;			// ms, timeout for each step of a transaction
	uint32_t u32ByteTimeout;		// ms per payload byte, added to the Data steps (0 = none)
	uint32_t u32CooldownTimeout;	// ms the line is left alone after a failure (0 = u32Timeout)
	uint32_t u32BroadcastGuard;		// ms between START and DATA on broadcasts
	uint16_t u16MaxDataLen;			// biggest payload accepted/sent (<= DORIME_MAX_DATA_LEN_LIMIT)

	// fxns
	struct {
		dorime_tx_f _tx;			// mandatory
		dorime_rx_f _rx;			// mandatory
		dorime_dir_f _dir;
		dorime_getcnt_f _tickCont;	// mandatory
		dorime_abort_f _abort;		// mandatory
		dorime_lock_f _lock;
		dorime_unlock_f _unlock;
		dorime_alloc_f _memAlloc;	// both or none (default: malloc/free)
		dorime_free_f _memFree;
	}fxns;

	// interns: do not touch
	struct {
		uint64_t u64Tick;			// ms
		uint64_t u64Deadline;		// ms
		uint32_t u32LastCnt;

		uint32_t u32ID;				// ID of the next/current transmission
		uint32_t u32RxID;			// ID of the packet being received
		uint16_t u16DestAddr;
		uint16_t u16DataLen;		// length of the data being sent/received
		uint16_t u16AckCode;
		const uint8_t *pu8TxData;

		dorime_dev_type_e enType;
		dorime_state_e enState;
		dorime_interface_dir_e enInterfaceDir;
		uint8_t u8InitFlag;

		bool TxIsWaiting;
		bool RxIsWaiting;
		bool isBroadcast;
		bool bTxTransaction;		// current transaction is a send
		bool bDeliver;				// deliver received data after final ACK

		// written by ISRs / other tasks, consumed by dorime_handler()
		volatile bool bTxEvent;
		volatile bool bRxEvent;
		volatile bool bAbortReq;
		volatile bool bSendPending;
		volatile bool bTxBusy;
	}_internal;

	// packets
	struct{
		uint8_t Header[DORIME_PCK_OVERHEAD];	// START and ACK packets
		uint8_t *Buffer;						// DATA packet (dynamic)
		uint32_t BufferLen;
	}packet;
}dorime_t;

/**
 * Publics
 */

dorime_err_e dorime_init(dorime_t *control);

// Call periodically (main loop or task). All state changes happen here.
dorime_err_e dorime_handler(dorime_t *control);

// Starts sending `len` bytes to `destAddr`. `data` MUST stay valid until
// DORIME_EVT_ACK_OK, DORIME_EVT_SEND_FAILED or DORIME_EVT_TIMEOUT.
// Returns DORIME_NOT_ALLOWED while another send is in progress.
dorime_err_e dorime_send_data(dorime_t *control, uint16_t destAddr, const uint8_t *data, uint16_t len);

dorime_err_e dorime_abort(dorime_t *control);

// Port events (ISR-safe): see dorime-sp-interface.h
dorime_err_e dorime_rx_event(dorime_t *control);

dorime_err_e dorime_tx_event(dorime_t *control);

// Weak routines
// User can replace this functions to build a routine
// with a better performance
uint32_t dorime_crc32(uint8_t *arr, uint32_t len);

// callbacks (weak default does nothing). Called from dorime_handler().
// For DORIME_EVT_DATA_REC the data is only valid during the callback:
// the user MUST copy it.
void dorime_cb_event(dorime_t *control, dorime_event_e event, void* params);

#endif /* DORIME_SP_H_ */
