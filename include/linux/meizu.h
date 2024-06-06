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

enum mz_device_model {
    MZ_DEVICE_UNKNOWN = -1,
    MZ_DEVICE_16TH,      // Meizu 16th
    MZ_DEVICE_16THPLUS,  // Meizu 16th Plus
    MZ_DEVICE_ZERO,      // Meizu Zero
};

struct mz_device_info {
    // Software version (sw_version)
    char sw_version[16];
    // Hardware version (hw_version)
    int hw_version;
    // Device model
    enum mz_device_model model;
};

int mz_part_read(const char *part, char *buf, size_t count, loff_t offset);

int mz_get_hw_version(void);
enum mz_device_model mz_get_model(void);

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

/*
 * Convenience methods
 */

static inline bool mz_is_16th(void)
{
    return mz_get_model() == MZ_DEVICE_16TH;
}

static inline bool mz_is_16th_plus(void)
{
    return mz_get_model() == MZ_DEVICE_16THPLUS;
}

static inline bool mz_is_zero(void)
{
    return mz_get_model() == MZ_DEVICE_ZERO;
}

#endif
