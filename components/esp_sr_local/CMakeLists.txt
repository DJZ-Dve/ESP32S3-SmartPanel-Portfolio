set(ESP_SR_DIR "${CMAKE_CURRENT_LIST_DIR}/../../lib/esp-sr")
set(ESP_SR_TARGET_DIR "${ESP_SR_DIR}/lib/esp32s3")

idf_component_register(
    SRCS
        "${ESP_SR_DIR}/src/model_path.c"
        "${ESP_SR_DIR}/src/esp_sr_debug.c"
        "${ESP_SR_DIR}/src/esp_mn_speech_commands.c"
        "${ESP_SR_DIR}/src/esp_process_sdkconfig.c"
    INCLUDE_DIRS
        "${ESP_SR_DIR}/include/esp32s3"
        "${ESP_SR_DIR}/src/include"
    REQUIRES
        esp_partition
        espressif__cjson
        espressif__dl_fft
        espressif__esp-dsp
        spiffs
    PRIV_REQUIRES
        spi_flash
)

target_link_libraries(${COMPONENT_LIB} "-L \"${ESP_SR_TARGET_DIR}\"")

add_prebuilt_library(dl_lib "${ESP_SR_TARGET_DIR}/libdl_lib.a" PRIV_REQUIRES ${COMPONENT_NAME})
add_prebuilt_library(c_speech_features "${ESP_SR_TARGET_DIR}/libc_speech_features.a" PRIV_REQUIRES ${COMPONENT_NAME})
add_prebuilt_library(esp_audio_processor "${ESP_SR_TARGET_DIR}/libesp_audio_processor.a" PRIV_REQUIRES ${COMPONENT_NAME})
add_prebuilt_library(esp_audio_front_end "${ESP_SR_TARGET_DIR}/libesp_audio_front_end.a" PRIV_REQUIRES ${COMPONENT_NAME})
add_prebuilt_library(fst "${ESP_SR_TARGET_DIR}/libfst.a" PRIV_REQUIRES ${COMPONENT_NAME})
add_prebuilt_library(flite_g2p "${ESP_SR_TARGET_DIR}/libflite_g2p.a" PRIV_REQUIRES ${COMPONENT_NAME})
add_prebuilt_library(multinet "${ESP_SR_TARGET_DIR}/libmultinet.a" PRIV_REQUIRES ${COMPONENT_NAME})
add_prebuilt_library(hufzip "${ESP_SR_TARGET_DIR}/libhufzip.a" PRIV_REQUIRES ${COMPONENT_NAME})
add_prebuilt_library(vadnet "${ESP_SR_TARGET_DIR}/libvadnet.a" PRIV_REQUIRES ${COMPONENT_NAME})
add_prebuilt_library(wakenet "${ESP_SR_TARGET_DIR}/libwakenet.a" PRIV_REQUIRES ${COMPONENT_NAME})
add_prebuilt_library(nsnet "${ESP_SR_TARGET_DIR}/libnsnet.a" PRIV_REQUIRES ${COMPONENT_NAME})

idf_component_get_property(esp_dsp_lib espressif__esp-dsp COMPONENT_LIB)
idf_component_get_property(dl_fft_lib espressif__dl_fft COMPONENT_LIB)
idf_component_get_property(cjson_lib espressif__cjson COMPONENT_LIB)

set(sr_libs
    $<TARGET_FILE:${esp_dsp_lib}>
    $<TARGET_FILE:${dl_fft_lib}>
    $<TARGET_FILE:${cjson_lib}>
    dl_lib
    c_speech_features
    esp_audio_front_end
    esp_audio_processor
    fst
    flite_g2p
    hufzip
    multinet
    nsnet
    vadnet
    wakenet
)

target_link_libraries(${COMPONENT_LIB}
    "-Wl,--start-group"
    ${sr_libs}
    "-Wl,--end-group"
)
