# Board profiles

`sdkconfig.defaults.<target>` describes a *chip*. These files describe a
*board*, which is not the same thing and cannot be inferred from it: the
ESP32-C5 appears here twice, once as a Waveshare kit with a WS2812 and a CH343
UART bridge, and once as a XIAO with a plain LED and only native USB.

Layered on top of the target defaults:

```bash
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/xiao-esp32c5.defaults" \
       --preview set-target esp32c5 build
```

ESP-IDF loads `sdkconfig.defaults`, then `sdkconfig.defaults.<target>`
automatically, then the board file, so a board only has to state what differs.

Building without one gives the reference board for that chip: the XIAO
ESP32S3, and the DevKitC-style layout for the C5 and C6.
