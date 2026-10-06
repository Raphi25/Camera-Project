"""Tk widget construction; behavior is injected through action dataclasses."""

import tkinter as tk
from dataclasses import dataclass
from tkinter import ttk
from typing import Callable


@dataclass(frozen=True)
class MainPageVariables:
    port: tk.StringVar
    transfer_status: tk.StringVar
    transfer_progress: tk.DoubleVar
    device_status: tk.StringVar
    sd_status: tk.StringVar
    otg_status: tk.StringVar
    schedule_preview: tk.StringVar


@dataclass(frozen=True)
class MainPageActions:
    show_settings: Callable[[], None]
    refresh_ports: Callable[[], None]
    start_program: Callable[[], None]
    stop_program: Callable[[], None]
    retrieve_media: Callable[[], None]
    eject_otg: Callable[[], None]
    delete_media: Callable[[], None]
    refresh_summaries: Callable[[], None]
    open_summary: Callable[[], None]
    delete_summaries: Callable[[], None]
    refresh_images: Callable[[], None]
    open_image: Callable[[], None]
    select_image: Callable


@dataclass(frozen=True)
class MainPageWidgets:
    port_combo: ttk.Combobox
    media_transfer_progress: ttk.Progressbar
    summary_listbox: tk.Listbox
    summary_text: tk.Text
    image_listbox: tk.Listbox
    image_preview: ttk.Label
    log: tk.Text


def build_main_page(
    frame: ttk.Frame,
    variables: MainPageVariables,
    actions: MainPageActions,
) -> MainPageWidgets:
    title_row = ttk.Frame(frame)
    title_row.grid(row=0, column=0, columnspan=5, sticky="we", pady=(0, 10))
    ttk.Label(
        title_row, text="GUI over USB-C / BT", font=("Segoe UI", 13, "bold")
    ).pack(side=tk.LEFT)
    ttk.Button(title_row, text="Settings", command=actions.show_settings).pack(
        side=tk.RIGHT
    )

    ttk.Label(frame, text="COM Port").grid(row=1, column=0, sticky="w")
    port_combo = ttk.Combobox(
        frame, textvariable=variables.port, state="readonly", width=20
    )
    port_combo.grid(row=1, column=1, sticky="we", padx=(8, 8))
    ttk.Button(frame, text="Refresh", command=actions.refresh_ports).grid(
        row=1, column=2, sticky="w"
    )

    button_row = ttk.Frame(frame)
    button_row.grid(row=2, column=0, columnspan=5, sticky="w", pady=(14, 8))
    ttk.Button(button_row, text="Start Program", command=actions.start_program).pack(
        side=tk.LEFT
    )
    ttk.Button(button_row, text="Stop Program", command=actions.stop_program).pack(
        side=tk.LEFT, padx=(8, 0)
    )
    ttk.Button(button_row, text="Get SD Media", command=actions.retrieve_media).pack(
        side=tk.LEFT, padx=(8, 0)
    )
    ttk.Button(button_row, text="Eject OTG", command=actions.eject_otg).pack(
        side=tk.LEFT, padx=(8, 0)
    )
    ttk.Button(
        button_row, text="Delete All SD Media", command=actions.delete_media
    ).pack(side=tk.LEFT, padx=(8, 0))

    transfer = ttk.Frame(frame)
    transfer.grid(row=3, column=0, columnspan=5, sticky="we", pady=(0, 8))
    ttk.Label(transfer, textvariable=variables.transfer_status).grid(
        row=0, column=0, sticky="w"
    )
    progress = ttk.Progressbar(
        transfer,
        variable=variables.transfer_progress,
        maximum=100.0,
        mode="determinate",
    )
    progress.grid(row=1, column=0, sticky="we", pady=(4, 0))
    transfer.columnconfigure(0, weight=1)

    ttk.Label(
        frame,
        text="PC time is synced once automatically when the GUI detects the device.",
    ).grid(row=4, column=0, columnspan=5, sticky="w", pady=(0, 8))

    dashboard = ttk.LabelFrame(frame, text="Device status dashboard", padding=8)
    dashboard.grid(row=5, column=0, columnspan=5, sticky="we", pady=(4, 8))
    ttk.Label(dashboard, textvariable=variables.device_status).grid(
        row=0, column=0, sticky="w"
    )
    ttk.Label(dashboard, textvariable=variables.sd_status).grid(
        row=1, column=0, sticky="w", pady=(4, 0)
    )
    ttk.Label(dashboard, textvariable=variables.otg_status).grid(
        row=2, column=0, sticky="w", pady=(4, 0)
    )
    ttk.Label(
        dashboard,
        textvariable=variables.schedule_preview,
        foreground="#555555",
    ).grid(row=3, column=0, sticky="w", pady=(4, 0))
    dashboard.columnconfigure(0, weight=1)

    notebook = ttk.Notebook(frame)
    notebook.grid(row=6, column=0, columnspan=5, sticky="nsew", pady=(0, 8))

    summary = ttk.Frame(notebook, padding=8)
    notebook.add(summary, text="Daily summaries")
    summary_buttons = ttk.Frame(summary)
    summary_buttons.grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 6))
    ttk.Button(
        summary_buttons, text="Refresh Summaries", command=actions.refresh_summaries
    ).pack(side=tk.LEFT)
    ttk.Button(
        summary_buttons, text="Open Selected", command=actions.open_summary
    ).pack(side=tk.LEFT, padx=(8, 0))
    ttk.Button(
        summary_buttons, text="Delete Summaries", command=actions.delete_summaries
    ).pack(side=tk.LEFT, padx=(8, 0))
    summary_listbox = tk.Listbox(summary, height=5, exportselection=False)
    summary_listbox.grid(row=1, column=0, sticky="nsew")
    summary_scroll = ttk.Scrollbar(
        summary, orient="vertical", command=summary_listbox.yview
    )
    summary_scroll.grid(row=1, column=1, sticky="ns")
    summary_listbox.configure(yscrollcommand=summary_scroll.set)
    summary_text = tk.Text(summary, height=7, wrap="word")
    summary_text.grid(row=1, column=2, sticky="nsew", padx=(8, 0))
    summary.columnconfigure(0, weight=1)
    summary.columnconfigure(2, weight=3)
    summary.rowconfigure(1, weight=1)

    images = ttk.Frame(notebook, padding=8)
    notebook.add(images, text="Images")
    image_buttons = ttk.Frame(images)
    image_buttons.grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 6))
    ttk.Button(
        image_buttons, text="Refresh Images", command=actions.refresh_images
    ).pack(side=tk.LEFT)
    ttk.Button(
        image_buttons, text="Open Selected", command=actions.open_image
    ).pack(side=tk.LEFT, padx=(8, 0))
    image_listbox = tk.Listbox(images, height=8, exportselection=False)
    image_listbox.grid(row=1, column=0, sticky="nsew")
    image_listbox.bind("<<ListboxSelect>>", actions.select_image)
    image_scroll = ttk.Scrollbar(
        images, orient="vertical", command=image_listbox.yview
    )
    image_scroll.grid(row=1, column=1, sticky="ns")
    image_listbox.configure(yscrollcommand=image_scroll.set)
    image_preview = ttk.Label(
        images,
        text="Select a transferred image to preview it.",
        anchor="center",
        relief="sunken",
    )
    image_preview.grid(row=1, column=2, sticky="nsew", padx=(8, 0))
    images.columnconfigure(0, weight=1)
    images.columnconfigure(2, weight=3)
    images.rowconfigure(1, weight=1)

    log = tk.Text(frame, height=10, wrap="word")
    log.grid(row=7, column=0, columnspan=4, sticky="nsew")
    log_scroll = ttk.Scrollbar(frame, orient="vertical", command=log.yview)
    log_scroll.grid(row=7, column=4, sticky="ns")
    log.configure(yscrollcommand=log_scroll.set)
    frame.columnconfigure(1, weight=1)
    frame.rowconfigure(6, weight=1)
    frame.rowconfigure(7, weight=1)
    return MainPageWidgets(
        port_combo,
        progress,
        summary_listbox,
        summary_text,
        image_listbox,
        image_preview,
        log,
    )


@dataclass(frozen=True)
class SettingsPageVariables:
    baud: tk.StringVar
    transport: tk.StringVar
    ble_target: tk.StringVar
    schedule_enabled: tk.BooleanVar
    schedule_start: tk.StringVar
    schedule_stop: tk.StringVar
    schedule_days: tk.StringVar
    capture_interval: tk.StringVar
    burst_enabled: tk.BooleanVar


@dataclass(frozen=True)
class SettingsPageActions:
    show_main: Callable[[], None]
    find_ble: Callable[[], None]
    apply_schedule: Callable[[], None]
    change_password: Callable[[], None]


def build_settings_page(
    frame: ttk.Frame,
    variables: SettingsPageVariables,
    actions: SettingsPageActions,
) -> None:
    title_row = ttk.Frame(frame)
    title_row.grid(row=0, column=0, columnspan=3, sticky="we", pady=(0, 16))
    ttk.Label(title_row, text="Settings", font=("Segoe UI", 13, "bold")).pack(
        side=tk.LEFT
    )
    ttk.Button(title_row, text="Back", command=actions.show_main).pack(side=tk.RIGHT)

    ttk.Label(frame, text="Baud").grid(row=1, column=0, sticky="w")
    ttk.Entry(frame, textvariable=variables.baud, width=12).grid(
        row=1, column=1, sticky="w", padx=(8, 8)
    )
    ttk.Label(frame, text="Transport").grid(row=2, column=0, sticky="w", pady=(10, 0))
    ttk.Combobox(
        frame,
        textvariable=variables.transport,
        state="readonly",
        width=12,
        values=("Serial", "BT"),
    ).grid(row=2, column=1, sticky="w", padx=(8, 8), pady=(10, 0))

    ttk.Label(frame, text="BT Name/Address").grid(
        row=3, column=0, sticky="w", pady=(10, 0)
    )
    bt_row = ttk.Frame(frame)
    bt_row.grid(row=3, column=1, sticky="w", padx=(8, 8), pady=(10, 0))
    ttk.Entry(bt_row, textvariable=variables.ble_target, width=32).pack(side=tk.LEFT)
    ttk.Button(bt_row, text="Find BT", command=actions.find_ble).pack(
        side=tk.LEFT, padx=(8, 0)
    )
    ttk.Separator(frame, orient="horizontal").grid(
        row=4, column=0, columnspan=3, sticky="we", pady=(18, 14)
    )
    ttk.Label(frame, text="Program schedule", font=("Segoe UI", 10, "bold")).grid(
        row=5, column=0, columnspan=3, sticky="w"
    )
    ttk.Checkbutton(
        frame,
        text="Use daily run window",
        variable=variables.schedule_enabled,
    ).grid(row=6, column=0, columnspan=2, sticky="w", pady=(8, 0))

    for row, label, variable, width in (
        (7, "Start time", variables.schedule_start, 8),
        (8, "Stop time", variables.schedule_stop, 8),
        (9, "Days to run", variables.schedule_days, 8),
    ):
        ttk.Label(frame, text=label).grid(row=row, column=0, sticky="w", pady=(8, 0))
        ttk.Entry(frame, textvariable=variable, width=width).grid(
            row=row, column=1, sticky="w", padx=(8, 8), pady=(8, 0)
        )

    ttk.Label(frame, text="Capture interval").grid(
        row=10, column=0, sticky="w", pady=(8, 0)
    )
    interval_row = ttk.Frame(frame)
    interval_row.grid(row=10, column=1, sticky="w", padx=(8, 8), pady=(8, 0))
    ttk.Entry(interval_row, textvariable=variables.capture_interval, width=8).pack(
        side=tk.LEFT
    )
    ttk.Label(interval_row, text="seconds").pack(side=tk.LEFT, padx=(6, 0))
    ttk.Checkbutton(
        frame,
        text="Take three back-to-back burst images each capture cycle",
        variable=variables.burst_enabled,
    ).grid(row=11, column=0, columnspan=3, sticky="w", pady=(10, 0))
    ttk.Button(frame, text="Apply", command=actions.apply_schedule).grid(
        row=12, column=0, sticky="w", pady=(10, 0)
    )
    ttk.Label(
        frame,
        text=(
            "Use HH:MM. Days = 0 means repeat every day indefinitely. "
            "Interval range: 5 to 3600 seconds."
        ),
        foreground="#555555",
    ).grid(row=13, column=0, columnspan=3, sticky="w", pady=(8, 0))
    ttk.Separator(frame, orient="horizontal").grid(
        row=14, column=0, columnspan=3, sticky="we", pady=(18, 14)
    )
    ttk.Button(
        frame, text="Change Password", command=actions.change_password
    ).grid(row=15, column=0, sticky="w")
    ttk.Label(
        frame,
        text=(
            "Settings are applied immediately. Serial reconnects automatically "
            "when baud changes."
        ),
        foreground="#555555",
    ).grid(row=16, column=0, columnspan=3, sticky="w", pady=(16, 0))
    frame.columnconfigure(1, weight=1)
