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


// clock mode
enum {
	I2SMFDL_CLKMODE_NORMAL = 0, // normal operation
	I2SMFDL_CLKMODE_EXT_1CLK,   // external BCLK or HIBCLK
	I2SMFDL_CLKMODE_EXT_3CLK,   // external MCLK/BCLK/LRCLK
	I2SMFDL_CLKMODE_HIBCLK,     // internal PLL Generate HIBCLK
};

// external clk type for 1clk mode
enum {
	I2SMFDL_CLK_TYPE_FIXED = 0,
	I2SMFDL_CLK_TYPE_VARIABLE,
};

// external clk order for 1clk mode
enum {
	I2SMFDL_CLKSEL_ORDER_VAR_FIX_PLL = 0,
	I2SMFDL_CLKSEL_ORDER_FIX_VAR_PLL,
	I2SMFDL_CLKSEL_ORDER_PLL,
};

// external clk resources
struct i2smfdl_clks {
	int type;
	struct clk *clk;
	struct gpio_desc *en;
	unsigned int freq;
};

// private data
struct i2smfdl_priv {
	struct device *dev;

	int clk_mode;
	struct i2smfdl_clks *clks;
	int clks_num;
	int clks_previd;

	// 1clk mode
	int min_fs;
	int max_fs;
	unsigned int min_bclk;
	unsigned int min_mclk;
	int clksel_order;
	unsigned int intpll_fallback;

	// fs limit
	unsigned int max_rate;
	unsigned int min_rate;

	int rj_slotwidth;
	int clk_always_on;
};

static int snd_i2smfdl_clkon(struct i2smfdl_priv *i2smfdl, bool mode, int idx)
{
	int err = 0;

	dev_dbg(i2smfdl->dev, "%s %d %d" , __FUNCTION__, mode, idx);

	if ( mode == false ) {
		if (i2smfdl->clks[idx].en) {
			gpiod_set_value_cansleep(i2smfdl->clks[idx].en, 0);
		}
		if (!IS_ERR(i2smfdl->clks[idx].clk)) {
			clk_disable_unprepare(i2smfdl->clks[idx].clk);
		}
		if ( idx == i2smfdl->clks_previd)
			i2smfdl->clks_previd = -1;
	}else{
		// check prev clk
		if (i2smfdl->clks_previd == idx) {
			return 0;
		}
		// disable previous clk
		if (i2smfdl->clks_previd != idx && i2smfdl->clks_previd >= 0) {
			dev_dbg(i2smfdl->dev, "%s prev_clk %d disabled" , __FUNCTION__, i2smfdl->clks_previd);
			if (i2smfdl->clks[i2smfdl->clks_previd].en) {
				gpiod_set_value_cansleep(i2smfdl->clks[i2smfdl->clks_previd].en, 0);
			}
			if (!IS_ERR(i2smfdl->clks[i2smfdl->clks_previd].clk)) {
				clk_disable_unprepare(i2smfdl->clks[i2smfdl->clks_previd].clk);
			}
		}
		i2smfdl->clks_previd = idx;
		if (idx < 0) {
			return 0;
		}
		if (i2smfdl->clks[idx].en) {
			gpiod_set_value_cansleep(i2smfdl->clks[idx].en, 1);
		}
		if (!IS_ERR(i2smfdl->clks[idx].clk)) {
			err = clk_prepare_enable(i2smfdl->clks[idx].clk);
			if (err) {
				i2smfdl->clks_previd = -1;
			}
		}
	}

	return err;
}

static struct snd_soc_dai_link snd_i2smfdl_dai[];
static int snd_i2smfdl_set_daifmt(struct snd_pcm_substream *substream, unsigned int dai_fmt)
{
	struct snd_soc_dai_link *dai = snd_i2smfdl_dai;
	unsigned int dai_fmt_prev = dai->dai_fmt;

	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_card *card = rtd->card;
	struct i2smfdl_priv *i2smfdl = snd_soc_card_get_drvdata(card);


	if (!i2smfdl->intpll_fallback) {
		dev_dbg(rtd->dev, "%s skip. internal pll fallback not allowed.", __FUNCTION__);
		return 0;
	}

	dai->dai_fmt = (dai->dai_fmt & (~SND_SOC_DAIFMT_MASTER_MASK)) | dai_fmt;

	if (dai->dai_fmt != dai_fmt_prev) {
		snd_soc_dai_set_fmt(cpu_dai, dai->dai_fmt);
		dev_dbg(rtd->dev, "%s daifmt change. %x to %x\n", __FUNCTION__, dai_fmt_prev, dai->dai_fmt);
	}

	return 0;
}

static int snd_i2smfdl_select_fixed_clk(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	int i;
	int err = -1;

	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_card *card = rtd->card;
	struct i2smfdl_priv *i2smfdl = snd_soc_card_get_drvdata(card);

	dev_dbg(rtd->dev, "%s" , __FUNCTION__);

	for ( i = 0; i < i2smfdl->clks_num ; i++) {
		int fs;

		if (i2smfdl->clks[i].type != I2SMFDL_CLK_TYPE_FIXED ) {
			continue;
		}

		// div check
		if ( i2smfdl->clks[i].freq % params_rate(params) ) {
			dev_dbg(rtd->dev, "%s not suitable clk. clk_get_rate = %d / freq = %d" , __FUNCTION__, i2smfdl->clks[i].freq, params_rate(params));
			continue;
		}

		// fs calc & check
		fs = i2smfdl->clks[i].freq / params_rate(params);
		if ( (i2smfdl->max_fs && i2smfdl->max_fs < fs) ||
			 (i2smfdl->min_fs == 0 && (params_width(params)*2 > fs)) ||
			 (i2smfdl->min_fs && i2smfdl->min_fs > fs )) {
			dev_dbg(rtd->dev, "%s not suitable clk. clk_get_rate = %d / freq = %d / fs = %d / sw = %d" , __FUNCTION__, 
				i2smfdl->clks[i].freq, params_rate(params), fs, params_width(params));
			continue;
		}

		// set clk
		snd_i2smfdl_set_daifmt(substream, SND_SOC_DAIFMT_CBM_CFS);
		snd_i2smfdl_clkon(i2smfdl, true, i);
		err = snd_soc_dai_set_bclk_ratio(cpu_dai, fs);
		dev_dbg(rtd->dev, "%s clk_set_rate = %d / freq = %d / fs = %d / sw = %d" , __FUNCTION__, 
			i2smfdl->clks[i].freq, params_rate(params), fs, params_width(params));
		break;
	}

	if ( i == i2smfdl->clks_num ) {
		err = -1;
	}

	return err;
}

static int snd_i2smfdl_select_variable_clk(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	int i;
	int err = -1;

	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_card *card = rtd->card;
	struct i2smfdl_priv *i2smfdl = snd_soc_card_get_drvdata(card);

	dev_dbg(rtd->dev, "%s" , __FUNCTION__);

	for ( i = 0; i < i2smfdl->clks_num ; i++) {
		unsigned int freq, real_freq;
		int base_fs, fs;
		err = 0;

		if (i2smfdl->clks[i].type != I2SMFDL_CLK_TYPE_VARIABLE || IS_ERR(i2smfdl->clks[i].clk)) {
			continue;
		}

		base_fs = params_width(params) * 2;
		if (i2smfdl->min_fs && i2smfdl->min_fs > base_fs) {
			base_fs = i2smfdl->min_fs;
		}
		freq = params_rate(params) * base_fs;

		while ( freq <= i2smfdl->min_bclk )
			freq = freq << 1;
		fs = freq / params_rate(params);
		if (i2smfdl->max_fs && i2smfdl->max_fs < fs) {
			dev_warn(rtd->dev, "%s: fs too high. base_fs %d, calc fs %d, max fs %d", __FUNCTION__, base_fs, fs, i2smfdl->max_fs);
		}

		snd_i2smfdl_set_daifmt(substream, SND_SOC_DAIFMT_CBM_CFS);
		err = clk_set_rate(i2smfdl->clks[i].clk, freq);
		if (err) continue;
		err = snd_i2smfdl_clkon(i2smfdl, true, i);
		if (err) continue;
		err = snd_soc_dai_set_bclk_ratio(cpu_dai, fs);
		if (err) continue;
		real_freq = clk_get_rate(i2smfdl->clks[i].clk);
		dev_dbg(rtd->dev, "%s clk_set_rate = %d / clk_get_rate = %d / fs = %d / sw = %d" , __FUNCTION__, freq, real_freq, fs, params_width(params));
		break;
	}

	if ( i == i2smfdl->clks_num ) {
		err = -1;
	}

	return err;
}

static int snd_i2smfdl_select_intpll_clk(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	int err = 0;

	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_card *card = rtd->card;
	struct i2smfdl_priv *i2smfdl = snd_soc_card_get_drvdata(card);

	unsigned int freq;
	int base_fs, fs;

	dev_dbg(rtd->dev, "%s" , __FUNCTION__);

	base_fs = params_width(params) * 2;
	if (i2smfdl->min_fs && i2smfdl->min_fs > base_fs) {
		base_fs = i2smfdl->min_fs;
	}
	freq = params_rate(params) * base_fs;

	while ( freq <= i2smfdl->min_bclk )
		freq = freq << 1;
	fs = freq / params_rate(params);

	if (i2smfdl->max_fs && i2smfdl->max_fs < fs) {
		dev_warn(rtd->dev, "%s: fs too high. base_fs %d, calc fs %d, max fs %d", __FUNCTION__, base_fs, fs, i2smfdl->max_fs);
	}

	snd_i2smfdl_set_daifmt(substream, SND_SOC_DAIFMT_CBS_CFS);
	err = snd_i2smfdl_clkon(i2smfdl, true, -1);

	err = snd_soc_dai_set_bclk_ratio(cpu_dai, fs);
	snd_soc_dai_set_clkdiv(cpu_dai, 0, 1); // set div1
	dev_dbg(rtd->dev, "%s clk_set_rate = %d / fs = %d / sw = %d" , __FUNCTION__, freq, fs, params_width(params));

	return err;
}

static int snd_i2smfdl_set_ext1clk(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	int ret = -1;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct i2smfdl_priv *i2smfdl = snd_soc_card_get_drvdata(card);

	// clock select order
	if (i2smfdl->clksel_order == I2SMFDL_CLKSEL_ORDER_VAR_FIX_PLL) {
		// variable clock 1st
		ret = snd_i2smfdl_select_variable_clk(substream, params);
		if ( ret < 0 ) {
			ret = snd_i2smfdl_select_fixed_clk(substream, params);
		}
	}else
	if (i2smfdl->clksel_order == I2SMFDL_CLKSEL_ORDER_FIX_VAR_PLL) {
		// fixed clock 1st
		ret = snd_i2smfdl_select_fixed_clk(substream, params);
		if ( ret < 0 ) {
			ret = snd_i2smfdl_select_variable_clk(substream, params);
		}
	}

	// fallback to internal pll if it is allowed
	if ( ret < 0 && i2smfdl->intpll_fallback ) {
		ret = snd_i2smfdl_select_intpll_clk(substream, params);
	}

	return ret;
}

static int snd_i2smfdl_set_ext3clk(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	int err = 0;

	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_card *card = rtd->card;
	struct i2smfdl_priv *i2smfdl = snd_soc_card_get_drvdata(card);

	unsigned long mclk;
	unsigned long bclk;
	unsigned long lrclk;
	unsigned long real_freq;
	unsigned int fs;

	dev_dbg(rtd->dev, "%s" , __FUNCTION__);

	lrclk = params_rate(params);
	fs = params_width(params) * 2;
	if (i2smfdl->min_fs && i2smfdl->min_fs > fs) {
		fs = i2smfdl->min_fs;
	}
	if (i2smfdl->max_fs && i2smfdl->max_fs < fs) {
		fs = i2smfdl->max_fs;
	}

	/* calcurate clk ratio */
	mclk = bclk = lrclk * fs;
	while ( mclk <= i2smfdl->min_mclk )
		mclk = mclk << 1;

	while ( bclk <= i2smfdl->min_bclk )
		bclk = bclk << 1;

	
	if (params_format(params)==SNDRV_PCM_FORMAT_DSD_U32_LE){
		lrclk++;
	}

	/* clk setup */
	snd_i2smfdl_set_daifmt(substream, SND_SOC_DAIFMT_CBM_CFM);

//	if (!IS_ERR(i2smfdl->clks[0].clk)) clk_set_rate(i2smfdl->clks[0].clk, mclk);
//	if (!IS_ERR(i2smfdl->clks[1].clk)) clk_set_rate(i2smfdl->clks[1].clk, bclk);
	if (!IS_ERR(i2smfdl->clks[2].clk)) clk_set_rate(i2smfdl->clks[2].clk, lrclk);

//	if (!IS_ERR(i2smfdl->clks[0].clk)) clk_prepare_enable(i2smfdl->clks[0].clk);
//	if (!IS_ERR(i2smfdl->clks[1].clk)) clk_prepare_enable(i2smfdl->clks[1].clk);
	if (!IS_ERR(i2smfdl->clks[2].clk)) clk_prepare_enable(i2smfdl->clks[2].clk);
	err = snd_soc_dai_set_bclk_ratio(cpu_dai, fs);

/*
	if (!IS_ERR(i2smfdl->clks[0].clk)){
		real_freq = clk_get_rate(i2smfdl->clks[0].clk);
	}else{
	        dev_dbg(rtd->dev, "is_err %s" , __FUNCTION__);
		real_freq = clk_get_rate(i2smfdl->clks[1].clk);
	}
*/
	dev_dbg(rtd->dev, "%s clk_set_rate = %ld / clk_get_rate = %ld / bclk = %ld / lrclk = %ld / fs = %d / sw = %d" , __FUNCTION__, mclk, real_freq, bclk, lrclk, fs, params_width(params));

	return err;
}


static const char * const i2smfdl_clksel_order_select_texts[] = {
	"Variable->Fixed->IntPLL",
	"Fixed->Variable->IntPLL",
	"IntPLL Only",
};

static const char * const i2smfdl_clksel_order_wopll_select_texts[] = {
	"Variable->Fixed",
	"Fixed->Variable",
};

static const struct soc_enum i2smfdl_clksel_order_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(i2smfdl_clksel_order_select_texts),
			    i2smfdl_clksel_order_select_texts);
static const struct soc_enum i2smfdl_clksel_order_wopll_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(i2smfdl_clksel_order_wopll_select_texts),
			    i2smfdl_clksel_order_wopll_select_texts);

static int snd_i2smfdl_get_clksel_order(struct snd_kcontrol *kcontrol,
		      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct i2smfdl_priv *i2smfdl = snd_soc_card_get_drvdata(card);

	ucontrol->value.enumerated.item[0] = i2smfdl->clksel_order;

	return 0;
}

static int snd_i2smfdl_set_clksel_order(struct snd_kcontrol *kcontrol,
		      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct i2smfdl_priv *i2smfdl = snd_soc_card_get_drvdata(card);
	int num;

	num = ucontrol->value.enumerated.item[0];
	if (num > 2)
		return -EINVAL;

	i2smfdl->clksel_order = num;

	return 0;
}


static const char * const i2smfdl_maxrate_select_texts[] = {
	"3072kHz", "1536kHz", "768kHz",
	"384kHz" , "192kHz" , "96kHz",
};

static const struct soc_enum i2smfdl_maxrate_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(i2smfdl_maxrate_select_texts),
		i2smfdl_maxrate_select_texts);

static int snd_i2smfdl_get_maxrate(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct i2smfdl_priv *i2smfdl = snd_soc_card_get_drvdata(card);

	int num = 0;
	while (( 3072000 >> num) != i2smfdl->max_rate ) num++;
	ucontrol->value.enumerated.item[0] = num;

	return 0;
}

static int snd_i2smfdl_set_maxrate(struct snd_kcontrol *kcontrol,
                      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct i2smfdl_priv *i2smfdl = snd_soc_card_get_drvdata(card);
	unsigned int rate = 3072000;
	int shift;

	shift = ucontrol->value.enumerated.item[0];
	i2smfdl->max_rate = rate >> shift;

	return 0;
}


static struct snd_kcontrol_new i2smfdl_snd_controls[] = {
	SOC_ENUM_EXT("MaxSamplingRate", i2smfdl_maxrate_enum, snd_i2smfdl_get_maxrate, snd_i2smfdl_set_maxrate),
};

static struct snd_kcontrol_new i2smfdl_snd_controls_clk[] = {
	SOC_ENUM_EXT("MaxSamplingRate", i2smfdl_maxrate_enum, snd_i2smfdl_get_maxrate, snd_i2smfdl_set_maxrate),
	SOC_ENUM_EXT("ClockSelectOrder", i2smfdl_clksel_order_enum, snd_i2smfdl_get_clksel_order, snd_i2smfdl_set_clksel_order),
};
static struct snd_kcontrol_new i2smfdl_snd_controls_clk_wopll[] = {
	SOC_ENUM_EXT("MaxSamplingRate", i2smfdl_maxrate_enum, snd_i2smfdl_get_maxrate, snd_i2smfdl_set_maxrate),
	SOC_ENUM_EXT("ClockSelectOrder", i2smfdl_clksel_order_wopll_enum, snd_i2smfdl_get_clksel_order, snd_i2smfdl_set_clksel_order),
};

static int snd_i2smfdl_init(struct snd_soc_pcm_runtime *rtd)
{
	dev_dbg(rtd->dev, "%s" , __FUNCTION__);

	return 0;
}

static int snd_i2smfdl_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct i2smfdl_priv *i2smfdl = snd_soc_card_get_drvdata(card);

	dev_dbg(rtd->dev, "%s rate = %d" , __FUNCTION__, params_rate(params));

	// not use ext clock
	if ( i2smfdl->clk_mode == I2SMFDL_CLKMODE_NORMAL || i2smfdl->clks_num == 0) {
		dev_dbg(rtd->dev, "%s external clk not set. (CBS_CFS mode)" , __FUNCTION__);
		return 0;
	}

	// not use ext clock. hibclk mode.
	if ( i2smfdl->clk_mode == I2SMFDL_CLKMODE_HIBCLK ) {
		dev_dbg(rtd->dev, "%s external clk not set. (CBS_CFS hibclk mode)" , __FUNCTION__);
		return snd_i2smfdl_select_intpll_clk(substream, params);
	}

	// ext 3clock mode
	if ( i2smfdl->clk_mode == I2SMFDL_CLKMODE_EXT_3CLK ) {
		dev_dbg(rtd->dev, "%s external clk set. (CBM_CFM 3clk mode)" , __FUNCTION__);
		return snd_i2smfdl_set_ext3clk(substream, params);
	}

	// ext 1clock mode
	dev_dbg(rtd->dev, "%s external clk set. (CBM_CFS 1clk mode)" , __FUNCTION__);
	return snd_i2smfdl_set_ext1clk(substream, params);
}


static int snd_i2smfdl_startup(
	struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct i2smfdl_priv *i2smfdl = snd_soc_card_get_drvdata(card);

	dev_dbg(rtd->dev, "%s" , __FUNCTION__);
	return snd_pcm_hw_constraint_minmax(substream->runtime, SNDRV_PCM_HW_PARAM_RATE,
                                           i2smfdl->min_rate, i2smfdl->max_rate);
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
	.driver_name  = "MultiFunsionDaiLinkDriver",
	.owner        = THIS_MODULE,
	.dai_link     = snd_i2smfdl_dai,
	.num_links    = ARRAY_SIZE(snd_i2smfdl_dai),
};

static void snd_i2smfdl_parse_device_tree_options(struct device *dev, struct i2smfdl_priv *i2smfdl)
{
	int ret;
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
			i2smfdl->lrclk_period_map |= PERIOD_TO_MAP(simple_strtol(output, &endp, 10));
			DBGOUT("%s: lrclk_periods[%d] = %s\n", __func__, i,output);
		}
	}else{
		i2smfdl->lrclk_period_map = PERIOD_TO_MAP(32); // default 64fs only
	}
	DBGOUT("%s: i2smfdl->lrclk_period_map = %x\n", __func__, i2smfdl->lrclk_period_map);
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
		i2smfdl->rj_slotwidth = 32;
		ret = of_property_read_u32(dev->of_node, "rj_slotwidth", &output);
		if (ret == 0){
			i2smfdl->rj_slotwidth = output;
			dev_info(dev, "%s: i2smfdl->rj_slotwidth = %d\n", __func__, i2smfdl->rj_slotwidth);
		}
	}

	/* get clk mode */
	{
		const char *output;
		i2smfdl->clk_mode = I2SMFDL_CLKMODE_NORMAL;
		ret = of_property_read_string(dev->of_node, "clk_mode", &output);
		if (ret == 0){
			if (!strncmp(output, "NORMAL", sizeof("NORMAL"))) {
				/* check */
				if ((dai->dai_fmt & SND_SOC_DAIFMT_MASTER_MASK) != SND_SOC_DAIFMT_CBS_CFS ) {
					dev_warn(dev, "dai_fmt not set CBS_CFS mode in normal mode.");
				}
			i2smfdl->clk_mode = I2SMFDL_CLKMODE_NORMAL;
			}else
			if (!strncmp(output, "EXT_1CLK", sizeof("EXT_1CLK"))) {
				i2smfdl->clk_mode = I2SMFDL_CLKMODE_EXT_1CLK;
			}else
			if (!strncmp(output, "EXT_3CLK", sizeof("EXT_3CLK"))) {
			i2smfdl->clk_mode = I2SMFDL_CLKMODE_EXT_3CLK;
			}else
			if (!strncmp(output, "HIBCLK", sizeof("HIBCLK"))) {
				i2smfdl->clk_mode = I2SMFDL_CLKMODE_HIBCLK;
				/* check */
				if ((dai->dai_fmt & SND_SOC_DAIFMT_MASTER_MASK) != SND_SOC_DAIFMT_CBS_CFS ) {
					dev_warn(dev, "dai_fmt not set CBS_CFS mode in hibclk mode.");
				}
			}
		}

		dev_info(dev, "clkmode = %s\n", output);
	}


	/* get clk always on */
	{
		u32 output;
		i2smfdl->clk_always_on = -1; // default off.
		ret = of_property_read_u32(dev->of_node, "clk_always_on", &output);
		if (ret == 0 && output == 1){
			i2smfdl->clk_always_on = 0; // clk alwasys on enable, set clk stop state.
			dev_info(dev, "%s: i2smfdl->clk_always_on = enabled\n", __func__);
		}
	}

	/* setup clocks */
	if (i2smfdl->clk_mode == I2SMFDL_CLKMODE_EXT_1CLK){
		int clk_num, freq_num, gpio_num, i;
		freq_num = of_property_count_u32_elems(dev->of_node, "fixed-freqs");
		gpio_num = gpiod_count(dev, "en");
		clk_num  = of_count_phandle_with_args(dev->of_node, "clocks", "#clock-cells");
		if (freq_num < 0) freq_num = 0;
		if (gpio_num < 0) gpio_num = 0;
		if (clk_num  < 0) clk_num  = 0;
		dev_dbg(dev, "freq_num %d / gpio_num %d / clk_num %d", freq_num, gpio_num, clk_num);
		if ( freq_num == gpio_num ) {
			i2smfdl->clks = devm_kzalloc(dev, sizeof(struct i2smfdl_clks) * (freq_num + clk_num), GFP_KERNEL);
			for ( i = 0; i < freq_num; i++) {
				i2smfdl->clks[i].en   = devm_gpiod_get_index_optional(dev, "en", i, GPIOD_OUT_LOW);
				of_property_read_u32_index(dev->of_node, "fixed-freqs", i, &(i2smfdl->clks[i].freq));
				i2smfdl->clks[i].type = I2SMFDL_CLK_TYPE_FIXED;
				dev_info(dev, "%s: i2smfdl->clk[%d] %d Hz fixed clock", __func__, i, i2smfdl->clks[i].freq);
			}
			for ( i = 0; i < clk_num; i++) {
				i2smfdl->clks[i+freq_num].clk = of_clk_get(dev->of_node, i);
				i2smfdl->clks[i+freq_num].type = I2SMFDL_CLK_TYPE_VARIABLE;
				dev_info(dev, "%s: i2smfdl->clk[%d] variable clock", __func__, i+freq_num);
			}
			i2smfdl->clks_num = freq_num + clk_num;
		}

		/* check */
		if (i2smfdl->clks_num == 0) {
			// set internal pll only when nothing external clk
			i2smfdl->clksel_order = I2SMFDL_CLKSEL_ORDER_PLL;
			dev_warn(dev, "not found clocks in ext 1clk mode.");
		}
		if ((dai->dai_fmt & SND_SOC_DAIFMT_MASTER_MASK) != SND_SOC_DAIFMT_CBM_CFS ) {
			dev_warn(dev, "dai_fmt not set CBM_CFS mode in ext 1clk mode.");
		}
	}else
	if (i2smfdl->clk_mode == I2SMFDL_CLKMODE_EXT_3CLK){
		i2smfdl->clks_num = of_count_phandle_with_args(dev->of_node, "clocks", "#clock-cells");
		i2smfdl->clks = devm_kzalloc(dev, sizeof(struct i2smfdl_clks) * 3, GFP_KERNEL);
		i2smfdl->clks[0].clk = clk_get(dev, "mclk");
		i2smfdl->clks[1].clk = clk_get(dev, "bclk");
		i2smfdl->clks[2].clk = clk_get(dev, "lrclk");

		/* check */
		if (!i2smfdl->clks[1].clk || !i2smfdl->clks[2].clk) {
			dev_warn(dev, "BCLK and LRCLK required in ext 3clk mode.");
		}
		if ((dai->dai_fmt & SND_SOC_DAIFMT_MASTER_MASK) != SND_SOC_DAIFMT_CBM_CFM ) {
			dev_warn(dev, "dai_fmt not set CBM_CFM mode in ext 3clk mode.");
		}
	}

	/* setup clock calculate options */
	{
		of_property_read_u32(dev->of_node, "min_bclk", &i2smfdl->min_bclk);
		of_property_read_u32(dev->of_node, "min_mclk", &i2smfdl->min_mclk);
		of_property_read_u32(dev->of_node, "min_fs", &i2smfdl->min_fs);
		of_property_read_u32(dev->of_node, "max_fs", &i2smfdl->max_fs);
		of_property_read_u32(dev->of_node, "max_rate", &i2smfdl->max_rate);
		if (!i2smfdl->max_rate) i2smfdl->max_rate = 768000;
		of_property_read_u32(dev->of_node, "min_rate", &i2smfdl->min_rate);
		if (!i2smfdl->min_rate) i2smfdl->min_rate = 8000;
		of_property_read_u32(dev->of_node, "intpll_fallback", &i2smfdl->intpll_fallback);
		if (i2smfdl->intpll_fallback) dev_info(dev, "allow internal pll fallback operation.");
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

	// setup default value
	i2smfdl->clks_previd = -1;

	// setup mixer
	snd_i2smfdl.controls        = i2smfdl_snd_controls;
	snd_i2smfdl.num_controls    = ARRAY_SIZE(i2smfdl_snd_controls);
	if (i2smfdl->clks_num && i2smfdl->clk_mode == I2SMFDL_CLKMODE_EXT_1CLK){
		if (i2smfdl->intpll_fallback) {
			snd_i2smfdl.controls        = i2smfdl_snd_controls_clk;
			snd_i2smfdl.num_controls    = ARRAY_SIZE(i2smfdl_snd_controls_clk);
		} else {
			snd_i2smfdl.controls        = i2smfdl_snd_controls_clk_wopll;
			snd_i2smfdl.num_controls    = ARRAY_SIZE(i2smfdl_snd_controls_clk_wopll);
		}
	}

	// sound card register
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

