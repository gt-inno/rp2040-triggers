"""Tkinter GUI for configuring the RP2040 4-trigger firmware over USB serial."""

from __future__ import annotations

import csv
import json
import queue
import re
import threading
import tkinter as tk
from dataclasses import dataclass
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from typing import Any

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # The GUI can still open and explain how to install pyserial.
    serial = None
    list_ports = None


CHANNEL_COUNT = 4
TRIGGER_MODE_VALUES = ("time", "edge_count")
EDGE_VALUES = ("rising", "falling", "both")
LEVEL_VALUES = ("low", "high")
PULL_VALUES = ("down", "up", "none")
PROFILE_SCHEMA = 6
DEFAULT_CLOCK_FREQ_KHZ = 125000
CLOCK_FREQ_MIN_KHZ = 48000
CLOCK_FREQ_MAX_KHZ = 200000
DEFAULT_EDGE_COUNT = 16742
DEFAULT_PULSE_WIDTH_EDGES = 100
DEFAULT_AUTO_CLEAR_DELAY_NS = 10000000
DEFAULT_STEP_REDUCE_EVERY = 4
DEFAULT_STEP_REDUCE_EDGE_DELTA = 1
DEFAULT_STEP_REDUCE_DELAY_NS = 1
EDGE_COUNT_MAX = 65535
TRIGGER_PANEL_HEIGHT = 220
DEFAULT_DIAG_CHANNEL = 1
DEFAULT_DIAG_INPUT_GPIO = 3
DEFAULT_DIAG_OUTPUT_GPIO = 10
DEFAULT_DIAG_FIRE_AFTER_EDGES = 4220
DEFAULT_DIAG_PULSE_WIDTH_EDGES = 120
DEFAULT_DIAG_IDLE_GAP_US = 50
DEFAULT_SWEEP_START_EDGE = 4100
DEFAULT_SWEEP_STOP_EDGE = 4300
DEFAULT_SWEEP_STEP = 5
DEFAULT_SWEEP_IDLE_GAP_US = 5000
DIAG_COLUMNS = (
    "run_index",
    "trigger_edge",
    "ch",
    "total_edges",
    "fire_after_edges",
    "pulse_width_edges",
    "first_us",
    "trigger_on_us",
    "trigger_off_us",
    "last_edge_us",
    "idle_gap_us",
    "duration_us",
    "avg_swclk_period_ns",
    "min_period_ns",
    "max_period_ns",
    "fired",
)

CLOCK_RE = re.compile(r"^Clock:\s+sys_khz=(?P<clock_freq_khz>\d+)")
FREQ_OK_RE = re.compile(r"^\[OK\]\s+freq=(?P<clock_freq_khz>\d+)\s+kHz")
MONITOR_RE = re.compile(
    r"^MON:\s+"
    r"active=(?P<active>[01])\s+"
    r"ch=(?P<ch>[0-4])\s+"
    r"input=GP(?P<input_gpio>\d+)\s+"
    r"edges=(?P<edges>\d+)\s+"
    r"elapsed_us=(?P<elapsed_us>\d+)\s+"
    r"rate_hz=(?P<rate_hz>\d+)\s+"
    r"period_ns=(?P<period_ns>\d+)"
)

DIAG_EVENT_RE = re.compile(
    r"^DIAG_EVENT\s+"
    r"ch=(?P<ch>\d+)\s+"
    r"type=(?P<type>[a-z_]+)\s+"
    r"edge=(?P<edge>\d+)\s+"
    r"us=(?P<us>\d+)"
)

STATUS_RE = re.compile(
    r"^CH(?P<ch>[1-4]):\s+"
    r"enabled=(?P<enabled>[01])\s+"
    r"input=GP(?P<input_gpio>\d+)\s+"
    r"output=GP(?P<output_gpio>\d+)\s+"
    r"mode=(?P<trigger_mode>time|edge_count)\s+"
    r"edge=(?P<edge>rising|falling|both)\s+"
    r"delay_us=(?P<delay_us>\d+)\s+"
    r"width_us=(?P<width_us>\d+)\s+"
    r"edge_count=(?P<edge_count>\d+)\s+"
    r"pulse_width_edges=(?P<pulse_width_edges>\d+)\s+"
    r"(?:auto_clear_edges=(?P<auto_clear_edges>[01])\s+"
    r"auto_clear_delay_ns=(?P<auto_clear_delay_ns>\d+)\s+)?"
    r"(?:step_reduce_enabled=(?P<step_reduce_enabled>[01])\s+"
    r"step_reduce_every=(?P<step_reduce_every>\d+)\s+"
    r"step_reduce_edge_delta=(?P<step_reduce_edge_delta>\d+)\s+"
    r"step_reduce_delay_ns=(?P<step_reduce_delay_ns>\d+)\s+"
    r"step_reduce_count=(?P<step_reduce_count>\d+)\s+"
    r"step_current_edge_count=(?P<step_current_edge_count>\d+)\s+"
    r"step_current_delay_ns=(?P<step_current_delay_ns>\d+)\s+)?"
    r"edge_seen=(?P<edge_seen>\d+)\s+"
    r"idle=(?P<idle>low|high)\s+"
    r"active=(?P<active>low|high)\s+"
    r"pending=(?P<pending>[01])\s+"
    r"events=(?P<events>\d+)\s+"
    r"last_event_us=(?P<last_event_us>\d+)"
    r"(?:\s+input_level=(?P<input_level>[01])\s+"
    r"output_level=(?P<output_level>[01])\s+"
    r"pull=(?P<pull>none|up|down))?"
)


class Tooltip:
    def __init__(self, widget: tk.Widget, text: str, delay_ms: int = 450) -> None:
        self.widget = widget
        self.text = text
        self.delay_ms = delay_ms
        self.after_id: str | None = None
        self.window: tk.Toplevel | None = None

        widget.bind("<Enter>", self._schedule, add="+")
        widget.bind("<Leave>", self._hide, add="+")
        widget.bind("<ButtonPress>", self._hide, add="+")

    def _schedule(self, _event: tk.Event | None = None) -> None:
        self._cancel()
        self.after_id = self.widget.after(self.delay_ms, self._show)

    def _cancel(self) -> None:
        if self.after_id is not None:
            self.widget.after_cancel(self.after_id)
            self.after_id = None

    def _show(self) -> None:
        if self.window is not None:
            return

        x = self.widget.winfo_rootx() + 18
        y = self.widget.winfo_rooty() + self.widget.winfo_height() + 8
        self.window = tk.Toplevel(self.widget)
        self.window.wm_overrideredirect(True)
        self.window.wm_geometry(f"+{x}+{y}")

        label = tk.Label(
            self.window,
            text=self.text,
            justify="left",
            wraplength=320,
            background="#ffffe0",
            relief="solid",
            borderwidth=1,
            padx=8,
            pady=5,
        )
        label.pack()

    def _hide(self, _event: tk.Event | None = None) -> None:
        self._cancel()
        if self.window is not None:
            self.window.destroy()
            self.window = None


@dataclass
class ChannelVars:
    enabled: tk.BooleanVar
    input_gpio: tk.StringVar
    output_gpio: tk.StringVar
    trigger_mode: tk.StringVar
    pull: tk.StringVar
    edge: tk.StringVar
    delay_us: tk.StringVar
    width_us: tk.StringVar
    edge_count: tk.StringVar
    pulse_width_edges: tk.StringVar
    auto_clear_edges: tk.BooleanVar
    auto_clear_delay_ns: tk.StringVar
    step_reduce_enabled: tk.BooleanVar
    step_reduce_every: tk.StringVar
    step_reduce_edge_delta: tk.StringVar
    step_reduce_delay_ns: tk.StringVar
    step_reduce_count: tk.StringVar
    step_current_edge_count: tk.StringVar
    step_current_delay_ns: tk.StringVar
    idle: tk.StringVar
    active: tk.StringVar
    input_level: tk.StringVar
    output_level: tk.StringVar
    pending: tk.StringVar
    events: tk.StringVar
    edge_seen: tk.StringVar
    monitor_rate: tk.StringVar
    monitor_period: tk.StringVar
    monitor_edges: tk.StringVar
    last_event_us: tk.StringVar


class TriggerConfigurator(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("RP2040 Trigger Configurator")
        self.minsize(1100, 820)

        self.serial_conn: Any | None = None
        self.serial_lock = threading.Lock()
        self.reader_stop = threading.Event()
        self.reader_thread: threading.Thread | None = None
        self.rx_queue: queue.Queue[tuple[str, str]] = queue.Queue()

        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value="115200")
        self.connection_var = tk.StringVar(value="Disconnected")
        self.armed_var = tk.StringVar(value="Unknown")
        self.clock_freq_var = tk.StringVar(value=str(DEFAULT_CLOCK_FREQ_KHZ))
        self.manual_command_var = tk.StringVar()
        self.connected_widgets: list[ttk.Widget] = []
        self.pending_load_refresh = False
        self.pending_clock_save = False
        self.pending_clock_refresh = False
        self.monitoring_channel: int | None = None
        self.diag_channel_var = tk.StringVar(value=str(DEFAULT_DIAG_CHANNEL))
        self.diag_input_gpio_var = tk.StringVar(value=str(DEFAULT_DIAG_INPUT_GPIO))
        self.diag_output_gpio_var = tk.StringVar(value=str(DEFAULT_DIAG_OUTPUT_GPIO))
        self.diag_fire_after_edges_var = tk.StringVar(
            value=str(DEFAULT_DIAG_FIRE_AFTER_EDGES)
        )
        self.diag_pulse_width_edges_var = tk.StringVar(
            value=str(DEFAULT_DIAG_PULSE_WIDTH_EDGES)
        )
        self.diag_idle_gap_us_var = tk.StringVar(value=str(DEFAULT_DIAG_IDLE_GAP_US))
        self.sweep_start_edge_var = tk.StringVar(value=str(DEFAULT_SWEEP_START_EDGE))
        self.sweep_stop_edge_var = tk.StringVar(value=str(DEFAULT_SWEEP_STOP_EDGE))
        self.sweep_step_var = tk.StringVar(value=str(DEFAULT_SWEEP_STEP))
        self.sweep_pulse_width_edges_var = tk.StringVar(
            value=str(DEFAULT_DIAG_PULSE_WIDTH_EDGES)
        )
        self.sweep_idle_gap_us_var = tk.StringVar(value=str(DEFAULT_SWEEP_IDLE_GAP_US))
        self.diag_output_path_var = tk.StringVar(value="diagnostic_log.csv")
        self.diag_rows: list[dict[str, str]] = []
        self.diag_raw_lines: list[str] = []
        self.diag_tree: ttk.Treeview | None = None

        self.channel_vars = [self._new_channel_vars(i) for i in range(CHANNEL_COUNT)]
        self.time_mode_widgets: list[list[ttk.Widget]] = [
            [] for _ in range(CHANNEL_COUNT)
        ]
        self.edge_count_mode_widgets: list[list[ttk.Widget]] = [
            [] for _ in range(CHANNEL_COUNT)
        ]

        self._configure_style()
        self._build_layout()
        for index in range(CHANNEL_COUNT):
            self._update_channel_mode_ui(index)
        self.refresh_ports()
        self._set_connected_state(False)
        self.after(50, self._poll_serial_queue)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def _configure_style(self) -> None:
        style = ttk.Style(self)
        if "vista" in style.theme_names():
            style.theme_use("vista")
        style.configure("Header.TLabel", font=("Segoe UI", 11, "bold"))
        style.configure("Status.TLabel", padding=(8, 4))
        style.configure("Danger.TButton", foreground="#7a1b1b")

    def _add_tooltip(self, text: str, *widgets: tk.Widget) -> None:
        for widget in widgets:
            Tooltip(widget, text)

    def _new_channel_vars(self, index: int) -> ChannelVars:
        return ChannelVars(
            enabled=tk.BooleanVar(value=True),
            input_gpio=tk.StringVar(value=str(2 + index)),
            output_gpio=tk.StringVar(value=str(10 + index)),
            trigger_mode=tk.StringVar(value="time"),
            pull=tk.StringVar(value="down"),
            edge=tk.StringVar(value="rising"),
            delay_us=tk.StringVar(value="0"),
            width_us=tk.StringVar(value="100"),
            edge_count=tk.StringVar(value=str(DEFAULT_EDGE_COUNT)),
            pulse_width_edges=tk.StringVar(value=str(DEFAULT_PULSE_WIDTH_EDGES)),
            auto_clear_edges=tk.BooleanVar(value=True),
            auto_clear_delay_ns=tk.StringVar(value=str(DEFAULT_AUTO_CLEAR_DELAY_NS)),
            step_reduce_enabled=tk.BooleanVar(value=False),
            step_reduce_every=tk.StringVar(value=str(DEFAULT_STEP_REDUCE_EVERY)),
            step_reduce_edge_delta=tk.StringVar(value=str(DEFAULT_STEP_REDUCE_EDGE_DELTA)),
            step_reduce_delay_ns=tk.StringVar(value=str(DEFAULT_STEP_REDUCE_DELAY_NS)),
            step_reduce_count=tk.StringVar(value="0"),
            step_current_edge_count=tk.StringVar(value=str(DEFAULT_EDGE_COUNT)),
            step_current_delay_ns=tk.StringVar(value="0"),
            idle=tk.StringVar(value="low"),
            active=tk.StringVar(value="high"),
            input_level=tk.StringVar(value="0"),
            output_level=tk.StringVar(value="0"),
            pending=tk.StringVar(value="0"),
            events=tk.StringVar(value="0"),
            edge_seen=tk.StringVar(value="0"),
            monitor_rate=tk.StringVar(value="0 Hz"),
            monitor_period=tk.StringVar(value="0 ns"),
            monitor_edges=tk.StringVar(value="0"),
            last_event_us=tk.StringVar(value="0"),
        )

    def _build_layout(self) -> None:
        root = ttk.Frame(self, padding=12)
        root.grid(row=0, column=0, sticky="nsew")
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)
        root.columnconfigure(0, weight=1)
        root.rowconfigure(2, weight=1)

        self._build_connection_bar(root)
        self._build_action_bar(root)

        body = ttk.PanedWindow(root, orient="vertical")
        body.grid(row=2, column=0, pady=(10, 0), sticky="nsew")

        controls = ttk.Frame(body)
        controls.columnconfigure(0, weight=1)
        controls.rowconfigure(1, weight=1)

        console = ttk.Frame(body)
        console.columnconfigure(0, weight=1)
        console.rowconfigure(0, weight=1)

        body.add(controls, weight=5)
        body.add(console, weight=1)

        self._build_profile_bar(controls)

        work_tabs = ttk.Notebook(controls)
        work_tabs.grid(row=1, column=0, pady=(10, 0), sticky="nsew")

        triggers_tab = ttk.Frame(work_tabs)
        triggers_tab.columnconfigure(0, weight=1)
        triggers_tab.rowconfigure(0, weight=1)

        diagnostics_tab = ttk.Frame(work_tabs)
        diagnostics_tab.columnconfigure(0, weight=1)
        diagnostics_tab.rowconfigure(0, weight=1)

        work_tabs.add(triggers_tab, text="Triggers")
        work_tabs.add(diagnostics_tab, text="Diagnostics")

        self._build_channel_grid(triggers_tab)
        self._build_diagnostics_panel(diagnostics_tab)
        self._build_console(console)

    def _build_connection_bar(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="Connection", padding=10)
        frame.grid(row=0, column=0, sticky="ew")
        frame.columnconfigure(1, weight=1)

        ttk.Label(frame, text="Port").grid(row=0, column=0, padx=(0, 6), sticky="w")
        self.port_combo = ttk.Combobox(frame, textvariable=self.port_var, width=38)
        self.port_combo.grid(row=0, column=1, padx=(0, 8), sticky="ew")

        ttk.Button(frame, text="Refresh", command=self.refresh_ports).grid(
            row=0, column=2, padx=(0, 8)
        )

        ttk.Label(frame, text="Baud").grid(row=0, column=3, padx=(0, 6), sticky="w")
        self.baud_combo = ttk.Combobox(
            frame,
            textvariable=self.baud_var,
            values=("9600", "57600", "115200", "230400", "460800", "921600"),
            width=10,
        )
        self.baud_combo.grid(row=0, column=4, padx=(0, 8))

        self.connect_button = ttk.Button(frame, text="Connect", command=self.toggle_connection)
        self.connect_button.grid(row=0, column=5, padx=(0, 8))

        self.status_button = ttk.Button(frame, text="Read Status", command=self.read_status)
        self.status_button.grid(row=0, column=6, padx=(0, 8))
        self.connected_widgets.append(self.status_button)

        ttk.Label(frame, textvariable=self.connection_var, style="Status.TLabel").grid(
            row=0, column=7, sticky="e"
        )

    def _build_action_bar(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="Device", padding=10)
        frame.grid(row=1, column=0, pady=(10, 0), sticky="ew")
        frame.columnconfigure(8, weight=1)

        self.arm_button = ttk.Button(frame, text="Arm", command=lambda: self.send_command("arm"))
        self.arm_button.grid(row=0, column=0, padx=(0, 8))
        self.connected_widgets.append(self.arm_button)

        self.disarm_button = ttk.Button(
            frame, text="Disarm", command=lambda: self.send_command("disarm")
        )
        self.disarm_button.grid(row=0, column=1, padx=(0, 8))
        self.connected_widgets.append(self.disarm_button)

        ttk.Separator(frame, orient="vertical").grid(row=0, column=2, padx=8, sticky="ns")

        for index in range(CHANNEL_COUNT):
            fire_button = ttk.Button(
                frame,
                text=f"Fire CH{index + 1}",
                command=lambda ch=index: self.fire_channel(ch),
            )
            fire_button.grid(row=0, column=3 + index, padx=(0, 8))
            self.connected_widgets.append(fire_button)

        ttk.Label(frame, text="Armed").grid(row=0, column=7, padx=(16, 6), sticky="e")
        ttk.Label(frame, textvariable=self.armed_var, style="Status.TLabel").grid(
            row=0, column=8, sticky="w"
        )

        clock_label = ttk.Label(frame, text="Clock kHz")
        clock_label.grid(row=0, column=9, padx=(16, 6), sticky="e")
        clock_spin = ttk.Spinbox(
            frame,
            textvariable=self.clock_freq_var,
            from_=CLOCK_FREQ_MIN_KHZ,
            to=CLOCK_FREQ_MAX_KHZ,
            increment=1000,
            width=10,
        )
        clock_spin.grid(row=0, column=10, padx=(0, 8))
        self._add_tooltip(
            "RP2040 system clock in kHz. Higher values can improve timing resolution, "
            "but use only frequencies accepted by the firmware.",
            clock_label,
            clock_spin,
        )

        clock_button = ttk.Button(frame, text="Set Clock", command=self.apply_clock)
        clock_button.grid(row=0, column=11)
        self.connected_widgets.append(clock_button)

    def _build_channel_grid(self, parent: ttk.Frame) -> None:
        frame = ttk.Frame(parent)
        frame.grid(row=0, column=0, sticky="nsew")
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(0, weight=1)

        canvas = tk.Canvas(frame, highlightthickness=0)
        scrollbar = ttk.Scrollbar(frame, orient="vertical", command=canvas.yview)
        channels_frame = ttk.Frame(canvas)
        window_id = canvas.create_window((0, 0), window=channels_frame, anchor="nw")

        channels_frame.bind(
            "<Configure>",
            lambda _event: canvas.configure(scrollregion=canvas.bbox("all")),
        )
        canvas.bind(
            "<Configure>",
            lambda event: canvas.itemconfigure(window_id, width=event.width),
        )
        canvas.configure(yscrollcommand=scrollbar.set)

        canvas.grid(row=0, column=0, sticky="nsew")
        scrollbar.grid(row=0, column=1, sticky="ns")
        channels_frame.columnconfigure(0, weight=1)
        self._bind_canvas_mousewheel(canvas)

        for index, vars_ in enumerate(self.channel_vars):
            channel = ttk.LabelFrame(channels_frame, text=f"Trigger {index + 1}", padding=10)
            channel.grid(row=index, column=0, padx=(0, 6), pady=6, sticky="ew")
            channel.columnconfigure(0, weight=1)
            channel.rowconfigure(0, weight=1)
            channels_frame.rowconfigure(index, weight=0)

            trigger_canvas = tk.Canvas(
                channel,
                height=TRIGGER_PANEL_HEIGHT,
                highlightthickness=0,
            )
            trigger_scrollbar = ttk.Scrollbar(
                channel,
                orient="vertical",
                command=trigger_canvas.yview,
            )
            trigger_content = ttk.Frame(trigger_canvas)
            trigger_window_id = trigger_canvas.create_window(
                (0, 0),
                window=trigger_content,
                anchor="nw",
            )

            trigger_content.bind(
                "<Configure>",
                lambda _event, canvas=trigger_canvas: canvas.configure(
                    scrollregion=canvas.bbox("all")
                ),
            )
            trigger_canvas.bind(
                "<Configure>",
                lambda event, canvas=trigger_canvas, window_id=trigger_window_id:
                    canvas.itemconfigure(window_id, width=event.width),
            )
            trigger_canvas.configure(yscrollcommand=trigger_scrollbar.set)

            trigger_canvas.grid(row=0, column=0, sticky="nsew")
            trigger_scrollbar.grid(row=0, column=1, sticky="ns")
            self._build_channel_controls(trigger_content, index, vars_)
            self._bind_canvas_mousewheel(trigger_canvas, trigger_content, recursive=True)

    def _build_channel_controls(
        self, parent: ttk.Frame, index: int, vars_: ChannelVars
    ) -> None:
        for col in range(5):
            parent.columnconfigure(col, weight=1 if col < 4 else 0)

        pins = ttk.Frame(parent)
        time_settings = ttk.Frame(parent)
        edge_settings = ttk.Frame(parent)
        output_settings = ttk.Frame(parent)
        actions = ttk.Frame(parent)

        pins.grid(row=0, column=0, padx=(0, 16), sticky="nsew")
        time_settings.grid(row=0, column=1, padx=(0, 16), sticky="nsew")
        edge_settings.grid(row=0, column=2, padx=(0, 16), sticky="nsew")
        output_settings.grid(row=0, column=3, padx=(0, 16), sticky="nsew")
        actions.grid(row=0, column=4, sticky="nsew")

        for section in (pins, time_settings, edge_settings, output_settings):
            section.columnconfigure(1, weight=1)

        enabled_checkbox = ttk.Checkbutton(pins, text="Enabled", variable=vars_.enabled)
        enabled_checkbox.grid(row=0, column=0, columnspan=2, sticky="w")
        self._add_tooltip(
            "Enables this trigger channel. When unchecked, the firmware ignores this input "
            "and keeps the channel output idle while preserving the saved settings.",
            enabled_checkbox,
        )
        activation_label = ttk.Label(pins, text="Activation")
        activation_label.grid(row=1, column=0, columnspan=2, sticky="w", pady=(8, 2))
        self._add_tooltip(
            "Choose whether this channel fires by time delay after a GPIO edge or by counting "
            "rising SWCLK/input edges.",
            activation_label,
        )
        activation = ttk.Frame(pins)
        activation.grid(row=2, column=0, columnspan=2, sticky="ew")
        delay_radio = ttk.Radiobutton(
            activation,
            text="Delay",
            variable=vars_.trigger_mode,
            value="time",
            command=lambda ch=index: self._update_channel_mode_ui(ch),
        )
        delay_radio.grid(row=0, column=0, sticky="w")
        self._add_tooltip(
            "Delay mode uses the selected input edge, then waits delay_us before pulsing "
            "the output for width_us.",
            delay_radio,
        )
        edge_count_radio = ttk.Radiobutton(
            activation,
            text="Edge Count",
            variable=vars_.trigger_mode,
            value="edge_count",
            command=lambda ch=index: self._update_channel_mode_ui(ch),
        )
        edge_count_radio.grid(row=1, column=0, sticky="w", pady=(2, 0))
        self._add_tooltip(
            "Edge Count mode counts rising edges on a PWM B-capable input pin and fires "
            "when Count is reached.",
            edge_count_radio,
        )

        pins_label = ttk.Label(pins, text="Pins")
        pins_label.grid(row=3, column=0, columnspan=2, sticky="w", pady=(8, 2))
        input_label = ttk.Label(pins, text="Input GP")
        input_label.grid(row=4, column=0, sticky="w")
        input_spin = ttk.Spinbox(pins, textvariable=vars_.input_gpio, from_=0, to=29, width=8)
        input_spin.grid(row=4, column=1, padx=(8, 0), sticky="ew")
        self._add_tooltip(
            "GPIO used as the trigger input. Edge Count mode requires an odd PWM B-capable "
            "pin such as GP3, GP5, or GP7.",
            input_label,
            input_spin,
        )
        output_label = ttk.Label(pins, text="Output GP")
        output_label.grid(row=5, column=0, sticky="w", pady=(4, 0))
        output_spin = ttk.Spinbox(pins, textvariable=vars_.output_gpio, from_=0, to=29, width=8)
        output_spin.grid(row=5, column=1, padx=(8, 0), pady=(4, 0), sticky="ew")
        self._add_tooltip(
            "GPIO driven by this trigger. The output is driven to the configured Active level "
            "during a pulse and Idle otherwise.",
            output_label,
            output_spin,
        )
        pull_label = ttk.Label(pins, text="Pull")
        pull_label.grid(row=6, column=0, sticky="w", pady=(4, 0))
        pull_combo = ttk.Combobox(
            pins, textvariable=vars_.pull, values=PULL_VALUES, state="readonly", width=10
        )
        pull_combo.grid(row=6, column=1, padx=(8, 0), pady=(4, 0), sticky="ew")
        self._add_tooltip(
            "Internal input pull resistor. Use none when the external circuit already drives "
            "the line strongly.",
            pull_label,
            pull_combo,
        )

        time_title = ttk.Label(time_settings, text="Delay Mode")
        time_title.grid(row=0, column=0, columnspan=2, sticky="w")
        edge_label = ttk.Label(time_settings, text="Edge")
        edge_label.grid(row=1, column=0, sticky="w", pady=(8, 0))
        edge_combo = ttk.Combobox(
            time_settings, textvariable=vars_.edge, values=EDGE_VALUES, state="readonly", width=10
        )
        edge_combo.grid(row=1, column=1, padx=(8, 0), pady=(8, 0), sticky="ew")
        self._add_tooltip(
            "Input transition used in Delay mode. Ignored when Activation is Edge Count.",
            edge_label,
            edge_combo,
        )
        delay_label = ttk.Label(time_settings, text="Delay us")
        delay_label.grid(row=2, column=0, sticky="w", pady=(4, 0))
        delay_spin = ttk.Spinbox(
            time_settings, textvariable=vars_.delay_us, from_=0, to=4294967295, width=12
        )
        delay_spin.grid(row=2, column=1, padx=(8, 0), pady=(4, 0), sticky="ew")
        self._add_tooltip(
            "Delay in microseconds from the selected input edge to output activation in "
            "Delay mode.",
            delay_label,
            delay_spin,
        )
        width_label = ttk.Label(time_settings, text="Width us")
        width_label.grid(row=3, column=0, sticky="w", pady=(4, 0))
        width_spin = ttk.Spinbox(
            time_settings, textvariable=vars_.width_us, from_=1, to=4294967295, width=12
        )
        width_spin.grid(row=3, column=1, padx=(8, 0), pady=(4, 0), sticky="ew")
        self._add_tooltip(
            "Output pulse width in microseconds for Delay mode and manual Fire.",
            width_label,
            width_spin,
        )

        step_title = ttk.Label(time_settings, text="Step Reduce")
        step_title.grid(row=5, column=0, columnspan=2, sticky="w", pady=(12, 0))
        self._add_tooltip(
            "Optional runtime sweep. When enabled, the firmware counts completed trigger "
            "pulses on this channel and reduces the active timing after each group.",
            step_title,
        )
        step_checkbox = ttk.Checkbutton(
            time_settings,
            text="Enabled",
            variable=vars_.step_reduce_enabled,
        )
        step_checkbox.grid(row=6, column=0, columnspan=2, sticky="w", pady=(4, 0))
        self._add_tooltip(
            "Disabled by default. When enabled, the reduction starts from the saved base "
            "Count or Delay each time the firmware is armed.",
            step_checkbox,
        )
        step_every_label = ttk.Label(time_settings, text="Every")
        step_every_label.grid(row=7, column=0, sticky="w", pady=(4, 0))
        step_every_spin = ttk.Spinbox(
            time_settings,
            textvariable=vars_.step_reduce_every,
            from_=1,
            to=4_294_967_295,
            width=12,
        )
        step_every_spin.grid(row=7, column=1, padx=(8, 0), pady=(4, 0), sticky="ew")
        self._add_tooltip(
            "X value: after this many completed trigger pulses, apply one reduction step "
            "to the next trigger.",
            step_every_label,
            step_every_spin,
        )
        step_edge_label = ttk.Label(time_settings, text="Edge -Y")
        step_edge_label.grid(row=8, column=0, sticky="w", pady=(4, 0))
        step_edge_spin = ttk.Spinbox(
            time_settings,
            textvariable=vars_.step_reduce_edge_delta,
            from_=0,
            to=EDGE_COUNT_MAX,
            width=12,
        )
        step_edge_spin.grid(row=8, column=1, padx=(8, 0), pady=(4, 0), sticky="ew")
        self._add_tooltip(
            "Y value for Edge Count mode. For example, Every 4 and Edge -Y 1 makes the "
            "next edge threshold one edge earlier after every four completed pulses.",
            step_edge_label,
            step_edge_spin,
        )
        step_delay_label = ttk.Label(time_settings, text="Delay -ns")
        step_delay_label.grid(row=9, column=0, sticky="w", pady=(4, 0))
        step_delay_spin = ttk.Spinbox(
            time_settings,
            textvariable=vars_.step_reduce_delay_ns,
            from_=0,
            to=4_294_967_295,
            width=12,
        )
        step_delay_spin.grid(row=9, column=1, padx=(8, 0), pady=(4, 0), sticky="ew")
        self._add_tooltip(
            "Y value for Delay mode, in nanoseconds. The base Delay field is still set in "
            "microseconds; this step value subtracts from the live runtime delay.",
            step_delay_label,
            step_delay_spin,
        )
        ttk.Label(time_settings, text="Step Count").grid(
            row=10, column=0, sticky="w", pady=(4, 0)
        )
        ttk.Label(time_settings, textvariable=vars_.step_reduce_count).grid(
            row=10, column=1, padx=(8, 0), pady=(4, 0), sticky="w"
        )
        ttk.Label(time_settings, text="Current Edge").grid(
            row=11, column=0, sticky="w", pady=(4, 0)
        )
        ttk.Label(time_settings, textvariable=vars_.step_current_edge_count).grid(
            row=11, column=1, padx=(8, 0), pady=(4, 0), sticky="w"
        )
        ttk.Label(time_settings, text="Current ns").grid(
            row=12, column=0, sticky="w", pady=(4, 0)
        )
        ttk.Label(time_settings, textvariable=vars_.step_current_delay_ns).grid(
            row=12, column=1, padx=(8, 0), pady=(4, 0), sticky="w"
        )
        self.time_mode_widgets[index].extend(
            [time_title, edge_label, edge_combo, delay_label, delay_spin, width_label, width_spin]
        )

        edge_count_title = ttk.Label(edge_settings, text="Edge Count Mode")
        edge_count_title.grid(row=0, column=0, columnspan=2, sticky="w")
        edge_count_label = ttk.Label(edge_settings, text="Count")
        edge_count_label.grid(row=1, column=0, sticky="w", pady=(8, 0))
        edge_count_spin = ttk.Spinbox(
            edge_settings, textvariable=vars_.edge_count, from_=1, to=EDGE_COUNT_MAX, width=12
        )
        edge_count_spin.grid(row=1, column=1, padx=(8, 0), pady=(8, 0), sticky="ew")
        self._add_tooltip(
            "Number of rising input edges to count before driving the output active in "
            "Edge Count mode. Maximum is 65535.",
            edge_count_label,
            edge_count_spin,
        )
        pulse_edges_label = ttk.Label(edge_settings, text="Width Edges")
        pulse_edges_label.grid(row=2, column=0, sticky="w", pady=(4, 0))
        pulse_edges_spin = ttk.Spinbox(
            edge_settings,
            textvariable=vars_.pulse_width_edges,
            from_=1,
            to=EDGE_COUNT_MAX,
            width=12,
        )
        pulse_edges_spin.grid(row=2, column=1, padx=(8, 0), pady=(4, 0), sticky="ew")
        self._add_tooltip(
            "How many additional rising input edges keep the output active after Count "
            "is reached.",
            pulse_edges_label,
            pulse_edges_spin,
        )
        auto_clear_checkbox = ttk.Checkbutton(
            edge_settings,
            text="Auto Clear",
            variable=vars_.auto_clear_edges,
        )
        auto_clear_checkbox.grid(row=3, column=0, columnspan=2, sticky="w", pady=(6, 0))
        self._add_tooltip(
            "After an edge-count output pulse finishes, wait the configured delay, "
            "clear the PWM edge counter, and restart counting from zero.",
            auto_clear_checkbox,
        )
        clear_delay_label = ttk.Label(edge_settings, text="Clear Delay ns")
        clear_delay_label.grid(row=4, column=0, sticky="w", pady=(4, 0))
        clear_delay_spin = ttk.Spinbox(
            edge_settings,
            textvariable=vars_.auto_clear_delay_ns,
            from_=0,
            to=4_294_967_295,
            width=12,
        )
        clear_delay_spin.grid(row=4, column=1, padx=(8, 0), pady=(4, 0), sticky="ew")
        self._add_tooltip(
            "Nanoseconds to wait after the edge-count pulse finishes before clearing "
            "the counter when Auto Clear is enabled.",
            clear_delay_label,
            clear_delay_spin,
        )
        ttk.Label(edge_settings, text="Edges Seen").grid(
            row=5, column=0, sticky="w", pady=(4, 0)
        )
        ttk.Label(edge_settings, textvariable=vars_.edge_seen).grid(
            row=5, column=1, padx=(8, 0), pady=(4, 0), sticky="w"
        )
        ttk.Label(edge_settings, text="Monitor Hz").grid(
            row=6, column=0, sticky="w", pady=(4, 0)
        )
        ttk.Label(edge_settings, textvariable=vars_.monitor_rate).grid(
            row=6, column=1, padx=(8, 0), pady=(4, 0), sticky="w"
        )
        ttk.Label(edge_settings, text="Monitor ns").grid(
            row=7, column=0, sticky="w", pady=(4, 0)
        )
        ttk.Label(edge_settings, textvariable=vars_.monitor_period).grid(
            row=7, column=1, padx=(8, 0), pady=(4, 0), sticky="w"
        )
        ttk.Label(edge_settings, text="Monitor Edges").grid(
            row=8, column=0, sticky="w", pady=(4, 0)
        )
        ttk.Label(edge_settings, textvariable=vars_.monitor_edges).grid(
            row=8, column=1, padx=(8, 0), pady=(4, 0), sticky="w"
        )
        self.edge_count_mode_widgets[index].extend(
            [
                edge_count_title,
                edge_count_label,
                edge_count_spin,
                pulse_edges_label,
                pulse_edges_spin,
                auto_clear_checkbox,
                clear_delay_label,
                clear_delay_spin,
            ]
        )

        output_title = ttk.Label(output_settings, text="Output / Live")
        output_title.grid(row=0, column=0, columnspan=2, sticky="w")
        idle_label = ttk.Label(output_settings, text="Idle")
        idle_label.grid(row=1, column=0, sticky="w", pady=(8, 0))
        idle_combo = ttk.Combobox(
            output_settings, textvariable=vars_.idle, values=LEVEL_VALUES, state="readonly", width=8
        )
        idle_combo.grid(row=1, column=1, padx=(8, 0), pady=(8, 0), sticky="ew")
        self._add_tooltip(
            "Output level used when the channel is not firing.",
            idle_label,
            idle_combo,
        )
        active_label = ttk.Label(output_settings, text="Active")
        active_label.grid(row=2, column=0, sticky="w", pady=(4, 0))
        active_combo = ttk.Combobox(
            output_settings,
            textvariable=vars_.active,
            values=LEVEL_VALUES,
            state="readonly",
            width=8,
        )
        active_combo.grid(row=2, column=1, padx=(8, 0), pady=(4, 0), sticky="ew")
        self._add_tooltip(
            "Output level driven during a trigger pulse.",
            active_label,
            active_combo,
        )
        ttk.Label(output_settings, text="In").grid(
            row=3, column=0, sticky="w", pady=(4, 0)
        )
        ttk.Label(output_settings, textvariable=vars_.input_level).grid(
            row=3, column=1, padx=(8, 0), pady=(4, 0), sticky="w"
        )
        ttk.Label(output_settings, text="Out").grid(
            row=4, column=0, sticky="w", pady=(4, 0)
        )
        ttk.Label(output_settings, textvariable=vars_.output_level).grid(
            row=4, column=1, padx=(8, 0), pady=(4, 0), sticky="w"
        )
        ttk.Label(output_settings, text="Pending").grid(
            row=5, column=0, sticky="w", pady=(4, 0)
        )
        ttk.Label(output_settings, textvariable=vars_.pending).grid(
            row=5, column=1, padx=(8, 0), pady=(4, 0), sticky="w"
        )
        ttk.Label(output_settings, text="Events").grid(
            row=6, column=0, sticky="w", pady=(4, 0)
        )
        ttk.Label(output_settings, textvariable=vars_.events).grid(
            row=6, column=1, padx=(8, 0), pady=(4, 0), sticky="w"
        )
        ttk.Label(output_settings, text="Last us").grid(
            row=7, column=0, sticky="w", pady=(4, 0)
        )
        ttk.Label(output_settings, textvariable=vars_.last_event_us).grid(
            row=7, column=1, padx=(8, 0), pady=(4, 0), sticky="w"
        )
        current_edge_label = ttk.Label(output_settings, text="Current Count")
        current_edge_label.grid(row=8, column=0, sticky="w", pady=(4, 0))
        current_edge_value = ttk.Label(output_settings, textvariable=vars_.step_current_edge_count)
        current_edge_value.grid(row=8, column=1, padx=(8, 0), pady=(4, 0), sticky="w")
        self._add_tooltip(
            "Live edge-count trigger threshold currently used by the firmware. "
            "When Step Reduce is enabled, this value shows the reduced/new threshold.",
            current_edge_label,
            current_edge_value,
        )

        actions.columnconfigure(0, weight=1)
        actions.columnconfigure(1, weight=1)

        apply_button = ttk.Button(
            actions,
            text=f"Apply CH{index + 1}",
            command=lambda ch=index: self.apply_channel(ch),
        )
        apply_button.grid(row=0, column=0, columnspan=2, sticky="ew")
        self.connected_widgets.append(apply_button)

        fire_button = ttk.Button(
            actions,
            text=f"Fire CH{index + 1}",
            command=lambda ch=index: self.fire_channel(ch),
        )
        fire_button.grid(row=1, column=0, columnspan=2, pady=(6, 0), sticky="ew")
        self.connected_widgets.append(fire_button)

        active_button = ttk.Button(
            actions,
            text="Drive Active",
            command=lambda ch=index: self.drive_channel(ch, "active"),
        )
        active_button.grid(row=2, column=0, padx=(0, 4), pady=(6, 0), sticky="ew")
        self.connected_widgets.append(active_button)

        idle_button = ttk.Button(
            actions,
            text="Drive Idle",
            command=lambda ch=index: self.drive_channel(ch, "idle"),
        )
        idle_button.grid(row=2, column=1, padx=(4, 0), pady=(6, 0), sticky="ew")
        self.connected_widgets.append(idle_button)

        clear_edges_button = ttk.Button(
            actions,
            text="Clear Edges",
            command=lambda ch=index: self.clear_edges(ch),
        )
        clear_edges_button.grid(row=3, column=0, columnspan=2, pady=(6, 0), sticky="ew")
        self.connected_widgets.append(clear_edges_button)

        start_monitor_button = ttk.Button(
            actions,
            text="Start Monitor",
            command=lambda ch=index: self.start_monitor(ch),
        )
        start_monitor_button.grid(row=4, column=0, columnspan=2, pady=(6, 0), sticky="ew")
        self.connected_widgets.append(start_monitor_button)

        stop_monitor_button = ttk.Button(
            actions,
            text="Stop Monitor",
            command=self.stop_monitor,
        )
        stop_monitor_button.grid(row=5, column=0, columnspan=2, pady=(6, 0), sticky="ew")
        self.connected_widgets.append(stop_monitor_button)

    def _build_profile_bar(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="Profile", padding=10)
        frame.grid(row=0, column=0, sticky="ew")
        frame.columnconfigure(6, weight=1)

        self.apply_all_button = ttk.Button(frame, text="Apply All", command=self.apply_all)
        self.apply_all_button.grid(row=0, column=0, padx=(0, 8))
        self.connected_widgets.append(self.apply_all_button)

        ttk.Button(frame, text="Load JSON", command=self.load_profile).grid(
            row=0, column=1, padx=(0, 8)
        )
        ttk.Button(frame, text="Save JSON", command=self.save_profile).grid(
            row=0, column=2, padx=(0, 8)
        )

        save_device_button = ttk.Button(
            frame, text="Save Device", command=lambda: self.send_command("save")
        )
        save_device_button.grid(row=0, column=3, padx=(16, 8))
        self.connected_widgets.append(save_device_button)

        load_device_button = ttk.Button(frame, text="Load Device", command=self.load_device)
        load_device_button.grid(row=0, column=4, padx=(0, 8))
        self.connected_widgets.append(load_device_button)

        reset_button = ttk.Button(frame, text="Factory Reset", command=self.factory_reset_device)
        reset_button.grid(row=0, column=5, padx=(0, 8))
        self.connected_widgets.append(reset_button)

    def _build_diagnostics_panel(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="Diagnostics", padding=10)
        frame.grid(row=0, column=0, sticky="nsew")
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(5, weight=1)

        fields = ttk.Frame(frame)
        fields.grid(row=0, column=0, sticky="ew")
        for col in range(12):
            fields.columnconfigure(col, weight=1 if col % 2 == 1 else 0)

        field_specs = (
            (
                "Channel",
                self.diag_channel_var,
                1,
                CHANNEL_COUNT,
                5,
                "Channel polarity/settings used for the diagnostic output.",
            ),
            (
                "Input GP",
                self.diag_input_gpio_var,
                0,
                29,
                6,
                "SWCLK/input GPIO for diagnostic counting. Must be an odd PWM B-capable pin.",
            ),
            (
                "Output GP",
                self.diag_output_gpio_var,
                0,
                29,
                6,
                "GPIO driven for the diagnostic pulse.",
            ),
            (
                "Fire Edges",
                self.diag_fire_after_edges_var,
                1,
                EDGE_COUNT_MAX,
                10,
                "Rising-edge count where the diagnostic output turns active.",
            ),
            (
                "Width Edges",
                self.diag_pulse_width_edges_var,
                1,
                EDGE_COUNT_MAX,
                10,
                "Rising-edge pulse width for the diagnostic output.",
            ),
            (
                "Idle Gap us",
                self.diag_idle_gap_us_var,
                1,
                4_294_967_295,
                10,
                "Diagnostic transaction ends after this many microseconds with no SWCLK edges.",
            ),
        )
        for index, (label, variable, minimum, maximum, width, tooltip) in enumerate(field_specs):
            label_widget = ttk.Label(fields, text=label)
            label_widget.grid(
                row=0,
                column=index * 2,
                padx=(0 if index == 0 else 12, 4),
                sticky="w",
            )
            spinbox = ttk.Spinbox(
                fields,
                textvariable=variable,
                from_=minimum,
                to=maximum,
                width=width,
            )
            spinbox.grid(row=0, column=index * 2 + 1, sticky="ew")
            self._add_tooltip(tooltip, label_widget, spinbox)

        file_frame = ttk.Frame(frame)
        file_frame.grid(row=1, column=0, pady=(8, 0), sticky="ew")
        file_frame.columnconfigure(1, weight=1)
        output_file_label = ttk.Label(file_frame, text="Output File")
        output_file_label.grid(row=0, column=0, padx=(0, 6), sticky="w")
        output_file_entry = ttk.Entry(file_frame, textvariable=self.diag_output_path_var)
        output_file_entry.grid(row=0, column=1, padx=(0, 8), sticky="ew")
        self._add_tooltip(
            "CSV path for saved diagnostic runs. Raw DIAG/DIAG_EVENT lines are saved beside it as .log.",
            output_file_label,
            output_file_entry,
        )
        ttk.Button(file_frame, text="Browse", command=self.browse_diag_output_path).grid(
            row=0, column=2
        )

        buttons = ttk.Frame(frame)
        buttons.grid(row=2, column=0, pady=(8, 0), sticky="ew")
        for col in range(5):
            buttons.columnconfigure(col, weight=1)

        configure_button = ttk.Button(
            buttons,
            text="Configure Diagnostic",
            command=self.configure_diagnostic,
        )
        configure_button.grid(row=0, column=0, padx=(0, 8), sticky="ew")
        self.connected_widgets.append(configure_button)

        arm_button = ttk.Button(buttons, text="Arm Diagnostic", command=self.arm_diagnostic)
        arm_button.grid(row=0, column=1, padx=(0, 8), sticky="ew")
        self.connected_widgets.append(arm_button)

        stop_button = ttk.Button(buttons, text="Stop Diagnostic", command=self.stop_diagnostic)
        stop_button.grid(row=0, column=2, padx=(0, 8), sticky="ew")
        self.connected_widgets.append(stop_button)

        ttk.Button(buttons, text="Save Diagnostic Log", command=self.save_diagnostic_log).grid(
            row=0, column=3, padx=(0, 8), sticky="ew"
        )
        ttk.Button(buttons, text="Clear Diagnostic Log", command=self.clear_diagnostic_log).grid(
            row=0, column=4, sticky="ew"
        )

        sweep_fields = ttk.LabelFrame(frame, text="Sweep Diagnostics", padding=8)
        sweep_fields.grid(row=3, column=0, pady=(8, 0), sticky="ew")
        for col in range(10):
            sweep_fields.columnconfigure(col, weight=1 if col % 2 == 1 else 0)

        sweep_specs = (
            (
                "Start Edge",
                self.sweep_start_edge_var,
                1,
                EDGE_COUNT_MAX,
                10,
                "First trigger edge used by the sweep.",
            ),
            (
                "Stop Edge",
                self.sweep_stop_edge_var,
                1,
                EDGE_COUNT_MAX,
                10,
                "Last trigger edge allowed by the sweep.",
            ),
            (
                "Step",
                self.sweep_step_var,
                1,
                EDGE_COUNT_MAX,
                8,
                "Amount added to the trigger edge after each completed sweep run.",
            ),
            (
                "Width Edges",
                self.sweep_pulse_width_edges_var,
                1,
                EDGE_COUNT_MAX,
                10,
                "Rising-edge pulse width used for each sweep run.",
            ),
            (
                "Idle Gap us",
                self.sweep_idle_gap_us_var,
                1,
                4_294_967_295,
                10,
                "Sweep transaction ends after this many microseconds with no SWCLK edges.",
            ),
        )
        for index, (label, variable, minimum, maximum, width, tooltip) in enumerate(sweep_specs):
            label_widget = ttk.Label(sweep_fields, text=label)
            label_widget.grid(
                row=0,
                column=index * 2,
                padx=(0 if index == 0 else 12, 4),
                sticky="w",
            )
            spinbox = ttk.Spinbox(
                sweep_fields,
                textvariable=variable,
                from_=minimum,
                to=maximum,
                width=width,
            )
            spinbox.grid(row=0, column=index * 2 + 1, sticky="ew")
            self._add_tooltip(tooltip, label_widget, spinbox)

        sweep_buttons = ttk.Frame(frame)
        sweep_buttons.grid(row=4, column=0, pady=(8, 0), sticky="ew")
        for col in range(4):
            sweep_buttons.columnconfigure(col, weight=1)

        sweep_configure_button = ttk.Button(
            sweep_buttons,
            text="Configure Sweep",
            command=self.configure_sweep_diagnostic,
        )
        sweep_configure_button.grid(row=0, column=0, padx=(0, 8), sticky="ew")
        self.connected_widgets.append(sweep_configure_button)

        sweep_arm_button = ttk.Button(
            sweep_buttons,
            text="Arm Sweep",
            command=self.arm_sweep_diagnostic,
        )
        sweep_arm_button.grid(row=0, column=1, padx=(0, 8), sticky="ew")
        self.connected_widgets.append(sweep_arm_button)

        sweep_stop_button = ttk.Button(
            sweep_buttons,
            text="Stop Sweep",
            command=self.stop_diagnostic,
        )
        sweep_stop_button.grid(row=0, column=2, padx=(0, 8), sticky="ew")
        self.connected_widgets.append(sweep_stop_button)

        ttk.Button(
            sweep_buttons,
            text="Save Sweep CSV",
            command=self.save_sweep_csv,
        ).grid(row=0, column=3, sticky="ew")

        table = ttk.Frame(frame)
        table.grid(row=5, column=0, pady=(8, 0), sticky="nsew")
        table.columnconfigure(0, weight=1)
        table.rowconfigure(0, weight=1)

        self.diag_tree = ttk.Treeview(table, columns=DIAG_COLUMNS, show="headings", height=5)
        widths = {
            "run_index": 80,
            "trigger_edge": 95,
            "ch": 45,
            "total_edges": 95,
            "fire_after_edges": 115,
            "pulse_width_edges": 120,
            "first_us": 95,
            "trigger_on_us": 105,
            "trigger_off_us": 105,
            "last_edge_us": 105,
            "idle_gap_us": 90,
            "duration_us": 95,
            "avg_swclk_period_ns": 145,
            "min_period_ns": 105,
            "max_period_ns": 105,
            "fired": 55,
        }
        for column in DIAG_COLUMNS:
            self.diag_tree.heading(column, text=column)
            self.diag_tree.column(column, width=widths[column], minwidth=45, anchor="center")

        yscroll = ttk.Scrollbar(table, orient="vertical", command=self.diag_tree.yview)
        xscroll = ttk.Scrollbar(table, orient="horizontal", command=self.diag_tree.xview)
        self.diag_tree.configure(yscrollcommand=yscroll.set, xscrollcommand=xscroll.set)
        self.diag_tree.grid(row=0, column=0, sticky="nsew")
        yscroll.grid(row=0, column=1, sticky="ns")
        xscroll.grid(row=1, column=0, sticky="ew")

    def _build_console(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="Console", padding=10)
        frame.grid(row=0, column=0, sticky="nsew")
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(0, weight=1)

        self.console = tk.Text(frame, width=100, height=8, wrap="word", state="disabled")
        scrollbar = ttk.Scrollbar(frame, orient="vertical", command=self.console.yview)
        self.console.configure(yscrollcommand=scrollbar.set)
        self.console.grid(row=0, column=0, sticky="nsew")
        scrollbar.grid(row=0, column=1, sticky="ns")

        command_frame = ttk.Frame(frame)
        command_frame.grid(row=1, column=0, columnspan=2, pady=(8, 0), sticky="ew")
        command_frame.columnconfigure(0, weight=1)

        entry = ttk.Entry(command_frame, textvariable=self.manual_command_var)
        entry.grid(row=0, column=0, padx=(0, 8), sticky="ew")
        entry.bind("<Return>", lambda _event: self.send_manual_command())

        self.send_button = ttk.Button(
            command_frame, text="Send Command", command=self.send_manual_command
        )
        self.send_button.grid(row=0, column=1)
        self.connected_widgets.append(self.send_button)

    def _bind_canvas_mousewheel(
        self,
        canvas: tk.Canvas,
        root: tk.Widget | None = None,
        recursive: bool = False,
    ) -> None:
        def on_mousewheel(event: tk.Event) -> None:
            delta = int(-event.delta / 120)
            if delta == 0:
                delta = -1 if event.delta > 0 else 1
            canvas.yview_scroll(delta, "units")

        def on_scroll_up(_event: tk.Event) -> None:
            canvas.yview_scroll(-1, "units")

        def on_scroll_down(_event: tk.Event) -> None:
            canvas.yview_scroll(1, "units")

        def bind_scroll(_event: tk.Event) -> None:
            canvas.bind_all("<MouseWheel>", on_mousewheel)
            canvas.bind_all("<Button-4>", on_scroll_up)
            canvas.bind_all("<Button-5>", on_scroll_down)

        def unbind_scroll(_event: tk.Event) -> None:
            canvas.unbind_all("<MouseWheel>")
            canvas.unbind_all("<Button-4>")
            canvas.unbind_all("<Button-5>")

        def bind_widget_tree(widget: tk.Widget) -> None:
            widget.bind("<Enter>", bind_scroll, add="+")
            if recursive:
                for child in widget.winfo_children():
                    bind_widget_tree(child)

        bind_widget_tree(root or canvas)
        canvas.bind("<Leave>", unbind_scroll, add="+")
        if root is not None:
            root.bind("<Leave>", unbind_scroll, add="+")

    def refresh_ports(self) -> None:
        if serial is None or list_ports is None:
            self.port_combo.configure(values=())
            self._log("Install pyserial to list and open serial ports: python -m pip install pyserial")
            return

        ports = [
            f"{port.device} - {port.description}" for port in sorted(list_ports.comports())
        ]
        self.port_combo.configure(values=ports)
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def toggle_connection(self) -> None:
        if self.serial_conn is None:
            self.connect()
        else:
            self.disconnect()

    def connect(self) -> None:
        if serial is None:
            messagebox.showerror(
                "Missing dependency",
                "pyserial is required for USB serial access.\n\nRun: python -m pip install pyserial",
            )
            return

        port = self._selected_port()
        if not port:
            messagebox.showwarning("No port", "Choose a serial port or type one, such as COM5.")
            return

        try:
            baud = int(self.baud_var.get())
        except ValueError:
            messagebox.showwarning("Invalid baud", "Baud rate must be a number.")
            return

        try:
            conn = serial.Serial(port, baudrate=baud, timeout=0.1, write_timeout=1)
        except Exception as exc:
            messagebox.showerror("Connection failed", str(exc))
            self._log(f"[ERR] connection failed: {exc}")
            return

        self.serial_conn = conn
        self.reader_stop.clear()
        self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader_thread.start()

        self.connection_var.set(f"Connected to {port}")
        self._set_connected_state(True)
        self._log(f"[OK] connected to {port}")
        self.after(500, self.read_status)

    def disconnect(self) -> None:
        self.reader_stop.set()
        with self.serial_lock:
            if self.serial_conn is not None:
                try:
                    self.serial_conn.close()
                except Exception:
                    pass
        self.serial_conn = None
        self.connection_var.set("Disconnected")
        self.armed_var.set("Unknown")
        self.monitoring_channel = None
        self._set_connected_state(False)
        self._log("[OK] disconnected")

    def _reader_loop(self) -> None:
        while not self.reader_stop.is_set():
            try:
                conn = self.serial_conn
                if conn is None:
                    break
                raw = conn.readline()
                if not raw:
                    continue
                text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                self.rx_queue.put(("line", text))
            except Exception as exc:
                if not self.reader_stop.is_set():
                    self.rx_queue.put(("error", str(exc)))
                break

    def _poll_serial_queue(self) -> None:
        while True:
            try:
                kind, text = self.rx_queue.get_nowait()
            except queue.Empty:
                break

            if kind == "line":
                if text:
                    self._log(text)
                    self._parse_device_line(text)
                    self._handle_device_ack(text)
            else:
                self._log(f"[ERR] serial read failed: {text}")
                self.disconnect()

        self.after(50, self._poll_serial_queue)

    def _parse_device_line(self, text: str) -> None:
        if text.startswith("Armed:"):
            state = text.split(":", 1)[1].strip()
            self.armed_var.set("Yes" if state == "yes" else "No")
            return

        clock_match = CLOCK_RE.match(text)
        if clock_match:
            self.clock_freq_var.set(clock_match.group("clock_freq_khz"))
            return

        freq_ok_match = FREQ_OK_RE.match(text)
        if freq_ok_match:
            self.clock_freq_var.set(freq_ok_match.group("clock_freq_khz"))
            return

        if text.startswith("DIAG"):
            self._parse_diag_line(text)
            return

        monitor_match = MONITOR_RE.match(text)
        if monitor_match:
            self._parse_monitor_line(monitor_match.groupdict())
            return

        match = STATUS_RE.match(text)
        if not match:
            return

        data = match.groupdict()
        index = int(data["ch"]) - 1
        vars_ = self.channel_vars[index]
        vars_.enabled.set(data["enabled"] == "1")
        vars_.input_gpio.set(data["input_gpio"])
        vars_.output_gpio.set(data["output_gpio"])
        vars_.trigger_mode.set(data["trigger_mode"])
        self._update_channel_mode_ui(index)
        vars_.edge.set(data["edge"])
        if data.get("pull"):
            vars_.pull.set(data["pull"])
        vars_.delay_us.set(data["delay_us"])
        vars_.width_us.set(data["width_us"])
        vars_.edge_count.set(data["edge_count"])
        vars_.pulse_width_edges.set(data["pulse_width_edges"])
        if data.get("auto_clear_edges") is not None:
            vars_.auto_clear_edges.set(data["auto_clear_edges"] == "1")
        if data.get("auto_clear_delay_ns") is not None:
            vars_.auto_clear_delay_ns.set(data["auto_clear_delay_ns"])
        if data.get("step_reduce_enabled") is not None:
            vars_.step_reduce_enabled.set(data["step_reduce_enabled"] == "1")
        if data.get("step_reduce_every") is not None:
            vars_.step_reduce_every.set(data["step_reduce_every"])
        if data.get("step_reduce_edge_delta") is not None:
            vars_.step_reduce_edge_delta.set(data["step_reduce_edge_delta"])
        if data.get("step_reduce_delay_ns") is not None:
            vars_.step_reduce_delay_ns.set(data["step_reduce_delay_ns"])
        if data.get("step_reduce_count") is not None:
            vars_.step_reduce_count.set(data["step_reduce_count"])
        if data.get("step_current_edge_count") is not None:
            vars_.step_current_edge_count.set(data["step_current_edge_count"])
        if data.get("step_current_delay_ns") is not None:
            vars_.step_current_delay_ns.set(data["step_current_delay_ns"])
        vars_.edge_seen.set(data["edge_seen"])
        vars_.idle.set(data["idle"])
        vars_.active.set(data["active"])
        if data.get("input_level") is not None:
            vars_.input_level.set(data["input_level"])
        if data.get("output_level") is not None:
            vars_.output_level.set(data["output_level"])
        vars_.pending.set(data["pending"])
        vars_.events.set(data["events"])
        vars_.last_event_us.set(data["last_event_us"])

    def _parse_diag_line(self, text: str) -> None:
        self.diag_raw_lines.append(text)

        if text.startswith("DIAG_EVENT"):
            DIAG_EVENT_RE.match(text)
            return

        if not text.startswith("DIAG "):
            return

        data = self._parse_key_values(text.split(maxsplit=1)[1])
        if "trigger_edge" not in data and "fire_after_edges" in data:
            data["trigger_edge"] = data["fire_after_edges"]
        if "fire_after_edges" not in data and "trigger_edge" in data:
            data["fire_after_edges"] = data["trigger_edge"]
        if "run_index" not in data:
            data["run_index"] = str(len(self.diag_rows))

        row = {column: data.get(column, "") for column in DIAG_COLUMNS}
        self.diag_rows.append(row)
        if self.diag_tree is not None:
            self.diag_tree.insert(
                "",
                "end",
                values=[row[column] for column in DIAG_COLUMNS],
            )
            self.diag_tree.yview_moveto(1.0)

    def _parse_key_values(self, text: str) -> dict[str, str]:
        data: dict[str, str] = {}
        for token in text.split():
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            data[key] = value
        return data

    def _parse_monitor_line(self, data: dict[str, str]) -> None:
        ch = int(data["ch"])
        active = data["active"] == "1"
        if ch < 1 or ch > CHANNEL_COUNT:
            if not active:
                self.monitoring_channel = None
            return

        index = ch - 1
        vars_ = self.channel_vars[index]
        vars_.monitor_edges.set(data["edges"])
        vars_.monitor_rate.set(self._format_rate(int(data["rate_hz"])))
        vars_.monitor_period.set(f"{data['period_ns']} ns")
        self.monitoring_channel = index if active else None

    def _format_rate(self, rate_hz: int) -> str:
        if rate_hz >= 1_000_000:
            return f"{rate_hz / 1_000_000:.3f} MHz"
        if rate_hz >= 1_000:
            return f"{rate_hz / 1_000:.3f} kHz"
        return f"{rate_hz} Hz"

    def _handle_device_ack(self, text: str) -> None:
        freq_ok_match = FREQ_OK_RE.match(text)
        if freq_ok_match and self.pending_clock_save:
            self.clock_freq_var.set(freq_ok_match.group("clock_freq_khz"))
            self.pending_clock_save = False
            if self.send_command("save"):
                self.pending_clock_refresh = True
                self.after(900, self._clock_refresh_fallback)
            else:
                self.after(150, self.read_status)
            return

        if self.pending_clock_refresh and text == "[OK] settings saved":
            self.pending_clock_refresh = False
            self.after(150, self.read_status)
            return

        if (self.pending_clock_save or self.pending_clock_refresh) and text.startswith("[ERR]"):
            self.pending_clock_save = False
            self.pending_clock_refresh = False
            self.after(150, self.read_status)
            return

        if not self.pending_load_refresh:
            return

        if text == "[OK] settings loaded":
            self.pending_load_refresh = False
            self.after(150, self.read_status)
            return

        if text.startswith("[ERR]"):
            self.pending_load_refresh = False

    def _selected_port(self) -> str:
        return self.port_var.get().split(" - ", 1)[0].strip()

    def _set_connected_state(self, connected: bool) -> None:
        self.connect_button.configure(text="Disconnect" if connected else "Connect")
        state = "normal" if connected else "disabled"
        for widget in self.connected_widgets:
            widget.configure(state=state)

    def _set_mode_widget_enabled(self, widget: ttk.Widget, enabled: bool) -> None:
        if isinstance(widget, ttk.Combobox):
            widget.configure(state="readonly" if enabled else "disabled")
            return

        widget.state(["!disabled"] if enabled else ["disabled"])

    def _update_channel_mode_ui(self, index: int) -> None:
        if index < 0 or index >= CHANNEL_COUNT:
            return

        mode = self.channel_vars[index].trigger_mode.get()
        time_enabled = mode == "time"
        edge_count_enabled = mode == "edge_count"

        for widget in self.time_mode_widgets[index]:
            self._set_mode_widget_enabled(widget, time_enabled)
        for widget in self.edge_count_mode_widgets[index]:
            self._set_mode_widget_enabled(widget, edge_count_enabled)

    def send_command(self, command: str, log: bool = True) -> bool:
        command = command.strip()
        if not command:
            return False

        conn = self.serial_conn
        if conn is None or not getattr(conn, "is_open", False):
            self._log("[ERR] not connected")
            return False

        try:
            with self.serial_lock:
                conn.write((command + "\n").encode("utf-8"))
                conn.flush()
        except Exception as exc:
            self._log(f"[ERR] write failed: {exc}")
            return False

        if log:
            self._log(f"> {command}")
        return True

    def send_manual_command(self) -> None:
        command = self.manual_command_var.get()
        if self.send_command(command):
            self.manual_command_var.set("")

    def read_status(self) -> None:
        self.send_command("status")

    def fire_channel(self, index: int) -> None:
        self.send_command(f"fire {index + 1}")

    def drive_channel(self, index: int, level: str) -> None:
        self.send_command(f"drive {index + 1} {level}")

    def clear_edges(self, index: int) -> None:
        if self.send_command(f"clear_edges {index + 1}"):
            self.after(150, self.read_status)

    def start_monitor(self, index: int) -> None:
        if self.monitoring_channel is not None and self.monitoring_channel != index:
            self.send_command("monitor_stop")

        vars_ = self.channel_vars[index]
        vars_.monitor_rate.set("0 Hz")
        vars_.monitor_period.set("0 ns")
        vars_.monitor_edges.set("0")

        if self.send_command(f"monitor_start {index + 1}"):
            self.monitoring_channel = index
            self.after(300, self._poll_monitor_status)

    def stop_monitor(self) -> None:
        if self.send_command("monitor_stop"):
            self.monitoring_channel = None
            self.after(150, self.read_status)

    def _poll_monitor_status(self) -> None:
        if self.monitoring_channel is None:
            return

        if self.send_command("monitor_status", log=False):
            self.after(500, self._poll_monitor_status)
        else:
            self.monitoring_channel = None

    def _read_diag_channel(self) -> int:
        channel = self._read_u32(self.diag_channel_var.get(), "Diagnostic channel")
        if channel < 1 or channel > CHANNEL_COUNT:
            raise ValueError(f"Diagnostic channel must be between 1 and {CHANNEL_COUNT}.")
        return channel

    def _diag_config(self) -> dict[str, int]:
        channel = self._read_diag_channel()
        input_gpio = self._read_gpio(
            self.diag_input_gpio_var.get(), "Diagnostic input GPIO"
        )
        output_gpio = self._read_gpio(
            self.diag_output_gpio_var.get(), "Diagnostic output GPIO"
        )
        if input_gpio == output_gpio:
            raise ValueError("Diagnostic input and output GPIO must be different.")
        if input_gpio % 2 == 0:
            raise ValueError(
                "Diagnostic input must be a PWM B-channel GPIO such as GP3, GP5, or GP7."
            )

        fire_after_edges = self._read_edge_count(
            self.diag_fire_after_edges_var.get(), "Diagnostic fire after edges"
        )
        pulse_width_edges = self._read_edge_count(
            self.diag_pulse_width_edges_var.get(), "Diagnostic pulse width edges"
        )
        idle_gap_us = self._read_u32(self.diag_idle_gap_us_var.get(), "Diagnostic idle gap us")
        if idle_gap_us == 0:
            raise ValueError("Diagnostic idle gap us must be greater than 0.")

        return {
            "channel": channel,
            "input_gpio": input_gpio,
            "output_gpio": output_gpio,
            "fire_after_edges": fire_after_edges,
            "pulse_width_edges": pulse_width_edges,
            "idle_gap_us": idle_gap_us,
        }

    def _sweep_config(self) -> dict[str, int]:
        channel = self._read_diag_channel()
        input_gpio = self._read_gpio(
            self.diag_input_gpio_var.get(), "Diagnostic input GPIO"
        )
        output_gpio = self._read_gpio(
            self.diag_output_gpio_var.get(), "Diagnostic output GPIO"
        )
        if input_gpio == output_gpio:
            raise ValueError("Diagnostic input and output GPIO must be different.")
        if input_gpio % 2 == 0:
            raise ValueError(
                "Diagnostic input must be a PWM B-channel GPIO such as GP3, GP5, or GP7."
            )

        start_edge = self._read_edge_count(
            self.sweep_start_edge_var.get(), "Sweep start edge"
        )
        stop_edge = self._read_edge_count(
            self.sweep_stop_edge_var.get(), "Sweep stop edge"
        )
        if stop_edge < start_edge:
            raise ValueError("Sweep stop edge must be greater than or equal to start edge.")

        step = self._read_edge_count(self.sweep_step_var.get(), "Sweep step")
        pulse_width_edges = self._read_edge_count(
            self.sweep_pulse_width_edges_var.get(), "Sweep pulse width edges"
        )
        idle_gap_us = self._read_u32(self.sweep_idle_gap_us_var.get(), "Sweep idle gap us")
        if idle_gap_us == 0:
            raise ValueError("Sweep idle gap us must be greater than 0.")

        return {
            "channel": channel,
            "input_gpio": input_gpio,
            "output_gpio": output_gpio,
            "start_edge": start_edge,
            "stop_edge": stop_edge,
            "step": step,
            "pulse_width_edges": pulse_width_edges,
            "idle_gap_us": idle_gap_us,
        }

    def configure_diagnostic(self) -> None:
        try:
            config = self._diag_config()
        except ValueError as exc:
            messagebox.showwarning("Invalid diagnostic value", str(exc))
            return

        self.send_command(
            "diag_config "
            f"{config['channel']} "
            f"{config['input_gpio']} "
            f"{config['output_gpio']} "
            f"{config['fire_after_edges']} "
            f"{config['pulse_width_edges']} "
            f"{config['idle_gap_us']}"
        )

    def arm_diagnostic(self) -> None:
        try:
            channel = self._read_diag_channel()
        except ValueError as exc:
            messagebox.showwarning("Invalid diagnostic value", str(exc))
            return

        if self.monitoring_channel is not None:
            self.send_command("monitor_stop")
            self.monitoring_channel = None

        self.send_command(f"diag_arm {channel}")

    def configure_sweep_diagnostic(self) -> None:
        try:
            config = self._sweep_config()
        except ValueError as exc:
            messagebox.showwarning("Invalid sweep value", str(exc))
            return

        self.send_command(
            "diag_sweep_config "
            f"{config['channel']} "
            f"{config['input_gpio']} "
            f"{config['output_gpio']} "
            f"{config['start_edge']} "
            f"{config['stop_edge']} "
            f"{config['step']} "
            f"{config['pulse_width_edges']} "
            f"{config['idle_gap_us']}"
        )

    def arm_sweep_diagnostic(self) -> None:
        try:
            channel = self._read_diag_channel()
        except ValueError as exc:
            messagebox.showwarning("Invalid sweep value", str(exc))
            return

        if self.monitoring_channel is not None:
            self.send_command("monitor_stop")
            self.monitoring_channel = None

        self.send_command(f"diag_sweep_arm {channel}")

    def stop_diagnostic(self) -> None:
        self.send_command("diag_stop")

    def browse_diag_output_path(self) -> None:
        path = filedialog.asksaveasfilename(
            title="Save diagnostic capture",
            defaultextension=".csv",
            filetypes=(("CSV files", "*.csv"), ("All files", "*.*")),
            initialfile=Path(self.diag_output_path_var.get()).name or "diagnostic_log.csv",
        )
        if path:
            self.diag_output_path_var.set(path)

    def save_diagnostic_log(self) -> None:
        path_text = self.diag_output_path_var.get().strip()
        if not path_text:
            messagebox.showwarning("Missing path", "Choose an output file path.")
            return

        csv_path = Path(path_text).expanduser()
        log_path = csv_path.with_suffix(".log")

        try:
            with csv_path.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=DIAG_COLUMNS)
                writer.writeheader()
                writer.writerows(self.diag_rows)

            raw_text = "\n".join(self.diag_raw_lines)
            if raw_text:
                raw_text += "\n"
            log_path.write_text(raw_text, encoding="utf-8")
        except Exception as exc:
            messagebox.showerror("Save failed", str(exc))
            self._log(f"[ERR] diagnostic save failed: {exc}")
            return

        self._log(f"[OK] saved diagnostic CSV: {csv_path}")
        self._log(f"[OK] saved diagnostic raw log: {log_path}")

    def save_sweep_csv(self) -> None:
        self.save_diagnostic_log()

    def clear_diagnostic_log(self) -> None:
        self.diag_rows.clear()
        self.diag_raw_lines.clear()
        if self.diag_tree is not None:
            for item in self.diag_tree.get_children():
                self.diag_tree.delete(item)

    def apply_clock(self, request_status: bool = True) -> bool:
        try:
            clock_freq_khz = self._read_clock_freq_khz()
        except ValueError as exc:
            messagebox.showwarning("Invalid clock value", str(exc))
            return False

        if not self.send_command(f"freq {clock_freq_khz}"):
            return False

        self.pending_clock_save = True
        self.pending_clock_refresh = False
        self.after(900, self._clock_refresh_fallback)
        if request_status:
            self.after(1200, self._clock_refresh_fallback)
        return True

    def _clock_refresh_fallback(self) -> None:
        if self.pending_clock_save:
            self.pending_clock_save = False
            if self.send_command("save"):
                self.pending_clock_refresh = True
                self.after(900, self._clock_refresh_fallback)
            else:
                self.read_status()
            return

        if self.pending_clock_refresh:
            self.pending_clock_refresh = False
            self.read_status()

    def apply_all(self) -> None:
        try:
            configs = self._all_channel_configs()
            clock_freq_khz = self._read_clock_freq_khz()
        except ValueError as exc:
            messagebox.showwarning("Invalid profile value", str(exc))
            return

        if not self.send_command(f"freq {clock_freq_khz}"):
            return
        for index, config in enumerate(configs):
            if not self._send_channel_config(index, config):
                return
        if not self.send_command("save"):
            return
        self.read_status()

    def apply_channel(self, index: int, request_status: bool = True) -> bool:
        try:
            configs = self._all_channel_configs()
        except ValueError as exc:
            messagebox.showwarning("Invalid channel value", str(exc))
            return False

        if not self._send_channel_config(index, configs[index]):
            return False

        if not self.send_command("save"):
            return False

        if request_status:
            self.read_status()
        return True

    def load_device(self) -> None:
        if self.send_command("load"):
            self.pending_load_refresh = True
            self.after(700, self._load_device_refresh_fallback)

    def _load_device_refresh_fallback(self) -> None:
        if not self.pending_load_refresh:
            return

        self.pending_load_refresh = False
        self.read_status()

    def factory_reset_device(self) -> None:
        if not messagebox.askyesno(
            "Factory reset",
            "Restore firmware defaults and erase saved trigger settings?",
        ):
            return

        if self.send_command("factory_reset"):
            self.read_status()

    def _send_channel_config(self, index: int, config: dict[str, Any]) -> bool:
        ch = index + 1
        commands = [f"set {ch} enabled {1 if config['enabled'] else 0}"]

        if config["trigger_mode"] == "time":
            commands.append(f"set {ch} mode time")

        commands.extend(
            [
                f"set {ch} input_gpio {config['input_gpio']}",
                f"set {ch} output_gpio {config['output_gpio']}",
                f"set {ch} pull {config['pull']}",
                f"set {ch} edge {config['edge']}",
                f"set {ch} delay_us {config['delay_us']}",
                f"set {ch} width_us {config['width_us']}",
                f"set {ch} edge_count {config['edge_count']}",
                f"set {ch} pulse_width_edges {config['pulse_width_edges']}",
                f"set {ch} auto_clear_edges {1 if config['auto_clear_edges'] else 0}",
                f"set {ch} auto_clear_delay_ns {config['auto_clear_delay_ns']}",
                f"set {ch} step_reduce_enabled {1 if config['step_reduce_enabled'] else 0}",
                f"set {ch} step_reduce_every {config['step_reduce_every']}",
                f"set {ch} step_reduce_edge_delta {config['step_reduce_edge_delta']}",
                f"set {ch} step_reduce_delay_ns {config['step_reduce_delay_ns']}",
            ]
        )

        if config["trigger_mode"] == "edge_count":
            commands.append(f"set {ch} mode edge_count")

        commands.extend(
            [
                f"set {ch} idle {config['idle']}",
                f"set {ch} active {config['active']}",
            ]
        )

        for command in commands:
            if not self.send_command(command):
                return False

        return True

    def _channel_config(self, index: int) -> dict[str, Any]:
        vars_ = self.channel_vars[index]
        trigger_mode = vars_.trigger_mode.get()
        edge = vars_.edge.get()
        pull = vars_.pull.get()
        idle = vars_.idle.get()
        active = vars_.active.get()

        if trigger_mode not in TRIGGER_MODE_VALUES:
            raise ValueError(f"Channel {index + 1}: mode must be time or edge_count.")
        if edge not in EDGE_VALUES:
            raise ValueError(f"Channel {index + 1}: edge must be rising, falling, or both.")
        if pull not in PULL_VALUES:
            raise ValueError(f"Channel {index + 1}: pull must be down, up, or none.")
        if idle not in LEVEL_VALUES:
            raise ValueError(f"Channel {index + 1}: idle must be low or high.")
        if active not in LEVEL_VALUES:
            raise ValueError(f"Channel {index + 1}: active must be low or high.")

        input_gpio = self._read_gpio(vars_.input_gpio.get(), f"Channel {index + 1} input GPIO")
        output_gpio = self._read_gpio(vars_.output_gpio.get(), f"Channel {index + 1} output GPIO")
        if input_gpio == output_gpio:
            raise ValueError(f"Channel {index + 1}: input and output GPIO must be different.")
        if trigger_mode == "edge_count" and input_gpio % 2 == 0:
            raise ValueError(
                f"Channel {index + 1}: edge_count mode needs a PWM B-channel input pin such as GP3, GP5, or GP7."
            )

        delay_us = self._read_u32(vars_.delay_us.get(), f"Channel {index + 1} delay_us")
        width_us = self._read_u32(vars_.width_us.get(), f"Channel {index + 1} width_us")
        if width_us == 0:
            raise ValueError(f"Channel {index + 1}: width_us must be greater than 0.")
        edge_count = self._read_edge_count(
            vars_.edge_count.get(), f"Channel {index + 1} edge_count"
        )
        pulse_width_edges = self._read_edge_count(
            vars_.pulse_width_edges.get(), f"Channel {index + 1} pulse_width_edges"
        )
        auto_clear_delay_ns = self._read_u32(
            vars_.auto_clear_delay_ns.get(), f"Channel {index + 1} auto_clear_delay_ns"
        )
        step_reduce_every = self._read_u32(
            vars_.step_reduce_every.get(), f"Channel {index + 1} step_reduce_every"
        )
        if step_reduce_every == 0:
            raise ValueError(f"Channel {index + 1}: step_reduce_every must be greater than 0.")
        step_reduce_edge_delta = self._read_u32(
            vars_.step_reduce_edge_delta.get(),
            f"Channel {index + 1} step_reduce_edge_delta",
        )
        if step_reduce_edge_delta > EDGE_COUNT_MAX:
            raise ValueError(
                f"Channel {index + 1}: step_reduce_edge_delta must be between 0 and {EDGE_COUNT_MAX}."
            )
        step_reduce_delay_ns = self._read_u32(
            vars_.step_reduce_delay_ns.get(), f"Channel {index + 1} step_reduce_delay_ns"
        )

        return {
            "enabled": vars_.enabled.get(),
            "input_gpio": input_gpio,
            "output_gpio": output_gpio,
            "trigger_mode": trigger_mode,
            "pull": pull,
            "edge": edge,
            "delay_us": delay_us,
            "width_us": width_us,
            "edge_count": edge_count,
            "pulse_width_edges": pulse_width_edges,
            "auto_clear_edges": vars_.auto_clear_edges.get(),
            "auto_clear_delay_ns": auto_clear_delay_ns,
            "step_reduce_enabled": vars_.step_reduce_enabled.get(),
            "step_reduce_every": step_reduce_every,
            "step_reduce_edge_delta": step_reduce_edge_delta,
            "step_reduce_delay_ns": step_reduce_delay_ns,
            "idle": idle,
            "active": active,
        }

    def _all_channel_configs(self) -> list[dict[str, Any]]:
        configs = [self._channel_config(index) for index in range(CHANNEL_COUNT)]
        used: dict[int, str] = {}
        for index, config in enumerate(configs):
            for role in ("input_gpio", "output_gpio"):
                gpio = config[role]
                label = f"CH{index + 1} {role.replace('_gpio', '')}"
                if gpio in used:
                    raise ValueError(f"GPIO {gpio} is used by both {used[gpio]} and {label}.")
                used[gpio] = label

        used_slices: dict[int, str] = {}
        for index, config in enumerate(configs):
            if config["trigger_mode"] != "edge_count":
                continue
            pwm_slice = (config["input_gpio"] >> 1) & 0x7
            label = f"CH{index + 1} edge counter"
            if pwm_slice in used_slices:
                raise ValueError(
                    f"PWM slice {pwm_slice} is used by both {used_slices[pwm_slice]} and {label}."
                )
            used_slices[pwm_slice] = label
        return configs

    def _read_edge_count(self, value: Any, label: str) -> int:
        number = self._read_u32(value, label)
        if number == 0 or number > EDGE_COUNT_MAX:
            raise ValueError(f"{label} must be between 1 and {EDGE_COUNT_MAX}.")
        return number

    def _read_clock_freq_khz(self) -> int:
        clock_freq_khz = self._read_u32(self.clock_freq_var.get(), "Clock frequency")
        if clock_freq_khz < CLOCK_FREQ_MIN_KHZ or clock_freq_khz > CLOCK_FREQ_MAX_KHZ:
            raise ValueError(
                f"Clock frequency must be between {CLOCK_FREQ_MIN_KHZ} and {CLOCK_FREQ_MAX_KHZ} kHz."
            )
        return clock_freq_khz

    def _profile_data(self) -> dict[str, Any]:
        settable_keys = (
            "enabled",
            "input_gpio",
            "output_gpio",
            "trigger_mode",
            "pull",
            "edge",
            "delay_us",
            "width_us",
            "edge_count",
            "pulse_width_edges",
            "auto_clear_edges",
            "auto_clear_delay_ns",
            "step_reduce_enabled",
            "step_reduce_every",
            "step_reduce_edge_delta",
            "step_reduce_delay_ns",
            "idle",
            "active",
        )
        channels = []
        for config in self._all_channel_configs():
            channels.append({key: config[key] for key in settable_keys})

        return {
            "schema": PROFILE_SCHEMA,
            "clock_freq_khz": self._read_clock_freq_khz(),
            "channels": channels,
        }

    def save_profile(self) -> None:
        try:
            profile = self._profile_data()
        except ValueError as exc:
            messagebox.showwarning("Invalid profile", str(exc))
            return

        path = filedialog.asksaveasfilename(
            title="Save RP2040 trigger profile",
            defaultextension=".json",
            filetypes=(("JSON files", "*.json"), ("All files", "*.*")),
            initialfile="rp2040_trigger_profile.json",
        )
        if not path:
            return

        Path(path).write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")
        self._log(f"[OK] saved profile: {path}")

    def load_profile(self) -> None:
        path = filedialog.askopenfilename(
            title="Load RP2040 trigger profile",
            filetypes=(("JSON files", "*.json"), ("All files", "*.*")),
        )
        if not path:
            return

        try:
            data = json.loads(Path(path).read_text(encoding="utf-8"))
            self._apply_profile_data(data)
        except Exception as exc:
            messagebox.showerror("Load failed", str(exc))
            self._log(f"[ERR] load failed: {exc}")
            return

        self._log(f"[OK] loaded profile: {path}")

    def _apply_profile_data(self, data: dict[str, Any]) -> None:
        if data.get("schema") not in (1, 2, 3, 4, 5, PROFILE_SCHEMA):
            raise ValueError(f"Unsupported profile schema: {data.get('schema')!r}")

        if "clock_freq_khz" in data:
            clock_freq_khz = self._read_u32(data["clock_freq_khz"], "clock_freq_khz")
            if clock_freq_khz < CLOCK_FREQ_MIN_KHZ or clock_freq_khz > CLOCK_FREQ_MAX_KHZ:
                raise ValueError(
                    f"clock_freq_khz must be between {CLOCK_FREQ_MIN_KHZ} and {CLOCK_FREQ_MAX_KHZ}."
                )
            self.clock_freq_var.set(str(clock_freq_khz))

        channels = data.get("channels")
        if not isinstance(channels, list) or len(channels) != CHANNEL_COUNT:
            raise ValueError(f"Profile must contain {CHANNEL_COUNT} channels.")

        for index, channel in enumerate(channels):
            if not isinstance(channel, dict):
                raise ValueError(f"Channel {index + 1} must be an object.")
            self._set_channel_vars(index, channel)

    def _set_channel_vars(self, index: int, channel: dict[str, Any]) -> None:
        vars_ = self.channel_vars[index]
        vars_.enabled.set(bool(channel.get("enabled", True)))

        trigger_mode = channel.get("trigger_mode", "time")
        edge = channel.get("edge", "rising")
        pull = channel.get("pull", "down")
        idle = channel.get("idle", "low")
        active = channel.get("active", "high")
        if trigger_mode not in TRIGGER_MODE_VALUES:
            raise ValueError(f"Channel {index + 1}: invalid mode {trigger_mode!r}.")
        if edge not in EDGE_VALUES:
            raise ValueError(f"Channel {index + 1}: invalid edge {edge!r}.")
        if pull not in PULL_VALUES:
            raise ValueError(f"Channel {index + 1}: invalid pull {pull!r}.")
        if idle not in LEVEL_VALUES:
            raise ValueError(f"Channel {index + 1}: invalid idle {idle!r}.")
        if active not in LEVEL_VALUES:
            raise ValueError(f"Channel {index + 1}: invalid active {active!r}.")

        if "input_gpio" in channel:
            vars_.input_gpio.set(str(self._read_gpio(channel["input_gpio"], "input_gpio")))
        if "output_gpio" in channel:
            vars_.output_gpio.set(str(self._read_gpio(channel["output_gpio"], "output_gpio")))

        vars_.trigger_mode.set(trigger_mode)
        self._update_channel_mode_ui(index)
        vars_.edge.set(edge)
        vars_.pull.set(pull)
        vars_.delay_us.set(str(self._read_u32(channel.get("delay_us", 0), "delay_us")))
        width_us = self._read_u32(channel.get("width_us", 100), "width_us")
        if width_us == 0:
            raise ValueError(f"Channel {index + 1}: width_us must be greater than 0.")
        vars_.width_us.set(str(width_us))
        vars_.edge_count.set(
            str(self._read_edge_count(channel.get("edge_count", DEFAULT_EDGE_COUNT), "edge_count"))
        )
        vars_.pulse_width_edges.set(
            str(
                self._read_edge_count(
                    channel.get("pulse_width_edges", DEFAULT_PULSE_WIDTH_EDGES),
                    "pulse_width_edges",
                )
            )
        )
        vars_.auto_clear_edges.set(bool(channel.get("auto_clear_edges", True)))
        vars_.auto_clear_delay_ns.set(
            str(
                self._read_u32(
                    channel.get("auto_clear_delay_ns", DEFAULT_AUTO_CLEAR_DELAY_NS),
                    "auto_clear_delay_ns",
                )
            )
        )
        vars_.step_reduce_enabled.set(bool(channel.get("step_reduce_enabled", False)))
        step_reduce_every = self._read_u32(
            channel.get("step_reduce_every", DEFAULT_STEP_REDUCE_EVERY),
            "step_reduce_every",
        )
        if step_reduce_every == 0:
            raise ValueError(f"Channel {index + 1}: step_reduce_every must be greater than 0.")
        vars_.step_reduce_every.set(str(step_reduce_every))
        step_reduce_edge_delta = self._read_u32(
            channel.get("step_reduce_edge_delta", DEFAULT_STEP_REDUCE_EDGE_DELTA),
            "step_reduce_edge_delta",
        )
        if step_reduce_edge_delta > EDGE_COUNT_MAX:
            raise ValueError(
                f"Channel {index + 1}: step_reduce_edge_delta must be between 0 and {EDGE_COUNT_MAX}."
            )
        vars_.step_reduce_edge_delta.set(str(step_reduce_edge_delta))
        vars_.step_reduce_delay_ns.set(
            str(
                self._read_u32(
                    channel.get("step_reduce_delay_ns", DEFAULT_STEP_REDUCE_DELAY_NS),
                    "step_reduce_delay_ns",
                )
            )
        )
        vars_.step_reduce_count.set("0")
        vars_.step_current_edge_count.set(vars_.edge_count.get())
        vars_.step_current_delay_ns.set(str(self._read_u32(vars_.delay_us.get(), "delay_us") * 1000))
        vars_.idle.set(idle)
        vars_.active.set(active)

    def _read_u32(self, value: Any, label: str) -> int:
        try:
            number = int(str(value).strip())
        except ValueError as exc:
            raise ValueError(f"{label} must be a whole number.") from exc
        if number < 0 or number > 0xFFFFFFFF:
            raise ValueError(f"{label} must be between 0 and 4294967295.")
        return number

    def _read_gpio(self, value: Any, label: str) -> int:
        number = self._read_u32(value, label)
        if number > 29:
            raise ValueError(f"{label} must be between 0 and 29.")
        if number == 25:
            raise ValueError(f"{label} cannot be GPIO 25 because it is used for the onboard LED.")
        return number

    def _log(self, text: str) -> None:
        self.console.configure(state="normal")
        self.console.insert("end", text + "\n")
        self.console.see("end")
        self.console.configure(state="disabled")

    def _on_close(self) -> None:
        if self.serial_conn is not None:
            self.disconnect()
        self.destroy()


def main() -> None:
    app = TriggerConfigurator()
    app.mainloop()


if __name__ == "__main__":
    main()
