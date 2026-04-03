#include <sys/socket.h>
#include <sys/un.h>
#include "stdint.h"
#include <string>
#include <iostream>
#include <memory>
#include <stdarg.h>
#include <netinet/in.h>
#include <thread>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <vector>

#include "Log.h"

using namespace std;

string string_format(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    size_t size = vsnprintf(nullptr, 0, format, args) + 1; // Extra space for '\0'
    va_end(args);
    unique_ptr<char[]> buf(new char[size]);
    va_start(args, format);
    vsnprintf(buf.get(), size, format, args);
    va_end(args);
    return string(buf.get(), buf.get() + size - 1); // We don't want the '\0' inside
}

int udp_socket_init(const std::string &client_addr, int client_port)
{
    struct sockaddr_in saddr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) throw std::runtime_error(string_format("Error opening socket: %s", strerror(errno)));

    bzero((char *) &saddr, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = inet_addr(client_addr.c_str());
    saddr.sin_port = htons((unsigned short)client_port);

    if (connect(fd, (struct sockaddr *) &saddr, sizeof(saddr)) < 0)
    {
        throw std::runtime_error(string_format("Connect error: %s", strerror(errno)));
    }

    LOGI("UDP server start! fd:{}",fd);
    return fd;
}

void send_data_to_udp(int socketfd,uint8_t * buf, int len){
    while(len>1024){
        send(socketfd, buf, 1024, MSG_DONTWAIT);
        buf+=1024;
        len-=1024;
    }

    if(send(socketfd,buf,len,MSG_DONTWAIT)<0){
        LOGE("error when sending!");
    }

}

// int socket_init(){
//         struct sockaddr_un saddr;
//         struct sockaddr_un saddr_remote;
//         int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
//         if (fd < 0) throw std::runtime_error(string_format("Error opening socket: %s", strerror(errno)));

//         bzero((char *) &saddr, sizeof(saddr));
//         saddr.sun_family = AF_UNIX;
//         strcpy(saddr.sun_path, "/home/ncer/testsocket");

//         int len = strlen(saddr.sun_path) + sizeof(saddr.sun_family);
//         if( bind(fd, (struct sockaddr*)&saddr, len)  != 0)
//         {
//             throw std::runtime_error(string_format("Connect error: %s", strerror(errno)));
//         }

//         if( listen(fd, 5) != 0 )
//         {
//             throw std::runtime_error(string_format("Connect error: %s", strerror(errno)));
//         }


//         uint32_t temp=0;
//         int sockfd;
//         if( (sockfd = accept(fd, (struct sockaddr*)&saddr_remote, &temp)) != -1 ){
//             printf("connected!\n");
//         }
//         return sockfd;
// }
