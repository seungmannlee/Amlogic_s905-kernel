/*
 * Copyright (C) 2017 BayLibre, SAS
 * Author: Jerome Brunet <jbrunet@baylibre.com>
 * Copyright (C) 2017 Amlogic, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <sound/pcm_iec958.h>

#include "aiu-regs.h"
#include "audio-core.h"

#define DRV_NAME "meson-spdif-dai"

struct meson_spdif_dai {
	struct meson_audio_core_data *core;
	struct clk *iface;
	struct clk *fast;
	struct clk *mclk_i958;
	struct clk *mclk;
};

#define AIU_CLK_CTRL_958_DIV_EN			BIT(1)
#define AIU_CLK_CTRL_958_DIV_MASK		GENMASK(5, 4)
#define AIU_CLK_CTRL_958_DIV_MORE		BIT(12)
#define AIU_MEM_IEC958_CONTROL_MODE_LINEAR	BIT(8)
#define AIU_958_CTRL_HOLD_EN			BIT(0)
#define AIU_958_MISC_NON_PCM			BIT(0)
#define AIU_958_MISC_MODE_16BITS		BIT(1)
#define AIU_958_MISC_16BITS_ALIGN_MASK		GENMASK(6, 5)
#define AIU_958_MISC_16BITS_ALIGN(val)		((val) << 5)
#define AIU_958_MISC_MODE_32BITS		BIT(7)
#define AIU_958_MISC_32BITS_SHIFT_MASK		GENMASK(10, 8)
#define AIU_958_MISC_32BITS_SHIFT(val)		((val) << 8)
#define AIU_958_MISC_U_FROM_STREAM		BIT(12)
#define AIU_958_MISC_FORCE_LR			BIT(13)

#define AIU_CS_WORD_LEN 4

static void __hold(struct meson_spdif_dai *priv, bool enable)
{
	regmap_update_bits(priv->core->aiu, AIU_958_CTRL,
			   AIU_958_CTRL_HOLD_EN,
			   enable ? AIU_958_CTRL_HOLD_EN : 0);
}

static void __divider_enable(struct meson_spdif_dai *priv, bool enable)
{
	regmap_update_bits(priv->core->aiu, AIU_CLK_CTRL,
			   AIU_CLK_CTRL_958_DIV_EN,
			   enable ? AIU_CLK_CTRL_958_DIV_EN : 0);
}

static void __playback_start(struct meson_spdif_dai *priv)
{
	__divider_enable(priv, true);
	__hold(priv, false);
}

static void __playback_stop(struct meson_spdif_dai *priv)
{
	__hold(priv, true);
	__divider_enable(priv, false);
}

static int meson_spdif_dai_trigger(struct snd_pcm_substream *substream, int cmd,
				   struct snd_soc_dai *dai)
{
	struct meson_spdif_dai *priv = snd_soc_dai_get_drvdata(dai);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		__playback_start(priv);
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		__playback_stop(priv);
		return 0;

	default:
		return -EINVAL;
	}
}

static int __setup_spdif_clk(struct meson_spdif_dai *priv, unsigned int rate)
{
	unsigned int mrate;

	/* Leave the internal divisor alone */
	regmap_update_bits(priv->core->aiu, AIU_CLK_CTRL,
			   AIU_CLK_CTRL_958_DIV_MASK |
			   AIU_CLK_CTRL_958_DIV_MORE,
			   0);

	/* 2 * 32bits per subframe * 2 channels = 128 */
	mrate = rate * 128;
	return clk_set_rate(priv->mclk, mrate);
}

static int __setup_cs_word(struct meson_spdif_dai *priv,
			   struct snd_pcm_hw_params *params)
{
	u8 cs[AIU_CS_WORD_LEN];
	u32 val;
	int ret;

	ret = snd_pcm_create_iec958_consumer_hw_params(params, cs,
						       AIU_CS_WORD_LEN);
	if (ret < 0)
		return -EINVAL;

	/* Write the 1st half word */
	val = cs[1] | cs[0] << 8;
	regmap_write(priv->core->aiu, AIU_958_CHSTAT_L0, val);
	regmap_write(priv->core->aiu, AIU_958_CHSTAT_R0, val);

	/* Write the 2nd half word */
	val = cs[3] | cs[2] << 8;
	regmap_write(priv->core->aiu, AIU_958_CHSTAT_L1, val);
	regmap_write(priv->core->aiu, AIU_958_CHSTAT_R1, val);

	return 0;
}

static int __setup_pcm_fmt(struct meson_spdif_dai *priv,
			   unsigned int width)
{
	u32 val = 0;

	switch (width) {
	case 16:
		val |= AIU_958_MISC_MODE_16BITS;
		val |= AIU_958_MISC_16BITS_ALIGN(2);
		break;
	case 32:
	case 24:
		/*
		 * Looks like this should only be set for 32bits mode, but the
		 * vendor kernel sets it like this for 24bits as well, let's
		 * try and see
		 */
		val |= AIU_958_MISC_MODE_32BITS;
		break;
	default:
		return -EINVAL;
	}

	/* No idea what this actually does, copying the vendor kernel for now */
	val |= AIU_958_MISC_FORCE_LR;
	val |= AIU_958_MISC_U_FROM_STREAM;

	regmap_update_bits(priv->core->aiu, AIU_958_MISC,
			   AIU_958_MISC_NON_PCM |
			   AIU_958_MISC_MODE_16BITS |
			   AIU_958_MISC_16BITS_ALIGN_MASK |
			   AIU_958_MISC_MODE_32BITS |
			   AIU_958_MISC_FORCE_LR,
			   val);

	return 0;
}

static int meson_spdif_dai_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params,
				     struct snd_soc_dai *dai)
{
	struct meson_spdif_dai *priv = snd_soc_dai_get_drvdata(dai);
	int ret;

	ret = __setup_spdif_clk(priv, params_rate(params));
	if (ret) {
		dev_err(dai->dev, "Unable to set the spdif clock\n");
		return ret;
	}

	ret = __setup_cs_word(priv, params);
	if (ret) {
		dev_err(dai->dev, "Unable to set the channel status word\n");
		return ret;
	}

	ret = __setup_pcm_fmt(priv, params_width(params));
	if (ret) {
		dev_err(dai->dev, "Unable to set the pcm format\n");
		return ret;
	}

	return 0;
}

static int meson_spdif_dai_startup(struct snd_pcm_substream *substream,
				   struct snd_soc_dai *dai)
{
	struct meson_spdif_dai *priv = snd_soc_dai_get_drvdata(dai);
	int ret;

	/* Power up the spdif fast domain - can't write the registers w/o it */
	ret = clk_prepare_enable(priv->fast);
	if (ret)
		goto out_clk_fast;

	/* Make sure nothing gets out of the DAI yet*/
	__hold(priv, true);

	ret = clk_set_parent(priv->mclk, priv->mclk_i958);
	if (ret)
		return ret;

	/* Enable the clock gate */
	ret = clk_prepare_enable(priv->iface);
	if (ret)
		goto out_clk_iface;

	/* Enable the spdif clock */
	ret = clk_prepare_enable(priv->mclk);
	if (ret)
		goto out_mclk;

	/*
	 * Make sure the interface expect a memory layout we can work with
	 * MEM prefixed register usually belong to the DMA, but when the spdif
	 * DAI takes data from the i2s buffer, we need to make sure it works in
	 * split mode and not the  "normal mode" (channel samples packed in
	 * 32 bytes groups)
	 */
	regmap_update_bits(priv->core->aiu, AIU_MEM_IEC958_CONTROL,
			   AIU_MEM_IEC958_CONTROL_MODE_LINEAR,
			   AIU_MEM_IEC958_CONTROL_MODE_LINEAR);

	return 0;

out_mclk:
	clk_disable_unprepare(priv->iface);
out_clk_iface:
	clk_disable_unprepare(priv->fast);
out_clk_fast:
	return ret;
}

static void meson_spdif_dai_shutdown(struct snd_pcm_substream *substream,
				   struct snd_soc_dai *dai)
{
	struct meson_spdif_dai *priv = snd_soc_dai_get_drvdata(dai);

	clk_disable_unprepare(priv->iface);
	clk_disable_unprepare(priv->mclk);
	clk_disable_unprepare(priv->fast);
}

static const struct snd_soc_dai_ops meson_spdif_dai_ops = {
	.startup    = meson_spdif_dai_startup,
	.shutdown   = meson_spdif_dai_shutdown,
	.trigger    = meson_spdif_dai_trigger,
	.hw_params  = meson_spdif_dai_hw_params,
};

static struct snd_soc_dai_driver meson_spdif_dai = {
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = (SNDRV_PCM_RATE_32000 |
			  SNDRV_PCM_RATE_44100 |
			  SNDRV_PCM_RATE_48000 |
			  SNDRV_PCM_RATE_96000 |
			  SNDRV_PCM_RATE_192000),
		.formats = (SNDRV_PCM_FMTBIT_S16_LE |
			    SNDRV_PCM_FMTBIT_S24_LE)
	},
	.ops = &meson_spdif_dai_ops,
};

static const struct snd_soc_component_driver meson_spdif_dai_component = {
	.name	= DRV_NAME,
};

static int meson_spdif_dai_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct meson_spdif_dai *priv;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);
	priv->core = dev_get_drvdata(dev->parent);

	priv->fast = devm_clk_get(dev, "fast");
	if (IS_ERR(priv->fast)) {
		if (PTR_ERR(priv->fast) != -EPROBE_DEFER)
			dev_err(dev, "Can't get spdif fast domain clockt\n");
		return PTR_ERR(priv->fast);
	}

	priv->iface = devm_clk_get(dev, "iface");
	if (IS_ERR(priv->iface)) {
		if (PTR_ERR(priv->iface) != -EPROBE_DEFER)
			dev_err(dev,
				"Can't get the dai clock gate\n");
		return PTR_ERR(priv->iface);
	}

	priv->mclk_i958 = devm_clk_get(dev, "mclk_i958");
	if (IS_ERR(priv->mclk_i958)) {
		if (PTR_ERR(priv->mclk_i958) != -EPROBE_DEFER)
			dev_err(dev, "Can't get the spdif master clock\n");
		return PTR_ERR(priv->mclk_i958);
	}

	/*
	 * TODO: the spdif dai can also get its data from the i2s fifo.
	 * For this use-case, the DAI driver will need to get the i2s master
	 * clock in order to reparent the spdif clock from cts_mclk_i958 to
	 * cts_amclk
	 */

	priv->mclk = devm_clk_get(dev, "mclk");
	if (IS_ERR(priv->mclk)) {
		if (PTR_ERR(priv->mclk) != -EPROBE_DEFER)
			dev_err(dev, "Can't get the spdif input mux clock\n");
		return PTR_ERR(priv->mclk);
	}

	return devm_snd_soc_register_component(dev, &meson_spdif_dai_component,
					       &meson_spdif_dai, 1);
}

static const struct of_device_id meson_spdif_dai_of_match[] = {
	{ .compatible = "amlogic,meson-spdif-dai", },
	{ .compatible = "amlogic,meson-gxbb-spdif-dai", },
	{ .compatible = "amlogic,meson-gxl-spdif-dai", },
	{}
};
MODULE_DEVICE_TABLE(of, meson_spdif_dai_of_match);

static struct platform_driver meson_spdif_dai_pdrv = {
	.probe = meson_spdif_dai_probe,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = meson_spdif_dai_of_match,
	},
};
module_platform_driver(meson_spdif_dai_pdrv);

MODULE_DESCRIPTION("Meson spdif DAI ASoC Driver");
MODULE_AUTHOR("Jerome Brunet <jbrunet@baylibre.com>");
MODULE_LICENSE("GPL v2");
