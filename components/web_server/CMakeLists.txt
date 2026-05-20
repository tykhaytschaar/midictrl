idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "web/index.html"
    REQUIRES config config_store state_machine wifi_manager
    PRIV_REQUIRES esp_http_server json esp_timer
)
