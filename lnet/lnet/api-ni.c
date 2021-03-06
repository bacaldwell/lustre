/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2014, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#define DEBUG_SUBSYSTEM S_LNET
#include <lnet/lib-lnet.h>
#include <lnet/lib-dlc.h>
#ifdef __KERNEL__
#include <linux/log2.h>
#endif

#ifdef __KERNEL__
#define D_LNI D_CONSOLE
#else
#define D_LNI D_CONFIG
#endif

lnet_t      the_lnet;                           /* THE state of the network */
EXPORT_SYMBOL(the_lnet);

#ifdef __KERNEL__

static char *ip2nets = "";
CFS_MODULE_PARM(ip2nets, "s", charp, 0444,
                "LNET network <- IP table");

static char *networks = "";
CFS_MODULE_PARM(networks, "s", charp, 0444,
                "local networks");

static char *routes = "";
CFS_MODULE_PARM(routes, "s", charp, 0444,
                "routes to non-local networks");

static int rnet_htable_size = LNET_REMOTE_NETS_HASH_DEFAULT;
CFS_MODULE_PARM(rnet_htable_size, "i", int, 0444,
		"size of remote network hash table");

static int lnet_ping(lnet_process_id_t id, int timeout_ms,
		     lnet_process_id_t *ids, int n_ids);

static char *
lnet_get_routes(void)
{
        return routes;
}

static char *
lnet_get_networks(void)
{
	char   *nets;
	int     rc;

	if (*networks != 0 && *ip2nets != 0) {
		LCONSOLE_ERROR_MSG(0x101, "Please specify EITHER 'networks' or "
				   "'ip2nets' but not both at once\n");
		return NULL;
	}

	if (*ip2nets != 0) {
		rc = lnet_parse_ip2nets(&nets, ip2nets);
		return (rc == 0) ? nets : NULL;
	}

	if (*networks != 0)
		return networks;

	return "tcp";
}

static void
lnet_init_locks(void)
{
	spin_lock_init(&the_lnet.ln_eq_wait_lock);
	init_waitqueue_head(&the_lnet.ln_eq_waitq);
	mutex_init(&the_lnet.ln_lnd_mutex);
	mutex_init(&the_lnet.ln_api_mutex);
}

static void
lnet_fini_locks(void)
{
}

#else

static char *
lnet_get_routes(void)
{
        char *str = getenv("LNET_ROUTES");

        return (str == NULL) ? "" : str;
}

static char *
lnet_get_networks (void)
{
	static char	  default_networks[256];
	char		 *networks = getenv("LNET_NETWORKS");
	char		 *str;
	char		 *sep;
	int		  len;
	int		  nob;
	struct list_head *tmp;

	if (networks != NULL)
		return networks;

        /* In userland, the default 'networks=' is the list of known net types */
        len = sizeof(default_networks);
        str = default_networks;
        *str = 0;
        sep = "";

	list_for_each(tmp, &the_lnet.ln_lnds) {
		lnd_t *lnd = list_entry(tmp, lnd_t, lnd_list);

		nob = snprintf(str, len, "%s%s", sep,
			       libcfs_lnd2str(lnd->lnd_type));
		if (nob >= len) {
			/* overflowed the string; leave it where it was */
			*str = 0;
			break;
		}
		len -= nob;
		str += nob;
		sep = ",";
	}

	return default_networks;
}

# ifndef HAVE_LIBPTHREAD

static void lnet_init_locks(void)
{
	the_lnet.ln_eq_wait_lock = 0;
	the_lnet.ln_lnd_mutex = 0;
	the_lnet.ln_api_mutex = 0;
}

static void lnet_fini_locks(void)
{
	LASSERT(the_lnet.ln_api_mutex == 0);
	LASSERT(the_lnet.ln_lnd_mutex == 0);
	LASSERT(the_lnet.ln_eq_wait_lock == 0);
}

# else

static void lnet_init_locks(void)
{
	pthread_cond_init(&the_lnet.ln_eq_cond, NULL);
	pthread_mutex_init(&the_lnet.ln_eq_wait_lock, NULL);
	pthread_mutex_init(&the_lnet.ln_lnd_mutex, NULL);
	pthread_mutex_init(&the_lnet.ln_api_mutex, NULL);
}

static void lnet_fini_locks(void)
{
	pthread_mutex_destroy(&the_lnet.ln_api_mutex);
	pthread_mutex_destroy(&the_lnet.ln_lnd_mutex);
	pthread_mutex_destroy(&the_lnet.ln_eq_wait_lock);
	pthread_cond_destroy(&the_lnet.ln_eq_cond);
}

# endif
#endif

static int
lnet_create_remote_nets_table(void)
{
	int		  i;
	struct list_head *hash;

	LASSERT(the_lnet.ln_remote_nets_hash == NULL);
	LASSERT(the_lnet.ln_remote_nets_hbits > 0);
	LIBCFS_ALLOC(hash, LNET_REMOTE_NETS_HASH_SIZE * sizeof(*hash));
	if (hash == NULL) {
		CERROR("Failed to create remote nets hash table\n");
		return -ENOMEM;
	}

	for (i = 0; i < LNET_REMOTE_NETS_HASH_SIZE; i++)
		INIT_LIST_HEAD(&hash[i]);
	the_lnet.ln_remote_nets_hash = hash;
	return 0;
}

static void
lnet_destroy_remote_nets_table(void)
{
	int i;

	if (the_lnet.ln_remote_nets_hash == NULL)
		return;

	for (i = 0; i < LNET_REMOTE_NETS_HASH_SIZE; i++)
		LASSERT(list_empty(&the_lnet.ln_remote_nets_hash[i]));

	LIBCFS_FREE(the_lnet.ln_remote_nets_hash,
		    LNET_REMOTE_NETS_HASH_SIZE *
		    sizeof(the_lnet.ln_remote_nets_hash[0]));
	the_lnet.ln_remote_nets_hash = NULL;
}

static void
lnet_destroy_locks(void)
{
	if (the_lnet.ln_res_lock != NULL) {
		cfs_percpt_lock_free(the_lnet.ln_res_lock);
		the_lnet.ln_res_lock = NULL;
	}

	if (the_lnet.ln_net_lock != NULL) {
		cfs_percpt_lock_free(the_lnet.ln_net_lock);
		the_lnet.ln_net_lock = NULL;
	}

	lnet_fini_locks();
}

static int
lnet_create_locks(void)
{
	lnet_init_locks();

	the_lnet.ln_res_lock = cfs_percpt_lock_alloc(lnet_cpt_table());
	if (the_lnet.ln_res_lock == NULL)
		goto failed;

	the_lnet.ln_net_lock = cfs_percpt_lock_alloc(lnet_cpt_table());
	if (the_lnet.ln_net_lock == NULL)
		goto failed;

	return 0;

 failed:
	lnet_destroy_locks();
	return -ENOMEM;
}

static void lnet_assert_wire_constants(void)
{
        /* Wire protocol assertions generated by 'wirecheck'
         * running on Linux robert.bartonsoftware.com 2.6.8-1.521
         * #1 Mon Aug 16 09:01:18 EDT 2004 i686 athlon i386 GNU/Linux
         * with gcc version 3.3.3 20040412 (Red Hat Linux 3.3.3-7) */

        /* Constants... */
        CLASSERT (LNET_PROTO_TCP_MAGIC == 0xeebc0ded);
        CLASSERT (LNET_PROTO_TCP_VERSION_MAJOR == 1);
        CLASSERT (LNET_PROTO_TCP_VERSION_MINOR == 0);
        CLASSERT (LNET_MSG_ACK == 0);
        CLASSERT (LNET_MSG_PUT == 1);
        CLASSERT (LNET_MSG_GET == 2);
        CLASSERT (LNET_MSG_REPLY == 3);
        CLASSERT (LNET_MSG_HELLO == 4);

        /* Checks for struct ptl_handle_wire_t */
        CLASSERT ((int)sizeof(lnet_handle_wire_t) == 16);
        CLASSERT ((int)offsetof(lnet_handle_wire_t, wh_interface_cookie) == 0);
        CLASSERT ((int)sizeof(((lnet_handle_wire_t *)0)->wh_interface_cookie) == 8);
        CLASSERT ((int)offsetof(lnet_handle_wire_t, wh_object_cookie) == 8);
        CLASSERT ((int)sizeof(((lnet_handle_wire_t *)0)->wh_object_cookie) == 8);

        /* Checks for struct lnet_magicversion_t */
        CLASSERT ((int)sizeof(lnet_magicversion_t) == 8);
        CLASSERT ((int)offsetof(lnet_magicversion_t, magic) == 0);
        CLASSERT ((int)sizeof(((lnet_magicversion_t *)0)->magic) == 4);
        CLASSERT ((int)offsetof(lnet_magicversion_t, version_major) == 4);
        CLASSERT ((int)sizeof(((lnet_magicversion_t *)0)->version_major) == 2);
        CLASSERT ((int)offsetof(lnet_magicversion_t, version_minor) == 6);
        CLASSERT ((int)sizeof(((lnet_magicversion_t *)0)->version_minor) == 2);

        /* Checks for struct lnet_hdr_t */
        CLASSERT ((int)sizeof(lnet_hdr_t) == 72);
        CLASSERT ((int)offsetof(lnet_hdr_t, dest_nid) == 0);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->dest_nid) == 8);
        CLASSERT ((int)offsetof(lnet_hdr_t, src_nid) == 8);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->src_nid) == 8);
        CLASSERT ((int)offsetof(lnet_hdr_t, dest_pid) == 16);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->dest_pid) == 4);
        CLASSERT ((int)offsetof(lnet_hdr_t, src_pid) == 20);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->src_pid) == 4);
        CLASSERT ((int)offsetof(lnet_hdr_t, type) == 24);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->type) == 4);
        CLASSERT ((int)offsetof(lnet_hdr_t, payload_length) == 28);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->payload_length) == 4);
        CLASSERT ((int)offsetof(lnet_hdr_t, msg) == 32);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->msg) == 40);

        /* Ack */
        CLASSERT ((int)offsetof(lnet_hdr_t, msg.ack.dst_wmd) == 32);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->msg.ack.dst_wmd) == 16);
        CLASSERT ((int)offsetof(lnet_hdr_t, msg.ack.match_bits) == 48);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->msg.ack.match_bits) == 8);
        CLASSERT ((int)offsetof(lnet_hdr_t, msg.ack.mlength) == 56);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->msg.ack.mlength) == 4);

        /* Put */
        CLASSERT ((int)offsetof(lnet_hdr_t, msg.put.ack_wmd) == 32);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->msg.put.ack_wmd) == 16);
        CLASSERT ((int)offsetof(lnet_hdr_t, msg.put.match_bits) == 48);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->msg.put.match_bits) == 8);
        CLASSERT ((int)offsetof(lnet_hdr_t, msg.put.hdr_data) == 56);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->msg.put.hdr_data) == 8);
        CLASSERT ((int)offsetof(lnet_hdr_t, msg.put.ptl_index) == 64);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->msg.put.ptl_index) == 4);
        CLASSERT ((int)offsetof(lnet_hdr_t, msg.put.offset) == 68);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->msg.put.offset) == 4);

        /* Get */
        CLASSERT ((int)offsetof(lnet_hdr_t, msg.get.return_wmd) == 32);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->msg.get.return_wmd) == 16);
        CLASSERT ((int)offsetof(lnet_hdr_t, msg.get.match_bits) == 48);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->msg.get.match_bits) == 8);
        CLASSERT ((int)offsetof(lnet_hdr_t, msg.get.ptl_index) == 56);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->msg.get.ptl_index) == 4);
        CLASSERT ((int)offsetof(lnet_hdr_t, msg.get.src_offset) == 60);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->msg.get.src_offset) == 4);
        CLASSERT ((int)offsetof(lnet_hdr_t, msg.get.sink_length) == 64);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->msg.get.sink_length) == 4);

        /* Reply */
        CLASSERT ((int)offsetof(lnet_hdr_t, msg.reply.dst_wmd) == 32);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->msg.reply.dst_wmd) == 16);

        /* Hello */
        CLASSERT ((int)offsetof(lnet_hdr_t, msg.hello.incarnation) == 32);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->msg.hello.incarnation) == 8);
        CLASSERT ((int)offsetof(lnet_hdr_t, msg.hello.type) == 40);
        CLASSERT ((int)sizeof(((lnet_hdr_t *)0)->msg.hello.type) == 4);
}

static lnd_t *
lnet_find_lnd_by_type (int type)
{
	lnd_t		 *lnd;
	struct list_head *tmp;

	/* holding lnd mutex */
	list_for_each(tmp, &the_lnet.ln_lnds) {
		lnd = list_entry(tmp, lnd_t, lnd_list);

		if ((int)lnd->lnd_type == type)
			return lnd;
	}
	return NULL;
}

void
lnet_register_lnd (lnd_t *lnd)
{
	LNET_MUTEX_LOCK(&the_lnet.ln_lnd_mutex);

	LASSERT(the_lnet.ln_init);
	LASSERT(libcfs_isknown_lnd(lnd->lnd_type));
	LASSERT(lnet_find_lnd_by_type(lnd->lnd_type) == NULL);

	list_add_tail(&lnd->lnd_list, &the_lnet.ln_lnds);
	lnd->lnd_refcount = 0;

	CDEBUG(D_NET, "%s LND registered\n", libcfs_lnd2str(lnd->lnd_type));

	LNET_MUTEX_UNLOCK(&the_lnet.ln_lnd_mutex);
}
EXPORT_SYMBOL(lnet_register_lnd);

void
lnet_unregister_lnd (lnd_t *lnd)
{
	LNET_MUTEX_LOCK(&the_lnet.ln_lnd_mutex);

	LASSERT(the_lnet.ln_init);
	LASSERT(lnet_find_lnd_by_type(lnd->lnd_type) == lnd);
	LASSERT(lnd->lnd_refcount == 0);

	list_del(&lnd->lnd_list);
	CDEBUG(D_NET, "%s LND unregistered\n", libcfs_lnd2str(lnd->lnd_type));

	LNET_MUTEX_UNLOCK(&the_lnet.ln_lnd_mutex);
}
EXPORT_SYMBOL(lnet_unregister_lnd);

void
lnet_counters_get(lnet_counters_t *counters)
{
	lnet_counters_t *ctr;
	int		i;

	memset(counters, 0, sizeof(*counters));

	lnet_net_lock(LNET_LOCK_EX);

	cfs_percpt_for_each(ctr, i, the_lnet.ln_counters) {
		counters->msgs_max     += ctr->msgs_max;
		counters->msgs_alloc   += ctr->msgs_alloc;
		counters->errors       += ctr->errors;
		counters->send_count   += ctr->send_count;
		counters->recv_count   += ctr->recv_count;
		counters->route_count  += ctr->route_count;
		counters->drop_count   += ctr->drop_count;
		counters->send_length  += ctr->send_length;
		counters->recv_length  += ctr->recv_length;
		counters->route_length += ctr->route_length;
		counters->drop_length  += ctr->drop_length;

	}
	lnet_net_unlock(LNET_LOCK_EX);
}
EXPORT_SYMBOL(lnet_counters_get);

void
lnet_counters_reset(void)
{
	lnet_counters_t *counters;
	int		i;

	lnet_net_lock(LNET_LOCK_EX);

	cfs_percpt_for_each(counters, i, the_lnet.ln_counters)
		memset(counters, 0, sizeof(lnet_counters_t));

	lnet_net_unlock(LNET_LOCK_EX);
}
EXPORT_SYMBOL(lnet_counters_reset);

#ifdef LNET_USE_LIB_FREELIST

int
lnet_freelist_init(lnet_freelist_t *fl, int n, int size)
{
        char *space;

        LASSERT (n > 0);

        size += offsetof (lnet_freeobj_t, fo_contents);

        LIBCFS_ALLOC(space, n * size);
        if (space == NULL)
                return (-ENOMEM);

	INIT_LIST_HEAD(&fl->fl_list);
	fl->fl_objs = space;
	fl->fl_nobjs = n;
	fl->fl_objsize = size;

	do {
		list_add((struct list_head *)space, &fl->fl_list);
		space += size;
	} while (--n != 0);

	return 0;
}

void
lnet_freelist_fini(lnet_freelist_t *fl)
{
	struct list_head *el;
	int		  count;

        if (fl->fl_nobjs == 0)
                return;

        count = 0;
        for (el = fl->fl_list.next; el != &fl->fl_list; el = el->next)
                count++;

        LASSERT (count == fl->fl_nobjs);

        LIBCFS_FREE(fl->fl_objs, fl->fl_nobjs * fl->fl_objsize);
        memset (fl, 0, sizeof (*fl));
}

#endif /* LNET_USE_LIB_FREELIST */

static __u64 lnet_create_interface_cookie(void)
{
	/* NB the interface cookie in wire handles guards against delayed
	 * replies and ACKs appearing valid after reboot. Initialisation time,
	 * even if it's only implemented to millisecond resolution is probably
	 * easily good enough. */
	struct timeval tv;
	__u64          cookie;
#ifndef __KERNEL__
	int            rc = gettimeofday (&tv, NULL);
	LASSERT (rc == 0);
#else
	do_gettimeofday(&tv);
#endif
	cookie = tv.tv_sec;
	cookie *= 1000000;
	cookie += tv.tv_usec;
	return cookie;
}

static char *
lnet_res_type2str(int type)
{
	switch (type) {
	default:
		LBUG();
	case LNET_COOKIE_TYPE_MD:
		return "MD";
	case LNET_COOKIE_TYPE_ME:
		return "ME";
	case LNET_COOKIE_TYPE_EQ:
		return "EQ";
	}
}

static void
lnet_res_container_cleanup(struct lnet_res_container *rec)
{
	int	count = 0;

	if (rec->rec_type == 0) /* not set yet, it's uninitialized */
		return;

	while (!list_empty(&rec->rec_active)) {
		struct list_head *e = rec->rec_active.next;

		list_del_init(e);
		if (rec->rec_type == LNET_COOKIE_TYPE_EQ) {
			lnet_eq_free(list_entry(e, lnet_eq_t, eq_list));

		} else if (rec->rec_type == LNET_COOKIE_TYPE_MD) {
			lnet_md_free(list_entry(e, lnet_libmd_t, md_list));

		} else { /* NB: Active MEs should be attached on portals */
			LBUG();
		}
		count++;
	}

	if (count > 0) {
		/* Found alive MD/ME/EQ, user really should unlink/free
		 * all of them before finalize LNet, but if someone didn't,
		 * we have to recycle garbage for him */
		CERROR("%d active elements on exit of %s container\n",
		       count, lnet_res_type2str(rec->rec_type));
	}

#ifdef LNET_USE_LIB_FREELIST
	lnet_freelist_fini(&rec->rec_freelist);
#endif
	if (rec->rec_lh_hash != NULL) {
		LIBCFS_FREE(rec->rec_lh_hash,
			    LNET_LH_HASH_SIZE * sizeof(rec->rec_lh_hash[0]));
		rec->rec_lh_hash = NULL;
	}

	rec->rec_type = 0; /* mark it as finalized */
}

static int
lnet_res_container_setup(struct lnet_res_container *rec,
			 int cpt, int type, int objnum, int objsz)
{
	int	rc = 0;
	int	i;

	LASSERT(rec->rec_type == 0);

	rec->rec_type = type;
	INIT_LIST_HEAD(&rec->rec_active);

#ifdef LNET_USE_LIB_FREELIST
	memset(&rec->rec_freelist, 0, sizeof(rec->rec_freelist));
	rc = lnet_freelist_init(&rec->rec_freelist, objnum, objsz);
	if (rc != 0)
		goto out;
#endif
	rec->rec_lh_cookie = (cpt << LNET_COOKIE_TYPE_BITS) | type;

	/* Arbitrary choice of hash table size */
	LIBCFS_CPT_ALLOC(rec->rec_lh_hash, lnet_cpt_table(), cpt,
			 LNET_LH_HASH_SIZE * sizeof(rec->rec_lh_hash[0]));
	if (rec->rec_lh_hash == NULL) {
		rc = -ENOMEM;
		goto out;
	}

	for (i = 0; i < LNET_LH_HASH_SIZE; i++)
		INIT_LIST_HEAD(&rec->rec_lh_hash[i]);

	return 0;

out:
	CERROR("Failed to setup %s resource container\n",
	       lnet_res_type2str(type));
	lnet_res_container_cleanup(rec);
	return rc;
}

static void
lnet_res_containers_destroy(struct lnet_res_container **recs)
{
	struct lnet_res_container	*rec;
	int				i;

	cfs_percpt_for_each(rec, i, recs)
		lnet_res_container_cleanup(rec);

	cfs_percpt_free(recs);
}

static struct lnet_res_container **
lnet_res_containers_create(int type, int objnum, int objsz)
{
	struct lnet_res_container	**recs;
	struct lnet_res_container	*rec;
	int				rc;
	int				i;

	recs = cfs_percpt_alloc(lnet_cpt_table(), sizeof(*rec));
	if (recs == NULL) {
		CERROR("Failed to allocate %s resource containers\n",
		       lnet_res_type2str(type));
		return NULL;
	}

	cfs_percpt_for_each(rec, i, recs) {
		rc = lnet_res_container_setup(rec, i, type, objnum, objsz);
		if (rc != 0) {
			lnet_res_containers_destroy(recs);
			return NULL;
		}
	}

	return recs;
}

lnet_libhandle_t *
lnet_res_lh_lookup(struct lnet_res_container *rec, __u64 cookie)
{
	/* ALWAYS called with lnet_res_lock held */
	struct list_head	*head;
	lnet_libhandle_t	*lh;
	unsigned int		hash;

	if ((cookie & LNET_COOKIE_MASK) != rec->rec_type)
		return NULL;

	hash = cookie >> (LNET_COOKIE_TYPE_BITS + LNET_CPT_BITS);
	head = &rec->rec_lh_hash[hash & LNET_LH_HASH_MASK];

	list_for_each_entry(lh, head, lh_hash_chain) {
		if (lh->lh_cookie == cookie)
			return lh;
	}

	return NULL;
}

void
lnet_res_lh_initialize(struct lnet_res_container *rec, lnet_libhandle_t *lh)
{
	/* ALWAYS called with lnet_res_lock held */
	unsigned int	ibits = LNET_COOKIE_TYPE_BITS + LNET_CPT_BITS;
	unsigned int	hash;

	lh->lh_cookie = rec->rec_lh_cookie;
	rec->rec_lh_cookie += 1 << ibits;

	hash = (lh->lh_cookie >> ibits) & LNET_LH_HASH_MASK;

	list_add(&lh->lh_hash_chain, &rec->rec_lh_hash[hash]);
}

#ifndef __KERNEL__
/**
 * Reserved API - do not use.
 * Temporary workaround to allow uOSS and test programs force server
 * mode in userspace. See comments near ln_server_mode_flag in
 * lnet/lib-types.h */

void
lnet_server_mode() {
        the_lnet.ln_server_mode_flag = 1;
}
#endif

static int lnet_unprepare(void);

static int
lnet_prepare(lnet_pid_t requested_pid)
{
	/* Prepare to bring up the network */
	struct lnet_res_container **recs;
	int			  rc = 0;

	if (requested_pid == LNET_PID_ANY) {
		/* Don't instantiate LNET just for me */
		return -ENETDOWN;
	}

        LASSERT (the_lnet.ln_refcount == 0);

        the_lnet.ln_routing = 0;

#ifdef __KERNEL__
        LASSERT ((requested_pid & LNET_PID_USERFLAG) == 0);
        the_lnet.ln_pid = requested_pid;
#else
        if (the_lnet.ln_server_mode_flag) {/* server case (uOSS) */
		LASSERT ((requested_pid & LNET_PID_USERFLAG) == 0);

		if (current_uid() != 0)	/* Only root can run user-space server */
			return -EPERM;
		the_lnet.ln_pid = requested_pid;

        } else {/* client case (liblustre) */

                /* My PID must be unique on this node and flag I'm userspace */
                the_lnet.ln_pid = getpid() | LNET_PID_USERFLAG;
        }
#endif

	INIT_LIST_HEAD(&the_lnet.ln_test_peers);
	INIT_LIST_HEAD(&the_lnet.ln_nis);
	INIT_LIST_HEAD(&the_lnet.ln_nis_cpt);
	INIT_LIST_HEAD(&the_lnet.ln_nis_zombie);
	INIT_LIST_HEAD(&the_lnet.ln_routers);
	INIT_LIST_HEAD(&the_lnet.ln_drop_rules);
	INIT_LIST_HEAD(&the_lnet.ln_delay_rules);

	rc = lnet_create_remote_nets_table();
	if (rc != 0)
		goto failed;

	the_lnet.ln_interface_cookie = lnet_create_interface_cookie();

	the_lnet.ln_counters = cfs_percpt_alloc(lnet_cpt_table(),
						sizeof(lnet_counters_t));
	if (the_lnet.ln_counters == NULL) {
		CERROR("Failed to allocate counters for LNet\n");
		rc = -ENOMEM;
		goto failed;
	}

	rc = lnet_peer_tables_create();
	if (rc != 0)
		goto failed;

	rc = lnet_msg_containers_create();
	if (rc != 0)
		goto failed;

	rc = lnet_res_container_setup(&the_lnet.ln_eq_container, 0,
				      LNET_COOKIE_TYPE_EQ, LNET_FL_MAX_EQS,
				      sizeof(lnet_eq_t));
	if (rc != 0)
		goto failed;

	recs = lnet_res_containers_create(LNET_COOKIE_TYPE_ME, LNET_FL_MAX_MES,
					  sizeof(lnet_me_t));
	if (recs == NULL)
		goto failed;

	the_lnet.ln_me_containers = recs;

	recs = lnet_res_containers_create(LNET_COOKIE_TYPE_MD, LNET_FL_MAX_MDS,
					  sizeof(lnet_libmd_t));
	if (recs == NULL)
		goto failed;

	the_lnet.ln_md_containers = recs;

	rc = lnet_portals_create();
	if (rc != 0) {
		CERROR("Failed to create portals for LNet: %d\n", rc);
		goto failed;
	}

	return 0;

 failed:
	lnet_unprepare();
	return rc;
}

static int
lnet_unprepare (void)
{
	/* NB no LNET_LOCK since this is the last reference.  All LND instances
	 * have shut down already, so it is safe to unlink and free all
	 * descriptors, even those that appear committed to a network op (eg MD
	 * with non-zero pending count) */

	lnet_fail_nid(LNET_NID_ANY, 0);

	LASSERT(the_lnet.ln_refcount == 0);
	LASSERT(list_empty(&the_lnet.ln_test_peers));
	LASSERT(list_empty(&the_lnet.ln_nis));
	LASSERT(list_empty(&the_lnet.ln_nis_cpt));
	LASSERT(list_empty(&the_lnet.ln_nis_zombie));

	lnet_portals_destroy();

	if (the_lnet.ln_md_containers != NULL) {
		lnet_res_containers_destroy(the_lnet.ln_md_containers);
		the_lnet.ln_md_containers = NULL;
	}

	if (the_lnet.ln_me_containers != NULL) {
		lnet_res_containers_destroy(the_lnet.ln_me_containers);
		the_lnet.ln_me_containers = NULL;
	}

	lnet_res_container_cleanup(&the_lnet.ln_eq_container);

	lnet_msg_containers_destroy();
	lnet_peer_tables_destroy();
	lnet_rtrpools_free(0);

	if (the_lnet.ln_counters != NULL) {
		cfs_percpt_free(the_lnet.ln_counters);
		the_lnet.ln_counters = NULL;
	}
	lnet_destroy_remote_nets_table();

	return 0;
}

lnet_ni_t  *
lnet_net2ni_locked(__u32 net, int cpt)
{
	struct list_head *tmp;
	lnet_ni_t	 *ni;

	LASSERT(cpt != LNET_LOCK_EX);

	list_for_each(tmp, &the_lnet.ln_nis) {
		ni = list_entry(tmp, lnet_ni_t, ni_list);

		if (LNET_NIDNET(ni->ni_nid) == net) {
			lnet_ni_addref_locked(ni, cpt);
			return ni;
		}
	}

	return NULL;
}

lnet_ni_t *
lnet_net2ni(__u32 net)
{
	lnet_ni_t *ni;

	lnet_net_lock(0);
	ni = lnet_net2ni_locked(net, 0);
	lnet_net_unlock(0);

	return ni;
}
EXPORT_SYMBOL(lnet_net2ni);

static unsigned int
lnet_nid_cpt_hash(lnet_nid_t nid, unsigned int number)
{
	__u64		key = nid;
	unsigned int	val;

	LASSERT(number >= 1 && number <= LNET_CPT_NUMBER);

	if (number == 1)
		return 0;

	val = hash_long(key, LNET_CPT_BITS);
	/* NB: LNET_CP_NUMBER doesn't have to be PO2 */
	if (val < number)
		return val;

	return (unsigned int)(key + val + (val >> 1)) % number;
}

int
lnet_cpt_of_nid_locked(lnet_nid_t nid)
{
	struct lnet_ni *ni;

	/* must called with hold of lnet_net_lock */
	if (LNET_CPT_NUMBER == 1)
		return 0; /* the only one */

	/* take lnet_net_lock(any) would be OK */
	if (!list_empty(&the_lnet.ln_nis_cpt)) {
		list_for_each_entry(ni, &the_lnet.ln_nis_cpt, ni_cptlist) {
			if (LNET_NIDNET(ni->ni_nid) != LNET_NIDNET(nid))
				continue;

			LASSERT(ni->ni_cpts != NULL);
			return ni->ni_cpts[lnet_nid_cpt_hash
					   (nid, ni->ni_ncpts)];
		}
	}

	return lnet_nid_cpt_hash(nid, LNET_CPT_NUMBER);
}

int
lnet_cpt_of_nid(lnet_nid_t nid)
{
	int	cpt;
	int	cpt2;

	if (LNET_CPT_NUMBER == 1)
		return 0; /* the only one */

	if (list_empty(&the_lnet.ln_nis_cpt))
		return lnet_nid_cpt_hash(nid, LNET_CPT_NUMBER);

	cpt = lnet_net_lock_current();
	cpt2 = lnet_cpt_of_nid_locked(nid);
	lnet_net_unlock(cpt);

	return cpt2;
}
EXPORT_SYMBOL(lnet_cpt_of_nid);

int
lnet_islocalnet(__u32 net)
{
	struct lnet_ni	*ni;
	int		cpt;

	cpt = lnet_net_lock_current();

	ni = lnet_net2ni_locked(net, cpt);
	if (ni != NULL)
		lnet_ni_decref_locked(ni, cpt);

	lnet_net_unlock(cpt);

	return ni != NULL;
}

lnet_ni_t  *
lnet_nid2ni_locked(lnet_nid_t nid, int cpt)
{
	struct lnet_ni	 *ni;
	struct list_head *tmp;

	LASSERT(cpt != LNET_LOCK_EX);

	list_for_each(tmp, &the_lnet.ln_nis) {
		ni = list_entry(tmp, lnet_ni_t, ni_list);

		if (ni->ni_nid == nid) {
			lnet_ni_addref_locked(ni, cpt);
			return ni;
		}
	}

	return NULL;
}

int
lnet_islocalnid(lnet_nid_t nid)
{
	struct lnet_ni	*ni;
	int		cpt;

	cpt = lnet_net_lock_current();
	ni = lnet_nid2ni_locked(nid, cpt);
	if (ni != NULL)
		lnet_ni_decref_locked(ni, cpt);
	lnet_net_unlock(cpt);

	return ni != NULL;
}

int
lnet_count_acceptor_nis (void)
{
	/* Return the # of NIs that need the acceptor. */
	int		 count = 0;
#if defined(__KERNEL__) || defined(HAVE_LIBPTHREAD)
	struct list_head *tmp;
	struct lnet_ni	 *ni;
	int		 cpt;

	cpt = lnet_net_lock_current();
	list_for_each(tmp, &the_lnet.ln_nis) {
		ni = list_entry(tmp, lnet_ni_t, ni_list);

		if (ni->ni_lnd->lnd_accept != NULL)
			count++;
	}

	lnet_net_unlock(cpt);

#endif /* defined(__KERNEL__) || defined(HAVE_LIBPTHREAD) */
	return count;
}

static lnet_ping_info_t *
lnet_ping_info_create(int num_ni)
{
	lnet_ping_info_t *ping_info;
	unsigned int	 infosz;

	infosz = offsetof(lnet_ping_info_t, pi_ni[num_ni]);
	LIBCFS_ALLOC(ping_info, infosz);
	if (ping_info == NULL) {
		CERROR("Can't allocate ping info[%d]\n", num_ni);
		return NULL;
	}

	ping_info->pi_nnis = num_ni;
	ping_info->pi_pid = the_lnet.ln_pid;
	ping_info->pi_magic = LNET_PROTO_PING_MAGIC;
	ping_info->pi_features = LNET_PING_FEAT_NI_STATUS;

	return ping_info;
}

static inline int
lnet_get_ni_count(void)
{
	struct lnet_ni *ni;
	int	       count = 0;

	lnet_net_lock(0);

	list_for_each_entry(ni, &the_lnet.ln_nis, ni_list)
		count++;

	lnet_net_unlock(0);

	return count;
}

static inline void
lnet_ping_info_free(lnet_ping_info_t *pinfo)
{
	LIBCFS_FREE(pinfo,
		    offsetof(lnet_ping_info_t,
			     pi_ni[pinfo->pi_nnis]));
}

static void
lnet_ping_info_destroy(void)
{
	struct lnet_ni	*ni;

	lnet_net_lock(LNET_LOCK_EX);

	list_for_each_entry(ni, &the_lnet.ln_nis, ni_list) {
		lnet_ni_lock(ni);
		ni->ni_status = NULL;
		lnet_ni_unlock(ni);
	}

	lnet_ping_info_free(the_lnet.ln_ping_info);
	the_lnet.ln_ping_info = NULL;

	lnet_net_unlock(LNET_LOCK_EX);
}

static void
lnet_ping_event_handler(lnet_event_t *event)
{
	lnet_ping_info_t *pinfo = event->md.user_ptr;

	if (event->unlinked)
		pinfo->pi_features = LNET_PING_FEAT_INVAL;
}

static int
lnet_ping_info_setup(lnet_ping_info_t **ppinfo, lnet_handle_md_t *md_handle,
		     int ni_count, bool set_eq)
{
	lnet_handle_me_t  me_handle;
	lnet_process_id_t id = {LNET_NID_ANY, LNET_PID_ANY};
	lnet_md_t	  md = {NULL};
	int		  rc, rc2;

	if (set_eq) {
		rc = LNetEQAlloc(0, lnet_ping_event_handler,
				 &the_lnet.ln_ping_target_eq);
		if (rc != 0) {
			CERROR("Can't allocate ping EQ: %d\n", rc);
			return rc;
		}
	}

	*ppinfo = lnet_ping_info_create(ni_count);
	if (*ppinfo == NULL) {
		rc = -ENOMEM;
		goto failed_0;
	}

	rc = LNetMEAttach(LNET_RESERVED_PORTAL, id,
			  LNET_PROTO_PING_MATCHBITS, 0,
			  LNET_UNLINK, LNET_INS_AFTER,
			  &me_handle);
	if (rc != 0) {
		CERROR("Can't create ping ME: %d\n", rc);
		goto failed_1;
	}

	/* initialize md content */
	md.start     = *ppinfo;
	md.length    = offsetof(lnet_ping_info_t,
				pi_ni[(*ppinfo)->pi_nnis]);
	md.threshold = LNET_MD_THRESH_INF;
	md.max_size  = 0;
	md.options   = LNET_MD_OP_GET | LNET_MD_TRUNCATE |
		       LNET_MD_MANAGE_REMOTE;
	md.user_ptr  = NULL;
	md.eq_handle = the_lnet.ln_ping_target_eq;
	md.user_ptr = *ppinfo;

	rc = LNetMDAttach(me_handle, md, LNET_RETAIN, md_handle);
	if (rc != 0) {
		CERROR("Can't attach ping MD: %d\n", rc);
		goto failed_2;
	}

	return 0;

failed_2:
	rc2 = LNetMEUnlink(me_handle);
	LASSERT(rc2 == 0);
failed_1:
	lnet_ping_info_free(*ppinfo);
	*ppinfo = NULL;
failed_0:
	if (set_eq)
		LNetEQFree(the_lnet.ln_ping_target_eq);
	return rc;
}

static void
lnet_ping_md_unlink(lnet_ping_info_t *pinfo, lnet_handle_md_t *md_handle)
{
	sigset_t	blocked = cfs_block_allsigs();

	LNetMDUnlink(*md_handle);
	LNetInvalidateHandle(md_handle);

	/* NB md could be busy; this just starts the unlink */
	while (pinfo->pi_features != LNET_PING_FEAT_INVAL) {
		CDEBUG(D_NET, "Still waiting for ping MD to unlink\n");
		cfs_pause(cfs_time_seconds(1));
	}

	cfs_restore_sigs(blocked);
}

static void
lnet_ping_info_install_locked(lnet_ping_info_t *ping_info)
{
	int			i;
	lnet_ni_t		*ni;
	lnet_ni_status_t	*ns;

	i = 0;
	list_for_each_entry(ni, &the_lnet.ln_nis, ni_list) {
		LASSERT(i < ping_info->pi_nnis);

		ns = &ping_info->pi_ni[i];

		ns->ns_nid = ni->ni_nid;

		lnet_ni_lock(ni);
		ns->ns_status = (ni->ni_status != NULL) ?
				ni->ni_status->ns_status : LNET_NI_STATUS_UP;
		ni->ni_status = ns;
		lnet_ni_unlock(ni);

		i++;
	}
}

static void
lnet_ping_target_update(lnet_ping_info_t *pinfo, lnet_handle_md_t md_handle)
{
	lnet_ping_info_t *old_pinfo = NULL;
	lnet_handle_md_t old_md;

	/* switch the NIs to point to the new ping info created */
	lnet_net_lock(LNET_LOCK_EX);

	if (!the_lnet.ln_routing)
		pinfo->pi_features |= LNET_PING_FEAT_RTE_DISABLED;
	lnet_ping_info_install_locked(pinfo);

	if (the_lnet.ln_ping_info != NULL) {
		old_pinfo = the_lnet.ln_ping_info;
		old_md = the_lnet.ln_ping_target_md;
	}
	the_lnet.ln_ping_target_md = md_handle;
	the_lnet.ln_ping_info = pinfo;

	lnet_net_unlock(LNET_LOCK_EX);

	if (old_pinfo != NULL) {
		/* unlink the old ping info */
		lnet_ping_md_unlink(old_pinfo, &old_md);
		lnet_ping_info_free(old_pinfo);
	}
}

static void
lnet_ping_target_fini(void)
{
	int             rc;

	lnet_ping_md_unlink(the_lnet.ln_ping_info,
			    &the_lnet.ln_ping_target_md);

	rc = LNetEQFree(the_lnet.ln_ping_target_eq);
	LASSERT(rc == 0);

	lnet_ping_info_destroy();
}

static int
lnet_ni_tq_credits(lnet_ni_t *ni)
{
	int	credits;

	LASSERT(ni->ni_ncpts >= 1);

	if (ni->ni_ncpts == 1)
		return ni->ni_maxtxcredits;

	credits = ni->ni_maxtxcredits / ni->ni_ncpts;
	credits = max(credits, 8 * ni->ni_peertxcredits);
	credits = min(credits, ni->ni_maxtxcredits);

	return credits;
}

static void
lnet_ni_unlink_locked(lnet_ni_t *ni)
{
	if (!list_empty(&ni->ni_cptlist)) {
		list_del_init(&ni->ni_cptlist);
		lnet_ni_decref_locked(ni, 0);
	}

	/* move it to zombie list and nobody can find it anymore */
	LASSERT(!list_empty(&ni->ni_list));
	list_move(&ni->ni_list, &the_lnet.ln_nis_zombie);
	lnet_ni_decref_locked(ni, 0);	/* drop ln_nis' ref */
}

static void
lnet_clear_zombies_nis_locked(void)
{
	int		i;
	int		islo;
	lnet_ni_t	*ni;

	/* Now wait for the NI's I just nuked to show up on ln_zombie_nis
	 * and shut them down in guaranteed thread context */
	i = 2;
	while (!list_empty(&the_lnet.ln_nis_zombie)) {
		int	*ref;
		int	j;

		ni = list_entry(the_lnet.ln_nis_zombie.next,
				lnet_ni_t, ni_list);
		list_del_init(&ni->ni_list);
		cfs_percpt_for_each(ref, j, ni->ni_refs) {
			if (*ref == 0)
				continue;
			/* still busy, add it back to zombie list */
			list_add(&ni->ni_list, &the_lnet.ln_nis_zombie);
			break;
		}

		if (!list_empty(&ni->ni_list)) {
			lnet_net_unlock(LNET_LOCK_EX);
			++i;
			if ((i & (-i)) == i) {
				CDEBUG(D_WARNING,
				       "Waiting for zombie LNI %s\n",
				       libcfs_nid2str(ni->ni_nid));
			}
			cfs_pause(cfs_time_seconds(1));
			lnet_net_lock(LNET_LOCK_EX);
			continue;
		}

		ni->ni_lnd->lnd_refcount--;
		lnet_net_unlock(LNET_LOCK_EX);

		islo = ni->ni_lnd->lnd_type == LOLND;

		LASSERT(!in_interrupt());
		(ni->ni_lnd->lnd_shutdown)(ni);

		/* can't deref lnd anymore now; it might have unregistered
		 * itself...  */

		if (!islo)
			CDEBUG(D_LNI, "Removed LNI %s\n",
			      libcfs_nid2str(ni->ni_nid));

		lnet_ni_free(ni);
		i = 2;
		lnet_net_lock(LNET_LOCK_EX);
	}
}

static void
lnet_shutdown_lndnis(void)
{
	int		i;
	lnet_ni_t	*ni;

	/* NB called holding the global mutex */

	/* All quiet on the API front */
	LASSERT(!the_lnet.ln_shutdown);
	LASSERT(the_lnet.ln_refcount == 0);
	LASSERT(list_empty(&the_lnet.ln_nis_zombie));

	lnet_net_lock(LNET_LOCK_EX);
	the_lnet.ln_shutdown = 1;	/* flag shutdown */

	/* Unlink NIs from the global table */
	while (!list_empty(&the_lnet.ln_nis)) {
		ni = list_entry(the_lnet.ln_nis.next,
				lnet_ni_t, ni_list);
		lnet_ni_unlink_locked(ni);
	}

	/* Drop the cached eqwait NI. */
	if (the_lnet.ln_eq_waitni != NULL) {
		lnet_ni_decref_locked(the_lnet.ln_eq_waitni, 0);
		the_lnet.ln_eq_waitni = NULL;
	}

	/* Drop the cached loopback NI. */
	if (the_lnet.ln_loni != NULL) {
		lnet_ni_decref_locked(the_lnet.ln_loni, 0);
		the_lnet.ln_loni = NULL;
	}

	lnet_net_unlock(LNET_LOCK_EX);

	/* Clear lazy portals and drop delayed messages which hold refs
	 * on their lnet_msg_t::msg_rxpeer */
	for (i = 0; i < the_lnet.ln_nportals; i++)
		LNetClearLazyPortal(i);

	/* Clear the peer table and wait for all peers to go (they hold refs on
	 * their NIs) */
	lnet_peer_tables_cleanup(NULL);

	lnet_net_lock(LNET_LOCK_EX);

	lnet_clear_zombies_nis_locked();
	the_lnet.ln_shutdown = 0;
	lnet_net_unlock(LNET_LOCK_EX);
}

/* shutdown down the NI and release refcount */
static void
lnet_shutdown_lndni(struct lnet_ni *ni)
{
	lnet_net_lock(LNET_LOCK_EX);
	lnet_ni_unlink_locked(ni);
	lnet_net_unlock(LNET_LOCK_EX);

	/* Do peer table cleanup for this ni */
	lnet_peer_tables_cleanup(ni);

	lnet_net_lock(LNET_LOCK_EX);
	lnet_clear_zombies_nis_locked();
	lnet_net_unlock(LNET_LOCK_EX);
}

static int
lnet_startup_lndni(struct lnet_ni *ni, __s32 peer_timeout,
		   __s32 peer_cr, __s32 peer_buf_cr, __s32 credits)
{
	int			rc = 0;
	int			lnd_type;
	lnd_t			*lnd;
	struct lnet_tx_queue	*tq;
	int			i;

	lnd_type = LNET_NETTYP(LNET_NIDNET(ni->ni_nid));

	LASSERT(libcfs_isknown_lnd(lnd_type));

	if (lnd_type == CIBLND || lnd_type == OPENIBLND ||
	    lnd_type == IIBLND || lnd_type == VIBLND) {
		CERROR("LND %s obsoleted\n", libcfs_lnd2str(lnd_type));
		goto failed0;
	}

	/* Make sure this new NI is unique. */
	lnet_net_lock(LNET_LOCK_EX);
	if (!lnet_net_unique(LNET_NIDNET(ni->ni_nid), &the_lnet.ln_nis)) {
		if (lnd_type == LOLND) {
			lnet_net_unlock(LNET_LOCK_EX);
			lnet_ni_free(ni);
			return 0;
		}
		lnet_net_unlock(LNET_LOCK_EX);

		CERROR("Net %s is not unique\n",
		       libcfs_net2str(LNET_NIDNET(ni->ni_nid)));
		goto failed0;
	}
	lnet_net_unlock(LNET_LOCK_EX);

	LNET_MUTEX_LOCK(&the_lnet.ln_lnd_mutex);
	lnd = lnet_find_lnd_by_type(lnd_type);

#ifdef __KERNEL__
	if (lnd == NULL) {
		LNET_MUTEX_UNLOCK(&the_lnet.ln_lnd_mutex);
		rc = request_module("%s",
				    libcfs_lnd2modname(lnd_type));
		LNET_MUTEX_LOCK(&the_lnet.ln_lnd_mutex);

		lnd = lnet_find_lnd_by_type(lnd_type);
		if (lnd == NULL) {
			LNET_MUTEX_UNLOCK(&the_lnet.ln_lnd_mutex);
			CERROR("Can't load LND %s, module %s, rc=%d\n",
			       libcfs_lnd2str(lnd_type),
			       libcfs_lnd2modname(lnd_type), rc);
#ifndef HAVE_MODULE_LOADING_SUPPORT
			LCONSOLE_ERROR_MSG(0x104, "Your kernel must be "
					   "compiled with kernel module "
					   "loading support.");
#endif
			goto failed0;
		}
	}
#else
	if (lnd == NULL) {
		LNET_MUTEX_UNLOCK(&the_lnet.ln_lnd_mutex);
		CERROR("LND %s not supported\n",
			libcfs_lnd2str(lnd_type));
		goto failed0;
	}
#endif

	lnet_net_lock(LNET_LOCK_EX);
	lnd->lnd_refcount++;
	lnet_net_unlock(LNET_LOCK_EX);

	ni->ni_lnd = lnd;

	rc = (lnd->lnd_startup)(ni);

	LNET_MUTEX_UNLOCK(&the_lnet.ln_lnd_mutex);

	if (rc != 0) {
		LCONSOLE_ERROR_MSG(0x105, "Error %d starting up LNI %s\n",
				   rc, libcfs_lnd2str(lnd->lnd_type));
		lnet_net_lock(LNET_LOCK_EX);
		lnd->lnd_refcount--;
		lnet_net_unlock(LNET_LOCK_EX);
		goto failed0;
	}

	/* If given some LND tunable parameters, parse those now to
	 * override the values in the NI structure. */
	if (peer_buf_cr >= 0)
		ni->ni_peerrtrcredits = peer_buf_cr;
	if (peer_timeout >= 0)
		ni->ni_peertimeout = peer_timeout;
	/*
	 * TODO
	 * Note: For now, don't allow the user to change
	 * peertxcredits as this number is used in the
	 * IB LND to control queue depth.
	 * if (peer_cr != -1)
	 *	ni->ni_peertxcredits = peer_cr;
	 */
	if (credits >= 0)
		ni->ni_maxtxcredits = credits;

	LASSERT(ni->ni_peertimeout <= 0 || lnd->lnd_query != NULL);

	lnet_net_lock(LNET_LOCK_EX);
	/* refcount for ln_nis */
	lnet_ni_addref_locked(ni, 0);
	list_add_tail(&ni->ni_list, &the_lnet.ln_nis);
	if (ni->ni_cpts != NULL) {
		lnet_ni_addref_locked(ni, 0);
		list_add_tail(&ni->ni_cptlist, &the_lnet.ln_nis_cpt);
	}

	lnet_net_unlock(LNET_LOCK_EX);

	if (lnd->lnd_type == LOLND) {
		lnet_ni_addref(ni);
		LASSERT(the_lnet.ln_loni == NULL);
		the_lnet.ln_loni = ni;
		return 0;
	}

#ifndef __KERNEL__
	if (lnd->lnd_wait != NULL) {
		if (the_lnet.ln_eq_waitni == NULL) {
			lnet_ni_addref(ni);
			the_lnet.ln_eq_waitni = ni;
		}
	} else {
# ifndef HAVE_LIBPTHREAD
		LCONSOLE_ERROR_MSG(0x106, "LND %s not supported in a "
					"single-threaded runtime\n",
					libcfs_lnd2str(lnd_type));
		/* shutdown the NI since if we get here then it must've already
		 * been started
		 */
		lnet_shutdown_lndni(ni);
		return -EINVAL;
# endif
	}
#endif
	if (ni->ni_peertxcredits == 0 || ni->ni_maxtxcredits == 0) {
		LCONSOLE_ERROR_MSG(0x107, "LNI %s has no %scredits\n",
				   libcfs_lnd2str(lnd->lnd_type),
				   ni->ni_peertxcredits == 0 ? 
					"" : "per-peer ");
		/* shutdown the NI since if we get here then it must've already
		 * been started
		 */
		lnet_shutdown_lndni(ni);
		return -EINVAL;
	}

	cfs_percpt_for_each(tq, i, ni->ni_tx_queues) {
		tq->tq_credits_min =
		tq->tq_credits_max =
		tq->tq_credits = lnet_ni_tq_credits(ni);
	}

	CDEBUG(D_LNI, "Added LNI %s [%d/%d/%d/%d]\n",
		libcfs_nid2str(ni->ni_nid), ni->ni_peertxcredits,
		lnet_ni_tq_credits(ni) * LNET_CPT_NUMBER,
		ni->ni_peerrtrcredits, ni->ni_peertimeout);

	return 0;
failed0:
	lnet_ni_free(ni);
	return -EINVAL;
}

static int
lnet_startup_lndnis(struct list_head *nilist)
{
	struct lnet_ni		*ni;
	int			rc;
	int			lnd_type;
	int			ni_count = 0;

	while (!list_empty(nilist)) {
		ni = list_entry(nilist->next, lnet_ni_t, ni_list);
		list_del(&ni->ni_list);
		rc = lnet_startup_lndni(ni, -1, -1, -1, -1);

		if (rc < 0)
			goto failed;

		ni_count++;
	}

	if (the_lnet.ln_eq_waitni != NULL && ni_count > 1) {
		lnd_type = the_lnet.ln_eq_waitni->ni_lnd->lnd_type;
		LCONSOLE_ERROR_MSG(0x109, "LND %s can only run single-network"
				   "\n",
				   libcfs_lnd2str(lnd_type));
		rc = -EINVAL;
		goto failed;
	}

	return ni_count;
failed:
	lnet_shutdown_lndnis();

	return rc;
}

/**
 * Initialize LNet library.
 *
 * Only userspace program needs to call this function - it's automatically
 * called in the kernel at module loading time. Caller has to call LNetFini()
 * after a call to LNetInit(), if and only if the latter returned 0. It must
 * be called exactly once.
 *
 * \return 0 on success, and -ve on failures.
 */
int
LNetInit(void)
{
	int	rc;

	lnet_assert_wire_constants();
	LASSERT(!the_lnet.ln_init);

	memset(&the_lnet, 0, sizeof(the_lnet));

	/* refer to global cfs_cpt_table for now */
	the_lnet.ln_cpt_table	= cfs_cpt_table;
	the_lnet.ln_cpt_number	= cfs_cpt_number(cfs_cpt_table);

	LASSERT(the_lnet.ln_cpt_number > 0);
	if (the_lnet.ln_cpt_number > LNET_CPT_MAX) {
		/* we are under risk of consuming all lh_cookie */
		CERROR("Can't have %d CPTs for LNet (max allowed is %d), "
		       "please change setting of CPT-table and retry\n",
		       the_lnet.ln_cpt_number, LNET_CPT_MAX);
		return -1;
	}

	while ((1 << the_lnet.ln_cpt_bits) < the_lnet.ln_cpt_number)
		the_lnet.ln_cpt_bits++;

	rc = lnet_create_locks();
	if (rc != 0) {
		CERROR("Can't create LNet global locks: %d\n", rc);
		return -1;
	}

	the_lnet.ln_refcount = 0;
	the_lnet.ln_init = 1;
	LNetInvalidateHandle(&the_lnet.ln_rc_eqh);
	INIT_LIST_HEAD(&the_lnet.ln_lnds);
	INIT_LIST_HEAD(&the_lnet.ln_rcd_zombie);
	INIT_LIST_HEAD(&the_lnet.ln_rcd_deathrow);

#ifdef __KERNEL__
	/* The hash table size is the number of bits it takes to express the set
	 * ln_num_routes, minus 1 (better to under estimate than over so we
	 * don't waste memory). */
	if (rnet_htable_size <= 0)
		rnet_htable_size = LNET_REMOTE_NETS_HASH_DEFAULT;
	else if (rnet_htable_size > LNET_REMOTE_NETS_HASH_MAX)
		rnet_htable_size = LNET_REMOTE_NETS_HASH_MAX;
	the_lnet.ln_remote_nets_hbits = max_t(int, 1,
					   order_base_2(rnet_htable_size) - 1);

	/* All LNDs apart from the LOLND are in separate modules.  They
	 * register themselves when their module loads, and unregister
	 * themselves when their module is unloaded. */
#else
	the_lnet.ln_remote_nets_hbits = 8;

	/* Register LNDs
	 * NB the order here determines default 'networks=' order */
# ifdef HAVE_LIBPTHREAD
	LNET_REGISTER_ULND(the_tcplnd);
# endif
#endif
	lnet_register_lnd(&the_lolnd);
	return 0;
}
EXPORT_SYMBOL(LNetInit);

/**
 * Finalize LNet library.
 *
 * Only userspace program needs to call this function. It can be called
 * at most once.
 *
 * \pre LNetInit() called with success.
 * \pre All LNet users called LNetNIFini() for matching LNetNIInit() calls.
 */
void
LNetFini(void)
{
	LASSERT(the_lnet.ln_init);
	LASSERT(the_lnet.ln_refcount == 0);

	while (!list_empty(&the_lnet.ln_lnds))
		lnet_unregister_lnd(list_entry(the_lnet.ln_lnds.next,
					       lnd_t, lnd_list));
	lnet_destroy_locks();

	the_lnet.ln_init = 0;
}
EXPORT_SYMBOL(LNetFini);

/**
 * Set LNet PID and start LNet interfaces, routing, and forwarding.
 *
 * Userspace program should call this after a successful call to LNetInit().
 * Users must call this function at least once before any other functions.
 * For each successful call there must be a corresponding call to
 * LNetNIFini(). For subsequent calls to LNetNIInit(), \a requested_pid is
 * ignored.
 *
 * The PID used by LNet may be different from the one requested.
 * See LNetGetId().
 *
 * \param requested_pid PID requested by the caller.
 *
 * \return >= 0 on success, and < 0 error code on failures.
 */
int
LNetNIInit(lnet_pid_t requested_pid)
{
	int			im_a_router = 0;
	int			rc, rc2;
	int			ni_count;
	lnet_ping_info_t	*pinfo;
	lnet_handle_md_t	md_handle;
	struct list_head	net_head;

	INIT_LIST_HEAD(&net_head);

	LNET_MUTEX_LOCK(&the_lnet.ln_api_mutex);

	LASSERT(the_lnet.ln_init);
	CDEBUG(D_OTHER, "refs %d\n", the_lnet.ln_refcount);

	if (the_lnet.ln_refcount > 0) {
		rc = the_lnet.ln_refcount++;
		LNET_MUTEX_UNLOCK(&the_lnet.ln_api_mutex);
		return rc;
	}

	rc = lnet_prepare(requested_pid);
	if (rc != 0) {
		LNET_MUTEX_UNLOCK(&the_lnet.ln_api_mutex);
		return rc;
	}

	/* Add in the loopback network */
	if (lnet_ni_alloc(LNET_MKNET(LOLND, 0), NULL, &net_head) == NULL) {
		rc = -ENOMEM;
		goto failed0;
	}

	/* If LNet is being initialized via DLC it is possible
	 * that the user requests not to load module parameters (ones which
	 * are supported by DLC) on initialization.  Therefore, make sure not
	 * to load networks, routes and forwarding from module parameters
	 * in this case.  On cleanup in case of failure only clean up
	 * routes if it has been loaded */
	if (!the_lnet.ln_nis_from_mod_params) {
		rc = lnet_parse_networks(&net_head,
					 lnet_get_networks());
		if (rc < 0)
			goto failed0;
	}

	ni_count = lnet_startup_lndnis(&net_head);
	if (ni_count < 0) {
		rc = ni_count;
		goto failed0;
	}

	if (!the_lnet.ln_nis_from_mod_params) {
		rc = lnet_parse_routes(lnet_get_routes(), &im_a_router);
		if (rc != 0)
			goto failed1;

		rc = lnet_check_routes();
		if (rc != 0)
			goto failed2;

		rc = lnet_rtrpools_alloc(im_a_router);
		if (rc != 0)
			goto failed2;
	}

	rc = lnet_acceptor_start();
	if (rc != 0)
		goto failed2;
	the_lnet.ln_refcount = 1;
	/* Now I may use my own API functions... */

	rc = lnet_ping_info_setup(&pinfo, &md_handle, ni_count, true);
	if (rc != 0)
		goto failed3;

	lnet_ping_target_update(pinfo, md_handle);

	rc = lnet_router_checker_start();
	if (rc != 0)
		goto failed4;

	lnet_fault_init();
	lnet_proc_init();

	LNET_MUTEX_UNLOCK(&the_lnet.ln_api_mutex);

	return 0;

failed4:
	lnet_ping_md_unlink(pinfo, &md_handle);
	lnet_ping_info_free(pinfo);
	rc2 = LNetEQFree(the_lnet.ln_ping_target_eq);
	LASSERT(rc2 == 0);
failed3:
	the_lnet.ln_refcount = 0;
	lnet_acceptor_stop();
failed2:
	if (!the_lnet.ln_nis_from_mod_params)
		lnet_destroy_routes();
failed1:
	lnet_shutdown_lndnis();
failed0:
	lnet_unprepare();
	LASSERT(rc < 0);
	LNET_MUTEX_UNLOCK(&the_lnet.ln_api_mutex);
	while (!list_empty(&net_head)) {
		struct lnet_ni *ni;
		ni = list_entry(net_head.next, struct lnet_ni, ni_list);
		list_del_init(&ni->ni_list);
		lnet_ni_free(ni);
	}
	return rc;
}
EXPORT_SYMBOL(LNetNIInit);

/**
 * Stop LNet interfaces, routing, and forwarding.
 *
 * Users must call this function once for each successful call to LNetNIInit().
 * Once the LNetNIFini() operation has been started, the results of pending
 * API operations are undefined.
 *
 * \return always 0 for current implementation.
 */
int
LNetNIFini()
{
        LNET_MUTEX_LOCK(&the_lnet.ln_api_mutex);

        LASSERT (the_lnet.ln_init);
        LASSERT (the_lnet.ln_refcount > 0);

        if (the_lnet.ln_refcount != 1) {
                the_lnet.ln_refcount--;
        } else {
		LASSERT(!the_lnet.ln_niinit_self);

		lnet_fault_fini();

                lnet_proc_fini();
                lnet_router_checker_stop();
                lnet_ping_target_fini();

                /* Teardown fns that use my own API functions BEFORE here */
                the_lnet.ln_refcount = 0;

                lnet_acceptor_stop();
                lnet_destroy_routes();
                lnet_shutdown_lndnis();
                lnet_unprepare();
        }

        LNET_MUTEX_UNLOCK(&the_lnet.ln_api_mutex);
        return 0;
}
EXPORT_SYMBOL(LNetNIFini);

/**
 * Grabs the ni data from the ni structure and fills the out
 * parameters
 *
 * \param[in] ni network	interface structure
 * \param[out] cpt_count	the number of cpts the ni is on
 * \param[out] nid		Network Interface ID
 * \param[out] peer_timeout	NI peer timeout
 * \param[out] peer_tx_crdits	NI peer transmit credits
 * \param[out] peer_rtr_credits NI peer router credits
 * \param[out] max_tx_credits	NI max transmit credit
 * \param[out] net_config	Network configuration
 */
static void
lnet_fill_ni_info(struct lnet_ni *ni, __u32 *cpt_count, __u64 *nid,
		  int *peer_timeout, int *peer_tx_credits,
		  int *peer_rtr_credits, int *max_tx_credits,
		  struct lnet_ioctl_net_config *net_config)
{
	int i;

	if (ni == NULL)
		return;

	if (net_config == NULL)
		return;

	CLASSERT(ARRAY_SIZE(ni->ni_interfaces) ==
		 ARRAY_SIZE(net_config->ni_interfaces));

	if (ni->ni_interfaces[0] != NULL) {
		for (i = 0; i < ARRAY_SIZE(ni->ni_interfaces); i++) {
			if (ni->ni_interfaces[i] != NULL) {
				strncpy(net_config->ni_interfaces[i],
					ni->ni_interfaces[i],
					sizeof(net_config->ni_interfaces[i]));
			}
		}
	}

	*nid = ni->ni_nid;
	*peer_timeout = ni->ni_peertimeout;
	*peer_tx_credits = ni->ni_peertxcredits;
	*peer_rtr_credits = ni->ni_peerrtrcredits;
	*max_tx_credits = ni->ni_maxtxcredits;

	net_config->ni_status = ni->ni_status->ns_status;

	for (i = 0;
	     ni->ni_cpts != NULL && i < ni->ni_ncpts &&
	     i < LNET_MAX_SHOW_NUM_CPT;
	     i++)
		net_config->ni_cpts[i] = ni->ni_cpts[i];

	*cpt_count = ni->ni_ncpts;
}

int
lnet_get_net_config(int idx, __u32 *cpt_count, __u64 *nid, int *peer_timeout,
		    int *peer_tx_credits, int *peer_rtr_credits,
		    int *max_tx_credits,
		    struct lnet_ioctl_net_config *net_config)
{
	struct lnet_ni		*ni;
	struct list_head	*tmp;
	int			cpt;
	int			rc = -ENOENT;

	cpt = lnet_net_lock_current();

	list_for_each(tmp, &the_lnet.ln_nis) {
		ni = list_entry(tmp, lnet_ni_t, ni_list);
		if (idx-- == 0) {
			rc = 0;
			lnet_ni_lock(ni);
			lnet_fill_ni_info(ni, cpt_count, nid, peer_timeout,
					  peer_tx_credits, peer_rtr_credits,
					  max_tx_credits, net_config);
			lnet_ni_unlock(ni);
			break;
		}
	}

	lnet_net_unlock(cpt);
	return rc;
}

int
lnet_dyn_add_ni(lnet_pid_t requested_pid, char *nets,
		__s32 peer_timeout, __s32 peer_cr, __s32 peer_buf_cr,
		__s32 credits)
{
	lnet_ping_info_t	*pinfo;
	lnet_handle_md_t	md_handle;
	struct lnet_ni		*ni;
	struct list_head	net_head;
	int			rc;

	INIT_LIST_HEAD(&net_head);

	/* Create a ni structure for the network string */
	rc = lnet_parse_networks(&net_head, nets);
	if (rc <= 0)
		return rc == 0 ? -EINVAL : rc;

	LNET_MUTEX_LOCK(&the_lnet.ln_api_mutex);

	if (rc > 1) {
		rc = -EINVAL; /* only add one interface per call */
		goto failed0;
	}

	rc = lnet_ping_info_setup(&pinfo, &md_handle, 1 + lnet_get_ni_count(),
				  false);
	if (rc != 0)
		goto failed0;

	ni = list_entry(net_head.next, struct lnet_ni, ni_list);
	list_del_init(&ni->ni_list);

	rc = lnet_startup_lndni(ni, peer_timeout, peer_cr,
				peer_buf_cr, credits);
	if (rc != 0)
		goto failed1;

	lnet_ping_target_update(pinfo, md_handle);
	LNET_MUTEX_UNLOCK(&the_lnet.ln_api_mutex);

	return 0;

failed1:
	lnet_ping_md_unlink(pinfo, &md_handle);
	lnet_ping_info_free(pinfo);
failed0:
	LNET_MUTEX_UNLOCK(&the_lnet.ln_api_mutex);
	while (!list_empty(&net_head)) {
		ni = list_entry(net_head.next, struct lnet_ni, ni_list);
		list_del_init(&ni->ni_list);
		lnet_ni_free(ni);
	}
	return rc;
}

int
lnet_dyn_del_ni(__u32 net)
{
	lnet_ni_t	 *ni;
	lnet_ping_info_t *pinfo;
	lnet_handle_md_t  md_handle;
	int		  rc;

	/* don't allow userspace to shutdown the LOLND */
	if (LNET_NETTYP(net) == LOLND)
		return -EINVAL;

	LNET_MUTEX_LOCK(&the_lnet.ln_api_mutex);
	/* create and link a new ping info, before removing the old one */
	rc = lnet_ping_info_setup(&pinfo, &md_handle,
				  lnet_get_ni_count() - 1, false);
	if (rc != 0)
		goto out;

	ni = lnet_net2ni(net);
	if (ni == NULL) {
		rc = -EINVAL;
		goto failed;
	}

	/* decrement the reference counter taken by lnet_net2ni() */
	lnet_ni_decref_locked(ni, 0);

	lnet_shutdown_lndni(ni);
	lnet_ping_target_update(pinfo, md_handle);
	goto out;
failed:
	lnet_ping_md_unlink(pinfo, &md_handle);
	lnet_ping_info_free(pinfo);
out:
	LNET_MUTEX_UNLOCK(&the_lnet.ln_api_mutex);

	return rc;
}

/**
 * This is an ugly hack to export IOC_LIBCFS_DEBUG_PEER and
 * IOC_LIBCFS_PORTALS_COMPATIBILITY commands to users, by tweaking the LNet
 * internal ioctl handler.
 *
 * IOC_LIBCFS_PORTALS_COMPATIBILITY is now deprecated, don't use it.
 *
 * \param cmd IOC_LIBCFS_DEBUG_PEER to print debugging data about a peer.
 * The data will be printed to system console. Don't use it excessively.
 * \param arg A pointer to lnet_process_id_t, process ID of the peer.
 *
 * \return Always return 0 when called by users directly (i.e., not via ioctl).
 */
int
LNetCtl(unsigned int cmd, void *arg)
{
	struct libcfs_ioctl_data *data = arg;
	struct lnet_ioctl_config_data *config;
	lnet_process_id_t         id = {0};
	lnet_ni_t                *ni;
	int                       rc;

	CLASSERT(LIBCFS_IOC_DATA_MAX >= sizeof(struct lnet_ioctl_net_config) +
					sizeof(struct lnet_ioctl_config_data));
	LASSERT(the_lnet.ln_init);

	switch (cmd) {
	case IOC_LIBCFS_GET_NI:
		rc = LNetGetId(data->ioc_count, &id);
		data->ioc_nid = id.nid;
		return rc;

	case IOC_LIBCFS_FAIL_NID:
		return lnet_fail_nid(data->ioc_nid, data->ioc_count);

	case IOC_LIBCFS_ADD_ROUTE:
		config = arg;

		if (config->cfg_hdr.ioc_len < sizeof(*config))
			return -EINVAL;

		LNET_MUTEX_LOCK(&the_lnet.ln_api_mutex);
		rc = lnet_add_route(config->cfg_net,
				    config->cfg_config_u.cfg_route.rtr_hop,
				    config->cfg_nid,
				    config->cfg_config_u.cfg_route.
					rtr_priority);
		LNET_MUTEX_UNLOCK(&the_lnet.ln_api_mutex);
		return (rc != 0) ? rc : lnet_check_routes();

	case IOC_LIBCFS_DEL_ROUTE:
		config = arg;

		if (config->cfg_hdr.ioc_len < sizeof(*config))
			return -EINVAL;

		LNET_MUTEX_LOCK(&the_lnet.ln_api_mutex);
		rc = lnet_del_route(config->cfg_net, config->cfg_nid);
		LNET_MUTEX_UNLOCK(&the_lnet.ln_api_mutex);
		return rc;

	case IOC_LIBCFS_GET_ROUTE:
		config = arg;

		if (config->cfg_hdr.ioc_len < sizeof(*config))
			return -EINVAL;

		return lnet_get_route(config->cfg_count,
				      &config->cfg_net,
				      &config->cfg_config_u.cfg_route.rtr_hop,
				      &config->cfg_nid,
				      &config->cfg_config_u.cfg_route.rtr_flags,
				      &config->cfg_config_u.cfg_route.
					rtr_priority);

	case IOC_LIBCFS_GET_NET: {
		struct lnet_ioctl_net_config *net_config;
		size_t total = sizeof(*config) + sizeof(*net_config);

		config = arg;

		if (config->cfg_hdr.ioc_len < total)
			return -EINVAL;

		net_config = (struct lnet_ioctl_net_config *)
			config->cfg_bulk;
		if (config == NULL || net_config == NULL)
			return -1;

		return lnet_get_net_config(config->cfg_count,
					   &config->cfg_ncpts,
					   &config->cfg_nid,
					   &config->cfg_config_u.
						cfg_net.net_peer_timeout,
					   &config->cfg_config_u.cfg_net.
						net_peer_tx_credits,
					   &config->cfg_config_u.cfg_net.
						net_peer_rtr_credits,
					   &config->cfg_config_u.cfg_net.
						net_max_tx_credits,
					   net_config);
	}

	case IOC_LIBCFS_GET_LNET_STATS:
	{
		struct lnet_ioctl_lnet_stats *lnet_stats = arg;

		if (lnet_stats->st_hdr.ioc_len < sizeof(*lnet_stats))
			return -EINVAL;

		lnet_counters_get(&lnet_stats->st_cntrs);
		return 0;
	}

#if defined(__KERNEL__) && defined(LNET_ROUTER)
	case IOC_LIBCFS_CONFIG_RTR:
		config = arg;

		if (config->cfg_hdr.ioc_len < sizeof(*config))
			return -EINVAL;

		LNET_MUTEX_LOCK(&the_lnet.ln_api_mutex);
		if (config->cfg_config_u.cfg_buffers.buf_enable) {
			rc = lnet_rtrpools_enable();
			LNET_MUTEX_UNLOCK(&the_lnet.ln_api_mutex);
			return rc;
		}
		lnet_rtrpools_disable();
		LNET_MUTEX_UNLOCK(&the_lnet.ln_api_mutex);
		return 0;

	case IOC_LIBCFS_ADD_BUF:
		config = arg;

		if (config->cfg_hdr.ioc_len < sizeof(*config))
			return -EINVAL;

		LNET_MUTEX_LOCK(&the_lnet.ln_api_mutex);
		rc = lnet_rtrpools_adjust(config->cfg_config_u.cfg_buffers.
						buf_tiny,
					  config->cfg_config_u.cfg_buffers.
						buf_small,
					  config->cfg_config_u.cfg_buffers.
						buf_large);
		LNET_MUTEX_UNLOCK(&the_lnet.ln_api_mutex);
		return rc;
#endif

	case IOC_LIBCFS_GET_BUF: {
		struct lnet_ioctl_pool_cfg *pool_cfg;
		size_t total = sizeof(*config) + sizeof(*pool_cfg);

		config = arg;

		if (config->cfg_hdr.ioc_len < total)
			return -EINVAL;

		pool_cfg = (struct lnet_ioctl_pool_cfg *)config->cfg_bulk;
		return lnet_get_rtr_pool_cfg(config->cfg_count, pool_cfg);
	}

	case IOC_LIBCFS_GET_PEER_INFO: {
		struct lnet_ioctl_peer *peer_info = arg;

		if (peer_info->pr_hdr.ioc_len < sizeof(*peer_info))
			return -EINVAL;

		return lnet_get_peer_info(
		   peer_info->pr_count,
		   &peer_info->pr_nid,
		   peer_info->pr_lnd_u.pr_peer_credits.cr_aliveness,
		   &peer_info->pr_lnd_u.pr_peer_credits.cr_ncpt,
		   &peer_info->pr_lnd_u.pr_peer_credits.cr_refcount,
		   &peer_info->pr_lnd_u.pr_peer_credits.cr_ni_peer_tx_credits,
		   &peer_info->pr_lnd_u.pr_peer_credits.cr_peer_tx_credits,
		   &peer_info->pr_lnd_u.pr_peer_credits.cr_peer_rtr_credits,
		   &peer_info->pr_lnd_u.pr_peer_credits.cr_peer_min_rtr_credits,
		   &peer_info->pr_lnd_u.pr_peer_credits.cr_peer_tx_qnob);
	}

	case IOC_LIBCFS_NOTIFY_ROUTER:
		return lnet_notify(NULL, data->ioc_nid, data->ioc_flags,
				   cfs_time_current() -
				   cfs_time_seconds(cfs_time_current_sec() -
						    (time_t)data->ioc_u64[0]));

	case IOC_LIBCFS_PORTALS_COMPATIBILITY:
		/* This can be removed once lustre stops calling it */
		return 0;

	case IOC_LIBCFS_LNET_DIST:
		rc = LNetDist(data->ioc_nid, &data->ioc_nid, &data->ioc_u32[1]);
		if (rc < 0 && rc != -EHOSTUNREACH)
			return rc;

		data->ioc_u32[0] = rc;
		return 0;

	case IOC_LIBCFS_TESTPROTOCOMPAT:
		lnet_net_lock(LNET_LOCK_EX);
		the_lnet.ln_testprotocompat = data->ioc_flags;
		lnet_net_unlock(LNET_LOCK_EX);
		return 0;

	case IOC_LIBCFS_LNET_FAULT:
		return lnet_fault_ctl(data->ioc_flags, data);

	case IOC_LIBCFS_PING:
		id.nid = data->ioc_nid;
		id.pid = data->ioc_u32[0];
		rc = lnet_ping(id, data->ioc_u32[1], /* timeout */
			       (lnet_process_id_t __user *)data->ioc_pbuf1,
			       data->ioc_plen1/sizeof(lnet_process_id_t));
		if (rc < 0)
			return rc;
		data->ioc_count = rc;
		return 0;

	case IOC_LIBCFS_DEBUG_PEER: {
		/* CAVEAT EMPTOR: this one designed for calling directly; not
		 * via an ioctl */
		id = *((lnet_process_id_t *) arg);

		lnet_debug_peer(id.nid);

		ni = lnet_net2ni(LNET_NIDNET(id.nid));
		if (ni == NULL) {
			CDEBUG(D_WARNING, "No NI for %s\n", libcfs_id2str(id));
		} else {
			if (ni->ni_lnd->lnd_ctl == NULL) {
				CDEBUG(D_WARNING, "No ctl for %s\n",
				       libcfs_id2str(id));
			} else {
				(void)ni->ni_lnd->lnd_ctl(ni, cmd, arg);
			}

			lnet_ni_decref(ni);
		}
		return 0;
	}

	default:
		ni = lnet_net2ni(data->ioc_net);
		if (ni == NULL)
			return -EINVAL;

		if (ni->ni_lnd->lnd_ctl == NULL)
			rc = -EINVAL;
		else
			rc = ni->ni_lnd->lnd_ctl(ni, cmd, arg);

		lnet_ni_decref(ni);
		return rc;
	}
	/* not reached */
}
EXPORT_SYMBOL(LNetCtl);

/**
 * Retrieve the lnet_process_id_t ID of LNet interface at \a index. Note that
 * all interfaces share a same PID, as requested by LNetNIInit().
 *
 * \param index Index of the interface to look up.
 * \param id On successful return, this location will hold the
 * lnet_process_id_t ID of the interface.
 *
 * \retval 0 If an interface exists at \a index.
 * \retval -ENOENT If no interface has been found.
 */
int
LNetGetId(unsigned int index, lnet_process_id_t *id)
{
	struct lnet_ni	 *ni;
	struct list_head *tmp;
	int		  cpt;
	int		  rc = -ENOENT;

	LASSERT(the_lnet.ln_init);
	LASSERT(the_lnet.ln_refcount > 0);

	cpt = lnet_net_lock_current();

	list_for_each(tmp, &the_lnet.ln_nis) {
		if (index-- != 0)
			continue;

		ni = list_entry(tmp, lnet_ni_t, ni_list);

		id->nid = ni->ni_nid;
		id->pid = the_lnet.ln_pid;
		rc = 0;
		break;
	}

	lnet_net_unlock(cpt);
	return rc;
}
EXPORT_SYMBOL(LNetGetId);

/**
 * Print a string representation of handle \a h into buffer \a str of
 * \a len bytes.
 */
void
LNetSnprintHandle(char *str, int len, lnet_handle_any_t h)
{
        snprintf(str, len, LPX64, h.cookie);
}
EXPORT_SYMBOL(LNetSnprintHandle);

static int
lnet_ping(lnet_process_id_t id, int timeout_ms, lnet_process_id_t __user *ids,
	  int n_ids)
{
	lnet_handle_eq_t     eqh;
	lnet_handle_md_t     mdh;
	lnet_event_t         event;
	lnet_md_t            md = { NULL };
	int                  which;
	int                  unlinked = 0;
	int                  replied = 0;
	const int            a_long_time = 60000; /* mS */
	int                  infosz;
	lnet_ping_info_t    *info;
	lnet_process_id_t    tmpid;
	int                  i;
	int                  nob;
	int                  rc;
	int                  rc2;
	sigset_t         blocked;

	infosz = offsetof(lnet_ping_info_t, pi_ni[n_ids]);

	if (n_ids <= 0 ||
	    id.nid == LNET_NID_ANY ||
	    timeout_ms > 500000 ||		/* arbitrary limit! */
	    n_ids > 20)				/* arbitrary limit! */
		return -EINVAL;

	if (id.pid == LNET_PID_ANY)
		id.pid = LNET_PID_LUSTRE;

	LIBCFS_ALLOC(info, infosz);
	if (info == NULL)
		return -ENOMEM;

	/* NB 2 events max (including any unlink event) */
	rc = LNetEQAlloc(2, LNET_EQ_HANDLER_NONE, &eqh);
	if (rc != 0) {
		CERROR("Can't allocate EQ: %d\n", rc);
		goto out_0;
	}

	/* initialize md content */
	md.start     = info;
	md.length    = infosz;
	md.threshold = 2; /*GET/REPLY*/
	md.max_size  = 0;
	md.options   = LNET_MD_TRUNCATE;
	md.user_ptr  = NULL;
	md.eq_handle = eqh;

	rc = LNetMDBind(md, LNET_UNLINK, &mdh);
	if (rc != 0) {
		CERROR("Can't bind MD: %d\n", rc);
		goto out_1;
	}

	rc = LNetGet(LNET_NID_ANY, mdh, id,
		     LNET_RESERVED_PORTAL,
		     LNET_PROTO_PING_MATCHBITS, 0);

	if (rc != 0) {
		/* Don't CERROR; this could be deliberate! */

		rc2 = LNetMDUnlink(mdh);
		LASSERT(rc2 == 0);

		/* NB must wait for the UNLINK event below... */
		unlinked = 1;
		timeout_ms = a_long_time;
	}

	do {
		/* MUST block for unlink to complete */
		if (unlinked)
			blocked = cfs_block_allsigs();

		rc2 = LNetEQPoll(&eqh, 1, timeout_ms, &event, &which);

		if (unlinked)
			cfs_restore_sigs(blocked);

		CDEBUG(D_NET, "poll %d(%d %d)%s\n", rc2,
		       (rc2 <= 0) ? -1 : event.type,
		       (rc2 <= 0) ? -1 : event.status,
		       (rc2 > 0 && event.unlinked) ? " unlinked" : "");

		LASSERT(rc2 != -EOVERFLOW);     /* can't miss anything */

		if (rc2 <= 0 || event.status != 0) {
			/* timeout or error */
			if (!replied && rc == 0)
				rc = (rc2 < 0) ? rc2 :
				     (rc2 == 0) ? -ETIMEDOUT :
				     event.status;

			if (!unlinked) {
				/* Ensure completion in finite time... */
				LNetMDUnlink(mdh);
				/* No assertion (racing with network) */
				unlinked = 1;
				timeout_ms = a_long_time;
			} else if (rc2 == 0) {
				/* timed out waiting for unlink */
				CWARN("ping %s: late network completion\n",
				      libcfs_id2str(id));
			}
		} else if (event.type == LNET_EVENT_REPLY) {
			replied = 1;
			rc = event.mlength;
		}

	} while (rc2 <= 0 || !event.unlinked);

	if (!replied) {
		if (rc >= 0)
			CWARN("%s: Unexpected rc >= 0 but no reply!\n",
			      libcfs_id2str(id));
		rc = -EIO;
		goto out_1;
	}

	nob = rc;
	LASSERT(nob >= 0 && nob <= infosz);

	rc = -EPROTO;                           /* if I can't parse... */

	if (nob < 8) {
		/* can't check magic/version */
		CERROR("%s: ping info too short %d\n",
		       libcfs_id2str(id), nob);
		goto out_1;
	}

	if (info->pi_magic == __swab32(LNET_PROTO_PING_MAGIC)) {
		lnet_swap_pinginfo(info);
	} else if (info->pi_magic != LNET_PROTO_PING_MAGIC) {
		CERROR("%s: Unexpected magic %08x\n",
		       libcfs_id2str(id), info->pi_magic);
		goto out_1;
	}

	if ((info->pi_features & LNET_PING_FEAT_NI_STATUS) == 0) {
		CERROR("%s: ping w/o NI status: 0x%x\n",
		       libcfs_id2str(id), info->pi_features);
		goto out_1;
	}

	if (nob < offsetof(lnet_ping_info_t, pi_ni[0])) {
		CERROR("%s: Short reply %d(%d min)\n", libcfs_id2str(id),
		       nob, (int)offsetof(lnet_ping_info_t, pi_ni[0]));
		goto out_1;
	}

	if (info->pi_nnis < n_ids)
		n_ids = info->pi_nnis;

	if (nob < offsetof(lnet_ping_info_t, pi_ni[n_ids])) {
		CERROR("%s: Short reply %d(%d expected)\n", libcfs_id2str(id),
		       nob, (int)offsetof(lnet_ping_info_t, pi_ni[n_ids]));
		goto out_1;
	}

	rc = -EFAULT;                           /* If I SEGV... */

	for (i = 0; i < n_ids; i++) {
		tmpid.pid = info->pi_pid;
		tmpid.nid = info->pi_ni[i].ns_nid;
		if (copy_to_user(&ids[i], &tmpid, sizeof(tmpid)))
			goto out_1;
	}
	rc = info->pi_nnis;

 out_1:
	rc2 = LNetEQFree(eqh);
	if (rc2 != 0)
		CERROR("rc2 %d\n", rc2);
	LASSERT(rc2 == 0);

 out_0:
	LIBCFS_FREE(info, infosz);
	return rc;
}
