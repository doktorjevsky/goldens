from __future__ import annotations

import re
import subprocess
from pathlib import Path

from litewinwrap import Automation, Goldens, TargetNotFoundError


def main() -> None:
    automation = Automation(threshold=0.92)
    subprocess.Popen(["calc.exe"])

    calculator = automation.find_window(
        re.compile(r"^Calculator$", re.IGNORECASE),
        timeout_seconds=5.0,
    )
    calculator.focus()
    targets = Goldens.from_png(Path(__file__).with_name("calculator.png"))

    # The not_found crop represents Calculator before its display changes.
    # Pressing a nonzero digit makes that visual state disappear.
    pressed = calculator.click(targets["calculator/button_7"])
    print(f"Pressed button_7 at {pressed.click}")

    try:
        calculator.locate(targets["calculator/not_found"])
    except TargetNotFoundError as error:
        print(f"Target {error.target.name!r} was not found, as expected")
        print(f"  threshold: {error.threshold:.4f}")
        print(f"  best score: {error.best_score:.4f}")
        print(f"  attempts: {error.attempts}")
        print(f"  elapsed: {error.elapsed_seconds:.3f}s")
        if error.last_capture is not None:
            print(f"  last capture: {error.last_capture.rect}")
    else:
        raise RuntimeError("Expected the 'not_found' target to be absent")


if __name__ == "__main__":
    main()
