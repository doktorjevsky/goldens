from __future__ import annotations

import re
import subprocess
from pathlib import Path

from litewinwrap import Goldens, Window, win32
from litewinwrap.match import TargetNotFoundError


def main() -> None:
    win32.enable_per_monitor_dpi_awareness()
    subprocess.Popen(["calc.exe"])

    calculator = Window.find(
        re.compile(r"^Calculator$", re.IGNORECASE),
        timeout=5.0,
    )
    calculator.focus(timeout=2.0)
    targets = Goldens(Path(__file__).with_name("calculator.png"))

    # The not_found crop represents Calculator before its display changes.
    # Pressing a nonzero digit makes that visual state disappear.
    pressed = calculator.click_target(
        targets["button_7"],
        threshold=0.92,
        timeout=2.0,
    )
    print(f"Pressed button_7 at {pressed.click}")

    try:
        calculator.find_target(
            targets["not_found"],
            threshold=0.92,
            timeout=2.0,
        )
    except TargetNotFoundError as error:
        print(f"Target {error.target.name!r} was not found, as expected")
        print(f"  threshold: {error.threshold:.4f}")
        print(f"  best score: {error.best_score:.4f}")
        print(f"  attempts: {error.attempts}")
        print(f"  elapsed: {error.elapsed:.3f}s")
        if error.last_capture is not None:
            print(f"  last capture: {error.last_capture.rect}")
    else:
        raise RuntimeError("Expected the 'not_found' target to be absent")


if __name__ == "__main__":
    main()
