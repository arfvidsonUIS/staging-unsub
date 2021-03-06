/* linuxserial.h
 *
 * Copyright (c) 2010 - 2014 UNISYS CORPORATION
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 */

#ifndef __LINUXSERIAL_H__
#define __LINUXSERIAL_H__

#include "timskmod.h"

/* struct linux_serial is an opaque structure to users.
 * Fields are declared only in the implementation .c files.
 */
struct linux_serial;

struct linux_serial *linuxserial_create(int devno, void *context,
					void (*transmit_char)(void *, u8));
void linuxserial_rx_char(struct linux_serial *ls, u8 c);
void linuxserial_destroy(struct linux_serial *ls);

#endif
