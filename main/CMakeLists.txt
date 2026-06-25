idf_component_register(SRCS "blue.c"
                    INCLUDE_DIRS "."
                    REQUIRES bt
                    PRIV_REQUIRES nvs_flash)
