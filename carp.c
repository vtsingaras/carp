/*
 * carp.c
 *
 * 2004 Copyright (c) Evgeniy Polyakov <johnpol@2ka.mipt.ru>
 * 2012 Copyright (c) Damien Churchill <damoxc@gmail.com>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/uaccess.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/in.h>
#include <linux/in_route.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/if_arp.h>
#include <linux/mroute.h>
#include <linux/init.h>
#include <linux/in6.h>
#include <linux/etherdevice.h>
#include <linux/inetdevice.h>
#include <linux/igmp.h>
#include <linux/netfilter_ipv4.h>
#include <linux/crypto.h>
#include <linux/random.h>

#include <net/route.h>
#include <net/sock.h>
#include <net/ip.h>
#include <net/icmp.h>
#include <net/protocol.h>
#include <net/arp.h>
#include <net/checksum.h>
#include <net/inet_ecn.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>

#include <asm/scatterlist.h>

#ifdef CONFIG_IPV6
#include <net/ipv6.h>
#include <net/ip6_fib.h>
#include <net/ip6_route.h>
#endif

#include "carp.h"
#include "carp_log.h"
#include "carp_queue.h"
#include "carp_ioctl.h"

int carp_preempt = 0;
int carp_max_devices = 1;
int carp_tx_queues = CARP_DEFAULT_TX_QUEUES;

/*---------------------------- Module parameters ----------------------------*/
MODULE_PARM_DESC(preempt, "Pre-empt masters going down");
module_param_named(preempt, carp_preempt, int, 0444);

MODULE_PARM_DESC(max_carps, "Max number of carp devices");
module_param_named(max_carps, carp_max_devices, int, 0444);

MODULE_PARM_DESC(tx_queues, "Max number of transmit queues (default = 16)");
module_param_named(tx_queues, carp_tx_queues, int, 0);

/*----------------------------- Global variables ----------------------------*/
int carp_net_id __read_mostly;

static struct carp_net *cn_global;
static void carp_del_all_timeouts(struct carp *);

static int carp_dev_init(struct net_device *);
static void carp_dev_uninit(struct net_device *);
static void carp_dev_setup(struct net_device *);
static int carp_dev_close(struct net_device *);
static int carp_dev_open(struct net_device *);
static int carp_dev_ioctl (struct net_device *, struct ifreq *, int);
static int carp_check_params(struct carp *, struct carp_ioctl_params);

static int carp_dev_xmit(struct sk_buff *, struct net_device *);

static struct net_device_stats *carp_dev_get_stats(struct net_device *);

static u32 inline addr2val(u8, u8, u8, u8);

static int  __init carp_init(void);
static void __exit carp_exit(void);

/*----------------------------- Global functions ----------------------------*/
static u32 inline addr2val(u8 a1, u8 a2, u8 a3, u8 a4)
{
    u32 ret;
    ret = ((a1 << 24) | (a2 << 16) | (a3 << 8) | (a4 << 0));
    return htonl(ret);
}

struct carp *carp_get_by_vhid(u8 vhid)
{
    struct list_head *ptr;
    struct carp *entry;

    list_for_each(ptr, &cn_global->dev_list) {
        entry = list_entry(ptr, struct carp, carp_list);
        if (entry->vhid == vhid)
            return entry;
    }
    return NULL;
}

/*----------------------------- Device functions ----------------------------*/
static void carp_dev_uninit(struct net_device *dev)
{
    struct carp *carp = netdev_priv(dev);

    carp_del_all_timeouts(carp);
    carp_remove_proc_entry(carp);
    crypto_free_hash(carp->hash);
    list_del(&(cn_global->dev_list));

    if (carp->odev)
        dev_put(carp->odev);
    dev_put(dev);
}

static int carp_check_params(struct carp *carp, struct carp_ioctl_params p)
{
    carp_dbg("%s\n", __func__);
    if (p.state != INIT && p.state != BACKUP && p.state != MASTER)
    {
    	log("Wrong state %d.\n", p.state);
    	return -1;
    }

    if (!__dev_get_by_name(dev_net(carp->dev), p.devname))
    {
    	log("No such device %s.\n", p.devname);
    	return -2;
    }

    if (p.md_timeout > MAX_MD_TIMEOUT || p.adv_timeout > MAX_ADV_TIMEOUT ||
        !p.md_timeout || !p.adv_timeout)
    	return -3;

    return 0;
}

static void carp_del_all_timeouts(struct carp *carp)
{
    if (timer_pending(&carp->md_timer))
        del_timer_sync(&carp->md_timer);
    if (timer_pending(&carp->adv_timer))
        del_timer_sync(&carp->adv_timer);
}

static void carp_send_arp(struct carp *carp)
{
    struct in_device *in_dev;
    struct in_ifaddr *ifa;
    struct sk_buff *skb;

    if (carp->dev == NULL || carp->odev == NULL) {
        return;
    }

    rcu_read_lock();
    if ((in_dev = __in_dev_get_rcu(carp->dev)) == NULL) {
        rcu_read_unlock();
        return;
    }

    for (ifa = in_dev->ifa_list; ifa; ifa = ifa->ifa_next) {
        skb = arp_create(ARPOP_REQUEST, ETH_P_ARP, ifa->ifa_address, carp->odev,
                         ifa->ifa_address, NULL, carp->odev->dev_addr, carp->dev->dev_addr);
        if (!skb) {
            pr_err("%s: ARP packet allocation failed\n", carp->name);
            continue;
        }
        arp_xmit(skb);
    }

    rcu_read_unlock();
}

int carp_set_interface(struct carp *carp, char *dev_name)
{
    struct net_device *real_dev;
    struct in_device *in_dev;

    if (carp->dev == NULL)
        return 1;

    real_dev = dev_get_by_name(dev_net(carp->dev), dev_name);
    if (real_dev) {
        pr_info("%s: Setting carpdev to %s", carp->dev->name, real_dev->name);
        carp->odev = real_dev;
        carp->link = real_dev->ifindex;
        in_dev     = in_dev_get(real_dev);
        if (in_dev != NULL && in_dev->ifa_list != NULL) {
            carp->iph.saddr = in_dev->ifa_list[0].ifa_address;
        }

        carp->dev->hard_header_len = real_dev->hard_header_len;
        carp->dev->mtu = real_dev->mtu;

        carp->odev->flags |= IFF_BROADCAST | IFF_ALLMULTI;
        carp->oflags = carp->odev->flags;

    } else {
        return 1;
    }

    return 0;
}

void carp_set_run(struct carp *carp, sa_family_t af)
{
    if (carp->odev == NULL) {
        dev_change_flags(carp->dev, carp->dev->flags & ~IFF_RUNNING);
        carp_set_state(carp, INIT);
        return;
    }

    if (carp->dev->flags & IFF_UP && carp->vhid > 0) {
        carp->dev->flags |= IFF_RUNNING;
    } else {
        carp->dev->flags &= ~IFF_RUNNING;
        return;
    }

    switch (carp->state) {
        case INIT:
            carp_set_state(carp, BACKUP);
            carp_set_run(carp, 0);
            break;
        case BACKUP:
            if (timer_pending(&carp->adv_timer))
                del_timer_sync(&carp->adv_timer);
            mod_timer(&carp->md_timer, jiffies + carp->md_timeout);
            break;
        case MASTER:
    		if (!timer_pending(&carp->adv_timer))
    			mod_timer(&carp->adv_timer, jiffies + carp->adv_timeout);
            break;
    }
}

void carp_set_state(struct carp *carp, enum carp_state state)
{
    static const char *carp_states[] = { CARP_STATES };

    if (carp->state == state)
        return;

    carp_dbg("%s\n", __func__);
    pr_info("%s: state transition: %s -> %s.\n", carp->name,
            carp_states[carp->state], carp_states[state]);

    carp->state = state;

    // TODO: set the link state of the carpX interface
    switch (state) {
    	case MASTER:
    		carp_call_queue(MASTER_QUEUE);
    		if (!timer_pending(&carp->adv_timer))
    			mod_timer(&carp->adv_timer, jiffies + carp->adv_timeout);
    		break;
    	case BACKUP:
    		carp_call_queue(BACKUP_QUEUE);
    		if (!timer_pending(&carp->md_timer))
    			mod_timer(&carp->md_timer, jiffies + carp->md_timeout);
    		break;
    	default:
    		break;
    }
}

void carp_master_down(unsigned long data)
{
    struct carp *carp = (struct carp *)data;
    carp_dbg("%s\n", __func__);

    switch (carp->state) {
        case INIT:
            carp_dbg("%s: master_down event in INIT state\n", carp->name);
            break;
        case MASTER:
            break;
        case BACKUP:
            carp_set_state(carp, MASTER);
            carp_proto_adv(carp);
            //if (carp->balancing == CARP_BAL_NONE) {
                carp_send_arp(carp);
                carp->carp_delayed_arp = 2;
            //}
            carp_set_run(carp, 0);
            break;
    }
}

static int carp_dev_xmit(struct sk_buff *skb, struct net_device *carp_dev)
{
    carp_dbg("%s\n", __func__);
#if 0
    struct carp *cp = netdev_priv(dev);
    struct net_device_stats *stats = &cp->stat;
    struct iphdr  *iph = skb->nh.iph;
    u8     tos;
    u16    df;
    struct rtable *rt;
    struct net_device *tdev;
    u32    dst;
    int    mtu;
    int err;
    int pkt_len = skb->len;
    log("%s\n", __func__);

    skb->ip_summed = CHECKSUM_NONE;
    skb->protocol = htons(ETH_P_IP);

    ip_select_ident(iph, &rt->u.dst, NULL);
    ip_send_check(iph);
    err = NF_HOOK(PF_INET, NF_IP_LOCAL_OUT, skb, NULL, rt->u.dst.dev, dst_output);
    if (err == NET_XMIT_SUCCESS || err == NET_XMIT_CN) {
    	stats->tx_bytes += pkt_len;
    	stats->tx_packets++;
    } else {
    	stats->tx_errors++;
    	stats->tx_aborted_errors++;
    }
#endif
    return 0;
}

static int carp_dev_ioctl (struct net_device *carp_dev, struct ifreq *ifr, int cmd)
{
    int err = 0;
    struct carp *carp = netdev_priv(carp_dev);
    //struct carp_net *cn = net_generic(dev_net(carp_dev), carp_net_id);

    struct net_device *tdev = NULL;
    struct carp_ioctl_params p;
    struct timeval tv;

    carp_dbg("%s\n", __func__);

    memset(&p, 0, sizeof(p));

    err = -EPERM;
    if (!capable(CAP_NET_ADMIN))
    	goto err_out;

    switch (cmd)
    {
    	case SIOC_SETCARPPARAMS:
    		err = -EFAULT;
    		if (copy_from_user(&p, ifr->ifr_ifru.ifru_data, sizeof(p)))
    			goto err_out;

    		err = -EINVAL;
    		if (carp_check_params(carp, p))
    			goto err_out;

    		carp_dbg("Setting new CARP parameters.\n");

    		if (memcmp(p.devname, carp->odev->name, IFNAMSIZ) && (tdev = dev_get_by_name(dev_net(carp_dev), p.devname)) != NULL)
    			carp_dev_close(carp->dev);


    		spin_lock(&carp->lock);

    		if (tdev)
    		{
    			carp->odev->flags = carp->oflags;
    			dev_put(carp->odev);

    			carp->odev 	= tdev;
    			carp->link 	= carp->odev->ifindex;
    			carp->oflags 	= carp->odev->flags;
    			carp->odev->flags |= IFF_BROADCAST | IFF_ALLMULTI;
    		}

    		carp_set_state(carp, p.state);
    		memcpy(carp->carp_pad, p.carp_pad, sizeof(carp->carp_pad));
    		memcpy(carp->carp_key, p.carp_key, sizeof(carp->carp_key));
    		carp->vhid = p.carp_vhid;
    		carp->advbase = p.carp_advbase;
    		carp->advskew = p.carp_advskew;

            tv.tv_sec = 3 * carp->advbase;
            if (carp->advbase == 0 && carp->advskew == 0)
                tv.tv_usec = 3 * 1000000 / 256;
            else
                tv.tv_usec = carp->advskew * 1000000 / 256;
            carp->md_timeout  = timeval_to_jiffies(&tv);

            tv.tv_sec = carp->advbase;
            if (carp->advbase == 0 && carp->advskew == 0)
                tv.tv_usec = 1 * 1000000 / 256;
            else
                tv.tv_usec = carp->advskew * 1000000 / 256;
            carp->adv_timeout = timeval_to_jiffies(&tv);


    		spin_unlock(&carp->lock);
    		if (tdev)
    			carp_dev_open(carp->dev);
    		break;
    	case SIOC_GETCARPPARAMS:

    		carp_dbg("Dumping CARP parameters.\n");

    		spin_lock(&carp->lock);
    		p.state = carp->state;
    		memcpy(p.carp_pad, carp->carp_pad, sizeof(carp->carp_pad));
    		memcpy(p.carp_key, carp->carp_key, sizeof(carp->carp_key));
    		p.carp_vhid = carp->vhid;
    		p.carp_advbase = carp->advbase;
    		p.carp_advskew = carp->advskew;
    		p.md_timeout = carp->md_timeout;
    		p.adv_timeout = carp->adv_timeout;
    		memcpy(p.devname, carp->odev->name, sizeof(p.devname));
    		p.devname[sizeof(p.devname) - 1] = '\0';
    		spin_unlock(&carp->lock);

    		err = -EFAULT;
    		if (copy_to_user(ifr->ifr_ifru.ifru_data, &p, sizeof(p)))
    			goto err_out;
    		break;
    	default:
    		err = -EINVAL;
    		break;

    }
    err = 0;

err_out:
    return err;
}

static struct net_device_stats *carp_dev_get_stats(struct net_device *carp_dev)
{
    struct carp *carp = netdev_priv(carp_dev);
    return &(carp->stat);
}

static int carp_dev_change_mtu(struct net_device *carp_dev, int new_mtu)
{
    carp_dev->mtu = new_mtu;
    return 0;
}

static int carp_set_mac_address(struct net_device *carp_dev, void *addr)
{
    struct carp *carp = netdev_priv(carp_dev);
    struct sockaddr *address = addr;

    if (!is_valid_ether_addr(address->sa_data))
        return -EADDRNOTAVAIL;

    memcpy(carp_dev->dev_addr, address->sa_data, carp_dev->addr_len);
    memcpy(carp->hwaddr, address->sa_data, carp_dev->addr_len);
    return 0;
}

static const struct net_device_ops carp_netdev_ops = {
    .ndo_init            = carp_dev_init,
    .ndo_uninit          = carp_dev_uninit,
    .ndo_open            = carp_dev_open,
    .ndo_stop            = carp_dev_close,
    .ndo_do_ioctl        = carp_dev_ioctl,
    .ndo_change_mtu      = carp_dev_change_mtu,
    .ndo_start_xmit      = carp_dev_xmit,
    .ndo_get_stats       = carp_dev_get_stats,
// NOTE: the below were never implemented in the old carp module
//    .ndo_validate_addr   = eth_validate_addr,
//    .ndo_set_rx_mode     = set_multicast_list,
    .ndo_set_mac_address = carp_set_mac_address,
//    .ndo_get_stats64     = carp_get_stats64,
};

static void carp_dev_setup(struct net_device *carp_dev)
{
    int res;
    struct carp *carp = netdev_priv(carp_dev);

    carp_dbg("%s\n", __func__);

    /* Initialise the device entry points */
    carp_dev->netdev_ops = &carp_netdev_ops;

    carp_dev->destructor = free_netdev;

    // FIXME: what happened to the owner field?
    //carp_dev->owner = THIS_MODULE;
    carp_dev->type            = ARPHRD_ETHER;
    carp_dev->hard_header_len = LL_MAX_HEADER;
    carp_dev->mtu             = 1500;
    carp_dev->flags           = IFF_NOARP;
    carp_dev->iflink          = 0;
    carp_dev->addr_len        = 4;

    /* Initialise carp options */
    carp->iph.saddr = addr2val(10, 0, 0, 3);
    carp->iph.daddr = MULTICAST_ADDR;
    carp->iph.tos   = 0;

    carp->state     = INIT;
    carp->vhid      = 0;
    carp->advskew   = 0;
    carp->advbase   = CARP_DFLTINTV;
    carp->version   = CARP_VERSION;

    /* Setup the carp advertisements */
    memset(carp->carp_key, 1, sizeof(carp->carp_key));
    get_random_bytes(&carp->carp_adv_counter, 8);

    carp->md_timeout  = carp_calculate_timeout(3, carp->advbase, carp->advskew);
    carp->adv_timeout = carp_calculate_timeout(1, carp->advbase, carp->advskew);

    init_timer(&carp->md_timer);
    carp->md_timer.data      = (unsigned long)carp;
    carp->md_timer.function  = carp_master_down;

    init_timer(&carp->adv_timer);
    carp->adv_timer.data     = (unsigned long)carp;
    carp->adv_timer.function = carp_advertise;

    carp->hash = crypto_alloc_hash("hmac(sha1)", 0, CRYPTO_ALG_ASYNC);
    if (!carp->hash) {
        pr_err("Failed to allocate SHA1 hash.\n");
        res = -EINVAL;
        goto out;
    }

    res = carp_init_queues();
    if (res)
        goto err_out_crypto_free;

    return;

err_out_crypto_free:
    crypto_free_hash(carp->hash);
out:
    return;
}

/* Called when the link state is set to UP */
static int carp_dev_open(struct net_device *carp_dev)
{
    struct carp *carp = netdev_priv(carp_dev);
    struct rtable *rt;
    struct flowi4 fl4 = {
        .flowi4_oif   = carp->link,
        .daddr        = carp->iph.daddr,
        .saddr        = carp->iph.saddr,
        .flowi4_tos   = RT_TOS(carp->iph.tos),
        .flowi4_proto = IPPROTO_CARP,
    };
    carp_dbg("%s", __func__);

    if (carp->odev == NULL)
        return 0;

    rt = ip_route_output_key(dev_net(carp_dev), &fl4);
    if (rt == NULL)
        return -EADDRNOTAVAIL;

    carp_dev = rt->dst.dev;
    ip_rt_put(rt);
    if (in_dev_get(carp_dev) == NULL)
    	return -EADDRNOTAVAIL;
    carp->mlink = carp_dev->ifindex;
    ip_mc_inc_group(in_dev_get(carp_dev), carp->iph.daddr);

    carp->dev->flags |= IFF_UP;
    carp_set_run(carp, 0);

    return 0;
}

/* Called when the link state is set to DOWN */
static int carp_dev_close(struct net_device *carp_dev)
{
    struct carp *carp = netdev_priv(carp_dev);
    struct in_device *in_dev = inetdev_by_index(dev_net(carp_dev), carp->mlink);

    if (in_dev) {
    	ip_mc_dec_group(in_dev, carp->iph.daddr);
    	in_dev_put(in_dev);
    }

    carp_del_all_timeouts(carp);

    carp->carp_bow_out = 1;
    carp_proto_adv(carp);
    carp->carp_bow_out = 0;

    carp_set_state(carp, INIT);

    return 0;
}

/*
 * Called from registration process
 */
static int carp_dev_init(struct net_device *carp_dev)
{
    struct carp *carp;
    struct iphdr *iph;
    carp_dbg("%s\n", __func__);

    carp = netdev_priv(carp_dev);
    iph = &carp->iph;

    if (!iph->daddr || !MULTICAST(iph->daddr) || !iph->saddr)
    	return -EINVAL;

    dev_hold(carp_dev);

    ip_eth_mc_map(carp->iph.daddr, carp_dev->dev_addr);
    memcpy(carp_dev->broadcast, &iph->daddr, 4);

    carp_dev->netdev_ops = &carp_netdev_ops;

    carp->dev = carp_dev;
    strncpy(carp->name, carp_dev->name, IFNAMSIZ);

    carp_create_proc_entry(carp);
    carp_prepare_sysfs_group(carp);
    list_add_tail(&carp->carp_list, &cn_global->dev_list);

    return 0;
}

static int carp_validate(struct nlattr *tb[], struct nlattr *data[])
{
    carp_dbg("%s", __func__);
    if (tb[IFLA_ADDRESS]) {
        if (nla_len(tb[IFLA_ADDRESS]) != ETH_ALEN)
            return -EINVAL;
        if (!is_valid_ether_addr(nla_data(tb[IFLA_ADDRESS])))
            return -EADDRNOTAVAIL;
    }
    return 0;
}

static int carp_get_tx_queues(struct net *net, struct nlattr *tb[],
                              unsigned int *num_queues,
                              unsigned int *real_num_queues)
{
    carp_dbg("%s", __func__);
    return 0;
}

static struct rtnl_link_ops carp_link_ops __read_mostly = {
    .kind          = "carp",
    .priv_size     = sizeof(struct carp),
    .setup         = carp_dev_setup,
    .validate      = carp_validate,
    .get_tx_queues = carp_get_tx_queues,
};

int carp_create(struct net *net, const char *name)
{
    struct net_device *carp_dev;
    int res;
    carp_dbg("%s", __func__);

    rtnl_lock();

    carp_dev = alloc_netdev_mq(sizeof(struct carp),
                               name ? name : "carp%d",
                               carp_dev_setup, carp_tx_queues);
    if (!carp_dev) {
        pr_err("%s: eek! can't alloc netdev!\n", name);
        rtnl_unlock();
        return -ENOMEM;
    }

    dev_net_set(carp_dev, net);
    carp_dev->rtnl_link_ops = &carp_link_ops;

    res = register_netdevice(carp_dev);

    netif_carrier_off(carp_dev);

    rtnl_unlock();
    if (res < 0)
        free_netdev(carp_dev);
    return res;
}

static int __net_init carp_net_init(struct net *net)
{
    struct carp_net *cn = net_generic(net, carp_net_id);
    carp_dbg("%s", __func__);

    cn->net = net;
    INIT_LIST_HEAD(&cn->dev_list);

    cn_global = cn;

    carp_create_proc_dir(cn);
    //carp_create_sysfs(cn);

    return 0;
}

static void __net_exit carp_net_exit(struct net *net)
{
    struct carp_net *cn = net_generic(net, carp_net_id);
    carp_dbg("%s", __func__);

    //carp_destroy_sysfs(cn);
    carp_destroy_proc_dir(cn);
}

static struct pernet_operations carp_net_ops = {
    .init = carp_net_init,
    .exit = carp_net_exit,
    .id   = &carp_net_id,
    .size = sizeof(struct carp_net),
};

static int __init carp_init(void)
{
    int i;
    int res;
    carp_dbg("%s", __func__);

    pr_info("carp: %s", DRV_DESC);

    if (carp_preempt == 1)
        carp_dbg("carp: Using master pre-emption.");

    res = register_pernet_subsys(&carp_net_ops);
    if (res)
        goto out;

    res = carp_register_protocol();
    if (res)
        goto err_proto;

    res = rtnl_link_register(&carp_link_ops);
    if (res)
        goto err_link;

    carp_create_debugfs();

    for (i = 0; i < carp_max_devices; i++) {
        res = carp_create(&init_net, NULL);
        if (res)
            goto err;
    }

out:
    return res;
err:
    carp_dbg("carp: error creating netdev");
    rtnl_link_unregister(&carp_link_ops);
err_link:
    carp_dbg("carp: error registering link");
    carp_unregister_protocol();
err_proto:
    carp_dbg("carp: error registering protocol");
    unregister_pernet_subsys(&carp_net_ops);
    goto out;
}


static void __exit carp_exit(void)
{
    carp_dbg("%s", __func__);
    pr_info("carp: unloading");
    carp_destroy_debugfs();

    rtnl_link_unregister(&carp_link_ops);
    unregister_pernet_subsys(&carp_net_ops);

    carp_fini_queues();

    if (carp_unregister_protocol() < 0)
        pr_info("Failed to remove CARP protocol handler.\n");
}

module_init(carp_init);
module_exit(carp_exit);
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
MODULE_DESCRIPTION(DRV_DESCRIPTION ", v" DRV_VERSION);
MODULE_AUTHOR("Damien Churchill, Evgeniy Polyakov");
