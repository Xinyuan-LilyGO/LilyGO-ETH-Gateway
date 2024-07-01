# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

list(APPEND SDKCONFIG_DEFAULTS "sdkconfig")

add_definitions(-DSX1302_SPI_HOST=SPI2_HOST)
add_definitions(-DPIN_NUM_MISO=9)
add_definitions(-DPIN_NUM_MOSI=11)
add_definitions(-DPIN_NUM_CLK=10)
add_definitions(-DPIN_NUM_CS=41)
# add_definitions(-DI2C_MASTER_SDA_IO=17)
# add_definitions(-DI2C_MASTER_SCL_IO=18)
add_definitions(-DGPS_UART_TXD=42)
add_definitions(-DGPS_UART_RXD=39)
add_definitions(-DSX1302_RESET_PIN=2)
add_definitions(-DSX1302_POWER_EN_PIN=GPIO_NUM_NC)
add_definitions(-DSX1302_CONFIG_PIN=0)
add_definitions(-DBOARD_SD_CS_PIN=12)
add_definitions(-DBLINK_GPIO=38)
# add_definitions(-DLED_BLUE_GPIO=GPIO_NUM_NC)
# add_definitions(-DLED_GREEN_GPIO=GPIO_NUM_NC)
# add_definitions(-DLED_RED_GPIO=GPIO_NUM_NC)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ESXP1302-Pkt-Fwd)
