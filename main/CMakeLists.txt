idf_component_register(SRCS main.c
    img.c
    SI523_App.c 
    mmc56x3.c 
    led_strip_encoder.c
    esp_hidd_prf_api.c
    hid_dev.c
    hid_device_le_prf.c
    lowpw.c
 INCLUDE_DIRS "."
 
 )

set(COMPONENT_REQUIRES esp_adc tinyusb bt)

target_compile_options(${COMPONENT_LIB} PRIVATE 
    -Wno-unused-const-variable
)

