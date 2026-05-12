# RP2040 4-Trigger Firmware - No Picotool Build

This project uses the OEM Raspberry Pi Pico SDK, not Arduino.

This version intentionally avoids `pico_add_extra_outputs()` so Pico SDK 2.x does not try to build host tools such as picotool/pioasm.

Instead, it calls the prebuilt `elf2uf2.exe` included with the Raspberry Pi Pico SDK Windows installer.

## Default pins

| Channel | Input | Output |
|---|---:|---:|
| CH1 | GP2 | GP10 |
| CH2 | GP3 | GP11 |
| CH3 | GP4 | GP12 |
| CH4 | GP5 | GP13 |

## Build

Delete your old build folder first:

```powershell
cd C:\Users\gt-in\VScode-projects\rp2040-triggers
rmdir /s /q build
mkdir build
cd build
cmake .. -DPICO_SDK_PATH="C:\Program Files\Raspberry Pi\Pico SDK v2.2.0\pico-sdk"
cmake --build .
```

Output:

```text
rp2040_4trigger_oem_sdk.uf2
```

## Python GUI configurator

Install the GUI dependency:

```powershell
python -m pip install -r requirements.txt
```

Run the configurator:

```powershell
python tools\config_gui.py
```

The GUI connects to the Pico over its USB CDC serial port. Use **Read Status** after connecting, edit channel settings, then use **Apply CHx** or **Apply All** to send the matching firmware commands. Profiles can be saved and loaded as JSON on the PC.

The GUI can configure each channel's input GPIO, output GPIO, input pull, input edge, output polarity, delay, width, and enabled state.

For a quick oscilloscope check, connect the scope ground to Pico GND, probe the configured output pin, then use **Drive Active**. The output should hold its active level until **Drive Idle** is pressed. After that, use **Read Status** and check that the input level changes when you drive the configured input pin.

## Commands

```text
help
status
arm
disarm
fire 1
drive 1 active
drive 1 idle
set 1 input_gpio 2
set 1 output_gpio 10
set 1 pull down
set 1 edge rising
set 1 edge falling
set 1 edge both
set 1 delay_us 50
set 1 width_us 100
set 1 enabled 1
set 1 idle low
set 1 active high
```

## Warning

RP2040 GPIO is 3.3V only. Use protection/level shifting for ECU/DAP/automotive signals above 3.3V.
