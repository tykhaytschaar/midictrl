idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board config config_store input_manager midi_out state_machine web_server wifi_manager esp_timer
)
