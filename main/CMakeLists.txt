idf_component_register(SRCS "blue.c"
                    "proto-c/led_control.pb.c"
                    INCLUDE_DIRS "."
                    "proto-c"
                    REQUIRES bt
                    PRIV_REQUIRES nvs_flash app_update livekit__nanopb)
