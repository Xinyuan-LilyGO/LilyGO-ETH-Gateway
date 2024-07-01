
<h1 align = "center">🌟LilyGO ETH Gateway</h1>

### [EN](./README.MD)/CN

## 🏳️‍🌈News:

* 本项目是基于**T-ETH-ELite**和**T-ETH-ELite-Gateway-Shield**硬件组合的LoRa网关项目，以上两个硬件缺一不可

## 1️⃣ Quick Start

配置方法一:

1. 手机连接 ESP32S3 产生网络热点名称为:LilyGo-Gateway 密码:12345678 
2. 在浏览器中输入`192.168.4.1` 打开网关配置页面
3. 根据标题填写相对应的栏目,填写完成之后点击Apply
4. 点击重启按钮,网关将重启按照填写的参数运行

配置方法二:

1. 通过网线接入以太网接口
2. 打开Serial监视器,从串行监视器中得到连接的IP地址，使用同一个局域网的电脑在浏览器中输入串行打印的IP地址,打开网关配置页面
3. 根据标题填写相对应的栏目,填写完成之后点击Apply
4. 点击重启按钮,网关将重启按照填写的参数运行

网关配置参数解释:

- **Next time Boot** : 点击重启之后运行的模式是什么,
  * Soft AP Mode : 网络热点模式,只用于配置网关设置
  * Station Mode : 站模式,用于连接AP
  * Ethernet Mode ： 以太网模式,此模式可以不设置连接WiFi
- **Frequency Plan** : LoRa网关运行的频率计划, 请根据使用地的法律法规进行设置 
  * CN470 : Asia
  * EU868 : Europe
  * US915 : USA
  * 请注意,频率计划要与实际使用的SX1302网关频率适应，比如购买的868MHz的LoRa网关，只能配置为868MHz，不能配置为470MHz,915MHz
  * 其他计划请参考 [The Things Network Regional Parameters][1]
- **Radio 1 Center Frequency**: 射频中心频率设置 , 中心频率仅供参考,根据当地法律法规进行填写 , 如果使用[TTN][1] ， 可以在配置网关完成后下载全局配置文件，找到中心频率，将它填写到栏目中
  * CN470 : 470600000 Hz 
  * EU868 : 867500000 Hz 
  * US915 : 915600000 Hz 
  * 请注意,频率计划要与实际使用的SX1302网关频率适应，比如购买的868MHz的LoRa网关，只能配置为868MHz，不能配置为470MHz,915MHz
- **Radio 2 Center Frequency**: 射频中心频率设置 , 中心频率仅供参考,根据当地法律法规进行填写, 如果使用[TTN][1] ， 可以在配置网关完成后下载全局配置文件，找到中心频率，将它填写到栏目中
  * CN470 : 471400000 Hz
  * EU868 : 868500000 Hz
  * US915 : 916300000 Hz
  * 请注意,频率计划要与实际使用的SX1302网关频率适应，比如购买的868MHz的LoRa网关，只能配置为868MHz，不能配置为470MHz,915MHz
- **Wi-Fi SSID**: 无线ap名称
  * 如果 Next time Boot 选择为 Ethernet Mode 可以不填写,如果要使用无线接入模式,则填写WiFi SSDI
- **Wi-Fi Password**: 无线ap密码
  * 如果 Next time Boot 选择为 Ethernet Mode 可以不填写，如果要使用无线接入模式,则填写WiFi 密码
- **NS Host**: LoRa 网关服务器域名或者IP地址
  * [The Things Network][2] 在创建网关完成之后可以在网关界面查看接入域名
- **NS Port**: LoRa 网关服务器端口
  * [The Things Network][2] 默认使用 1700 作为通讯端口
- **Gateway ID**: 八个字节的网关ID，可以随意填写十六进制的八个字节,不能与其他的网关ID重复,例如 E84E06FFFE316166

[1]: https://www.thethingsnetwork.org/docs/lorawan/regional-parameters/
[2]: https://www.thethingsnetwork.org

## 2️⃣ Build project

1. 项目编译基于**Ubuntu 20.04.6 LTS**,其余操作系统请自行测试
2. 该示例依赖于 esp-idf V5.2.2，请参阅[入门指南](https://docs.espressif.com/projects/esp-idf/en/v5.2.2/esp32s3/index.html)了解配置和使用 esp-idf 构建项目的完整步骤。
3. 按照以下命令进行编译项目
    ```bash

    # 克隆esp-idf
    git clone -b v5.2.2 --recursive https://github.com/espressif/esp-idf.git

    # 进入idf目录
    cd esp-idf

    # 安装idf项目依赖
    ./install.sh

    # 导出idf环境变量
    . ./export.sh

    # 克隆项目
    git clone https://gitee.com/lewisxhe/esp32_sx1302.git

    # 进入项目目录
    cd esp32_sx1302

    # 编译
    idf.py build

    # 写入固件到板子
    idf.py -b 468000 -p /dev/ttyACM0 flash monitor

    ```
























