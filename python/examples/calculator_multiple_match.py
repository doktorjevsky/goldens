from __future__ import annotations

import re
import subprocess
import time
from pathlib import Path

from litewinwrap import Goldens, Window, win32
from litewinwrap.match import TargetAmbiguousError


def main() -> None:
    win32.enable_per_monitor_dpi_awareness()
    subprocess.Popen(["calc.exe"])

    calculator = Window.find(
        re.compile(r"^Calculator$", re.IGNORECASE),
        timeout=5.0,
    )
    calculator.focus(timeout=2.0)

    example_dir = Path(__file__).parent
    targets = Goldens.from_root(example_dir)

    for _ in range(6):
        calculator.click_target(
            targets["calculator/button_1"],
            threshold=0.92,
            timeout=2.0,
        )
    print("Pressed button_1 six times")

    # SendInput completes before the target application is required to repaint.
    time.sleep(0.25)

    try:
        calculator.find_target(
            targets["calculator_ones/multiple_match"],
            threshold=0.92,
            timeout=2.0,
        )
    except TargetAmbiguousError as error:
        print(
            f"Target {error.target.name!r} matched "
            f"{len(error.matches)} distinct locations, as expected"
        )
        for index, found in enumerate(error.matches, start=1):
            print(
                f"  {index}: score={found.score:.4f} "
                f"rect={found.rect} click={found.click}"
            )
    else:
        raise RuntimeError("Expected 'multiple_match' to be ambiguous")


if __name__ == "__main__":
    main()
