idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES config config_store state_machine web_server wifi_manager esp_timer
)
