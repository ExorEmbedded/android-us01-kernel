#ifndef __LINUX_I2C_TSC2004_H
#define __LINUX_I2C_TSC2004_H

/* linux/i2c/tsc2004.h */

#define TSC2004_FLAG_SWAPXY	(1 << 0)	/* swap axis */
#define TSC2004_FLAG_INVERTX	(1 << 1)	/* invert x values */
#define TSC2004_FLAG_INVERTY	(1 << 2)	/* invert y values */

struct tsc2004_platform_data {
	u16	x_plate_ohms;		/* must be non-zero value */
	s16	irq_gpio;		/* must be valid */
	u16	flags;			/* refer to above defines */
	u16	minx, maxx, miny, maxy;	/* optional calibration values */
};

#endif
