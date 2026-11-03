#ifndef RJM_LWIPOPTS_H
#define RJM_LWIPOPTS_H

/* lwIP for the USB-NCM Web configuration portal (bare metal, polled). */
#define NO_SYS 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define MEM_LIBC_MALLOC 0
#define MEM_ALIGNMENT 4
#define MEM_SIZE 12000
#define MEMP_NUM_TCP_SEG 32
#define MEMP_NUM_TCP_PCB 8
#define PBUF_POOL_SIZE 8
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 0
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_DHCP 0
#define LWIP_DNS 0
#define LWIP_IGMP 0
#define LWIP_SINGLE_NETIF 1
#define TCP_MSS (1500 - 20 - 20)
#define TCP_WND (4 * TCP_MSS)
#define TCP_SND_BUF (4 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * TCP_SND_BUF + TCP_MSS - 1) / TCP_MSS)
#define LWIP_IP_ACCEPT_UDP_PORT(p) ((p) == PP_NTOHS(67))
#define ETH_PAD_SIZE 0
#define LWIP_CHKSUM_ALGORITHM 3
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define MEM_STATS 0
#define SYS_STATS 0
#define MEMP_STATS 0
#define LINK_STATS 0

#endif
