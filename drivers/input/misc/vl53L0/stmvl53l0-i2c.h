/*
 *  stmvl53l0-i2c.h - Linux kernel modules for STM VL53L0 FlightSense TOF sensor
 *
 *  Copyright (C) 2016 STMicroelectronics Imaging Division
 * 
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
/*
 * Defines
 */
#ifndef STMVL53L0_I2C_H
#define STMVL53L0_I2C_H

#include <linux/types.h>
#include <uapi/media/cam_req_mgr.h>
#include "cam_subdev.h"

#define CAMX_TOF_DEV_NAME "cam-laser-driver"

struct i2c_data {
	struct i2c_client *client;
	int rst_gpio;
	int irq_gpio;
	struct regulator *vana;
	const char *vana_name;
	int vana_min_voltage;
	int vana_max_voltage;
	int vana_load;
	uint8_t power_up;
	char subdev_initialized;
	struct cam_subdev v4l2_dev_str;
	char device_name[20];
};
int stmvl53l0_init_i2c(void);
void stmvl53l0_exit_i2c(void *);
int stmvl53l0_power_up_i2c(void *, unsigned int *);
int stmvl53l0_power_down_i2c(void *);
int stmvl53l0_i2c_power_status(void *);

#endif /* STMVL53L0_I2C_H */
