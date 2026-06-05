#
# ThumbyOne common code.
#
# Exports (as PARENT_SCOPE variables — consumers add to their own
# add_executable/target_sources rather than linking a library here;
# see the big comment block below for why):
#
#   THUMBYONE_COMMON_SRC      — handoff API + QMI/ATRANS setup.
#                                Every slot and the lobby needs this.
#   THUMBYONE_COMMON_INCLUDE  — include path for the handoff header
#                                and slot_layout.h.
#   THUMBYONE_COMMON_FS_SRC   — shared flash-disk + FatFs glue +
#                                lobby-owned mount/format API.
#                                For targets that mount the shared
#                                FAT themselves (lobby, NES, P8).
#                                NOT for DOOM (doesn't touch FAT)
#                                or MPY (has its own diskio through
#                                the MicroPython bdev layer).
#   THUMBYONE_FATFS_SRC       — upstream FatFs R0.15 sources
#                                (ff.c + ffunicode.c + ffsystem.c).
#                                Paired with THUMBYONE_COMMON_FS_SRC.
#   THUMBYONE_FATFS_INCLUDE   — include path for ff.h / diskio.h /
#                                ffconf.h.
#   THUMBYONE_PICKER_SRC      — LCD + font + picker UI, shared between
#                                the lobby and the MPY-slot C picker.
#                                LCD is the GC9107 via SPI+DMA; font
#                                is a bitmapped 6x8 with 2x variant.
#   THUMBYONE_PICKER_INCLUDE  — include path for lcd_gc9107.h + font.h.
#   THUMBYONE_PT_JSON         — canonical partition-table JSON path.
#
# No CMake library wrapping: every target already links pico_stdlib
# directly, and wrapping this code in a STATIC/OBJECT library either
# absorbs pico_stdlib's transitive sources or needs extra propagation
# plumbing. Having every consumer compile the handoff source itself
# is the simple, dedup-free answer.
#

# Common handoff — QMI/ATRANS restore, scratch-based slot handoff.
set(THUMBYONE_COMMON_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/thumbyone_handoff.c
    PARENT_SCOPE)

set(THUMBYONE_COMMON_INCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}
    PARENT_SCOPE)

# Shared-FAT flash-disk + FatFs glue + lobby-owned mount/format.
# Any target that adds this MUST also pick up THUMBYONE_FATFS_SRC
# (ff.c etc.) and add THUMBYONE_FATFS_INCLUDE to its include path.
set(THUMBYONE_COMMON_FS_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/fs/thumbyone_disk.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fs/thumbyone_diskio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/fs/thumbyone_fs.c
    PARENT_SCOPE)

# Upstream FatFs R0.15 — symlinked from mp-thumby/lib/fatfs so
# there's one authoritative copy that the MPY port also compiles.
set(THUMBYONE_FATFS_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/fatfs/ff.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/fatfs/ffunicode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/fatfs/ffsystem.c
    PARENT_SCOPE)

set(THUMBYONE_FATFS_INCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/fatfs
    ${CMAKE_CURRENT_SOURCE_DIR}/fs
    PARENT_SCOPE)

# LCD (GC9107) + bitmap font. Shared between the ThumbyOne lobby
# and the MPY slot's C picker (both need a selector UI before any
# Python runtime exists). Consumers also need hardware_spi +
# hardware_dma + hardware_gpio linked in, same as the lobby has.
set(THUMBYONE_PICKER_UI_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/picker/lcd_gc9107.c
    ${CMAKE_CURRENT_SOURCE_DIR}/picker/font.c
    PARENT_SCOPE)

# The MPY-slot picker itself — adds picker.c on top of the UI bits.
# MPY slot wants THUMBYONE_PICKER_SRC, which is PICKER_UI_SRC + picker.c.
# The lobby uses PICKER_UI_SRC only (draws its own home screen).
set(THUMBYONE_PICKER_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/picker/lcd_gc9107.c
    ${CMAKE_CURRENT_SOURCE_DIR}/picker/font.c
    ${CMAKE_CURRENT_SOURCE_DIR}/picker/picker_bmp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/picker/menu_watchdog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/picker/picker.c
    PARENT_SCOPE)

set(THUMBYONE_PICKER_INCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}/picker
    PARENT_SCOPE)

# Generate the partition-table JSON from the same slot-enable flags
# that drive common/slot_layout.h.  This is the single source of
# truth — the previously-committed pt.json / pt_with_md.json files
# are no longer used (they were hand-edited and inevitably drifted
# out of sync with slot_layout.h every time we added a slot).
#
# tools/gen_pt.py reads THUMBYONE_WITH_* from its CLI flags; CMake
# passes whichever flags are set.  Output lands in the build dir
# and is what pico_embed_pt_in_binary picks up.
set(THUMBYONE_PT_JSON    ${CMAKE_BINARY_DIR}/pt_generated.json)
set(THUMBYONE_PT_CMAKE   ${CMAKE_BINARY_DIR}/pt_generated.cmake)
set(GEN_PT_TOOL          ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_pt.py)

set(GEN_PT_FLAGS "")
if (THUMBYONE_WITH_MD)
    list(APPEND GEN_PT_FLAGS --with-md)
endif()
if (NOT THUMBYONE_WITH_NES)
    list(APPEND GEN_PT_FLAGS --no-nes)
endif()
if (NOT THUMBYONE_WITH_P8)
    list(APPEND GEN_PT_FLAGS --no-p8)
endif()
if (NOT THUMBYONE_WITH_DOOM)
    list(APPEND GEN_PT_FLAGS --no-doom)
endif()
if (NOT THUMBYONE_WITH_MPY)
    list(APPEND GEN_PT_FLAGS --no-mpy)
endif()
if (NOT THUMBYONE_WITH_SCUMM)
    list(APPEND GEN_PT_FLAGS --no-scumm)
endif()
if (NOT THUMBYONE_WITH_CRAFT)
    list(APPEND GEN_PT_FLAGS --no-craft)
endif()
if (NOT THUMBYONE_WITH_ROGUE)
    list(APPEND GEN_PT_FLAGS --no-rogue)
endif()
if (NOT THUMBYONE_WITH_ELITE)
    list(APPEND GEN_PT_FLAGS --no-elite)
endif()

add_custom_command(
    OUTPUT  ${THUMBYONE_PT_JSON} ${THUMBYONE_PT_CMAKE}
    COMMAND python3 ${GEN_PT_TOOL} ${GEN_PT_FLAGS}
            -o ${THUMBYONE_PT_JSON}
            --cmake-out ${THUMBYONE_PT_CMAKE}
    DEPENDS ${GEN_PT_TOOL}
    COMMENT "Generating partition table"
    VERBATIM
)
add_custom_target(thumbyone_pt_json DEPENDS ${THUMBYONE_PT_JSON})

# Run the generator immediately at configure time too, so the CMake
# mirror file exists for include() below — add_custom_command alone
# fires at build time, which is too late for the parent CMakeLists.
execute_process(
    COMMAND python3 ${GEN_PT_TOOL} ${GEN_PT_FLAGS}
            -o ${THUMBYONE_PT_JSON}
            --cmake-out ${THUMBYONE_PT_CMAKE}
)

# Export to parent for lobby's pico_embed_pt_in_binary call.
set(THUMBYONE_PT_JSON  ${THUMBYONE_PT_JSON}  PARENT_SCOPE)
set(THUMBYONE_PT_CMAKE ${THUMBYONE_PT_CMAKE} PARENT_SCOPE)

# Shared battery helper — ADC sampling + EMA smoothing + percent /
# charging hysteresis. Consumed by every slot (NES, P8, DOOM, MPY
# picker) and the lobby so "35% 3.42V" means the same thing
# everywhere. Pure pico-sdk usage (adc + stdlib) — no FatFs, no
# display dependency.
set(THUMBYONE_COMMON_BATTERY_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/thumbyone_battery.c
    PARENT_SCOPE)

set(THUMBYONE_COMMON_BATTERY_INCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    PARENT_SCOPE)

# Shared shared-FAT disk-usage helper — f_getfree wrapper + compact
# "X.XM/Y.YM" formatter. Consumed by anything that shows disk usage
# (lobby, NES picker menu, P8 picker menu, MPY picker menu). DOOM
# doesn't use it — it has no FatFs. Needs THUMBYONE_FATFS_INCLUDE
# already on the consumer's include path (for ff.h).
set(THUMBYONE_COMMON_FS_STATS_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/thumbyone_fs_stats.c
    PARENT_SCOPE)

set(THUMBYONE_COMMON_FS_STATS_INCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    PARENT_SCOPE)

# Shared hardware-PWM backlight driver — GP7 slice 3 channel B,
# 0..255 with a floor constant so the screen never goes invisible.
# Consumed by the lobby, the common picker LCD (MPY slot), and
# NES / P8 LCD drivers under THUMBYONE_SLOT_MODE. NOT used by
# DOOM — it drives GP7 as a plain GPIO "on" and has no
# brightness slider.
set(THUMBYONE_COMMON_BACKLIGHT_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/thumbyone_backlight.c
    PARENT_SCOPE)

set(THUMBYONE_COMMON_BACKLIGHT_INCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    PARENT_SCOPE)

# Shared front-RGB-indicator driver — GP10/11/12, brightness-scaled
# from thumbyone_backlight_get() with a ~63 % peak-duty cap so the
# LED dims with the screen instead of blinding the user. Consumed
# by the lobby (USB indicator) and the common picker (live LED
# repaint on slider drag). Depends on thumbyone_backlight.
set(THUMBYONE_COMMON_LED_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/thumbyone_led.c
    PARENT_SCOPE)

set(THUMBYONE_COMMON_LED_INCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    PARENT_SCOPE)

# Shared system-wide settings (volume / brightness) — tiny helper
# that loads + saves /.volume and /.brightness on the shared FAT.
# Consumed by every slot + lobby; DOOM does NOT use this (no FatFs).
# Needs FatFs on the consumer's include path (THUMBYONE_FATFS_INCLUDE).
set(THUMBYONE_COMMON_SETTINGS_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/thumbyone_settings.c
    PARENT_SCOPE)

set(THUMBYONE_COMMON_SETTINGS_INCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    PARENT_SCOPE)

# Shared BM8563 RTC driver — vendored from the engine's lib/bm8563
# (Tuupola's hardware-agnostic library) plus a thin i2c0 wrapper.
# Lobby uses it for the SET TIME submenu and corner clock; ThumbyNES
# slot uses it to feed peanut-GB's gb_set_rtc / gb_tick_rtc for
# Pokemon Crystal/Gold/Silver day-night and berry growth. Pulls
# hardware_i2c via pico-sdk (every consumer already links pico_stdlib).
set(THUMBYONE_COMMON_RTC_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/thumbyone_rtc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/bm8563/bm8563.c
    PARENT_SCOPE)

set(THUMBYONE_COMMON_RTC_INCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    PARENT_SCOPE)
