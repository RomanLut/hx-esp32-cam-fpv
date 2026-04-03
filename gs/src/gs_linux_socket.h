#pragma once
#include <stdint.h>
#include <string>

void send_data_to_udp(int socketfd,uint8_t * buf, int len);
int udp_socket_init(const std::string &client_addr, int client_port);
