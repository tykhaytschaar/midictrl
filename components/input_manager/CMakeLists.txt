idf_component_register(
    SRCS "input_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES state_machine
    PRIV_REQUIRES esp_driver_gpio esp_timer
)
