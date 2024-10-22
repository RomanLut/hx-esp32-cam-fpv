set(srcs 
    dummy.cpp 
    ../../components/air/main.cpp 
    ../../components/air/file_server.cpp 
    ../../components/air/nvs_args.cpp 
    ../../components/air/queue.cpp 
    ../../components/air/wifi.cpp 
    ../../components/air/osd.cpp 
    ../../components/air/msp.cpp 
    ../../components/air/temperature_sensor.cpp 
)

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS "../../components/esp32-camera/target/private_include/"  
                    EMBED_FILES "../../components/air/favicon.ico" "../../components/air/index.html"
                    REQUIRES esp_timer)

