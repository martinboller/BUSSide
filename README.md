# BUSSide (Updated Fork)
License: GPLv3

An automated tool for hardware hacking. This repository is a modernized fork of the original **BUSSide** project developed by **Silvio Cesare**.

## Project Status: 2026 Update ## 
The original BUSSide was a ground-breaking tool for hardware security researchers. However hasn't been maintained for a while |  and dependencies evolved. This fork was created to ensure the tool remains accessible and functional in a modern ecosystem.

### Key Improvements:
* **Python 3 Migration:** Fully ported the client-side logic from Python 2.7 to Python 3.x.
* **C++ Maintenance:** Fixed compilation errors in the firmware (C++ pieces) caused by updated toolchains and deprecated libraries |  ensuring it builds cleanly in 2026 environments.
* **Dependency Refresh:** Updated requirements to align with current serial and communication libraries.

---

## Attribution
**Original Author:** [Silvio Cesare](https://github.com/silviocesare) - [The BUSSide can be found here](https://github.com/BSidesCbr/BUSSide)

Silvio did all the heavy lifting |  research |  and original implementation of the bus logic. This fork is maintained by [Martin Boller](https://github.com/martinboller) simply to keep the project alive and running on Python 3 and latest versions of the Arduino IDE.

---

## Overview
BUSSide is designed to interface with various hardware buses to assist in device exploitation and security auditing. It acts as a bridge between your workstation and the target's physical layer using a NodeMCU ESP 8226 v2 (v3 has a different pin layout |  but works on a breadboard).

### Supported Protocols
| Protocol | Capabilities |
| :--- | :--- |
| **UART** | Automated baud rate detection and interactive shell access. |
| **SPI** | Flash dumping |  peripheral interaction |  and sniffing. |
| **I2C** | Device address scanning and EEPROM data extraction. |
| **JTAG** | Pin Discovery. |

---

## Getting Started

### 1. Hardware Requirements
BUSSide has been tested on a NodeMCU v1 (often sold as v2 or even 3). You will need one of these devices connected to your host machine via USB.
[Here's one on AliExpress](https://www.aliexpress.com/item/1005010165879727.html). Used together with the original BUSSide board or the [Burtleina board by Luca Bongiorni](https://github.com/whid-injector/) you have a nice extra tool for Hardware Penetration Testing to verify what you've found (or not) with the [WHIDBoard](https://github.com/whid-injector/WHIDBOARD) or other tools.

### 2. Installation

#### NodeMCU Firmware

To flash the firmware (build instruction |  see [below](#3-building-from-source)):
```bash
git clone http://github.com/martinboller/BUSSide.git
apt-get install esptool
esptool --port /dev/ttyUSB0 write_flash 0x00000 BUSSide/FirmwareImages/*.bin
```

#### Client
Clone this repository and install the necessary Python dependencies:

```bash
git clone https://github.com/martinboller/BUSSide.git
cd BUSSide/Client/
pip install -r requirements.txt
```

Then run it with ``` python3 ./busside.py /dev/ttyUSB0 ``` adjust serial port to fit your setup.

### 3. Building from source
Install the latest Arduino IDE (or similar)
Make sure to 
- include library EspSoftwareSerial (Sketch -> Include Library -> select EspSoftwareSerial from list)
- Install the NodeMCU board into the IDE: *File -> Preferences* and in *Additional Boards Manager URLs* add http://arduino.esp8266.com/stable/package_esp8266com_index.json
- *Tools -> Boards -> Boards Manager* Search for esp8266 and install the package.
- Set the board to NodeMCU 1.0
- Set the speed to 160MHz (via the IDE in the Tools dropdown)
- Then Compile and/or upload directly to the NodeMCU.

### NodeMCU v3 issues

Had some issues on Ubuntu (not good ol' Debian) with NodeMCU v3 boards registering as ID 1a86:7523 QinHeng Electronics CH340 serial converter |  which conflicts with: 

``` sh
# Device: 1A86:7523

# Baum [NLS eReader Zoomax (20 cells)]

ENV{PRODUCT}=="1a86/7523/*" |  ENV{BRLTTY_BRAILLE_DRIVER}="bm" |  GOTO="brltty_usb_run"
```

edit the /usr/lib/udev/rules.d/85-brltty.rules and comment the last line out |  the reboot.

### UART Changes
Have changed to discover_tx function in uart.ino to send 2 CR+LF's to all pins in succession and listening for a response consisting of 2 ASCII characters (or more) on the designated RX pin.

1. Discovering RX on the BUSSide (TX on the DUT)

![](/images/discover_rx.png)

2. Then use the obtained information to discover TX on the BUSSide (RX on the DUT)

![](/images/discover_tx.png)


Use pin 4 and 5 (on the Burtlëina board) for UART testing as other pins can be a PITA.

## GPIO Mapping

Pin Index | Label | Actual GPIO | Conflict / Status
---|---|---|---|
1 | D0 | 0 | "Boot Pin. High = Run |  Low = Flash."
2 | D1 | 1 | Hardware TX. Used by your PC/Python script!
3 | D2 | 2 | Boot Pin / Internal LED.
4 | D3 | 3 | Hardware RX. Used by your PC/Python script!
5 | D4 | 4 | Safe.
6 | D5 | 5 | Safe.
7 | D6 | 12 |
8 | D7 | 13 |
9 | D8 | 16 |