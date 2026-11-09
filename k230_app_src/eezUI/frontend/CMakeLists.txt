# CMakeLists.txt — 本机 (Ubuntu/PC) LVGL 模拟器
#
# 干什么: 把 EEZ 生成的 ui/*.c + 我们的 src/*.c 和 LVGL 9.6.0 (SDL2 后端)
#         编成一个桌面窗口程序 k230ui-sim, 不用板子就能看主界面。
# 怎么用:
#   bash scripts/fetch-lvgl.sh      # 拉 LVGL 9.6.0 (只需一次)
#   bash scripts/build-sim.sh       # 等价于 cmake -S . -B build-sim && cmake --build
#   bash scripts/run-sim.sh         # 跑起来
# 注意: 板端不用本文件 (板上用 SDK 的 liblvgl.so + 交叉编译, 见 tools/board-build.sh)
cmake_minimum_required(VERSION 3.16)
project(k230ui_sim C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lvgl/CMakeLists.txt")
    message(FATAL_ERROR "third_party/lvgl 不存在 —— 先跑: bash scripts/fetch-lvgl.sh")
endif()

# ---- LVGL 配置: 用本目录的 lv_conf.h (模拟器专用, 见文件头说明) ----
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h")

# 只编库本体, 不要 demo/example, 不要 ThorVG (省时间)
set(CONFIG_LV_BUILD_DEMOS OFF)
set(CONFIG_LV_BUILD_EXAMPLES OFF)
set(CONFIG_LV_USE_THORVG OFF)
# 告诉 LVGL 的 CMake 我们开了 SDL (它据此 find_package / pkg-config SDL2 并链接)
set(CONFIG_LV_USE_SDL ON)
# 不要联网抓依赖
set(LV_FETCH_DEPENDENCIES OFF)

add_subdirectory(third_party/lvgl EXCLUDE_FROM_ALL)

# ---- 我们的程序 ----
add_executable(k230ui-sim
    src/main_sdl.c
    src/ui_actions.c
    src/ui_pages.c
    src/backend_client.c
    src/font_cjk_20.c
    src/font_cjk_28.c
    ui/screens.c
    ui/styles.c
    ui/images.c
    ui/ui.c
)

target_include_directories(k230ui-sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
)

# LVGL 的 PUBLIC include 已经把 lvgl 头目录带出来了
target_link_libraries(k230ui-sim PRIVATE lvgl m pthread)

if(NOT MSVC)
    target_compile_options(k230ui-sim PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# 板端交叉编译时不许走这个 target
install(TARGETS k230ui-sim RUNTIME DESTINATION bin OPTIONAL)
