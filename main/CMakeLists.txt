# zcode-pet 玩法固件：桌面伴宠镜像 + 帧播放器。
#
# SRCS 只保留入口与 zcp_* 玩法实现；demo_*.c / ui_pixel.c 留在仓库但不参与编译
# （tools/validate.sh --static 的主机测试用显式文件清单单独编译它们，不受影响）。
# 12 段动画帧包由 zcode-pet 仓库的矢量管线离线烘焙（见 scripts/gen_device_assets.sh），
# EMBED_FILES 不展开通配符，必须逐个显式列出——新增 clip 时同步更新此清单与
# build_pet.sh 的资产守卫。

# ZCP-PATCH：LVGL 9.5.0 分块渲染在 LV_OS_NONE 单图元下死锁——tile 图层不在
# disp->layer_head 里，全局 lv_draw_dispatch() 触不到它们，等待循环忙自旋到
# task_wdt 风暴（宠物图片这种大面积重绘必触发）。managed_components 被
# gitignore，干净构建重新解析组件后补丁会丢，所以在配置期幂等重打。
set(ZCP_LV_REFR "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/lvgl__lvgl/src/core/lv_refr.c")
if(EXISTS "${ZCP_LV_REFR}")
    file(READ "${ZCP_LV_REFR}" _zcp_refr_src)
    if(NOT _zcp_refr_src MATCHES "ZCP-PATCH")
        string(REPLACE
            "                lv_draw_dispatch_wait_for_request();\n                lv_draw_dispatch();"
            "                lv_draw_dispatch_layer(disp_refr, tile_layer);\n                lv_draw_dispatch_wait_for_request();"
            _zcp_refr_src "${_zcp_refr_src}")
        file(WRITE "${ZCP_LV_REFR}" "${_zcp_refr_src}")
        message(STATUS "ZCP-PATCH: applied LVGL tile-dispatch deadlock fix to lv_refr.c")
    endif()
    unset(_zcp_refr_src)
endif()

idf_component_register(
    SRCS "main.c"
         "zcp_app.c"
         "zcp_pet.c"
         "zcp_bridge.c"
         "zcp_mirror.c"
         "zcp_zpf.c"
         "zcp_strings.c"
    INCLUDE_DIRS "."
    EMBED_FILES "assets/pet/ball-idle.zpf"
                "assets/pet/ball-drowsy.zpf"
                "assets/pet/ball-deep.zpf"
                "assets/pet/ball-working1.zpf"
                "assets/pet/ball-working2.zpf"
                "assets/pet/ball-working3.zpf"
                "assets/pet/ball-celebrate.zpf"
                "assets/pet/ball-error.zpf"
                "assets/pet/ball-pat.zpf"
                "assets/pet/ball-perk.zpf"
                "assets/pet/ball-aftermath.zpf"
                "assets/pet/ball-wait.zpf"
    REQUIRES bsp bt esp_hw_support esp_timer nvs_flash json
)

# 闭集中文子集字体（assets/fonts/zcp_font_16.c，lv_font_conv --no-compress 生成）。
# 按 docs/development/engineering/lvgl-chinese-fonts.md 的 CMake 集成方式：
# 追加 target_sources，不重复编译、不 #include 生成文件。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/zcp_font_16.c"
)
