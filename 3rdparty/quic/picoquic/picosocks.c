/*
* Author: Christian Huitema
* Copyright (c) 2017, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <sys/stat.h>
#include "picosocks.h"
#include "util.h"


static int bind_to_port(SOCKET_TYPE fd, int af, int port)
{
    struct sockaddr_storage sa;
    int addr_length = 0;

    memset(&sa, 0, sizeof(sa));

    if (af == AF_INET) {
        struct sockaddr_in* s4 = (struct sockaddr_in*)&sa;
#ifdef _WINDOWS
        s4->sin_family = (ADDRESS_FAMILY)af;
#else
        s4->sin_family = af;
#endif
        s4->sin_port = htons((unsigned short)port);
        addr_length = sizeof(struct sockaddr_in);
    } else {
        struct sockaddr_in6* s6 = (struct sockaddr_in6*)&sa;

        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons((unsigned short)port);
        addr_length = sizeof(struct sockaddr_in6);
    }

    return bind(fd, (struct sockaddr*)&sa, addr_length);
}

int picoquic_open_server_sockets(picoquic_server_sockets_t* sockets, int port)
{
    int ret = 0;
#ifndef NS3
    const int sock_af[] = { AF_INET6, AF_INET }; //这里应该是ipv6和ipv4都设置了一遍socket，
#else
    const int sock_af[] = { AF_INET };
#endif

    for (int i = 0; i < PICOQUIC_NB_SERVER_SOCKETS; i++) {
        if (ret == 0) {
            sockets->s_socket[i] = socket(sock_af[i], SOCK_DGRAM, IPPROTO_UDP);
        } else {
            sockets->s_socket[i] = INVALID_SOCKET;
        }

        if (sockets->s_socket[i] == INVALID_SOCKET) {
            ret = -1;
        }
        else {
#ifdef _WINDOWS
            int option_value = 1;
            if (sock_af[i] == AF_INET6) {
                ret = setsockopt(sockets->s_socket[i], IPPROTO_IPV6, IPV6_PKTINFO, (char*)&option_value, sizeof(int));
            }
            else {
                ret = setsockopt(sockets->s_socket[i], IPPROTO_IP, IP_PKTINFO, (char*)&option_value, sizeof(int));
            }
#else
            if (sock_af[i] == AF_INET6) {
                int val = 1;
                ret = setsockopt(sockets->s_socket[i], IPPROTO_IPV6, IPV6_V6ONLY,
                    &val, sizeof(val)); //表示只使用ipv6，禁止v4兼容
                if (ret == 0) {
                    val = 1;
                    ret = setsockopt(sockets->s_socket[i], IPPROTO_IPV6, IPV6_RECVPKTINFO, (char*)&val, sizeof(int)); //表示接受分组信息，这里的recv是接受不是恢复
                }
                if (ret == 0) {
                    val = 1;
                    //这里调用多次setsockopt函数，实际上，由于fd描述符没变化，相当于是对这个scoket进行多次选项设置
                    ret = setsockopt(sockets->s_socket[i], IPPROTO_IPV6, IPV6_DONTFRAG, &val, sizeof(val));//表示丢弃大的分组而非将其分片
                }
            }
            else {
                int val = 1;
#ifdef IP_PKTINFO
                ret = setsockopt(sockets->s_socket[i], IPPROTO_IP, IP_PKTINFO, (char*)&val, sizeof(int));//表示返回分组信息
#else
                /* The IP_PKTINFO structure is not defined on BSD */
                ret = setsockopt(sockets->s_socket[i], IPPROTO_IP, IP_RECVDSTADDR, (char*)&val, sizeof(int));
#endif
            }
#endif
            if (ret == 0) {
                ret = bind_to_port(sockets->s_socket[i], sock_af[i], port);//绑定端口
            }
        }
    }

    return ret;
}

void picoquic_close_server_sockets(picoquic_server_sockets_t* sockets)
{
    for (int i = 0; i < PICOQUIC_NB_SERVER_SOCKETS; i++) {
        if (sockets->s_socket[i] != INVALID_SOCKET) {
            SOCKET_CLOSE(sockets->s_socket[i]);
            sockets->s_socket[i] = INVALID_SOCKET;
        }
    }
}

void picoquic_close_one_server_sockets(int* sockets)
{
    for (int i = 0; i < PICOQUIC_NB_SERVER_SOCKETS; i++) {
        if (*sockets != INVALID_SOCKET) {
            SOCKET_CLOSE(*sockets);
            *sockets = INVALID_SOCKET;
        }
    }
}

int picoquic_select(SOCKET_TYPE* sockets, 
    int nb_sockets, 
    struct sockaddr_storage* addr_from, 
    socklen_t* from_length, 
    struct sockaddr_storage* addr_dest, 
    socklen_t* dest_length, 
    unsigned long* dest_if, 
    uint8_t* buffer, int buffer_max, 
    int64_t delta_t, 
    uint64_t* current_time, 
    picoquic_quic_t* quic)
{
    fd_set readfds;
    struct timeval tv;
    int ret_select = 0;
    int bytes_recv = 0;
    int sockmax = 0;

    FD_ZERO(&readfds);

    for (int i = 0; i < nb_sockets; i++) {
        if (sockmax < (int)sockets[i]) {
            sockmax = (int)sockets[i];
        }
        FD_SET(sockets[i], &readfds);
    }

    if (delta_t <= 0) {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
    }
    else {
        if (delta_t > 10000000) {
            tv.tv_sec = (long)10;
            tv.tv_usec = 0;
        }
        else {
            tv.tv_sec = (long)(delta_t / 1000000);
            tv.tv_usec = (long)(delta_t % 1000000);
        }
    }

select_retry:
    ret_select = select(sockmax + 1, &readfds, NULL, NULL, &tv);

    if (ret_select < 0) {
        bytes_recv = -1;
        if (bytes_recv <= 0) {
            DBG_PRINTF("Error: select returns %d, error: %s\n", ret_select, strerror(errno));
            if (errno == EINTR) {
                bytes_recv = 0;
                goto select_retry;
            }
        }
    }
    else if (ret_select > 0) {
        for (int i = 0; i < nb_sockets; i++) {
            if (FD_ISSET(sockets[i], &readfds)) {
                struct stat statbuf;
                fstat(sockets[i], &statbuf);
                if (S_ISSOCK(statbuf.st_mode)) {
                    bytes_recv = picoquic_recvmsg(sockets[i], addr_from, from_length,
                        addr_dest, dest_length, dest_if,
                        buffer, buffer_max, &quic->rcv_tos);
                }
                else {
                    bytes_recv = (int)read(sockets[i], buffer, (size_t)buffer_max);//将socket中的数据读到buf中，这和fpt中的Nread一样，buffer是数组名也就是指针，所以改变的是实参
                }
                // bytes_recv = recvfrom(socket[i], buffer, buffer_max, 0, addr_from, from_length);

                if (bytes_recv <= 0) {
#ifdef _WINDOWS
                    int last_error = WSAGetLastError();

                    if (last_error == WSAECONNRESET || last_error == WSAEMSGSIZE) {
                        bytes_recv = 0;
                        continue;
                    }
#endif
                    DBG_PRINTF("Could not receive packet on UDP socket[%d]= %d!\n",
                        i, (int)sockets[i]);
                    break;
                }
                else {
                    if (quic) {
                        quic->rcv_socket = sockets[i];
                    }
                    break;
                }
            }
        }
    }

exit:
    *current_time = picoquic_current_time();

    return bytes_recv;
}

int picoquic_select_for_one_socket(int* quic_socket, 
    struct sockaddr_storage* addr_from, 
    socklen_t* from_length, 
    struct sockaddr_storage* addr_dest, 
    socklen_t* dest_length, 
    unsigned long* dest_if, 
    uint8_t* buffer, int buffer_max, 
    int64_t delta_t, 
    uint64_t* current_time, 
    picoquic_quic_t* quic)
{
    /*
    fd_set readfds;
    struct timeval tv;
    int ret_select = 0;
    int bytes_recv = 0;
    int sockmax = 0;

    FD_ZERO(&readfds);  

    for (int i = 0; i < nb_sockets; i++) {
        if (sockmax < (int)sockets[i]) {
            sockmax = (int)sockets[i];
        }
        FD_SET(sockets[i], &readfds);
    }

    if (delta_t <= 0) {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
    }
    else {
        if (delta_t > 10000000) {
            tv.tv_sec = (long)10;
            tv.tv_usec = 0;
        }
        else {
            tv.tv_sec = (long)(delta_t / 1000000);
            tv.tv_usec = (long)(delta_t % 1000000);
        }
    }

select_retry:
    ret_select = select(sockmax + 1, &readfds, NULL, NULL, &tv);

    if (ret_select < 0) {
        bytes_recv = -1;
        if (bytes_recv <= 0) {
            DBG_PRINTF("Error: select returns %d, error: %s\n", ret_select, strerror(errno));
            if (errno == EINTR) {
                bytes_recv = 0;
                goto select_retry;
            }
        }
    }
    else if (ret_select > 0) {
        if (FD_ISSET(*quic_socket, &readfds)) {
            struct stat statbuf;
            fstat(*quic_socket, &statbuf);
            if (S_ISSOCK(statbuf.st_mode)) {
                bytes_recv = picoquic_recvmsg(*quic_socket, addr_from, from_length,
                    addr_dest, dest_length, dest_if,
                    buffer, buffer_max, &quic->rcv_tos);
            }
            else {
                bytes_recv = (int)read(*quic_socket, buffer, (size_t)buffer_max);//将socket中的数据读到buf中，这和fpt中的Nread一样，buffer是数组名也就是指针，所以改变的是实参
            }
            // bytes_recv = recvfrom(socket[i], buffer, buffer_max, 0, addr_from, from_length);

            if (bytes_recv <= 0) {
                DBG_PRINTF("Could not receive packet on UDP socket = %d!\n",
                    (int)*quic_socket);
            }
            else {
                if (quic) {
                    quic->rcv_socket = *quic_socket;
                }
            }
        }
    }

exit:
    *current_time = picoquic_current_time();*/

    struct pollfd pfd;
    pfd.fd = *quic_socket;
    pfd.events = POLLIN;

    int ret_poll = poll(&pfd, 1, delta_t / 1000); // Convert delta_t from microseconds to milliseconds

    int bytes_recv = 0;

    if (ret_poll > 0) {
        if (pfd.revents & POLLIN) {
            // The socket is ready for reading
            bytes_recv = picoquic_recvmsg(*quic_socket, addr_from, from_length,
                addr_dest, dest_length, dest_if,
                buffer, buffer_max, &quic->rcv_tos);
        }
    }
    else if (ret_poll < 0) {
        DBG_PRINTF("Error: poll returns %d, error: %s\n", ret_poll, strerror(errno));
    }

    *current_time = picoquic_current_time();

    return bytes_recv;
}

int picoquic_send_through_server_sockets(picoquic_server_sockets_t* sockets, 
    struct sockaddr* addr_dest, socklen_t dest_length, 
    struct sockaddr* addr_from, socklen_t from_length, unsigned long from_if, 
    const char* bytes, int length)
{
    /* Both Linux and Windows use separate sockets for V4 and V6 */
#ifndef NS3
    int socket_index = (addr_dest->sa_family == AF_INET) ? 1 : 0;
#else
    int socket_index = 0;
#endif

    int sent = picoquic_sendmsg(sockets->s_socket[socket_index], addr_dest, dest_length,
        addr_from, from_length, from_if, bytes, length);

#ifndef DISABLE_DEBUG_PRINTF
    if (sent <= 0) {
#ifdef _WINDOWS
        int last_error = WSAGetLastError();
#else
        int last_error = errno;
#endif
        DBG_PRINTF("Could not send packet on UDP socket[%d]= %d!\n",
            socket_index, last_error);
        DBG_PRINTF("Dest address length: %d, family: %d.\n",
            dest_length, addr_dest->sa_family);
    }
#endif

    return sent;
}

int picoquic_send_through_server_sockets_for_ps(int* quic_sockets, 
    struct sockaddr* addr_dest, socklen_t dest_length, 
    struct sockaddr* addr_from, socklen_t from_length, unsigned long from_if, 
    const char* bytes, int length)
{
    /* Both Linux and Windows use separate sockets for V4 and V6 */
#ifndef NS3
    int socket_index = (addr_dest->sa_family == AF_INET) ? 1 : 0;
#else
    int socket_index = 0;
#endif

    int sent = picoquic_sendmsg(*quic_sockets, addr_dest, dest_length,
        addr_from, from_length, from_if, bytes, length);

#ifndef DISABLE_DEBUG_PRINTF
    if (sent <= 0) {
#ifdef _WINDOWS
        int last_error = WSAGetLastError();
#else
        int last_error = errno;
#endif
        DBG_PRINTF("Could not send packet on UDP socket[%d]= %d!\n",
            socket_index, last_error);
        //DBG_PRINTF("Could not send packet on UDP socket[%d]= %s!\n",
          //  socket_index, strerror(errno));
        DBG_PRINTF("Dest address length: %d, family: %d.\n",
            dest_length, addr_dest->sa_family);
    }
#endif

    return sent;
}

int picoquic_get_server_address(const char* ip_address_text,
    int server_port, struct sockaddr_storage* server_address,
    int* server_addr_length, int* is_name)
{
    int ret = 0;
    struct sockaddr_in* ipv4_dest = (struct sockaddr_in*)server_address;
    struct sockaddr_in6* ipv6_dest = (struct sockaddr_in6*)server_address;

    /* get the IP address of the server */
    memset(server_address, 0, sizeof(struct sockaddr_storage));
    *is_name = 0;
    *server_addr_length = 0;

    if (inet_pton(AF_INET, ip_address_text, &ipv4_dest->sin_addr) == 1) {
        /* Valid IPv4 address */
        //DBG_PRINTF("getserveraddress->This is an ipv4 address\n");
        ipv4_dest->sin_family = AF_INET;
        ipv4_dest->sin_port = htons((unsigned short)server_port);
        //DBG_PRINTF("NPORT:%d", ipv4_dest->sin_port);
        *server_addr_length = sizeof(struct sockaddr_in);
    }
    else if (inet_pton(AF_INET6, ip_address_text, &ipv6_dest->sin6_addr) == 1) {
        /* Valid IPv6 address */
        ipv6_dest->sin6_family = AF_INET6;
        ipv6_dest->sin6_port = htons((unsigned short)server_port);
        *server_addr_length = sizeof(struct sockaddr_in6);
    }
    else {
        /* Server is described by name. Do a lookup for the IP address,
        * and then use the name as SNI parameter */
        struct addrinfo* result = NULL;
        struct addrinfo hints;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        if (getaddrinfo(ip_address_text, NULL, &hints, &result) != 0) {
            fprintf(stderr, "Cannot get IP address for %s\n", ip_address_text);
            ret = -1;
        }
        else {
            *is_name = 1;

            switch (result->ai_family) {
            case AF_INET:
                ipv4_dest->sin_family = AF_INET;
                ipv4_dest->sin_port = htons((unsigned short)server_port);
#ifdef _WINDOWS
                ipv4_dest->sin_addr.S_un.S_addr = ((struct sockaddr_in*)result->ai_addr)->sin_addr.S_un.S_addr;
#else
                ipv4_dest->sin_addr.s_addr = ((struct sockaddr_in*)result->ai_addr)->sin_addr.s_addr;
#endif
                * server_addr_length = sizeof(struct sockaddr_in);
                break;
            case AF_INET6:
                ipv6_dest->sin6_family = AF_INET6;
                ipv6_dest->sin6_port = htons((unsigned short)server_port);
                memcpy(&ipv6_dest->sin6_addr,
                    &((struct sockaddr_in6*)result->ai_addr)->sin6_addr,
                    sizeof(ipv6_dest->sin6_addr));
                *server_addr_length = sizeof(struct sockaddr_in6);
                break;
            default:
                fprintf(stderr, "Error getting IPv6 address for %s, family = %d\n",
                    ip_address_text, result->ai_family);
                ret = -1;
                break;
            }

            freeaddrinfo(result);
        }
    }

    return ret;
}

int picoquic_sendmsg(SOCKET_TYPE fd, struct 
    sockaddr* addr_dest, socklen_t dest_length, 
    struct sockaddr* addr_from, socklen_t from_length, 
    unsigned long dest_if, const char* bytes, int length)
{
    struct msghdr msg;
    struct iovec dataBuf;
    char cmsg_buffer[1024];
    int control_length = 0;
    int bytes_sent;
    struct cmsghdr* cmsg;

    /* Format the message header */

    dataBuf.iov_base = (char*)bytes;
    dataBuf.iov_len = length;

    memset(&msg, 0, sizeof(msg));
    msg.msg_name = addr_dest;
    msg.msg_namelen = dest_length;
    msg.msg_iov = &dataBuf;
    msg.msg_iovlen = 1;
    msg.msg_control = (void*)cmsg_buffer;
    msg.msg_controllen = sizeof(cmsg_buffer);

    /* Format the control message */
    cmsg = CMSG_FIRSTHDR(&msg);

    if (addr_from != NULL && from_length != 0) {
        if (addr_from->sa_family == AF_INET) {
#ifdef IP_PKTINFO
            memset(cmsg, 0, CMSG_SPACE(sizeof(struct in_pktinfo)));
            cmsg->cmsg_level = IPPROTO_IP;
            cmsg->cmsg_type = IP_PKTINFO;
            cmsg->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
            struct in_pktinfo* pktinfo = (struct in_pktinfo*)CMSG_DATA(cmsg);
            pktinfo->ipi_spec_dst.s_addr = ((struct sockaddr_in*)addr_from)->sin_addr.s_addr;
            pktinfo->ipi_ifindex = dest_if;
            control_length += CMSG_SPACE(sizeof(struct in_pktinfo));
#else
            /* The IP_PKTINFO structure is not defined on BSD */
            memset(cmsg, 0, CMSG_SPACE(sizeof(struct in_addr)));
            cmsg->cmsg_level = IPPROTO_IP;
            cmsg->cmsg_type = IP_SENDSRCADDR;
            cmsg->cmsg_len = CMSG_LEN(sizeof(struct in_addr));
            struct in_addr* pktinfo = (struct in_addr*)CMSG_DATA(cmsg);
            pktinfo->s_addr = ((struct sockaddr_in*)addr_from)->sin_addr.s_addr;
            control_length += CMSG_SPACE(sizeof(struct in_addr));
#endif
        }
        else if (addr_from->sa_family == AF_INET6) {
            memset(cmsg, 0, CMSG_SPACE(sizeof(struct in6_pktinfo)));
            cmsg->cmsg_level = IPPROTO_IPV6;
            cmsg->cmsg_type = IPV6_PKTINFO;
            cmsg->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
            struct in6_pktinfo* pktinfo6 = (struct in6_pktinfo*)CMSG_DATA(cmsg);
            memcpy(&pktinfo6->ipi6_addr, &((struct sockaddr_in6*)addr_from)->sin6_addr, sizeof(struct in6_addr));
            pktinfo6->ipi6_ifindex = dest_if;

            control_length += CMSG_SPACE(sizeof(struct in6_pktinfo));
        }
        else {
            DBG_PRINTF("Unexpected address family: %d\n", addr_from->sa_family);
        }

#if 0
#if defined(IP_PMTUDISC_DO) || defined(IP_DONTFRAG)
        if (addr_from->sa_family == AF_INET && length > PICOQUIC_INITIAL_MTU_IPV4) {
#ifdef CMSG_ALIGN
            struct cmsghdr* cmsg_2 = (struct cmsghdr*)((unsigned char*)cmsg + CMSG_ALIGN(cmsg->cmsg_len));
            {
#else
            struct cmsghdr* cmsg_2 = CMSG_NXTHDR((&msg), cmsg);
            if (cmsg_2 == NULL) {
                DBG_PRINTF("Cannot obtain second CMSG (control_length: %d)\n", control_length);
            }
            else {
#endif
#ifdef IP_PMTUDISC_DO
                /* This sets the don't fragment bit on Linux */
                int val = IP_PMTUDISC_DO;
                cmsg_2->cmsg_level = IPPROTO_IP;
                cmsg_2->cmsg_type = IP_MTU_DISCOVER;
#else
                /* On BSD systems, just use IP_DONTFRAG */
                int val = 1;
                cmsg_2->cmsg_level = IPPROTO_IP;
                cmsg_2->cmsg_type = IP_DONTFRAG;
#endif
                cmsg_2->cmsg_len = CMSG_LEN(sizeof(int));
                memcpy(CMSG_DATA(cmsg_2), &val, sizeof(int));
                control_length += CMSG_SPACE(sizeof(int));
            }
            }
#endif
#else
#if defined(IP_DONTFRAG)
        if (addr_from->sa_family == AF_INET && length > PICOQUIC_INITIAL_MTU_IP$
#ifdef CMSG_ALIGN
            struct cmsghdr* cmsg_2 = (struct cmsghdr*)((unsigned char*)cmsg $
        {
#else
                struct cmsghdr* cmsg_2 = CMSG_NXTHDR((&msg), cmsg);
        if (cmsg_2 == NULL) {
            DBG_PRINTF("Cannot obtain second CMSG (control_length: %d)\n", $
        }
        else {
#endif
            /* On BSD systems, just use IP_DONTFRAG */
            int val = 1;
            cmsg_2->cmsg_level = IPPROTO_IP;
            cmsg_2->cmsg_type = IP_DONTFRAG;
            cmsg_2->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cmsg_2), &val, sizeof(int));
            control_length += CMSG_SPACE(sizeof(int));
        }
        }
#endif


#endif

}

    msg.msg_controllen = control_length;
    if (control_length == 0) {
        msg.msg_control = NULL;
    }

    bytes_sent = sendmsg(fd, &msg, 0); //调用socket的sendmsg发送数据

    return bytes_sent;
}

int picoquic_recvmsg(SOCKET_TYPE fd,
    struct sockaddr_storage* addr_from,
    socklen_t* from_length,
    struct sockaddr_storage* addr_dest,
    socklen_t* dest_length,
    unsigned long* dest_if,
    uint8_t* buffer, int buffer_max,
    int* tos)
    {
        int bytes_recv = 0;//表示收到的字节数
        struct msghdr msg;//声明message结构体，包含地址，内容，辅助信息
        struct iovec dataBuf;//声明要接受的数据的变量
        char cmsg_buffer[1024];

        if (dest_length != NULL) {//如果目标长度指针不为空，则将该指针指向的内容，也就是目标地址的长度置为0
            *dest_length = 0;
        }

        if (dest_if != NULL) {
            *dest_if = 0;
        }

        dataBuf.iov_base = (char*)buffer;//将buffer和接受的数据连接，之后接收到数据，buffer便指向存储的数据地址了
        dataBuf.iov_len = buffer_max;

        msg.msg_name = (struct sockaddr*)addr_from;//以下应该是分配好内存地址，以待接收到socket中的msg信息时，能直接指向对应内容
        msg.msg_namelen = *from_length;
        msg.msg_iov = &dataBuf;
        msg.msg_iovlen = 1;
        msg.msg_flags = 0;
        msg.msg_control = (void*)cmsg_buffer;
        msg.msg_controllen = sizeof(cmsg_buffer);

        bytes_recv = recvmsg(fd, &msg, 0);//这一步就是调用socket接受信息；返回的是读取到的字节数

        if (bytes_recv <= 0) {
            *from_length = 0;
            if (bytes_recv <= -1) {
                printf("bytes_recv: %d, err: %s\n", bytes_recv, strerror(errno));
            }
        }
        else {
            /* Get the control information */
            struct cmsghdr* cmsg;
            *from_length = msg.msg_namelen;

            for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                if (cmsg->cmsg_level == IPPROTO_IP) {
#ifdef IP_PKTINFO
                    if (cmsg->cmsg_type == IP_PKTINFO && addr_dest != NULL && dest_length != NULL) {
                        struct in_pktinfo* pPktInfo = (struct in_pktinfo*)CMSG_DATA(cmsg);
                        ((struct sockaddr_in*)addr_dest)->sin_family = AF_INET;
                        ((struct sockaddr_in*)addr_dest)->sin_port = 0;
                        ((struct sockaddr_in*)addr_dest)->sin_addr.s_addr = pPktInfo->ipi_addr.s_addr;
                        *dest_length = sizeof(struct sockaddr_in);

                        if (dest_if != NULL) {
                            *dest_if = pPktInfo->ipi_ifindex;
                        }
                    }
#else
                    /* The IP_PKTINFO structure is not defined on BSD */
                    if ((cmsg->cmsg_level == IPPROTO_IP) && (cmsg->cmsg_type == IP_RECVDSTADDR)) {
                        if (addr_dest != NULL && dest_length != NULL) {
                            struct in_addr* pPktInfo = (struct in_addr*)CMSG_DATA(cmsg);
                            ((struct sockaddr_in*)addr_dest)->sin_family = AF_INET;
                            ((struct sockaddr_in*)addr_dest)->sin_port = 0;
                            ((struct sockaddr_in*)addr_dest)->sin_addr.s_addr = pPktInfo->s_addr;
                            *dest_length = sizeof(struct sockaddr_in);

                            if (dest_if != NULL) {
                                *dest_if = 0;
                            }
                        }
#endif
                        if (cmsg->cmsg_type == IP_TOS && tos) {
                            *tos = *(int*)CMSG_DATA(cmsg);
                        }
                    }
                    else if (cmsg->cmsg_level == IPPROTO_IPV6) {
                        if (cmsg->cmsg_type == IPV6_PKTINFO && addr_dest != NULL && dest_length != NULL) {
                            struct in6_pktinfo* pPktInfo6 = (struct in6_pktinfo*)CMSG_DATA(cmsg);

                            ((struct sockaddr_in6*)addr_dest)->sin6_family = AF_INET6;
                            ((struct sockaddr_in6*)addr_dest)->sin6_port = 0;
                            memcpy(&((struct sockaddr_in6*)addr_dest)->sin6_addr, &pPktInfo6->ipi6_addr, sizeof(struct in6_addr));
                            *dest_length = sizeof(struct sockaddr_in6);

                            if (dest_if != NULL) {
                                *dest_if = pPktInfo6->ipi6_ifindex;
                            }
                        }
                        else if (cmsg->cmsg_type == IPV6_TCLASS && tos) {
                            *tos = *(int*)CMSG_DATA(cmsg);
                        }
                    }
                }
            }

            return bytes_recv;
        }