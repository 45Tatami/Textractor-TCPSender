# Textractor TCP-Sender
[Textractor](https://github.com/Artikash/Textractor) extension that sends sentences over TCP.
Useful for setups where the clipboard is not enough.
See [here](https://github.com/45Tatami/native-inserter) for an example of the receiving server side.

Configuration done at runtime via interface.

![Purrint_1707](https://user-images.githubusercontent.com/96940591/149813301-b10d229c-f093-43fa-a483-5848f71e9d2c.png)

## Building

Requires Visual Studio 2019 or meson. Drop into VS, build x86 or x64 depending on which Textractor architecture you use.
Drop resulting dll into Textractor extension window.

### meson

```
meson setup build
ninja -Cbuild
```

Project includes example cross-compilation definition files for a mingw32 toolchain under `cross`.


```
# x86/32bit
meson setup --cross-file=cross/i686-w64-mingw32.txt build

# x86_64/64bit
meson setup --cross-file=cross/x86_64-w64-mingw32.txt build
```

## Mock Application

A simple application for loading the built dll and calling the usual entrypoint
with some sample text can optionally be built and started via meson.

    meson configure build -Dbuild_mock=enabled
    ninja -Cbuild run_mock
    # build/mock/mock_textractor.exe <path to dll>
