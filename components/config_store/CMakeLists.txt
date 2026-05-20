idf_component_register(
    SRCS "config_store.c"
    INCLUDE_DIRS "include"
    REQUIRES config
    PRIV_REQUIRES nvs_flash json littlefs
)
