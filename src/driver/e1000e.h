#ifndef IXY_E1000E_H
#define IXY_E1000E_H

#include "stats.h"

static const char* driver_name = "ixy-e1000e";

static const int MAX_RX_QUEUE_ENTRIES = 4096;
static const int MAX_TX_QUEUE_ENTRIES = 4096;

static const int NUM_RX_QUEUE_ENTRIES = 512;
static const int NUM_TX_QUEUE_ENTRIES = 512;

static const int PKT_BUF_ENTRY_SIZE = 2048;
static const int MIN_MEMPOOL_ENTRIES = 4096;

static const int TX_CLEAN_BATCH = 32;

static const uint64_t INTERRUPT_INITIAL_INTERVAL = 1000 * 1000 * 1000;

struct e1000e_device {
    struct ixy_device ixy;
    uint8_t* addr;
    void* rx_queues;
    void* tx_queues;
};

#define IXY_TO_E1000E(ixy_device) container_of(ixy_device, struct e1000e_device, ixy)

struct ixy_device* e1000e_init(const char* pci_addr, uint16_t rx_queues, uint16_t tx_queues, int interrupt_timeout);
uint32_t e1000e_get_link_speed(const struct ixy_device* ixy);
struct mac_address e1000e_get_mac_addr(const struct ixy_device* ixy);
void e1000e_set_mac_addr(struct ixy_device* ixy, struct mac_address mac);
void e1000e_set_promisc(struct ixy_device* ixy, bool enabled);
void e1000e_read_stats(struct ixy_device* ixy, struct device_stats* stats);
uint32_t e1000e_tx_batch(struct ixy_device* ixy, uint16_t queue_id, struct pkt_buf* bufs[], uint32_t num_bufs);
uint32_t e1000e_rx_batch(struct ixy_device* ixy, uint16_t queue_id, struct pkt_buf* bufs[], uint32_t num_bufs);

#endif // IXY_E1000E_H
