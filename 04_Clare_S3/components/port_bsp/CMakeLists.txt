idf_component_register(
    SRCS 
    "i2c_bsp.cpp" 
    "display_bsp.cpp" 
    "codec_bsp.cpp"
    PRIV_REQUIRES 
    driver
    pmicpower
    espressif__esp_lcd_co5300
    esp_timer
    REQUIRES
    esp_driver_sdmmc
    fatfs
    codec_board
    waveshare__esp_lcd_touch_cst9217
    INCLUDE_DIRS 
    "./" 
    EMBED_FILES "./pcm/canon.pcm")
