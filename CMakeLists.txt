# ESP-IDF only. stm-forge never reads this file: it compiles every .c under
# src/, chip folders included, puts src/ on the include path of this
# library's sources and adds include/ -- which is why each port file is empty
# on the other platform. Here the chip files are listed one by one, and
# PRIV_INCLUDE_DIRS "src" lets a chip folder reach tof_priv.h by plain name.
#
# The core is always built, even with CONFIG_TOF off; each chip driver only
# when its Kconfig option is on. Kconfig values are available here during
# the build phase, and only SRCS depends on them -- never REQUIRES, which IDF
# resolves before sdkconfig. That is also why TOF off still builds the core:
# an empty SRCS would need a second, differently-shaped register call, and an
# unreferenced core costs nothing once --gc-sections has run.
#
# REQUIRES driver, not esp_driver_i2c/uart/gpio: the split components do not
# exist on IDF 5.2, and driver still pulls them in on later versions.
# esp_timer is tof_time_us()'s clock.
set(srcs "src/tof.c" "src/tof_log.c" "src/tof_port_esp.c")

if(CONFIG_TOF AND NOT (CONFIG_TOF_VL53L1X OR CONFIG_TOF_TFLUNA))
    message(WARNING "tof: CONFIG_TOF is on but no chip driver is selected; "
                    "set CONFIG_TOF_<CHIP>=y (esp-forge: TOF_<CHIP> = y)")
endif()

if(CONFIG_TOF_VL53L1X)
    list(APPEND srcs "src/vl53l1x/vl53l1x.c")
endif()
if(CONFIG_TOF_TFLUNA)
    list(APPEND srcs "src/tfluna/tfluna.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES driver esp_timer)
