

#include "log.h"
#include "e1000e.h"
#include "driver/e1000e_type.h"
#include "pci.h"
#include "memory.h"

// allocate for each rx queue, keep state for the receive function
struct e1000e_rx_queue {
    volatile struct e1000_rx_desc* descriptors;
    struct mempool* mempool;
    uint16_t num_entries;
	// position we are reading from
	uint16_t rx_index;
	// virtual addresses to map descriptors back to their mbuf for freeing
	void* virtual_addresses[];
};

// allocated for each tx queue, keeps state for the transmit function
struct e1000e_tx_queue {
	volatile struct e1000_tx_desc* descriptors;
	uint16_t num_entries;
	// position to clean up descriptors that where sent out by the nic
	uint16_t clean_index;
	// position to insert packets for transmission
	uint16_t tx_index;
	// virtual addresses to map descriptors back to their mbuf for freeing
	void* virtual_addresses[];
};

static void clear_interrupts(struct e1000e_device* dev) {
    info("Clear device interrupts %s", dev->ixy.pci_addr);
    set_reg32(dev->addr, E1000_IMC, 0xFFFFFFFF);
    get_reg32(dev->addr, E1000_ICR);
}

static void clear_interrupt(struct e1000e_device* dev, uint16_t queue_id) {
    info("Claer queue %d device interrupts", queue_id);
    set_reg32(dev->addr, E1000_IMC, 1 << queue_id);
    get_reg32(dev->addr, E1000_ICR);
}

static void disable_interrupts(struct e1000e_device* dev) {
    info("Disable device interrupts %s", dev->ixy.pci_addr);
    set_reg32(dev->addr, E1000_IMS, 0x00000000);
    clear_interrupts(dev);
}

static void disable_interrupt(struct e1000e_device* dev, uint16_t queue_id) {
	// Clear interrupt mask to stop from interrupts being generated
	uint32_t mask = get_reg32(dev->addr, E1000_IMS);
	mask &= ~(1 << queue_id);
	set_reg32(dev->addr, E1000_IMS, mask);
	clear_interrupt(dev, queue_id);

	debug("Using polling");
}

// see section 4.6.3
static void init_link(struct e1000e_device* dev) {
   // reset PHY, see section 3.2.1.2
   // also, refer to e1000_phy_hw_reset_generic in dpdk e1000_phy.c 
   uint32_t ctrl;
   ctrl = get_reg32(dev->addr, E1000_CTL);
   ctrl |= E1000_CTL_PHY_RST;
   set_reg32(dev->addr, E1000_CTL, ctrl);
   usleep(10000);

   // link setup, see section 4.6.3.2, the prefered way is that MAC settings are solved by PHY status
   // also, refer to e1000_setup_copper_link_82571 in dpdk e1000_82571.c
   // FRCSPD: force speed
   // FRCDPLX: for duplex
   // ASDE: auto-speed detection enable
   ctrl = get_reg32(dev->addr, E1000_CTL);
   ctrl |= E1000_CTL_SLU; // enable communication between MAC and PHY

   // ASDE is not set to 0 in dpdk implementation
   ctrl &= ~(E1000_CTL_FRCSPD | E1000_CTL_FRCDPLX);
   set_reg32(dev->addr, E1000_CTL, ctrl);
}

uint32_t e1000e_get_link_speed(const struct ixy_device* ixy) {
	struct e1000e_device* dev = IXY_TO_E1000E(ixy);
	uint32_t status = get_reg32(dev->addr, E1000_STATUS);

	if (!(status & E1000_STATUS_LU)) {
		return 0;
	}

	int  value = (status & E1000_LINKSPEED) >> 6;
	switch (value) {
		case E1000_1000Mbps:
			return 1000;
		case E1000_100Mbps:
			return 100;
		case E1000_10Mbps:
			return 10;
		default:
			return 0;
  }
  return 0;
}

// see section 4.6.5
static void init_rx(struct e1000e_device* dev) {
    /*
        1. set MAC (x)
        2. set MTA to receive broadcast packets (x)
        3. set interrupt (x)
            1. RXT: when get the packets
            2. RXO: when RX FIFO overflow
            3. RXDMT: when number of RX descriptor is below than threashold
            4. LSC: when link status change
        4. set RCTL (receive control register)
            open receiver after set up descriptor ring buffer (receive queue)
    */

    // disable receiver
    clear_flags32(dev->addr, E1000_RCTL, E1000_RCTL_EN);

    // enable CRC offloading
    set_flags32(dev->addr, E1000_RCTL, E1000_RCTL_SECRC);

    // disable VLAN filter
    clear_flags32(dev->addr, E1000_RCTL, E1000_RCTL_VFE);

    /*
        see section 4.6.5.1, setup receive queue
        The following should be done once per receive queue:
        • Allocate a region of memory for the receive descriptor list.
        • Receive buffers of appropriate size should be allocated and pointers to these
        buffers should be stored in the descriptor ring.
        • Program the descriptor base address with the address of the region.
        • Set the length register to the size of the descriptor ring.
        • If needed, program the head and tail registers. Note: the head and tail pointers are
        initialized (by hardware) to zero after a power-on or a software-initiated device
        reset.
        • The tail pointer should be set to point one descriptor beyond the end.
    */
    for (uint16_t i = 0; i < dev->ixy.num_rx_queues; i++) {
        info("initializing rx queeu %d", i);
        /*
            82574 has no advanced feature like SRRCTL（Split and Receive Control Register
            to do fine-grined control, like advanced split header, RSS, DCA, multiple queue
        */

        // allocate a region of memory for receive descriptor
        uint32_t ring_size_bytes = NUM_RX_QUEUE_ENTRIES * sizeof(struct e1000_rx_desc);
        struct dma_memory mem = memory_allocate_dma(ring_size_bytes, true);
        memset(mem.virt, -1, ring_size_bytes);

        // tell the device where it can write to
        set_reg32(dev->addr, E1000_RDBAL(i), (uint32_t) (mem.phy & 0xFFFFFFFFull));
        set_reg32(dev->addr, E1000_RDBAH(i), (uint32_t) (mem.phy >> 32));
        set_reg32(dev->addr, E1000_RDLEN(i), ring_size_bytes);
        debug("rx ring %d phy addr:  0x%012lX", i, mem.phy);
		debug("rx ring %d virt addr: 0x%012lX", i, (uintptr_t) mem.virt);

        // set receive head and receive tail to zero
        set_reg32(dev->addr, E1000_RDH(i), 0); // set receive descriptor head to 0
		set_reg32(dev->addr, E1000_RDT(i), 0); // set receive descritpor tail to 0

        // private data for the driver
        struct e1000e_rx_queue* queue = ((struct e1000e_rx_queue*)(dev->rx_queues)) + i;
        queue->num_entries = NUM_RX_QUEUE_ENTRIES;
        queue->rx_index = 0;
        queue->descriptors = (struct e1000_rx_desc*) mem.virt;
    }

    // open receiver
    set_flags32(dev->addr, E1000_RCTL, E1000_RCTL_EN);
}

// see section 4.6.6
static void init_tx(struct e1000e_device* dev) {
    /*
        The following should be done once per transmit queue:
        • Allocate a region of memory for the transmit descriptor list.
        • Program the descriptor base address with the address of the region.
        • Set the length register to the size of the descriptor ring.
        • If needed, program the head and tail registers.
    */
    for (uint16_t i = 0; i < dev->ixy.num_tx_queues; i++) {
        info("initializing tx queue %d", i);

        uint32_t ring_size_bytes = NUM_TX_QUEUE_ENTRIES * sizeof(struct e1000_tx_desc);
        struct dma_memory mem = memory_allocate_dma(ring_size_bytes, true);
        memset(mem.virt, 0, ring_size_bytes);

        set_reg32(dev->addr, E1000_TDBAL(i), (uint32_t) (mem.phy & 0xFFFFFFFFull));
		set_reg32(dev->addr, E1000_TDBAH(i), (uint32_t) (mem.phy >> 32));
		set_reg32(dev->addr, E1000_TDLEN(i), ring_size_bytes);
        debug("tx ring %d phy addr:  0x%012lX", i, mem.phy);
		debug("tx ring %d virt addr: 0x%012lX", i, (uintptr_t) mem.virt);

        // set receive head and receive tail to zero
        set_reg32(dev->addr, E1000_TDH(i), 0); // set receive descriptor head to 0
		set_reg32(dev->addr, E1000_TDT(i), 0); // set receive descritpor tail to 0

        struct e1000e_tx_queue* queue = ((struct e1000e_tx_queue*)(dev->tx_queues)) + i;
		queue->num_entries = NUM_TX_QUEUE_ENTRIES;
		queue->descriptors = (struct e1000_tx_desc*) mem.virt;
    }

    set_flags32(dev->addr, E1000_TCTL, E1000_TCTL_EN);
}

static void wait_for_link(const struct e1000e_device* dev) {
    info("Waiting for link...");
    int32_t max_wait = 10000000; // 10 seconds in us
    uint32_t poll_interval = 100000; // 10 ms in us
	while (!(e1000e_get_link_speed(&dev->ixy)) && max_wait > 0) {
		usleep(poll_interval);
		max_wait -= poll_interval;
	}
	info("Link speed is %d Mbit/s", e1000e_get_link_speed(&dev->ixy));
}

// see section 4.6
static void reset_and_init(struct e1000e_device* dev) {
    info("Resetting device %s", dev->ixy.pci_addr);

    // 4.6.1
    disable_interrupts(dev);

    // 4.6.2
    // e1000e has no CTRL_RST_MASK like 82599, so use CTRL_RST
    set_flags32(dev->addr, E1000_CTL, E1000_CTL_RST);
    wait_clear_reg32(dev->addr, E1000_CTL, E1000_CTL_RST);
    usleep(10000);

    // 4.6.1
    disable_interrupts(dev);

    struct mac_address mac = get_mac_addr(&dev->ixy);
    info("Initializing device %s", dev->ixy.pci_addr);
	info("MAC address %02x:%02x:%02x:%02x:%02x:%02x", mac.addr[0], mac.addr[1], mac.addr[2], mac.addr[3], mac.addr[4], mac.addr[5]);

    // 4.6.3
    init_link(dev);

    // 4.6,4
    e1000e_read_stats(&dev->ixy, NULL);

    // 4.6.5
    init_rx(dev);

    // 4.6.6
    init_tx(dev);

    // enable receive broadcast and unicast
    e1000e_set_promisc(&dev->ixy, true);

    // wait for some time for the link to come up
    wait_for_link(dev);
    info("Done resetting device %s, ctrl_reg=%x", dev->ixy.pci_addr, get_reg32(dev->addr, E1000_CTL)) ;
}

struct ixy_device* e1000e_init(const char* pci_addr, uint16_t rx_queues, uint16_t tx_queues, int interrupt_timeout) {
    if (getuid()) {
		warn("Not running as root, this will probably fail");
	}
	if (rx_queues > MAX_QUEUES) {
		error("cannot configure %d rx queues: limit is %d", rx_queues, MAX_QUEUES);
	}
	if (tx_queues > MAX_QUEUES) {
		error("cannot configure %d tx queues: limit is %d", tx_queues, MAX_QUEUES);
	}

    struct e1000e_device* dev = (struct e1000e_device*) malloc(sizeof(struct e1000e_device));
    dev->ixy.pci_addr = strdup(pci_addr);

    dev->ixy.driver_name = driver_name;
    dev->ixy.num_rx_queues = rx_queues;
    dev->ixy.num_tx_queues = tx_queues;
    dev->ixy.rx_batch = e1000e_rx_batch;
    dev->ixy.tx_batch = e1000e_tx_batch;
    dev->ixy.read_stats = e1000e_read_stats;
    dev->ixy.set_promisc = e1000e_set_promisc;
    dev->ixy.get_link_speed = e1000e_get_link_speed;
    dev->ixy.get_mac_addr = e1000e_get_mac_addr;
    dev->ixy.set_mac_addr = e1000e_set_mac_addr;
    dev->ixy.interrupts.interrupts_enabled = false;
	dev->ixy.interrupts.itr_rate = 0;
	dev->ixy.interrupts.timeout_ms = 0;

    // todo: add vfio map resource here
    dev->addr = pci_map_resource(pci_addr);

    dev->rx_queues = calloc(rx_queues, sizeof(struct e1000e_rx_queue) + sizeof(void*) * NUM_RX_QUEUE_ENTRIES);
    dev->tx_queues = calloc(tx_queues, sizeof(struct e1000e_tx_queue) + sizeof(void*) * NUM_TX_QUEUE_ENTRIES);
    reset_and_init(dev);
    return &dev->ixy;
}

void e1000e_read_stats(struct ixy_device* ixy, struct device_stats* stats) {
    struct e1000e_device* dev = IXY_TO_E1000E(ixy);

    uint32_t rx_pkts = get_reg32(dev->addr, E1000_GPRC);
    uint32_t tx_pkts = get_reg32(dev->addr, E1000_GPTC);
    uint64_t rx_bytes = get_reg32(dev->addr, E1000_GORCL) + (((uint64_t) get_reg32(dev->addr, E1000_GORCH)) << 32);
    uint64_t tx_bytes = get_reg32(dev->addr, E1000_GOTCL) + (((uint64_t) get_reg32(dev->addr, E1000_GOTCH)) << 32);

    
    if (stats) {
        stats->rx_pkts += rx_pkts;
        stats->tx_pkts += tx_pkts;
        stats->rx_bytes += rx_bytes;
        stats->tx_bytes += tx_bytes;
    }
}

struct mac_address e1000e_get_mac_addr(const struct ixy_device* ixy) {
    struct mac_address mac;
    struct e1000e_device* dev = IXY_TO_E1000E(ixy);

    uint32_t rar_low = get_reg32(dev->addr, E1000_RAL(0));
    uint32_t rar_high = get_reg32(dev->addr, E1000_RAH(0));

    mac.addr[0] = rar_low;
    mac.addr[1] = rar_low >> 8;
    mac.addr[2] = rar_low >> 16;
    mac.addr[3] = rar_low >> 24;
    mac.addr[4] = rar_high;
    mac.addr[5] = rar_high >> 8;

    return mac;
}

void e1000e_set_mac_addr(struct ixy_device* ixy, struct mac_address mac) {
    struct e1000e_device* dev = IXY_TO_E1000E(ixy);

    uint32_t rar_low = mac.addr[0] + (mac.addr[1] << 8) + (mac.addr[2] << 16) + (mac.addr[3] << 24);
    uint32_t rar_high = mac.addr[4] + (mac.addr[5] << 8);

    set_reg32(dev->addr, E1000_RAL(0), rar_low);
    set_reg32(dev->addr, E1000_RAH(0), rar_high);
}

void e1000e_set_promisc(struct ixy_device* ixy, bool enabled) {
	struct e1000e_device* dev = IXY_TO_E1000E(ixy);
	if (enabled) {
		info("enabling promisc mode");
		set_flags32(dev->addr, E1000_RCTL, E1000_RCTL_MPE | E1000_RCTL_UPE);
	} else {
		info("disabling promisc mode");
		clear_flags32(dev->addr, E1000_RCTL, E1000_RCTL_MPE | E1000_RCTL_UPE);
	}
}

// advance index with wrap-around, this line is the reason why we require a power of two for the queue size
#define wrap_ring(index, ring_size) (uint16_t) ((index + 1) & (ring_size - 1))

uint32_t e1000e_rx_batch(struct ixy_device* ixy, uint16_t queue_id, struct pkt_buf* bufs[], uint32_t num_bufs) {
    struct e1000e_device* dev = IXY_TO_E1000E(ixy);
    struct e1000e_rx_queue* queue = ((struct e1000e_rx_queue*) (dev->rx_queues)) + queue_id;
    uint16_t rx_index = queue->rx_index;
    uint16_t last_rx_index = rx_index;
    uint32_t buf_index, status;

    for (buf_index = 0; buf_index < num_bufs; buf_index++) {
        volatile struct e1000_rx_desc* desc_ptr = queue->descriptors + rx_index;
        status = desc_ptr->status;
        if (status & E1000_RXD_STAT_DD) {
            if (!(status & E1000_RXD_STAT_EOP)) {
				error("multi-segment packets are not supported - increase buffer size or decrease MTU");
			}

            struct e1000_rx_desc desc = *desc_ptr;
            struct pkt_buf* buf = (struct pkt_buf*) queue->virtual_addresses[rx_index];
            buf->size = desc.length;

            struct pkt_buf* new_buf = pkt_buf_alloc(queue->mempool);
            if (!new_buf) {
                // we could handle empty mempools more gracefully here, but it would be quite messy...
				// make your mempools large enough
				error("failed to allocate new mbuf for rx, you are either leaking memory or your mempool is too small");
            }

            // reset the descriptor
            desc_ptr->addr = (uint64_t)new_buf->buf_addr_phy + offsetof(struct pkt_buf, data);
            desc_ptr->status = 0;
            queue->virtual_addresses[rx_index] = new_buf;
            bufs[buf_index] = buf;

            last_rx_index = rx_index;
            rx_index = wrap_ring(rx_index, queue->num_entries);
        } else {
            break;
        }
    }
    if (rx_index != last_rx_index) {
        // update the tail pointer to tell the nic the number of descritpror we can use
        set_reg32(dev->addr, E1000_RDT(queue_id), last_rx_index);
        queue->rx_index = rx_index;
    }

    return buf_index;
}

uint32_t e1000e_tx_batch(struct ixy_device* ixy, uint16_t queue_id, struct pkt_buf* bufs[], uint32_t num_bufs) {
    struct e1000e_device* dev = IXY_TO_E1000E(ixy);
    struct e1000e_tx_queue* queue = ((struct e1000e_tx_queue*)(dev->tx_queues)) + queue_id;
    uint16_t clean_index = queue->clean_index;
    uint32_t status, sent, next_index;
    int32_t cleanable, cleanup_to, i;

    // step 1: clean up descriptors that were sent out by the hardware and return them to the mempool
	// start by reading step 2 which is done first for each packet
	// cleaning up must be done in batches for performance reasons, so this is unfortunately somewhat complicated
    while (true) {
        cleanable = queue->tx_index - clean_index;
        if (cleanable < 0) { // handle wrap around
            cleanable = queue->num_entries + cleanable;
        }
        if (cleanable < TX_CLEAN_BATCH) {
            break;
        }

        cleanup_to = clean_index + TX_CLEAN_BATCH - 1;
        if (cleanup_to >= queue->num_entries) {
            cleanup_to -= queue->num_entries;
        }

        /* 
            e1000_adv_tx_desc has more feature than e1000_tx_desc
            e1000_tx_desc is a leagcy format for descriptor
            e1000_adv_tx_desc supports TSO, checksum offload, VLAN tag
        */
        volatile struct e1000_tx_desc* txd = queue->descriptors + cleanup_to;
        status = txd->status;

        // hardware sets this flag as soon as it's sent out, we can give back all bufs in the batch back to the mempool
        if (status & E1000_TXD_STAT_DD) {
            i = clean_index;
            while (true) {
                struct pkt_buf* buf = queue->virtual_addresses[i];
                pkt_buf_free(buf);
                if (i == cleanup_to) {
                    break;
                }
                i = wrap_ring(i, queue->num_entries);
            }

            clean_index = wrap_ring(cleanup_to, queue->num_entries);
        } else {
            break;
        }
    }

    queue->clean_index = clean_index;

    // step 2: send out as many of our packets as possible
    for (sent = 0; sent < num_bufs; sent++) {
        next_index = wrap_ring(queue->tx_index, queue->num_entries);

        if (clean_index == next_index) {
            break;
        }

        struct pkt_buf* buf = bufs[sent];
        queue->virtual_addresses[queue->tx_index] = (void*) buf;
        volatile struct e1000_tx_desc* txd = queue->descriptors + queue->tx_index;
        queue->tx_index = next_index;

        // NIC reads from here
        txd->addr = (uint64_t)buf->buf_addr_phy + offsetof(struct pkt_buf, data);
        txd->length = (uint16_t)buf->size;

        /* Transmit Descriptor bit definitions */
        txd->cmd = 
            E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
    }

    set_reg32(dev->addr, E1000_TDT(queue_id), queue->tx_index);
    return sent;
}
