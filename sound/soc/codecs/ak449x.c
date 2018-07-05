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

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <sound/initval.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>

#include "ak449x.h"

#define AK449X_DEBUG

#ifdef AK449X_DEBUG
#define ak449x_debug(...) printk(__VA_ARGS__)
#else
#define	ak449x_debug(...)
#endif

/* AK449X Codec Private Data */
struct ak449x_priv {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *reset_gpiod;
	struct gpio_desc *mute_gpiod;
	int digfil;	/* SSLOW, SD, SLOW bits */
	int fs;		/* sampling rate */
	int fmt;
	int slots;
	int slot_width;
	int mute;

	int chip;
	int chmode; // bit1: mono, bit0:sellr
	int phase;
};

/* ak449x chip variant */
enum {
	AK449X_CHIP_AK4495 = 0, // default
	AK449X_CHIP_AK4490 = 1,
	AK449X_CHIP_AK4497 = 2,
	AK449X_CHIP_AK4493 = 3,
};
static const char * const ak449x_chipname[] = {
	"AK4495", "AK4490", "AK4497", "AK4493"
};

/* channel mode */
enum {
	AK449X_CHMODE_STEREO = 0, // default
	AK449X_CHMODE_STEREO_INVERT = 1,
	AK449X_CHMODE_MONO_LCH = 2,
	AK449X_CHMODE_MONO_RCH = 3,	
};

/* phase mode */
enum {
	AK449X_PHASE_LN_RN = 0, // LCH Non-invert, RCH Non-invert  default
	AK449X_PHASE_LN_RI = 1, // LCH Non-invert, RCH invert
	AK449X_PHASE_LI_RN = 2, // LCH invert, RCH Non-invert
	AK449X_PHASE_LI_RI = 3, // LCH invert, RCH invert
};

/* reg_default for ak4490/ak4495 */
static const struct reg_default ak4495_reg_defaults[] = {
	{ 0x00, 0x04 },	/*	0x00	AK449X_00_CONTROL1	*/
	{ 0x01, 0x22 },	/*	0x01	AK449X_01_CONTROL2	*/
	{ 0x02, 0x00 },	/*	0x02	AK449X_02_CONTROL3	*/
	{ 0x03, 0xFF },	/*	0x03	AK449X_03_LCHATT	*/
	{ 0x04, 0xFF },	/*	0x04	AK449X_04_RCHATT	*/
	{ 0x05, 0x00 },	/*	0x05	AK449X_05_CONTROL4	*/
	{ 0x06, 0x00 },	/*	0x06	AK449X_06_DSD1		*/
	{ 0x07, 0x00 },	/*	0x07	AK449X_07_CONTROL5	*/
	{ 0x08, 0x00 },	/*	0x08	AK449X_08_SOUND_CONTROL	*/
	{ 0x09, 0x00 },	/*	0x09	AK449X_09_DSD2		*/
};

/* reg_default for ak4493/ak4497 */
static const struct reg_default ak4497_reg_defaults[] = {
	{ 0x00, 0x0C },	/*	0x00	AK449X_00_CONTROL1	*/
	{ 0x01, 0x22 },	/*	0x01	AK449X_01_CONTROL2	*/
	{ 0x02, 0x00 },	/*	0x02	AK449X_02_CONTROL3	*/
	{ 0x03, 0xFF },	/*	0x03	AK449X_03_LCHATT	*/
	{ 0x04, 0xFF },	/*	0x04	AK449X_04_RCHATT	*/
	{ 0x05, 0x00 },	/*	0x05	AK449X_05_CONTROL4	*/
	{ 0x06, 0x00 },	/*	0x06	AK449X_06_DSD1		*/
	{ 0x07, 0x01 },	/*	0x07	AK449X_07_CONTROL5	*/
	{ 0x08, 0x00 },	/*	0x08	AK449X_08_SOUND_CONTROL	*/
	{ 0x09, 0x00 },	/*	0x09	AK449X_09_DSD2		*/
	{ 0x0A, 0x04 },	/*	0x0A	AK449X_0A_CONTROL6	*/
	{ 0x0B, 0x00 },	/*	0x0B	AK449X_0B_CONTROL7	*/
	{ 0x0C, 0x00 },	/*	0x0C	AK449X_0C_RESERVED	*/
	{ 0x0D, 0x00 },	/*	0x0D	AK449X_0D_RESERVED	*/
	{ 0x0E, 0x00 },	/*	0x0E	AK449X_0E_RESERVED	*/
	{ 0x0F, 0x00 },	/*	0x0F	AK449X_0F_RESERVED	*/
	{ 0x10, 0x00 },	/*	0x10	AK449X_10_RESERVED	*/
	{ 0x11, 0x00 },	/*	0x11	AK449X_11_RESERVED	*/
	{ 0x12, 0x00 },	/*	0x12	AK449X_12_RESERVED	*/
	{ 0x13, 0x00 },	/*	0x13	AK449X_13_RESERVED	*/
	{ 0x14, 0x00 },	/*	0x14	AK449X_14_RESERVED	*/
	{ 0x15, 0x00 },	/*	0x15	AK449X_15_CONTROL8	*/
};


/*
 * Volume control:
 * from -127 to 0 dB in 0.5 dB steps (mute instead of -127.5 dB)
 */
static DECLARE_TLV_DB_SCALE(dac_tlv, -12750, 50, 1);

/*
 * DEM1 bit DEM0 bit Mode
 * 0 0 44.1kHz
 * 0 1 OFF (default)
 * 1 0 48kHz
 * 1 1 32kHz
 */
static const char * const ak449x_dem_select_texts[] = {
	"44.1kHz", "OFF", "48kHz", "32kHz"
};

/*
 * SSLOW, SD, SLOW bits Digital Filter Setting
 * 0, 0, 0 : Sharp Roll-Off Filter
 * 0, 0, 1 : Slow Roll-Off Filter
 * 0, 1, 0 : Short delay Sharp Roll-Off Filter
 * 0, 1, 1 : Short delay Slow Roll-Off Filter
 * 1, *, * : Super Slow Roll-Off Filter(NOS)
 */
static const char * const ak449x_digfil_select_texts[] = {
	"Sharp Roll-Off Filter",
	"Slow Roll-Off Filter",
	"Short delay Sharp Roll-Off Filter",
	"Short delay Slow Roll-Off Filter",
	"Super Slow Roll-Off Filter(NOS)"
};

/*
 * DZFB: Inverting Enable of DZF
 * 0: DZF goes H at Zero Detection
 * 1: DZF goes L at Zero Detection
 */
static const char * const ak449x_dzfb_select_texts[] = {"H", "L"};

/*
 * SC2-0 bits: Sound Mode Setting
 * ak4490 0-2
 * ak4495 0-5
 * ak4497 0-2,4?
 * ak4493 0-5?
 */
static const char * const ak4490_sc_select_texts[] = {
	"Mode1", "Mode2", "Mode3"
};

static const char * const ak4495_sc_select_texts[] = {
	"Mode1", "Mode2", "Mode3", "Mode4", "Mode5"
};

static const char * const ak4497_sc_select_texts[] = {
	"Setting1/4", "Setting2/4", "Setting3/4", "Setting2/4", "Setting1/5", "Setting2/5", "Setting3/5"
};

static const char * const ak4493_sc_select_texts[] = {
	"Setting1/5", "Setting2/5", "Setting3/5", "Setting4/5", "Setting1/6", "Setting2/6", "Setting3/6", "Setting4/6"
};


/*
 * HLOAD: Heavy Load Mode Enable (ak4497 only)
 * 0: Disable (default)
 * 1: Enable
 */
static const char * const ak4497_hload_select_texts[] = {"OFF", "ON"};


/*
 * GC2-0 bits: Gain Control
 *           PCM    DSD    DSD BP
 * 0, 0, 0 : 2.8Vpp 2.6Vpp 2.5Vpp
 * 0, 0, 1 : 2.8Vpp 2.5Vpp 2.5Vpp
 * 0, 1, 0 : 2.5Vpp 2.5Vpp 2.5Vpp
 * 0, 1, 1 : 2.5Vpp 2.5Vpp 2.5Vpp Dup
 * 1, 0, 0 : 3.75Vpp 3.75Vpp 2.5Vpp
 * 1, 0, 1 : 3.75Vpp 2.5Vpp 2.5Vpp
 * 1, 1, 0 : 2.8Vpp 2.5Vpp 2.5Vpp Dup
 * 1, 1, 1 : 2.8Vpp 2.5Vpp 2.5Vpp Dup
 */

static const char * const ak449x_gc_select_texts[] = {
	"PCM/DSD2.8Vpp/DSDBP2.5Vpp",
	"PCM2.8Vpp/DSD2.5Vpp",
	"ALL2.5Vpp",
	"ALL2.5Vpp",
	"PCM/DSD3.75Vpp/DSDBP2.5Vpp",
	"PCM3.75Vpp/DSD2.5Vpp",
};



// generic control
static const struct soc_enum ak449x_dem_enum =
	SOC_ENUM_SINGLE(AK449X_01_CONTROL2, 1,
			ARRAY_SIZE(ak449x_dem_select_texts),
			ak449x_dem_select_texts);

static const struct soc_enum ak449x_digfil_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(ak449x_digfil_select_texts),
			    ak449x_digfil_select_texts);

static const struct soc_enum ak449x_dzfb_enum =
	SOC_ENUM_SINGLE(AK449X_02_CONTROL3, 2,
			ARRAY_SIZE(ak449x_dzfb_select_texts),
			ak449x_dzfb_select_texts);

// chip specific control
static const struct soc_enum ak4490_sm_enum =
	SOC_ENUM_SINGLE(AK449X_08_SOUND_CONTROL, 0,
			ARRAY_SIZE(ak4490_sc_select_texts),
			ak4490_sc_select_texts);

static const struct soc_enum ak4495_sm_enum =
	SOC_ENUM_SINGLE(AK449X_08_SOUND_CONTROL, 0,
			ARRAY_SIZE(ak4495_sc_select_texts),
			ak4495_sc_select_texts);

static const struct soc_enum ak4497_sm_enum =
	SOC_ENUM_SINGLE(AK449X_08_SOUND_CONTROL, 0,
			ARRAY_SIZE(ak4497_sc_select_texts),
			ak4497_sc_select_texts);

static const struct soc_enum ak4493_sm_enum =
	SOC_ENUM_SINGLE(AK449X_08_SOUND_CONTROL, 0,
			ARRAY_SIZE(ak4493_sc_select_texts),
			ak4493_sc_select_texts);


static const struct soc_enum ak4497_hload_enum =
	SOC_ENUM_SINGLE(AK449X_08_SOUND_CONTROL, 3,
			ARRAY_SIZE(ak4497_hload_select_texts),
			ak4497_hload_select_texts);

static const struct soc_enum ak449x_gc_enum =
	SOC_ENUM_SINGLE(AK449X_07_CONTROL5, 1,
			ARRAY_SIZE(ak449x_gc_select_texts),
			ak449x_gc_select_texts);



static int get_digfil(struct snd_kcontrol *kcontrol,
		      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct ak449x_priv *ak449x = snd_soc_codec_get_drvdata(codec);

	ucontrol->value.enumerated.item[0] = ak449x->digfil;

	return 0;
}

static int set_digfil(struct snd_kcontrol *kcontrol,
		      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct ak449x_priv *ak449x = snd_soc_codec_get_drvdata(codec);
	int num;

	num = ucontrol->value.enumerated.item[0];
	if (num > 4)
		return -EINVAL;

	ak449x->digfil = num;

	/* write SD bit */
	snd_soc_update_bits(codec, AK449X_01_CONTROL2,
			    AK449X_SD_MASK,
			    ((ak449x->digfil & 0x02) << 4));

	/* write SLOW bit */
	snd_soc_update_bits(codec, AK449X_02_CONTROL3,
			    AK449X_SLOW_MASK,
			    (ak449x->digfil & 0x01));

	/* write SSLOW bit */
	snd_soc_update_bits(codec, AK449X_05_CONTROL4,
			    AK449X_SSLOW_MASK,
			    ((ak449x->digfil & 0x04) >> 2));

	return 0;
}

static const struct snd_kcontrol_new ak4490_snd_controls[] = {
	SOC_DOUBLE_R_TLV("Volume", AK449X_03_LCHATT, AK449X_04_RCHATT, 0, 0xFF, 0, dac_tlv),
	SOC_ENUM("De-emphasis Response", ak449x_dem_enum),
	SOC_ENUM_EXT("Digital Filter Setting", ak449x_digfil_enum, get_digfil, set_digfil),
//	SOC_ENUM("AK449X Inverting Enable of DZFB", ak449x_dzfb_enum),
	SOC_ENUM("Sound Mode", ak4490_sm_enum),
};
static const struct snd_kcontrol_new ak4495_snd_controls[] = {
	SOC_DOUBLE_R_TLV("Digital Volume", AK449X_03_LCHATT, AK449X_04_RCHATT, 0, 0xFF, 0, dac_tlv),
	SOC_ENUM("De-emphasis Response", ak449x_dem_enum),
	SOC_ENUM_EXT("Digital Filter Setting", ak449x_digfil_enum, get_digfil, set_digfil),
//	SOC_ENUM("AK449X Inverting Enable of DZFB", ak449x_dzfb_enum),
	SOC_ENUM("Sound Mode", ak4495_sm_enum),
};
static const struct snd_kcontrol_new ak4497_snd_controls[] = {
	SOC_DOUBLE_R_TLV("Digital Volume", AK449X_03_LCHATT, AK449X_04_RCHATT, 0, 0xFF, 0, dac_tlv),
	SOC_ENUM("De-emphasis Response", ak449x_dem_enum),
	SOC_ENUM_EXT("Digital Filter Setting", ak449x_digfil_enum, get_digfil, set_digfil),
//	SOC_ENUM("AK449X Inverting Enable of DZFB", ak449x_dzfb_enum),
	SOC_ENUM("Sound Mode", ak4497_sm_enum),
	SOC_ENUM("Gain Control", ak449x_gc_enum),
	SOC_ENUM("AK449X HLOAD", ak4497_hload_enum),
};
static const struct snd_kcontrol_new ak4493_snd_controls[] = {
	SOC_DOUBLE_R_TLV("Digital Volume", AK449X_03_LCHATT, AK449X_04_RCHATT, 0, 0xFF, 0, dac_tlv),
	SOC_ENUM("De-emphasis Response", ak449x_dem_enum),
	SOC_ENUM_EXT("Digital Filter Setting", ak449x_digfil_enum, get_digfil, set_digfil),
//	SOC_ENUM("AK449X Inverting Enable of DZFB", ak449x_dzfb_enum),
	SOC_ENUM("Sound Mode", ak4493_sm_enum),
	SOC_ENUM("Gain Control", ak449x_gc_enum),
};


/* ak449x dapm widgets */
static const struct snd_soc_dapm_widget ak449x_dapm_widgets[] = {
	SND_SOC_DAPM_DAC("DACL", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_DAC("DACR", NULL, SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_OUTPUT("OUTL"),
	SND_SOC_DAPM_OUTPUT("OUTR"),
};

static const struct snd_soc_dapm_route ak449x_intercon[] = {
	{ "DACL", NULL, "Playback" },
	{ "DACR", NULL, "Playback" },

	{ "OUTL", NULL, "DACL" },
	{ "OUTR", NULL, "DACR" },
};

static int ak449x_rstn_control(struct snd_soc_codec *codec, int bit)
{
	int ret;

	if (bit)
		ret = snd_soc_update_bits(codec,
					  AK449X_00_CONTROL1,
					  AK449X_RSTN_MASK,
					  0x1);
	else
		ret = snd_soc_update_bits(codec,
					  AK449X_00_CONTROL1,
					  AK449X_RSTN_MASK,
					  0x0);
	return ret;
}

static int ak449x_chmode_set(struct snd_soc_codec *codec)
{
	struct ak449x_priv *ak449x = snd_soc_codec_get_drvdata(codec);

	snd_soc_update_bits(codec, AK449X_02_CONTROL3, AK449X_MONO_MASK, ak449x->chmode << 2); /* ((chmod >> 1) << 3) & 0x8 */
	snd_soc_update_bits(codec, AK449X_02_CONTROL3, AK449X_SELLR_MASK, ak449x->chmode << 1);

	return 0;
}

static int ak449x_phase_set(struct snd_soc_codec *codec)
{
	struct ak449x_priv *ak449x = snd_soc_codec_get_drvdata(codec);
	int ret;

	ret = snd_soc_update_bits(codec, AK449X_05_CONTROL4, AK449X_INV_MASK, ak449x->phase << AK449X_INV_SHIFT);

	return ret;
}

static int ak449x_hw_params(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params,
			    struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct ak449x_priv *ak449x = snd_soc_codec_get_drvdata(codec);
	int pcm_width = max(params_physical_width(params), ak449x->slot_width);
	int nfs1;
	u8 format;

	ak449x_debug("%s\n", __FUNCTION__);

	nfs1 = params_rate(params);
	ak449x->fs = nfs1;

	/* Master Clock Frequency Auto Setting Mode Enable */
	snd_soc_update_bits(codec, AK449X_00_CONTROL1, 0x80, 0x80);

	switch (pcm_width) {
	case 16:
		if (ak449x->fmt == SND_SOC_DAIFMT_I2S)
			format = AK449X_DIF_24BIT_I2S;
		else
			format = AK449X_DIF_16BIT_LSB;
		break;
	case 24:
		switch (ak449x->fmt) {
		case SND_SOC_DAIFMT_I2S:
			format = AK449X_DIF_24BIT_I2S;
			break;
		case SND_SOC_DAIFMT_RIGHT_J:
			format = AK449X_DIF_24BIT_LSB;
			break;
		default:
			return -EINVAL;
		}
		break;
	case 32:
		switch (ak449x->fmt) {
		case SND_SOC_DAIFMT_I2S:
			format = AK449X_DIF_32BIT_I2S;
			break;
		case SND_SOC_DAIFMT_LEFT_J:
			format = AK449X_DIF_32BIT_MSB;
			break;
		case SND_SOC_DAIFMT_RIGHT_J:
			format = AK449X_DIF_32BIT_LSB;
			break;
		case SND_SOC_DAIFMT_DSP_B:
			format = AK449X_DIF_32BIT_MSB;
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	snd_soc_update_bits(codec, AK449X_00_CONTROL1, AK449X_DIF_MASK, format);
	ak449x_debug("%s set dif format = %x\n", __FUNCTION__, format);

	ak449x_rstn_control(codec, 0);
	ak449x_rstn_control(codec, 1);

	return 0;
}

static int ak449x_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_codec *codec = dai->codec;
	struct ak449x_priv *ak449x = snd_soc_codec_get_drvdata(codec);

	ak449x_debug("%s\n", __FUNCTION__);

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS: /* Slave Mode */
		break;
	case SND_SOC_DAIFMT_CBM_CFM: /* Master Mode is not supported */
	case SND_SOC_DAIFMT_CBS_CFM:
	case SND_SOC_DAIFMT_CBM_CFS:
	default:
		dev_err(codec->dev, "Master mode unsupported\n");
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_LEFT_J:
	case SND_SOC_DAIFMT_RIGHT_J:
	case SND_SOC_DAIFMT_DSP_B:
		ak449x->fmt = fmt & SND_SOC_DAIFMT_FORMAT_MASK;
		break;
	default:
		dev_err(codec->dev, "Audio format 0x%02X unsupported\n",
			fmt & SND_SOC_DAIFMT_FORMAT_MASK);
		return -EINVAL;
	}

	return 0;
}

static const int att_speed[] = { 4080, 2040, 510, 255 };

static int ak449x_set_dai_mute(struct snd_soc_dai *dai, int mute)
{
	struct snd_soc_codec *codec = dai->codec;
	struct ak449x_priv *ak449x = snd_soc_codec_get_drvdata(codec);
	int nfs, ndt, ret, reg;
	int ats;

	ak449x_debug("%s mute = %d\n", __FUNCTION__, mute);

	/* state check */
	if ( ak449x->mute == mute ) {
		return 0;
	}
	ak449x->mute = mute;

	/* calculate att transition time */
	nfs = ak449x->fs;
	if(ak449x->chip == AK449X_CHIP_AK4493 || ak449x->chip == AK449X_CHIP_AK4497) {
	       	// for AK4493/4497 variable rate
		reg = snd_soc_read(codec, AK449X_0B_CONTROL7);
		ats = (reg & AK449X_ATS_MASK) >> AK449X_ATS_SHIFT;

		ndt = att_speed[ats] / (nfs / 1000);
	}else{ // for ak4490/4495 fixed rate
		ndt = 7424 / (nfs / 1000);
	}

	if (mute) {
		ret = snd_soc_update_bits(codec, AK449X_01_CONTROL2, 0x01, 1);
		mdelay(ndt);
		if (ak449x->mute_gpiod)
			gpiod_set_value_cansleep(ak449x->mute_gpiod, 1);
	} else {
		if (ak449x->mute_gpiod)
			gpiod_set_value_cansleep(ak449x->mute_gpiod, 0);
		ret = snd_soc_update_bits(codec, AK449X_01_CONTROL2, 0x01, 0);
		mdelay(ndt);
	}

	return 0;
}

static void ak449x_parse_device_tree_options(struct device *dev, struct ak449x_priv *ak449x)
{
	int ret;
	ak449x_debug("%s\n", __FUNCTION__);

	/* get chip name */
	{
		const char *output;
		ret = of_property_read_string(dev->of_node, "chip", &output);
		if (ret == 0) {
			if (!strncmp(output, "AK4490", sizeof("AK4490"))) {
				ak449x->chip = AK449X_CHIP_AK4490;
			}else
			if (!strncmp(output, "AK4495", sizeof("AK4495"))) {
				ak449x->chip = AK449X_CHIP_AK4495;
			}else
			if (!strncmp(output, "AK4497", sizeof("AK4497"))) {
				ak449x->chip = AK449X_CHIP_AK4497;
			}else
			if (!strncmp(output, "AK4493", sizeof("AK4493"))) {
				ak449x->chip = AK449X_CHIP_AK4493;
			}
		}
		ak449x_debug("%s: ak449x->chip = %d\n", __FUNCTION__, ak449x->chip);
	}
	

	/* get mono/stereo mode */
	{
		const char *output;
		ret = of_property_read_string(dev->of_node, "chmode", &output);
		if (ret == 0) {
			if (!strncmp(output, "STEREO", sizeof("STEREO"))) {
				ak449x->chmode = AK449X_CHMODE_STEREO;
			}else
			if (!strncmp(output, "STEREO_INVERT", sizeof("STEREO_INVERT"))) {
				ak449x->chmode = AK449X_CHMODE_STEREO_INVERT;
			}else
			if (!strncmp(output, "MONO_LCH", sizeof("MONO_LCH"))) {
				ak449x->chmode = AK449X_CHMODE_MONO_LCH;
			}else
			if (!strncmp(output, "MONO_RCH", sizeof("MONO_RCH"))) {
				ak449x->chmode = AK449X_CHMODE_MONO_RCH;
			}
		}
		ak449x_debug("%s: ak449x->chmode = %d\n", __FUNCTION__, ak449x->chmode);
	}

	/* channel phase */
	{
		const char *output;
		ret = of_property_read_string(dev->of_node, "phase", &output);
		if (ret == 0) {
			if (!strncmp(output, "LNRN", sizeof("LNRN"))) {
				ak449x->phase = AK449X_PHASE_LN_RN;
			}else
			if (!strncmp(output, "LNRI", sizeof("LNRI"))) {
				ak449x->phase = AK449X_PHASE_LN_RI;
			}else
			if (!strncmp(output, "LIRN", sizeof("LIRN"))) {
				ak449x->phase = AK449X_PHASE_LI_RN;
			}else
			if (!strncmp(output, "LIRI", sizeof("LIRI"))) {
				ak449x->phase = AK449X_PHASE_LI_RI;
			}
		}
		ak449x_debug("%s: ak449x->phase = %d\n", __FUNCTION__, ak449x->phase);
	}
}


#define AK449X_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE |\
			 SNDRV_PCM_FMTBIT_S24_LE |\
			 SNDRV_PCM_FMTBIT_S32_LE)

static const unsigned int ak449x_rates[] = {
	8000, 11025,  16000, 22050,
	32000, 44100, 48000, 88200,
	96000, 176400, 192000, 352800,
	384000, 705600, 768000, 1411200,
	1536000,
};

static const struct snd_pcm_hw_constraint_list ak449x_rate_constraints = {
	.count = ARRAY_SIZE(ak449x_rates),
	.list = ak449x_rates,
};

static int ak449x_startup(struct snd_pcm_substream *substream,
			  struct snd_soc_dai *dai)
{
	int ret;

	ak449x_debug("%s\n", __FUNCTION__);
	ret = snd_pcm_hw_constraint_list(substream->runtime, 0,
					 SNDRV_PCM_HW_PARAM_RATE,
					 &ak449x_rate_constraints);

	return ret;
}

static struct snd_soc_dai_ops ak449x_dai_ops = {
	.startup        = ak449x_startup,
	.hw_params	= ak449x_hw_params,
	.set_fmt	= ak449x_set_dai_fmt,
	.digital_mute	= ak449x_set_dai_mute,
};

static struct snd_soc_dai_driver ak449x_dai = {
	.name = "ak449x-aif",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_KNOT,
		.formats = AK449X_FORMATS,
	},
	.ops = &ak449x_dai_ops,
};

static void ak449x_power_off(struct ak449x_priv *ak449x)
{
	if (ak449x->reset_gpiod) {
		ak449x_debug("%s\n", __FUNCTION__);
		gpiod_set_value_cansleep(ak449x->reset_gpiod, 1); // pdn enable
		usleep_range(1000, 2000);
	}
}

static void ak449x_power_on(struct ak449x_priv *ak449x)
{
	if (ak449x->reset_gpiod) {
		ak449x_debug("%s\n", __FUNCTION__);
		gpiod_set_value_cansleep(ak449x->reset_gpiod, 0); // pdn disable
		usleep_range(1000, 2000);
	}
}

static void ak449x_init(struct snd_soc_codec *codec)
{
	struct ak449x_priv *ak449x = snd_soc_codec_get_drvdata(codec);

	dev_info(codec->dev, "%s\n", __FUNCTION__);
	/* External Mute ON */
	if (ak449x->mute_gpiod)
		gpiod_set_value_cansleep(ak449x->mute_gpiod, 1);

	ak449x->mute = -1; /* reset mute state */

	ak449x_power_on(ak449x);

	snd_soc_update_bits(codec, AK449X_00_CONTROL1, 0x80, 0x80);   /* ACKS bit = 1; 10000000 */
	ak449x_chmode_set(codec);
	ak449x_phase_set(codec);
	ak449x_rstn_control(codec, 1);
}

static int ak449x_probe(struct snd_soc_codec *codec)
{
	struct ak449x_priv *ak449x = snd_soc_codec_get_drvdata(codec);

	dev_info(codec->dev, "%s\n", __FUNCTION__);
	ak449x_init(codec);

	ak449x->fs = 48000;

	return 0;
}

static int ak449x_remove(struct snd_soc_codec *codec)
{
	struct ak449x_priv *ak449x = snd_soc_codec_get_drvdata(codec);

	ak449x_debug("%s\n", __FUNCTION__);
	ak449x_power_off(ak449x);

	return 0;
}


#ifdef CONFIG_PM
static int __maybe_unused ak449x_runtime_suspend(struct device *dev)
{
	struct ak449x_priv *ak449x = dev_get_drvdata(dev);

	ak449x_debug("%s\n", __FUNCTION__);
	regcache_cache_only(ak449x->regmap, true);

	ak449x_power_off(ak449x);

	if (ak449x->mute_gpiod)
		gpiod_set_value_cansleep(ak449x->mute_gpiod, 0);

	return 0;
}

static int __maybe_unused ak449x_runtime_resume(struct device *dev)
{
	struct ak449x_priv *ak449x = dev_get_drvdata(dev);

	ak449x_debug("%s\n", __FUNCTION__);
	if (ak449x->mute_gpiod)
		gpiod_set_value_cansleep(ak449x->mute_gpiod, 1);

	ak449x_power_off(ak449x);
	ak449x_power_on(ak449x);

	regcache_cache_only(ak449x->regmap, false);
	regcache_mark_dirty(ak449x->regmap);

	return regcache_sync(ak449x->regmap);
}
#endif /* CONFIG_PM */

static struct snd_soc_codec_driver soc_codec_driver_ak449x = {
	.probe			= ak449x_probe,
	.remove			= ak449x_remove,

	.component_driver = {
		.controls		= ak4495_snd_controls,
		.num_controls		= ARRAY_SIZE(ak4495_snd_controls),
		.dapm_widgets		= ak449x_dapm_widgets,
		.num_dapm_widgets	= ARRAY_SIZE(ak449x_dapm_widgets),
		.dapm_routes		= ak449x_intercon,
		.num_dapm_routes	= ARRAY_SIZE(ak449x_intercon),
	},
};


static struct regmap_config ak449x_regmap = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = AK449X_09_DSD2,
	.reg_defaults = ak4495_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(ak4495_reg_defaults),
	.cache_type = REGCACHE_RBTREE,
};

static const struct dev_pm_ops ak449x_pm = {
	SET_RUNTIME_PM_OPS(ak449x_runtime_suspend, ak449x_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

static int ak449x_i2c_probe(struct i2c_client *i2c)
{
	struct ak449x_priv *ak449x;
	int ret;

	ak449x_debug("%s\n", __FUNCTION__);
	ak449x = devm_kzalloc(&i2c->dev, sizeof(*ak449x), GFP_KERNEL);
	if (!ak449x){
		dev_err(&i2c->dev, "nomem");
		return -ENOMEM;
	}

	ak449x_parse_device_tree_options(&i2c->dev, ak449x);

	// Control override
	if (ak449x->chip == AK449X_CHIP_AK4490) {
		soc_codec_driver_ak449x.component_driver.controls = ak4490_snd_controls;
		soc_codec_driver_ak449x.component_driver.num_controls = ARRAY_SIZE(ak4490_snd_controls);
	}else
	if (ak449x->chip == AK449X_CHIP_AK4497) {
		soc_codec_driver_ak449x.component_driver.controls = ak4497_snd_controls;
		soc_codec_driver_ak449x.component_driver.num_controls = ARRAY_SIZE(ak4497_snd_controls);
	}else
	if (ak449x->chip == AK449X_CHIP_AK4493) {
		soc_codec_driver_ak449x.component_driver.controls = ak4493_snd_controls;
		soc_codec_driver_ak449x.component_driver.num_controls = ARRAY_SIZE(ak4493_snd_controls);
	}

	// regmap override
	if (ak449x->chip == AK449X_CHIP_AK4497 || ak449x->chip == AK449X_CHIP_AK4493) {
		ak449x_regmap.max_register = AK449X_15_CONTROL8;
		ak449x_regmap.reg_defaults = ak4497_reg_defaults;
		ak449x_regmap.num_reg_defaults = ARRAY_SIZE(ak4497_reg_defaults);
	}

	// register i2c with regmap
	ak449x->regmap = devm_regmap_init_i2c(i2c, &ak449x_regmap);
	if (IS_ERR(ak449x->regmap))
		return PTR_ERR(ak449x->regmap);

	i2c_set_clientdata(i2c, ak449x);
	ak449x->dev = &i2c->dev;

	// setup gpios
	ak449x->reset_gpiod = devm_gpiod_get_optional(ak449x->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ak449x->reset_gpiod)){
		dev_err(ak449x->dev, "error requesting reset-gpios: %ld\n", PTR_ERR(ak449x->reset_gpiod));
		return PTR_ERR(ak449x->reset_gpiod);
	}

	ak449x->mute_gpiod = devm_gpiod_get_optional(ak449x->dev, "mute", GPIOD_OUT_LOW);
	if (IS_ERR(ak449x->mute_gpiod)) {
		dev_err(ak449x->dev, "error requesting mute-gpios: %ld\n", PTR_ERR(ak449x->mute_gpiod));
		return PTR_ERR(ak449x->mute_gpiod);
	}

	// register codec
	ret = snd_soc_register_codec(ak449x->dev, &soc_codec_driver_ak449x, &ak449x_dai, 1);
	if (ret < 0) {
		dev_err(ak449x->dev, "Failed to register CODEC: %d\n", ret);
		return ret;
	}

	pm_runtime_enable(&i2c->dev);

	dev_info(&i2c->dev, "registered ak449x. chip variant = %s", ak449x_chipname[ak449x->chip]);
	return 0;
}

static int ak449x_i2c_remove(struct i2c_client *i2c)
{
	pm_runtime_disable(&i2c->dev);

	return 0;
}

static const struct of_device_id ak449x_of_match[] = {
	{ .compatible = "asahi-kasei,ak449x", },
	{ },
};

static struct i2c_driver ak449x_i2c_driver = {
	.driver = {
		.name = "ak449x-codec",
		.pm = &ak449x_pm,
		.of_match_table = ak449x_of_match,
		},
	.probe_new = ak449x_i2c_probe,
	.remove = ak449x_i2c_remove,
};

module_i2c_driver(ak449x_i2c_driver);

MODULE_AUTHOR("__tkz__ <tkz@lrclk.com>");
MODULE_DESCRIPTION("ASoC AK449X DAC driver");
MODULE_LICENSE("GPL v2");

