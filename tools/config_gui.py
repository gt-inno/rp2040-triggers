"""Tkinter GUI for configuring the RP2040 4-trigger firmware over USB serial."""

from __future__ import annotations

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
EDGE_VALUES = ("rising", "falling", "both")
LEVEL_VALUES = ("low", "high")
PULL_VALUES = ("down", "up", "none")
PROFILE_SCHEMA = 2

STATUS_RE = re.compile(
    r"^CH(?P<ch>[1-4]):\s+"
    r"enabled=(?P<enabled>[01])\s+"
    r"input=GP(?P<input_gpio>\d+)\s+"
    r"output=GP(?P<output_gpio>\d+)\s+"
    r"edge=(?P<edge>rising|falling|both)\s+"
    r"delay_us=(?P<delay_us>\d+)\s+"
    r"width_us=(?P<width_us>\d+)\s+"
    r"idle=(?P<idle>low|high)\s+"
    r"active=(?P<active>low|high)\s+"
    r"pending=(?P<pending>[01])\s+"
    r"events=(?P<events>\d+)\s+"
    r"last_event_us=(?P<last_event_us>\d+)"
    r"(?:\s+input_level=(?P<input_level>[01])\s+"
    r"output_level=(?P<output_level>[01])\s+"
    r"pull=(?P<pull>none|up|down))?"
)


@dataclass
class ChannelVars:
    enabled: tk.BooleanVar
    input_gpio: tk.StringVar
    output_gpio: tk.StringVar
    pull: tk.StringVar
    edge: tk.StringVar
    delay_us: tk.StringVar
    width_us: tk.StringVar
    idle: tk.StringVar
    active: tk.StringVar
    input_level: tk.StringVar
    output_level: tk.StringVar
    pending: tk.StringVar
    events: tk.StringVar
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
        self.manual_command_var = tk.StringVar()
        self.connected_widgets: list[ttk.Widget] = []

        self.channel_vars = [self._new_channel_vars(i) for i in range(CHANNEL_COUNT)]

        self._configure_style()
        self._build_layout()
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

    def _new_channel_vars(self, index: int) -> ChannelVars:
        return ChannelVars(
            enabled=tk.BooleanVar(value=True),
            input_gpio=tk.StringVar(value=str(2 + index)),
            output_gpio=tk.StringVar(value=str(10 + index)),
            pull=tk.StringVar(value="down"),
            edge=tk.StringVar(value="rising"),
            delay_us=tk.StringVar(value="0"),
            width_us=tk.StringVar(value="100"),
            idle=tk.StringVar(value="low"),
            active=tk.StringVar(value="high"),
            input_level=tk.StringVar(value="0"),
            output_level=tk.StringVar(value="0"),
            pending=tk.StringVar(value="0"),
            events=tk.StringVar(value="0"),
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

        body = ttk.PanedWindow(root, orient="horizontal")
        body.grid(row=2, column=0, pady=(10, 0), sticky="nsew")

        controls = ttk.Frame(body)
        controls.columnconfigure(0, weight=1)
        controls.rowconfigure(0, weight=1)

        console = ttk.Frame(body)
        console.columnconfigure(0, weight=1)
        console.rowconfigure(0, weight=1)

        body.add(controls, weight=3)
        body.add(console, weight=2)

        self._build_channel_grid(controls)
        self._build_profile_bar(controls)
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

    def _build_channel_grid(self, parent: ttk.Frame) -> None:
        frame = ttk.Frame(parent)
        frame.grid(row=0, column=0, sticky="nsew")
        frame.columnconfigure(0, weight=1)
        frame.columnconfigure(1, weight=1)
        frame.rowconfigure(0, weight=1)
        frame.rowconfigure(1, weight=1)

        for index, vars_ in enumerate(self.channel_vars):
            channel = ttk.LabelFrame(frame, text=f"Channel {index + 1}", padding=10)
            row = index // 2
            col = index % 2
            channel.grid(row=row, column=col, padx=6, pady=6, sticky="nsew")
            channel.columnconfigure(1, weight=1)
            self._build_channel_controls(channel, index, vars_)

    def _build_channel_controls(
        self, parent: ttk.LabelFrame, index: int, vars_: ChannelVars
    ) -> None:
        ttk.Checkbutton(parent, text="Enabled", variable=vars_.enabled).grid(
            row=0, column=0, columnspan=2, sticky="w"
        )

        ttk.Label(parent, text="Input GP").grid(row=1, column=0, sticky="w", pady=(8, 0))
        ttk.Spinbox(parent, textvariable=vars_.input_gpio, from_=0, to=29, width=8).grid(
            row=1, column=1, sticky="ew", pady=(8, 0)
        )

        ttk.Label(parent, text="Output GP").grid(row=2, column=0, sticky="w", pady=(6, 0))
        ttk.Spinbox(parent, textvariable=vars_.output_gpio, from_=0, to=29, width=8).grid(
            row=2, column=1, sticky="ew", pady=(6, 0)
        )

        ttk.Label(parent, text="Input Pull").grid(row=3, column=0, sticky="w", pady=(6, 0))
        ttk.Combobox(
            parent, textvariable=vars_.pull, values=PULL_VALUES, state="readonly"
        ).grid(row=3, column=1, sticky="ew", pady=(6, 0))

        ttk.Label(parent, text="Input Edge").grid(row=4, column=0, sticky="w", pady=(6, 0))
        ttk.Combobox(
            parent, textvariable=vars_.edge, values=EDGE_VALUES, state="readonly"
        ).grid(row=4, column=1, sticky="ew", pady=(6, 0))

        ttk.Label(parent, text="Delay us").grid(row=5, column=0, sticky="w", pady=(6, 0))
        ttk.Spinbox(parent, textvariable=vars_.delay_us, from_=0, to=4294967295).grid(
            row=5, column=1, sticky="ew", pady=(6, 0)
        )

        ttk.Label(parent, text="Width us").grid(row=6, column=0, sticky="w", pady=(6, 0))
        ttk.Spinbox(parent, textvariable=vars_.width_us, from_=1, to=4294967295).grid(
            row=6, column=1, sticky="ew", pady=(6, 0)
        )

        ttk.Label(parent, text="Output Idle").grid(row=7, column=0, sticky="w", pady=(6, 0))
        ttk.Combobox(
            parent, textvariable=vars_.idle, values=LEVEL_VALUES, state="readonly"
        ).grid(row=7, column=1, sticky="ew", pady=(6, 0))

        ttk.Label(parent, text="Output Active").grid(row=8, column=0, sticky="w", pady=(6, 0))
        ttk.Combobox(
            parent, textvariable=vars_.active, values=LEVEL_VALUES, state="readonly"
        ).grid(row=8, column=1, sticky="ew", pady=(6, 0))

        stats = ttk.Frame(parent)
        stats.grid(row=9, column=0, columnspan=2, sticky="ew", pady=(10, 0))
        stats.columnconfigure(1, weight=1)
        stats.columnconfigure(3, weight=1)
        stats.columnconfigure(5, weight=1)

        ttk.Label(stats, text="In").grid(row=0, column=0, sticky="w")
        ttk.Label(stats, textvariable=vars_.input_level).grid(row=0, column=1, sticky="w")
        ttk.Label(stats, text="Out").grid(row=0, column=2, padx=(12, 0), sticky="w")
        ttk.Label(stats, textvariable=vars_.output_level).grid(row=0, column=3, sticky="w")
        ttk.Label(stats, text="Pending").grid(row=0, column=4, padx=(12, 0), sticky="w")
        ttk.Label(stats, textvariable=vars_.pending).grid(row=0, column=5, sticky="w")
        ttk.Label(stats, text="Events").grid(row=1, column=0, sticky="w")
        ttk.Label(stats, textvariable=vars_.events).grid(row=1, column=1, sticky="w")
        ttk.Label(stats, text="Last us").grid(row=1, column=2, padx=(12, 0), sticky="w")
        ttk.Label(stats, textvariable=vars_.last_event_us).grid(row=1, column=3, sticky="w")

        buttons = ttk.Frame(parent)
        buttons.grid(row=10, column=0, columnspan=2, sticky="ew", pady=(10, 0))
        buttons.columnconfigure(0, weight=1)
        buttons.columnconfigure(1, weight=1)
        buttons.columnconfigure(2, weight=1)
        buttons.columnconfigure(3, weight=1)

        apply_button = ttk.Button(
            buttons,
            text=f"Apply CH{index + 1}",
            command=lambda ch=index: self.apply_channel(ch),
        )
        apply_button.grid(row=0, column=0, padx=(0, 4), sticky="ew")
        self.connected_widgets.append(apply_button)

        fire_button = ttk.Button(
            buttons,
            text=f"Fire CH{index + 1}",
            command=lambda ch=index: self.fire_channel(ch),
        )
        fire_button.grid(row=0, column=1, padx=(4, 0), sticky="ew")
        self.connected_widgets.append(fire_button)

        active_button = ttk.Button(
            buttons,
            text="Drive Active",
            command=lambda ch=index: self.drive_channel(ch, "active"),
        )
        active_button.grid(row=0, column=2, padx=(8, 4), sticky="ew")
        self.connected_widgets.append(active_button)

        idle_button = ttk.Button(
            buttons,
            text="Drive Idle",
            command=lambda ch=index: self.drive_channel(ch, "idle"),
        )
        idle_button.grid(row=0, column=3, padx=(4, 0), sticky="ew")
        self.connected_widgets.append(idle_button)

    def _build_profile_bar(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="Profile", padding=10)
        frame.grid(row=1, column=0, pady=(10, 0), sticky="ew")
        frame.columnconfigure(3, weight=1)

        self.apply_all_button = ttk.Button(frame, text="Apply All", command=self.apply_all)
        self.apply_all_button.grid(row=0, column=0, padx=(0, 8))
        self.connected_widgets.append(self.apply_all_button)

        ttk.Button(frame, text="Load JSON", command=self.load_profile).grid(
            row=0, column=1, padx=(0, 8)
        )
        ttk.Button(frame, text="Save JSON", command=self.save_profile).grid(
            row=0, column=2, padx=(0, 8)
        )

    def _build_console(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="Console", padding=10)
        frame.grid(row=0, column=0, sticky="nsew")
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(0, weight=1)

        self.console = tk.Text(frame, width=52, height=28, wrap="word", state="disabled")
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
            else:
                self._log(f"[ERR] serial read failed: {text}")
                self.disconnect()

        self.after(50, self._poll_serial_queue)

    def _parse_device_line(self, text: str) -> None:
        if text.startswith("Armed:"):
            state = text.split(":", 1)[1].strip()
            self.armed_var.set("Yes" if state == "yes" else "No")
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
        vars_.edge.set(data["edge"])
        if data.get("pull"):
            vars_.pull.set(data["pull"])
        vars_.delay_us.set(data["delay_us"])
        vars_.width_us.set(data["width_us"])
        vars_.idle.set(data["idle"])
        vars_.active.set(data["active"])
        if data.get("input_level") is not None:
            vars_.input_level.set(data["input_level"])
        if data.get("output_level") is not None:
            vars_.output_level.set(data["output_level"])
        vars_.pending.set(data["pending"])
        vars_.events.set(data["events"])
        vars_.last_event_us.set(data["last_event_us"])

    def _selected_port(self) -> str:
        return self.port_var.get().split(" - ", 1)[0].strip()

    def _set_connected_state(self, connected: bool) -> None:
        self.connect_button.configure(text="Disconnect" if connected else "Connect")
        state = "normal" if connected else "disabled"
        for widget in self.connected_widgets:
            widget.configure(state=state)

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

    def apply_all(self) -> None:
        try:
            configs = self._all_channel_configs()
        except ValueError as exc:
            messagebox.showwarning("Invalid channel value", str(exc))
            return

        for index, config in enumerate(configs):
            if not self._send_channel_config(index, config):
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

        if request_status:
            self.read_status()
        return True

    def _send_channel_config(self, index: int, config: dict[str, Any]) -> bool:
        ch = index + 1
        commands = [
            f"set {ch} enabled {1 if config['enabled'] else 0}",
            f"set {ch} input_gpio {config['input_gpio']}",
            f"set {ch} output_gpio {config['output_gpio']}",
            f"set {ch} pull {config['pull']}",
            f"set {ch} edge {config['edge']}",
            f"set {ch} delay_us {config['delay_us']}",
            f"set {ch} width_us {config['width_us']}",
            f"set {ch} idle {config['idle']}",
            f"set {ch} active {config['active']}",
        ]

        for command in commands:
            if not self.send_command(command):
                return False

        return True

    def _channel_config(self, index: int) -> dict[str, Any]:
        vars_ = self.channel_vars[index]
        edge = vars_.edge.get()
        pull = vars_.pull.get()
        idle = vars_.idle.get()
        active = vars_.active.get()

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

        delay_us = self._read_u32(vars_.delay_us.get(), f"Channel {index + 1} delay_us")
        width_us = self._read_u32(vars_.width_us.get(), f"Channel {index + 1} width_us")
        if width_us == 0:
            raise ValueError(f"Channel {index + 1}: width_us must be greater than 0.")

        return {
            "enabled": vars_.enabled.get(),
            "input_gpio": input_gpio,
            "output_gpio": output_gpio,
            "pull": pull,
            "edge": edge,
            "delay_us": delay_us,
            "width_us": width_us,
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
        return configs

    def _profile_data(self) -> dict[str, Any]:
        settable_keys = (
            "enabled",
            "input_gpio",
            "output_gpio",
            "pull",
            "edge",
            "delay_us",
            "width_us",
            "idle",
            "active",
        )
        channels = []
        for config in self._all_channel_configs():
            channels.append({key: config[key] for key in settable_keys})

        return {
            "schema": PROFILE_SCHEMA,
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
        if data.get("schema") not in (1, PROFILE_SCHEMA):
            raise ValueError(f"Unsupported profile schema: {data.get('schema')!r}")

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

        edge = channel.get("edge", "rising")
        pull = channel.get("pull", "down")
        idle = channel.get("idle", "low")
        active = channel.get("active", "high")
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

        vars_.edge.set(edge)
        vars_.pull.set(pull)
        vars_.delay_us.set(str(self._read_u32(channel.get("delay_us", 0), "delay_us")))
        width_us = self._read_u32(channel.get("width_us", 100), "width_us")
        if width_us == 0:
            raise ValueError(f"Channel {index + 1}: width_us must be greater than 0.")
        vars_.width_us.set(str(width_us))
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
