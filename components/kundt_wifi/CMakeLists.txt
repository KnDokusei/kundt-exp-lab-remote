idf_component_register(
    SRCS "kundt_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif esp_timer lwip kundt_config
)

# Treat warnings as errors in our own code (not in IDF/managed components).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
