/*
 * Kernel Device Mapper for abstracting ZAC/ZBC devices as normal
 * block devices for linux file systems.
 *
 * Linux kernel CRC implemenations
 *
 * This file is licensed under  the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef _LIB_CRC_H_
#define  _LIB_CRC_H_

#include "utypes.h"

#ifdef __cplusplus
extern "C" {
#endif

u16 crc16(u16 crc, const void *buffer, size_t len);
u32 crc32c(u32 crc, const void *data, unsigned int length);
u32 crc32(u32 crc, const void *p, unsigned int length);

#ifdef __cplusplus
}
#endif

#endif // _LIB_CRC_H_
