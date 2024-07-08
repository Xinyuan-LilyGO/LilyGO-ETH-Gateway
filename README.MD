<h1 align = "center">🌟LilyGO ETH Gateway</h1>

### [CN](./README_CN.MD)/EN

## 🏳️‍🌈News:

* This project is a LoRa gateway project based on the combination of **T-ETH-ELite** and **T-ETH-ELite-Gateway-Shield** hardware. The above two hardware are indispensable

## 1️⃣ Quick Start

Configuration method 1:

1. Connect the mobile phone to ESP32S3 to generate a network hotspot named: LilyGo-Gateway Password: 12345678

2. Enter `192.168.4.1` in the browser to open the gateway configuration page

3. Fill in the corresponding column according to the title, and click Apply after filling in

4. Click the restart button, the gateway will restart and run according to the filled parameters

Configuration method 2:

1. Connect to the Ethernet interface via a network cable

2. Open the Serial monitor, get the connected IP address from the serial monitor, use a computer on the same LAN to enter the serial printed IP address in the browser, and open the gateway configuration page
3. Fill in the corresponding column according to the title, and click Apply after filling in
4. Click the restart button, the gateway will restart and run according to the filled parameters

Gateway configuration parameter explanation:

- **Next time Boot**: What mode is running after clicking restart,
  * Soft AP Mode: Network hotspot mode, only used to configure gateway settings
  * Station Mode: Station mode, used to connect to AP
  * Ethernet Mode: Ethernet mode, this mode can not be set to connect to WiFi
- **Frequency Plan**: The frequency plan of the LoRa gateway operation, please set it according to the laws and regulations of the place of use
  * CN470: Asia
  * EU868: Europe
  * US915: USA
  * Please note that the frequency plan must be adapted to the actual frequency of the SX1302 gateway used. For example, the purchased 868MHz LoRa gateway can only be configured to 868MHz, not 470MHz, 915MHz
* For other plans, please refer to [The Things Network Regional Parameters][1]
- **Radio 1 Center Frequency**: RF center frequency setting, the center frequency is for reference only, fill in according to local laws and regulations, if using [TTN][1], you can download the global configuration file after configuring the gateway, find the center frequency, and fill it in the column
  * CN470: 470600000 Hz
  * EU868: 867500000 Hz
  * US915: 915600000 Hz
  * Please note that the frequency plan must be adapted to the actual SX1302 gateway frequency used. For example, the purchased 868MHz LoRa gateway can only be configured to 868MHz, not 470MHz, 915MHz
- **Radio 2 Center Frequency**: RF center frequency setting, the center frequency is for reference only, fill in according to local laws and regulations, if using [TTN][1], After configuring the gateway, you can download the global configuration file, find the center frequency, and fill it in the column
  * CN470: 471400000 Hz
  * EU868: 868500000 Hz
  * US915: 916300000 Hz
  * Please note that the frequency plan must be adapted to the actual frequency of the SX1302 gateway used. For example, the purchased 868MHz LoRa gateway can only be configured to 868MHz, not 470MHz, 915MHz
- **Wi-Fi SSID**: wireless ap name
  * If Next time Boot is selected as Ethernet Mode, you can leave it blank. If you want to use wireless access mode, fill in WiFi SSDI
- **Wi-Fi Password**: wireless ap password
  * If Next time Boot is selected as Ethernet Mode, you can leave it blank. If you want to use wireless access mode, fill in WiFi password
- **NS Host**: LoRa gateway server domain name or IP address
  * [The Things Network][2] After the gateway is created, you can view the access domain name in the gateway interface
- **NS Port**: LoRa gateway server port
  * [The Things Network][2] uses 1700 as the default communication port
- **Gateway ID**: 8-byte gateway ID, you can fill in the 8-byte hexadecimal format at will, and it cannot be repeated with other gateway IDs, such as E84E06FFFE316166

[1]: https://www.thethingsnetwork.org/docs/lorawan/regional-parameters/
[2]: https://www.thethingsnetwork.org

## 2️⃣ Build project

1. The project is compiled based on **Ubuntu 20.04.6 LTS**, please test it yourself for other operating systems
2. This example depends on esp-idf V5.2.2, please refer to the [Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/v5.2.2/esp32s3/index.html) for complete steps to configure and build a project with esp-idf.
3. Compile the project according to the following command
   
  ```bash

  # Clone esp-idf
  git clone -b v5.2.2 --recursive https://github.com/espressif/esp-idf.git

  # Enter the idf directory
  cd esp-idf

  # Install idf project dependencies
  ./install.sh

  # Export idf environment variables
  . ./export.sh

  # Clone the project
  git clone https://github.com/Xinyuan-LilyGO/LilyGO-ETH-Gateway.git

  # Enter the project directory
  cd LilyGO-ETH-Gateway

  # Compile
  idf.py build

  # Write firmware to the board
  idf.py -b 468000 -p /dev/ttyACM0 flash monitor

  ```