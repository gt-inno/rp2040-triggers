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

The GUI connects to the Pico over its USB CDC serial port. Use **Read Status** after connecting, edit channel settings, then use **Apply CHx** or **Apply All** to send the matching firmware commands. Applied settings are saved to the Pico flash and loaded again on boot. Profiles can also be saved and loaded as JSON on the PC.

The GUI can configure each channel's input GPIO, output GPIO, input pull, input edge, output polarity, delay, width, edge-count trigger settings, optional step reduction, enabled state, RP2040 system clock, and SWCLK diagnostic captures.

The firmware emulates EEPROM-style storage in the Pico's flash. It keeps only trigger configuration and the selected system clock, not runtime state such as event counters, pending pulses, live pin levels, or the armed state. The unit always boots disarmed.

Use `freq <khz>` to change the RP2040 system clock while disarmed. The firmware accepts attainable Pico SDK PLL frequencies from 48000 to 200000 kHz; for example, `freq 200000` runs the system clock at 200 MHz. Use `freq` or `status` to read the current clock.

The onboard/status LED turns on while armed. While disarmed, it gives a short heartbeat blink every 2 seconds. Trigger events also blink the LED briefly. On boards with a single-color Pico LED this may not appear red; on boards with a red status LED, that LED will blink.

Each channel can run in `time` mode or `edge_count` mode. Time mode uses the existing GPIO edge interrupt, the selected `edge` setting, `delay_us`, and `width_us`.

The two trigger modes are mutually exclusive per channel. In the GUI, choose **Activation -> Delay** to use time/delay mode, or **Activation -> Edge Count** to use rising-edge counter mode. Selecting one mode disables/ignores the other mode for that channel. To disable a trigger channel completely, clear its **Enabled** checkbox. Over serial, use `set <ch> mode time`, `set <ch> mode edge_count`, or `set <ch> enabled 0`, then `save` if the change should persist after reboot.

Edge-count mode uses RP2040 PWM hardware to count rising edges on the input pin. The `edge` and `delay_us` settings are not used in this mode; the trigger is rising-edge count based. After arming, it waits for the first SWCLK/input rising edge, counts rising edges, sets the output active when `edge_count` is reached, keeps the output active for `pulse_width_edges` more rising edges, then returns the output idle. It is not one-shot per arm: after each output pulse completes, the counter resets to zero and waits for the next rising-edge sequence while the unit remains armed. The default edge-count target is `16742`; the default pulse width is `100` edges.

The GUI's **Auto Clear** option is enabled by default for edge-count mode. After an edge-count output pulse finishes, the firmware waits `auto_clear_delay_ns` before clearing/restarting the PWM edge counter. The default delay is `10000000 ns`. This is intended to discard tail-edge residue after a trigger while keeping the existing repeating edge-count behavior. Over serial, use `set <ch> auto_clear_edges 0|1` and `set <ch> auto_clear_delay_ns <value>`.

The GUI's **Step Reduce** option is disabled by default. When enabled, the firmware counts completed trigger pulses for that channel. After every `step_reduce_every` pulses, it reduces the next live edge-count threshold by `step_reduce_edge_delta` in `edge_count` mode, or reduces the next live delay by `step_reduce_delay_ns` in `time` mode. The saved base `edge_count` and `delay_us` values are not rewritten by this runtime sweep; arming starts again from the saved base values. The default controls are `step_reduce_every=4`, `step_reduce_edge_delta=1`, and `step_reduce_delay_ns=1`.

The edge-count values are capped at `65535` for both `edge_count` and `pulse_width_edges`. This is a hardware limit from the RP2040 PWM counter/wrap register, which is 16-bit. If SWCLK/input edges stop while the output is active, the output remains active until `pulse_width_edges` more rising edges arrive, or until the channel is disabled, reconfigured, or the unit is disarmed.

For `edge_count` mode, the input GPIO must be a PWM B-channel pin. On RP2040 these are odd-numbered GPIOs such as GP1, GP3, GP5, GP7, GP9, GP11, GP13, GP15, GP17, GP19, GP21, GP23, GP25, GP27, and GP29. Because GP25 is commonly the onboard LED, this firmware still rejects it for trigger input. Practical SWCLK choices include GP3, GP5, GP7, GP9, and similar unused odd GPIOs.

Only one edge-count channel can use a given PWM slice. For example, GP3 and GP19 are both PWM slice 1 B-channel pins, so they cannot both be used for edge-count channels at the same time. The firmware and GUI reject overlapping GPIOs and overlapping edge-count PWM slices.

Use `clear_edges <ch>` or the GUI's **Clear Edges** button to reset the current edge counter for that channel. If the channel is armed in edge-count mode, this restarts the count from zero and drives the output back to idle.

Use the GUI's **Start Monitor** and **Stop Monitor** buttons, or the serial commands `monitor_start <ch>`, `monitor_status`, and `monitor_stop`, to measure the rising-edge rate on a trigger input. This is intended for SWCLK sampling before choosing `edge_count` and `pulse_width_edges`. Monitoring requires the unit to be disarmed and the selected input must be a PWM B-channel pin such as GP3, GP5, or GP7. While monitoring, the GUI shows the measured rate, period, and total counted edges.

The **Diagnostics** section provides a one-shot `edge_count_diagnostic` workflow for DAP/SWD timing. It is separate from the normal armed trigger modes. Configure it with a channel, PWM B input GPIO, output GPIO, `fire_after_edges`, `pulse_width_edges`, and `idle_gap_us`, then press **Arm Diagnostic**. The default diagnostic values are channel `1`, input `GP3`, output `GP10`, `fire_after_edges=4220`, `pulse_width_edges=120`, and `idle_gap_us=50`.

After diagnostic arming, the firmware waits for the first SWCLK rising edge, starts the edge count from zero, drives the output active when the count reaches `fire_after_edges`, keeps it active for `pulse_width_edges` more rising edges, then returns the output idle. If SWCLK stops at the trigger edge and the edge-counted pulse cannot reach `trigger_off`, a timer fallback estimates the SWCLK period and clears the output after `estimated_period * pulse_width_edges`, so `trigger_off_us` is still recorded. The transaction finishes only after no SWCLK rising edges arrive for `idle_gap_us`, then the firmware prints `DIAG_EVENT` lines and one `DIAG` summary line over USB serial. It does not repeat until another `diag_arm <ch>` command is sent.

Diagnostic `fire_after_edges` and `pulse_width_edges` are capped at `65535` for this first version. The diagnostic input must also be an odd PWM B-channel GPIO such as GP3, GP5, GP7, or GP9. The summary includes `trigger_edge`, total edge count, trigger on/off timestamps, last edge timestamp, estimated average SWCLK period, min/max sampled period, transaction duration, and fired state. The GUI displays completed `DIAG` summaries in a table and saves the table to CSV plus the raw diagnostic serial lines to a `.log` file.

The **Sweep Diagnostics** controls step through a range of trigger edges for PicoEMP/DAP timing work. Configure with `start_edge`, `stop_edge`, `step`, `pulse_width_edges`, and `idle_gap_us`, then press **Arm Sweep** for one DAP transaction. After each completed transaction, the firmware increments the target by `step`; press **Arm Sweep** again before the next DAP transaction. Defaults are `start_edge=4100`, `stop_edge=4300`, `step=5`, `pulse_width_edges=120`, and `idle_gap_us=5000`.

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
freq
freq 200000
save
load
factory_reset
config_status
clear_edges 1
monitor_start 2
monitor_status
monitor_stop
diag_config 1 3 10 4220 120 50
diag_arm 1
diag_sweep_config 1 3 10 4100 4300 5 120 5000
diag_sweep_arm 1
diag_status
diag_stop
set 1 input_gpio 2
set 1 output_gpio 10
set 1 mode time
set 1 mode edge_count
set 1 pull down
set 1 edge rising
set 1 edge falling
set 1 edge both
set 1 delay_us 50
set 1 width_us 100
set 1 edge_count 16742
set 1 pulse_width_edges 100
set 1 auto_clear_edges 1
set 1 auto_clear_delay_ns 10000000
set 1 step_reduce_enabled 0
set 1 step_reduce_every 4
set 1 step_reduce_edge_delta 1
set 1 step_reduce_delay_ns 1
set 1 enabled 1
set 1 idle low
set 1 active high
```

## Warning

RP2040 GPIO is 3.3V only. Use protection/level shifting for ECU/DAP/automotive signals above 3.3V.
