

1、确定时钟源修改以下配置如：
	硬件采用外部24.576Mhz时钟，此时钟同时给mcasp模块与dac模块提供时钟。
	ret = snd_soc_dai_set_clkdiv(cpu_dai, MCASP_CLKDIV_BCLK, sysclk/evm_get_bclk(params));
		if (ret < 0){
			printk("+++++++++++++++can't set CPU DAI clock divider %d\n",ret);	
			return ret;
		}
	ret = snd_soc_dai_set_sysclk(cpu_dai, MCASP_CLK_HCLK_AHCLK, sysclk, SND_SOC_CLOCK_IN);//设置外部AUXCLK时钟输入到解码模块
		if (ret < 0){
			printk("+++++++++++++++can't set CPU DAI clock %d\n",ret);	
			return ret;
		}
2、添加
	mcasp0_pins: mcasp0_pins {
		pinctrl-single,pins = <
		.
		.//mcasp0 io 配置
		.
		>
	};
	&mcasp0	{
		pinctrl-names = "default";
		pinctrl-0 = <&mcasp0_pins>;
		status = "okay";
		op-mode = <0>;          /* MCASP_IIS_MODE DAVINCI_MCASP_IIS_MODE or DAVINCI_MCASP_DIT_MODE */
		tdm-slots = <2>;
		serial-dir = <  /* 0: INACTIVE, 1: TX, 2: RX */
			1 1 1 1
		>;
		tx-num-evt = <32>;
		rx-num-evt = <32>;
	};
	clk_mcasp0: clk_mcasp0 {
		#clock-cells = <0>;
		compatible = "gpio-gate-clock";
		enable-gpios = <&gpiox x 1>; //有源时钟开启管脚
	};
	sound {
		compatible = "ti,pcm1690-evm-audio";
		ti,model = "TI PCM1690";
		ti,audio-codec = <&pcm1690>;
		ti,mcasp-controller = <&mcasp0>;
		ti,codec-clock-rate = <24576000>;
	};
