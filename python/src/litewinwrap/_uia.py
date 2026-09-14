"""Small native UI Automation COM binding; all COM calls run on one MTA thread."""

from __future__ import annotations

import atexit
import ctypes
import queue
import sys
import threading
import uuid
from concurrent.futures import Future
from typing import Callable, TypeVar


_T = TypeVar("_T")
_PTR = ctypes.c_void_p
_INT = ctypes.c_int32
_HRESULT = ctypes.c_int32
_GUID = ctypes.c_ubyte * 16
_CLSID = "ff48dba4-60ef-4201-aa87-54103eef594e"
_IID = "30cbe57d-d9d0-452a-ab13-7ac5ac4825ee"
_PATTERNS = {
    "Invoke": (10000, "fb377fbe-8ea6-46d5-9c73-6499642d3059"),
    "Value": (10002, "a94cd8b1-0844-4cd6-9d2d-640537ab39e9"),
    "Toggle": (10015, "94cf8058-9b8d-4ab9-8bfd-4cd0a33c8c70"),
}
CONTROL_TYPES = dict(
    enumerate(
        (
            "Button",
            "Calendar",
            "CheckBox",
            "ComboBox",
            "Edit",
            "Hyperlink",
            "Image",
            "ListItem",
            "List",
            "Menu",
            "MenuBar",
            "MenuItem",
            "ProgressBar",
            "RadioButton",
            "ScrollBar",
            "Slider",
            "Spinner",
            "StatusBar",
            "Tab",
            "TabItem",
            "Text",
            "ToolBar",
            "ToolTip",
            "Tree",
            "TreeItem",
            "Custom",
            "Group",
            "Thumb",
            "DataGrid",
            "DataItem",
            "Document",
            "SplitButton",
            "Window",
            "Pane",
            "Header",
            "HeaderItem",
            "Table",
            "TitleBar",
            "Separator",
            "SemanticZoom",
            "AppBar",
        ),
        50000,
    )
)


class UIAutomationError(OSError):
    """A native UI Automation call failed."""

    def __init__(self, operation: str, hresult: int) -> None:
        self.operation = operation
        self.hresult = hresult & 0xFFFFFFFF
        super().__init__(f"{operation} failed (HRESULT 0x{self.hresult:08X})")


class ElementUnavailableError(UIAutomationError):
    """The application destroyed or replaced a retained UI element."""


class ElementUnsupportedError(RuntimeError):
    """The element does not expose the requested UI Automation pattern."""


class ElementReadOnlyError(RuntimeError):
    """A field exposes Value but cannot be edited."""


def _check(status: int, operation: str) -> None:
    if status < 0:
        error = (
            ElementUnavailableError
            if status & 0xFFFFFFFF == 0x80040201
            else UIAutomationError
        )
        raise error(operation, status)


def _guid(value: str) -> _GUID:
    return _GUID.from_buffer_copy(uuid.UUID(value).bytes_le)


def _method(pointer: int, slot: int, result: object, *arguments: object):
    table = ctypes.cast(pointer, ctypes.POINTER(ctypes.POINTER(_PTR))).contents
    return ctypes.WINFUNCTYPE(result, _PTR, *arguments)(table[slot])


class _Com:
    def __init__(self, worker: _Worker, pointer: int) -> None:
        self.worker = worker
        self.pointer = pointer

    def call(self, slot: int, types: tuple, *args, operation: str) -> None:
        _check(
            _method(self.pointer, slot, _HRESULT, *types)(self.pointer, *args),
            operation,
        )

    def integer(self, slot: int, operation: str) -> int:
        value = _INT()
        self.call(
            slot, (ctypes.POINTER(_INT),), ctypes.byref(value), operation=operation
        )
        return value.value

    def string(self, slot: int, operation: str) -> str:
        value = _PTR()
        self.call(
            slot, (ctypes.POINTER(_PTR),), ctypes.byref(value), operation=operation
        )
        try:
            return (
                ctypes.wstring_at(value.value, self.worker.oleaut32.SysStringLen(value))
                if value.value
                else ""
            )
        finally:
            self.worker.oleaut32.SysFreeString(value)

    def output(self, slot: int, types: tuple, *args, operation: str) -> _Com:
        value = _PTR()
        self.call(
            slot,
            (*types, ctypes.POINTER(_PTR)),
            *args,
            ctypes.byref(value),
            operation=operation,
        )
        if not value.value:
            raise ElementUnavailableError(operation, 0x80040201)
        return _Com(self.worker, value.value)

    def close(self) -> None:
        pointer, self.pointer = self.pointer, 0
        if pointer:
            self.worker.release(pointer)

    def __del__(self) -> None:
        self.close()


class _Worker:
    def __init__(self) -> None:
        self.tasks: queue.Queue = queue.Queue()
        self.ready: Future = Future()
        self.closed = False
        self.thread = threading.Thread(
            target=self._run, name="litewinwrap-uia", daemon=True
        )
        self.thread.start()
        self.ready.result()

    def _run(self) -> None:
        initialized = False
        client = None
        try:
            ole32 = ctypes.WinDLL("ole32")
            ole32.CoInitializeEx.argtypes = [_PTR, ctypes.c_uint32]
            ole32.CoInitializeEx.restype = _HRESULT
            ole32.CoUninitialize.argtypes = []
            ole32.CoUninitialize.restype = None
            ole32.CoCreateInstance.argtypes = [
                ctypes.POINTER(_GUID),
                _PTR,
                ctypes.c_uint32,
                ctypes.POINTER(_GUID),
                ctypes.POINTER(_PTR),
            ]
            ole32.CoCreateInstance.restype = _HRESULT
            self.oleaut32 = ctypes.WinDLL("oleaut32")
            self.oleaut32.SysStringLen.argtypes = [_PTR]
            self.oleaut32.SysStringLen.restype = ctypes.c_uint32
            self.oleaut32.SysFreeString.argtypes = [_PTR]
            self.oleaut32.SysFreeString.restype = None
            _check(ole32.CoInitializeEx(None, 0), "CoInitializeEx(MTA)")
            initialized = True
            pointer = _PTR()
            _check(
                ole32.CoCreateInstance(
                    ctypes.byref(_guid(_CLSID)),
                    None,
                    1,
                    ctypes.byref(_guid(_IID)),
                    ctypes.byref(pointer),
                ),
                "CoCreateInstance(UIAutomation)",
            )
            client = self.client = _Com(self, pointer.value)
            self.ready.set_result(None)
            while True:
                task = self.tasks.get()
                if task is None:
                    break
                function, future = task
                try:
                    value = function()
                except BaseException as error:
                    if future is not None:
                        future.set_exception(error)
                else:
                    if future is not None:
                        future.set_result(value)
        except BaseException as error:
            if not self.ready.done():
                self.ready.set_exception(error)
        finally:
            if client is not None:
                client.close()
            if initialized:
                ole32.CoUninitialize()
            self.closed = True

    def call(self, function: Callable[[], _T]) -> _T:
        if self.closed:
            raise RuntimeError("UI Automation worker has stopped")
        if threading.current_thread() is self.thread:
            return function()
        future: Future[_T] = Future()
        self.tasks.put((function, future))
        return future.result()

    def release(self, pointer: int) -> None:
        def release() -> None:
            _method(pointer, 2, ctypes.c_uint32)(pointer)

        if threading.current_thread() is self.thread:
            release()
        elif not self.closed:
            self.tasks.put((release, None))


_worker: _Worker | None = None
_worker_lock = threading.Lock()


def _get_worker() -> _Worker:
    global _worker
    if sys.platform != "win32":
        raise OSError("UI Automation requires Windows")
    with _worker_lock:
        if _worker is None:
            _worker = _Worker()
        return _worker


def _shutdown() -> None:
    if _worker is not None and not _worker.closed:
        _worker.tasks.put(None)
        _worker.thread.join(timeout=1.0)


atexit.register(_shutdown)


class NativeElement:
    def __init__(self, pointer: _Com) -> None:
        self._pointer = pointer

    @property
    def _worker(self) -> _Worker:
        return self._pointer.worker

    def _pattern(self, name: str, *, required: bool = True) -> _Com | None:
        pattern_id, iid = _PATTERNS[name]
        value = _PTR()
        status = _method(
            self._pointer.pointer,
            14,
            _HRESULT,
            _INT,
            ctypes.POINTER(_GUID),
            ctypes.POINTER(_PTR),
        )(
            self._pointer.pointer,
            pattern_id,
            ctypes.byref(_guid(iid)),
            ctypes.byref(value),
        )
        if status & 0xFFFFFFFF in (0x80040200, 0x80004002, 0x80070057) or (
            status >= 0 and not value.value
        ):
            if required:
                raise ElementUnsupportedError(
                    f"Element does not support the {name} pattern"
                )
            return None
        _check(status, f"GetCurrentPatternAs({name})")
        return _Com(self._worker, value.value)

    def property(self, name: str):
        strings = {
            "name": 23,
            "automation_id": 29,
            "class_name": 30,
            "framework_id": 40,
        }
        integers = {
            "process_id": 20,
            "control_type": 21,
            "enabled": 28,
            "is_password": 35,
            "offscreen": 38,
        }

        def read():
            if name in strings:
                return self._pointer.string(strings[name], f"Current{name}")
            if name in integers:
                value = self._pointer.integer(integers[name], f"Current{name}")
                if name == "control_type":
                    return CONTROL_TYPES.get(value, f"ControlType({value})")
                return (
                    bool(value)
                    if name in ("enabled", "offscreen", "is_password")
                    else value
                )
            if name == "hwnd":
                value = _PTR()
                self._pointer.call(
                    36,
                    (ctypes.POINTER(_PTR),),
                    ctypes.byref(value),
                    operation="CurrentNativeWindowHandle",
                )
                return value.value or 0
            if name == "rect":
                # UIA's CurrentBoundingRectangle returns RECT, in physical screen pixels.
                value = (_INT * 4)()
                self._pointer.call(
                    43,
                    (_PTR,),
                    ctypes.byref(value),
                    operation="CurrentBoundingRectangle",
                )
                return tuple(value)
            pattern = self._pattern(
                "Value" if name in ("value", "read_only") else "Toggle"
            )
            try:
                if name == "value":
                    return pattern.string(4, "CurrentValue")
                if name == "read_only":
                    return bool(pattern.integer(5, "CurrentIsReadOnly"))
                return pattern.integer(4, "CurrentToggleState")
            finally:
                pattern.close()

        return self._worker.call(read)

    def info(self) -> dict:
        def read():
            info = {
                name: self.property(name)
                for name in (
                    "name",
                    "automation_id",
                    "control_type",
                    "class_name",
                    "framework_id",
                    "process_id",
                    "hwnd",
                    "enabled",
                    "offscreen",
                    "is_password",
                    "rect",
                )
            }
            supported = []
            for name in _PATTERNS:
                pattern = self._pattern(name, required=False)
                if pattern is not None:
                    supported.append(name)
                    pattern.close()
            info["patterns"] = tuple(supported)
            return info

        return self._worker.call(read)

    def elements(self, recursive: bool) -> tuple[NativeElement, ...]:
        def find():
            condition = self._worker.client.output(
                18, (), operation="ControlViewCondition"
            )
            array = None
            try:
                array = self._pointer.output(
                    6,
                    (_INT, _PTR),
                    4 if recursive else 2,
                    condition.pointer,
                    operation="FindAll",
                )
                return tuple(
                    NativeElement(
                        array.output(4, (_INT,), index, operation="GetElement")
                    )
                    for index in range(array.integer(3, "ElementArray.Length"))
                )
            finally:
                if array is not None:
                    array.close()
                condition.close()

        return self._worker.call(find)

    def action(self, name: str, value: str | None = None) -> None:
        def act():
            pattern = self._pattern(
                "Value"
                if name == "set_value"
                else "Invoke"
                if name == "invoke"
                else "Toggle"
            )
            try:
                if name == "set_value":
                    if pattern.integer(5, "CurrentIsReadOnly"):
                        raise ElementReadOnlyError("Element's value is read-only")
                    pattern.call(3, (ctypes.c_wchar_p,), value, operation="SetValue")
                else:
                    pattern.call(
                        3, (), operation="Invoke" if name == "invoke" else "Toggle"
                    )
            finally:
                pattern.close()

        self._worker.call(act)


def from_handle(hwnd: int) -> NativeElement:
    worker = _get_worker()
    return worker.call(
        lambda: NativeElement(
            worker.client.output(6, (_PTR,), hwnd, operation="ElementFromHandle")
        )
    )
