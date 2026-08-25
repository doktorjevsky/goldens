from __future__ import annotations

import time
from collections.abc import Iterable

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
}


def _key_input(vk: int, flags: int = 0) -> win32.INPUT:
    if vk in _EXTENDED_KEYS:
        flags |= KEYEVENTF_EXTENDEDKEY
    value = win32.INPUT()
    value.type = win32.INPUT_KEYBOARD
    value.ki = win32.KEYBDINPUT(
        wVk=vk,
        wScan=0,
        dwFlags=flags,
        time=0,
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
        time=0,
        dwExtraInfo=0,
    )
    return value


def key_down(vk: int) -> int:
    return win32.send_input([_key_input(vk)])


def key_up(vk: int) -> int:
    return win32.send_input([_key_input(vk, KEYEVENTF_KEYUP)])


def press(vk: int, *, count: int = 1, interval: float = 0.0) -> int:
    if count <= 0:
        raise ValueError("Press count must be positive")
    sent = 0
    for index in range(count):
        sent += win32.send_input(
            [_key_input(vk), _key_input(vk, KEYEVENTF_KEYUP)]
        )
        if index + 1 < count and interval > 0:
            time.sleep(interval)
    return sent


def hotkey(*keys: int) -> int:
    if not keys:
        return 0
    inputs = [_key_input(key) for key in keys]
    inputs.extend(_key_input(key, KEYEVENTF_KEYUP) for key in reversed(keys))
    return win32.send_input(inputs)


def write(text: str, *, interval: float = 0.0, chunk_size: int = 256) -> int:
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
    if interval > 0:
        for index, pair in enumerate(pairs):
            sent += win32.send_input(pair)
            if index + 1 < len(pairs):
                time.sleep(interval)
        return sent

    inputs = [event for pair in pairs for event in pair]
    for index in range(0, len(inputs), chunk_size):
        sent += win32.send_input(inputs[index : index + chunk_size])
    return sent


def release(keys: Iterable[int]) -> int:
    return win32.send_input(_key_input(key, KEYEVENTF_KEYUP) for key in keys)
