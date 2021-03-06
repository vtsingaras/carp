/*
 * 	carp.h
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

#ifndef __CARP_H
#define __CARP_H

#include <linux/netdevice.h>
#include <linux/if.h>
#include <linux/ip.h>
#include <linux/proc_fs.h>

#include "carp_ioctl.h"

#define DRV_VERSION     "0.0.2"
#define DRV_RELDATE     "March 21, 2012"
#define DRV_NAME        "carp"
#define DRV_DESCRIPTION "Common Address Redundancy Protocol Driver"
#define DRV_DESC DRV_DESCRIPTION ": v" DRV_VERSION " (" DRV_RELDATE ")\n"

#define IPPROTO_CARP           112
#define CARP_TTL               255
#define	CARP_SIG_LEN            20
#define CARP_DEFAULT_TX_QUEUES  16
#define CARP_STATE_LEN           8
#define CARP_STATES "INIT", "MASTER", "BACKUP"

/* carp_version */
#define	CARP_VERSION             2

/* carp_type */
#define CARP_ADVERTISEMENT       0x01

/* carp_advbase */
#define CARP_DFLTINTV            1

#define MULTICAST(x)    (((x) & htonl(0xf0000000)) == htonl(0xe0000000))
#define MULTICAST_ADDR  addr2val(224, 0, 0, 18)

#define timeval_before(before, after)    		\
    (((before)->tv_sec == (after)->tv_sec) ? ((before)->tv_usec < (after)->tv_usec) : ((before)->tv_sec < (after)->tv_sec))

#define timeval_to_ms(tv) \
    ((tv->tv_sec * 1000) + (tv->tv_usec / USEC_PER_MSEC))

extern int carp_net_id;

extern int carp_preempt;
extern int carp_max_devices;
extern int carp_tx_queues;

/*
 * carp->flags definitions.
 */
#define CARP_DATA_AVAIL		(1<<0)

/*
 * The CARP header layout is as follows:
 *
 *     0                   1                   2                   3
 *     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |Version| Type  | VirtualHostID |    AdvSkew    |    Auth Len   |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |    Demotion   |     AdvBase   |          Checksum             |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                         Counter (1)                           |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                         Counter (2)                           |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                        SHA-1 HMAC (1)                         |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                        SHA-1 HMAC (2)                         |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                        SHA-1 HMAC (3)                         |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                        SHA-1 HMAC (4)                         |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                        SHA-1 HMAC (5)                         |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 */

struct carp_header {
#if defined(__LITTLE_ENDIAN_BITFIELD)
	u8	carp_type:4,
		carp_version:4;
#elif defined (__BIG_ENDIAN_BITFIELD)
	u8	carp_version:4,
		carp_type:4;
#else
#error	"Please fix <asm/byteorder.h>"
#endif
	u8	carp_vhid;
	u8	carp_advskew;
	u8	carp_authlen;
	u8	carp_demote;
	u8	carp_advbase;
	u16	carp_cksum;
	u32	carp_counter[2];
	u8	carp_md[CARP_SIG_LEN];
};

struct carp_stat {
	u32	crc_errors;
	u32	ver_errors;
	u32	vhid_errors;
	u32	hmac_errors;
	u32	counter_errors;

	u32	mem_errors;
	u32	xmit_errors;

	u32	bytes_sent;
};

struct carp_net {
    struct net            *net;
    struct list_head       dev_list;
    struct proc_dir_entry *proc_dir;
    struct class_attribute class_attr_carp;
};

struct carp {
	struct net_device_stats stat;
	struct net_device      *dev, *odev;

	char                    name[IFNAMSIZ];

	int	                    link, mlink;
	struct iphdr            iph;

	u32                     md_timeout, adv_timeout;
	struct timer_list       md_timer, adv_timer;

    /* carp params */
    u8                      vhid;
    u8                      advbase;
    u8                      advskew;
    u8                      version;

	enum carp_state         state;
	struct carp_stat        cstat;

	u8                      carp_key[CARP_KEY_LEN];
	u8                      carp_pad[CARP_HMAC_PAD_LEN];
	struct crypto_hash     *hash;

    u8                      hwaddr[ETH_ALEN];

    int                     carp_bow_out;
    int                     carp_delayed_arp;
	u64                     carp_adv_counter;

	spinlock_t              lock;

	u32                     flags;
	unsigned short          oflags;

    struct   proc_dir_entry *proc_entry;
    char     proc_file_name[IFNAMSIZ];

    struct dentry           *debug_dir;
    struct list_head         carp_list;
};

static inline char *carp_state_fmt(struct carp *carp)
{
    switch (carp->state) {
        case MASTER:
            return "MASTER";
        case INIT:
            return "INIT";
        case BACKUP:
            return "BACKUP";
    }
    return NULL;
}

static inline u32 carp_calculate_timeout(u8 mod, u8 advbase, u8 advskew)
{
    struct timeval tv;
    tv.tv_sec = mod * advbase;
    if (advbase == 0 && advskew == 0)
        tv.tv_usec = mod * 1000000 / 256;
    else
        tv.tv_usec = advskew * 1000000 / 256;
    return timeval_to_jiffies(&tv);
}

// Implemented in carp.c
int carp_crypto_hmac(struct carp *, struct scatterlist *, u8 *);
int carp_set_interface(struct carp *, char *);
void carp_set_run(struct carp *, sa_family_t);
void carp_set_state(struct carp *, enum carp_state);
void carp_master_down(unsigned long);
struct carp * carp_get_by_vhid(u8);

// Implemented in carp_proto.c
void carp_advertise(unsigned long data);
int carp_register_protocol(void);
int carp_unregister_protocol(void);

// Implemented in carp_debugfs.c
void carp_create_debugfs(void);
void carp_destroy_debugfs(void);

// Implemented in carp_procfs.c
void carp_proto_adv(struct carp *);
void carp_create_proc_entry(struct carp *);
void carp_remove_proc_entry(struct carp *);
void __net_init carp_create_proc_dir(struct carp_net *);
void __net_exit carp_destroy_proc_dir(struct carp_net *);

// Implemented in carp_sysfs.c
int carp_create_sysfs(struct carp_net *);
void carp_destroy_sysfs(struct carp_net *);
void carp_prepare_sysfs_group(struct carp *);

#endif /* __CARP_H */
