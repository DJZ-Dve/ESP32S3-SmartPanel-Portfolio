idf_component_register(
    SRCS
        "App_FlashGuard.cpp"
        "idf_bootstrap.cpp"
        "idf/App_IdfAdc.cpp"
        "idf/App_IdfAppMode.cpp"
        "idf/App_IdfAudio.cpp"
        "idf/App_IdfBleAircon.cpp"
        "idf/App_IdfBlePresetScenes.cpp"
        "idf/App_IdfCellular.cpp"
        "idf/App_IdfCommandExecutor.cpp"
        "idf/App_IdfConsole.cpp"
        "idf/App_IdfDisplay.cpp"
        "idf/App_IdfFilesystem.cpp"
        "idf/App_IdfInput.cpp"
        "idf/App_IdfIr.cpp"
        "idf/App_IdfLearnFlow.cpp"
        "idf/App_IdfLog.cpp"
        "idf/App_IdfLvgl.cpp"
        "idf/App_IdfNetwork.cpp"
        "idf/App_IdfMqtt.cpp"
        "idf/App_IdfOta.cpp"
        "idf/App_IdfPowerSave.cpp"
        "idf/App_IdfRecorder.cpp"
        "idf/App_IdfRf433.cpp"
        "idf/App_IdfScene.cpp"
        "idf/App_IdfServer.cpp"
        "idf/App_IdfSensors.cpp"
        "idf/App_IdfSystem.cpp"
        "idf/App_IdfTasks.cpp"
        "idf/App_IdfTime.cpp"
        "idf/App_IdfTransport.cpp"
        "idf/App_IdfUi.cpp"
        "idf/App_IdfVadNet.cpp"
        "idf/App_IdfWakeWord.cpp"
        "ui.c"
        "ui_AIScreen.c"
        "ui_MainScreen.c"
        "ui_theme.c"
        "my_font_misans_20.c"
        "ui_img_fengnuan_off.c"
        "ui_img_fengnuan_on.c"
        "ui_img_icon_ai.c"
        "ui_img_icon_bind.c"
        "ui_img_icon_info.c"
        "ui_img_icon_lang.c"
        "ui_img_icon_network.c"
        "ui_img_icon_settings.c"
        "ui_img_icon_volume.c"
        "ui_img_kongtiao_off.c"
        "ui_img_kongtiao_on.c"
        "ui_img_shuinuan_off.c"
        "ui_img_shuinuan_on.c"
    INCLUDE_DIRS
        "../include"
    REQUIRES
        app_update
        bt
        esp_adc
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_rmt
        esp_driver_spi
        esp_driver_uart
        esp_driver_usb_serial_jtag
        esp_http_server
        esp_lcd
        esp_http_client
        esp_event
        esp_netif
        esp_wifi
        esp_app_format
        esp_partition
        esp_system
        esp_sr_local
        freertos
        heap
        joltwallet__littlefs
        json
        log
        lvgl
        lwip
        mqtt
        mbedtls
        nvs_flash
        spi_flash
        vfs
)

littlefs_create_partition_image(spiffs ../data FLASH_IN_PROJECT)

set(SR_MODELS_IMAGE "${CMAKE_SOURCE_DIR}/board_models/srmodels.bin")
if(EXISTS "${SR_MODELS_IMAGE}")
    partition_table_get_partition_info(MODEL_OFFSET "--partition-name model" "offset")
    if("${MODEL_OFFSET}")
        idf_component_get_property(ESPTOOL_MAIN_ARGS esptool_py FLASH_ARGS)
        idf_component_get_property(ESPTOOL_SUB_ARGS esptool_py FLASH_SUB_ARGS)
        esptool_py_flash_target(model-flash "${ESPTOOL_MAIN_ARGS}" "${ESPTOOL_SUB_ARGS}")
        esptool_py_flash_target_image(model-flash model "${MODEL_OFFSET}" "${SR_MODELS_IMAGE}")
        esptool_py_flash_target_image(flash model "${MODEL_OFFSET}" "${SR_MODELS_IMAGE}")
    endif()
else()
    message(WARNING "board_models/srmodels.bin not found; model partition will not be included in idf.py flash")
endif()
