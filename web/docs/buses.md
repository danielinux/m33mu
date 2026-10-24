# Logic-level buses, devices and sniffers

I²C, SPI and UART traffic between the emulated STM32H563 and simulated parts is modelled at transaction level (`web/sim/buses.ts`). Wires and line levels decide who takes part; individual bit edges are not simulated.

## Routing

A peripheral reaches the wires through the header pad whose GPIO is in alternate-function mode with the AF that selects it. The table covers the enabled NUCLEO-H563ZI header pads, from STM32H563 datasheet DS14258 Rev 6 (Tables "Alternate function AF0 to AF7 / AF8 to AF15"):

| Pad (contact) | AF → signal |
|---|---|
| PB8 (CN7-2), PB6 (CN10-14) | AF4 I2C1_SCL |
| PB9 (CN7-4), PB7 (CN10-16) | AF4 I2C1_SDA |
| PF1 (CN9-19) / PF0 (CN9-21) | AF4 I2C2_SCL / I2C2_SDA |
| PA5 (CN7-10) | AF5 SPI1_SCK |
| PG9 (CN7-12), PA6 (CN9-1) | AF5 SPI1_MISO |
| PB5 (CN7-14), PD7 (CN9-2) | AF5 SPI1_MOSI |
| PD5 (CN9-6) / PD6 (CN9-4) | AF7 USART2_TX / USART2_RX |
| PB6 / PB7 (CN10-14/16) | AF8 LPUART1_TX / LPUART1_RX |

PD14 (CN7-16, "SPI_A_CS") has no SPI1 function: chip select is a GPIO, and a device is selected when its CS net reads low. USART3 is the ST-LINK virtual COM port on the board and always has a terminal tab.

The emulator side is a host hook in `mm_i2c_bus` / `mm_spi_bus` (`mm_web_set_bus_host()`); the worker installs it while a bench is active. UART bytes leave through the existing UART backend rings and are routed by the worker after each slice, so cross-UART latency is one scheduling slice rather than the byte time.

## Faults

| Situation | Result |
|---|---|
| SCL or SDA reads low while idle (no pull-up, line held low) | I²C BUSY stays set; the controller cannot start. Logged once per change. |
| No powered device at the address, or a device with SDA or SCL not connected | NACK (the sniffer names the missing line) |
| Device VCC − GND below 1.6 V | device absent (NACK / not selected) |
| Two I²C devices at one address | both act; reads are the wired-AND of their data; "address clash" |
| SPI: no device selected | MISO reads its pull level (0xFF high, 0x00 low or floating) |
| SPI: device MOSI unwired | device receives 0xFF |
| SPI: device MISO unwired | MCU reads the MISO line level |
| SPI: several devices selected | replies are AND-ed; "bus contention" |
| UART TX with nothing wired to it | bytes are dropped |

Firmware error handling then behaves as on hardware (HAL timeouts on BUSY, NACK flags). ARLO is reported by the I²C model when the host returns arbitration lost; no current device does.

## Devices

- **24C02 EEPROM** (`i2c-eeprom`, 0x50 + A2..A0): 256 bytes, 8-byte page wrap, pointer auto-increment.
- **TMP102** (`i2c-temp`, 0x48 + ADD0): register 0 is the set temperature in 1/16 °C.
- **W25Q SPI NOR** (`spi-flash`): JEDEC EF 40 18; 0x03 read, 0x06/0x04 WREN/WRDI, 0x02 page program, 0x05 status, 0x20 4 KiB erase; 64 KiB array.
- **USB-UART adapter** (`usb-uart`): a terminal tab; its TX idles high through 1 kΩ.
- **Bus sniffer** (`sniffer`): four clips; its tab lists every transaction on the nets it touches, with CSV export.

Electrically, modules draw about 33 µA from VCC; bus outputs of devices are not modelled as analog drivers.
