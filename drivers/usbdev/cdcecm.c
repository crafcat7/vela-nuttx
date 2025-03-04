/****************************************************************************
 * drivers/usbdev/cdcecm.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* References:
 *   [CDCECM1.2] Universal Serial Bus - Communications Class - Subclass
 *               Specification for Ethernet Control Model Devices - Rev 1.2
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <debug.h>

#include <arpa/inet.h>

#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/irq.h>
#include <nuttx/wqueue.h>
#include <nuttx/semaphore.h>
#include <nuttx/net/ip.h>
#include <nuttx/net/netdev.h>
#include <nuttx/usb/usbdev.h>
#include <nuttx/usb/cdc.h>
#include <nuttx/usb/usbdev_trace.h>

#ifdef CONFIG_NET_PKT
#  include <nuttx/net/pkt.h>
#endif

#ifdef CONFIG_BOARD_USBDEV_SERIALSTR
#include <nuttx/board.h>
#endif

#include "cdcecm.h"

#ifdef CONFIG_NET_CDCECM

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Work queue support is required. */

#if !defined(CONFIG_SCHED_WORKQUEUE)
#  error Work queue support is required in this configuration (CONFIG_SCHED_WORKQUEUE)
#endif

/* The low priority work queue is preferred.  If it is not enabled, LPWORK
 * will be the same as HPWORK. NOTE: Use of the high priority work queue will
 * have a negative impact on interrupt handling latency and overall system
 * performance.  This should be avoided.
 */

#define ETHWORK LPWORK

/* CONFIG_CDCECM_NINTERFACES determines the number of physical interfaces
 * that will be supported.
 */

#ifndef CONFIG_CDCECM_NINTERFACES
#  define CONFIG_CDCECM_NINTERFACES 1
#endif

/* TX timeout = 1 minute */

#define CDCECM_TXTIMEOUT (60*CLK_TCK)

/* This is a helper pointer for accessing the contents of Ethernet header */

#define BUF ((FAR struct eth_hdr_s *)self->dev.d_buf)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* The cdcecm_driver_s encapsulates all state information for a single
 * hardware interface
 */

struct cdcecm_driver_s
{
  /* USB CDC-ECM device */

  struct usbdevclass_driver_s  usbdev;      /* USB device class vtable */
  struct usbdev_devinfo_s      devinfo;
  FAR struct usbdev_req_s     *ctrlreq;     /* Allocated control request */
  FAR struct usbdev_ep_s      *epint;       /* Interrupt IN endpoint */
  FAR struct usbdev_ep_s      *epbulkin;    /* Bulk IN endpoint */
  FAR struct usbdev_ep_s      *epbulkout;   /* Bulk OUT endpoint */
  uint8_t                      config;      /* Selected configuration number */

  uint16_t                     pktbuf[(CONFIG_NET_ETH_PKTSIZE +
                                       CONFIG_NET_GUARDSIZE + 1) / 2];

  struct usbdev_req_s         *rdreq;       /* Single read request */
  bool                         rxpending;   /* Packet available in rdreq */

  struct usbdev_req_s         *wrreq;       /* Single write request */
  sem_t                        wrreq_idle;  /* Is the wrreq available? */
  bool                         txdone;      /* Did a write request complete? */

  /* Network device */

  bool                         bifup;       /* true:ifup false:ifdown */
  struct work_s                irqwork;     /* For deferring interrupt work
                                             * to the work queue */
  struct work_s                pollwork;    /* For deferring poll work to
                                             * the work queue */

  /* This holds the information visible to the NuttX network */

  struct net_driver_s          dev;         /* Interface understood by the
                                             * network */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Network Device ***********************************************************/

/* Common TX logic */

static int  cdcecm_transmit(FAR struct cdcecm_driver_s *priv);
static int  cdcecm_txpoll(FAR struct net_driver_s *dev);

/* Interrupt handling */

static void cdcecm_reply(struct cdcecm_driver_s *priv);
static void cdcecm_receive(FAR struct cdcecm_driver_s *priv);
static void cdcecm_txdone(FAR struct cdcecm_driver_s *priv);

static void cdcecm_interrupt_work(FAR void *arg);

/* NuttX callback functions */

static int  cdcecm_ifup(FAR struct net_driver_s *dev);
static int  cdcecm_ifdown(FAR struct net_driver_s *dev);

static void cdcecm_txavail_work(FAR void *arg);
static int  cdcecm_txavail(FAR struct net_driver_s *dev);

#if defined(CONFIG_NET_MCASTGROUP) || defined(CONFIG_NET_ICMPv6)
static int  cdcecm_addmac(FAR struct net_driver_s *dev,
              FAR const uint8_t *mac);
#ifdef CONFIG_NET_MCASTGROUP
static int  cdcecm_rmmac(FAR struct net_driver_s *dev,
              FAR const uint8_t *mac);
#endif
#endif
#ifdef CONFIG_NETDEV_IOCTL
static int  cdcecm_ioctl(FAR struct net_driver_s *dev, int cmd,
              unsigned long arg);
#endif

/* USB Device Class Driver **************************************************/

/* USB Device Class methods */

static int  cdcecm_bind(FAR struct usbdevclass_driver_s *driver,
              FAR struct usbdev_s *dev);

static void cdcecm_unbind(FAR struct usbdevclass_driver_s *driver,
              FAR struct usbdev_s *dev);

static int  cdcecm_setup(FAR struct usbdevclass_driver_s *driver,
              FAR struct usbdev_s *dev, FAR const struct usb_ctrlreq_s *ctrl,
              FAR uint8_t *dataout, size_t outlen);

static void cdcecm_disconnect(FAR struct usbdevclass_driver_s *driver,
                              FAR struct usbdev_s *dev);

/* USB Device Class helpers */

static void cdcecm_ep0incomplete(FAR struct usbdev_ep_s *ep,
              FAR struct usbdev_req_s *req);
static void cdcecm_rdcomplete(FAR struct usbdev_ep_s *ep,
              FAR struct usbdev_req_s *req);
static void cdcecm_wrcomplete(FAR struct usbdev_ep_s *ep,
              FAR struct usbdev_req_s *req);

static int cdcecm_mkepdesc(int epidx,
              FAR struct usb_epdesc_s *epdesc,
              FAR struct usbdev_devinfo_s *devinfo,
              uint8_t speed);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* USB Device Class Methods */

static const struct usbdevclass_driverops_s g_usbdevops =
{
  cdcecm_bind,
  cdcecm_unbind,
  cdcecm_setup,
  cdcecm_disconnect,
  NULL,
  NULL
};

#ifndef CONFIG_CDCECM_COMPOSITE
static const struct usb_devdesc_s g_devdesc =
{
  USB_SIZEOF_DEVDESC,
  USB_DESC_TYPE_DEVICE,
  {
    LSBYTE(0x0200),
    MSBYTE(0x0200)
  },
  USB_CLASS_CDC,
  CDC_SUBCLASS_ECM,
  CDC_PROTO_NONE,
  CONFIG_CDCECM_EP0MAXPACKET,
  {
    LSBYTE(CONFIG_CDCECM_VENDORID),
    MSBYTE(CONFIG_CDCECM_VENDORID)
  },
  {
    LSBYTE(CONFIG_CDCECM_PRODUCTID),
    MSBYTE(CONFIG_CDCECM_PRODUCTID)
  },
  {
    LSBYTE(CDCECM_VERSIONNO),
    MSBYTE(CDCECM_VERSIONNO)
  },
  CDCECM_MANUFACTURERSTRID,
  CDCECM_PRODUCTSTRID,
  CDCECM_SERIALSTRID,
  CDCECM_NCONFIGS
};
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: cdcecm_transmit
 *
 * Description:
 *   Start hardware transmission.  Called either from the txdone interrupt
 *   handling or from watchdog based polling.
 *
 * Input Parameters:
 *   priv - Reference to the driver state structure
 *
 * Returned Value:
 *   OK on success; a negated errno on failure
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static int cdcecm_transmit(FAR struct cdcecm_driver_s *self)
{
  /* Wait until the USB device request for Ethernet frame transmissions
   * becomes available.
   */

  while (nxsem_wait(&self->wrreq_idle) != OK)
    {
    }

  /* Increment statistics */

  NETDEV_TXPACKETS(&self->dev);

  /* Send the packet: address=priv->dev.d_buf, length=priv->dev.d_len */

  memcpy(self->wrreq->buf, self->dev.d_buf, self->dev.d_len);
  self->wrreq->len = self->dev.d_len;

  return EP_SUBMIT(self->epbulkin, self->wrreq);
}

/****************************************************************************
 * Name: cdcecm_txpoll
 *
 * Description:
 *   The transmitter is available, check if the network has any outgoing
 *   packets ready to send.  This is a callback from devif_poll().
 *   devif_poll() may be called:
 *
 *   1. When the preceding TX packet send is complete,
 *   2. When the preceding TX packet send times out and the interface is
 *      reset
 *   3. During normal TX polling
 *
 * Input Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   OK on success; a negated errno on failure
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static int cdcecm_txpoll(FAR struct net_driver_s *dev)
{
  FAR struct cdcecm_driver_s *priv =
    (FAR struct cdcecm_driver_s *)dev->d_private;

  /* Send the packet */

  cdcecm_transmit(priv);

  /* Check if there is room in the device to hold another packet. If
   * not, return a non-zero value to terminate the poll.
   */

  return 1;
}

/****************************************************************************
 * Name: cdcecm_reply
 *
 * Description:
 *   After a packet has been received and dispatched to the network, it
 *   may return with an outgoing packet.  This function checks for that
 *   case and performs the transmission if necessary.
 *
 * Input Parameters:
 *   priv - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static void cdcecm_reply(struct cdcecm_driver_s *priv)
{
  /* If the packet dispatch resulted in data that should be sent out on the
   * network, the field d_len will set to a value > 0.
   */

  if (priv->dev.d_len > 0)
    {
      /* And send the packet */

      cdcecm_transmit(priv);
    }
}

/****************************************************************************
 * Name: cdcecm_receive
 *
 * Description:
 *   An interrupt was received indicating the availability of a new RX packet
 *
 * Input Parameters:
 *   priv - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static void cdcecm_receive(FAR struct cdcecm_driver_s *self)
{
  /* Check for errors and update statistics */

  /* Check if the packet is a valid size for the network buffer
   * configuration.
   */

  /* Copy the data from the hardware to self->dev.d_buf.  Set
   * amount of data in self->dev.d_len
   */

  memcpy(self->dev.d_buf, self->rdreq->buf, self->rdreq->xfrd);
  self->dev.d_len = self->rdreq->xfrd;

#ifdef CONFIG_NET_PKT
  /* When packet sockets are enabled, feed the frame into the tap */

  pkt_input(&self->dev);
#endif

  /* We only accept IP packets of the configured type and ARP packets */

#ifdef CONFIG_NET_IPv4
  if (BUF->type == HTONS(ETHTYPE_IP))
    {
      ninfo("IPv4 frame\n");
      NETDEV_RXIPV4(&self->dev);

      /* Receive an IPv4 packet from the network device */

      ipv4_input(&self->dev);

      /* Check for a reply to the IPv4 packet */

      cdcecm_reply(self);
    }
  else
#endif
#ifdef CONFIG_NET_IPv6
  if (BUF->type == HTONS(ETHTYPE_IP6))
    {
      ninfo("IPv6 frame\n");
      NETDEV_RXIPV6(&self->dev);

      /* Dispatch IPv6 packet to the network layer */

      ipv6_input(&self->dev);

      /* Check for a reply to the IPv6 packet */

      cdcecm_reply(self);
    }
  else
#endif
#ifdef CONFIG_NET_ARP
  if (BUF->type == HTONS(ETHTYPE_ARP))
    {
      /* Dispatch ARP packet to the network layer */

      arp_input(&self->dev);
      NETDEV_RXARP(&self->dev);

      /* If the above function invocation resulted in data that should be
       * sent out on the network, d_len field will set to a value > 0.
       */

      if (self->dev.d_len > 0)
        {
          cdcecm_transmit(self);
        }
    }
  else
#endif
    {
      NETDEV_RXDROPPED(&self->dev);
    }
}

/****************************************************************************
 * Name: cdcecm_txdone
 *
 * Description:
 *   An interrupt was received indicating that the last TX packet(s) is done
 *
 * Input Parameters:
 *   priv - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static void cdcecm_txdone(FAR struct cdcecm_driver_s *priv)
{
  /* Check for errors and update statistics */

  NETDEV_TXDONE(&priv->dev);

  /* In any event, poll the network for new TX data */

  devif_poll(&priv->dev, cdcecm_txpoll);
}

/****************************************************************************
 * Name: cdcecm_interrupt_work
 *
 * Description:
 *   Perform interrupt related work from the worker thread
 *
 * Input Parameters:
 *   arg - The argument passed when work_queue() was called.
 *
 * Returned Value:
 *   OK on success
 *
 * Assumptions:
 *   Runs on a worker thread.
 *
 ****************************************************************************/

static void cdcecm_interrupt_work(FAR void *arg)
{
  FAR struct cdcecm_driver_s *self = (FAR struct cdcecm_driver_s *)arg;
  irqstate_t flags;

  /* Lock the network and serialize driver operations if necessary.
   * NOTE: Serialization is only required in the case where the driver work
   * is performed on an LP worker thread and where more than one LP worker
   * thread has been configured.
   */

  net_lock();

  /* Check if we received an incoming packet, if so, call cdcecm_receive() */

  if (self->rxpending)
    {
      cdcecm_receive(self);

      flags = enter_critical_section();
      self->rxpending = false;
      EP_SUBMIT(self->epbulkout, self->rdreq);
      leave_critical_section(flags);
    }

  /* Check if a packet transmission just completed.  If so, call
   * cdcecm_txdone. This may disable further Tx interrupts if there
   * are no pending transmissions.
   */

  if (self->txdone)
    {
      flags = enter_critical_section();
      self->txdone = false;
      leave_critical_section(flags);

      cdcecm_txdone(self);
    }

  net_unlock();
}

/****************************************************************************
 * Name: cdcecm_ifup
 *
 * Description:
 *   NuttX Callback: Bring up the Ethernet interface when an IP address is
 *   provided
 *
 * Input Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static int cdcecm_ifup(FAR struct net_driver_s *dev)
{
  FAR struct cdcecm_driver_s *priv =
    (FAR struct cdcecm_driver_s *)dev->d_private;

#ifdef CONFIG_NET_IPv4
  ninfo("Bringing up: %u.%u.%u.%u\n",
        ip4_addr1(dev->d_ipaddr), ip4_addr2(dev->d_ipaddr),
        ip4_addr3(dev->d_ipaddr), ip4_addr4(dev->d_ipaddr));
#endif
#ifdef CONFIG_NET_IPv6
  ninfo("Bringing up: %04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x\n",
        dev->d_ipv6addr[0], dev->d_ipv6addr[1], dev->d_ipv6addr[2],
        dev->d_ipv6addr[3], dev->d_ipv6addr[4], dev->d_ipv6addr[5],
        dev->d_ipv6addr[6], dev->d_ipv6addr[7]);
#endif

  priv->bifup = true;
  return OK;
}

/****************************************************************************
 * Name: cdcecm_ifdown
 *
 * Description:
 *   NuttX Callback: Stop the interface.
 *
 * Input Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static int cdcecm_ifdown(FAR struct net_driver_s *dev)
{
  FAR struct cdcecm_driver_s *priv =
    (FAR struct cdcecm_driver_s *)dev->d_private;
  irqstate_t flags;

  /* Disable the Ethernet interrupt */

  flags = enter_critical_section();

  /* Put the EMAC in its reset, non-operational state.  This should be
   * a known configuration that will guarantee the cdcecm_ifup() always
   * successfully brings the interface back up.
   */

  /* Mark the device "down" */

  priv->bifup = false;
  leave_critical_section(flags);
  return OK;
}

/****************************************************************************
 * Name: cdcecm_txavail_work
 *
 * Description:
 *   Perform an out-of-cycle poll on the worker thread.
 *
 * Input Parameters:
 *   arg - Reference to the NuttX driver state structure (cast to void*)
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Runs on a work queue thread.
 *
 ****************************************************************************/

static void cdcecm_txavail_work(FAR void *arg)
{
  FAR struct cdcecm_driver_s *self = (FAR struct cdcecm_driver_s *)arg;

  /* Lock the network and serialize driver operations if necessary.
   * NOTE: Serialization is only required in the case where the driver work
   * is performed on an LP worker thread and where more than one LP worker
   * thread has been configured.
   */

  net_lock();

  /* Ignore the notification if the interface is not yet up */

  if (self->bifup)
    {
      devif_poll(&self->dev, cdcecm_txpoll);
    }

  net_unlock();
}

/****************************************************************************
 * Name: cdcecm_txavail
 *
 * Description:
 *   Driver callback invoked when new TX data is available.  This is a
 *   stimulus perform an out-of-cycle poll and, thereby, reduce the TX
 *   latency.
 *
 * Input Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static int cdcecm_txavail(FAR struct net_driver_s *dev)
{
  FAR struct cdcecm_driver_s *priv =
    (FAR struct cdcecm_driver_s *)dev->d_private;

  /* Is our single work structure available?  It may not be if there are
   * pending interrupt actions and we will have to ignore the Tx
   * availability action.
   */

  if (work_available(&priv->pollwork))
    {
      /* Schedule to serialize the poll on the worker thread. */

      work_queue(ETHWORK, &priv->pollwork, cdcecm_txavail_work, priv, 0);
    }

  return OK;
}

/****************************************************************************
 * Name: cdcecm_addmac
 *
 * Description:
 *   NuttX Callback: Add the specified MAC address to the hardware multicast
 *   address filtering
 *
 * Input Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *   mac  - The MAC address to be added
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

#if defined(CONFIG_NET_MCASTGROUP) || defined(CONFIG_NET_ICMPv6)
static int cdcecm_addmac(FAR struct net_driver_s *dev,
                         FAR const uint8_t *mac)
{
  FAR struct cdcecm_driver_s *priv =
    (FAR struct cdcecm_driver_s *)dev->d_private;

  /* Add the MAC address to the hardware multicast routing table */

  UNUSED(priv); /* Not yet implemented */
  return OK;
}
#endif

/****************************************************************************
 * Name: cdcecm_rmmac
 *
 * Description:
 *   NuttX Callback: Remove the specified MAC address from the hardware
 *   multicast address filtering
 *
 * Input Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *   mac  - The MAC address to be removed
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

#ifdef CONFIG_NET_MCASTGROUP
static int cdcecm_rmmac(FAR struct net_driver_s *dev, FAR const uint8_t *mac)
{
  FAR struct cdcecm_driver_s *priv =
    (FAR struct cdcecm_driver_s *)dev->d_private;

  /* Add the MAC address to the hardware multicast routing table */

  UNUSED(priv); /* Not yet implemented */
  return OK;
}
#endif

/****************************************************************************
 * Name: cdcecm_ioctl
 *
 * Description:
 *   Handle network IOCTL commands directed to this device.
 *
 * Input Parameters:
 *   dev - Reference to the NuttX driver state structure
 *   cmd - The IOCTL command
 *   arg - The argument for the IOCTL command
 *
 * Returned Value:
 *   OK on success; Negated errno on failure.
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

#ifdef CONFIG_NETDEV_IOCTL
static int cdcecm_ioctl(FAR struct net_driver_s *dev, int cmd,
                      unsigned long arg)
{
  /* Decode and dispatch the driver-specific IOCTL command */

  switch (cmd)
    {
      /* Add cases here to support the IOCTL commands */

      default:
        nerr("ERROR: Unrecognized IOCTL command: %d\n", cmd);
        return -ENOTTY;  /* Special return value for this case */
    }

  return OK;
}
#endif

/****************************************************************************
 * USB Device Class Helpers
 ****************************************************************************/

/****************************************************************************
 * Name: cdcecm_ep0incomplete
 *
 * Description:
 *   Handle completion of EP0 control operations
 *
 ****************************************************************************/

static void cdcecm_ep0incomplete(FAR struct usbdev_ep_s *ep,
                                 FAR struct usbdev_req_s *req)
{
  if (req->result || req->xfrd != req->len)
    {
      uerr("result: %hd, xfrd: %zu\n", req->result, req->xfrd);
    }
}

/****************************************************************************
 * Name: cdcecm_rdcomplete
 *
 * Description:
 *   Handle completion of read request on the bulk OUT endpoint.
 *
 ****************************************************************************/

static void cdcecm_rdcomplete(FAR struct usbdev_ep_s *ep,
                              FAR struct usbdev_req_s *req)
{
  FAR struct cdcecm_driver_s *self = (FAR struct cdcecm_driver_s *)ep->priv;

  uinfo("buf: %p, flags 0x%hhx, len %zu, xfrd %zu, result %hd\n",
        req->buf, req->flags, req->len, req->xfrd, req->result);

  switch (req->result)
    {
      case 0:  /* Normal completion */
        {
          DEBUGASSERT(!self->rxpending);
          self->rxpending = true;
          work_queue(ETHWORK, &self->irqwork,
                     cdcecm_interrupt_work, self, 0);
        }
        break;

      case -ESHUTDOWN:  /* Disconnection */
        break;

      default: /* Some other error occurred */
        {
          uerr("req->result: %hd\n", req->result);
          EP_SUBMIT(self->epbulkout, self->rdreq);
        }
        break;
    }
}

/****************************************************************************
 * Name: cdcecm_wrcomplete
 *
 * Description:
 *   Handle completion of write request.  This function probably executes
 *   in the context of an interrupt handler.
 *
 ****************************************************************************/

static void cdcecm_wrcomplete(FAR struct usbdev_ep_s *ep,
                              FAR struct usbdev_req_s *req)
{
  FAR struct cdcecm_driver_s *self = (FAR struct cdcecm_driver_s *)ep->priv;
  int rc;

  uinfo("buf: %p, flags 0x%hhx, len %zu, xfrd %zu, result %hd\n",
        req->buf, req->flags, req->len, req->xfrd, req->result);

  /* The single USB device write request is available for upcoming
   * transmissions again.
   */

  rc = nxsem_post(&self->wrreq_idle);

  if (rc != OK)
    {
      nerr("nxsem_post failed! rc: %d\n", rc);
    }

  /* Inform the network layer that an Ethernet frame was transmitted. */

  self->txdone = true;
  work_queue(ETHWORK, &self->irqwork, cdcecm_interrupt_work, self, 0);
}

/****************************************************************************
 * Name: cdcecm_resetconfig
 *
 * Description:
 *   Mark the device as not configured and disable all endpoints.
 *
 ****************************************************************************/

static void cdcecm_resetconfig(FAR struct cdcecm_driver_s *self)
{
  /* Are we configured? */

  if (self->config != CDCECM_CONFIGID_NONE)
    {
      /* Yes.. but not anymore */

      self->config = CDCECM_CONFIGID_NONE;

      /* Inform the networking layer that the link is down */

      self->dev.d_ifdown(&self->dev);

      /* Disable endpoints.  This should force completion of all pending
       * transfers.
       */

      EP_DISABLE(self->epint);
      EP_DISABLE(self->epbulkin);
      EP_DISABLE(self->epbulkout);
    }
}

/****************************************************************************
 * Name: cdcecm_setconfig
 *
 *   Set the device configuration by allocating and configuring endpoints and
 *   by allocating and queue read and write requests.
 *
 ****************************************************************************/

static int cdcecm_setconfig(FAR struct cdcecm_driver_s *self, uint8_t config)
{
  struct usb_ss_epdesc_s epdesc;
  int ret = OK;

  if (config == self->config)
    {
      return OK;
    }

  cdcecm_resetconfig(self);

  if (config == CDCECM_CONFIGID_NONE)
    {
      return OK;
    }

  if (config != CDCECM_CONFIGID)
    {
      return -EINVAL;
    }

  cdcecm_mkepdesc(CDCECM_EP_INTIN_IDX,
                  &epdesc.epdesc, &self->devinfo, self->usbdev.speed);
  ret = EP_CONFIGURE(self->epint, &epdesc.epdesc, false);

  if (ret < 0)
    {
      goto error;
    }

  self->epint->priv = self;

  cdcecm_mkepdesc(CDCECM_EP_BULKIN_IDX,
                  &epdesc.epdesc, &self->devinfo, self->usbdev.speed);
  ret = EP_CONFIGURE(self->epbulkin, &epdesc.epdesc, false);

  if (ret < 0)
    {
      goto error;
    }

  self->epbulkin->priv = self;

  cdcecm_mkepdesc(CDCECM_EP_BULKOUT_IDX,
                  &epdesc.epdesc, &self->devinfo, self->usbdev.speed);
  ret = EP_CONFIGURE(self->epbulkout, &epdesc.epdesc, true);

  if (ret < 0)
    {
      goto error;
    }

  self->epbulkout->priv = self;

  /* Queue read requests in the bulk OUT endpoint */

  DEBUGASSERT(!self->rxpending);

  self->rdreq->callback = cdcecm_rdcomplete,
  ret = EP_SUBMIT(self->epbulkout, self->rdreq);
  if (ret != OK)
    {
      uerr("EP_SUBMIT failed. ret %d\n", ret);
      goto error;
    }

  /* We are successfully configured */

  self->config = config;

  /* Set client's MAC address */

  memcpy(self->dev.d_mac.ether.ether_addr_octet,
         "\x00\xe0\xde\xad\xbe\xef", IFHWADDRLEN);

  /* Report link up to networking layer */

  if (self->dev.d_ifup(&self->dev) == OK)
    {
      self->dev.d_flags |= IFF_UP;
    }

  return OK;

error:
  cdcecm_resetconfig(self);
  return ret;
}

/****************************************************************************
 * Name: cdcecm_setinterface
 *
 ****************************************************************************/

static int cdcecm_setinterface(FAR struct cdcecm_driver_s *self,
                               uint16_t interface, uint16_t altsetting)
{
  netdev_carrier_on(&self->dev);
  uinfo("interface: %hu, altsetting: %hu\n", interface, altsetting);
  return OK;
}

/****************************************************************************
 * Name: cdcecm_mkstrdesc
 *
 * Description:
 *   Construct a string descriptor
 *
 ****************************************************************************/

static int cdcecm_mkstrdesc(uint8_t id, FAR struct usb_strdesc_s *strdesc)
{
  FAR uint8_t *data = (FAR uint8_t *)(strdesc + 1);
  FAR const char *str;
  int len;
  int ndata;
  int i;

  switch (id)
    {
#ifndef CONFIG_CDCECM_COMPOSITE
    case 0:
      {
        /* Descriptor 0 is the language id */

        strdesc->len  = 4;
        strdesc->type = USB_DESC_TYPE_STRING;
        data[0] = LSBYTE(CDCECM_STR_LANGUAGE);
        data[1] = MSBYTE(CDCECM_STR_LANGUAGE);
        return 4;
      }

    case CDCECM_MANUFACTURERSTRID:
      str = CONFIG_CDCECM_VENDORSTR;
      break;

    case CDCECM_PRODUCTSTRID:
      str = CONFIG_CDCECM_PRODUCTSTR;
      break;

    case CDCECM_SERIALSTRID:
#ifdef CONFIG_BOARD_USBDEV_SERIALSTR
      str = board_usbdev_serialstr();
#else
      str = "0";
#endif
      break;

    case CDCECM_CONFIGSTRID:
      str = "Default";
      break;
#endif

    case CDCECM_MACSTRID:
      str = "020000112233";
      break;

    default:
      uwarn("Unknown string descriptor index: %d\n", id);
      return -EINVAL;
    }

  /* The string is utf16-le.  The poor man's utf-8 to utf16-le
   * conversion below will only handle 7-bit en-us ascii
   */

  len = strlen(str);
  if (len > (CDCECM_MAXSTRLEN / 2))
    {
      len = (CDCECM_MAXSTRLEN / 2);
    }

  for (i = 0, ndata = 0; i < len; i++, ndata += 2)
    {
      data[ndata]     = str[i];
      data[ndata + 1] = 0;
    }

  strdesc->len  = ndata + 2;
  strdesc->type = USB_DESC_TYPE_STRING;
  return strdesc->len;
}

/****************************************************************************
 * Name: cdcecm_mkepcompdesc
 *
 * Description:
 *   Construct the endpoint companion descriptor
 *
 ****************************************************************************/

#ifdef CONFIG_USBDEV_SUPERSPEED
static void cdcecm_mkepcompdesc(int epidx,
                                FAR struct usb_ss_epcompdesc_s *epcompdesc)
{
    switch (epidx)
    {
    case CDCECM_EP_INTIN_IDX:  /* Interrupt IN endpoint */
      {
        epcompdesc->len  = USB_SIZEOF_SS_EPCOMPDESC;                      /* Descriptor length */
        epcompdesc->type = USB_DESC_TYPE_ENDPOINT_COMPANION;              /* Descriptor type */

        if (CONFIG_CDCECM_EPINTIN_MAXBURST >= USB_SS_INT_EP_MAXBURST)
          {
            epcompdesc->mxburst = USB_SS_INT_EP_MAXBURST - 1;
          }
        else
          {
            epcompdesc->mxburst = CONFIG_CDCECM_EPINTIN_MAXBURST;
          }

        epcompdesc->attr      = 0;
        epcompdesc->wbytes[0] = LSBYTE((epcompdesc->mxburst + 1) *
                                       CONFIG_CDCECM_EPINTIN_SSSIZE);
        epcompdesc->wbytes[1] = MSBYTE((epcompdesc->mxburst + 1) *
                                       CONFIG_CDCECM_EPINTIN_SSSIZE);
      }
      break;

    case CDCECM_EP_BULKOUT_IDX:
      {
        epcompdesc->len  = USB_SIZEOF_SS_EPCOMPDESC;                      /* Descriptor length */
        epcompdesc->type = USB_DESC_TYPE_ENDPOINT_COMPANION;              /* Descriptor type */

        if (CONFIG_CDCECM_EPBULKOUT_MAXBURST >= USB_SS_BULK_EP_MAXBURST)
          {
            epcompdesc->mxburst = USB_SS_BULK_EP_MAXBURST - 1;
          }
        else
          {
            epcompdesc->mxburst = CONFIG_CDCECM_EPBULKOUT_MAXBURST;
          }

        if (CONFIG_CDCECM_EPBULKOUT_MAXSTREAM > USB_SS_BULK_EP_MAXSTREAM)
          {
            epcompdesc->attr = USB_SS_BULK_EP_MAXSTREAM;
          }
        else
          {
            epcompdesc->attr = CONFIG_CDCECM_EPBULKOUT_MAXSTREAM;
          }

        epcompdesc->wbytes[0] = 0;
        epcompdesc->wbytes[1] = 0;
      }
      break;

    case CDCECM_EP_BULKIN_IDX:
      {
        epcompdesc->len  = USB_SIZEOF_SS_EPCOMPDESC;                      /* Descriptor length */
        epcompdesc->type = USB_DESC_TYPE_ENDPOINT_COMPANION;              /* Descriptor type */

        if (CONFIG_CDCECM_EPBULKIN_MAXBURST >= USB_SS_BULK_EP_MAXBURST)
          {
            epcompdesc->mxburst = USB_SS_BULK_EP_MAXBURST - 1;
          }
        else
          {
            epcompdesc->mxburst = CONFIG_CDCECM_EPBULKIN_MAXBURST;
          }

        if (CONFIG_CDCECM_EPBULKIN_MAXSTREAM > USB_SS_BULK_EP_MAXSTREAM)
          {
            epcompdesc->attr = USB_SS_BULK_EP_MAXSTREAM;
          }
        else
          {
            epcompdesc->attr = CONFIG_CDCECM_EPBULKIN_MAXSTREAM;
          }

        epcompdesc->wbytes[0] = 0;
        epcompdesc->wbytes[1] = 0;
      }
      break;

    default:
      break;
    }
}
#endif

/****************************************************************************
 * Name: cdcecm_mkepdesc
 *
 * Description:
 *   Construct the endpoint descriptor
 *
 ****************************************************************************/

static int cdcecm_mkepdesc(int epidx,
                           FAR struct usb_epdesc_s *epdesc,
                           FAR struct usbdev_devinfo_s *devinfo,
                           uint8_t speed)
{
  uint16_t intin_mxpktsz   = CONFIG_CDCECM_EPINTIN_FSSIZE;
  uint16_t bulkout_mxpktsz = CONFIG_CDCECM_EPBULKOUT_FSSIZE;
  uint16_t bulkin_mxpktsz  = CONFIG_CDCECM_EPBULKIN_FSSIZE;
  int len = sizeof(struct usb_epdesc_s);

#ifdef CONFIG_USBDEV_SUPERSPEED
  if (speed == USB_SPEED_SUPER ||
      speed == USB_SPEED_SUPER_PLUS ||
      speed == USB_SPEED_UNKNOWN)
    {
      /* Maximum packet size (super speed) */

      intin_mxpktsz   = CONFIG_CDCECM_EPINTIN_SSSIZE;
      bulkout_mxpktsz = CONFIG_CDCECM_EPBULKOUT_SSSIZE;
      bulkin_mxpktsz  = CONFIG_CDCECM_EPBULKIN_SSSIZE;
      len += sizeof(struct usb_ss_epcompdesc_s);
    }
  else
#endif
#ifdef CONFIG_USBDEV_DUALSPEED
  if (speed == USB_SPEED_HIGH)
    {
      /* Maximum packet size (high speed) */

      intin_mxpktsz   = CONFIG_CDCECM_EPINTIN_HSSIZE;
      bulkout_mxpktsz = CONFIG_CDCECM_EPBULKOUT_HSSIZE;
      bulkin_mxpktsz  = CONFIG_CDCECM_EPBULKIN_HSSIZE;
    }
#else
  UNUSED(speed);
#endif

  if (epdesc == NULL)
    {
      return len;
    }

  epdesc->len  = USB_SIZEOF_EPDESC;            /* Descriptor length */
  epdesc->type = USB_DESC_TYPE_ENDPOINT;       /* Descriptor type */

  switch (epidx)
    {
      case CDCECM_EP_INTIN_IDX:  /* Interrupt IN endpoint */
        {
          epdesc->addr            = USB_DIR_IN |
                                    devinfo->epno[CDCECM_EP_INTIN_IDX];
          epdesc->attr            = USB_EP_ATTR_XFER_INT;
          epdesc->mxpacketsize[0] = LSBYTE(intin_mxpktsz);
          epdesc->mxpacketsize[1] = MSBYTE(intin_mxpktsz);
          epdesc->interval        = 5;
        }
        break;

      case CDCECM_EP_BULKIN_IDX:
        {
          epdesc->addr            = USB_DIR_IN |
                                    devinfo->epno[CDCECM_EP_BULKIN_IDX];
          epdesc->attr            = USB_EP_ATTR_XFER_BULK;
          epdesc->mxpacketsize[0] = LSBYTE(bulkin_mxpktsz);
          epdesc->mxpacketsize[1] = MSBYTE(bulkin_mxpktsz);
          epdesc->interval        = 0;
        }
        break;

      case CDCECM_EP_BULKOUT_IDX:
        {
          epdesc->addr            = USB_DIR_OUT |
                                    devinfo->epno[CDCECM_EP_BULKOUT_IDX];
          epdesc->attr            = USB_EP_ATTR_XFER_BULK;
          epdesc->mxpacketsize[0] = LSBYTE(bulkout_mxpktsz);
          epdesc->mxpacketsize[1] = MSBYTE(bulkout_mxpktsz);
          epdesc->interval        = 0;
        }
        break;

      default:
        DEBUGPANIC();
    }

#ifdef CONFIG_USBDEV_SUPERSPEED
  if (speed == USB_SPEED_SUPER || speed == USB_SPEED_SUPER_PLUS)
    {
      epdesc++;
      cdcecm_mkepcompdesc(epidx, (FAR struct usb_ss_epcompdesc_s *)epdesc);
    }
#endif

  return len;
}

/****************************************************************************
 * Name: cdcecm_mkcfgdesc
 *
 * Description:
 *   Construct the config descriptor
 *
 ****************************************************************************/

static int16_t cdcecm_mkcfgdesc(FAR uint8_t *desc,
                                FAR struct usbdev_devinfo_s *devinfo,
                                uint8_t speed, uint8_t type)
{
#ifndef CONFIG_CDCECM_COMPOSITE
  FAR struct usb_cfgdesc_s *cfgdesc = NULL;
#endif
  int16_t len = 0;
  int ret;

  /* Check for switches between high and full speed */

  if (type == USB_DESC_TYPE_OTHERSPEEDCONFIG && speed < USB_SPEED_SUPER)
    {
      speed = speed == USB_SPEED_HIGH ? USB_SPEED_FULL : USB_SPEED_HIGH;
    }

#ifndef CONFIG_CDCECM_COMPOSITE
  if (desc)
    {
      cfgdesc = (FAR struct usb_cfgdesc_s *)desc;
      cfgdesc->len         = USB_SIZEOF_CFGDESC;
      cfgdesc->type        = type;
      cfgdesc->ninterfaces = CDCECM_NINTERFACES;
      cfgdesc->cfgvalue    = CDCECM_CONFIGID;
      cfgdesc->icfg        = devinfo->strbase + CDCECM_CONFIGSTRID;
      cfgdesc->attr        = USB_CONFIG_ATTR_ONE | CDCECM_SELFPOWERED |
                             CDCECM_REMOTEWAKEUP;
      cfgdesc->mxpower     = (CONFIG_USBDEV_MAXPOWER + 1) / 2;

      desc += USB_SIZEOF_CFGDESC;
    }

  len += USB_SIZEOF_CFGDESC;

#elif defined(CONFIG_COMPOSITE_IAD)

  /* Interface association descriptor */

  if (desc)
    {
      FAR struct usb_iaddesc_s *iaddesc = (FAR struct usb_iaddesc_s *)desc;

      iaddesc->len       = USB_SIZEOF_IADDESC;                  /* Descriptor length */
      iaddesc->type      = USB_DESC_TYPE_INTERFACEASSOCIATION;  /* Descriptor type */
      iaddesc->firstif   = devinfo->ifnobase;                   /* Number of first interface of the function */
      iaddesc->nifs      = devinfo->ninterfaces;                /* Number of interfaces associated with the function */
      iaddesc->classid   = USB_CLASS_CDC;                       /* Class code */
      iaddesc->subclass  = CDC_SUBCLASS_ECM;                    /* Sub-class code */
      iaddesc->protocol  = CDC_PROTO_NONE;                      /* Protocol code */
      iaddesc->ifunction = 0;                                   /* Index to string identifying the function */

      desc += USB_SIZEOF_IADDESC;
    }

  len += USB_SIZEOF_IADDESC;
#endif

  /* Communications Class Interface */

  if (desc)
    {
      FAR struct usb_ifdesc_s *ifdesc = (FAR struct usb_ifdesc_s *)desc;

      ifdesc->len      = USB_SIZEOF_IFDESC;
      ifdesc->type     = USB_DESC_TYPE_INTERFACE;
      ifdesc->ifno     = devinfo->ifnobase;
      ifdesc->alt      = 0;
      ifdesc->neps     = 1;
      ifdesc->classid  = USB_CLASS_CDC;
      ifdesc->subclass = CDC_SUBCLASS_ECM;
      ifdesc->protocol = CDC_PROTO_NONE;
      ifdesc->iif      = 0;

      desc += USB_SIZEOF_IFDESC;
    }

  len += USB_SIZEOF_IFDESC;

  if (desc)
    {
      FAR struct cdc_hdr_funcdesc_s *hdrdesc;

      hdrdesc = (FAR struct cdc_hdr_funcdesc_s *)desc;
      hdrdesc->size    = SIZEOF_HDR_FUNCDESC;
      hdrdesc->type    = USB_DESC_TYPE_CSINTERFACE;
      hdrdesc->subtype = CDC_DSUBTYPE_HDR;
      hdrdesc->cdc[0]  = LSBYTE(0x0110);
      hdrdesc->cdc[1]  = MSBYTE(0x0110);

      desc += SIZEOF_HDR_FUNCDESC;
    }

  len += SIZEOF_HDR_FUNCDESC;

  if (desc)
    {
      FAR struct cdc_union_funcdesc_s *uniondesc;

      uniondesc = (FAR struct cdc_union_funcdesc_s *)desc;
      uniondesc->size = SIZEOF_UNION_FUNCDESC(1);
      uniondesc->type = USB_DESC_TYPE_CSINTERFACE;
      uniondesc->subtype = CDC_DSUBTYPE_UNION;
      uniondesc->master = devinfo->ifnobase;
      uniondesc->slave[0] = devinfo->ifnobase + 1;

      desc += SIZEOF_UNION_FUNCDESC(1);
    }

  len += SIZEOF_UNION_FUNCDESC(1);

  if (desc)
    {
      FAR struct cdc_ecm_funcdesc_s *ecmdesc;

      ecmdesc = (FAR struct cdc_ecm_funcdesc_s *)desc;
      ecmdesc->size       = SIZEOF_ECM_FUNCDESC;
      ecmdesc->type       = USB_DESC_TYPE_CSINTERFACE;
      ecmdesc->subtype    = CDC_DSUBTYPE_ECM;
      ecmdesc->mac        = devinfo->strbase + CDCECM_MACSTRID;
      ecmdesc->stats[0]   = 0;
      ecmdesc->stats[1]   = 0;
      ecmdesc->stats[2]   = 0;
      ecmdesc->stats[3]   = 0;
      ecmdesc->maxseg[0]  = LSBYTE(CONFIG_NET_ETH_PKTSIZE);
      ecmdesc->maxseg[1]  = MSBYTE(CONFIG_NET_ETH_PKTSIZE);
      ecmdesc->nmcflts[0] = LSBYTE(0);
      ecmdesc->nmcflts[1] = MSBYTE(0);
      ecmdesc->npwrflts   = 0;

      desc += SIZEOF_ECM_FUNCDESC;
    }

  len += SIZEOF_ECM_FUNCDESC;

  ret = cdcecm_mkepdesc(CDCECM_EP_INTIN_IDX,
                        (FAR struct usb_epdesc_s *)desc,
                        devinfo, speed);
  if (desc)
    {
      desc += ret;
    }

  len += ret;

  /* Data Class Interface */

  if (desc)
    {
      FAR struct usb_ifdesc_s *ifdesc;

      ifdesc = (FAR struct usb_ifdesc_s *)desc;
      ifdesc->len      = USB_SIZEOF_IFDESC;
      ifdesc->type     = USB_DESC_TYPE_INTERFACE;
      ifdesc->ifno     = devinfo->ifnobase + 1;
      ifdesc->alt      = 0;
      ifdesc->neps     = 0;
      ifdesc->classid  = USB_CLASS_CDC_DATA;
      ifdesc->subclass = CDC_SUBCLASS_ECM;
      ifdesc->protocol = CDC_PROTO_NONE;
      ifdesc->iif      = 0;

      desc += USB_SIZEOF_IFDESC;
    }

  len += USB_SIZEOF_IFDESC;

  if (desc)
    {
      FAR struct usb_ifdesc_s *ifdesc;

      ifdesc = (FAR struct usb_ifdesc_s *)desc;
      ifdesc->len      = USB_SIZEOF_IFDESC;
      ifdesc->type     = USB_DESC_TYPE_INTERFACE;
      ifdesc->ifno     = devinfo->ifnobase + 1;
      ifdesc->alt      = 1;
      ifdesc->neps     = 2;
      ifdesc->classid  = USB_CLASS_CDC_DATA;
      ifdesc->subclass = CDC_SUBCLASS_ECM;
      ifdesc->protocol = CDC_PROTO_NONE;
      ifdesc->iif      = 0;

      desc += USB_SIZEOF_IFDESC;
    }

  len += USB_SIZEOF_IFDESC;

  ret = cdcecm_mkepdesc(CDCECM_EP_BULKIN_IDX,
                        (FAR struct usb_epdesc_s *)desc,
                        devinfo, speed);
  if (desc)
    {
      desc += ret;
    }

  len += ret;

  ret = cdcecm_mkepdesc(CDCECM_EP_BULKOUT_IDX,
                        (FAR struct usb_epdesc_s *)desc,
                        devinfo, speed);
  if (desc)
    {
      desc += ret;
    }

  len += ret;

#ifndef CONFIG_CDCECM_COMPOSITE
  if (cfgdesc)
    {
      cfgdesc->totallen[0] = LSBYTE(len);
      cfgdesc->totallen[1] = MSBYTE(len);
    }
#endif

  DEBUGASSERT(len <= CDCECM_MXDESCLEN);
  return len;
}

/****************************************************************************
 * Name: cdcecm_getdescriptor
 *
 * Description:
 *   Copy the USB CDC-ECM Device USB Descriptor of a given Type and a given
 *   Index into the provided Descriptor Buffer.
 *
 * Input Parameter:
 *   drvr  - The USB Device Fuzzer Driver instance.
 *   type  - The Type of USB Descriptor requested.
 *   index - The Index of the USB Descriptor requested.
 *   desc  - The USB Descriptor is copied into this buffer, which must be at
 *           least CDCECM_MXDESCLEN bytes wide.
 *
 * Returned Value:
 *   The size in bytes of the requested USB Descriptor or a negated errno in
 *   case of failure.
 *
 ****************************************************************************/

static int cdcecm_getdescriptor(FAR struct cdcecm_driver_s *self,
                                uint8_t type, uint8_t index, FAR void *desc)
{
  uinfo("type: 0x%02hhx, index: 0x%02hhx\n", type, index);

  switch (type)
    {
#ifndef CONFIG_CDCECM_COMPOSITE
    case USB_DESC_TYPE_DEVICE:
      {
        return usbdev_copy_devdesc(desc, &g_devdesc, self->usbdev.speed);
      }
      break;
#endif

#ifdef CONFIG_USBDEV_DUALSPEED
    case USB_DESC_TYPE_OTHERSPEEDCONFIG:
#endif /* CONFIG_USBDEV_DUALSPEED */
    case USB_DESC_TYPE_CONFIG:
      {
        return cdcecm_mkcfgdesc((FAR uint8_t *)desc, &self->devinfo,
                                self->usbdev.speed, type);
      }
      break;

    case USB_DESC_TYPE_STRING:
      {
        return cdcecm_mkstrdesc(index, (FAR struct usb_strdesc_s *)desc);
      }
      break;

    default:
      uwarn("Unsupported descriptor type: 0x%02hhx\n", type);
      break;
    }

  return -ENOTSUP;
}

/****************************************************************************
 * USB Device Class Methods
 ****************************************************************************/

/****************************************************************************
 * Name: cdcecm_bind
 *
 * Description:
 *   Invoked when the driver is bound to an USB device
 *
 ****************************************************************************/

static int cdcecm_bind(FAR struct usbdevclass_driver_s *driver,
                       FAR struct usbdev_s *dev)
{
  FAR struct cdcecm_driver_s *self = (FAR struct cdcecm_driver_s *)driver;
  int ret = OK;

  uinfo("\n");

#ifndef CONFIG_CDCECM_COMPOSITE
  dev->ep0->priv = self;
#endif

  /* Preallocate control request */

  self->ctrlreq = usbdev_allocreq(dev->ep0, CDCECM_MXDESCLEN);

  if (self->ctrlreq == NULL)
    {
      ret = -ENOMEM;
      goto error;
    }

  self->ctrlreq->callback = cdcecm_ep0incomplete;

  self->epint     = DEV_ALLOCEP(dev,
                                USB_DIR_IN |
                                self->devinfo.epno[CDCECM_EP_INTIN_IDX],
                                true, USB_EP_ATTR_XFER_INT);
  self->epbulkin  = DEV_ALLOCEP(dev,
                                USB_DIR_IN |
                                self->devinfo.epno[CDCECM_EP_BULKIN_IDX],
                                true, USB_EP_ATTR_XFER_BULK);
  self->epbulkout = DEV_ALLOCEP(dev,
                                USB_DIR_OUT |
                                self->devinfo.epno[CDCECM_EP_BULKOUT_IDX],
                                false, USB_EP_ATTR_XFER_BULK);

  if (!self->epint || !self->epbulkin || !self->epbulkout)
    {
      uerr("Failed to allocate endpoints!\n");
      ret = -ENODEV;
      goto error;
    }

  self->epint->priv     = self;
  self->epbulkin->priv  = self;
  self->epbulkout->priv = self;

  /* Pre-allocate read requests.  The buffer size is one full packet. */

  self->rdreq = usbdev_allocreq(self->epbulkout,
                  CONFIG_NET_ETH_PKTSIZE + CONFIG_NET_GUARDSIZE);
  if (self->rdreq == NULL)
    {
      uerr("Out of memory\n");
      ret = -ENOMEM;
      goto error;
    }

  self->rdreq->callback = cdcecm_rdcomplete;

  /* Pre-allocate a single write request.  Buffer size is one full packet. */

  self->wrreq = usbdev_allocreq(self->epbulkin,
                  CONFIG_NET_ETH_PKTSIZE + CONFIG_NET_GUARDSIZE);
  if (self->wrreq == NULL)
    {
      uerr("Out of memory\n");
      ret = -ENOMEM;
      goto error;
    }

  self->wrreq->callback = cdcecm_wrcomplete;

  /* The single write request just allocated is available now. */

  ret = nxsem_init(&self->wrreq_idle, 0, 1);

  if (ret != OK)
    {
      uerr("nxsem_init failed. ret: %d\n", ret);
      goto error;
    }

  self->txdone = false;
  self->dev.d_len = 0;

#ifndef CONFIG_CDCECM_COMPOSITE
#ifdef CONFIG_USBDEV_SELFPOWERED
  DEV_SETSELFPOWERED(dev);
#endif

  /* And pull-up the data line for the soft connect function (unless we are
   * part of a composite device)
   */

  DEV_CONNECT(dev);
#endif
  return OK;

error:
  uerr("cdcecm_bind failed! ret: %d\n", ret);
  cdcecm_unbind(driver, dev);
  return ret;
}

static void cdcecm_unbind(FAR struct usbdevclass_driver_s *driver,
                          FAR struct usbdev_s *dev)
{
  FAR struct cdcecm_driver_s *self = (FAR struct cdcecm_driver_s *)driver;

#ifdef CONFIG_DEBUG_FEATURES
  if (!driver || !dev)
    {
      usbtrace(TRACE_CLSERROR(USBSER_TRACEERR_INVALIDARG), 0);
      return;
    }
#endif

  /* Make sure that the endpoints have been unconfigured.  If
   * we were terminated gracefully, then the configuration should
   * already have been reset.  If not, then calling cdcacm_resetconfig
   * should cause the endpoints to immediately terminate all
   * transfers and return the requests to us (with result == -ESHUTDOWN)
   */

  cdcecm_resetconfig(self);
  up_mdelay(50);

  /* Free the interrupt IN endpoint */

  if (self->epint)
    {
      DEV_FREEEP(dev, self->epint);
      self->epint = NULL;
    }

  /* Free the pre-allocated control request */

  if (self->ctrlreq != NULL)
    {
      usbdev_freereq(dev->ep0, self->ctrlreq);
      self->ctrlreq = NULL;
    }

  /* Free pre-allocated read requests (which should all have
   * been returned to the free list at this time -- we don't check)
   */

  if (self->rdreq != NULL)
    {
      usbdev_freereq(self->epbulkout, self->rdreq);
      self->rdreq = NULL;
    }

  /* Free the bulk OUT endpoint */

  if (self->epbulkout)
    {
      DEV_FREEEP(dev, self->epbulkout);
      self->epbulkout = NULL;
    }

  /* Free write requests that are not in use (which should be all
   * of them)
   */

  if (self->wrreq != NULL)
    {
      usbdev_freereq(self->epbulkin, self->wrreq);
      self->wrreq = NULL;
    }

  /* Free the bulk IN endpoint */

  if (self->epbulkin)
    {
      DEV_FREEEP(dev, self->epbulkin);
      self->epbulkin = NULL;
    }

  /* Clear out all data in the buffer */

  self->dev.d_len = 0;
}

static int cdcecm_setup(FAR struct usbdevclass_driver_s *driver,
                        FAR struct usbdev_s *dev,
                        FAR const struct usb_ctrlreq_s *ctrl,
                        FAR uint8_t *dataout,
                        size_t outlen)
{
  FAR struct cdcecm_driver_s *self = (FAR struct cdcecm_driver_s *)driver;
  uint16_t value = GETUINT16(ctrl->value);
  uint16_t index = GETUINT16(ctrl->index);
  uint16_t len = GETUINT16(ctrl->len);
  int ret = -EOPNOTSUPP;

  uinfo("\n");

  if ((ctrl->type & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_STANDARD)
    {
      switch (ctrl->req)
        {
          case USB_REQ_GETDESCRIPTOR:
            {
              uint8_t descindex = ctrl->value[0];
              uint8_t desctype  = ctrl->value[1];

              self->usbdev.speed = dev->speed;
              ret = cdcecm_getdescriptor(self, desctype, descindex,
                                         self->ctrlreq->buf);
            }
            break;

          case USB_REQ_SETCONFIGURATION:
            ret = cdcecm_setconfig(self, value);
            break;

          case USB_REQ_SETINTERFACE:
            ret = cdcecm_setinterface(self, index, value);
            break;

          default:
            uwarn("Unsupported standard req: 0x%02hhx\n", ctrl->req);
            break;
        }
    }
  else if ((ctrl->type & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_CLASS)
    {
      switch (ctrl->req)
        {
          case ECM_SET_PACKET_FILTER:

            /* SetEthernetPacketFilter is the only required CDCECM subclass
             * specific request, but it is still ok to always operate in
             * promiscuous mode and rely on the host to do the filtering.
             * This is especially true for our case:
             *   A simulated point-to-point connection.
             */

            uinfo("ECM_SET_PACKET_FILTER wValue: 0x%04hx, wIndex: 0x%04hx\n",
                  GETUINT16(ctrl->value), GETUINT16(ctrl->index));

            ret = OK;
            break;

          default:
            uwarn("Unsupported class req: 0x%02hhx\n", ctrl->req);
            break;
        }
    }
  else
    {
      uwarn("Unsupported type: 0x%02hhx\n", ctrl->type);
    }

  if (ret >= 0)
    {
      FAR struct usbdev_req_s *ctrlreq = self->ctrlreq;

      ctrlreq->len   = MIN(len, ret);
      ctrlreq->flags = USBDEV_REQFLAGS_NULLPKT;

#ifndef CONFIG_CDCECM_COMPOSITE
      ret = EP_SUBMIT(dev->ep0, ctrlreq);
      uinfo("EP_SUBMIT ret: %d\n", ret);
#else
      ret = composite_ep0submit(driver, dev, ctrlreq, ctrl);
#endif
      if (ret < 0)
        {
          ctrlreq->result = OK;
          cdcecm_ep0incomplete(dev->ep0, ctrlreq);
        }
    }

  return ret;
}

static void cdcecm_disconnect(FAR struct usbdevclass_driver_s *driver,
                              FAR struct usbdev_s *dev)
{
  uinfo("\n");
}

/****************************************************************************
 * Name: cdcecm_classobject
 *
 * Description:
 *   Register USB CDC/ECM and return the class object.
 *
 * Returned Value:
 *   A pointer to the allocated class object (NULL on failure).
 *
 ****************************************************************************/

static int cdcecm_classobject(int minor,
                              FAR struct usbdev_devinfo_s *devinfo,
                              FAR struct usbdevclass_driver_s **classdev)
{
  FAR struct cdcecm_driver_s *self;
  int ret;

  /* Initialize the driver structure */

  self = kmm_zalloc(sizeof(struct cdcecm_driver_s));
  if (!self)
    {
      nerr("Out of memory!\n");
      return -ENOMEM;
    }

  /* Network device initialization */

  self->dev.d_buf     = (FAR uint8_t *)self->pktbuf;
  self->dev.d_ifup    = cdcecm_ifup;     /* I/F up (new IP address) callback */
  self->dev.d_ifdown  = cdcecm_ifdown;   /* I/F down callback */
  self->dev.d_txavail = cdcecm_txavail;  /* New TX data callback */
#ifdef CONFIG_NET_MCASTGROUP
  self->dev.d_addmac  = cdcecm_addmac;   /* Add multicast MAC address */
  self->dev.d_rmmac   = cdcecm_rmmac;    /* Remove multicast MAC address */
#endif
#ifdef CONFIG_NETDEV_IOCTL
  self->dev.d_ioctl   = cdcecm_ioctl;    /* Handle network IOCTL commands */
#endif
  self->dev.d_private = self;            /* Used to recover private state from dev */

  /* USB device initialization */

#if defined(CONFIG_USBDEV_SUPERSPEED)
  self->usbdev.speed  = USB_SPEED_SUPER;
#elif defined(CONFIG_USBDEV_DUALSPEED)
  self->usbdev.speed  = USB_SPEED_HIGH;
#else
  self->usbdev.speed  = USB_SPEED_FULL;
#endif
  self->usbdev.ops    = &g_usbdevops;

  memcpy(&self->devinfo, devinfo, sizeof(struct usbdev_devinfo_s));

  /* Put the interface in the down state.  This usually amounts to resetting
   * the device and/or calling cdcecm_ifdown().
   */

  cdcecm_ifdown(&self->dev);

  /* Read the MAC address from the hardware into
   * priv->dev.d_mac.ether.ether_addr_octet
   * Applies only if the Ethernet MAC has its own internal address.
   */

  memcpy(self->dev.d_mac.ether.ether_addr_octet,
         "\x00\xe0\xde\xad\xbe\xef", IFHWADDRLEN);

  /* Register the device with the OS so that socket IOCTLs can be performed */

  ret = netdev_register(&self->dev, NET_LL_ETHERNET);
  if (ret < 0)
    {
      nerr("netdev_register failed. ret: %d\n", ret);
    }

  *classdev = (FAR struct usbdevclass_driver_s *)self;
  return ret;
}

/****************************************************************************
 * Name: cdcecm_uninitialize
 *
 * Description:
 *   Un-initialize the USB CDC/ECM class driver.  This function is used
 *   internally by the USB composite driver to uninitialize the CDC/ECM
 *   driver.  This same interface is available (with an untyped input
 *   parameter) when the CDC/ECM driver is used standalone.
 *
 * Input Parameters:
 *   There is one parameter, it differs in typing depending upon whether the
 *   CDC/ECM driver is an internal part of a composite device, or a
 *   standalone USB driver:
 *
 *     classdev - The class object returned by cdcecm_classobject()
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void cdcecm_uninitialize(FAR struct usbdevclass_driver_s *classdev)
{
  FAR struct cdcecm_driver_s *self = (FAR struct cdcecm_driver_s *)classdev;
  int ret;

  /* Un-register the CDC/ECM netdev device */

  ret = netdev_unregister(&self->dev);
  if (ret < 0)
    {
      nerr("ERROR: netdev_unregister failed. ret: %d\n", ret);
    }

#ifndef CONFIG_CDCECM_COMPOSITE
  usbdev_unregister(&self->usbdev);
#endif

  /* And free the driver structure */

  kmm_free(self);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: cdcecm_initialize
 *
 * Description:
 *   Register CDC/ECM USB device interface. Register the corresponding
 *   network driver to NuttX and bring up the network.
 *
 * Input Parameters:
 *   minor - Device minor number.
 *   handle - An optional opaque reference to the CDC/ECM class object that
 *     may subsequently be used with cdcecm_uninitialize().
 *
 * Returned Value:
 *   Zero (OK) means that the driver was successfully registered.  On any
 *   failure, a negated errno value is returned.
 *
 ****************************************************************************/

#ifndef CONFIG_CDCECM_COMPOSITE
int cdcecm_initialize(int minor, FAR void **handle)
{
  FAR struct usbdevclass_driver_s *drvr = NULL;
  struct usbdev_devinfo_s devinfo;
  int ret;

  memset(&devinfo, 0, sizeof(struct usbdev_devinfo_s));
  devinfo.ninterfaces                 = CDCECM_NINTERFACES;
  devinfo.nstrings                    = CDCECM_NSTRIDS;
  devinfo.nendpoints                  = CDCECM_NUM_EPS;
  devinfo.epno[CDCECM_EP_INTIN_IDX]   = CONFIG_CDCECM_EPINTIN;
  devinfo.epno[CDCECM_EP_BULKIN_IDX]  = CONFIG_CDCECM_EPBULKIN;
  devinfo.epno[CDCECM_EP_BULKOUT_IDX] = CONFIG_CDCECM_EPBULKOUT;

  ret = cdcecm_classobject(minor, &devinfo, &drvr);
  if (ret == OK)
    {
      ret = usbdev_register(drvr);
      if (ret < 0)
        {
          uinfo("usbdev_register failed. ret %d\n", ret);
        }
    }

  if (handle)
    {
      *handle = (FAR void *)drvr;
    }

  return ret;
}
#endif

/****************************************************************************
 * Name: cdcecm_get_composite_devdesc
 *
 * Description:
 *   Helper function to fill in some constants into the composite
 *   configuration struct.
 *
 * Input Parameters:
 *     dev - Pointer to the configuration struct we should fill
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifdef CONFIG_CDCECM_COMPOSITE
void cdcecm_get_composite_devdesc(struct composite_devdesc_s *dev)
{
  memset(dev, 0, sizeof(struct composite_devdesc_s));

  /* The callback functions for the CDC/ECM class.
   *
   * classobject() and uninitialize() must be provided by board-specific
   * logic
   */

  dev->mkconfdesc   = cdcecm_mkcfgdesc;
  dev->mkstrdesc    = cdcecm_mkstrdesc;
  dev->classobject  = cdcecm_classobject;
  dev->uninitialize = cdcecm_uninitialize;

  dev->nconfigs     = CDCECM_NCONFIGS; /* Number of configurations supported  */
  dev->configid     = CDCECM_CONFIGID; /* The only supported configuration ID */

  /* Let the construction function calculate the size of config descriptor */

  dev->cfgdescsize  = cdcecm_mkcfgdesc(NULL, NULL, USB_SPEED_UNKNOWN, 0);

  /* Board-specific logic must provide the device minor */

  /* Interfaces.
   *
   * ifnobase must be provided by board-specific logic
   */

  dev->devinfo.ninterfaces = CDCECM_NINTERFACES; /* Number of interfaces in the configuration */

  /* Strings.
   *
   * strbase must be provided by board-specific logic
   */

  dev->devinfo.nstrings    = CDCECM_NSTRIDS + 1;     /* Number of Strings */

  /* Endpoints.
   *
   * Endpoint numbers must be provided by board-specific logic.
   */

  dev->devinfo.nendpoints  = CDCECM_NUM_EPS;
}
#endif /* CONFIG_CDCECM_COMPOSITE */

#endif /* CONFIG_NET_CDCECM */
