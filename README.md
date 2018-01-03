


这是一个PCM1690的DAC声卡驱动，目前支持于AM335X mcasp接口平台，其它有待测试。<br>
驱动基于PCM1680修改而来，用于集成于ti am335x sdk开发包。<br>
<br><br>
如果你要使用此驱动需要做以下工作：<br>
1、参考<br>
http://processors.wiki.ti.com/index.php/Sitara_Linux_Audio_DAC_Example<br>
2、读step.txt<br>
说明：<br>
如果不采用24.576Mhz时钟，例如采用24Mhz那么支持的音频频率有问题，读取excel表，看看配置。<br>
		

		
		
