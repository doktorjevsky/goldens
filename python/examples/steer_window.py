from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path

import cv2
import numpy as np

from litewinwrap import Target, Window, keyboard, match, mouse, win32


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Find a window, click a visual template, and optionally type text."
    )
    parser.add_argument("window", help="Case-insensitive regular expression for the title")
    parser.add_argument("template", type=Path, help="Cropped template PNG")
    parser.add_argument("--text", help="Text to type after clicking")
    parser.add_argument("--click-x", type=float, default=0.5)
    parser.add_argument("--click-y", type=float, default=0.5)
    parser.add_argument("--threshold", type=float, default=0.92)
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--start", help="Executable to start before finding the window")
    parser.add_argument(
        "--start-arg",
        action="append",
        default=[],
        help="Argument for --start; may be supplied more than once",
    )
    args = parser.parse_args()

    win32.enable_per_monitor_dpi_awareness()

    process = None
    if args.start is not None:
        process = subprocess.Popen([args.start, *args.start_arg])
        print(f"Started process {process.pid}: {args.start}")

    print("Visible top-level windows:")
    for item in Window.list():
        print(f"  {int(item.hwnd):>10}  {item.title}")

    window = Window.find(
        re.compile(args.window, re.IGNORECASE),
        process_id=process.pid if process is not None else None,
        timeout=args.timeout,
    )
    window.focus(timeout=args.timeout)

    print(f"Selected: {int(window.hwnd)}  {window.title}")
    print("Current visible descendants:")
    for child in window.get_children():
        print(
            f"  {int(child.hwnd):>10}  "
            f"{child.class_name:<24}  {child.title}"
        )

    encoded = np.fromfile(args.template, dtype=np.uint8)
    pixels = cv2.imdecode(encoded, cv2.IMREAD_COLOR)
    if pixels is None:
        raise ValueError(f"Could not decode template image: {args.template}")
    target = Target(
        name=args.template.stem,
        pixels=pixels,
        click=(args.click_x, args.click_y),
    )
    found = match.find(
        window,
        target,
        threshold=args.threshold,
        timeout=args.timeout,
    )
    mouse_events = mouse.click(found.click)
    print(
        f"Clicked {found.target!r} at {found.click}; "
        f"score={found.score:.4f}, rect={found.rect}, "
        f"mouse events accepted={mouse_events}"
    )

    if args.text is not None:
        sent = keyboard.write(args.text)
        print(f"Keyboard events accepted: {sent}")


if __name__ == "__main__":
    main()
