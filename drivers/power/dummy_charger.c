/*
 * dummy_charger.c - Dummy Charger Driver
 *
 * Copyright (C) 2014 Blumotix
 * Pierluigi Passaro <info@phoenixsoftware.it>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/platform_device.h>

struct dummy_data {
	struct device *dev;
	struct power_supply psy;
	struct power_supply acpsy;
};

static enum power_supply_property dummy_charger_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_PRESENT,
};

static enum power_supply_property dummy_charger_mains_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static int dummy_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = POWER_SUPPLY_STATUS_FULL;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = 100;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		val->intval = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static __devinit int dummy_probe(struct platform_device *pdev)
{
	struct dummy_data *data;
	struct device *dev = &pdev->dev;
	int ret = 0;

	data = kzalloc(sizeof(struct dummy_data), GFP_KERNEL);
	if (data == NULL) {
		dev_err(dev, "Cannot allocate memory.\n");
		return -ENOMEM;
	}
	data->dev = dev;
	platform_set_drvdata(pdev, data);

	data->acpsy.name = "dummy-mains";
	data->acpsy.type = POWER_SUPPLY_TYPE_MAINS;
	data->acpsy.get_property = dummy_get_property;
	data->acpsy.properties = dummy_charger_mains_props;
	data->acpsy.num_properties = ARRAY_SIZE(dummy_charger_mains_props);

	ret = power_supply_register(dev, &data->acpsy);
	if (ret) {
		dev_err(dev, "failed: power supply register.\n");
		goto err;
	}

	data->psy.name = "dummy-battery";
	data->psy.type = POWER_SUPPLY_TYPE_BATTERY;
	data->psy.get_property = dummy_get_property;
	data->psy.properties = dummy_charger_battery_props;
	data->psy.num_properties = ARRAY_SIZE(dummy_charger_battery_props);

	ret = power_supply_register(dev, &data->psy);
	if (ret) {
		dev_err(dev, "failed: power supply register.\n");
		goto err;
	}

	power_supply_changed(&data->psy);
	power_supply_changed(&data->acpsy);

	return 0;

err:
	kfree(data);
	return ret;
}

static __devexit int dummy_remove(struct platform_device *pdev)
{
	struct dummy_data *data = platform_get_drvdata(pdev);

	if (data) {
		power_supply_unregister(&data->psy);
		power_supply_unregister(&data->acpsy);
		kfree(data);
	}

	return 0;
}

static struct platform_driver dummy_driver = {
	.probe	= dummy_probe,
	.remove	= __devexit_p(dummy_remove),
	.driver = {
		.name	= "dummy-charger",
		.owner	= THIS_MODULE,
	},
};

static int __init dummy_init(void)
{
	return platform_driver_register(&dummy_driver);
}
module_init(dummy_init);

static void __exit dummy_exit(void)
{
	platform_driver_unregister(&dummy_driver);
}
module_exit(dummy_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Dummy Charger Driver");
MODULE_AUTHOR("Pierluigi Passaro <info@phoenixsoftware.it>");
MODULE_ALIAS("dummy-charger");
