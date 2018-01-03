/*
 * pcm1690 ASoC codec driver
 *
 * Copyright (c) StreamUnlimited GmbH 2013
 *	Marek Belisko <marek.belisko@streamunlimited.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#define pcm1690_PCM_FORMATS (SNDRV_PCM_FMTBIT_S32_LE)

#define pcm1690_PCM_RATES   (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 | \
			     SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100  | \
			     SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200  | \
			     SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_192000)

#define pcm1690_SOFT_MUTE_ALL		0xff
#define pcm1690_DEEMPH_RATE_MASK	0x30
#define pcm1690_DEEMPH_MASK		0x30

#define pcm1690_ATT_CONTROL(X)	(X >= 72 ? X : X + 71) /* Attenuation level */
#define pcm1690_SOFT_MUTE	0x44	/* Soft mute control register */
#define pcm1690_FMT_CONTROL	0x41	/* Audio interface data format */
#define pcm1690_DEEMPH_CONTROL	0x46	/* De-emphasis control */
#define pcm1690_ZERO_DETECT_STATUS	0x45	/* Zero detect status reg */

static void display_cpm1690_reg(struct i2c_client *client);

static const struct reg_default pcm1690_reg_defaults[] = {
	{ 0x40,	0xc0 },
	{ 0x41,	0x04 },//soc default is 0x04
	{ 0x42,	0x00 },
	{ 0x43,	0x00 },
	{ 0x44,	0x00 },
	//{ 0x45,	0x00 },//Zero flag (read-only)
	{ 0x46,	0x00 },
	//{ 0x47,	0x00 },//not reg
	{ 0x48,	0xff },
	{ 0x49,	0xff },
	{ 0x4A,	0xff },
	{ 0x4B,	0xff },
	{ 0x4C,	0xff },
	{ 0x4D,	0xff },
	{ 0x4E,	0xff },
	{ 0x4F,	0xff },
};

static bool pcm1690_accessible_reg(struct device *dev, unsigned int reg)
{
	return ((reg <= 0x4F &&  reg >= 0x40) && !(reg == 0x47));
}

static bool pcm1690_writeable_reg(struct device *dev, unsigned int reg)
{
	return pcm1690_accessible_reg(dev, reg) && (reg != pcm1690_ZERO_DETECT_STATUS);
}

struct pcm1690_private {
	struct regmap *regmap;
	unsigned int format;
	/* Current deemphasis status */
	unsigned int deemph;
	/* Current rate for deemphasis control */
	unsigned int rate;
};

static const int pcm1690_deemph[] = {44100, 48000, 32000 };

static int pcm1690_set_deemph(struct snd_soc_codec *codec)
{
	struct pcm1690_private *priv = snd_soc_codec_get_drvdata(codec);
	int i = 0, val = -1, enable = 0;

	if (priv->deemph) {
		for (i = 0; i < ARRAY_SIZE(pcm1690_deemph); i++) {
			if (pcm1690_deemph[i] == priv->rate) {
				val = i;
				break;
			}
		}
	}

	if (val != -1) {
		regmap_update_bits(priv->regmap, pcm1690_DEEMPH_CONTROL,
				   pcm1690_DEEMPH_RATE_MASK, val << 4);
		enable = 1;
	} else {
		enable = 0;
	}

	/* enable/disable deemphasis functionality */
	return regmap_update_bits(priv->regmap, pcm1690_DEEMPH_CONTROL,
					pcm1690_DEEMPH_MASK, enable);
}

static int pcm1690_get_deemph(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct pcm1690_private *priv = snd_soc_codec_get_drvdata(codec);

	ucontrol->value.integer.value[0] = priv->deemph;

	return 0;
}

static int pcm1690_put_deemph(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);
	struct pcm1690_private *priv = snd_soc_codec_get_drvdata(codec);

	priv->deemph = ucontrol->value.integer.value[0];

	return pcm1690_set_deemph(codec);
}

static int pcm1690_set_dai_fmt(struct snd_soc_dai *codec_dai,
			      unsigned int format)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	struct pcm1690_private *priv = snd_soc_codec_get_drvdata(codec);

	/* The pcm1690 can only be slave to all clocks */
	if ((format & SND_SOC_DAIFMT_MASTER_MASK) != SND_SOC_DAIFMT_CBS_CFS) {
		dev_err(codec->dev, "Invalid clocking mode\n");
		return -EINVAL;
	}

	priv->format = format;

	return 0;
}

static int pcm1690_digital_mute(struct snd_soc_dai *dai, int mute)
{
	struct snd_soc_codec *codec = dai->codec;
	struct pcm1690_private *priv = snd_soc_codec_get_drvdata(codec);
	int val;

	if (mute)
		val = pcm1690_SOFT_MUTE_ALL;
	else
		val = 0;

	return regmap_write(priv->regmap, pcm1690_SOFT_MUTE, val);
}


//pcm1691.pdf:
/*
FMTDA Audio interface format selection
0000 16-/20-/24-/32-bit I2S format (default)
0001 16-/20-/24-/32-bit left-justified format
0010 24-bit right-justified format
0011 16-bit right-justified format
0100 24-bit I2S mode DSP format
0101 24-bit left-justified mode DSP format
0110 24-bit I2S mode TDM format
0111 24-bit left-justified mode TDM format
1000 24-bit high-speed I2S mode TDM format
1001 24-bit high-speed left-justified mode TDM format
101x Reserved
11xx Reserved
1    Slow roll-off
*/
static int pcm1690_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct pcm1690_private *priv = snd_soc_codec_get_drvdata(codec);
	int val = 0, ret;

	priv->rate = params_rate(params);
	switch (priv->format & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		switch (params_width(params)) {
			val = 0x00;//16-/20-/24-/32-bit I2S format (default)
		}
	break;
	case SND_SOC_DAIFMT_RIGHT_J:
		switch (params_width(params)) {
		case 24:
			val = 0x02; //24-bit right-justified format
			break;
		case 16:
			val = 0x03; //16-bit right-justified format
			break;
		default:
			dev_err(codec->dev, "Invalid sound output format on right justified\n");
			return -EINVAL;
		}
	break;
	case SND_SOC_DAIFMT_LEFT_J:
		val = 0x01;//16-/20-/24-/32-bit left-justified format
		break;
	case SND_SOC_DAIFMT_DSP_A:
		switch (params_width(params)) {
		case 24:
			val = 0x04; //24-bit I2S mode DSP format
			break;
		default:
			dev_err(codec->dev, "Invalid sound output format on dsp_a\n");
			return -EINVAL;
		}
		break;
	default:
		dev_err(codec->dev, "Invalid DAI format\n");
		return -EINVAL;
	}
	ret = regmap_update_bits(priv->regmap, pcm1690_FMT_CONTROL, 0x0f, val);
	if (ret < 0)
		return ret;
	printk("+++++++++++++++pcm1690_hw_params pcm1690_FMT_CONTROL 0x41 -> %x\n",val);
	return pcm1690_set_deemph(codec);
}

static const struct snd_soc_dai_ops pcm1690_dai_ops = {
	.set_fmt	= pcm1690_set_dai_fmt,
	.hw_params	= pcm1690_hw_params,
	.digital_mute	= pcm1690_digital_mute,
};

static const struct snd_soc_dapm_widget pcm1690_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("VOUT1"),
	SND_SOC_DAPM_OUTPUT("VOUT2"),
	SND_SOC_DAPM_OUTPUT("VOUT3"),
	SND_SOC_DAPM_OUTPUT("VOUT4"),
	SND_SOC_DAPM_OUTPUT("VOUT5"),
	SND_SOC_DAPM_OUTPUT("VOUT6"),
	SND_SOC_DAPM_OUTPUT("VOUT7"),
	SND_SOC_DAPM_OUTPUT("VOUT8"),
};

static const struct snd_soc_dapm_route pcm1690_dapm_routes[] = {
	{ "VOUT1", NULL, "Playback" },
	{ "VOUT2", NULL, "Playback" },
	{ "VOUT3", NULL, "Playback" },
	{ "VOUT4", NULL, "Playback" },
	{ "VOUT5", NULL, "Playback" },
	{ "VOUT6", NULL, "Playback" },
	{ "VOUT7", NULL, "Playback" },
	{ "VOUT8", NULL, "Playback" },
};

static const DECLARE_TLV_DB_SCALE(pcm1690_dac_tlv, -6350, 50, 1);

static const struct snd_kcontrol_new pcm1690_controls[] = {
	SOC_DOUBLE_R_TLV("Channel 1/2 Playback Volume",
			pcm1690_ATT_CONTROL(1), pcm1690_ATT_CONTROL(2), 0,
			0x7f, 0, pcm1690_dac_tlv),
	SOC_DOUBLE_R_TLV("Channel 3/4 Playback Volume",
			pcm1690_ATT_CONTROL(3), pcm1690_ATT_CONTROL(4), 0,
			0x7f, 0, pcm1690_dac_tlv),
	SOC_DOUBLE_R_TLV("Channel 5/6 Playback Volume",
			pcm1690_ATT_CONTROL(5), pcm1690_ATT_CONTROL(6), 0,
			0x7f, 0, pcm1690_dac_tlv),
	SOC_DOUBLE_R_TLV("Channel 7/8 Playback Volume",
			pcm1690_ATT_CONTROL(7), pcm1690_ATT_CONTROL(8), 0,
			0x7f, 0, pcm1690_dac_tlv),
	SOC_SINGLE_BOOL_EXT("De-emphasis Switch", 0,
			    pcm1690_get_deemph, pcm1690_put_deemph),
};

static struct snd_soc_dai_driver pcm1690_dai = {
	.name = "pcm1690-hifi",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 8,
		.rates = pcm1690_PCM_RATES,
		.formats = pcm1690_PCM_FORMATS,
	},
	.ops = &pcm1690_dai_ops,
};

#ifdef CONFIG_OF
static const struct of_device_id pcm1690_dt_ids[] = {
	{ .compatible = "ti,pcm1690", },
	{ }
};
MODULE_DEVICE_TABLE(of, pcm1690_dt_ids);
#endif

static const struct regmap_config pcm1690_regmap = {
	.reg_bits		= 8,
	.val_bits		= 8,
	.max_register		= 0x4F,
	.reg_defaults		= pcm1690_reg_defaults,
	.num_reg_defaults	= ARRAY_SIZE(pcm1690_reg_defaults),
	.writeable_reg		= pcm1690_writeable_reg,
	.readable_reg		= pcm1690_accessible_reg,
	//.cache_type		= REGCACHE_FLAT,
	.cache_type		= REGCACHE_RBTREE,
};

static struct snd_soc_codec_driver soc_codec_dev_pcm1690 = {
	.component_driver = {
		.controls		= pcm1690_controls,
		.num_controls		= ARRAY_SIZE(pcm1690_controls),
		.dapm_widgets		= pcm1690_dapm_widgets,
		.num_dapm_widgets	= ARRAY_SIZE(pcm1690_dapm_widgets),
		.dapm_routes		= pcm1690_dapm_routes,
		.num_dapm_routes	= ARRAY_SIZE(pcm1690_dapm_routes),
	},
};

static const struct i2c_device_id pcm1690_i2c_id[] = {
	{"pcm1690", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, pcm1690_i2c_id);


static int pcm1690_write_reg( struct i2c_client* client,uint8_t reg,uint8_t data)  
{  
    unsigned char buffer[2];    
    buffer[0] = reg;  
    buffer[1] = data;   
    if( 2!= i2c_master_send(client,buffer,2) ) {  
        printk( KERN_ERR "pcm1690_i2c_write fail! \n" );  
        return -1;  
    }      
    return 0;  
} 
static unsigned char pcm1690_read_reg(struct i2c_client *client, unsigned char reg)  
{  
    unsigned char buf;  
    i2c_master_send(client, &reg, 1);  // 发送寄存器地址  
    i2c_master_recv(client, &buf, 1);  // 接收寄存器的值  
  
    return  buf;  
}
static void display_cpm1690_reg(struct i2c_client *client){
	
	printk("+0x40 -> %x\n",pcm1690_read_reg(client,0x40));
	printk("+0x41 -> %x\n",pcm1690_read_reg(client,0x41));
	printk("+0x42 -> %x\n",pcm1690_read_reg(client,0x42));
	printk("+0x43 -> %x\n",pcm1690_read_reg(client,0x43));
	printk("+0x44 -> %x\n",pcm1690_read_reg(client,0x44));
	printk("+0x45 -> %x\n",pcm1690_read_reg(client,0x45));
	printk("+0x46 -> %x\n",pcm1690_read_reg(client,0x46));
	printk("+0x47 -> %x\n",pcm1690_read_reg(client,0x47));
	printk("+0x48 -> %x\n",pcm1690_read_reg(client,0x48));
	printk("+0x49 -> %x\n",pcm1690_read_reg(client,0x49));
	printk("+0x4A -> %x\n",pcm1690_read_reg(client,0x4A));
	printk("+0x4B -> %x\n",pcm1690_read_reg(client,0x4B));
	printk("+0x4C -> %x\n",pcm1690_read_reg(client,0x4C));
	printk("+0x4D -> %x\n",pcm1690_read_reg(client,0x4D));
	printk("+0x4E -> %x\n",pcm1690_read_reg(client,0x4E));
	printk("+0x4F -> %x\n",pcm1690_read_reg(client,0x4F));
}

static int pcm1690_i2c_probe(struct i2c_client *client,
			      const struct i2c_device_id *id)
{
	int ret;
	struct pcm1690_private *priv;
	//printk("++++++++++++++++++++++++++++++pcm1690_i2c_probe++++++++++++++++++++++++\n");	
	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regmap = devm_regmap_init_i2c(client, &pcm1690_regmap);
	if (IS_ERR(priv->regmap)) {
		ret = PTR_ERR(priv->regmap);
		dev_err(&client->dev, "Failed to create regmap: %d\n", ret);
		return ret;
	}
	i2c_set_clientdata(client, priv);
	//display_cpm1690_reg(client);
	return snd_soc_register_codec(&client->dev, &soc_codec_dev_pcm1690,&pcm1690_dai, 1);	
}

static int pcm1690_i2c_remove(struct i2c_client *client)
{
	snd_soc_unregister_codec(&client->dev);
	return 0;
}

static struct i2c_driver pcm1690_i2c_driver = {
	.driver = {
		.name	= "pcm1690",
		.of_match_table = of_match_ptr(pcm1690_dt_ids),
	},
	.id_table	= pcm1690_i2c_id,
	.probe		= pcm1690_i2c_probe,
	.remove		= pcm1690_i2c_remove,
};

module_i2c_driver(pcm1690_i2c_driver);

MODULE_DESCRIPTION("Texas Instruments pcm1690 ALSA SoC Codec Driver");
MODULE_AUTHOR("Marek Belisko <marek.belisko@streamunlimited.com>");
MODULE_LICENSE("GPL");
