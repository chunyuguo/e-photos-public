# E-Photo

ESP32-P4 electronic photo frame firmware source distribution.

This public copy contains application source, the web interface, build configuration, dependency lock data, the two required fonts, the boot image, and the QR image.

## Build

Use ESP-IDF 5.5.4 or a compatible release and set `IDF_PATH` to your local installation:

```bash
export IDF_PATH=/path/to/esp-idf
source "$IDF_PATH/export.sh"
./build.sh c5_v1.3 build
```

The other supported profiles are `c6_v1.3`, `c5_v3.2`, and `c6_v3.2`. Generated files are written to ignored `build_*` directories.

