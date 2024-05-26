/*
 *  Copyright (C) 2024 MeizuCustoms enthusiasts
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <linux/device.h>
#include <linux/types.h>

#ifndef __MEIZU_H
#define __MEIZU_H

int mz_part_read(const char *part, char *buf, size_t count, loff_t offset);

/* Read data from 'reserved' partition */
static inline int mz_reserved_read(char *buf, size_t count, loff_t offset)
{
    return mz_part_read("reserved", buf, count, offset);
}

/* Read data from 'private' partition,
   where the most calibration data is stored */
static inline int mz_private_read(char *buf, size_t count, loff_t offset)
{
    return mz_part_read("private", buf, count, offset);
}

#endif
