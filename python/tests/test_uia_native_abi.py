"""Windows-only ABI tests use local COM vtables, without a desktop provider."""

from __future__ import annotations

import ctypes
import sys
import unittest

from litewinwrap import _uia, ElementUnsupportedError


@unittest.skipUnless(sys.platform == "win32", "Requires Windows' stdcall ABI")
class NativeABITests(unittest.TestCase):
    def setUp(self):
        self.callbacks = []
        self.tables = []
        self.objects = []
        test = self

        class Worker:
            oleaut32 = ctypes.WinDLL("oleaut32")

            def call(self, function):
                return function()

            def release(self, pointer):
                test.releases.append(pointer)

        self.releases = []
        self.worker = Worker()
        self.worker.oleaut32.SysStringLen.argtypes = [ctypes.c_void_p]
        self.worker.oleaut32.SysStringLen.restype = ctypes.c_uint32
        self.worker.oleaut32.SysFreeString.argtypes = [ctypes.c_void_p]
        self.worker.oleaut32.SysFreeString.restype = None
        self.worker.oleaut32.SysAllocStringLen.argtypes = [
            ctypes.c_wchar_p,
            ctypes.c_uint32,
        ]
        self.worker.oleaut32.SysAllocStringLen.restype = ctypes.c_void_p

    def com(self, slots):
        table = (ctypes.c_void_p * 44)()
        for slot, (types, callback) in slots.items():
            wrapped = ctypes.WINFUNCTYPE(ctypes.c_int32, ctypes.c_void_p, *types)(
                callback
            )
            self.callbacks.append(wrapped)
            table[slot] = ctypes.cast(wrapped, ctypes.c_void_p).value
        obj = ctypes.pointer(ctypes.c_void_p(ctypes.addressof(table)))
        self.tables.append(table)
        self.objects.append(obj)
        return _uia._Com(self.worker, ctypes.cast(obj, ctypes.c_void_p).value)

    def test_window_handle_and_rectangle_use_correct_native_widths(self):
        handle = (
            0x1234567887654321 if ctypes.sizeof(ctypes.c_void_p) == 8 else 0x12345678
        )

        def hwnd(_this, output):
            output[0] = handle
            return 0

        def rect(_this, output):
            fields = ctypes.cast(output, ctypes.POINTER(ctypes.c_int32))
            fields[0], fields[1], fields[2], fields[3] = -200, -50, 800, 450
            return 0

        pointer = self.com(
            {
                36: ((ctypes.POINTER(ctypes.c_void_p),), hwnd),
                43: ((ctypes.c_void_p,), rect),
            }
        )
        native = _uia.NativeElement(pointer)
        self.assertEqual(native.property("hwnd"), handle)
        self.assertEqual(native.property("rect"), (-200, -50, 800, 450))
        pointer.close()
        self.assertEqual(len(self.releases), 1)

    def test_bstr_uses_explicit_length_and_preserves_embedded_null(self):
        text = "\u00c5\0B"

        def name(_this, output):
            output[0] = self.worker.oleaut32.SysAllocStringLen(text, len(text))
            return 0

        pointer = self.com({23: ((ctypes.POINTER(ctypes.c_void_p),), name)})
        self.assertEqual(_uia.NativeElement(pointer).property("name"), text)
        pointer.close()

    def test_pattern_query_uses_requested_interface_and_classifies_absence(self):
        seen = []

        def pattern(_this, pattern_id, iid, output):
            seen.append((pattern_id, bytes(iid.contents)))
            output[0] = None
            return -2147220992  # UIA_E_NOTSUPPORTED

        pointer = self.com(
            {
                14: (
                    (
                        ctypes.c_int32,
                        ctypes.POINTER(_uia._GUID),
                        ctypes.POINTER(ctypes.c_void_p),
                    ),
                    pattern,
                )
            }
        )
        native = _uia.NativeElement(pointer)
        self.assertIsNone(native._pattern("Invoke", required=False))
        self.assertEqual(
            seen[0], (10000, bytes(_uia._guid("fb377fbe-8ea6-46d5-9c73-6499642d3059")))
        )
        with self.assertRaises(ElementUnsupportedError):
            native._pattern("Invoke")
        pointer.close()


if __name__ == "__main__":
    unittest.main()
