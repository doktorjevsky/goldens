from __future__ import annotations

import re
import subprocess

from litewinwrap import Goldens, Window, win32


def main() -> None:
    win32.enable_per_monitor_dpi_awareness()
    subprocess.Popen(["calc.exe"])

    calculator = Window.find(
        re.compile(r"^Calculator$", re.IGNORECASE),
        timeout=5.0,
    )
    calculator.focus(timeout=2.0)
    targets = Goldens("calculator.png")

    # 1234567890 + 9876543210 - 10 * 2 / 5 =
    # This deliberately exercises every target named in the example resource.
    buttons = [
        *(f"button_{digit}" for digit in "1234567890"),
        "button_plus",
        *(f"button_{digit}" for digit in "9876543210"),
        "button_minus",
        "button_1",
        "button_0",
        "button_times",
        "button_2",
        "button_div",
        "button_5",
        "button_equals",
    ]

    for name in buttons:
        found = calculator.click_target(
            targets[name],
            threshold=0.92,
            timeout=2.0,
        )
        print(f"{name:<14} score={found.score:.4f} click={found.click}")


if __name__ == "__main__":
    main()
