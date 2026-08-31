# Installing litewinwrap on a Windows VM

PyPI is not required. Build a wheel, copy it to the VM, and install that file
directly. The wheel is pure Python and supports Python 3.11 or later; the VM
must be Windows 11 for live desktop automation.

## Build the wheel

From the repository's `python` directory:

```powershell
py -m pip install --upgrade build
py -m unittest discover -s tests -v
py -m build
```

The installable artifact is:

```text
dist/litewinwrap-0.1.0a8-py3-none-any.whl
```

The source archive beside it is useful for redistribution or inspection but is
not needed for a normal VM installation.

## Install on a VM with internet access

Copy the wheel to the VM by shared folder, remote desktop drive sharing, SCP,
or another normal file-transfer mechanism. Then create a dedicated virtual
environment and install it:

```powershell
py -m venv C:\Tools\litewinwrap-env
C:\Tools\litewinwrap-env\Scripts\python -m pip install --upgrade pip
C:\Tools\litewinwrap-env\Scripts\python -m pip install `
    C:\Transfer\litewinwrap-0.1.0a8-py3-none-any.whl
```

`pip` reads the wheel's metadata and downloads its NumPy, OpenCV, and Pillow
dependencies from the configured package index. It does not upload
`litewinwrap` anywhere.

Verify the installed copy:

```powershell
C:\Tools\litewinwrap-env\Scripts\python -c `
    "import litewinwrap; print(litewinwrap.__version__)"
```

The expected output is `0.1.0a8`.

## Install on a VM without internet access

On a Windows machine with the same Python version and CPU architecture as the
VM, download the package and all dependencies into one directory:

```powershell
New-Item -ItemType Directory -Force wheelhouse
py -m pip download --dest wheelhouse `
    .\dist\litewinwrap-0.1.0a8-py3-none-any.whl
```

Copy the complete `wheelhouse` directory to the VM, then install without using
an index:

```powershell
py -m venv C:\Tools\litewinwrap-env
C:\Tools\litewinwrap-env\Scripts\python -m pip install `
    --no-index `
    --find-links C:\Transfer\wheelhouse `
    litewinwrap==0.1.0a8
```

Downloading the dependency bundle on matching Windows and Python versions is
important because NumPy and OpenCV use platform- and interpreter-specific
wheels.

## Install directly from a shared checkout

When the VM can access the repository, a wheel transfer is optional:

```powershell
py -m pip install X:\path\to\goldens\python
```

For active development, use an editable installation instead:

```powershell
py -m pip install -e X:\path\to\goldens\python
```

Editable installation is convenient but couples the VM to that checkout. The
wheel is the more reproducible choice for alpha testing.

## Updating the VM

Give distinct builds distinct versions, for example `0.1.0a8`, rebuild, copy
the new wheel, and run:

```powershell
C:\Tools\litewinwrap-env\Scripts\python -m pip install --upgrade `
    C:\Transfer\litewinwrap-0.1.0a8-py3-none-any.whl
```
