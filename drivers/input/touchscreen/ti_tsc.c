/*
 * TI Touch Screen driver
 *
 * Copyright (C) 2011 Texas Instruments Incorporated - http://www.ti.com/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */


#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/input/ti_tsc.h>
#include <linux/delay.h>
#include <linux/mfd/ti_tscadc.h>

#define MAX_12BIT                       ((1 << 12) - 1)

int pen = 1;
unsigned int bckup_x = 0, bckup_y = 0;

struct tscadc {
	struct input_dev	*input;
	struct ti_tscadc_dev	*mfd_tscadc;
	int			wires;
	struct tsc_axis		x;
	struct tsc_axis		y;
	int			x_plate_resistance;
	int			irq;
	int			steps_to_config;
	int			calibration_type;
	int			deltax, deltay;
	int			a[7]; // ref tslib/plugins/linear.c
};

static unsigned int tscadc_readl(struct tscadc *ts, unsigned int reg)
{
	return readl(ts->mfd_tscadc->tscadc_base + reg);
}

static void tscadc_writel(struct tscadc *tsc, unsigned int reg,
					unsigned int val)
{
	writel(val, tsc->mfd_tscadc->tscadc_base + reg);
}

static void tsc_step_config(struct tscadc *ts_dev)
{
	unsigned int	stepconfigx = 0, stepconfigy = 0;
	unsigned int	delay, chargeconfig = 0;
	unsigned int	stepconfigz1 = 0, stepconfigz2 = 0;
	int		i, total_steps;

	/* Configure the Step registers */

	delay = TSCADC_STEPCONFIG_SAMPLEDLY | TSCADC_STEPCONFIG_OPENDLY;

	total_steps = 2 * ts_dev->steps_to_config;
	stepconfigx = TSCADC_STEPCONFIG_MODE_HWSYNC |
			TSCADC_STEPCONFIG_AVG_16 | TSCADC_STEPCONFIG_XPP;

	switch (ts_dev->wires) {
	case 4:
		stepconfigx |= TSCADC_STEPCONFIG_INP_AN2 |
				TSCADC_STEPCONFIG_XNN;
		break;
	case 5:
		stepconfigx |= TSCADC_STEPCONFIG_YNN |
				TSCADC_STEPCONFIG_INP_AN4 |
				TSCADC_STEPCONFIG_XNN |
				TSCADC_STEPCONFIG_YPP;
		break;
	case 8:
		stepconfigx |= TSCADC_STEPCONFIG_INP_AN2 |
				TSCADC_STEPCONFIG_XNN;
		break;
	}

	for (i = 1; i <= ts_dev->steps_to_config; i++) {
		tscadc_writel(ts_dev, TSCADC_REG_STEPCONFIG(i), stepconfigx);
		tscadc_writel(ts_dev, TSCADC_REG_STEPDELAY(i), delay);
	}

	stepconfigy = TSCADC_STEPCONFIG_MODE_HWSYNC |
			TSCADC_STEPCONFIG_AVG_16 | TSCADC_STEPCONFIG_YNN |
			TSCADC_STEPCONFIG_INM_ADCREFM;
	switch (ts_dev->wires) {
	case 4:
		stepconfigy |= TSCADC_STEPCONFIG_YPP;
		break;
	case 5:
		stepconfigy |= TSCADC_STEPCONFIG_XPP |
			TSCADC_STEPCONFIG_INP_AN4 | TSCADC_STEPCONFIG_XNP |
			TSCADC_STEPCONFIG_YPN;
		break;
	case 8:
		stepconfigy |= TSCADC_STEPCONFIG_YPP;
		break;
	}

	for (i = (ts_dev->steps_to_config + 1); i <= total_steps; i++) {
		tscadc_writel(ts_dev, TSCADC_REG_STEPCONFIG(i), stepconfigy);
		tscadc_writel(ts_dev, TSCADC_REG_STEPDELAY(i), delay);
	}

	chargeconfig = TSCADC_STEPCONFIG_XPP | TSCADC_STEPCONFIG_YNN |
			TSCADC_STEPCHARGE_RFP_XPUL |
			TSCADC_STEPCHARGE_RFM_XNUR |
			TSCADC_STEPCHARGE_INM_AN1 | TSCADC_STEPCHARGE_INP_AN1;
	tscadc_writel(ts_dev, TSCADC_REG_CHARGECONFIG, chargeconfig);
	tscadc_writel(ts_dev, TSCADC_REG_CHARGEDELAY, TSCADC_CHARGEDLY_OPENDLY);

	 /* Configure to calculate pressure */
	stepconfigz1 = TSCADC_STEPCONFIG_MODE_HWSYNC |
			TSCADC_STEPCONFIG_AVG_16 | TSCADC_STEPCONFIG_XNP |
			TSCADC_STEPCONFIG_YPN | TSCADC_STEPCONFIG_INM_ADCREFM;
	stepconfigz2 = stepconfigz1 | TSCADC_STEPCONFIG_INP_AN3;
	tscadc_writel(ts_dev, TSCADC_REG_STEPCONFIG(total_steps + 1),
						stepconfigz1);
	tscadc_writel(ts_dev, TSCADC_REG_STEPDELAY(total_steps + 1), delay);
	tscadc_writel(ts_dev, TSCADC_REG_STEPCONFIG(total_steps + 2),
						stepconfigz2);
	tscadc_writel(ts_dev, TSCADC_REG_STEPDELAY(total_steps + 2), delay);

	tscadc_writel(ts_dev, TSCADC_REG_SE, TSCADC_STPENB_STEPENB_TC);
}

static void tscadc_autocalibrate(struct tscadc *ts_dev, unsigned int *x, unsigned int *y) {
	if (*x < ts_dev->x.min) {
		ts_dev->x.min = *x;
		ts_dev->deltax = ts_dev->x.max - ts_dev->x.min + 1;
	} else if (*x > ts_dev->x.max) {
		ts_dev->x.max = *x;
		ts_dev->deltax = ts_dev->x.max - ts_dev->x.min + 1;
	}

	if (*y < ts_dev->y.min) {
		ts_dev->y.min = *y;
		ts_dev->deltay = ts_dev->y.max - ts_dev->y.min + 1;
	} else if (*y > ts_dev->y.max) {
		ts_dev->y.max = *y;
		ts_dev->deltay = ts_dev->y.max - ts_dev->y.min + 1;
	}

	dev_dbg(ts_dev->mfd_tscadc->dev, "X %u < %u < %u, Y %u < %u < %u\n",
				ts_dev->x.min, *x, ts_dev->x.max,
				ts_dev->y.min, *y, ts_dev->y.max);

	*x = ((*x - ts_dev->x.min) << 12) / ts_dev->deltax;
	*y = ((*y - ts_dev->y.min) << 12) / ts_dev->deltay;
}

// ref tslib/plugins/linear.c function linear_read
static void tscadc_externcalibrate(struct tscadc *ts_dev, unsigned int *x, unsigned int *y) {
	unsigned int xtemp = *x;
	unsigned int ytemp = *y;

	*x = (ts_dev->a[0] * xtemp + ts_dev->a[1] * ytemp + ts_dev->a[2]) / ts_dev->a[6];
	*y = (ts_dev->a[3] * xtemp + ts_dev->a[4] * ytemp + ts_dev->a[5]) / ts_dev->a[6];
}

static irqreturn_t tscadc_interrupt(int irq, void *dev)
{
	struct tscadc		*ts_dev = (struct tscadc *)dev;
	struct input_dev	*input_dev = ts_dev->input;
	unsigned int		status, irqclr = 0;
	int			i;
	int			fsm = 0;
	unsigned int		readx1 = 0, ready1 = 0;
	unsigned int		prev_val_x = ~0, prev_val_y = ~0;
	unsigned int		prev_diff_x = ~0, prev_diff_y = ~0;
	unsigned int		cur_diff_x = 0, cur_diff_y = 0;
	unsigned int		val_x = 0, val_y = 0, diffx = 0, diffy = 0;
	unsigned int		z1 = 0, z2 = 0, z = 0;
	unsigned int		channel;

	status = tscadc_readl(ts_dev, TSCADC_REG_IRQSTATUS);

	if (status & TSCADC_IRQENB_FIFO0THRES) {
		for (i = 0; i < ts_dev->steps_to_config; i++) {
			readx1 = tscadc_readl(ts_dev, TSCADC_REG_FIFO0);
			channel = readx1 & 0xf0000;
			channel = channel >> 0x10;
			if ((channel >= 0) &&
				(channel < ts_dev->steps_to_config)) {
				readx1 = readx1 & 0xfff;

				if (ts_dev->x.inverted) {
					if (ts_dev->calibration_type != TSC_CALIBRATION_FIXED)
						readx1 ^= 0xfff;
					else
						readx1 = ts_dev->x.max - readx1 + ts_dev->x.min;
				}

				if (readx1 > prev_val_x)
					cur_diff_x = readx1 - prev_val_x;
				else
					cur_diff_x = prev_val_x - readx1;

				if (cur_diff_x < prev_diff_x) {
					prev_diff_x = cur_diff_x;
					val_x = readx1;
				}
				prev_val_x = readx1;
			}
		}
		for (i = 0; i < ts_dev->steps_to_config; i++) {
			ready1 = tscadc_readl(ts_dev, TSCADC_REG_FIFO0);
			channel = ready1 & 0xf0000;
			channel = channel >> 0x10;
			if ((channel >= ts_dev->steps_to_config) &&
				(channel < (2 * ts_dev->steps_to_config - 1))) {
				ready1 &= 0xfff;

				if (ts_dev->y.inverted) {
					if (ts_dev->calibration_type != TSC_CALIBRATION_FIXED)
						ready1 ^= 0xfff;
					else
						ready1 = ts_dev->y.max - ready1 + ts_dev->y.min;
				}

				if (ready1 > prev_val_y)
					cur_diff_y = ready1 - prev_val_y;
				else
					cur_diff_y = prev_val_y - ready1;

				if (cur_diff_y < prev_diff_y) {
					prev_diff_y = cur_diff_y;
					val_y = ready1;
				}
				prev_val_y = ready1;
			}
		}

		if (val_x > bckup_x) {
			diffx = val_x - bckup_x;
			diffy = val_y - bckup_y;
		} else {
			diffx = bckup_x - val_x;
			diffy = bckup_y - val_y;
		}
		bckup_x = val_x;
		bckup_y = val_y;

		z1 = ((tscadc_readl(ts_dev, TSCADC_REG_FIFO0)) & 0xfff);
		z2 = ((tscadc_readl(ts_dev, TSCADC_REG_FIFO0)) & 0xfff);

		if (z2 < z1) {
			/* z2 must be greater than z1 */
			unsigned int tmp = z2;
			z2 = z1;
			z1 = tmp;
		}

		if ((pen == 0) && (z1 != 0) && (z2 != 0)) {
			/*
			 * Calculate pressure using formula
			 * Resistance(touch) = x plate resistance *
			 * x postion/4096 * ((z2 / z1) - 1)
			 */
			z = z2 - z1;
			z *= val_x;
			z *= ts_dev->x_plate_resistance;
			z /= z1;
			z = (z + 2047) >> 12;

			if (z <= MAX_12BIT) {
				if (ts_dev->calibration_type == TSC_CALIBRATION_AUTO)
					tscadc_autocalibrate(ts_dev, &val_x, &val_y);
				else if (ts_dev->calibration_type == TSC_CALIBRATION_EXTERN)
					tscadc_externcalibrate(ts_dev, &val_x, &val_y);
				input_report_abs(input_dev, ABS_X, val_x);
				input_report_abs(input_dev, ABS_Y, val_y);
				input_report_abs(input_dev, ABS_PRESSURE, z);
				input_report_key(input_dev, BTN_TOUCH, 1);
				input_sync(input_dev);
			}
		}
		irqclr |= TSCADC_IRQENB_FIFO0THRES;
	}

	udelay(315);

	status = tscadc_readl(ts_dev, TSCADC_REG_RAWIRQSTATUS);
	if (status & TSCADC_IRQENB_PENUP) {
		/* Pen up event */
		fsm = tscadc_readl(ts_dev, TSCADC_REG_ADCFSM);
		if (fsm == 0x10) {
			pen = 1;
			bckup_x = 0;
			bckup_y = 0;
			input_report_key(input_dev, BTN_TOUCH, 0);
			input_report_abs(input_dev, ABS_PRESSURE, 0);
			input_sync(input_dev);
		} else {
			pen = 0;
		}
		irqclr |= TSCADC_IRQENB_PENUP;
	}

	if (status & TSCADC_IRQENB_HW_PEN) {
		irqclr |= TSCADC_IRQENB_HW_PEN;
	}

	if (irqclr) {
		tscadc_writel(ts_dev, TSCADC_REG_IRQSTATUS, irqclr);
		tscadc_writel(ts_dev, TSCADC_REG_SE, TSCADC_STPENB_STEPENB_TC);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static ssize_t tscadc_calibration_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct ti_tscadc_dev	*tscadc_dev = dev->platform_data;
	struct tscadc		*ts_dev = tscadc_dev->tsc;

	if (ts_dev->calibration_type != TSC_CALIBRATION_EXTERN)
		return sprintf(buf, "%d %d %d %d %s\n",
				    ts_dev->x.min,
				    ts_dev->x.max,
				    ts_dev->y.min,
				    ts_dev->y.max,
				    (ts_dev->calibration_type == TSC_CALIBRATION_FIXED ? "fixed" : "auto"));

	return sprintf(buf, "%d %d %d %d %d %d %d\n",
			    ts_dev->a[0],
			    ts_dev->a[1],
			    ts_dev->a[2],
			    ts_dev->a[3],
			    ts_dev->a[4],
			    ts_dev->a[5],
			    ts_dev->a[6]);
}

static ssize_t tscadc_calibration_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct ti_tscadc_dev	*tscadc_dev = dev->platform_data;
	struct tscadc		*ts_dev = tscadc_dev->tsc;
	int			res, a[7];

	if (ts_dev->calibration_type != TSC_CALIBRATION_EXTERN) {
		dev_err(dev, "setting calibration not allowed in %s mode\n", (ts_dev->calibration_type == TSC_CALIBRATION_FIXED ? "fixed" : "auto"));
		return -EINVAL;
	}

	res = sscanf(buf, "%d %d %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]);
	if (res != 7) {
		dev_err(dev, "setting calibration in extern mode failed: %d parameters\n", res);
		return -EINVAL;
	} else if (!a[6]) {
		dev_err(dev, "setting calibration in extern mode failed: division by zero\n");
		return -EINVAL;
	}
	for (res = 0; res < 7; res++)
		ts_dev->a[res] = a[res];

	return count;
}

static DEVICE_ATTR(calibration, 0666, tscadc_calibration_show, tscadc_calibration_store);

static struct attribute *tscadc_attributes[] = {
	&dev_attr_calibration.attr,
	NULL
};

static const struct attribute_group tscadc_attr_group = {
	.attrs = tscadc_attributes,
};

/*
* The functions for inserting/removing driver as a module.
*/

static	int __devinit tscadc_probe(struct platform_device *pdev)
{
	struct tscadc			*ts_dev;
	struct input_dev		*input_dev;
	int				err;
	int				irqenable;
	struct ti_tscadc_dev		*tscadc_dev = pdev->dev.platform_data;
	struct mfd_tscadc_board		*pdata;

	pdata = (struct mfd_tscadc_board *)tscadc_dev->dev->platform_data;
	if (!pdata) {
		dev_err(tscadc_dev->dev, "Could not find platform data\n");
		return -EINVAL;
	}

	/* Allocate memory for device */
	ts_dev = kzalloc(sizeof(struct tscadc), GFP_KERNEL);
	if (!ts_dev) {
		dev_err(&pdev->dev, "failed to allocate memory.\n");
		return -ENOMEM;
	}

	tscadc_dev->tsc = ts_dev;
	ts_dev->mfd_tscadc = tscadc_dev;

	input_dev = input_allocate_device();
	if (!input_dev) {
		dev_err(&pdev->dev, "failed to allocate input device.\n");
		err = -ENOMEM;
		goto err_free_mem;
	}
	ts_dev->input = input_dev;

	ts_dev->irq = tscadc_dev->irq;
	err = request_irq(ts_dev->irq, tscadc_interrupt, IRQF_SHARED,
				pdev->dev.driver->name, ts_dev);
	if (err) {
		dev_err(&pdev->dev, "failed to allocate irq.\n");
		goto err_fail;
	}

	ts_dev->wires = pdata->tsc_init->wires;
	ts_dev->x_plate_resistance = pdata->tsc_init->x_plate_resistance;
	ts_dev->steps_to_config = pdata->tsc_init->steps_to_configure;
	ts_dev->x.min = pdata->tsc_init->x.min;
	ts_dev->x.max = pdata->tsc_init->x.max;
	ts_dev->x.inverted = pdata->tsc_init->x.inverted;
	ts_dev->y.min = pdata->tsc_init->y.min;
	ts_dev->y.max = pdata->tsc_init->y.max;
	ts_dev->y.inverted = pdata->tsc_init->y.inverted;
	ts_dev->calibration_type = pdata->tsc_init->calibration_type;

	/* IRQ Enable */
	irqenable = TSCADC_IRQENB_FIFO0THRES | TSCADC_IRQENB_FIFO0OVRRUN |
		TSCADC_IRQENB_FIFO0UNDRFLW;
	tscadc_writel(ts_dev, TSCADC_REG_IRQENABLE, irqenable);

	tsc_step_config(ts_dev);

	tscadc_writel(ts_dev, TSCADC_REG_FIFO0THR,
			ts_dev->steps_to_config * 2 + 1);

	tscadc_writel(ts_dev, TSCADC_REG_CLKDIV, TSCADC_CLKDIV_DEFAULT);

	input_dev->name = "ti-tsc";
	input_dev->dev.parent = &pdev->dev;

	input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);

	if (ts_dev->calibration_type != TSC_CALIBRATION_FIXED) {
		input_set_abs_params(input_dev, ABS_X, 0, MAX_12BIT, 0, 0);
		input_set_abs_params(input_dev, ABS_Y, 0, MAX_12BIT, 0, 0);
	} else {
		input_set_abs_params(input_dev, ABS_X,
				ts_dev->x.min ? : 0,
				ts_dev->x.max ? : MAX_12BIT,
				0, 0);
		input_set_abs_params(input_dev, ABS_Y,
				ts_dev->y.min ? : 0,
				ts_dev->y.max ? : MAX_12BIT,
				0, 0);
	}
	input_set_abs_params(input_dev, ABS_PRESSURE, 0, MAX_12BIT, 0, 0);

	if (ts_dev->calibration_type == TSC_CALIBRATION_EXTERN) {
		// ref tslib/plugins/linear.c function linear_mod_init
		// {1, 0, 0, 0, 1, 0, 1};
		ts_dev->a[0] = ts_dev->a[4] = ts_dev->a[6] = 1;
		ts_dev->a[1] = ts_dev->a[2] = ts_dev->a[3] = ts_dev->a[5] = 0;
	} else {
		ts_dev->deltax = ts_dev->x.max - ts_dev->x.min;
		ts_dev->deltay = ts_dev->y.max - ts_dev->y.min;
	}

	err = sysfs_create_group(&pdev->dev.kobj, &tscadc_attr_group);
	if (err) {
		dev_err(&pdev->dev, "create device file failed!\n");
		err = -EINVAL;
		goto err_free_irq;
	}

	/* register to the input system */
	err = input_register_device(input_dev);
	if (err)
		goto err_remove_attr;

	platform_set_drvdata(pdev, ts_dev);
	return 0;

err_remove_attr:
	sysfs_remove_group(&pdev->dev.kobj, &tscadc_attr_group);
err_free_irq:
	free_irq(ts_dev->irq, ts_dev);
err_fail:
	input_free_device(ts_dev->input);
err_free_mem:
	platform_set_drvdata(pdev, NULL);
	kfree(ts_dev);
	return err;
}

static int __devexit tscadc_remove(struct platform_device *pdev)
{
	struct ti_tscadc_dev	*tscadc_dev = pdev->dev.platform_data;
	struct tscadc		*ts_dev = tscadc_dev->tsc;

	sysfs_remove_group(&pdev->dev.kobj, &tscadc_attr_group);
	free_irq(ts_dev->irq, ts_dev);

	input_unregister_device(ts_dev->input);
	kfree(ts_dev);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static int tsc_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct ti_tscadc_dev	*tscadc_dev = pdev->dev.platform_data;
	struct tscadc		*ts_dev = tscadc_dev->tsc;
	unsigned int idle;

	if (device_may_wakeup(tscadc_dev->dev)) {
		idle = tscadc_readl(ts_dev, TSCADC_REG_IRQENABLE);
		tscadc_writel(ts_dev, TSCADC_REG_IRQENABLE,
				(idle | TSCADC_IRQENB_HW_PEN));
		tscadc_writel(ts_dev, TSCADC_REG_IRQWAKEUP,
				TSCADC_IRQWKUP_ENB);
	}
	return 0;
}

static int tsc_resume(struct platform_device *pdev)
{
	struct ti_tscadc_dev	*tscadc_dev = pdev->dev.platform_data;
	struct tscadc		*ts_dev = tscadc_dev->tsc;

	if (device_may_wakeup(tscadc_dev->dev)) {
		tscadc_writel(ts_dev, TSCADC_REG_IRQWAKEUP,
				0x00);
		tscadc_writel(ts_dev, TSCADC_REG_IRQCLR,
				TSCADC_IRQENB_HW_PEN);
	}
	tsc_step_config(ts_dev);
	/* Configure to value minus 1 */
	tscadc_writel(ts_dev, TSCADC_REG_FIFO0THR,
			ts_dev->steps_to_config * 2 + 1);

	tscadc_writel(ts_dev, TSCADC_REG_CLKDIV, TSCADC_CLKDIV_DEFAULT);

	return 0;
}

static struct platform_driver ti_tsc_driver = {
	.probe	  = tscadc_probe,
	.remove	 = __devexit_p(tscadc_remove),
	.driver	 = {
		.name   = "tsc",
		.owner  = THIS_MODULE,
	},
	.suspend = tsc_suspend,
	.resume = tsc_resume,
};

static int __init ti_tsc_init(void)
{
	return platform_driver_register(&ti_tsc_driver);
}
module_init(ti_tsc_init);

static void __exit ti_tsc_exit(void)
{
	platform_driver_unregister(&ti_tsc_driver);
}
module_exit(ti_tsc_exit);

MODULE_DESCRIPTION("TI touchscreen controller driver");
MODULE_AUTHOR("Rachna Patil <rachna@ti.com>");
MODULE_LICENSE("GPL");
