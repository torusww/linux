/*
 * Codec driver for Multiple Purpose
 * Copyright (c) 2019 __tkz__ <tkz@lrclk.com>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 */

#define DEBUG

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>

#include <sound/soc.h>

struct nullcodec_priv {
	struct device *dev;
	struct gpio_desc *reset_gpiod;
	struct gpio_desc *mute_gpiod;
	struct gpio_desc *x4_gpiod;

	int mute;
	unsigned long mute_prewait;
	unsigned long mute_postwait;
	unsigned long reset_holdtime;
	unsigned long x4_freq;
};

static int nullcodec_set_dai_mute(struct snd_soc_dai *dai, int mute)
{
	struct snd_soc_codec *codec = dai->codec;
	struct nullcodec_priv *priv = snd_soc_codec_get_drvdata(codec);

	dev_dbg(codec->dev, "%s mute = %d\n", __FUNCTION__, mute);

	/* state check */
	if ( priv->mute == mute ) {
		return 0;
	}
	priv->mute = mute;

	if (mute) {
		if (priv->mute_gpiod) {
			if (priv->mute_prewait) mdelay(priv->mute_prewait);
			gpiod_set_value_cansleep(priv->mute_gpiod, 1);
			if (priv->mute_postwait) mdelay(priv->mute_postwait);
		}
	} else {
		if (priv->mute_gpiod) {
			if (priv->mute_prewait) mdelay(priv->mute_prewait);
			gpiod_set_value_cansleep(priv->mute_gpiod, 0);
			if (priv->mute_postwait) mdelay(priv->mute_postwait);
		}
	}

	return 0;
}

static int nullcodec_startup(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct nullcodec_priv *priv = snd_soc_codec_get_drvdata(codec);

	if (priv->reset_gpiod) {
		gpiod_set_value_cansleep(priv->reset_gpiod, 1);
		mdelay(priv->reset_holdtime);
		gpiod_set_value_cansleep(priv->reset_gpiod, 0);
	}

	return 0;
}

static int nullcodec_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params,
		struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct nullcodec_priv *priv = snd_soc_codec_get_drvdata(codec);

	if (priv->x4_gpiod) {
		gpiod_set_value_cansleep(priv->x4_gpiod, params_rate(params) >= priv->x4_freq);
	}

	return 0;
}


static struct snd_soc_dai_ops nullcodec_dai_ops = {
	.startup        = nullcodec_startup,
	.hw_params      = nullcodec_hw_params,
	//        .set_fmt        = nullcodec_set_dai_fmt,
	.digital_mute   = nullcodec_set_dai_mute,
};


static struct snd_soc_dai_driver nullcodec_dai = {
	.name = "nullcodec",
	.playback = {
		.channels_min = 2,
		.channels_max = 2,
		.rates =	SNDRV_PCM_RATE_CONTINUOUS,
		.rate_min =	8000,
		.rate_max =	1536000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE |
			SNDRV_PCM_FMTBIT_S24_LE |
			SNDRV_PCM_FMTBIT_S32_LE
	},
	.ops = &nullcodec_dai_ops,
};

static struct snd_soc_codec_driver soc_codec_dev_nullcodec;

static int nullcodec_probe(struct platform_device *pdev)
{
	struct nullcodec_priv *priv;
	unsigned int bits_array[8], val, fmt;
	int num, i, ret;
	dev_dbg(&pdev->dev, "%s\n", __FUNCTION__);

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv){
		dev_err(&pdev->dev, "nomem");
		return -ENOMEM;
	}

	// setup gpios
	priv->reset_gpiod = devm_gpiod_get_optional(&pdev->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(priv->reset_gpiod)){
		dev_err(&pdev->dev, "error requesting reset-gpios: %ld\n", PTR_ERR(priv->reset_gpiod));
		return PTR_ERR(priv->reset_gpiod);
	}

	priv->mute_gpiod = devm_gpiod_get_optional(&pdev->dev, "mute", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->mute_gpiod)) {
		dev_err(&pdev->dev, "error requesting mute-gpios: %ld\n", PTR_ERR(priv->mute_gpiod));
		return PTR_ERR(priv->mute_gpiod);
	}

	priv->x4_gpiod = devm_gpiod_get_optional(&pdev->dev, "x4", GPIOD_OUT_LOW);
	if (IS_ERR(priv->x4_gpiod)) {
		dev_err(&pdev->dev, "error requesting x4-gpios: %ld\n", PTR_ERR(priv->x4_gpiod));
		return PTR_ERR(priv->x4_gpiod);
	}


	// setup parameters
	ret = of_property_read_u32(pdev->dev.of_node, "max_rate", &val);
	if (ret == 0){
		dev_info(&pdev->dev, "override max_rate. set to %d", val);
		nullcodec_dai.playback.rate_max = val;
	}
	ret = of_property_read_u32(pdev->dev.of_node, "min_rate", &val);
	if (ret == 0){
		dev_info(&pdev->dev, "override min_rate. set to %d", val);
		nullcodec_dai.playback.rate_min = val;
	}

	num = of_property_read_variable_u32_array(pdev->dev.of_node, "bits", bits_array, 0, 8);
	if ( num < 0 ) dev_err(&pdev->dev, "device-tree [bits] field error.");
	if ( num > 0 ) {
		fmt = 0;
		for ( i = 0; i < num; i++ ) {
			switch(bits_array[i]) {
				case 16:
					fmt |= SNDRV_PCM_FMTBIT_S16_LE;
					break;
				case 24:
					fmt |= SNDRV_PCM_FMTBIT_S24_LE;
					break;
				case 32:
					fmt |= SNDRV_PCM_FMTBIT_S32_LE;
					break;
				default:
					dev_warn(&pdev->dev, "unknown bit rate = %d", bits_array[i]);
					break;
			}
		}
		dev_info(&pdev->dev, "override formats. set to 0x%x", fmt);
		nullcodec_dai.playback.formats = fmt;
	}

	priv->mute = -1;
	ret = of_property_read_u32(pdev->dev.of_node, "mute_prewait", &val);
	if (ret == 0){
		dev_info(&pdev->dev, "mute pre wait set to %d ms", val);
		priv->mute_prewait = val;
	}
	ret = of_property_read_u32(pdev->dev.of_node, "mute_postwait", &val);
	if (ret == 0){
		dev_info(&pdev->dev, "mute post wait set to %d ms", val);
		priv->mute_postwait = val;
	}
	ret = of_property_read_u32(pdev->dev.of_node, "reset_holdtime", &val);
	if (ret == 0){
		dev_info(&pdev->dev, "reset hold time set to %d ms", val);
		priv->reset_holdtime = val;
	}

	priv->x4_freq = 100000;
	ret = of_property_read_u32(pdev->dev.of_node, "x4_freq", &val);
	if (ret == 0){
		dev_info(&pdev->dev, "override x4_freq. set to %d", val);
		priv->x4_freq = val;
	}

	dev_set_drvdata(&pdev->dev, priv);

	return snd_soc_register_codec(&pdev->dev, &soc_codec_dev_nullcodec,	&nullcodec_dai, 1);
}

static int nullcodec_remove(struct platform_device *pdev)
{
	snd_soc_unregister_codec(&pdev->dev);
	return 0;
}

static const struct of_device_id nullcodec_of_match[] = {
	{ .compatible = "nullcodec", },
	{ }
};
MODULE_DEVICE_TABLE(of, nullcodec_of_match);

static struct platform_driver nullcodec_driver = {
	.probe		= nullcodec_probe,
	.remove		= nullcodec_remove,
	.driver		= {
		.name	= "nullcodec",
		.of_match_table = nullcodec_of_match,
	},
};

module_platform_driver(nullcodec_driver);

MODULE_AUTHOR("__tkz__ <tkz@lrclk.com>");
MODULE_DESCRIPTION("ASoC Null Codec driver");
MODULE_LICENSE("GPL v2");

