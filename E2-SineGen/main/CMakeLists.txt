idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES kundt_config kundt_wifi kundt_mqtt kundt_ota kundt_led ad9833 servo
)

# Treat warnings as errors in our own code (not in IDF/managed components).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
