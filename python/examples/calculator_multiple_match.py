from __future__ import annotations

import re
import subprocess
from pathlib import Path

from litewinwrap import Automation, Goldens, TargetAmbiguousError


def main() -> None:
    automation = Automation(threshold=0.92)
    subprocess.Popen(["calc.exe"])

    calculator = automation.find_window(
        re.compile(r"^Calculator$", re.IGNORECASE),
        timeout_seconds=5.0,
    )
    calculator.focus()

    example_dir = Path(__file__).parent
    targets = Goldens.from_root(example_dir)

    for _ in range(6):
        calculator.click(targets["calculator/button_1"])
    print("Pressed button_1 six times")

    try:
        calculator.locate(targets["calculator_ones/multiple_match"])
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
