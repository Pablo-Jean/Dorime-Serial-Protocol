/*
 * dorime-sp.c
 *
 *  Created on: Mar 6, 2024
 *      Author: pablo-jean
 */


#include "dorime-sp.h"

/**
 * Macros
 */

#define DORIME_SET_STATE(c, s)		((c)->_internal.enState = (s))

#define PRV_ACK_VALID		0
#define PRV_ACK_REFUSED		1
#define PRV_ACK_INVALID		2

/*
 * Wire helpers (little-endian, no unaligned access, no packed structs)
 */

static void prv_put16(uint8_t *p, uint16_t v){
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
}

static void prv_put32(uint8_t *p, uint32_t v){
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint16_t prv_get16(const uint8_t *p){
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t prv_get32(const uint8_t *p){
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
			((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Header fields into `b` (first DORIME_PCK_OFF_DATA bytes)
static void prv_put_head(uint8_t *b, uint8_t header, uint32_t id, uint16_t addr, uint16_t len){
	b[DORIME_PCK_OFF_HEADER] = header;
	prv_put16(&b[DORIME_PCK_OFF_VERSION], DORIME_PACKET_VERSION);
	prv_put32(&b[DORIME_PCK_OFF_ID], id);
	prv_put16(&b[DORIME_PCK_OFF_ADDR], addr);
	prv_put16(&b[DORIME_PCK_OFF_LEN], len);
}

// CRC + Tail. `len` is the number of data bytes between head and CRC
static void prv_finish_frame(uint8_t *b, uint16_t len){
	uint32_t crcPos = DORIME_PCK_OFF_DATA + len;

	prv_put32(&b[crcPos], dorime_crc32(b, crcPos));
	b[crcPos + DORIME_PCK_CRC_SIZE] = DORIME_TAIL;
}

static void prv_build_short(uint8_t *b, uint8_t header, uint32_t id, uint16_t addr, uint16_t field){
	prv_put_head(b, header, id, addr, field);
	prv_finish_frame(b, 0);
}

static bool prv_frame_valid(const uint8_t *b, uint16_t len, uint8_t header){
	uint32_t crcPos = DORIME_PCK_OFF_DATA + len;

	if (b[DORIME_PCK_OFF_HEADER] != header)
		return false;
	if (b[crcPos + DORIME_PCK_CRC_SIZE] != DORIME_TAIL)
		return false;
	if ((prv_get16(&b[DORIME_PCK_OFF_VERSION]) >> 8) != DORIME_VERSION_MAJOR)
		return false;
	return (dorime_crc32((uint8_t*)b, crcPos) == prv_get32(&b[crcPos]));
}

/*
 * Privates
 */

static void prv_lock(dorime_t *c){
	if (c->fxns._lock != NULL){
		c->fxns._lock();
	}
}

static void prv_unlock(dorime_t *c){
	if (c->fxns._unlock != NULL){
		c->fxns._unlock();
	}
}

static dorime_err_e prv_dir(dorime_t *c, dorime_interface_dir_e dir){
	if (c->fxns._dir != NULL){
		if (c->fxns._dir(dir) != 0)
			return DORIME_FAILED;
	}
	c->_internal.enInterfaceDir = dir;
	return DORIME_OK;
}

static void prv_update_tick(dorime_t *c){
	uint32_t now = c->fxns._tickCont();
	uint32_t delta = now - c->_internal.u32LastCnt;	// wrap-safe

	c->_internal.u32LastCnt = now;
	c->_internal.u64Tick += (uint64_t)delta * c->u32MsPerCount;
}

static void prv_set_deadline(dorime_t *c, uint32_t ms){
	c->_internal.u64Deadline = c->_internal.u64Tick + ms;
}

static bool prv_expired(dorime_t *c){
	return (c->_internal.u64Tick >= c->_internal.u64Deadline);
}

static uint32_t prv_data_timeout(dorime_t *c){
	return c->u32Timeout + ((uint32_t)c->_internal.u16DataLen * c->u32ByteTimeout);
}

/*
 * The flags are cleared and the "waiting" marker set BEFORE the port is
 * called, so an event fired from an ISR before the call returns is not lost.
 */
static dorime_err_e prv_start_tx(dorime_t *c, uint8_t *data, uint16_t len){
	if (prv_dir(c, DORIME_INTERFACE_DIRECTION_TX) != DORIME_OK)
		return DORIME_FAILED;
	c->_internal.bTxEvent = false;
	c->_internal.TxIsWaiting = true;
	if (c->fxns._tx(data, len) != 0){
		c->_internal.TxIsWaiting = false;
		return DORIME_FAILED;
	}
	return DORIME_OK;
}

static dorime_err_e prv_start_rx(dorime_t *c, uint8_t *data, uint16_t len){
	if (prv_dir(c, DORIME_INTERFACE_DIRECTION_RX) != DORIME_OK)
		return DORIME_FAILED;
	c->_internal.bRxEvent = false;
	c->_internal.RxIsWaiting = true;
	if (c->fxns._rx(data, len) != 0){
		c->_internal.RxIsWaiting = false;
		return DORIME_FAILED;
	}
	return DORIME_OK;
}

static void prv_io_abort(dorime_t *c){
	c->fxns._abort();
	c->_internal.TxIsWaiting = false;
	c->_internal.RxIsWaiting = false;
	c->_internal.bTxEvent = false;
	c->_internal.bRxEvent = false;
}

// Cancels pending I/O (releasing the line) and frees the data buffer
static void prv_release(dorime_t *c){
	bool wasTx = c->_internal.TxIsWaiting;

	if (wasTx || c->_internal.RxIsWaiting){
		prv_io_abort(c);
		if (wasTx){
			prv_dir(c, DORIME_INTERFACE_DIRECTION_RX);
		}
	}
	if (c->packet.Buffer != NULL){
		c->fxns._memFree(c->packet.Buffer);
		c->packet.Buffer = NULL;
	}
}

static void prv_enter_cooldown(dorime_t *c){
	prv_release(c);
	prv_set_deadline(c, c->u32CooldownTimeout);
	DORIME_SET_STATE(c, DOR_STATE_COOLDOWN);
}

static void prv_tx_failed(dorime_t *c){
	dorime_cb_event(c, DORIME_EVT_SEND_FAILED, NULL);
	DORIME_SET_STATE(c, DOR_STATE_ABORT);
}

static void prv_rx_failed(dorime_t *c){
	dorime_cb_event(c, DORIME_EVT_REC_FAILED, NULL);
	prv_enter_cooldown(c);
}

// The packet does not carry the source address, so:
// Target -> Controller. Controller -> whoever is waiting (broadcast)
static uint16_t prv_ack_addr(dorime_t *c){
	if (c->_internal.enType == DORIME_TYPE_TARGET)
		return DORIME_ADDR_CONTROLLER;
	return DORIME_ADDR_BROADCAST;
}

static int prv_check_ack(dorime_t *c){
	const uint8_t *b = c->packet.Header;
	uint16_t addr;

	if (!prv_frame_valid(b, 0, DORIME_HEADER_ACK))
		return PRV_ACK_INVALID;
	if (prv_get32(&b[DORIME_PCK_OFF_ID]) != c->_internal.u32ID)
		return PRV_ACK_INVALID;
	addr = prv_get16(&b[DORIME_PCK_OFF_ADDR]);
	if (addr != c->u16Address && addr != DORIME_ADDR_BROADCAST)
		return PRV_ACK_INVALID;
	if (prv_get16(&b[DORIME_PCK_OFF_LEN]) != DORIME_PACKET_ACK)
		return PRV_ACK_REFUSED;
	return PRV_ACK_VALID;
}

// Receiver: arms the reception of the DATA packet announced by START
static void prv_begin_data_rx(dorime_t *c){
	prv_set_deadline(c, prv_data_timeout(c));
	DORIME_SET_STATE(c, DOR_STATE_WAITING_PACKET);
	if (prv_start_rx(c, c->packet.Buffer, (uint16_t)c->packet.BufferLen) != DORIME_OK){
		prv_rx_failed(c);
	}
}

/** Dorime States: receiving **/

static void prv_state_idle(dorime_t *c){
	bool pending;

	if (c->_internal.bRxEvent){
		// A START packet arrived
		c->_internal.bRxEvent = false;
		c->_internal.RxIsWaiting = false;
		DORIME_SET_STATE(c, DOR_STATE_ADDR_CHECK);
		return;
	}

	prv_lock(c);
	pending = c->_internal.bSendPending;
	prv_unlock(c);
	if (pending){
		if (c->_internal.RxIsWaiting){
			prv_io_abort(c);
		}
		DORIME_SET_STATE(c, DOR_STATE_START_SEND);
		return;
	}

	if (c->_internal.RxIsWaiting == false){
		// on failure, it is retried in the next call
		prv_start_rx(c, c->packet.Header, DORIME_PCK_OVERHEAD);
	}
}

static void prv_state_addr_check(dorime_t *c){
	const uint8_t *b = c->packet.Header;
	uint16_t addr, len;
	bool isBroadcast, mine;

	if (!prv_frame_valid(b, 0, DORIME_HEADER_START)){
		prv_rx_failed(c);
		return;
	}

	addr = prv_get16(&b[DORIME_PCK_OFF_ADDR]);
	len = prv_get16(&b[DORIME_PCK_OFF_LEN]);
	isBroadcast = (addr == DORIME_ADDR_BROADCAST);
	// Only Targets listen to broadcasts: a Target never broadcasts
	mine = (addr == c->u16Address) ||
			(isBroadcast && c->_internal.enType == DORIME_TYPE_TARGET);

	if (!mine){
		// Other devices are talking: keep quiet until they finish
		prv_enter_cooldown(c);
		return;
	}
	if (len == 0 || len > c->u16MaxDataLen){
		prv_rx_failed(c);
		return;
	}

	c->_internal.u32RxID = prv_get32(&b[DORIME_PCK_OFF_ID]);
	c->_internal.u16DataLen = len;
	c->_internal.isBroadcast = isBroadcast;
	c->_internal.bTxTransaction = false;
	c->_internal.bDeliver = false;

	c->packet.BufferLen = DORIME_PCK_OVERHEAD + len;
	c->packet.Buffer = (uint8_t*)c->fxns._memAlloc(c->packet.BufferLen);
	if (c->packet.Buffer == NULL){
		prv_rx_failed(c);
		return;
	}

	{
		dorime_evparams_addr_match_t params;

		params.addr = addr;
		dorime_cb_event(c, DORIME_EVT_ADDR_MATCH, &params);
	}

	if (isBroadcast){
		// Nobody answers a broadcast
		prv_begin_data_rx(c);
	}
	else{
		DORIME_SET_STATE(c, DOR_STATE_SEND_ACK);
	}
}

static void prv_state_send_ack(dorime_t *c){
	prv_build_short(c->packet.Header, DORIME_HEADER_ACK, c->_internal.u32RxID,
			prv_ack_addr(c), DORIME_PACKET_ACK);
	prv_set_deadline(c, c->u32Timeout);
	DORIME_SET_STATE(c, DOR_STATE_WAITING_ACK_SENDED);
	if (prv_start_tx(c, c->packet.Header, DORIME_PCK_OVERHEAD) != DORIME_OK){
		prv_rx_failed(c);
	}
}

static void prv_state_waiting_ack_sended(dorime_t *c){
	if (c->_internal.bTxEvent){
		c->_internal.bTxEvent = false;
		c->_internal.TxIsWaiting = false;
		prv_begin_data_rx(c);
	}
	else if (prv_expired(c)){
		DORIME_SET_STATE(c, DOR_STATE_TIMEOUT);
	}
}

static void prv_state_waiting_packet(dorime_t *c){
	if (c->_internal.bRxEvent){
		c->_internal.bRxEvent = false;
		c->_internal.RxIsWaiting = false;
		DORIME_SET_STATE(c, DOR_STATE_RECEIVED_PACKET);
	}
	else if (prv_expired(c)){
		DORIME_SET_STATE(c, DOR_STATE_TIMEOUT);
	}
}

static void prv_state_received_packet(dorime_t *c){
	const uint8_t *b = c->packet.Buffer;
	uint16_t len = c->_internal.u16DataLen;
	bool ok;

	ok = prv_frame_valid(b, len, DORIME_HEADER_DATA) &&
			(prv_get16(&b[DORIME_PCK_OFF_LEN]) == len) &&
			(prv_get32(&b[DORIME_PCK_OFF_ID]) == c->_internal.u32RxID);

	if (!ok){
		DORIME_SET_STATE(c, DOR_STATE_PACKET_FAILED);
		return;
	}

	if (c->_internal.isBroadcast){
		// No ACK: deliver right now
		dorime_evparams_data_t params;

		params.data = &c->packet.Buffer[DORIME_PCK_OFF_DATA];
		params.len = len;
		dorime_cb_event(c, DORIME_EVT_DATA_REC, &params);
		DORIME_SET_STATE(c, DOR_STATE_ABORT);
	}
	else{
		// The data is delivered only after the ACK left the device
		c->_internal.u16AckCode = DORIME_PACKET_ACK;
		c->_internal.bDeliver = true;
		DORIME_SET_STATE(c, DOR_STATE_SEND_FINAL_ACK);
	}
}

static void prv_state_packet_failed(dorime_t *c){
	dorime_cb_event(c, DORIME_EVT_REC_FAILED, NULL);
	if (c->_internal.isBroadcast){
		prv_enter_cooldown(c);
	}
	else{
		// Let the sender know right away, so it does not wait for a timeout
		c->_internal.u16AckCode = DORIME_PACKET_NOT_ACK;
		c->_internal.bDeliver = false;
		DORIME_SET_STATE(c, DOR_STATE_SEND_FINAL_ACK);
	}
}

static void prv_state_send_final_ack(dorime_t *c){
	prv_build_short(c->packet.Header, DORIME_HEADER_ACK, c->_internal.u32RxID,
			prv_ack_addr(c), c->_internal.u16AckCode);
	prv_set_deadline(c, c->u32Timeout);
	DORIME_SET_STATE(c, DOR_STATE_WAITING_FINAL_ACK_SENDED);
	if (prv_start_tx(c, c->packet.Header, DORIME_PCK_OVERHEAD) != DORIME_OK){
		// ACK not sent: do not deliver, the sender will time out and retry
		prv_rx_failed(c);
	}
}

static void prv_state_waiting_final_ack_sended(dorime_t *c){
	if (c->_internal.bTxEvent){
		c->_internal.bTxEvent = false;
		c->_internal.TxIsWaiting = false;
		if (c->_internal.bDeliver){
			dorime_evparams_data_t params;

			params.data = &c->packet.Buffer[DORIME_PCK_OFF_DATA];
			params.len = c->_internal.u16DataLen;
			// the user MUST copy the data, it will be freed soon
			dorime_cb_event(c, DORIME_EVT_DATA_REC, &params);
		}
		DORIME_SET_STATE(c, DOR_STATE_ABORT);
	}
	else if (prv_expired(c)){
		DORIME_SET_STATE(c, DOR_STATE_TIMEOUT);
	}
}

/** Dorime States: sending **/

static void prv_state_start_send(dorime_t *c){
	prv_lock(c);
	c->_internal.bSendPending = false;
	prv_unlock(c);

	c->_internal.bTxTransaction = true;
	c->_internal.isBroadcast = (c->_internal.u16DestAddr == DORIME_ADDR_BROADCAST);

	// Allocated now, so a lack of memory fails before touching the line
	c->packet.BufferLen = DORIME_PCK_OVERHEAD + c->_internal.u16DataLen;
	c->packet.Buffer = (uint8_t*)c->fxns._memAlloc(c->packet.BufferLen);
	if (c->packet.Buffer == NULL){
		prv_tx_failed(c);
		return;
	}

	prv_build_short(c->packet.Header, DORIME_HEADER_START, c->_internal.u32ID,
			c->_internal.u16DestAddr, c->_internal.u16DataLen);
	prv_set_deadline(c, c->u32Timeout);
	DORIME_SET_STATE(c, DOR_STATE_WAITING_START_SENDED);
	if (prv_start_tx(c, c->packet.Header, DORIME_PCK_OVERHEAD) != DORIME_OK){
		prv_tx_failed(c);
	}
}

static void prv_state_waiting_start_sended(dorime_t *c){
	if (c->_internal.bTxEvent){
		c->_internal.bTxEvent = false;
		c->_internal.TxIsWaiting = false;
		if (c->_internal.isBroadcast){
			// No ACK on broadcasts, just give the receivers time to arm
			prv_set_deadline(c, c->u32BroadcastGuard);
			DORIME_SET_STATE(c, DOR_STATE_START_WAITING_ACK);
		}
		else{
			prv_set_deadline(c, c->u32Timeout);
			DORIME_SET_STATE(c, DOR_STATE_START_WAITING_ACK);
			if (prv_start_rx(c, c->packet.Header, DORIME_PCK_OVERHEAD) != DORIME_OK){
				prv_tx_failed(c);
			}
		}
	}
	else if (prv_expired(c)){
		DORIME_SET_STATE(c, DOR_STATE_TIMEOUT);
	}
}

static void prv_state_start_waiting_ack(dorime_t *c){
	if (c->_internal.isBroadcast){
		if (prv_expired(c)){
			DORIME_SET_STATE(c, DOR_STATE_SEND_DATA);
		}
		return;
	}
	if (c->_internal.bRxEvent){
		c->_internal.bRxEvent = false;
		c->_internal.RxIsWaiting = false;
		DORIME_SET_STATE(c, DOR_STATE_START_REC_ACK);
	}
	else if (prv_expired(c)){
		DORIME_SET_STATE(c, DOR_STATE_TIMEOUT);
	}
}

static void prv_state_start_rec_ack(dorime_t *c){
	if (prv_check_ack(c) == PRV_ACK_VALID){
		DORIME_SET_STATE(c, DOR_STATE_SEND_DATA);
	}
	else{
		prv_tx_failed(c);
	}
}

static void prv_state_send_data(dorime_t *c){
	uint8_t *b = c->packet.Buffer;
	uint16_t len = c->_internal.u16DataLen;

	prv_put_head(b, DORIME_HEADER_DATA, c->_internal.u32ID,
			c->_internal.u16DestAddr, len);
	memcpy(&b[DORIME_PCK_OFF_DATA], c->_internal.pu8TxData, len);
	prv_finish_frame(b, len);

	prv_set_deadline(c, prv_data_timeout(c));
	DORIME_SET_STATE(c, DOR_STATE_WAITING_DATA_SENDED);
	if (prv_start_tx(c, b, (uint16_t)c->packet.BufferLen) != DORIME_OK){
		prv_tx_failed(c);
	}
}

static void prv_state_waiting_data_sended(dorime_t *c){
	if (c->_internal.bTxEvent){
		c->_internal.bTxEvent = false;
		c->_internal.TxIsWaiting = false;
		if (c->_internal.isBroadcast){
			// Nothing to wait for: done
			c->_internal.u32ID++;
			dorime_cb_event(c, DORIME_EVT_ACK_OK, NULL);
			DORIME_SET_STATE(c, DOR_STATE_ABORT);
		}
		else{
			prv_set_deadline(c, c->u32Timeout);
			DORIME_SET_STATE(c, DOR_STATE_SEND_WAITING_ACK);
			if (prv_start_rx(c, c->packet.Header, DORIME_PCK_OVERHEAD) != DORIME_OK){
				prv_tx_failed(c);
			}
		}
	}
	else if (prv_expired(c)){
		DORIME_SET_STATE(c, DOR_STATE_TIMEOUT);
	}
}

static void prv_state_send_waiting_ack(dorime_t *c){
	if (c->_internal.bRxEvent){
		c->_internal.bRxEvent = false;
		c->_internal.RxIsWaiting = false;
		DORIME_SET_STATE(c, DOR_STATE_SEND_REC_ACK);
	}
	else if (prv_expired(c)){
		DORIME_SET_STATE(c, DOR_STATE_TIMEOUT);
	}
}

static void prv_state_send_rec_ack(dorime_t *c){
	if (prv_check_ack(c) == PRV_ACK_VALID){
		// ID only advances on success: a user retry reuses the same ID
		c->_internal.u32ID++;
		dorime_cb_event(c, DORIME_EVT_ACK_OK, NULL);
		DORIME_SET_STATE(c, DOR_STATE_ABORT);
	}
	else{
		prv_tx_failed(c);
	}
}

/** Dorime States: common **/

static void prv_state_cooldown(dorime_t *c){
	if (prv_expired(c)){
		DORIME_SET_STATE(c, DOR_STATE_ABORT);
	}
}

static void prv_state_abort(dorime_t *c){
	prv_release(c);
	if (c->_internal.bTxTransaction){
		c->_internal.bTxTransaction = false;
		c->_internal.bTxBusy = false;
	}
	c->_internal.bDeliver = false;
	c->_internal.isBroadcast = false;
	DORIME_SET_STATE(c, DOR_STATE_IDLE);
}

static void prv_state_timeout(dorime_t *c){
	dorime_cb_event(c, DORIME_EVT_TIMEOUT, NULL);
	// The line may still be busy: wait before listening again
	prv_enter_cooldown(c);
}

/*
 * Publics
 */

dorime_err_e dorime_init(dorime_t *control){
	if (control == NULL){
		return DORIME_INVALID_HANDLE;
	}
	// check mandatory functions
	if (control->fxns._rx == NULL ||
			control->fxns._tx == NULL ||
			control->fxns._abort == NULL ||
			control->fxns._tickCont == NULL){
		return DORIME_INVALID_PARAMETERS;
	}
	// check for invalid parameters
	if (control->u32MsPerCount == 0 ||
			control->u32Timeout == 0 ||
			control->u16MaxDataLen == 0 ||
			control->u16MaxDataLen > DORIME_MAX_DATA_LEN_LIMIT ||
			control->u16Address == DORIME_ADDR_BROADCAST){
		return DORIME_INVALID_PARAMETERS;
	}
	// memory functions come in pairs
	if ((control->fxns._memAlloc == NULL) != (control->fxns._memFree == NULL)){
		return DORIME_INVALID_PARAMETERS;
	}

	// if no Memory Allocation fxn was declared, we will use
	// `malloc`and `free`.
	if (control->fxns._memAlloc == NULL){
		control->fxns._memAlloc = malloc;
		control->fxns._memFree = free;
	}
	if (control->u32CooldownTimeout == 0){
		control->u32CooldownTimeout = control->u32Timeout;
	}

	memset(&control->_internal, 0, sizeof(control->_internal));

	if (control->u16Address == DORIME_ADDR_CONTROLLER){
		control->_internal.enType = DORIME_TYPE_CONTROLLER;
	}
	else{
		control->_internal.enType = DORIME_TYPE_TARGET;
	}
	control->_internal.u8InitFlag = DORIME_INITIALIZED_FLAG;
	control->_internal.enState = DOR_STATE_IDLE;
	control->_internal.enInterfaceDir = DORIME_INTERFACE_DIRECTION_SLEEP;
	control->_internal.u32LastCnt = control->fxns._tickCont();

	control->packet.Buffer = NULL;
	control->packet.BufferLen = 0;

	return DORIME_OK;
}

dorime_err_e dorime_handler(dorime_t *control){
	if (control == NULL){
		return DORIME_INVALID_HANDLE;
	}
	if (control->_internal.u8InitFlag != DORIME_INITIALIZED_FLAG){
		return DORIME_NOT_INITIALIZED;
	}

	prv_update_tick(control);

	if (control->_internal.bAbortReq){
		control->_internal.bAbortReq = false;
		control->_internal.bSendPending = false;
		control->_internal.bTxTransaction = false;
		control->_internal.bTxBusy = false;
		DORIME_SET_STATE(control, DOR_STATE_ABORT);
	}

	switch (control->_internal.enState){
	case DOR_STATE_IDLE:
		prv_state_idle(control);
		break;
	case DOR_STATE_ADDR_CHECK:
		prv_state_addr_check(control);
		break;
	case DOR_STATE_SEND_ACK:
		prv_state_send_ack(control);
		break;
	case DOR_STATE_WAITING_ACK_SENDED:
		prv_state_waiting_ack_sended(control);
		break;
	case DOR_STATE_WAITING_PACKET:
		prv_state_waiting_packet(control);
		break;
	case DOR_STATE_RECEIVED_PACKET:
		prv_state_received_packet(control);
		break;
	case DOR_STATE_PACKET_FAILED:
		prv_state_packet_failed(control);
		break;
	case DOR_STATE_SEND_FINAL_ACK:
		prv_state_send_final_ack(control);
		break;
	case DOR_STATE_WAITING_FINAL_ACK_SENDED:
		prv_state_waiting_final_ack_sended(control);
		break;

	case DOR_STATE_START_SEND:
		prv_state_start_send(control);
		break;
	case DOR_STATE_WAITING_START_SENDED:
		prv_state_waiting_start_sended(control);
		break;
	case DOR_STATE_START_WAITING_ACK:
		prv_state_start_waiting_ack(control);
		break;
	case DOR_STATE_START_REC_ACK:
		prv_state_start_rec_ack(control);
		break;
	case DOR_STATE_SEND_DATA:
		prv_state_send_data(control);
		break;
	case DOR_STATE_WAITING_DATA_SENDED:
		prv_state_waiting_data_sended(control);
		break;
	case DOR_STATE_SEND_WAITING_ACK:
		prv_state_send_waiting_ack(control);
		break;
	case DOR_STATE_SEND_REC_ACK:
		prv_state_send_rec_ack(control);
		break;

	case DOR_STATE_COOLDOWN:
		prv_state_cooldown(control);
		break;
	case DOR_STATE_ABORT:
		prv_state_abort(control);
		break;
	case DOR_STATE_TIMEOUT:
		prv_state_timeout(control);
		break;
	default:
		DORIME_SET_STATE(control, DOR_STATE_ABORT);
		break;
	}

	return DORIME_OK;
}

dorime_err_e dorime_send_data(dorime_t *control, uint16_t destAddr, const uint8_t *data, uint16_t len){
	dorime_err_e err = DORIME_OK;

	if (control == NULL){
		return DORIME_INVALID_HANDLE;
	}
	if (control->_internal.u8InitFlag != DORIME_INITIALIZED_FLAG){
		return DORIME_NOT_INITIALIZED;
	}
	if (data == NULL || len == 0 || len > control->u16MaxDataLen){
		return DORIME_INVALID_PARAMETERS;
	}
	// A Target can only talk to the Controller (and never broadcasts)
	if (control->_internal.enType == DORIME_TYPE_TARGET &&
			destAddr != DORIME_ADDR_CONTROLLER){
		return DORIME_NOT_ALLOWED;
	}
	if (destAddr == control->u16Address){
		return DORIME_NOT_ALLOWED;
	}

	prv_lock(control);
	if (control->_internal.bTxBusy){
		err = DORIME_NOT_ALLOWED;
	}
	else{
		control->_internal.u16DestAddr = destAddr;
		control->_internal.pu8TxData = data;
		control->_internal.u16DataLen = len;
		control->_internal.bTxBusy = true;
		control->_internal.bSendPending = true;
	}
	prv_unlock(control);

	return err;
}

dorime_err_e dorime_abort(dorime_t *control){
	if (control == NULL){
		return DORIME_INVALID_HANDLE;
	}
	if (control->_internal.u8InitFlag != DORIME_INITIALIZED_FLAG){
		return DORIME_NOT_INITIALIZED;
	}

	// Executed by dorime_handler(), where all state changes live
	control->_internal.bAbortReq = true;

	return DORIME_OK;
}

dorime_err_e dorime_rx_event(dorime_t *control){
	if (control == NULL){
		return DORIME_INVALID_HANDLE;
	}
	if (control->_internal.u8InitFlag != DORIME_INITIALIZED_FLAG){
		return DORIME_NOT_INITIALIZED;
	}

	control->_internal.bRxEvent = true;

	return DORIME_OK;
}

dorime_err_e dorime_tx_event(dorime_t *control){
	if (control == NULL){
		return DORIME_INVALID_HANDLE;
	}
	if (control->_internal.u8InitFlag != DORIME_INITIALIZED_FLAG){
		return DORIME_NOT_INITIALIZED;
	}

	control->_internal.bTxEvent = true;

	return DORIME_OK;
}

/*
 * Weak Routines: These routines can be customized by developer,
 * to reach better performance
 */

DORIME_WEAK uint32_t dorime_crc32(uint8_t *arr, uint32_t len){
	return xcrc32(arr, len, 0x00);
}

DORIME_WEAK void dorime_cb_event(dorime_t *control, dorime_event_e event, void* params){
	(void)control;
	(void)event;
	(void)params;
}
