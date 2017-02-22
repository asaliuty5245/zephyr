/* MCUX Ethernet Driver
 *
 *  Copyright (c) 2016-2017 ARM Ltd
 *  Copyright (c) 2016 Linaro Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Driver Limitations:
 *
 * There is no statistics collection for either normal operation or
 * error behaviour.
 */

#define SYS_LOG_DOMAIN "dev/eth_mcux"
#define SYS_LOG_LEVEL SYS_LOG_LEVEL_DEBUG
#include <logging/sys_log.h>

#include <board.h>
#include <device.h>
#include <misc/util.h>
#include <kernel.h>
#include <net/nbuf.h>
#include <net/net_if.h>

#include "fsl_enet.h"
#include "fsl_phy.h"
#include "fsl_port.h"

enum eth_mcux_phy_state {
	eth_mcux_phy_state_initial,
	eth_mcux_phy_state_reset,
	eth_mcux_phy_state_autoneg,
	eth_mcux_phy_state_restart,
	eth_mcux_phy_state_read_status,
	eth_mcux_phy_state_read_duplex,
	eth_mcux_phy_state_wait
};

struct eth_context {
	struct net_if *iface;
	enet_handle_t enet_handle;
	struct k_sem tx_buf_sem;
	enum eth_mcux_phy_state phy_state;
	bool link_up;
	phy_duplex_t phy_duplex;
	phy_speed_t phy_speed;
	uint8_t mac_addr[6];
	struct k_work phy_work;
	struct k_delayed_work delayed_phy_work;
	/* TODO: FIXME. This Ethernet frame sized buffer is used for
	 * interfacing with MCUX. How it works is that hardware uses
	 * DMA scatter buffers to receive a frame, and then public
	 * MCUX call gathers them into this buffer (there's no other
	 * public interface). All this happens only for this driver
	 * to scatter this buffer again into Zephyr fragment buffers.
	 * This is not efficient, but proper resolution of this issue
	 * depends on introduction of zero-copy networking support
	 * in Zephyr, and adding needed interface to MCUX (or
	 * bypassing it and writing a more complex driver working
	 * directly with hardware).
	 */
	uint8_t frame_buf[1500];
};

static void eth_0_config_func(void);

static enet_rx_bd_struct_t __aligned(ENET_BUFF_ALIGNMENT)
rx_buffer_desc[CONFIG_ETH_MCUX_TX_BUFFERS];

static enet_tx_bd_struct_t __aligned(ENET_BUFF_ALIGNMENT)
tx_buffer_desc[CONFIG_ETH_MCUX_TX_BUFFERS];

/* Use ENET_FRAME_MAX_VALNFRAMELEN for VLAN frame size
 * Use ENET_FRAME_MAX_FRAMELEN for ethernet frame size
 */
#define ETH_MCUX_BUFFER_SIZE \
	ROUND_UP(ENET_FRAME_MAX_VALNFRAMELEN, ENET_BUFF_ALIGNMENT)

static uint8_t __aligned(ENET_BUFF_ALIGNMENT)
rx_buffer[CONFIG_ETH_MCUX_RX_BUFFERS][ETH_MCUX_BUFFER_SIZE];

static uint8_t __aligned(ENET_BUFF_ALIGNMENT)
tx_buffer[CONFIG_ETH_MCUX_TX_BUFFERS][ETH_MCUX_BUFFER_SIZE];

static void eth_mcux_decode_duplex_and_speed(uint32_t status,
					     phy_duplex_t *p_phy_duplex,
					     phy_speed_t *p_phy_speed)
{
	switch (status & PHY_CTL1_SPEEDUPLX_MASK) {
	case PHY_CTL1_10FULLDUPLEX_MASK:
		*p_phy_duplex = kPHY_FullDuplex;
		*p_phy_speed = kPHY_Speed10M;
		break;
	case PHY_CTL1_100FULLDUPLEX_MASK:
		*p_phy_duplex = kPHY_FullDuplex;
		*p_phy_speed = kPHY_Speed100M;
		break;
	case PHY_CTL1_100HALFDUPLEX_MASK:
		*p_phy_duplex = kPHY_HalfDuplex;
		*p_phy_speed = kPHY_Speed100M;
		break;
	case PHY_CTL1_10HALFDUPLEX_MASK:
		*p_phy_duplex = kPHY_HalfDuplex;
		*p_phy_speed = kPHY_Speed10M;
		break;
	}
}

static void eth_mcux_phy_event(struct eth_context *context)
{
	uint32_t status;
	bool link_up;
	phy_duplex_t phy_duplex = kPHY_FullDuplex;
	phy_speed_t phy_speed = kPHY_Speed100M;
	const uint32_t phy_addr = 0;

	SYS_LOG_DBG("phy_state=%d", context->phy_state);

	switch (context->phy_state) {
	case eth_mcux_phy_state_initial:
		/* Reset the PHY. */
		ENET_StartSMIWrite(ENET, phy_addr, PHY_BASICCONTROL_REG,
				   kENET_MiiWriteValidFrame,
				   PHY_BCTL_RESET_MASK);
		context->phy_state = eth_mcux_phy_state_reset;
		break;
	case eth_mcux_phy_state_reset:
		/* Setup PHY autonegotiation. */
		ENET_StartSMIWrite(ENET, phy_addr, PHY_AUTONEG_ADVERTISE_REG,
				   kENET_MiiWriteValidFrame,
				   (PHY_100BASETX_FULLDUPLEX_MASK |
				    PHY_100BASETX_HALFDUPLEX_MASK |
				    PHY_10BASETX_FULLDUPLEX_MASK |
				    PHY_10BASETX_HALFDUPLEX_MASK | 0x1U));
		context->phy_state = eth_mcux_phy_state_autoneg;
		break;
	case eth_mcux_phy_state_autoneg:
		/* Setup PHY autonegotiation. */
		ENET_StartSMIWrite(ENET, phy_addr, PHY_BASICCONTROL_REG,
				   kENET_MiiWriteValidFrame,
				   (PHY_BCTL_AUTONEG_MASK |
				    PHY_BCTL_RESTART_AUTONEG_MASK));
		context->phy_state = eth_mcux_phy_state_restart;
		break;
	case eth_mcux_phy_state_wait:
	case eth_mcux_phy_state_restart:
		/* Start reading the PHY basic status. */
		ENET_StartSMIRead(ENET, phy_addr, PHY_BASICSTATUS_REG,
				  kENET_MiiReadValidFrame);
		context->phy_state = eth_mcux_phy_state_read_status;
		break;
	case eth_mcux_phy_state_read_status:
		/* PHY Basic status is available. */
		status = ENET_ReadSMIData(ENET);
		link_up =  status & PHY_BSTATUS_LINKSTATUS_MASK;
		if (link_up && !context->link_up) {
			/* Start reading the PHY control register. */
			ENET_StartSMIRead(ENET, phy_addr, PHY_CONTROL1_REG,
					  kENET_MiiReadValidFrame);
			context->link_up = link_up;
			context->phy_state = eth_mcux_phy_state_read_duplex;
		} else if (!link_up && context->link_up) {
			SYS_LOG_INF("Link down");
			context->link_up = link_up;
			k_delayed_work_submit(&context->delayed_phy_work,
					      CONFIG_ETH_MCUX_PHY_TICK_MS);
			context->phy_state = eth_mcux_phy_state_wait;
		} else {
			k_delayed_work_submit(&context->delayed_phy_work,
					      CONFIG_ETH_MCUX_PHY_TICK_MS);
			context->phy_state = eth_mcux_phy_state_wait;
		}

		break;
	case eth_mcux_phy_state_read_duplex:
		/* PHY control register is available. */
		status = ENET_ReadSMIData(ENET);
		eth_mcux_decode_duplex_and_speed(status,
						 &phy_duplex,
						 &phy_speed);
		if (phy_speed != context->phy_speed ||
		    phy_duplex != context->phy_duplex) {
			context->phy_speed = phy_speed;
			context->phy_duplex = phy_duplex;
			ENET_SetMII(ENET,
				    (enet_mii_speed_t) phy_speed,
				    (enet_mii_duplex_t) phy_duplex);
		}

		SYS_LOG_INF("Enabled %sM %s-duplex mode.",
			    (phy_speed ? "100" : "10"),
			    (phy_duplex ? "full" : "half"));
		k_delayed_work_submit(&context->delayed_phy_work,
				      CONFIG_ETH_MCUX_PHY_TICK_MS);
		context->phy_state = eth_mcux_phy_state_wait;
		break;
	}
}

static void eth_mcux_phy_work(struct k_work *item)
{
	struct eth_context *context =
		CONTAINER_OF(item, struct eth_context, phy_work);

	eth_mcux_phy_event(context);
}

static void eth_mcux_delayed_phy_work(struct k_work *item)
{
	struct eth_context *context =
		CONTAINER_OF(item, struct eth_context, delayed_phy_work);

	eth_mcux_phy_event(context);
}

static int eth_tx(struct net_if *iface, struct net_buf *buf)
{
	struct eth_context *context = iface->dev->driver_data;
	const struct net_buf *frag;
	uint8_t *dst;
	status_t status;
	unsigned int imask;

	uint16_t total_len = net_nbuf_ll_reserve(buf) + net_buf_frags_len(buf);

	k_sem_take(&context->tx_buf_sem, K_FOREVER);

	/* As context->frame_buf is shared resource used by both eth_tx
	 * and eth_rx, we need to protect it with irq_lock.
	 */
	imask = irq_lock();

	/* Gather fragment buffers into flat Ethernet frame buffer
	 * which can be fed to MCUX Ethernet functions. First
	 * fragment is special - it contains link layer (Ethernet
	 * in our case) headers and must be treated specially.
	 */
	dst = context->frame_buf;
	memcpy(dst, net_nbuf_ll(buf),
	       net_nbuf_ll_reserve(buf) + buf->frags->len);
	dst += net_nbuf_ll_reserve(buf) + buf->frags->len;

	/* Continue with the rest of fragments (which contain only data) */
	frag = buf->frags->frags;
	while (frag) {
		memcpy(dst, frag->data, frag->len);
		dst += frag->len;
		frag = frag->frags;
	}

	status = ENET_SendFrame(ENET, &context->enet_handle, context->frame_buf,
				total_len);

	irq_unlock(imask);

	if (status) {
		SYS_LOG_ERR("ENET_SendFrame error: %d\n", status);
		return -1;
	}

	net_nbuf_unref(buf);
	return 0;
}

static void eth_rx(struct device *iface)
{
	struct eth_context *context = iface->driver_data;
	struct net_buf *buf, *prev_frag;
	const uint8_t *src;
	uint32_t frame_length = 0;
	status_t status;
	unsigned int imask;

	status = ENET_GetRxFrameSize(&context->enet_handle, &frame_length);
	if (status) {
		enet_data_error_stats_t error_stats;

		SYS_LOG_ERR("ENET_GetRxFrameSize return: %d", status);

		ENET_GetRxErrBeforeReadFrame(&context->enet_handle,
					     &error_stats);
		/* Flush the current read buffer.  This operation can
		 * only report failure if there is no frame to flush,
		 * which cannot happen in this context.
		 */
		status = ENET_ReadFrame(ENET, &context->enet_handle, NULL, 0);
		assert(status == kStatus_Success);
		return;
	}

	buf = net_nbuf_get_reserve_rx(0, K_NO_WAIT);
	if (!buf) {
		/* We failed to get a receive buffer.  We don't add
		 * any further logging here because the allocator
		 * issued a diagnostic when it failed to allocate.
		 *
		 * Flush the current read buffer.  This operation can
		 * only report failure if there is no frame to flush,
		 * which cannot happen in this context.
		 */
		status = ENET_ReadFrame(ENET, &context->enet_handle, NULL, 0);
		assert(status == kStatus_Success);
		return;
	}

	if (sizeof(context->frame_buf) < frame_length) {
		SYS_LOG_ERR("frame too large (%d)\n", frame_length);
		net_nbuf_unref(buf);
		status = ENET_ReadFrame(ENET, &context->enet_handle, NULL, 0);
		assert(status == kStatus_Success);
		return;
	}

	/* As context->frame_buf is shared resource used by both eth_tx
	 * and eth_rx, we need to protect it with irq_lock.
	 */
	imask = irq_lock();

	status = ENET_ReadFrame(ENET, &context->enet_handle,
				context->frame_buf, frame_length);
	if (status) {
		irq_unlock(imask);
		SYS_LOG_ERR("ENET_ReadFrame failed: %d\n", status);
		net_nbuf_unref(buf);
		return;
	}

	src = context->frame_buf;
	prev_frag = buf;
	do {
		struct net_buf *pkt_buf;
		size_t frag_len;

		pkt_buf = net_nbuf_get_reserve_data(0, K_NO_WAIT);
		if (!pkt_buf) {
			irq_unlock(imask);
			SYS_LOG_ERR("Failed to get fragment buf\n");
			net_nbuf_unref(buf);
			assert(status == kStatus_Success);
			return;
		}

		net_buf_frag_insert(prev_frag, pkt_buf);
		prev_frag = pkt_buf;
		frag_len = net_buf_tailroom(pkt_buf);
		if (frag_len > frame_length) {
			frag_len = frame_length;
		}

		memcpy(pkt_buf->data, src, frag_len);
		net_buf_add(pkt_buf, frag_len);
		src += frag_len;
		frame_length -= frag_len;
	} while (frame_length > 0);

	irq_unlock(imask);

	net_recv_data(context->iface, buf);
}

static void eth_callback(ENET_Type *base, enet_handle_t *handle,
			 enet_event_t event, void *param)
{
	struct device *iface = param;
	struct eth_context *context = iface->driver_data;

	switch (event) {
	case kENET_RxEvent:
		eth_rx(iface);
		break;
	case kENET_TxEvent:
		/* Free the TX buffer. */
		k_sem_give(&context->tx_buf_sem);
		break;
	case kENET_ErrEvent:
		/* Error event: BABR/BABT/EBERR/LC/RL/UN/PLR.  */
		break;
	case kENET_WakeUpEvent:
		/* Wake up from sleep mode event. */
		break;
#ifdef ENET_ENHANCEDBUFFERDESCRIPTOR_MODE
	case kENET_TimeStampEvent:
		/* Time stamp event.  */
		break;
	case kENET_TimeStampAvailEvent:
		/* Time stamp available event.  */
		break;
#endif
	}
}

#if defined(CONFIG_ETH_MCUX_0_RANDOM_MAC)
static void generate_mac(uint8_t *mac_addr)
{
	uint32_t entropy;

	entropy = sys_rand32_get();

	mac_addr[3] = entropy >> 8;
	mac_addr[4] = entropy >> 16;
	/* Locally administered, unicast */
	mac_addr[5] = ((entropy >> 0) & 0xfc) | 0x02;
}
#endif

static int eth_0_init(struct device *dev)
{
	struct eth_context *context = dev->driver_data;
	enet_config_t enet_config;
	uint32_t sys_clock;
	enet_buffer_config_t buffer_config = {
		.rxBdNumber = CONFIG_ETH_MCUX_RX_BUFFERS,
		.txBdNumber = CONFIG_ETH_MCUX_TX_BUFFERS,
		.rxBuffSizeAlign = ETH_MCUX_BUFFER_SIZE,
		.txBuffSizeAlign = ETH_MCUX_BUFFER_SIZE,
		.rxBdStartAddrAlign = rx_buffer_desc,
		.txBdStartAddrAlign = tx_buffer_desc,
		.rxBufferAlign = rx_buffer[0],
		.txBufferAlign = tx_buffer[0],
	};

	k_sem_init(&context->tx_buf_sem,
		   CONFIG_ETH_MCUX_TX_BUFFERS, CONFIG_ETH_MCUX_TX_BUFFERS);
	k_work_init(&context->phy_work, eth_mcux_phy_work);
	k_delayed_work_init(&context->delayed_phy_work,
			    eth_mcux_delayed_phy_work);

	sys_clock = CLOCK_GetFreq(kCLOCK_CoreSysClk);

	ENET_GetDefaultConfig(&enet_config);
	enet_config.interrupt |= kENET_RxFrameInterrupt;
	enet_config.interrupt |= kENET_TxFrameInterrupt;
	enet_config.interrupt |= kENET_MiiInterrupt;
	/* FIXME: Workaround for lack of driver API support for multicast
	 * management. So, instead we want to receive all multicast
	 * frames "by default", or otherwise basic IPv6 features, like
	 * address resolution, don't work. On Kinetis Ethernet controller,
	 * that translates to enabling promiscuous mode. The real
	 * fix depends on https://jira.zephyrproject.org/browse/ZEP-1673.
	 */
	enet_config.macSpecialConfig |= kENET_ControlPromiscuousEnable;

#if defined(CONFIG_ETH_MCUX_0_RANDOM_MAC)
	generate_mac(context->mac_addr);
#endif

	ENET_Init(ENET,
		  &context->enet_handle,
		  &enet_config,
		  &buffer_config,
		  context->mac_addr,
		  sys_clock);

	ENET_SetSMI(ENET, sys_clock, false);

	SYS_LOG_DBG("MAC %02x:%02x:%02x:%02x:%02x:%02x",
		    context->mac_addr[0], context->mac_addr[1],
		    context->mac_addr[2], context->mac_addr[3],
		    context->mac_addr[4], context->mac_addr[5]);

	ENET_SetCallback(&context->enet_handle, eth_callback, dev);
	eth_0_config_func();
	ENET_ActiveRead(ENET);

	k_work_submit(&context->phy_work);

	return 0;
}

static void eth_0_iface_init(struct net_if *iface)
{
	struct device *dev = net_if_get_device(iface);
	struct eth_context *context = dev->driver_data;

	net_if_set_link_addr(iface, context->mac_addr,
			     sizeof(context->mac_addr),
			     NET_LINK_ETHERNET);
	context->iface = iface;
}

static struct net_if_api api_funcs_0 = {
	.init	= eth_0_iface_init,
	.send	= eth_tx,
};

static void eth_mcux_rx_isr(void *p)
{
	struct device *dev = p;
	struct eth_context *context = dev->driver_data;

	ENET_ReceiveIRQHandler(ENET, &context->enet_handle);
}

static void eth_mcux_tx_isr(void *p)
{
	struct device *dev = p;
	struct eth_context *context = dev->driver_data;

	ENET_TransmitIRQHandler(ENET, &context->enet_handle);
}

static void eth_mcux_error_isr(void *p)
{
	struct device *dev = p;
	struct eth_context *context = dev->driver_data;
	uint32_t pending = ENET_GetInterruptStatus(ENET);

	if (pending & ENET_EIR_MII_MASK) {
		k_work_submit(&context->phy_work);
		ENET_ClearInterruptStatus(ENET, kENET_MiiInterrupt);
	}
}

static struct eth_context eth_0_context = {
	.phy_duplex = kPHY_FullDuplex,
	.phy_speed = kPHY_Speed100M,
	.mac_addr = {
		/* Freescale's OUI */
		0x00,
		0x04,
		0x9f,
#if !defined(CONFIG_ETH_MCUX_0_RANDOM_MAC)
		CONFIG_ETH_MCUX_0_MAC3,
		CONFIG_ETH_MCUX_0_MAC4,
		CONFIG_ETH_MCUX_0_MAC5
#endif
	}
};

NET_DEVICE_INIT(eth_mcux_0, CONFIG_ETH_MCUX_0_NAME,
		eth_0_init, &eth_0_context,
		NULL, CONFIG_ETH_INIT_PRIORITY, &api_funcs_0,
		ETHERNET_L2, NET_L2_GET_CTX_TYPE(ETHERNET_L2), 1500);

static void eth_0_config_func(void)
{
	IRQ_CONNECT(IRQ_ETH_RX, CONFIG_ETH_MCUX_0_IRQ_PRI,
		    eth_mcux_rx_isr, DEVICE_GET(eth_mcux_0), 0);
	irq_enable(IRQ_ETH_RX);

	IRQ_CONNECT(IRQ_ETH_TX, CONFIG_ETH_MCUX_0_IRQ_PRI,
		    eth_mcux_tx_isr, DEVICE_GET(eth_mcux_0), 0);
	irq_enable(IRQ_ETH_TX);

	IRQ_CONNECT(IRQ_ETH_ERR_MISC, CONFIG_ETH_MCUX_0_IRQ_PRI,
		    eth_mcux_error_isr, DEVICE_GET(eth_mcux_0), 0);
	irq_enable(IRQ_ETH_ERR_MISC);
}
