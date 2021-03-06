/* $Id$ */
/* Copyright (C) 2004 Alexander Chernov */

/* This file is derived from `netinet/in_systm.h' of the GNU C Library,
   version 2.3.2. The original copyright follows. */

/* System specific type definitions for networking code.
   Copyright (C) 1997 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307 USA.  */

#ifndef __RCC_NETINET_IN_SYSTM_H__
#define __RCC_NETINET_IN_SYSTM_H__ 1

//#include <sys/cdefs.h>
#include <features.h>
#include <sys/types.h>

/*
 * Network order versions of various data types. Unfortunately, BSD
 * assumes specific sizes for shorts (16 bit) and longs (32 bit) which
 * don't hold in general. As a consequence, the network order versions
 * may not reflect the actual size of the native data types.
 */

typedef u_int16_t n_short;      /* short as received from the net */
typedef u_int32_t n_long;       /* long as received from the net  */
typedef u_int32_t n_time;       /* ms since 00:00 GMT, byte rev   */

#endif /* __RCC_NETINET_IN_SYSTM_H__ */
