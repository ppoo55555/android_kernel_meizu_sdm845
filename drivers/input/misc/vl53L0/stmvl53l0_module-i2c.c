/*
 *  stmvl53l0_module-i2c.c - Linux kernel modules for STM VL53L0 FlightSense TOF
 *							sensor
 *
 *  Copyright (C) 2016 STMicroelectronics Imaging Division.
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
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/miscdevice.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/platform_device.h>
/*
 * power specific includes
 */
#include <linux/pwm.h>
#include <linux/regulator/consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/clk.h>
#include <linux/of_gpio.h>
/*
 * API includes
 */
#include "vl53l0_api.h"
#include "vl53l0_def.h"
#include "vl53l0_platform.h"
#include "stmvl53l0-i2c.h"
#include "stmvl53l0.h"

static int msm_tof_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return 0;
}

static long msm_tof_subdev_ioctl(struct v4l2_subdev *sd,
			unsigned int cmd, void *arg)
{
	return 0;
}

static int32_t msm_tof_power(struct v4l2_subdev *sd, int on)
{
	return 0;
}

static struct v4l2_subdev_core_ops msm_tof_subdev_core_ops = {
	.ioctl = msm_tof_subdev_ioctl,
	.s_power = msm_tof_power,
};

static const struct v4l2_subdev_internal_ops msm_tof_internal_ops = {
	.close = msm_tof_close,
};

static struct v4l2_subdev_ops msm_tof_subdev_ops = {
	.core = &msm_tof_subdev_core_ops,
};

static int stmvl53l0_parse_dt(struct device *dev, struct i2c_data *data)
{
	int ret = 0;

	data->subdev_initialized = 0;

	if (!dev->of_node)
		return -EINVAL;
	
	data->irq_gpio = of_get_named_gpio(dev->of_node,
					"stm,irq-gpio", 0);
	if (!gpio_is_valid(data->irq_gpio)) {
		dev_err(dev, "IRQ GPIO is invalid\n");
		return -EINVAL;
	}

	data->rst_gpio = of_get_named_gpio(dev->of_node,
					"gpios", 0);
	if (!gpio_is_valid(data->irq_gpio)) {
		dev_err(dev, "reset GPIO is invalid\n");
		return -EINVAL;
	}

	ret = devm_gpio_request_one(dev,
		data->rst_gpio, GPIOF_DIR_OUT,
		"stmvl53L0-reset");
	if (ret < 0) {
		dev_err(dev, "failed to request reset GPIO: %d\n", ret);
		return -EINVAL;
	}

	gpio_set_value_cansleep(data->rst_gpio, 0);

	ret = of_property_read_string(dev->of_node,
			"regulator-names", &data->vana_name);
	if (ret < 0) {
		dev_err(dev, "regulator name is invalid\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(dev->of_node,
			"rgltr-min-voltage", &data->vana_min_voltage);
	if (ret < 0) {
		dev_err(dev, "regulator minimum voltage is invalid\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(dev->of_node,
			"rgltr-max-voltage", &data->vana_max_voltage);
	if (ret < 0) {
		dev_err(dev, "regulator maximum voltage is invalid\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(dev->of_node,
			"rgltr-load-current", &data->vana_load);
	if (ret < 0) {
		dev_err(dev, "regulator load current is invalid\n");
		return -EINVAL;
	}

	data->vana = devm_regulator_get(dev, data->vana_name);
	if (IS_ERR(data->vana)) {
		dev_err(dev, "invalid vana regulator %s\n", data->vana_name);
		return -EINVAL;
	}

	return 0;
}

static int stmvl53l0_enable_vana(struct i2c_data *data)
{
	int ret = 0;

	ret = regulator_set_voltage(data->vana,
		data->vana_min_voltage, data->vana_max_voltage);
	if (ret < 0) {
		pr_err("%s: failed to set vana voltage: %d\n", __func__, ret);
		return -EINVAL;
	}

	ret = regulator_set_load(data->vana, data->vana_load);
	if (ret < 0) {
		pr_err("%s: failed to set vana voltage: %d\n", __func__, ret);
		return -EINVAL;
	}

	ret = regulator_enable(data->vana);
	if (ret < 0) {
		pr_err("%s: failed to enable VANA: %d\n", __func__, ret);
		return -EINVAL;
	}

	return 0;
}

static int stmvl53l0_disable_vana(struct i2c_data *data)
{
	int ret = 0;

	ret = regulator_disable(data->vana);
	if (ret < 0) {
		pr_err("%s: failed to enable VANA: %d\n", __func__, ret);
		return -EINVAL;
	}

	return 0;
}

static int stmvl53l0_reset(struct i2c_data *data, bool enable)
{
	if (!gpio_is_valid(data->rst_gpio))
		return -EINVAL;

	if (enable) {
		gpio_set_value_cansleep(data->rst_gpio, 1);
		usleep_range(2950, 3000);
	} else {
		usleep_range(2950, 3000);
		gpio_set_value_cansleep(data->rst_gpio, 0);
	}
	return 0;
}

static int stmvl53l0_init_subdev(struct device *dev, struct i2c_data *data)
{
	int ret = 0;

	while (data->subdev_initialized == false) {
		data->v4l2_dev_str.internal_ops =
			&msm_tof_internal_ops;
		data->v4l2_dev_str.ops = &msm_tof_subdev_ops;
		strcpy(data->v4l2_dev_str.name, CAMX_TOF_DEV_NAME);
		data->v4l2_dev_str.sd_flags =
			V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
		data->v4l2_dev_str.ent_function = CAM_IRLED_DEVICE_TYPE;
		data->v4l2_dev_str.token = data;

		ret = cam_register_subdev(&(data->v4l2_dev_str));
		if (ret) {
			dev_err(dev, "Fail to create subdev with %d", ret);
			continue;
		}

		data->subdev_initialized = true;
	}

	if (dev->init_name == NULL)
		strlcpy(data->device_name, dev->init_name, sizeof(data->device_name));
	else
		strlcpy(data->device_name, dev->kobj.name, sizeof(data->device_name));

	return 0;
}

static int stmvl53l0_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	int rc = 0;
	struct stmvl53l0_data *vl53l0_data = NULL;
	struct i2c_data *i2c_object = NULL;

	vl53l0_dbgmsg("Enter\n");

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE)) {
		rc = -EIO;
		return rc;
	}

	vl53l0_data = kzalloc(sizeof(struct stmvl53l0_data), GFP_KERNEL);
	if (!vl53l0_data) {
		rc = -ENOMEM;
		return rc;
	}
	if (vl53l0_data) {
		vl53l0_data->client_object =
		    kzalloc(sizeof(struct i2c_data), GFP_KERNEL);
		i2c_object = (struct i2c_data *)vl53l0_data->client_object;
	}
	i2c_object->client = client;

	/* setup bus type */
	vl53l0_data->bus_type = I2C_BUS;

	/* setup pinctrl and gpios */
	rc = stmvl53l0_parse_dt(&i2c_object->client->dev, i2c_object);
	if (rc)
		goto end;

	vl53l0_data->irq_gpio = i2c_object->irq_gpio;

	/* setup msm camera subdev */
	rc = stmvl53l0_init_subdev(&i2c_object->client->dev, i2c_object);
	if (rc)
		goto end;

	/* setup device name */
	vl53l0_data->dev_name = dev_name(&client->dev);

	/* setup device data */
	dev_set_drvdata(&client->dev, vl53l0_data);

	/* setup client data */
	i2c_set_clientdata(client, vl53l0_data);

	/* setup other stuff */
	rc = stmvl53l0_setup(vl53l0_data);

	/* init default value */
	i2c_object->power_up = 0;

	vl53l0_dbgmsg("End\n");
end:
	if (rc < 0) {
		kfree(i2c_object);
		kfree(vl53l0_data);
	}

	return rc;
}

static int stmvl53l0_remove(struct i2c_client *client)
{
	struct stmvl53l0_data *data = i2c_get_clientdata(client);

	vl53l0_dbgmsg("Enter\n");

	/* Power down the device */
	stmvl53l0_power_down_i2c(data->client_object);
	stmvl53l0_cleanup(data);
	kfree(data->client_object);
	kfree(data);
	vl53l0_dbgmsg("End\n");
	return 0;
}

static const struct i2c_device_id stmvl53l0_id[] = {
	{STMVL53L0_DRV_NAME, 0},
	{},
};

MODULE_DEVICE_TABLE(i2c, stmvl53l0_id);

static const struct of_device_id st_stmvl53l0_dt_match[] = {
	{.compatible = "st,stmvl53l0",},
	{},
};

static struct i2c_driver stmvl53l0_driver = {
	.driver = {
		   .name = STMVL53L0_DRV_NAME,
		   .owner = THIS_MODULE,
		   .of_match_table = st_stmvl53l0_dt_match,
		   },
	.probe = stmvl53l0_probe,
	.remove = stmvl53l0_remove,
	.id_table = stmvl53l0_id,

};

int stmvl53l0_power_up_i2c(void *i2c_object, unsigned int *preset_flag)
{
	int ret = 0;
	struct i2c_data *data = (struct i2c_data *)i2c_object;

	vl53l0_dbgmsg("Enter\n");

	ret = stmvl53l0_enable_vana(data);
	if (ret < 0) {
		pr_err("%s: failed to enable vana\n", __func__);
		goto err;
	}

	ret = stmvl53l0_reset(data, true);
	if (ret < 0) {
		pr_err("%s: failed to reset\n", __func__);
		goto err;
	}

	data->power_up = 1;
	*preset_flag = 1;

	vl53l0_dbgmsg("End\n");
	return 0;
err:
	cam_unregister_subdev(&data->v4l2_dev_str);
	return ret;
}


int stmvl53l0_i2c_power_status(void *i2c_object)
{
	struct i2c_data *data = (struct i2c_data *)i2c_object;
	return data->power_up;
}

int stmvl53l0_power_down_i2c(void *i2c_object)
{
	int ret = 0;
	struct i2c_data *data = (struct i2c_data *)i2c_object;

	vl53l0_dbgmsg("Enter\n");

	ret = stmvl53l0_reset(data, false);
	if (ret < 0) {
		pr_err("%s: failed to disable chip\n", __func__);
		goto err;
	}

	ret = stmvl53l0_disable_vana(data);
	if (ret < 0) {
		pr_err("%s: failed to disable vana\n", __func__);
		goto err;
	}

	data->power_up = 0;

	vl53l0_dbgmsg("End\n");
	return 0;
err:
	cam_unregister_subdev(&data->v4l2_dev_str);
	return ret;
}

int stmvl53l0_init_i2c(void)
{
	int ret = 0;

	vl53l0_dbgmsg("Enter\n");

	/* register as a i2c client device */
	ret = i2c_add_driver(&stmvl53l0_driver);
	if (ret)
		vl53l0_errmsg("%d erro ret:%d\n", __LINE__, ret);

	vl53l0_dbgmsg("End with rc:%d\n", ret);

	return ret;
}

void stmvl53l0_exit_i2c(void *i2c_object)
{
	vl53l0_dbgmsg("Enter\n");
	i2c_del_driver(&stmvl53l0_driver);

	vl53l0_dbgmsg("End\n");
}
