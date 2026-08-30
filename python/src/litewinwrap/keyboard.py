from __future__ import annotations

import time
from collections.abc import Iterable, Iterator
from contextlib import contextmanager
from typing import TypeAlias

from . import win32


KEYEVENTF_EXTENDEDKEY = 0x0001
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_UNICODE = 0x0004

VK_BACK = 0x08
VK_TAB = 0x09
VK_RETURN = 0x0D
VK_SHIFT = 0x10
VK_CONTROL = 0x11
VK_MENU = 0x12
VK_ESCAPE = 0x1B
VK_SPACE = 0x20
VK_PRIOR = 0x21
VK_NEXT = 0x22
VK_END = 0x23
VK_HOME = 0x24
VK_LEFT = 0x25
VK_UP = 0x26
VK_RIGHT = 0x27
VK_DOWN = 0x28
VK_INSERT = 0x2D
VK_DELETE = 0x2E
VK_LWIN = 0x5B
VK_RWIN = 0x5C
VK_APPS = 0x5D
VK_NUMPAD0 = 0x60
VK_MULTIPLY = 0x6A
VK_ADD = 0x6B
VK_SUBTRACT = 0x6D
VK_DECIMAL = 0x6E
VK_DIVIDE = 0x6F
VK_F1 = 0x70
VK_F24 = 0x87
VK_NUMLOCK = 0x90
VK_SCROLL = 0x91
VK_LSHIFT = 0xA0
VK_RSHIFT = 0xA1
VK_LCONTROL = 0xA2
VK_RCONTROL = 0xA3
VK_LMENU = 0xA4
VK_RMENU = 0xA5
VK_VOLUME_MUTE = 0xAD
VK_VOLUME_DOWN = 0xAE
VK_VOLUME_UP = 0xAF
VK_MEDIA_NEXT_TRACK = 0xB0
VK_MEDIA_PREV_TRACK = 0xB1
VK_MEDIA_STOP = 0xB2
VK_MEDIA_PLAY_PAUSE = 0xB3


Key: TypeAlias = str | int


_NAMED_KEYS = {
    "backspace": VK_BACK,
    "tab": VK_TAB,
    "enter": VK_RETURN,
    "return": VK_RETURN,
    "shift": VK_SHIFT,
    "ctrl": VK_CONTROL,
    "control": VK_CONTROL,
    "alt": VK_MENU,
    "escape": VK_ESCAPE,
    "esc": VK_ESCAPE,
    "space": VK_SPACE,
    "pageup": VK_PRIOR,
    "pgup": VK_PRIOR,
    "pagedown": VK_NEXT,
    "pgdn": VK_NEXT,
    "end": VK_END,
    "home": VK_HOME,
    "left": VK_LEFT,
    "up": VK_UP,
    "right": VK_RIGHT,
    "down": VK_DOWN,
    "insert": VK_INSERT,
    "ins": VK_INSERT,
    "delete": VK_DELETE,
    "del": VK_DELETE,
    "win": VK_LWIN,
    "windows": VK_LWIN,
    "leftwin": VK_LWIN,
    "rightwin": VK_RWIN,
    "menu": VK_APPS,
    "apps": VK_APPS,
    "numlock": VK_NUMLOCK,
    "scrolllock": VK_SCROLL,
    "leftshift": VK_LSHIFT,
    "rightshift": VK_RSHIFT,
    "leftctrl": VK_LCONTROL,
    "rightctrl": VK_RCONTROL,
    "leftalt": VK_LMENU,
    "rightalt": VK_RMENU,
    "multiply": VK_MULTIPLY,
    "add": VK_ADD,
    "subtract": VK_SUBTRACT,
    "decimal": VK_DECIMAL,
    "divide": VK_DIVIDE,
    "volumemute": VK_VOLUME_MUTE,
    "volumedown": VK_VOLUME_DOWN,
    "volumeup": VK_VOLUME_UP,
    "medianext": VK_MEDIA_NEXT_TRACK,
    "mediaprevious": VK_MEDIA_PREV_TRACK,
    "mediastop": VK_MEDIA_STOP,
    "mediaplaypause": VK_MEDIA_PLAY_PAUSE,
}

_EXTENDED_KEYS = {
    VK_PRIOR,
    VK_NEXT,
    VK_END,
    VK_HOME,
    VK_LEFT,
    VK_UP,
    VK_RIGHT,
    VK_DOWN,
    VK_INSERT,
    VK_DELETE,
    VK_RWIN,
    VK_APPS,
    VK_RCONTROL,
    VK_RMENU,
    VK_DIVIDE,
    VK_NUMLOCK,
    VK_VOLUME_MUTE,
    VK_VOLUME_DOWN,
    VK_VOLUME_UP,
    VK_MEDIA_NEXT_TRACK,
    VK_MEDIA_PREV_TRACK,
    VK_MEDIA_STOP,
    VK_MEDIA_PLAY_PAUSE,
}


def _key_code(key: Key) -> int:
    if isinstance(key, bool):
        raise TypeError("A keyboard key must be a name or virtual-key integer")
    if isinstance(key, int):
        if not 0 <= key <= 0xFF:
            raise ValueError("Virtual-key code must be between 0 and 255")
        return key
    if not isinstance(key, str):
        raise TypeError("A keyboard key must be a name or virtual-key integer")

    name = key.casefold().replace("_", "").replace("-", "").replace(" ", "")
    if len(name) == 1 and name.isascii() and name.isalnum():
        return ord(name.upper())
    if name.startswith("f") and name[1:].isdigit():
        number = int(name[1:])
        if 1 <= number <= 24:
            return VK_F1 + number - 1
    if name.startswith("numpad") and name[6:].isdigit():
        number = int(name[6:])
        if 0 <= number <= 9:
            return VK_NUMPAD0 + number
    try:
        return _NAMED_KEYS[name]
    except KeyError as error:
        raise ValueError(f"Unknown keyboard key: {key!r}") from error


def _key_input(vk: int, flags: int = 0) -> win32.INPUT:
    if vk in _EXTENDED_KEYS:
        flags |= KEYEVENTF_EXTENDEDKEY
    value = win32.INPUT()
    value.type = win32.INPUT_KEYBOARD
    value.ki = win32.KEYBDINPUT(
        wVk=vk,
        wScan=0,
        dwFlags=flags,
        time_milliseconds=0,
        dwExtraInfo=0,
    )
    return value


def _unicode_input(unit: int, flags: int = 0) -> win32.INPUT:
    value = win32.INPUT()
    value.type = win32.INPUT_KEYBOARD
    value.ki = win32.KEYBDINPUT(
        wVk=0,
        wScan=unit,
        dwFlags=KEYEVENTF_UNICODE | flags,
        time_milliseconds=0,
        dwExtraInfo=0,
    )
    return value


def down(key: Key) -> int:
    """Hold one named key down until a corresponding ``up`` call."""

    return win32.send_input([_key_input(_key_code(key))])


def up(key: Key) -> int:
    """Release one named key."""

    return win32.send_input([_key_input(_key_code(key), KEYEVENTF_KEYUP)])


def key_down(key: Key) -> int:
    return down(key)


def key_up(key: Key) -> int:
    return up(key)


@contextmanager
def hold(*keys: Key) -> Iterator[None]:
    """Hold a key or chord for the duration of a ``with`` block."""

    if not keys:
        raise ValueError("At least one key is required")
    codes = tuple(_key_code(key) for key in keys)
    win32.send_input([_key_input(code) for code in codes])
    try:
        yield
    finally:
        win32.send_input(
            [_key_input(code, KEYEVENTF_KEYUP) for code in reversed(codes)]
        )


def press(*keys: Key, count: int = 1, interval_seconds: float = 0.0) -> int:
    """Press one key or a chord such as ``press("ctrl", "q")``."""

    if not keys:
        raise ValueError("At least one key is required")
    if count <= 0:
        raise ValueError("Press count must be positive")
    codes = tuple(_key_code(key) for key in keys)
    sent = 0
    for index in range(count):
        inputs = [_key_input(code) for code in codes]
        inputs.extend(
            _key_input(code, KEYEVENTF_KEYUP) for code in reversed(codes)
        )
        sent += win32.send_input(inputs)
        if index + 1 < count and interval_seconds > 0:
            time.sleep(interval_seconds)
    return sent


def hotkey(*keys: Key) -> int:
    if not keys:
        return 0
    return press(*keys)


def write(
    text: str,
    *,
    interval_seconds: float = 0.0,
    chunk_size: int = 256,
) -> int:
    if chunk_size <= 0:
        raise ValueError("Chunk size must be positive")

    encoded = text.encode("utf-16-le")
    units = [
        int.from_bytes(encoded[index : index + 2], "little")
        for index in range(0, len(encoded), 2)
    ]
    pairs = [
        (_unicode_input(unit), _unicode_input(unit, KEYEVENTF_KEYUP))
        for unit in units
    ]

    sent = 0
    if interval_seconds > 0:
        for index, pair in enumerate(pairs):
            sent += win32.send_input(pair)
            if index + 1 < len(pairs):
                time.sleep(interval_seconds)
        return sent

    inputs = [event for pair in pairs for event in pair]
    for index in range(0, len(inputs), chunk_size):
        sent += win32.send_input(inputs[index : index + chunk_size])
    return sent


def type_text(
    text: str,
    *,
    interval_seconds: float = 0.0,
    chunk_size: int = 256,
) -> int:
    """Type literal Unicode text into the foreground application."""

    return write(
        text,
        interval_seconds=interval_seconds,
        chunk_size=chunk_size,
    )


def release(keys: Iterable[Key]) -> int:
    return win32.send_input(
        _key_input(_key_code(key), KEYEVENTF_KEYUP) for key in keys
    )
