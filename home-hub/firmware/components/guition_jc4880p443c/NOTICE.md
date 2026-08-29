# Attribution and source baseline

This component is a focused derivative of Espressif's Apache-2.0 licensed
`esp32_p4_function_ev_board` BSP, as modified and distributed by GUITION for
the `JC4880P443C_I_W`.

The implementation baseline is GUITION's `JC4880P443C_I_W.zip` download,
SHA-256 `7CEA2154667033A639B62A42D1952066CA55C78E187846351F5FACB0C3F5232F`.
Its board component identifies Espressif BSP commit
`4b3f542031dfe2a0b955517f804a99734f2cf8bb`, version 5.2.3. Only the display,
backlight and touch paths needed by BluePaws are represented here.

Original project: https://github.com/espressif/esp-bsp

GUITION package repository: https://github.com/guitionofficial/P4-series

The external ST7701, GT911, LVGL and LVGL port components are resolved by the
ESP-IDF Component Manager and retain their respective upstream licences.
