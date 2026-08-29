from __future__ import annotations

import re
import subprocess
from pathlib import Path

from litewinwrap import Automation, Goldens


def main() -> None:
    automation = Automation(threshold=0.92)
    subprocess.Popen(["calc.exe"])

    calculator = automation.find_window(
        re.compile(r"^Calculator$", re.IGNORECASE),
        timeout_seconds=5.0,
    )
    calculator.focus()
    targets = Goldens.from_root(Path(__file__).parent)

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
        target_name = f"calculator/{name}"
        found = calculator.click(targets[target_name])
        print(f"{target_name:<25} score={found.score:.4f} click={found.click}")


if __name__ == "__main__":
    main()
