idf_component_register(
    SRCS "main.c" "debug_stream.c"
    INCLUDE_DIRS "."
    REQUIRES kundt_config kundt_wifi kundt_mqtt kundt_ota kundt_led mic_capture esp_timer
)

# Treat warnings as errors in our own code (not in IDF/managed components).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
