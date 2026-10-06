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

## Deployment settings

Network OTA is disabled in this public build. The original OTA implementation remains in the source behind `EPHOTO_ENABLE_OTA` for private builds. The default access point uses the original name format (`E-Photo-XXXXXX`) and password `12345678`.

The optional embedded hosted co-processor images are omitted. A private build can provide a reviewed C5 or C6 image under `components/services/assets/` and enable `EPHOTO_ENABLE_EMBEDDED_HOSTED_FW=ON`; the main firmware remains buildable without them.

The original boot image and QR asset are included in this copy.
