/*
 * Audio driver for AK4490/4495/4497/4493 DAC
 * Copyright (c) 2018 __tkz__ <tkz@lrclk.com>
 *
 * based on code by Junichi Wakasugi/Mihai Serban (ak4458.c)
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 */


#ifndef _AK449X_H
#define _AK449X_H

#include <linux/regmap.h>

/* Settings */

#define AK449X_00_CONTROL1			0x00
#define AK449X_01_CONTROL2			0x01
#define AK449X_02_CONTROL3			0x02
#define AK449X_03_LCHATT			0x03
#define AK449X_04_RCHATT			0x04
#define AK449X_05_CONTROL4			0x05
#define AK449X_06_DSD1				0x06
#define AK449X_07_CONTROL5			0x07
#define AK449X_08_SOUND_CONTROL		0x08
#define AK449X_09_DSD2				0x09

// below ak4493/97 only
#define AK449X_0A_CONTROL6			0x0A
#define AK449X_0B_CONTROL7			0x0B
#define AK449X_0C_RESERVED			0x0C
#define AK449X_0D_RESERVED			0x0D
#define AK449X_0E_RESERVED			0x0E
#define AK449X_0F_RESERVED			0x0F
#define AK449X_10_RESERVED			0x10
#define AK449X_11_RESERVED			0x11
#define AK449X_12_RESERVED			0x12
#define AK449X_13_RESERVED			0x13
#define AK449X_14_RESERVED			0x14
#define AK449X_15_CONTROL8			0x15


/* Bitfield Definitions */
/* AK449X_00_CONTROL1 (0x00) Fields
 * Addr Register Name  D7     D6    D5    D4    D3    D2    D1    D0
 * 00H  Control 1      ACKS   0     0     0     DIF2  DIF1  DIF0  RSTN
 */

/* Digital Filter (SD, SLOW, SSLOW) */
#define AK449X_SD_MASK		GENMASK(5, 5)
#define AK449X_SLOW_MASK	GENMASK(0, 0)
#define AK449X_SSLOW_MASK	GENMASK(0, 0)

/* DIF2	1 0
 *  x	1 0 MSB justified  Figure 3 (default)
 *  x	1 1 I2S Compliment  Figure 4
 */
#define AK449X_DIF_SHIFT	1
#define AK449X_DIF_MASK		GENMASK(3, 1)

#define AK449X_DIF_16BIT_LSB	(0 << AK449X_DIF_SHIFT)
#define AK449X_DIF_24BIT_I2S	(3 << AK449X_DIF_SHIFT)
#define AK449X_DIF_24BIT_LSB	(4 << AK449X_DIF_SHIFT)
#define AK449X_DIF_32BIT_LSB	(5 << AK449X_DIF_SHIFT)
#define AK449X_DIF_32BIT_MSB	(6 << AK449X_DIF_SHIFT)
#define AK449X_DIF_32BIT_I2S	(7 << AK449X_DIF_SHIFT)

/* AK449X_00_CONTROL1 (0x00) D0 bit */
#define AK449X_RSTN_MASK	GENMASK(0, 0)
#define AK449X_RSTN		(0x1 << 0)


/* AK449X_02_CONTROL3 (0x02) */
#define AK449X_MONO_MASK	GENMASK(3,3)
#define AK449X_MONO		(0x1 << 3)

#define AK449X_SELLR_MASK	GENMASK(1,1)
#define AK449X_SELLR		(0x1 << 1)

#define AK449X_INV_MASK		GENMASK(7,6)
#define AK449X_INV_SHIFT	6

#define AK449X_GAINCNT_MASK	GENMASK(3,1)
#define AK449X_GAINCNT_SHIFT	1

/* DAC Digital attenuator transition time setting
 * Table 19
 * Mode	ATS1	ATS2	ATT speed
 * 0	0	0	4080/fs
 * 1	0	1	2040/fs
 * 2	1	0	510/fs
 * 3	1	1	255/fs
 * */
#define AK449X_ATS_SHIFT	6
#define AK449X_ATS_MASK		GENMASK(7, 6)

#endif /* _AK449X_H */

