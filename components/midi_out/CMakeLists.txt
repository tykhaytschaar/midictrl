idf_component_register(
    SRCS "midi_out.c"
    INCLUDE_DIRS "include"
    REQUIRES config
    PRIV_REQUIRES esp_driver_uart
)
