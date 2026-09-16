idf_component_register(
    SRCS "kundt_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_https_ota app_update mqtt json esp_http_client
)
