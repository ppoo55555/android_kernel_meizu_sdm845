/* drivers/regulator/twl80125-regulator.c
 *
 * Copyright (C) 2014 HTC Corporation.
 * Copyright (C) 2024 MeizuCustoms enthusiasts
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/of_gpio.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/i2c.h>
#include <linux/delay.h>

enum {
	VREG_TYPE_BUCK,
	VREG_TYPE_LDO,
	VREG_TYPE_MAX
};

struct twl80125_vreg {
	struct device *dev;
	struct regulator_desc rdesc;
	struct regulator_dev *rdev;
	struct regulator_init_data *init_data;
	const char *regulator_name;

	int regulator_type;
	u32 enable_bit;
	u32 vtg_control_addr;

	int vtg_index;
	bool enabled;
	bool inited;
	struct mutex mlock;
};

struct twl80125_regulator {
	struct device *dev;
	struct twl80125_vreg *twl80125_vregs;
	int total_vregs;
	int en_gpio;
	bool is_enabled;
	struct mutex i2c_lock;

	atomic_t total_vregs_enabled;

	struct regulator *vddio;
	const char *vddio_name;
	u32 vddio_min_volt;
	u32 vddio_max_volt;
	u32 vddio_load_cur;
};

static struct twl80125_regulator *regulator = NULL;

static const int twl80125_buck_voltages[5] =
    { 1000000, 1050000, 1100000, 1200000, 1800000 };

static const int twl80125_ldo_voltages[5] =
    { 1200000, 1800000, 2800000, 2900000, 3000000 };

#define LDO_UV_VMIN             500000
#define LDO_UV_STEP              25000
#define LDO_UV_VMAX            1800000
#define BUCK_UV_VMIN            600000
#define BUCK_UV_STEP             12500
#define BUCK_UV_VMAX           3000000

#define TWL80125_VSEL_ADDR			0x4
#define TWL80125_ERROR_CODE_ADDR	0xA
#define TWL80125_REG_ENABLE_ADDR	0xB

#define TWL80125_BUCK1_VSEL_BIT		0x0
#define TWL80125_BUCK2_VSEL_BIT		0x1

static int twl80125_enable(struct twl80125_regulator *reg, bool enable)
{
	int ret = 0;

	gpio_direction_output(reg->en_gpio, 1);
	if (enable) {
		ret = regulator_enable(reg->vddio);
		if (ret < 0)
			pr_err("%s: failed to enable vddio\n", __func__);
		
		gpio_set_value(reg->en_gpio, 1);
		mdelay(5);

		reg->is_enabled = true;
		pr_info("%s: enabled\n", __func__);
	} else {
		gpio_set_value(reg->en_gpio, 0);
		mdelay(1);

		ret = regulator_disable(reg->vddio);
		if (ret < 0)
			pr_err("%s: failed to disable vddio\n", __func__);

		reg->is_enabled = false;
		pr_info("%s: disabled\n", __func__);
	}

	return ret;
}

static int twl80125_i2c_write(struct device *dev, u8 reg_addr, u8 data)
{
	int res, i;
	struct i2c_client *client = to_i2c_client(dev);
	struct twl80125_regulator *reg = i2c_get_clientdata(client);
	u8 values[] = {reg_addr, data};

	struct i2c_msg msg[] = {
		{
			.addr	= client->addr,
			.flags	= 0,
			.len	= 2,
			.buf	= values,
		},
	};

	mutex_lock(&reg->i2c_lock);

	for (i = 0; i < 4; i++) {
		res = i2c_transfer(client->adapter, msg, 1);
		if (res < 0) {
			pr_err("%s: transfer failed (%d), attempt %d\n", __func__, res, i);
			continue;
		}
		mdelay(20);
	}

	mutex_unlock(&reg->i2c_lock);

	if (res > 0)
		res = 0;

	return res;
}

static int twl80125_i2c_read(struct device *dev, u8 reg_addr, u8 *data)
{
	int res;
	struct i2c_client *client = to_i2c_client(dev);
	struct twl80125_regulator *reg = i2c_get_clientdata(client);

	struct i2c_msg msg[] = {
		{
			.addr	= client->addr,
			.flags	= 0,
			.len	= 1,
			.buf	= &reg_addr,
		},
		{
			.addr	= client->addr,
			.flags	= I2C_M_RD,
			.len	= 1,
			.buf	= data,
		},
	};

	mutex_lock(&reg->i2c_lock);

	res = i2c_transfer(client->adapter, msg, 2);

	mutex_unlock(&reg->i2c_lock);

	if (res >= 0)
		res = 0;

	return res;
}

static int twl80125_check_error(void)
{
	int res = -EINVAL;
	u8 error_code = 0;

	if (regulator) {
		res = twl80125_i2c_read(regulator->dev, TWL80125_ERROR_CODE_ADDR, &error_code);
		pr_info("%s: error code=%d\n", __func__, error_code);
	}
	else
		pr_err("%s: regulator not init\n", __func__);

	return res;
}

static int twl80125_clear_error(void)
{
	int res = -EINVAL;
	if (regulator)
		res = twl80125_i2c_write(regulator->dev, TWL80125_ERROR_CODE_ADDR, 0);
	else
		pr_err("%s: regulator not init\n", __func__);
	
	return res;
}

static int twl80125_vreg_is_enabled(struct regulator_dev *rdev)
{
	struct twl80125_vreg *vreg = rdev_get_drvdata(rdev);
	struct device *dev = vreg->dev;
	uint8_t val = 0;
	int rc = 0;

	if (vreg->inited != 1) {
		pr_err("%s: vreg not inited ready\n", __func__);
		return -1;
	}
	mutex_lock(&vreg->mlock);
	rc = twl80125_i2c_read(dev, TWL80125_REG_ENABLE_ADDR, &val);
	mutex_unlock(&vreg->mlock);

	return ((val & (1 << vreg->enable_bit))? 1: 0);
}

static int twl80125_vreg_enable(struct regulator_dev *rdev)
{
	struct twl80125_vreg *vreg = rdev_get_drvdata(rdev);
	struct device *dev = vreg->dev;
	uint8_t val = 0;
	int rc = 0, vtg;

	// Enable regulator and GPIO if needed
	if (atomic_read(&regulator->total_vregs_enabled) <= 0)
		twl80125_enable(regulator, true);

	// Set voltage before actual enable
	mutex_lock(&vreg->mlock);
	if (vreg->enabled) {
		mutex_unlock(&vreg->mlock);
		return 0;
	}

	if (vreg->regulator_type == VREG_TYPE_LDO) {
		vtg = twl80125_ldo_voltages[vreg->vtg_index];
		val = (vtg - LDO_UV_VMIN) / LDO_UV_STEP;
	} else {
		vtg = twl80125_buck_voltages[vreg->vtg_index];
		val = (vtg - BUCK_UV_VMIN) / BUCK_UV_STEP;
	}
	twl80125_i2c_write(dev, vreg->vtg_control_addr, val);
	mutex_unlock(&vreg->mlock);

	// Clear error code before enable regualtor
	twl80125_check_error();
	twl80125_clear_error();
	mutex_lock(&vreg->mlock);
	rc = twl80125_i2c_read(dev, TWL80125_REG_ENABLE_ADDR, &val);
	val |= (1 << vreg->enable_bit);
	rc = twl80125_i2c_write(dev, TWL80125_REG_ENABLE_ADDR, val);
	mutex_unlock(&vreg->mlock);

	atomic_inc(&regulator->total_vregs_enabled);
	vreg->enabled = true;
	return rc;
}

static int twl80125_vreg_disable(struct regulator_dev *rdev)
{

	struct twl80125_vreg *vreg = rdev_get_drvdata(rdev);
	struct device *dev = vreg->dev;
	uint8_t val = 0;
	int rc = 0;

	mutex_lock(&vreg->mlock);
	if (!vreg->enabled) {
		mutex_unlock(&vreg->mlock);
		return 0;
	}

	rc = twl80125_i2c_read(dev, TWL80125_REG_ENABLE_ADDR, &val);
	val &= ~(1 << vreg->enable_bit);
	rc = twl80125_i2c_write(dev, TWL80125_REG_ENABLE_ADDR, val);
	mutex_unlock(&vreg->mlock);

	// Disable twl80125 if no vregs are being used
	if (atomic_dec_return(&regulator->total_vregs_enabled) <= 0)
		twl80125_enable(regulator, false);

	vreg->enabled = false;
	return rc;
}

static int twl80125_vreg_set_voltage_sel(struct regulator_dev *rdev, unsigned selector)
{
	struct twl80125_vreg *vreg = rdev_get_drvdata(rdev);
	mutex_lock(&vreg->mlock);
	vreg->vtg_index = selector;
	mutex_unlock(&vreg->mlock);
	return 0;
}

static int twl80125_vreg_get_voltage_sel(struct regulator_dev *rdev)
{
	struct twl80125_vreg *vreg = rdev_get_drvdata(rdev);
	return vreg->vtg_index;
}

int twl80125_vreg_list_voltage(struct regulator_dev *rdev, unsigned selector) {
	struct twl80125_vreg *vreg = rdev_get_drvdata(rdev);
	if (vreg->regulator_type == VREG_TYPE_BUCK)
		return twl80125_buck_voltages[selector];
	else
		return twl80125_ldo_voltages[selector];
}

static struct regulator_ops twl80125_vreg_ops = {
	.enable		= twl80125_vreg_enable,
	.disable	= twl80125_vreg_disable,
	.is_enabled	= twl80125_vreg_is_enabled,
	.list_voltage 		= twl80125_vreg_list_voltage,
	.set_voltage_sel	= twl80125_vreg_set_voltage_sel,
	.get_voltage_sel	= twl80125_vreg_get_voltage_sel,
};

static struct regulator_ops *vreg_ops[] = {
	[VREG_TYPE_BUCK]		= &twl80125_vreg_ops,
	[VREG_TYPE_LDO]			= &twl80125_vreg_ops,
};

static int twl80125_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct device_node *child = NULL;
	struct twl80125_vreg *twl80125_vreg = NULL;
	struct twl80125_regulator *reg;
	struct regulator_init_data *init_data;
	struct regulator_config reg_config = {};
	int en_gpio = 0;
	int num_vregs = 0;
	int vreg_idx = 0;
	int ret = 0;
	int i = 0;
	u32 init_volt;

	pr_info("%s\n", __func__);
	if (!dev->of_node) {
		dev_err(dev, "%s: device tree information missing\n", __func__);
		return -ENODEV;
	}

	reg = kzalloc(sizeof(struct twl80125_regulator), GFP_KERNEL);
	if (reg == NULL) {
		dev_err(dev, "%s: could not allocate memory for reg.\n", __func__);
		return -ENOMEM;
	}

	reg->dev = dev;

	ret = of_property_read_string(node, "regulator-names", &reg->vddio_name);
	if (ret) {
		pr_err("%s: Fail to read vddio regulator name: %d\n", __func__, ret);
		goto fail_free_regulator;
	}

	ret = of_property_read_u32(node, "rgltr-min-voltage", &reg->vddio_min_volt);
	if (ret) {
		pr_err("%s: Fail to read vddio min voltage: %d\n", __func__, ret);
		goto fail_free_regulator;
	}

	ret = of_property_read_u32(node, "rgltr-max-voltage", &reg->vddio_max_volt);
	if (ret) {
		pr_err("%s: Fail to read vddio max voltage: %d\n", __func__, ret);
		goto fail_free_regulator;
	}

	ret = of_property_read_u32(node, "rgltr-load-current", &reg->vddio_load_cur);
	if (ret) {
		pr_err("%s: Fail to read vddio min voltage: %d\n", __func__, ret);
		goto fail_free_regulator;
	}

	reg->vddio = regulator_get(dev, reg->vddio_name);
	if (IS_ERR(reg->vddio)) {
		pr_err("%s: Fail to get vddio: %d\n", __func__, PTR_ERR(reg->vddio));
	} else {
		ret = regulator_set_voltage(reg->vddio,
			reg->vddio_min_volt, reg->vddio_max_volt);
		if (ret < 0) {
			pr_err("%s: Fail to set vddio voltage: %d\n", __func__, ret);
			goto fail_free_vddio;
		}

		ret = regulator_set_load(reg->vddio, reg->vddio_load_cur);
		if (ret < 0) {
			pr_err("%s: Fail to set vddio max load: %d\n", __func__, ret);
			goto fail_free_vddio;
		}
	}

	reg->en_gpio = of_get_named_gpio(node, "twl,enable-gpio", 0);
	if (!gpio_is_valid(reg->en_gpio)) {
		pr_err("%s: Fail to read enable gpio: %d\n", __func__, reg->en_gpio);
		goto fail_free_vddio;
	}
	
	/* Calculate number of regulators */
	for_each_child_of_node(node, child)
		num_vregs++;
	
	reg->total_vregs = num_vregs;
	mutex_init(&reg->i2c_lock);

	reg->twl80125_vregs = kzalloc(sizeof(struct twl80125_vreg) * num_vregs, GFP_KERNEL);
	if (reg->twl80125_vregs == NULL) {
		dev_err(dev, "%s: could not allocate memory for twl80125_vreg\n", __func__);
		return -ENOMEM;
	}

	/* Get device tree properties */
	for_each_child_of_node(node, child) {
		twl80125_vreg = &reg->twl80125_vregs[vreg_idx++];
		ret = of_property_read_string(child, "regulator-name",
				&twl80125_vreg->regulator_name);
		if (ret) {
			dev_err(dev, "%s: regulator-name missing in DT node\n", __func__);
			goto fail_free_vreg;
		}

		ret = of_property_read_u32(child, "twl,regulator-type",
				&twl80125_vreg->regulator_type);
		if (ret) {
			dev_err(dev, "%s: twl,regulator-type missing in DT node\n", __func__);
			goto fail_free_vreg;
		}

		if ((twl80125_vreg->regulator_type < 0)
		    || (twl80125_vreg->regulator_type >= VREG_TYPE_MAX)) {
			dev_err(dev, "%s: invalid regulator type: %d\n", __func__, twl80125_vreg->regulator_type);
			ret = -EINVAL;
			goto fail_free_vreg;
		}
		twl80125_vreg->rdesc.ops = vreg_ops[twl80125_vreg->regulator_type];

		ret = of_property_read_u32(child, "twl,enable-bit",
				&twl80125_vreg->enable_bit);
		if (ret) {
			dev_err(dev, "%s: Fail to get vreg enable bit.\n", __func__);
			goto fail_free_vreg;
		}

		ret = of_property_read_u32(child, "twl,voltage-control-addr",
				&twl80125_vreg->vtg_control_addr);
		if (ret) {
			dev_err(dev, "%s: Fail to get vreg base address.\n", __func__);
			goto fail_free_vreg;
		}

		if (twl80125_vreg->regulator_type == VREG_TYPE_LDO)
			twl80125_vreg->rdesc.n_voltages = ARRAY_SIZE(twl80125_ldo_voltages);
		else if (twl80125_vreg->regulator_type == VREG_TYPE_BUCK)
			twl80125_vreg->rdesc.n_voltages = ARRAY_SIZE(twl80125_buck_voltages);

		init_data = of_get_regulator_init_data(dev, child, &twl80125_vreg->rdesc);
		if (init_data == NULL) {
			dev_err(dev, "%s: unable to allocate memory\n", __func__);
			ret = -ENOMEM;
			goto fail_free_vreg;
		}

		if (init_data->constraints.name == NULL) {
			dev_err(dev, "%s: regulator name not specified\n", __func__);
			ret = -EINVAL;
			goto fail_free_vreg;
		}

		twl80125_vreg->rdesc.name 	= init_data->constraints.name;
		twl80125_vreg->dev		= dev;
		init_data->constraints.valid_ops_mask |= REGULATOR_CHANGE_STATUS | REGULATOR_CHANGE_VOLTAGE;

		mutex_init(&twl80125_vreg->mlock);
		reg_config.dev = dev;
		reg_config.init_data = init_data;
		reg_config.driver_data = twl80125_vreg;
		reg_config.of_node = child;
		twl80125_vreg->rdev = regulator_register(&twl80125_vreg->rdesc, &reg_config);
		pr_info("%s: register regulator %s\n", __func__, twl80125_vreg->regulator_name);
		if (IS_ERR(twl80125_vreg->rdev)) {
			ret = PTR_ERR(twl80125_vreg->rdev);
			twl80125_vreg->rdev = NULL;
			pr_err("%s: regulator register failed: %s, ret = %d\n", __func__, twl80125_vreg->rdesc.name, ret);
			goto fail_free_vreg;
		}
		twl80125_vreg->inited = 1;
	}

	atomic_set(&reg->total_vregs_enabled, 0);
	i2c_set_clientdata(client, reg);
	regulator = reg;
	return 0;

fail_free_vreg:
	kfree(reg->twl80125_vregs);

fail_free_vddio:
	regulator_put(reg->vddio);

fail_free_regulator:
	kfree(reg);
	return ret;
}

static int twl80125_remove(struct i2c_client *client)
{
	struct twl80125_regulator *reg;

	reg = i2c_get_clientdata(client);
	kfree(reg->twl80125_vregs);
	kfree(reg);

	return 0;
}

static struct of_device_id twl80125_match_table[] = {
	{.compatible = "ti,twl80125"},
	{},
};

static const struct i2c_device_id twl80125_id[] = {
	{"twl80125", 0},
	{},
};

static struct i2c_driver twl80125_driver = {
	.driver = {
		.name		= "twl80125-regulator",
		.owner		= THIS_MODULE,
		.of_match_table	= twl80125_match_table,
	},
	.probe		= twl80125_probe,
	.remove		= twl80125_remove,
	.id_table	= twl80125_id,
};

int __init twl80125_regulator_init(void)
{
	int ret = 0;

	ret = i2c_add_driver(&twl80125_driver);
	if (ret)
		pr_err("%s: Driver registration failed\n", __func__);

	return ret;
}
EXPORT_SYMBOL(twl80125_regulator_init);

static void __exit twl80125_regulator_exit(void)
{
	i2c_del_driver(&twl80125_driver);
}

MODULE_AUTHOR("Jim Hsia <jim_hsia@htc.com>");
MODULE_DESCRIPTION("TWL80125 regulator driver");
MODULE_LICENSE("GPL v2");

module_init(twl80125_regulator_init);
module_exit(twl80125_regulator_exit);
