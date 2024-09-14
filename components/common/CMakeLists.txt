set(srcs fec_codec.cpp fec.cpp safe_printf.cpp structures.cpp crc.cpp circular_buffer.cpp vcd_profiler.cpp jpeg_parser.cpp avi.cpp util.cpp packets.cpp hx_mavlink_parser.cpp)

idf_component_register(SRCS "${srcs}"
                    INCLUDE_DIRS . 
                    REQUIRES esp_timer )

