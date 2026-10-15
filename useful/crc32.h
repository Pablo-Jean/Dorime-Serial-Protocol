/*
 * crc32.h
 *
 *  Created on: May 20, 2024
 *      Author: pablo-jean
 */

#ifndef DORIME_SERIAL_PROTOCOL_USEFUL_CRC32_H_
#define DORIME_SERIAL_PROTOCOL_USEFUL_CRC32_H_

#include <stdint.h>

uint32_t xcrc32(uint8_t *buf, uint32_t len, uint32_t init);

#endif /* DORIME_SERIAL_PROTOCOL_USEFUL_CRC32_H_ */
