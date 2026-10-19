# zcode-pet 玩法固件：桌面伴宠镜像 + 帧播放器。
#
# SRCS 只保留入口与 zcp_* 玩法实现；demo_*.c / ui_pixel.c 留在仓库但不参与编译
# （tools/validate.sh --static 的主机测试用显式文件清单单独编译它们，不受影响）。
# 12 段动画帧包由 zcode-pet 仓库的矢量管线离线烘焙（见 scripts/gen_device_assets.sh），
# EMBED_FILES 不展开通配符，必须逐个显式列出——新增 clip 时同步更新此清单与
# build_pet.sh 的资产守卫。
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
