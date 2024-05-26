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

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio/consumer.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>
#include <linux/gpio.h>

#include "sy7808.h"

struct sy7808_data {
    struct platform_device *pdev;

    cam_sensor_power_ctrl_t power;
    struct camera_io_master io;

    int id;
    struct mutex lock;
};

static struct sy7808_data g_sy7808[2] = {
    {}, {},
};

static int sy7808_probe(struct i2c_client *client)
{
	return 0;
}

static void sy7808_remove(struct i2c_client *client)
{
	return;
}

static const struct i2c_device_id sy7808_id_i2c[] = {
	{"si,sy7808", 0},
	{}
};

static const struct of_device_id sy7808_of_match[] = {
	{.compatible = "si,sy7808"},
	{},
};
MODULE_DEVICE_TABLE(of, sy7808_of_match);

static struct i2c_driver sy7808_i2c_driver = {
	.driver = {
		.name		    = "sy7808",
		.of_match_table = sy7808_of_match,
	},
	.id_table	= sy7808_id_i2c,
	.probe		= sy7808_i2c_probe,
	.remove		= sy7808_i2c_remove,
};

module_i2c_driver(sy7808_i2c_driver);
