/*
 * ASoC Driver for I2S Multi Function Dai Link
 *
 * Author:	__tkz__ <tkz@lrclk.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#define DEBUG

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>


#define CLK44_RATE 22579200UL
#define CLK48_RATE 24576000UL

#define CLK44_SELECT 0x01
#define CLK48_SELECT 0x02

struct i2smfdl_priv {
	struct device *dev;
	struct gpio_desc *clk44_gpiod;
	struct gpio_desc *clk48_gpiod;
	
	// parameter
	int rj_slotwidth;
	int clk_always_on;
};


static void snd_i2smfdl_select_clk(struct snd_soc_pcm_runtime *rtd, int clk_id)
{
	struct snd_soc_card *card = rtd->card;
	struct i2smfdl_priv *i2smfdl = snd_soc_card_get_drvdata(card);

	dev_dbg(rtd->dev, "%s clk_id = %d" , __FUNCTION__, clk_id);	

	switch(clk_id) {
		case CLK44_SELECT:
			if (i2smfdl->clk48_gpiod) gpiod_set_value_cansleep(i2smfdl->clk48_gpiod, 0);
			if (i2smfdl->clk44_gpiod) gpiod_set_value_cansleep(i2smfdl->clk44_gpiod, 1);
			break;

		case CLK48_SELECT:
			if (i2smfdl->clk44_gpiod) gpiod_set_value_cansleep(i2smfdl->clk44_gpiod, 0);
			if (i2smfdl->clk48_gpiod) gpiod_set_value_cansleep(i2smfdl->clk48_gpiod, 1);
			break;

		default:
			break;
	}
}

static int snd_i2smfdl_init(struct snd_soc_pcm_runtime *rtd)
{
	dev_dbg(rtd->dev, "%s" , __FUNCTION__);

	// set default clock
	snd_i2smfdl_select_clk(rtd, CLK44_SELECT);

	return 0;
}

static int snd_i2smfdl_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	int ret;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_card *card = rtd->card;
	struct i2smfdl_priv *i2smfdl = snd_soc_card_get_drvdata(card);

	int fs;
	unsigned long bclk;

	dev_dbg(rtd->dev, "%s rate = %d" , __FUNCTION__, params_rate(params));

	if ( !(CLK44_RATE % params_rate(params)) && i2smfdl->clk44_gpiod) {
		bclk = CLK44_RATE;
		snd_i2smfdl_select_clk(rtd, CLK44_SELECT);
	}else
	if ( !(CLK48_RATE % params_rate(params)) && i2smfdl->clk48_gpiod) {
		bclk = CLK48_RATE;
		snd_i2smfdl_select_clk(rtd, CLK48_SELECT);
	}else{
		return -EINVAL;
	}

	fs = bclk / params_rate(params);
	dev_dbg(rtd->dev, "%s set bclk = %ld, fs = %d" , __FUNCTION__, bclk, fs);
	ret = snd_soc_dai_set_bclk_ratio(cpu_dai, fs);

	return ret;
}

static int snd_i2smfdl_startup(
	struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;

	dev_dbg(rtd->dev, "%s" , __FUNCTION__);

	return 0;
}

static void snd_i2smfdl_shutdown(
	struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;

	dev_dbg(rtd->dev, "%s" , __FUNCTION__);

	snd_soc_dai_set_bclk_ratio(cpu_dai, 0);
}

/* machine stream operations */
static struct snd_soc_ops snd_i2smfdl_ops = {
	.hw_params = snd_i2smfdl_hw_params,
	.startup = snd_i2smfdl_startup,
	.shutdown = snd_i2smfdl_shutdown,
};

static struct snd_soc_dai_link snd_i2smfdl_dai[] = {
{
	.name		= "i2s-mfdl",
	.stream_name	= "I2S Multi Function Dai Link",
	.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBM_CFS,
	.ops		= &snd_i2smfdl_ops,
	.init		= snd_i2smfdl_init,
},
};

/* audio machine driver */
static struct snd_soc_card snd_i2smfdl = {
	.name         = "snd_i2smfdl",
	.driver_name  = "PsuedoExternalMasterClock",
	.owner        = THIS_MODULE,
	.dai_link     = snd_i2smfdl_dai,
	.num_links    = ARRAY_SIZE(snd_i2smfdl_dai),
};

static void snd_i2smfdl_parse_device_tree_options(struct device *dev, struct i2smfdl_priv *priv)
{
	int ret, cnt;
	struct snd_soc_dai_link *dai = snd_i2smfdl_dai;

#if 0
	/* get lrclk period map */
	cnt = of_property_count_strings(dev->of_node, "lrclk_periods");
	if ( cnt > 0 ) {
		int i;
		for ( i = 0 ; i < cnt ; i++) {
			const char *output;
			char *endp;
			ret = of_property_read_string_index(dev->of_node, "lrclk_periods", i, &output);
			if (ret) break;
			priv->lrclk_period_map |= PERIOD_TO_MAP(simple_strtol(output, &endp, 10));
			DBGOUT("%s: lrclk_periods[%d] = %s\n", __func__, i,output);
		}
	}else{
		priv->lrclk_period_map = PERIOD_TO_MAP(32); // default 64fs only
	}
	DBGOUT("%s: priv->lrclk_period_map = %x\n", __func__, priv->lrclk_period_map);

	/* get mclk max frequency */
	{
		u32 output;
		priv->mclk_max_freq = 0;
		ret = of_property_read_u32(dev->of_node, "mclk_max_freq", &output);
		if (ret == 0){
			priv->mclk_max_freq = output;
			DBGOUT("%s: priv->mclk_max_freq = %d\n", __func__, priv->mclk_max_freq);
		}
	}

	/* get rate max frequency */
	{
		u32 output;
		priv->mclk_max_freq = 0;
		ret = of_property_read_u32(dev->of_node, "mclk_max_freq", &output);
		if (ret == 0){
			priv->mclk_max_freq = output;
			DBGOUT("%s: priv->mclk_max_freq = %d\n", __func__, priv->mclk_max_freq);
		}
	}

	/* get rate min frequency */
	{
		u32 output;
		priv->mclk_max_freq = 0;
		ret = of_property_read_u32(dev->of_node, "mclk_max_freq", &output);
		if (ret == 0){
			priv->mclk_max_freq = output;
			DBGOUT("%s: priv->mclk_max_freq = %d\n", __func__, priv->mclk_max_freq);
		}
	}
#endif

	/* get dai_fmt master override*/
	{
		const char *output;
		unsigned int dai_fmt = 0;
		ret = of_property_read_string(dev->of_node, "daifmt_master_override", &output);
		if (ret == 0){
			if (!strncmp(output, "CBM_CFM", sizeof("CBM_CFM"))) {
				dai_fmt = SND_SOC_DAIFMT_CBM_CFM;
			}else
			if (!strncmp(output, "CBS_CFS", sizeof("CBS_CFS"))) {
				dai_fmt = SND_SOC_DAIFMT_CBS_CFS;
			}else
			if (!strncmp(output, "CBM_CFS", sizeof("CBM_CFS"))) {
				dai_fmt = SND_SOC_DAIFMT_CBM_CFS;
			}
		}

		if (dai_fmt) {
			dai->dai_fmt = (dai->dai_fmt & (~SND_SOC_DAIFMT_MASTER_MASK)) | dai_fmt;
			dev_info(dev, "daifmt_master_override = %s\n", output);
		}
	}

	/* get dai_fmt audio override*/
	{
		const char *output;
		unsigned int dai_fmt = 0;
		ret = of_property_read_string(dev->of_node, "daifmt_audio_override", &output);
		if (ret == 0){
			if (!strncmp(output, "I2S", sizeof("I2S"))) {
				dai_fmt = SND_SOC_DAIFMT_I2S;
			}else
			if (!strncmp(output, "RJ", sizeof("RJ"))) {
				dai_fmt = SND_SOC_DAIFMT_RIGHT_J;
			}else
			if (!strncmp(output, "LJ", sizeof("LJ"))) {
				dai_fmt = SND_SOC_DAIFMT_LEFT_J;
			}
		}

		if (dai_fmt) {
			dai->dai_fmt = (dai->dai_fmt & (~SND_SOC_DAIFMT_FORMAT_MASK)) | dai_fmt;
			dev_info(dev, "daifmt_audio_override = %s\n", output);
		}
	}

	/* get dai_fmt polarity override*/
	{
		const char *output;
		unsigned int dai_fmt = 0;
		ret = of_property_read_string(dev->of_node, "daifmt_polarity_override", &output);
		if (ret == 0){
			if (!strncmp(output, "IB_IF", sizeof("IB_IF"))) {
				dai_fmt = SND_SOC_DAIFMT_IB_IF;
			}else
			if (!strncmp(output, "IB_NF", sizeof("IB_NF"))) {
				dai_fmt = SND_SOC_DAIFMT_IB_NF;
			}else
			if (!strncmp(output, "NB_IF", sizeof("NB_IF"))) {
				dai_fmt = SND_SOC_DAIFMT_NB_IF;
			}else
			if (!strncmp(output, "NB_NF", sizeof("NB_NF"))) {
				dai_fmt = SND_SOC_DAIFMT_NB_NF;
			}
		}

		if (dai_fmt) {
			dai->dai_fmt = (dai->dai_fmt & (~SND_SOC_DAIFMT_INV_MASK)) | dai_fmt;
			dev_info(dev, "daifmt_polarity_override = %s\n", output);
		}
	}

	/* get Right justified slot width */
	{
		u32 output;
		priv->rj_slotwidth = 32;
		ret = of_property_read_u32(dev->of_node, "rj_slotwidth", &output);
		if (ret == 0){
			priv->rj_slotwidth = output;
			dev_info(dev, "%s: priv->rj_slotwidth = %d\n", __func__, priv->rj_slotwidth);
		}
	}

	/* get clk always on */
	{
		u32 output;
		priv->clk_always_on = -1; // default off.
		ret = of_property_read_u32(dev->of_node, "clk_always_on", &output);
		if (ret == 0 && output == 1){
			priv->clk_always_on = 0; // clk alwasys on enable, set clk stop state.
			dev_info(dev, "%s: priv->clk_always_on = enabled\n", __func__);
		}
	}

	return;
}


static int snd_i2smfdl_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct i2smfdl_priv *i2smfdl;
	dev_dbg(&pdev->dev, "%s\n", __FUNCTION__);

	i2smfdl = devm_kzalloc(&pdev->dev, sizeof(*i2smfdl), GFP_KERNEL);
	if (!i2smfdl){
		dev_err(&pdev->dev, "nomem");
		return -ENOMEM;
	}
	snd_i2smfdl.dev = &pdev->dev;
	snd_soc_card_set_drvdata(&snd_i2smfdl, i2smfdl);
	i2smfdl->dev = &pdev->dev;

	if (pdev->dev.of_node) {
		struct device_node *i2s_node;
		struct snd_soc_dai_link *dai;
		int cnt;

		dai = &snd_i2smfdl_dai[0];

		// find i2s controller
		i2s_node = of_parse_phandle(pdev->dev.of_node, "i2s-controller", 0);
		if (i2s_node) {
			dai->cpu_of_node = i2s_node;
			dai->platform_of_node = i2s_node;
		}

		// find i2s codecs
		cnt = of_count_phandle_with_args(pdev->dev.of_node, "i2s-codec", "#sound-dai-cells");
		if ( cnt > 0 ) {
			struct snd_soc_dai_link_component *codecs;
			struct of_phandle_args args;
			int i;

			dev_info(&pdev->dev, "%s: i2s-codec sound-dai = %d", __func__, cnt);

			dai->codec_name = NULL;
			dai->codec_dai_name = NULL;
			dai->codec_of_node = NULL;

			codecs = devm_kzalloc(&pdev->dev, sizeof(struct snd_soc_dai_link_component)*cnt, GFP_KERNEL);
			if (!codecs)
				return -ENOMEM;

			for ( i = 0; i < cnt ; i++ ) {
				ret = of_parse_phandle_with_args(pdev->dev.of_node, "i2s-codec",
					"#sound-dai-cells", i, &args);
				if (ret) {
					dev_err(&pdev->dev, "%s: Can't find codec by \"i2s-codec\"\n", __func__);
					return 0;
				}
				codecs[i].of_node = args.np;
				if (snd_soc_get_dai_name(&args, &codecs[i].dai_name) < 0 ) {
					dev_err(&pdev->dev, "%s: failed to find dai name, use codec's name as dai name.\n", __func__);
					codecs[i].dai_name = codecs[i].of_node->name;
				}
				dev_dbg(&pdev->dev, "add codec %s @ %s", args.np->name, codecs[i].dai_name);
			}
			dai->codecs = codecs;
			dai->num_codecs = cnt;
		}

		// find i2s aux dev
		cnt = of_count_phandle_with_args(pdev->dev.of_node, "i2s-auxdev", "#sound-dai-cells");
		if ( cnt > 0 ){
			struct snd_soc_aux_dev *auxdevs;
			struct of_phandle_args args;
			int i;

			dev_info(&pdev->dev, "%s: i2c-auxdev sound-dai = %d", __func__, cnt);
			auxdevs = devm_kzalloc(&pdev->dev, sizeof(struct snd_soc_dai_link_component)*cnt, GFP_KERNEL);
			if (!auxdevs)
				return -ENOMEM;
			for ( i = 0; i < cnt ; i++ ) {
				ret = of_parse_phandle_with_args(pdev->dev.of_node, "i2s-auxdev",
					"#sound-dai-cells", i, &args);
				if (ret) {
					dev_err(&pdev->dev, "%s: Can't find codec by \"i2s-auxdev\"\n", __func__);
					return 0;
				}
				auxdevs[i].codec_of_node = args.np;
				auxdevs[i].name    = args.np->name;
				dev_dbg(&pdev->dev, "add aux dev %s", auxdevs[i].name);
			}
			snd_i2smfdl.aux_dev = auxdevs;
			snd_i2smfdl.num_aux_devs = cnt;
		}
	}
	// setup options
	snd_i2smfdl_parse_device_tree_options(i2smfdl->dev, i2smfdl);

	// setup gpios
	i2smfdl->clk44_gpiod = devm_gpiod_get_optional(i2smfdl->dev, "clk44", GPIOD_OUT_LOW);
	if (IS_ERR(i2smfdl->clk44_gpiod)){
		dev_err(i2smfdl->dev, "error requesting clk44-gpios: %ld\n", PTR_ERR(i2smfdl->clk44_gpiod));
		return PTR_ERR(i2smfdl->clk44_gpiod);
	}
    
	i2smfdl->clk48_gpiod = devm_gpiod_get_optional(i2smfdl->dev, "clk48", GPIOD_OUT_LOW);
	if (IS_ERR(i2smfdl->clk48_gpiod)){
		dev_err(i2smfdl->dev, "error requesting clk48-gpios: %ld\n", PTR_ERR(i2smfdl->clk48_gpiod));
		return PTR_ERR(i2smfdl->clk48_gpiod);
	}

	ret = snd_soc_register_card(&snd_i2smfdl);
	if (ret && ret != -EPROBE_DEFER)
		dev_err(&pdev->dev,
			"snd_soc_register_card() failed: %d\n", ret);

	return ret;
}

static int snd_i2smfdl_remove(struct platform_device *pdev)
{
	return snd_soc_unregister_card(&snd_i2smfdl);
}

static const struct of_device_id snd_i2smfdl_of_match[] = {
	{ .compatible = "i2smfdl,multi-function-dai-link", },
	{},
};
MODULE_DEVICE_TABLE(of, snd_i2smfdl_of_match);

static struct platform_driver snd_i2smfdl_driver = {
	.driver = {
		.name   = "snd-i2smfdl",
		.owner  = THIS_MODULE,
		.of_match_table = snd_i2smfdl_of_match,
	},
	.probe          = snd_i2smfdl_probe,
	.remove         = snd_i2smfdl_remove,
};

module_platform_driver(snd_i2smfdl_driver);

MODULE_AUTHOR("__tkz__ <tkz@lrclk.com>");
MODULE_DESCRIPTION("ASoC Driver for I2S Multi Function Dai Link");
MODULE_LICENSE("GPL v2");

