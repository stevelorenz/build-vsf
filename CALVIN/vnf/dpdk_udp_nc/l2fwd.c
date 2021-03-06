/*
 *
 * =====================================================================================
 *       Filename:  l2fwd.c
 *
 *    Description:  Network coding UDP flows with NCKernel
 *
 *                  - Operations are performed in Layer 2
 *
 *                  - With default mode, only UDP payload is coded.
 *
 *      Dev-Note:
 *                  - Currently, concurrent processing is not implemented. The
 *                    Ethernet frames are handled one by one with a single
 *                    process and a single core. Namely, use DPDK synchronous
 *                    run-to-completion mode.
 *
 *                  - In order to reduce per-packet latency and implementation
 *                    complexity. mbufs are filtered and coded one by one. Some
 *                    instructions are prefetched with DPDK features. However,
 *                    it should be more efficient to process multiple mbufs
 *                    simultaneously with e.g. SIMD instructions.
 *
 *                  - For all coder types, a coding buffer(static uint8_t array)
 *                    is used to exchange data between mbufs and coder's own
 *                    buffer. The input mbuf is always freed after the coding
 *                    operator of all coder types. Additional mbufs are cloned
 *                    to encapsulate output(s) of the coder.
 *
 *        Version:  0.4
 *          Email:  xianglinks@gmail.com
 *
 * =====================================================================================
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_bus_pci.h>
#include <rte_byteorder.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_debug.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_interrupts.h>
#include <rte_ip.h>
#include <rte_kni.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_memcpy.h>
#include <rte_memory.h>
#include <rte_mempool.h>
#include <rte_pdump.h>
#include <rte_per_lcore.h>
#include <rte_prefetch.h>
#include <rte_random.h>
#include <rte_udp.h>

/*#include "kni.h"*/
#include "ncmbuf.h"

#ifndef DEBUG
#define DEBUG 0
#endif /* ifndef DEBUG 0 */

/* NCKernel options */
#ifndef FIELD
#define FIELD "binary8"
#endif

#ifndef PROTOCOL
#define PROTOCOL "noack"
#endif

#ifndef SYMBOL_SIZE
#define SYMBOL_SIZE "258"
#endif

#ifndef SYMBOLS
#define SYMBOLS "2"
#endif

#ifndef REDUNDANCY
#define REDUNDANCY "1"
#endif

#define MEMPOOL_CACHE_SIZE 256
#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 1024
#define MAX_RX_QUEUE_PER_LCORE 4
#define MAX_TX_QUEUE_PER_PORT 4

#define UDP_HDR_LEN 8
#define NC_MAX_DATA_LEN 1500
#define NC_MAX_HDR_LEN 90

/* Number of packets to read in one go if Batch processing is used */
#define DEFAULT_PKT_BURST_SZ 32

/**********************
 *  Global Variables  *
 **********************/

static volatile bool force_quit;

/* Enable debug mode */
static int debugging = 0;

/*static int verbose = 0;*/
static int filtering = 1;

static int mac_updating = 1;

static int packet_capturing = 0;

/* Enable running in the kni mode */
static int kni_mode = 0;

static int coder_type = 0;

static uint8_t nb_ports = 0;

static uint8_t nb_used_lcores = 1;

/* Burst-based packet processing
 * Default maximal received packets at each time
 * Smaller pkt_burst -> lower per-packet latency -> lower bandwidth
 * Rx polling and tx draining parameters are configurable
 * */
static int max_pkt_burst = 1;
static int poll_short_interval_us;
static int max_poll_short_try;
static int poll_long_interval_us;
static int burst_tx_drain_us = 10;

/* Processing delay of frames */
static uint64_t st_proc_tsc = 0;
static uint64_t proc_tsc = 0;
static double proc_time = 0.0;
static uint64_t nb_udp_dgrams = 0;

/* Source and destination MAC addresses used for sending frames */
static uint32_t dst_mac[ETHER_ADDR_LEN];
static uint32_t src_mac[ETHER_ADDR_LEN];
static struct ether_addr dst_mac_addr;
static struct ether_addr src_mac_addr;

static struct in_addr vnf_recv_ip_dst_addr;
uint32_t vnf_recv_dst_ip = 0;

static struct in_addr vnf_send_src_ip_addr;
uint32_t vnf_send_src_ip = 0;
static struct in_addr vnf_send_dst_ip_addr;
uint32_t vnf_send_dst_ip = 0;

static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

/* Ethernet addresses of ports */
static struct ether_addr l2fwd_ports_eth_addr[RTE_MAX_ETHPORTS];

/* mask of enabled ports */
static uint32_t l2fwd_enabled_port_mask = 0;

static uint32_t l2fwd_dst_ports[RTE_MAX_ETHPORTS];
static unsigned int l2fwd_rx_queue_per_lcore = 1;

struct lcore_queue_conf {
        unsigned n_rx_port;
        unsigned rx_port_list[MAX_RX_QUEUE_PER_LCORE];
        volatile bool
            start_rx_flag[MAX_RX_QUEUE_PER_LCORE]; /* A flag indicate the
                                                      queue should start
                                                      burst receiving */
        uint16_t rx_burst_try[MAX_RX_QUEUE_PER_LCORE];
} __rte_cache_aligned;

struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE];

static struct rte_eth_dev_tx_buffer* tx_buffer[RTE_MAX_ETHPORTS];

/* DPDK port configuration
 * - Rx and tx offloadings can be also configured here
 * */
static struct rte_eth_conf port_conf = {
    .rxmode =
        {
            .split_hdr_size = 0,
            .ignore_offload_bitfield = 1,
            .offloads = DEV_RX_OFFLOAD_CRC_STRIP,
        },
    .txmode =
        {
            .mq_mode = ETH_MQ_TX_NONE,
        },
};

struct rte_mempool* l2fwd_pktmbuf_pool = NULL;

#define KNI_MAX_KTHREAD 32
/*
 * Structure of port parameters
 */
struct kni_port_params {
        uint16_t port_id;    /* Port ID */
        uint32_t nb_lcore_k; /* Number of lcores for KNI multi kernel threads */
        uint32_t nb_kni;     /* Number of KNI devices to be created */
        unsigned lcore_k[KNI_MAX_KTHREAD];    /* lcore ID list for kthreads */
        struct rte_kni* kni[KNI_MAX_KTHREAD]; /* KNI context pointers */
} __rte_cache_aligned;

static struct kni_port_params* kni_port_params_array[RTE_MAX_ETHPORTS];

/* Since multiple threads can access this value, atomic counter is used here */
static rte_atomic32_t kni_stop = RTE_ATOMIC32_INIT(0);
static rte_atomic32_t kni_dev_up = RTE_ATOMIC32_INIT(0);

/* A flag indicate if the kni egress thread SHOULD sleep, situation(s):
 * - No frames are received by ingress thread for amount of tries
 * */
static rte_atomic16_t kni_egress_sleep = RTE_ATOMIC16_INIT(0);

/*static int kni_change_mtu(uint16_t portid, unsigned int new_mtu);*/
static int kni_config_network_interface(uint16_t portid, uint8_t if_up);
static int kni_config_mac_address(uint16_t portid, uint8_t mac_addr[]);

/* NC coders and operation functions */
struct nck_encoder enc;
struct nck_decoder dec;
struct nck_recoder rec;

/****************
 *  Prototypes  *
 ****************/

void l2fwd_put_rxq(struct rte_mbuf* m, uint16_t portid);
void nc_udp(int coder_type, struct rte_mbuf* m, uint16_t portid);

/******************************
 *  Function Implementations  *
 ******************************/

static inline __attribute__((always_inline)) void recalc_cksum_inline(
    struct ipv4_hdr* iph, struct udp_hdr* udph)
{
        udph->dgram_cksum = 0;
        iph->hdr_checksum = 0;
        udph->dgram_cksum = rte_ipv4_udptcp_cksum(iph, udph);
        iph->hdr_checksum = rte_ipv4_cksum(iph);
}

/**
 * @brief Convert Uint32 IP address to x.x.x.x format
 */
__attribute__((unused)) static void get_ip_str(uint32_t ip, char* ip_addr_str)
{
        ip = ntohl(ip);
        uint8_t octet[4];
        /*char ip_addr_str[16];*/
        for (int i = 0; i < 4; i++) {
                octet[i] = ip >> (i * 8);
        }
        sprintf(
            ip_addr_str, "%d.%d.%d.%d", octet[3], octet[2], octet[1], octet[0]);
}

/**
 * @brief Filter a Ethernet frame, return true if frame match the rules.
 */
static inline int8_t filter_ether_frame(struct rte_mbuf* m)
{
        struct ether_hdr* ethh;
        ethh = rte_pktmbuf_mtod(m, struct ether_hdr*);
        /* Filter out non-IP packets */
        if (ethh->ether_type != rte_cpu_to_be_16(ETHER_TYPE_IPv4))
                return -1;

        /* Filter out non-UDP packets */
        struct ipv4_hdr* iph;
        iph = rte_pktmbuf_mtod_offset(m, struct ipv4_hdr*, ETHER_HDR_LEN);
        if (iph->next_proto_id != IPPROTO_UDP)
                return -2;

        /* Filter out looping packets with source MAC address of any forwarding
         * ports  */
        uint8_t i;
        for (i = 0; i < nb_ports; ++i) {
                if (is_same_ether_addr(
                        &(ethh->s_addr), &(l2fwd_ports_eth_addr[i])))
                        return -3;
        }

        return 1;
}

/**
 * @brief Filter an array of mbufs
 */
static inline void filter_mbuf_array(
    struct rte_mbuf* mbuf_array[], uint16_t num)
{
        uint16_t i = 0;
        int ret = 0;
        for (i = 0; i < num; ++i) {
                rte_prefetch0(rte_pktmbuf_mtod(mbuf_array[i], void*));
                ret = filter_ether_frame(mbuf_array[i]);
                if (ret != 1) {
                        rte_pktmbuf_free(mbuf_array[i]);
                        mbuf_array[i] = NULL;
                }
        }
}

/* TODO:  <30-07-18, Zuo> Add NSH header inside UDP */

__attribute__((unused)) static inline bool is_ipv4_pkt(struct rte_mbuf* m)
{
        struct ether_hdr* ethh;
        ethh = rte_pktmbuf_mtod(m, struct ether_hdr*);
        if (ethh->ether_type == rte_cpu_to_be_16(ETHER_TYPE_IPv4)) {
                return true;
        }
        return false;
}

__attribute__((unused)) static inline bool is_udp_dgram(struct rte_mbuf* m)
{
        struct ipv4_hdr* iph;
        iph = rte_pktmbuf_mtod_offset(m, struct ipv4_hdr*, ETHER_HDR_LEN);
        if (iph->next_proto_id == IPPROTO_UDP) {
                return true;
        }

        return false;
}

static void l2fwd_mac_updating(struct rte_mbuf* m)
{
        struct ether_hdr* eth;
        eth = rte_pktmbuf_mtod(m, struct ether_hdr*);
        /* Update destination and source MAC addr */
        ether_addr_copy(&src_mac_addr, &eth->s_addr);
        ether_addr_copy(&dst_mac_addr, &eth->d_addr);
}

void l2fwd_put_rxq(struct rte_mbuf* m, uint16_t portid)
{
        unsigned dst_port;
        int sent;
        struct rte_eth_dev_tx_buffer* buffer;

        dst_port = l2fwd_dst_ports[portid];

        if (mac_updating) {
                l2fwd_mac_updating(m);
        }

        buffer = tx_buffer[dst_port];

        /* TODO:  <30-07-18, Zuo> Add TX latency manually here, the OpenStack
         * SFC extension seems can not handle such fast situation */
        sent = rte_eth_tx_buffer(dst_port, 0, buffer, m);

        /* Possible draining processes */
        if (sent) {
                RTE_LOG(DEBUG, USER1,
                    "Trigger %d UDP packets drained in the TX buffer\n", sent);
        }
}

void nc_udp(int coder_type, struct rte_mbuf* m, uint16_t portid)
{
        if (coder_type == 0) {
                encode_udp_data(
                    &enc, m, l2fwd_pktmbuf_pool, portid, l2fwd_put_rxq);
        } else if (coder_type == 1) {
                decode_udp_data(
                    &dec, m, l2fwd_pktmbuf_pool, portid, l2fwd_put_rxq);
        } else if (coder_type == 2) {
                recode_udp_data(
                    &rec, m, l2fwd_pktmbuf_pool, portid, l2fwd_put_rxq);
        } else {
                l2fwd_put_rxq(m, portid);
        }
}

/**
 * @brief Modify the IP addresses of given mbuf list
 *        This is required when KNI interfaces are used and frames are operated
 *        with AF_INET sockets in layer 4.
 *
 */
static inline void mod_ip_addr(uint16_t nb_rx, struct rte_mbuf* pkts_burst[],
    uint32_t* src_addr, uint32_t* dst_addr)
{
        uint16_t i;
        uint16_t in_iphdr_len;
        struct ipv4_hdr* iph;
        struct udp_hdr* udph;

        for (i = 0; i < nb_rx; ++i) {
                if (pkts_burst[i] == NULL) {
                        continue;
                }
                iph = rte_pktmbuf_mtod_offset(
                    pkts_burst[i], struct ipv4_hdr*, ETHER_HDR_LEN);
                if (src_addr != NULL) {
                        rte_memcpy(&iph->src_addr, src_addr, sizeof(uint32_t));
                }
                if (dst_addr != NULL) {
                        rte_memcpy(&iph->dst_addr, dst_addr, sizeof(uint32_t));
                }
                in_iphdr_len = (iph->version_ihl & 0x0F) * 32 / 8;
                udph = (struct udp_hdr*)((char*)iph + in_iphdr_len);
                recalc_cksum_inline(iph, udph);
        }
}

/**
 * @brief Push received packets to the bound KNI device
 */
static void push_kni(uint16_t portid, uint16_t nb_rx,
    struct rte_mbuf* pkts_burst[], uint8_t mod_ip)
{
        uint16_t i;
        uint16_t push_num = 0;

        if (likely(mod_ip == 1)) {
                /* Change destination IP address, this is required to allow
                 * operation in layer 4 */
                mod_ip_addr(nb_rx, pkts_burst, NULL, &vnf_recv_dst_ip);
        }

        push_num = rte_kni_tx_burst(
            kni_port_params_array[portid]->kni[0], pkts_burst, nb_rx);

        rte_kni_handle_request(kni_port_params_array[portid]->kni[0]);

        if (unlikely(push_num < nb_rx)) {
                /* Too much packets being pushed, KNI device drop some packets
                 */
                RTE_LOG(ERR, USER1,
                    "Too much packets are pushed into KNI device\n");
                for (i = push_num; i < nb_rx - push_num; ++i) {
                        rte_pktmbuf_free(pkts_burst[i]);
                        pkts_burst[i] = NULL;
                }
        }
}

/**
 * @brief Poll the KNI device and put received packets into rx queue
 */
static void poll_kni(uint16_t portid)
{
        uint8_t i = 0;
        uint16_t num_rx = 0;
        struct rte_mbuf* pkts_burst[max_pkt_burst];
        num_rx = rte_kni_rx_burst(
            kni_port_params_array[portid]->kni[0], pkts_burst, max_pkt_burst);

        if (unlikely(num_rx == 0)) {
                return;
        } else if (unlikely(num_rx > max_pkt_burst)) {
                RTE_LOG(ERR, USER1,
                    "Error receiving from KNI, port number:%u\n", portid);
                return;
        }
        rte_kni_handle_request(kni_port_params_array[portid]->kni[0]);
        RTE_LOG(DEBUG, USER1, "Recv %u packets from KNI device\n", num_rx);

        /* Filter out device OAM messages from kernel */
        filter_mbuf_array(pkts_burst, num_rx);
        mod_ip_addr(num_rx, pkts_burst, &vnf_send_src_ip, &vnf_send_dst_ip);
        for (i = 0; i < num_rx; ++i) {
                if (pkts_burst[i] != NULL) {
                        l2fwd_put_rxq(pkts_burst[i], portid);
                }
        }
}

/******************************************
 *  Just Used for Performance Comparison  *
 ******************************************/

/* ---------------------------------------------------------------------------*/

static void kni_ingress_loop(void)
{
        struct rte_mbuf* pkts_burst[max_pkt_burst];
        unsigned lcore_id;
        unsigned i, portid, nb_rx;
        struct lcore_queue_conf* qconf;
        uint8_t egress_sleep_flag;

        lcore_id = rte_lcore_id();
        qconf = &lcore_queue_conf[lcore_id];

        /* MARK: qconf SHOULD be modified to support multi-core feature */
        if (qconf->n_rx_port == 0) {
                RTE_LOG(INFO, USER1, "Lcore %u has nothing to do\n", lcore_id);
                return;
        }

        for (i = 0; i < qconf->n_rx_port; i++) {
                portid = qconf->rx_port_list[i];
                qconf->start_rx_flag[i] = true;
                qconf->rx_burst_try[i] = 0;
        }
        RTE_LOG(INFO, USER1, "Lcore %u enter kni ingress loop\n", lcore_id);
        while (!force_quit) {
                for (i = 0; i < qconf->n_rx_port; i++) {
                        portid = qconf->rx_port_list[i];
                        if (qconf->start_rx_flag[i]) {
                                nb_rx = rte_eth_rx_burst(
                                    portid, 0, pkts_burst, max_pkt_burst);
                                if (unlikely(nb_rx > (uint16_t)max_pkt_burst)) {
                                        RTE_LOG(ERR, USER1,
                                            "Error receiving from eth dev\n");
                                        return;
                                }
                                if (likely(nb_rx == 0)) {
                                        if (max_poll_short_try == 0) {
                                                /* Keep polling, SHOULD use 100%
                                                 * of CPU */
                                                continue;
                                        }
                                        /* execute "pause" instruction to avoid
                                         * context switch for short sleep */
                                        if (qconf->rx_burst_try[i]
                                            < max_poll_short_try) {
                                                qconf->rx_burst_try[i] += 1;
                                                rte_delay_us(
                                                    poll_short_interval_us);
                                        } else {
                                                egress_sleep_flag
                                                    = rte_atomic16_read(
                                                        &kni_egress_sleep);
                                                if (egress_sleep_flag == 0) {
                                                        rte_atomic16_inc(
                                                            &kni_egress_sleep);
                                                }
                                                /* Long sleep force running
                                                 * thread to suspend */
                                                usleep(poll_long_interval_us);
                                        }
                                } else {
                                        /* Process received burst packets */
                                        rte_atomic16_clear(&kni_egress_sleep);
                                        st_proc_tsc = rte_rdtsc();
                                        if (filtering) {
                                                filter_mbuf_array(
                                                    pkts_burst, nb_rx);
                                        }
                                        /* Put burst packets to kni
                                         * device */
                                        push_kni(portid, nb_rx, pkts_burst, 1);
                                        /* ISSUE: global variables are used here
                                           SHOULD be replaced with local
                                           variables
                                           */
                                        if (debugging == 1) {
                                                proc_tsc
                                                    = rte_rdtsc() - st_proc_tsc;
                                                proc_time
                                                    = (1.0 / rte_get_timer_hz())
                                                    * proc_tsc * 1000;
                                                RTE_LOG(INFO, USER1,
                                                    "[Port:%d] Process a burst "
                                                    "of %d packets, proc time: "
                                                    "%.4f ms, number of "
                                                    "already received UDP "
                                                    "packets: %ld\n",
                                                    portid, nb_rx, proc_time,
                                                    nb_udp_dgrams);
                                        }
                                        /* Reset rx try times */
                                        qconf->rx_burst_try[i] = 0;
                                }
                        } else {
                                /* TODO (Maybe): Check the link status and
                                 * update start_rx flags */
                        }
                }
        }
}

static void kni_egress_loop(void)
{
        unsigned lcore_id;
        unsigned i, portid;
        struct lcore_queue_conf* qconf;
        uint64_t prev_tsc, diff_tsc, cur_tsc;
        uint16_t sent;
        uint16_t sleep_flag;

        const uint64_t drain_tsc
            = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * burst_tx_drain_us;
        struct rte_eth_dev_tx_buffer* buffer;

        prev_tsc = 0;

        lcore_id = rte_lcore_id();
        RTE_LOG(INFO, USER1, "Lcore %u use the qconf of Lcore 0\n", lcore_id);
        qconf = &lcore_queue_conf[0];

        if (qconf->n_rx_port == 0) {
                RTE_LOG(INFO, USER1, "Lcore %u has nothing to do\n", lcore_id);
                return;
        }

        for (i = 0; i < qconf->n_rx_port; i++) {
                portid = qconf->rx_port_list[i];
                qconf->start_rx_flag[i] = true;
                qconf->rx_burst_try[i] = 0;
        }

        RTE_LOG(INFO, USER1, "Lcore %u enter kni egress loop\n", lcore_id);
        while (!force_quit) {
                sleep_flag = rte_atomic16_read(&kni_egress_sleep);
                if (sleep_flag == 1) {
                        usleep(poll_long_interval_us);
                }
                cur_tsc = rte_rdtsc();
                diff_tsc = cur_tsc - prev_tsc;
                if (unlikely(diff_tsc > drain_tsc)) {
                        /* TX burst queue drain */
                        for (i = 0; i < qconf->n_rx_port; i++) {
                                /* Get corresponded tx port of the rx port */
                                portid
                                    = l2fwd_dst_ports[qconf->rx_port_list[i]];
                                if (kni_mode) {
                                        poll_kni(portid);
                                }

                                buffer = tx_buffer[portid];
                                /* Drain all buffered packets in the tx
                                 * queue */
                                sent = rte_eth_tx_buffer_flush(
                                    portid, 0, buffer);
                                if (sent) {
                                        RTE_LOG(DEBUG, USER1,
                                            "Drain %d UDP packets in "
                                            "the tx "
                                            "queue\n",
                                            sent);
                                }
                        }

                        prev_tsc = cur_tsc;
                }
        }
}
/* ---------------------------------------------------------------------------*/

static void l2fwd_main_loop(void)
{
        /* MARK: Packets are read in a burst of size MAX_PKT_BURST
         * This is also a tunable parameter for latency
         * */
        struct rte_mbuf* pkts_burst[max_pkt_burst];
        struct rte_mbuf* m;
        unsigned lcore_id;
        unsigned i, j, portid, nb_rx;
        struct lcore_queue_conf* qconf;
        uint64_t prev_tsc, diff_tsc, cur_tsc;
        uint16_t sent;

        /* MARK: Why there is a US_PER_S - 1 is added? Maybe check jiffies */
        const uint64_t drain_tsc
            = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * burst_tx_drain_us;
        struct rte_eth_dev_tx_buffer* buffer;

        prev_tsc = 0;

        lcore_id = rte_lcore_id();
        qconf = &lcore_queue_conf[lcore_id]; /* queue configuration */

        if (qconf->n_rx_port == 0) {
                RTE_LOG(INFO, USER1, "Lcore %u has nothing to do\n", lcore_id);
                return;
        }

        RTE_LOG(INFO, USER1, "Entering main loop on lcore %u\n", lcore_id);

        for (i = 0; i < qconf->n_rx_port; i++) {
                portid = qconf->rx_port_list[i];
                qconf->start_rx_flag[i] = true;
                qconf->rx_burst_try[i] = 0;
        }

        /* Main loop for receiving, processing and sending packets
         * Currently use synchronous and non-blocking IO functions
         * TODO: Refactor the code here
         * */
        while (!force_quit) {

                cur_tsc = rte_rdtsc();

                diff_tsc = cur_tsc - prev_tsc;
                if (unlikely(diff_tsc > drain_tsc)) {
                        /* TX burst queue drain */
                        for (i = 0; i < qconf->n_rx_port; i++) {
                                /* Get corresponded tx port of the rx port */
                                portid
                                    = l2fwd_dst_ports[qconf->rx_port_list[i]];
                                if (kni_mode) {
                                        poll_kni(portid);
                                }

                                buffer = tx_buffer[portid];
                                /* Drain all buffered packets in the tx
                                 * queue */
                                sent = rte_eth_tx_buffer_flush(
                                    portid, 0, buffer);
                                if (sent) {
                                        RTE_LOG(DEBUG, USER1,
                                            "Drain %d UDP packets in "
                                            "the tx "
                                            "queue\n",
                                            sent);
                                }
                        }

                        prev_tsc = cur_tsc;
                }

                for (i = 0; i < qconf->n_rx_port; i++) {
                        portid = qconf->rx_port_list[i];
                        /* MARK: A burst number of packets are returned by this
                         * function, so can not get the recv timestamps for each
                         * packet here. Only is the burst number is one.
                         * */
                        if (qconf->start_rx_flag[i]) {
                                nb_rx = rte_eth_rx_burst(
                                    portid, 0, pkts_burst, max_pkt_burst);
                                if (unlikely(nb_rx > (uint16_t)max_pkt_burst)) {
                                        RTE_LOG(ERR, USER1,
                                            "Error receiving from eth dev\n");
                                        return;
                                }
                                if (likely(nb_rx == 0)) {
                                        if (max_poll_short_try == 0) {
                                                /* Keep polling, SHOULD use 100%
                                                 * of CPU */
                                                continue;
                                        }
                                        /* execute "pause" instruction to avoid
                                         * context switch for short sleep */
                                        if (qconf->rx_burst_try[i]
                                            < max_poll_short_try) {
                                                qconf->rx_burst_try[i] += 1;
                                                rte_delay_us(
                                                    poll_short_interval_us);
                                        } else {
                                                /* Long sleep force running
                                                 * thread to suspend */
                                                usleep(poll_long_interval_us);
                                        }
                                } else {
                                        /* Process received burst packets */
                                        /* Handle prefetches */
                                        st_proc_tsc = rte_rdtsc();
                                        if (filtering) {
                                                filter_mbuf_array(
                                                    pkts_burst, nb_rx);
                                        }
                                        if (kni_mode) {
                                                /* Put burst packets to kni */
                                                push_kni(portid, nb_rx,
                                                    pkts_burst, 1);
                                        } else {
                                                /* Process burst packets */
                                                for (j = 0; j < nb_rx; j++) {
                                                        m = pkts_burst[j];
                                                        if (m == NULL) {
                                                                continue;
                                                        }
                                                        nb_udp_dgrams += 1;
                                                        rte_prefetch0(
                                                            rte_pktmbuf_mtod(
                                                                m, void*));
                                                        nc_udp(coder_type, m,
                                                            portid);
                                                }
                                        }

                                        /* Evaluate processing delay of burst
                                         * packets */
                                        if (debugging == 1) {
                                                proc_tsc
                                                    = rte_rdtsc() - st_proc_tsc;
                                                proc_time
                                                    = (1.0 / rte_get_timer_hz())
                                                    * proc_tsc * 1000;
                                                RTE_LOG(INFO, USER1,
                                                    "[Port:%d] Process a burst "
                                                    "of %d packets, proc time: "
                                                    "%.4f ms, number of "
                                                    "already received UDP "
                                                    "packets: %ld\n",
                                                    portid, nb_rx, proc_time,
                                                    nb_udp_dgrams);
                                        }
                                        /* Reset rx try times */
                                        qconf->rx_burst_try[i] = 0;
                                }
                        } else {
                                /* TODO (Maybe): Check the link status and
                                 * update start_rx flags */
                        }
                }
        }
}

/**
 * @brief Wait for all KNI devices to be setup, eating 100% CPU...
 */
static void kni_dev_setup(void)
{
        uint8_t i;
        for (i = 0; i < nb_ports; ++i) {
                rte_kni_handle_request(kni_port_params_array[i]->kni[0]);
        }
}

__attribute__((unused)) static int kni_dev_setup_loop(void)
{
        uint32_t dev_up_num = 0;
        while (1) {
                dev_up_num = rte_atomic32_read(&kni_dev_up);
                if (dev_up_num == nb_ports) {
                        RTE_LOG(INFO, USER1,
                            "All KNI device are up, exit setup loop\n");
                        break;
                }
                if (force_quit) {
                        break;
                }
                kni_dev_setup();
        }
        return 0;
}

static void l2fwd_kni_dual_lcore_loop(void)
{
        RTE_LOG(INFO, USER1, "Current lcore ID: %u\n", rte_lcore_id());
        if (rte_lcore_id() == 0) {
                RTE_LOG(INFO, USER1,
                    "Lcore %u, Entering setup loop for all KNI devices\n",
                    rte_lcore_id());
                kni_dev_setup_loop();
                RTE_LOG(
                    INFO, USER1, "Lcore %u is used for RX.\n", rte_lcore_id());
                kni_ingress_loop();
        } else if (rte_lcore_id() == 1) {
                RTE_LOG(
                    INFO, USER1, "Lcore %u is used for TX.\n", rte_lcore_id());
                kni_egress_loop();
        }
}

static int l2fwd_launch_one_lcore(__attribute__((unused)) void* dummy)
{
        if (kni_mode == 1 && nb_used_lcores == 2) {
                RTE_LOG(
                    INFO, USER1, "Entering main loop for KNI with dual-core\n");
                l2fwd_kni_dual_lcore_loop();
                return 0;
        } else {
                RTE_LOG(INFO, USER1, "[MARK] Only support single lcore \n");
                if (kni_mode == 1) {
                        RTE_LOG(INFO, USER1,
                            "Lcore %u, Entering setup loop for KNI devices\n",
                            rte_lcore_id());
                        kni_dev_setup_loop();
                }
                RTE_LOG(
                    INFO, USER1, "Entering main loop for IO and processing.\n");
                l2fwd_main_loop();
                return 0;
        }
}

/* display usage */
static void l2fwd_usage(const char* prgname)
{
        printf(
            "%s [EAL options] -- [APP options]\n"
            "-o CODERTYPE: NC coder type. 0->encoder, 1->decoder, 2->recoder.\n"
            "-p PORTMASK: hexadecimal bitmask of ports to configure\n"
            "-q NQ: number of queue (=ports) per lcore (default is 1)\n"
            "-n NP: number of to be used ports\n"
            "-s MAC: Source MAC address presented in XX:XX:XX:XX:XX:XX format\n"
            "-d MAC: Destination MAC address presented in XX:XX:XX:XX:XX:XX "
            "format\n"
            "-i "
            "max_poll_short_try,poll_short_interval_us,poll_long_interval_us\n"
            "	Comma split numbers for rx polling try number and intervals(in "
            "microseconds).\n"
            "   For example it can be 10,10,1000:"
            "--[no-]mac-updating: Enable or disable MAC addresses updating "
            "(enabled by default)\n"
            "	When enabled:\n"
            "       - The source MAC address is replaced by the TX port MAC "
            "address\n"
            "       - The destination MAC address is replaced by the MAC "
            "provided by -d option\n"
            "--[no-]packet-capturing: Enable or disable packet capturing "
            "(disabled by default)\n"
            "   When enabled:\n"
            "		- The the pdump capture framework is initialized, the "
            "packets can be captured by official pdump-tool\n"
            "--[no-]debugging: Enable or disable debugging mode (disabled by "
            "default)\n"
            "	When enabled:\n"
            "		- The logging level is set to DEBUG and additional "
            "debug variables are created. (May slow down the program)\n",
            prgname);
}

static int l2fwd_parse_portmask(const char* portmask)
{
        char* end = NULL;
        unsigned long pm;

        /* parse hexadecimal string */
        pm = strtoul(portmask, &end, 16);
        if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
                return -1;

        if (pm == 0)
                return -1;

        return pm;
}

static unsigned int l2fwd_parse_nqueue(const char* q_arg)
{
        char* end = NULL;
        unsigned long n;

        /* parse hexadecimal string */
        n = strtoul(q_arg, &end, 10);
        if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
                return 0;
        if (n == 0)
                return 0;
        if (n >= MAX_RX_QUEUE_PER_LCORE)
                return 0;

        return n;
}

static const char short_options[]
    = "p:" /* portmask */
      "q:" /* number of queues */
      "s:" /* source MAC address */
      "d:" /* destination MAC address */
      "i:" /* rx polling parameters */
      "o:" /* UDP processing operation flag */
      "b:" /* maximal number of burst packets */
      "t:" /* period for draining the tx queue in us */
      "n:" /* number of ports */
    ;

#define CMD_LINE_OPT_MAC_UPDATING "mac-updating"
#define CMD_LINE_OPT_NO_MAC_UPDATING "no-mac-updating"
#define CMD_LINE_OPT_PACKET_CAPTURING "packet-capturing"
#define CMD_LINE_OPT_NO_PACKET_CAPTURING "no-packet-capturing"
#define CMD_LINE_OPT_DEBUGGING "debugging"
#define CMD_LINE_OPT_NO_DEBUGGING "no-debugging"
#define CMD_LINE_OPT_KNI_MODE "kni-mode"
#define CMD_LINE_OPT_NO_KNI_MODE "no-kni-mode"
#define CMD_LINE_OPT_FILTERING "filtering"
#define CMD_LINE_OPT_NO_FILTERING "no-filtering"

enum {
        /* long options mapped to a short option */

        /* first long only option value must be >= 256, so that we won't
         * conflict with short options */
        CMD_LINE_OPT_MIN_NUM = 256,
};

static const struct option lgopts[] = { { CMD_LINE_OPT_MAC_UPDATING,
                                            no_argument, &mac_updating, 1 },
        { CMD_LINE_OPT_NO_MAC_UPDATING, no_argument, &mac_updating, 0 },
        { CMD_LINE_OPT_PACKET_CAPTURING, no_argument, &packet_capturing, 1 },
        { CMD_LINE_OPT_NO_PACKET_CAPTURING, no_argument, &packet_capturing, 0 },
        { CMD_LINE_OPT_DEBUGGING, no_argument, &debugging, 1 },
        { CMD_LINE_OPT_NO_DEBUGGING, no_argument, &debugging, 0 },
        { CMD_LINE_OPT_KNI_MODE, no_argument, &kni_mode, 1 },
        { CMD_LINE_OPT_NO_KNI_MODE, no_argument, &kni_mode, 0 },
        { CMD_LINE_OPT_FILTERING, no_argument, &filtering, 1 },
        { CMD_LINE_OPT_NO_FILTERING, no_argument, &filtering, 0 },
        { NULL, 0, 0, 0 } };

/* Parse the argument given in the command line of the application */
static int udp_nc_parse_args(int argc, char** argv)
{
        int opt, ret, i;
        char** argvopt;
        int option_index;
        char* prgname = argv[0];
        char* end;
        end = NULL;

        argvopt = argv;

        while ((opt = getopt_long(
                    argc, argvopt, short_options, lgopts, &option_index))
            != EOF) {

                switch (opt) {
                /* portmask */
                case 'p':
                        l2fwd_enabled_port_mask = l2fwd_parse_portmask(optarg);
                        if (l2fwd_enabled_port_mask == 0) {
                                printf("Invalid portmask\n");
                                l2fwd_usage(prgname);
                                return -1;
                        }
                        break;

                        /* nqueue */
                case 'q':
                        l2fwd_rx_queue_per_lcore = l2fwd_parse_nqueue(optarg);
                        if (l2fwd_rx_queue_per_lcore == 0) {
                                printf("Invalid queue number\n");
                                l2fwd_usage(prgname);
                                return -1;
                        }
                        break;

                case 'd':
                        sscanf(optarg, "%02x:%02x:%02x:%02x:%02x:%02x",
                            &dst_mac[0], &dst_mac[1], &dst_mac[2], &dst_mac[3],
                            &dst_mac[4], &dst_mac[5]);
                        for (i = 0; i < ETHER_ADDR_LEN; ++i) {
                                dst_mac_addr.addr_bytes[i]
                                    = (uint8_t)dst_mac[i];
                        }
                        break;

                case 's':
                        sscanf(optarg, "%02x:%02x:%02x:%02x:%02x:%02x",
                            &src_mac[0], &src_mac[1], &src_mac[2], &src_mac[3],
                            &src_mac[4], &src_mac[5]);
                        for (i = 0; i < ETHER_ADDR_LEN; ++i) {
                                src_mac_addr.addr_bytes[i]
                                    = (uint8_t)src_mac[i];
                        }
                        break;

                case 'i':
                        sscanf(optarg, "%d,%d,%d", &max_poll_short_try,
                            &poll_short_interval_us, &poll_long_interval_us);
                        break;

                case 'o':
                        coder_type = strtol(optarg, &end, 10);
                        break;

                case 'b':
                        max_pkt_burst = strtol(optarg, &end, 10);
                        break;

                case 't':
                        burst_tx_drain_us = strtol(optarg, &end, 10);
                        break;

                case 'n':
                        nb_ports = strtoul(optarg, &end, 10);
                        if (nb_ports == 0) {
                                printf("Invalid port number!\n");
                                l2fwd_usage(prgname);
                                return -1;
                        }
                        break;

                        /* long options */
                case 0:
                        break;

                default:
                        l2fwd_usage(prgname);
                        return -1;
                }
        }

        if (optind >= 0)
                argv[optind - 1] = prgname;

        ret = optind - 1;
        optind = 1; /* reset getopt lib */
        return ret;
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void check_all_ports_link_status(uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90  /* 9s (90 * 100ms) in total */
        uint16_t portid;
        uint8_t count, all_ports_up, print_flag = 0;
        struct rte_eth_link link;

        printf("\nChecking link status");
        fflush(stdout);
        for (count = 0; count <= MAX_CHECK_TIME; count++) {
                if (force_quit)
                        return;
                all_ports_up = 1;
                RTE_ETH_FOREACH_DEV(portid)
                {
                        if (force_quit)
                                return;
                        if ((port_mask & (1 << portid)) == 0)
                                continue;
                        memset(&link, 0, sizeof(link));
                        rte_eth_link_get_nowait(portid, &link);
                        /* print link status if flag set */
                        if (print_flag == 1) {
                                if (link.link_status)
                                        printf("Port%d Link Up. Speed %u Mbps "
                                               "- %s\n",
                                            portid, link.link_speed,
                                            (link.link_duplex
                                                == ETH_LINK_FULL_DUPLEX)
                                                ? ("full-duplex")
                                                : ("half-duplex\n"));
                                else
                                        printf("Port %d Link Down\n", portid);
                                continue;
                        }
                        /* clear all_ports_up flag if any link down */
                        if (link.link_status == ETH_LINK_DOWN) {
                                all_ports_up = 0;
                                break;
                        }
                }
                /* after finally printing all link status, get out */
                if (print_flag == 1)
                        break;

                if (all_ports_up == 0) {
                        printf(".");
                        fflush(stdout);
                        rte_delay_ms(CHECK_INTERVAL);
                }

                /* set the print_flag if all ports up or timeout */
                if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
                        print_flag = 1;
                        printf("done\n");
                }
        }
}

static void signal_handler(int signum)
{
        if (signum == SIGINT || signum == SIGTERM) {
                printf(
                    "\n\nSignal %d received, preparing to exit...\n", signum);
                force_quit = true;
                if (kni_mode) {
                        rte_atomic32_inc(&kni_stop);
                }
                return;
        }
}

static int kni_config_network_interface(uint16_t portid, uint8_t if_up)
{
        int ret = 0;

        RTE_LOG(INFO, USER1, "Configure network interface of %d %s\n", portid,
            if_up ? "up" : "down");

        if (if_up != 0) {
                rte_eth_dev_stop(portid);
                ret = rte_eth_dev_start(portid);
                rte_atomic32_inc(&kni_dev_up);
                RTE_LOG(INFO, USER1, "Current num of up KNI device: %d\n",
                    rte_atomic32_read(&kni_dev_up));
        } else {
                rte_eth_dev_stop(portid);
        }

        return ret;
}

static int kni_config_mac_address(uint16_t portid, uint8_t mac_addr[])
{
        int ret = 0;
        ret = rte_eth_dev_default_mac_addr_set(
            portid, (struct ether_addr*)mac_addr);

        return ret;
}

/**
 * @brief Allocate KNI devices
 *
 *        To make it simple, allocate only one KNI device for each port
 *
 * @return
 */
static int kni_alloc(uint16_t portid)
{
        struct rte_kni* kni;
        struct kni_port_params** p = kni_port_params_array;
        struct rte_kni_conf conf;

        /* Create one KNI device */
        memset(&conf, 0, sizeof(conf));
        snprintf(conf.name, RTE_KNI_NAMESIZE, "vEth%u", portid);
        conf.core_id = p[portid]->lcore_k[0];
        conf.group_id = portid;
        conf.mbuf_size = NC_MAX_DATA_LEN + NC_MAX_HDR_LEN;
        conf.force_bind = 1;

        /* The first KNI device associated to a port is the master */
        struct rte_kni_ops ops;
        struct rte_eth_dev_info dev_info;

        memset(&dev_info, 0, sizeof(dev_info));
        rte_eth_dev_info_get(
            portid, &dev_info); // Get contextual info of an Eth device

        if (dev_info.pci_dev) {
                conf.addr = dev_info.pci_dev->addr;
                conf.id = dev_info.pci_dev->id;
        }

        rte_eth_macaddr_get(portid, (struct ether_addr*)&conf.mac_addr);
        rte_eth_dev_get_mtu(portid, &conf.mtu);

        memset(&ops, 0, sizeof(ops));
        ops.port_id = portid;
        // Ifce configuration callbacks can be added in the ops
        /*ops.change_mtu = kni_change_mtu;*/
        ops.config_network_if = kni_config_network_interface;
        ops.config_mac_address = kni_config_mac_address;

        kni = rte_kni_alloc(l2fwd_pktmbuf_pool, &conf, &ops);
        p[portid]->kni[0] = kni;

        return 0;
}

/**
 * @brief Free KNI subsystem resources
 *
 */
static int kni_free_kni(uint16_t portid)
{
        uint8_t i;
        struct kni_port_params** p = kni_port_params_array;

        for (i = 0; i < p[portid]->nb_kni; ++i) {
                if (rte_kni_release(p[portid]->kni[i])) {
                        RTE_LOG(INFO, USER1, "Failed to release KNI\n");
                }
                p[portid]->kni[i] = NULL;
        }

        return 0;
}

static void clean_up(void)
{
        uint16_t portid;
        uint16_t i;

        RTE_LOG(INFO, EAL, "Run cleanups.\n");

        if (kni_mode) {
                RTE_LOG(INFO, USER1, "Release KNI resources\n");
                RTE_ETH_FOREACH_DEV(portid) { kni_free_kni(portid); }
                for (i = 0; i < nb_ports; ++i) {
                        if (kni_port_params_array[i]) {
                                rte_free(kni_port_params_array[i]);
                                kni_port_params_array[i] = NULL;
                        }
                }
        }

        RTE_ETH_FOREACH_DEV(portid)
        {
                if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
                        continue;
                RTE_LOG(INFO, USER1, "Closing port %d...", portid);
                rte_eth_dev_stop(portid);
                rte_eth_dev_close(portid);
                RTE_LOG(INFO, USER1, " Done\n");
        }

        if (packet_capturing == 1) {
                rte_pdump_uninit();
        }

        switch (coder_type) {
        case 0:
                nck_free(&enc);
                break;
        case 1:
                nck_free(&dec);
                break;
        case 2:
                nck_free(&rec);
                break;

        case -1:
                break;
        }

        rte_eal_cleanup();
}

int main(int argc, char** argv)
{
        struct lcore_queue_conf* qconf;
        int ret;
        uint16_t portid, last_port;
        unsigned lcore_id, rx_lcore_id;
        unsigned nb_ports_in_mask = 0;
        unsigned int nb_lcores = 0;
        unsigned int nb_mbufs;
        size_t i;

        ret = rte_eal_init(argc, argv);
        if (ret < 0) {
                rte_eal_cleanup();
                rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
        }
        if (rte_lcore_count() < 1 || rte_lcore_count() > 2) {
                rte_eal_cleanup();
                rte_exit(EXIT_FAILURE,
                    "Invalid numbers of lcores. Support 1-2 cores\n");
        }
        argc -= ret;
        argv += ret;

        force_quit = false;
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        /* Parse application arguments (after the EAL ones) */
        ret = udp_nc_parse_args(argc, argv);
        if (ret < 0) {
                rte_eal_cleanup();
                rte_exit(EXIT_FAILURE, "Invalid L2FWD arguments\n");
        }

        if (nb_ports != 2) {
                rte_eal_cleanup();
                rte_exit(EXIT_FAILURE,
                    "Invalid number of ports. Support exactly 2 ports\n");
        }

        if (filtering && kni_mode) {
                rte_eal_cleanup();
                rte_exit(EXIT_FAILURE,
                    "KNI mode does not support filtering function.\n");
        }

        RTE_LOG(INFO, USER1, "DEBUG mode: %s\n",
            debugging ? "enabled" : "disabled");
        if (debugging) {
                /* init logger */
                rte_log_set_global_level(RTE_LOG_DEBUG);
                rte_log_set_level(RTE_LOGTYPE_USER1, RTE_LOG_DEBUG);
        } else {
                rte_log_set_global_level(RTE_LOG_INFO);
                rte_log_set_level(RTE_LOGTYPE_USER1, RTE_LOG_INFO);
        }

        RTE_LOG(INFO, USER1, "MAC updating: %s\n",
            mac_updating ? "enabled" : "disabled");

        RTE_LOG(INFO, USER1, "Eth frame filtering: %s\n",
            filtering ? "enabled" : "disabled");

        if (mac_updating) {
                RTE_LOG(INFO, USER1,
                    "Source MAC address: %02x:%02x:%02x:%02x:%02x:%02x\n",
                    src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4],
                    src_mac[5]);
                RTE_LOG(INFO, USER1,
                    "Destination MAC address: %02x:%02x:%02x:%02x:%02x:%02x\n",
                    dst_mac[0], dst_mac[1], dst_mac[2], dst_mac[3], dst_mac[4],
                    dst_mac[5]);
        }

        /* TODO:  <03-09-18, Zuo> Add this in config parser */
        RTE_LOG(INFO, USER1,
            "[WARN] KNI mode related IP modifications are hard-coded, remember "
            "to modify the IP address before compiling\n");
        inet_pton(AF_INET, "10.0.0.11", &vnf_recv_ip_dst_addr);
        vnf_recv_dst_ip = vnf_recv_ip_dst_addr.s_addr;
        inet_pton(AF_INET, "10.0.0.13", &vnf_send_src_ip_addr);
        vnf_send_src_ip = vnf_send_src_ip_addr.s_addr;
        inet_pton(AF_INET, "10.0.0.14", &vnf_send_dst_ip_addr);
        vnf_send_dst_ip = vnf_send_dst_ip_addr.s_addr;

        RTE_LOG(INFO, USER1, "Packet capturing: %s\n",
            packet_capturing ? "enabled" : "disabled");
        /* Initialise the pdump framework */
        if (packet_capturing) {
                /* Initialise the pdump framework */
                ret = rte_pdump_init(NULL);
                if (ret < 0)
                        rte_exit(EXIT_FAILURE,
                            "Can not initialize the pdump framework.");
        }

        RTE_LOG(INFO, USER1,
            "RX polling parameters: max_poll_short_try:%d, "
            "poll_short_interval_us:%d, poll_long_interval_us:%d\n",
            max_poll_short_try, poll_short_interval_us, poll_long_interval_us);

        RTE_LOG(INFO, USER1, "Number of to be used ports: %d\n", nb_ports);

        RTE_LOG(INFO, USER1, "NCKernel options: \n");
        RTE_LOG(INFO, USER1,
            "Protocol: %s, Symbol size: %s, Symbols: %s, Redundancy:%s\n",
            PROTOCOL, SYMBOL_SIZE, SYMBOLS, REDUNDANCY);
        struct nck_option_value options[]
            = { { "field", FIELD }, { "protocol", PROTOCOL },
                      { "symbol_size", SYMBOL_SIZE }, { "symbols", SYMBOLS },
                      { "redundancy", REDUNDANCY }, { NULL, NULL } };

        switch (coder_type) {
        case 0:
                RTE_LOG(INFO, USER1, "Coder type: NC encoder.\n");

                if (nck_create_encoder(
                        &enc, NULL, options, nck_option_from_array)) {
                        rte_exit(EXIT_FAILURE, "Failed to create encoder.\n");
                }
                break;
        case 1:
                RTE_LOG(INFO, USER1, "Coder type: NC decoder.\n");
                if (nck_create_decoder(
                        &dec, NULL, options, nck_option_from_array)) {
                        rte_exit(EXIT_FAILURE, "Failed to create decoder.\n");
                }
                break;
        case 2:
                RTE_LOG(INFO, USER1, "Coder type: NC recoder.\n");
                if (nck_create_recoder(
                        &rec, NULL, options, nck_option_from_array)) {
                        rte_exit(EXIT_FAILURE, "Failed to create recoder.\n");
                }
                break;

        case -1:
                RTE_LOG(INFO, USER1, "[WARN] Simple Forwarding.\n");
                break;

        default:
                rte_exit(EXIT_FAILURE, "Unknown coder type.\n");
        }

        RTE_LOG(INFO, USER1, "Maximal number of burst packets: %d\n",
            max_pkt_burst);

        RTE_LOG(
            INFO, USER1, "Drain tx queue period: %d us\n", burst_tx_drain_us);

        /* check port mask to possible port mask */
        if (l2fwd_enabled_port_mask & ~((1 << nb_ports) - 1))
                rte_exit(EXIT_FAILURE, "Invalid port mask; possible (0x%x)\n",
                    (1 << nb_ports) - 1);

        for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++)
                l2fwd_dst_ports[portid] = 0;

        last_port = 0;
        RTE_ETH_FOREACH_DEV(portid)
        {
                /* skip ports that are not enabled */
                if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
                        continue;

                if (nb_ports_in_mask % 2) {
                        l2fwd_dst_ports[portid] = last_port;
                        l2fwd_dst_ports[last_port] = portid;
                } else {
                        last_port = portid;
                }

                nb_ports_in_mask++;
        }

        if (nb_ports_in_mask % 2) {
                l2fwd_dst_ports[last_port] = last_port;
        }

        rx_lcore_id = 0;
        qconf = NULL;

        /* Initialize the port/queue configuration of each logical core */
        RTE_ETH_FOREACH_DEV(portid)
        {
                /* skip ports that are not enabled */
                if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
                        continue;

                /* get the lcore_id for this port */
                while (rte_lcore_is_enabled(rx_lcore_id) == 0
                    || lcore_queue_conf[rx_lcore_id].n_rx_port
                        == l2fwd_rx_queue_per_lcore) {
                        rx_lcore_id++;
                        if (rx_lcore_id >= RTE_MAX_LCORE)
                                rte_exit(EXIT_FAILURE, "Not enough cores\n");
                }

                if (qconf != &lcore_queue_conf[rx_lcore_id]) {
                        /* Assigned a new logical core in the loop above. */
                        qconf = &lcore_queue_conf[rx_lcore_id];
                        nb_lcores++;
                }

                qconf->rx_port_list[qconf->n_rx_port] = portid;
                qconf->n_rx_port++;
                RTE_LOG(
                    INFO, USER1, "Lcore %u: RX port %u\n", rx_lcore_id, portid);
        }

        /* Create mbuf pool for store packets */
        nb_mbufs = RTE_MAX(nb_ports
                * (nb_rxd + nb_txd + max_pkt_burst
                      + nb_lcores * MEMPOOL_CACHE_SIZE),
            8192U);
        RTE_LOG(INFO, USER1, "Number of mbufs: %u\n", nb_mbufs);
        l2fwd_pktmbuf_pool = rte_pktmbuf_pool_create("l2fwd_mbuf_pool",
            nb_mbufs, MEMPOOL_CACHE_SIZE, 0, NC_MAX_DATA_LEN + NC_MAX_HDR_LEN,
            rte_socket_id());

        if (l2fwd_pktmbuf_pool == NULL) {
                rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");
        }

        if (coder_type == 0) {
                check_mbuf_size(l2fwd_pktmbuf_pool, &enc, NC_MAX_HDR_LEN);
        }

        /* Init KNI Subsystem */
        RTE_LOG(
            INFO, USER1, "KNI mode: %s\n", kni_mode ? "enabled" : "disabled");
        if (kni_mode) {
                RTE_LOG(INFO, USER1,
                    "[WARN] KNI mode is ONLY used for comparison. The code is "
                    "not stable\n");
                RTE_LOG(INFO, USER1,
                    "[WARN] Some LOG messages are not correct if KNI mode "
                    "is enabled. Check the messages starting with [KNI]\n");
                nb_used_lcores = rte_lcore_count();
                RTE_LOG(INFO, USER1, "Number of to be used lcores: %u\n",
                    nb_used_lcores);
                RTE_LOG(INFO, USER1,
                    "[KNI] Multi-core affinity mechanism: one-core for RX and "
                    "one-core for TX\n");
                rte_kni_init(nb_ports);
                RTE_LOG(INFO, USER1,
                    "[KNI] Preallocate %d KNI interfaces, one interface per "
                    "port.\n",
                    nb_ports);

                memset(
                    &kni_port_params_array, 0, sizeof(kni_port_params_array));
                RTE_ETH_FOREACH_DEV(portid)
                {
                        kni_port_params_array[portid] = rte_zmalloc(
                            "KNI_port_params", sizeof(struct kni_port_params),
                            RTE_CACHE_LINE_SIZE);
                        kni_port_params_array[portid]->port_id = portid;
                        kni_port_params_array[portid]->nb_kni = 1;

                        if (rte_lcore_count() == 1) {
                                for (i = 0; i < KNI_MAX_KTHREAD; ++i) {
                                        kni_port_params_array[portid]
                                            ->lcore_k[i]
                                            = 0;
                                }
                        } else {
                                for (i = 0; i < KNI_MAX_KTHREAD; ++i) {
                                        kni_port_params_array[portid]
                                            ->lcore_k[i]
                                            = portid;
                                }
                        }

                        kni_port_params_array[portid]->nb_lcore_k = 1;
                }
        }

        /* Configure enabled DPDK ports */
        RTE_ETH_FOREACH_DEV(portid)
        {
                struct rte_eth_rxconf rxq_conf;
                struct rte_eth_txconf txq_conf;
                struct rte_eth_conf local_port_conf = port_conf;
                struct rte_eth_dev_info dev_info;

                if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) {
                        RTE_LOG(
                            INFO, USER1, "Skipping disabled port %u\n", portid);
                        continue;
                }

                RTE_LOG(INFO, USER1, "Initializing port %u... \n", portid);
                fflush(stdout);
                rte_eth_dev_info_get(portid, &dev_info);
                /* Enable TX offloading if device supports it */
                if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
                        local_port_conf.txmode.offloads
                            |= DEV_TX_OFFLOAD_MBUF_FAST_FREE;
                ret = rte_eth_dev_configure(portid, 1, 1, &local_port_conf);
                if (ret < 0) {
                        rte_exit(EXIT_FAILURE,
                            "Cannot configure device: err=%d, port=%u\n", ret,
                            portid);
                }

                ret = rte_eth_dev_adjust_nb_rx_tx_desc(
                    portid, &nb_rxd, &nb_txd);
                if (ret < 0) {
                        rte_exit(EXIT_FAILURE,
                            "Cannot adjust number of descriptors: err=%d, "
                            "port=%u\n",
                            ret, portid);
                }

                /* [Zuo] Get MAC addresses of all enabled port */
                rte_eth_macaddr_get(portid, &l2fwd_ports_eth_addr[portid]);

                fflush(stdout);
                rxq_conf = dev_info.default_rxconf;
                rxq_conf.offloads = local_port_conf.rxmode.offloads;
                ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
                    rte_eth_dev_socket_id(portid), &rxq_conf,
                    l2fwd_pktmbuf_pool);
                if (ret < 0) {
                        rte_exit(EXIT_FAILURE,
                            "rte_eth_rx_queue_setup:err=%d, port=%u\n", ret,
                            portid);
                }

                fflush(stdout);
                txq_conf = dev_info.default_txconf;
                txq_conf.txq_flags = ETH_TXQ_FLAGS_IGNORE;
                txq_conf.offloads = local_port_conf.txmode.offloads;
                ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
                    rte_eth_dev_socket_id(portid), &txq_conf);
                if (ret < 0) {
                        rte_exit(EXIT_FAILURE,
                            "rte_eth_tx_queue_setup:err=%d, port=%u\n", ret,
                            portid);
                }

                /* Initialize TX buffers */
                tx_buffer[portid] = rte_zmalloc_socket("tx_buffer",
                    RTE_ETH_TX_BUFFER_SIZE(max_pkt_burst), 0,
                    rte_eth_dev_socket_id(portid));
                if (tx_buffer[portid] == NULL)
                        rte_exit(EXIT_FAILURE,
                            "Cannot allocate buffer for tx on port %u\n",
                            portid);

                rte_eth_tx_buffer_init(tx_buffer[portid], max_pkt_burst);

                /* Start device */
                ret = rte_eth_dev_start(portid);
                if (ret < 0) {
                        rte_exit(EXIT_FAILURE,
                            "rte_eth_dev_start:err=%d, port=%u\n", ret, portid);
                }
                RTE_LOG(INFO, USER1, "Device started for port: %d\n", portid);

                /* MARK: Enable promiscuous mode in each enabled port */
                /*rte_eth_promiscuous_enable(portid);*/

                RTE_LOG(INFO, USER1,
                    "Port %u, MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n\n",
                    portid, l2fwd_ports_eth_addr[portid].addr_bytes[0],
                    l2fwd_ports_eth_addr[portid].addr_bytes[1],
                    l2fwd_ports_eth_addr[portid].addr_bytes[2],
                    l2fwd_ports_eth_addr[portid].addr_bytes[3],
                    l2fwd_ports_eth_addr[portid].addr_bytes[4],
                    l2fwd_ports_eth_addr[portid].addr_bytes[5]);

                if (kni_mode) {
                        kni_alloc(portid);
                }
        }

        check_all_ports_link_status(l2fwd_enabled_port_mask);

        ret = 0;
        /* Launch per-lcore init and operation function on every lcore
         * These functions run into main loops
         * */
        rte_eal_mp_remote_launch(l2fwd_launch_one_lcore, NULL, CALL_MASTER);
        RTE_LCORE_FOREACH_SLAVE(lcore_id)
        {
                if (rte_eal_wait_lcore(lcore_id) < 0) {
                        ret = -1;
                        break;
                }
        }

        clean_up();
        RTE_LOG(INFO, EAL, "App exits.\n");
        return ret;
}
