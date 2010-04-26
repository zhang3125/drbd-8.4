/*
   drbd_receiver.c

   This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

   Copyright (C) 2001-2008, LINBIT Information Technologies GmbH.
   Copyright (C) 1999-2008, Philipp Reisner <philipp.reisner@linbit.com>.
   Copyright (C) 2002-2008, Lars Ellenberg <lars.ellenberg@linbit.com>.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */


#include <linux/autoconf.h>
#include <linux/module.h>

#include <asm/uaccess.h>
#include <net/sock.h>

#include <linux/drbd.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/in.h>
#include <linux/mm.h>
#include <linux/memcontrol.h>
#include <linux/mm_inline.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/pkt_sched.h>
#define __KERNEL_SYSCALLS__
#include <linux/unistd.h>
#include <linux/vmalloc.h>
#include <linux/random.h>
#include "drbd_int.h"
#include "drbd_tracing.h"
#include "drbd_req.h"
#include "drbd_vli.h"
#ifdef HAVE_LINUX_SCATTERLIST_H
/* 2.6.11 (suse 9.3, fc4) does not include requisites
 * from linux/scatterlist.h :( */
#include <asm/scatterlist.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/scatterlist.h>
#endif

struct flush_work {
	struct drbd_work w;
	struct drbd_epoch *epoch;
};

enum finish_epoch {
	FE_STILL_LIVE,
	FE_DESTROYED,
	FE_RECYCLED,
};

STATIC int drbd_do_handshake(struct drbd_conf *mdev);
STATIC int drbd_do_auth(struct drbd_conf *mdev);

STATIC enum finish_epoch drbd_may_finish_epoch(struct drbd_conf *, struct drbd_epoch *, enum epoch_event);
STATIC int e_end_block(struct drbd_conf *, struct drbd_work *, int);

static struct drbd_epoch *previous_epoch(struct drbd_conf *mdev, struct drbd_epoch *epoch)
{
	struct drbd_epoch *prev;
	spin_lock(&mdev->epoch_lock);
	prev = list_entry(epoch->list.prev, struct drbd_epoch, list);
	if (prev == epoch || prev == mdev->current_epoch)
		prev = NULL;
	spin_unlock(&mdev->epoch_lock);
	return prev;
}

#ifdef DBG_ASSERTS
void drbd_assert_breakpoint(struct drbd_conf *mdev, char *exp,
			    char *file, int line)
{
	dev_err(DEV, "ASSERT( %s ) in %s:%d\n", exp, file, line);
}
#endif

#define GFP_TRY	(__GFP_HIGHMEM | __GFP_NOWARN)

static struct page *drbd_pp_first_page_or_try_alloc(struct drbd_conf *mdev)
{
	struct page *page = NULL;

	/* Yes, testing drbd_pp_vacant outside the lock is racy.
	 * So what. It saves a spin_lock. */
	if (drbd_pp_vacant > 0) {
		spin_lock(&drbd_pp_lock);
		page = drbd_pp_pool;
		if (page) {
			drbd_pp_pool = (struct page *)page_private(page);
			set_page_private(page, 0); /* just to be polite */
			drbd_pp_vacant--;
		}
		spin_unlock(&drbd_pp_lock);
	}
	/* GFP_TRY, because we must not cause arbitrary write-out: in a DRBD
	 * "criss-cross" setup, that might cause write-out on some other DRBD,
	 * which in turn might block on the other node at this very place.  */
	if (!page)
		page = alloc_page(GFP_TRY);
	if (page)
		atomic_inc(&mdev->pp_in_use);
	return page;
}

/* kick lower level device, if we have more than (arbitrary number)
 * reference counts on it, which typically are locally submitted io
 * requests.  don't use unacked_cnt, so we speed up proto A and B, too. */
static void maybe_kick_lo(struct drbd_conf *mdev)
{
	if (atomic_read(&mdev->local_cnt) >= mdev->net_conf->unplug_watermark)
		drbd_kick_lo(mdev);
}

static void reclaim_net_ee(struct drbd_conf *mdev, struct list_head *to_be_freed)
{
	struct drbd_epoch_entry *e;
	struct list_head *le, *tle;

	/* The EEs are always appended to the end of the list. Since
	   they are sent in order over the wire, they have to finish
	   in order. As soon as we see the first not finished we can
	   stop to examine the list... */

	list_for_each_safe(le, tle, &mdev->net_ee) {
		e = list_entry(le, struct drbd_epoch_entry, w.list);
		if (drbd_bio_has_active_page(e->private_bio))
			break;
		list_move(le, to_be_freed);
	}
}

static void drbd_kick_lo_and_reclaim_net(struct drbd_conf *mdev)
{
	LIST_HEAD(reclaimed);
	struct drbd_epoch_entry *e, *t;

	maybe_kick_lo(mdev);
	spin_lock_irq(&mdev->req_lock);
	reclaim_net_ee(mdev, &reclaimed);
	spin_unlock_irq(&mdev->req_lock);

	list_for_each_entry_safe(e, t, &reclaimed, w.list)
		drbd_free_ee(mdev, e);
}

/**
 * drbd_pp_alloc() - Returns a page, fails only if a signal comes in
 * @mdev:	DRBD device.
 * @retry:	whether or not to retry allocation forever (or until signalled)
 *
 * Tries to allocate a page, first from our own page pool, then from the
 * kernel, unless this allocation would exceed the max_buffers setting.
 * If @retry is non-zero, retry until DRBD frees a page somewhere else.
 */
STATIC struct page *drbd_pp_alloc(struct drbd_conf *mdev, int retry)
{
	struct page *page = NULL;
	DEFINE_WAIT(wait);

	if (atomic_read(&mdev->pp_in_use) < mdev->net_conf->max_buffers) {
		page = drbd_pp_first_page_or_try_alloc(mdev);
		if (page)
			return page;
	}

	for (;;) {
		prepare_to_wait(&drbd_pp_wait, &wait, TASK_INTERRUPTIBLE);

		drbd_kick_lo_and_reclaim_net(mdev);

		if (atomic_read(&mdev->pp_in_use) < mdev->net_conf->max_buffers) {
			page = drbd_pp_first_page_or_try_alloc(mdev);
			if (page)
				break;
		}

		if (!retry)
			break;

		if (signal_pending(current)) {
			dev_warn(DEV, "drbd_pp_alloc interrupted!\n");
			break;
		}

		schedule();
	}
	finish_wait(&drbd_pp_wait, &wait);

	return page;
}

/* Must not be used from irq, as that may deadlock: see drbd_pp_alloc.
 * Is also used from inside an other spin_lock_irq(&mdev->req_lock) */
STATIC void drbd_pp_free(struct drbd_conf *mdev, struct page *page)
{
	int free_it;

	spin_lock(&drbd_pp_lock);
	if (drbd_pp_vacant > (DRBD_MAX_SEGMENT_SIZE/PAGE_SIZE)*minor_count) {
		free_it = 1;
	} else {
		set_page_private(page, (unsigned long)drbd_pp_pool);
		drbd_pp_pool = page;
		drbd_pp_vacant++;
		free_it = 0;
	}
	spin_unlock(&drbd_pp_lock);

	atomic_dec(&mdev->pp_in_use);

	if (free_it)
		__free_page(page);

	wake_up(&drbd_pp_wait);
}

STATIC void drbd_pp_free_bio_pages(struct drbd_conf *mdev, struct bio *bio)
{
	struct page *p_to_be_freed = NULL;
	struct page *page;
	struct bio_vec *bvec;
	int i;

	spin_lock(&drbd_pp_lock);
	__bio_for_each_segment(bvec, bio, i, 0) {
		if (drbd_pp_vacant > (DRBD_MAX_SEGMENT_SIZE/PAGE_SIZE)*minor_count) {
			set_page_private(bvec->bv_page, (unsigned long)p_to_be_freed);
			p_to_be_freed = bvec->bv_page;
		} else {
			set_page_private(bvec->bv_page, (unsigned long)drbd_pp_pool);
			drbd_pp_pool = bvec->bv_page;
			drbd_pp_vacant++;
		}
	}
	spin_unlock(&drbd_pp_lock);
	atomic_sub(bio->bi_vcnt, &mdev->pp_in_use);

	while (p_to_be_freed) {
		page = p_to_be_freed;
		p_to_be_freed = (struct page *)page_private(page);
		set_page_private(page, 0); /* just to be polite */
		put_page(page);
	}

	wake_up(&drbd_pp_wait);
}

/*
You need to hold the req_lock:
 _drbd_wait_ee_list_empty()

You must not have the req_lock:
 drbd_free_ee()
 drbd_alloc_ee()
 drbd_init_ee()
 drbd_release_ee()
 drbd_ee_fix_bhs()
 drbd_process_done_ee()
 drbd_clear_done_ee()
 drbd_wait_ee_list_empty()
*/

struct drbd_epoch_entry *drbd_alloc_ee(struct drbd_conf *mdev,
				     u64 id,
				     sector_t sector,
				     unsigned int data_size,
				     gfp_t gfp_mask) __must_hold(local)
{
	struct request_queue *q;
	struct drbd_epoch_entry *e;
	struct page *page;
	struct bio *bio;
	unsigned int ds;

	if (FAULT_ACTIVE(mdev, DRBD_FAULT_AL_EE))
		return NULL;

	e = mempool_alloc(drbd_ee_mempool, gfp_mask & ~__GFP_HIGHMEM);
	if (!e) {
		if (!(gfp_mask & __GFP_NOWARN))
			dev_err(DEV, "alloc_ee: Allocation of an EE failed\n");
		return NULL;
	}

	bio = bio_alloc(gfp_mask & ~__GFP_HIGHMEM, div_ceil(data_size, PAGE_SIZE));
	if (!bio) {
		if (!(gfp_mask & __GFP_NOWARN))
			dev_err(DEV, "alloc_ee: Allocation of a bio failed\n");
		goto fail1;
	}

	bio->bi_bdev = mdev->ldev->backing_bdev;
	bio->bi_sector = sector;

	ds = data_size;
	while (ds) {
		page = drbd_pp_alloc(mdev, (gfp_mask & __GFP_WAIT));
		if (!page) {
			if (!(gfp_mask & __GFP_NOWARN))
				dev_err(DEV, "alloc_ee: Allocation of a page failed\n");
			goto fail2;
		}
		if (!bio_add_page(bio, page, min_t(int, ds, PAGE_SIZE), 0)) {
			drbd_pp_free(mdev, page);
			dev_err(DEV, "alloc_ee: bio_add_page(s=%llu,"
			    "data_size=%u,ds=%u) failed\n",
			    (unsigned long long)sector, data_size, ds);

			q = bdev_get_queue(bio->bi_bdev);
			if (q->merge_bvec_fn) {
#ifdef HAVE_bvec_merge_data
				struct bvec_merge_data bvm = {
					.bi_bdev = bio->bi_bdev,
					.bi_sector = bio->bi_sector,
					.bi_size = bio->bi_size,
					.bi_rw = bio->bi_rw,
				};
				int l = q->merge_bvec_fn(q, &bvm,
						&bio->bi_io_vec[bio->bi_vcnt]);
#else
				int l = q->merge_bvec_fn(q, bio,
						&bio->bi_io_vec[bio->bi_vcnt]);
#endif
				dev_err(DEV, "merge_bvec_fn() = %d\n", l);
			}

			/* dump more of the bio. */
			DUMPI(bio->bi_max_vecs);
			DUMPI(bio->bi_vcnt);
			DUMPI(bio->bi_size);
			DUMPI(bio->bi_phys_segments);

			goto fail2;
			break;
		}
		ds -= min_t(int, ds, PAGE_SIZE);
	}

	D_ASSERT(data_size == bio->bi_size);

	bio->bi_private = e;
	e->mdev = mdev;
	e->sector = sector;
	e->size = bio->bi_size;

	e->private_bio = bio;
	e->block_id = id;
	INIT_HLIST_NODE(&e->colision);
	e->epoch = NULL;
	e->flags = 0;

	trace_drbd_ee(mdev, e, "allocated");

	return e;

 fail2:
	drbd_pp_free_bio_pages(mdev, bio);
	bio_put(bio);
 fail1:
	mempool_free(e, drbd_ee_mempool);

	return NULL;
}

void drbd_free_ee(struct drbd_conf *mdev, struct drbd_epoch_entry *e)
{
	struct bio *bio = e->private_bio;
	trace_drbd_ee(mdev, e, "freed");
	drbd_pp_free_bio_pages(mdev, bio);
	bio_put(bio);
	D_ASSERT(hlist_unhashed(&e->colision));
	mempool_free(e, drbd_ee_mempool);
}

int drbd_release_ee(struct drbd_conf *mdev, struct list_head *list)
{
	LIST_HEAD(work_list);
	struct drbd_epoch_entry *e, *t;
	int count = 0;

	spin_lock_irq(&mdev->req_lock);
	list_splice_init(list, &work_list);
	spin_unlock_irq(&mdev->req_lock);

	list_for_each_entry_safe(e, t, &work_list, w.list) {
		drbd_free_ee(mdev, e);
		count++;
	}
	return count;
}


/*
 * This function is called from _asender only_
 * but see also comments in _req_mod(,barrier_acked)
 * and receive_Barrier.
 *
 * Move entries from net_ee to done_ee, if ready.
 * Grab done_ee, call all callbacks, free the entries.
 * The callbacks typically send out ACKs.
 */
STATIC int drbd_process_done_ee(struct drbd_conf *mdev)
{
	LIST_HEAD(work_list);
	LIST_HEAD(reclaimed);
	struct drbd_epoch_entry *e, *t;
	int ok = (mdev->state.conn >= C_WF_REPORT_PARAMS);

	spin_lock_irq(&mdev->req_lock);
	reclaim_net_ee(mdev, &reclaimed);
	list_splice_init(&mdev->done_ee, &work_list);
	spin_unlock_irq(&mdev->req_lock);

	list_for_each_entry_safe(e, t, &reclaimed, w.list)
		drbd_free_ee(mdev, e);

	/* possible callbacks here:
	 * e_end_block, and e_end_resync_block, e_send_discard_ack.
	 * all ignore the last argument.
	 */
	list_for_each_entry_safe(e, t, &work_list, w.list) {
		trace_drbd_ee(mdev, e, "process_done_ee");
		/* list_del not necessary, next/prev members not touched */
		ok = e->w.cb(mdev, &e->w, !ok) && ok;
		drbd_free_ee(mdev, e);
	}
	wake_up(&mdev->ee_wait);

	return ok;
}

void _drbd_wait_ee_list_empty(struct drbd_conf *mdev, struct list_head *head)
{
	DEFINE_WAIT(wait);

	/* avoids spin_lock/unlock
	 * and calling prepare_to_wait in the fast path */
	while (!list_empty(head)) {
		prepare_to_wait(&mdev->ee_wait, &wait, TASK_UNINTERRUPTIBLE);
		spin_unlock_irq(&mdev->req_lock);
		drbd_kick_lo(mdev);
		schedule();
		finish_wait(&mdev->ee_wait, &wait);
		spin_lock_irq(&mdev->req_lock);
	}
}

void drbd_wait_ee_list_empty(struct drbd_conf *mdev, struct list_head *head)
{
	spin_lock_irq(&mdev->req_lock);
	_drbd_wait_ee_list_empty(mdev, head);
	spin_unlock_irq(&mdev->req_lock);
}

#ifdef DEFINE_SOCK_CREATE_KERN
/* if there is no sock_create_kern,
 * there is also sock_create_lite missing */
int sock_create_lite(int family, int type, int protocol, struct socket **res)
{
	int err = 0;
	struct socket *sock = NULL;

	sock = sock_alloc();
	if (!sock)
		err = -ENOMEM;
	else
		sock->type = type;

	*res = sock;
	return err;
}
#endif

/* see also kernel_accept; which is only present since 2.6.18.
 * also we want to log which part of it failed, exactly */
STATIC int drbd_accept(struct drbd_conf *mdev, const char **what,
		struct socket *sock, struct socket **newsock)
{
	struct sock *sk = sock->sk;
	int err = 0;

	*what = "listen";
	err = sock->ops->listen(sock, 5);
	if (err < 0)
		goto out;

	*what = "sock_create_lite";
	err = sock_create_lite(sk->sk_family, sk->sk_type, sk->sk_protocol,
			       newsock);
	if (err < 0)
		goto out;

	*what = "accept";
	err = sock->ops->accept(sock, *newsock, 0);
	if (err < 0) {
		sock_release(*newsock);
		*newsock = NULL;
		goto out;
	}
	(*newsock)->ops  = sock->ops;

out:
	return err;
}

STATIC int drbd_recv_short(struct drbd_conf *mdev, struct socket *sock,
		    void *buf, size_t size, int flags)
{
	mm_segment_t oldfs;
	struct kvec iov = {
		.iov_base = buf,
		.iov_len = size,
	};
	struct msghdr msg = {
		.msg_iovlen = 1,
		.msg_iov = (struct iovec *)&iov,
		.msg_flags = (flags ? flags : MSG_WAITALL | MSG_NOSIGNAL)
	};
	int rv;

	oldfs = get_fs();
	set_fs(KERNEL_DS);
	rv = sock_recvmsg(sock, &msg, size, msg.msg_flags);
	set_fs(oldfs);

	return rv;
}

STATIC int drbd_recv(struct drbd_conf *mdev, void *buf, size_t size)
{
	mm_segment_t oldfs;
	struct kvec iov = {
		.iov_base = buf,
		.iov_len = size,
	};
	struct msghdr msg = {
		.msg_iovlen = 1,
		.msg_iov = (struct iovec *)&iov,
		.msg_flags = MSG_WAITALL | MSG_NOSIGNAL
	};
	int rv;

	oldfs = get_fs();
	set_fs(KERNEL_DS);

	for (;;) {
		rv = sock_recvmsg(mdev->data.socket, &msg, size, msg.msg_flags);
		if (rv == size)
			break;

		/* Note:
		 * ECONNRESET	other side closed the connection
		 * ERESTARTSYS	(on  sock) we got a signal
		 */

		if (rv < 0) {
			if (rv == -ECONNRESET)
				dev_info(DEV, "sock was reset by peer\n");
			else if (rv != -ERESTARTSYS)
				dev_err(DEV, "sock_recvmsg returned %d\n", rv);
			break;
		} else if (rv == 0) {
			dev_info(DEV, "sock was shut down by peer\n");
			break;
		} else	{
			/* signal came in, or peer/link went down,
			 * after we read a partial message
			 */
			/* D_ASSERT(signal_pending(current)); */
			break;
		}
	};

	set_fs(oldfs);

	if (rv != size)
		drbd_force_state(mdev, NS(conn, C_BROKEN_PIPE));

	return rv;
}

STATIC struct socket *drbd_try_connect(struct drbd_conf *mdev)
{
	const char *what;
	struct socket *sock;
	struct sockaddr_in6 src_in6;
	int err;
	int disconnect_on_error = 1;

	if (!get_net_conf(mdev))
		return NULL;

	what = "sock_create_kern";
	err = sock_create_kern(((struct sockaddr *)mdev->net_conf->my_addr)->sa_family,
		SOCK_STREAM, IPPROTO_TCP, &sock);
	if (err < 0) {
		sock = NULL;
		goto out;
	}

	sock->sk->sk_rcvtimeo =
	sock->sk->sk_sndtimeo =  mdev->net_conf->try_connect_int*HZ;

       /* explicitly bind to the configured IP as source IP
	*  for the outgoing connections.
	*  This is needed for multihomed hosts and to be
	*  able to use lo: interfaces for drbd.
	* Make sure to use 0 as port number, so linux selects
	*  a free one dynamically.
	*/
	memcpy(&src_in6, mdev->net_conf->my_addr,
	       min_t(int, mdev->net_conf->my_addr_len, sizeof(src_in6)));
	if (((struct sockaddr *)mdev->net_conf->my_addr)->sa_family == AF_INET6)
		src_in6.sin6_port = 0;
	else
		((struct sockaddr_in *)&src_in6)->sin_port = 0; /* AF_INET & AF_SCI */

	what = "bind before connect";
	err = sock->ops->bind(sock,
			      (struct sockaddr *) &src_in6,
			      mdev->net_conf->my_addr_len);
	if (err < 0)
		goto out;

	/* connect may fail, peer not yet available.
	 * stay C_WF_CONNECTION, don't go Disconnecting! */
	disconnect_on_error = 0;
	what = "connect";
	err = sock->ops->connect(sock,
				 (struct sockaddr *)mdev->net_conf->peer_addr,
				 mdev->net_conf->peer_addr_len, 0);

out:
	if (err < 0) {
		if (sock) {
			sock_release(sock);
			sock = NULL;
		}
		switch (-err) {
			/* timeout, busy, signal pending */
		case ETIMEDOUT: case EAGAIN: case EINPROGRESS:
		case EINTR: case ERESTARTSYS:
			/* peer not (yet) available, network problem */
		case ECONNREFUSED: case ENETUNREACH:
		case EHOSTDOWN:    case EHOSTUNREACH:
			disconnect_on_error = 0;
			break;
		default:
			dev_err(DEV, "%s failed, err = %d\n", what, err);
		}
		if (disconnect_on_error)
			drbd_force_state(mdev, NS(conn, C_DISCONNECTING));
	}
	put_net_conf(mdev);
	return sock;
}

STATIC struct socket *drbd_wait_for_connect(struct drbd_conf *mdev)
{
	int timeo, err;
	struct socket *s_estab = NULL, *s_listen;
	const char *what;

	if (!get_net_conf(mdev))
		return NULL;

	what = "sock_create_kern";
	err = sock_create_kern(((struct sockaddr *)mdev->net_conf->my_addr)->sa_family,
		SOCK_STREAM, IPPROTO_TCP, &s_listen);
	if (err) {
		s_listen = NULL;
		goto out;
	}

	timeo = mdev->net_conf->try_connect_int * HZ;
	timeo += (random32() & 1) ? timeo / 7 : -timeo / 7; /* 28.5% random jitter */

	s_listen->sk->sk_reuse    = 1; /* SO_REUSEADDR */
	s_listen->sk->sk_rcvtimeo = timeo;
	s_listen->sk->sk_sndtimeo = timeo;

	what = "bind before listen";
	err = s_listen->ops->bind(s_listen,
			      (struct sockaddr *) mdev->net_conf->my_addr,
			      mdev->net_conf->my_addr_len);
	if (err < 0)
		goto out;

	err = drbd_accept(mdev, &what, s_listen, &s_estab);

out:
	if (s_listen)
		sock_release(s_listen);
	if (err < 0) {
		if (err != -EAGAIN && err != -EINTR && err != -ERESTARTSYS) {
			dev_err(DEV, "%s failed, err = %d\n", what, err);
			drbd_force_state(mdev, NS(conn, C_DISCONNECTING));
		}
	}
	put_net_conf(mdev);

	return s_estab;
}

STATIC int drbd_send_fp(struct drbd_conf *mdev,
	struct socket *sock, enum drbd_packets cmd)
{
	struct p_header *h = (struct p_header *) &mdev->data.sbuf.header;

	return _drbd_send_cmd(mdev, sock, cmd, h, sizeof(*h), 0);
}

STATIC enum drbd_packets drbd_recv_fp(struct drbd_conf *mdev, struct socket *sock)
{
	struct p_header *h = (struct p_header *) &mdev->data.sbuf.header;
	int rr;

	rr = drbd_recv_short(mdev, sock, h, sizeof(*h), 0);

	if (rr == sizeof(*h) && h->magic == BE_DRBD_MAGIC)
		return be16_to_cpu(h->command);

	return 0xffff;
}

/**
 * drbd_socket_okay() - Free the socket if its connection is not okay
 * @mdev:	DRBD device.
 * @sock:	pointer to the pointer to the socket.
 */
static int drbd_socket_okay(struct drbd_conf *mdev, struct socket **sock)
{
	int rr;
	char tb[4];

	if (!*sock)
		return FALSE;

	rr = drbd_recv_short(mdev, *sock, tb, 4, MSG_DONTWAIT | MSG_PEEK);

	if (rr > 0 || rr == -EAGAIN) {
		return TRUE;
	} else {
		sock_release(*sock);
		*sock = NULL;
		return FALSE;
	}
}

/*
 * return values:
 *   1 yes, we have a valid connection
 *   0 oops, did not work out, please try again
 *  -1 peer talks different language,
 *     no point in trying again, please go standalone.
 *  -2 We do not have a network config...
 */
STATIC int drbd_connect(struct drbd_conf *mdev)
{
	struct socket *s, *sock, *msock;
	int try, h, ok;

	D_ASSERT(!mdev->data.socket);

	if (test_and_clear_bit(CREATE_BARRIER, &mdev->flags))
		dev_err(DEV, "CREATE_BARRIER flag was set in drbd_connect - now cleared!\n");

	if (drbd_request_state(mdev, NS(conn, C_WF_CONNECTION)) < SS_SUCCESS)
		return -2;

	clear_bit(DISCARD_CONCURRENT, &mdev->flags);

	sock  = NULL;
	msock = NULL;

	do {
		for (try = 0;;) {
			/* 3 tries, this should take less than a second! */
			s = drbd_try_connect(mdev);
			if (s || ++try >= 3)
				break;
			/* give the other side time to call bind() & listen() */
			__set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ / 10);
		}

		if (s) {
			if (!sock) {
				drbd_send_fp(mdev, s, P_HAND_SHAKE_S);
				sock = s;
				s = NULL;
			} else if (!msock) {
				drbd_send_fp(mdev, s, P_HAND_SHAKE_M);
				msock = s;
				s = NULL;
			} else {
				dev_err(DEV, "Logic error in drbd_connect()\n");
				goto out_release_sockets;
			}
		}

		if (sock && msock) {
			__set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ / 10);
			ok = drbd_socket_okay(mdev, &sock);
			ok = drbd_socket_okay(mdev, &msock) && ok;
			if (ok)
				break;
		}

retry:
		s = drbd_wait_for_connect(mdev);
		if (s) {
			try = drbd_recv_fp(mdev, s);
			drbd_socket_okay(mdev, &sock);
			drbd_socket_okay(mdev, &msock);
			switch (try) {
			case P_HAND_SHAKE_S:
				if (sock) {
					dev_warn(DEV, "initial packet S crossed\n");
					sock_release(sock);
				}
				sock = s;
				break;
			case P_HAND_SHAKE_M:
				if (msock) {
					dev_warn(DEV, "initial packet M crossed\n");
					sock_release(msock);
				}
				msock = s;
				set_bit(DISCARD_CONCURRENT, &mdev->flags);
				break;
			default:
				dev_warn(DEV, "Error receiving initial packet\n");
				sock_release(s);
				if (random32() & 1)
					goto retry;
			}
		}

		if (mdev->state.conn <= C_DISCONNECTING)
			goto out_release_sockets;
		if (signal_pending(current)) {
			flush_signals(current);
			smp_rmb();
			if (get_t_state(&mdev->receiver) == Exiting)
				goto out_release_sockets;
		}

		if (sock && msock) {
			ok = drbd_socket_okay(mdev, &sock);
			ok = drbd_socket_okay(mdev, &msock) && ok;
			if (ok)
				break;
		}
	} while (1);

	msock->sk->sk_reuse = 1; /* SO_REUSEADDR */
	sock->sk->sk_reuse = 1; /* SO_REUSEADDR */

	sock->sk->sk_allocation = GFP_NOIO;
	msock->sk->sk_allocation = GFP_NOIO;

	sock->sk->sk_priority = TC_PRIO_INTERACTIVE_BULK;
	msock->sk->sk_priority = TC_PRIO_INTERACTIVE;

	if (mdev->net_conf->sndbuf_size) {
		sock->sk->sk_sndbuf = mdev->net_conf->sndbuf_size;
		sock->sk->sk_userlocks |= SOCK_SNDBUF_LOCK;
	}

	if (mdev->net_conf->rcvbuf_size) {
		sock->sk->sk_rcvbuf = mdev->net_conf->rcvbuf_size;
		sock->sk->sk_userlocks |= SOCK_RCVBUF_LOCK;
	}

	/* NOT YET ...
	 * sock->sk->sk_sndtimeo = mdev->net_conf->timeout*HZ/10;
	 * sock->sk->sk_rcvtimeo = MAX_SCHEDULE_TIMEOUT;
	 * first set it to the P_HAND_SHAKE timeout,
	 * which we set to 4x the configured ping_timeout. */
	sock->sk->sk_sndtimeo =
	sock->sk->sk_rcvtimeo = mdev->net_conf->ping_timeo*4*HZ/10;

	msock->sk->sk_sndtimeo = mdev->net_conf->timeout*HZ/10;
	msock->sk->sk_rcvtimeo = mdev->net_conf->ping_int*HZ;

	/* we don't want delays.
	 * we use TCP_CORK where apropriate, though */
	drbd_tcp_nodelay(sock);
	drbd_tcp_nodelay(msock);

	mdev->data.socket = sock;
	mdev->meta.socket = msock;
	mdev->last_received = jiffies;

	D_ASSERT(mdev->asender.task == NULL);

	h = drbd_do_handshake(mdev);
	if (h <= 0)
		return h;

	if (mdev->cram_hmac_tfm) {
		/* drbd_request_state(mdev, NS(conn, WFAuth)); */
		switch (drbd_do_auth(mdev)) {
		case -1:
			dev_err(DEV, "Authentication of peer failed\n");
			return -1;
		case 0:
			dev_err(DEV, "Authentication of peer failed, trying again.\n");
			return 0;
		}
	}

	if (drbd_request_state(mdev, NS(conn, C_WF_REPORT_PARAMS)) < SS_SUCCESS)
		return 0;

	sock->sk->sk_sndtimeo = mdev->net_conf->timeout*HZ/10;
	sock->sk->sk_rcvtimeo = MAX_SCHEDULE_TIMEOUT;

	atomic_set(&mdev->packet_seq, 0);
	mdev->peer_seq = 0;

	drbd_thread_start(&mdev->asender);

	if (!drbd_send_protocol(mdev))
		return -1;
	drbd_send_sync_param(mdev, &mdev->sync_conf);
	drbd_send_sizes(mdev, 0, 0);
	drbd_send_uuids(mdev);
	drbd_send_state(mdev);
	clear_bit(USE_DEGR_WFC_T, &mdev->flags);
	clear_bit(RESIZE_PENDING, &mdev->flags);

	return 1;

out_release_sockets:
	if (sock)
		sock_release(sock);
	if (msock)
		sock_release(msock);
	return -1;
}

STATIC int drbd_recv_header(struct drbd_conf *mdev, struct p_header *h)
{
	int r;

	r = drbd_recv(mdev, h, sizeof(*h));

	if (unlikely(r != sizeof(*h))) {
		dev_err(DEV, "short read expecting header on sock: r=%d\n", r);
		return FALSE;
	};
	h->command = be16_to_cpu(h->command);
	h->length  = be16_to_cpu(h->length);
	if (unlikely(h->magic != BE_DRBD_MAGIC)) {
		dev_err(DEV, "magic?? on data m: 0x%lx c: %d l: %d\n",
		    (long)be32_to_cpu(h->magic),
		    h->command, h->length);
		return FALSE;
	}
	mdev->last_received = jiffies;

	return TRUE;
}

STATIC enum finish_epoch drbd_flush_after_epoch(struct drbd_conf *mdev, struct drbd_epoch *epoch)
{
	int rv;

	if (mdev->write_ordering >= WO_bdev_flush && get_ldev(mdev)) {
		rv = blkdev_issue_flush(mdev->ldev->backing_bdev, NULL);
		if (rv) {
			dev_err(DEV, "local disk flush failed with status %d\n", rv);
			/* would rather check on EOPNOTSUPP, but that is not reliable.
			 * don't try again for ANY return value != 0
			 * if (rv == -EOPNOTSUPP) */
			drbd_bump_write_ordering(mdev, WO_drain_io);
		}
		put_ldev(mdev);
	}

	return drbd_may_finish_epoch(mdev, epoch, EV_BARRIER_DONE);
}

STATIC int w_flush(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	struct flush_work *fw = (struct flush_work *)w;
	struct drbd_epoch *epoch = fw->epoch;

	kfree(w);

	if (!test_and_set_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &epoch->flags))
		drbd_flush_after_epoch(mdev, epoch);

	drbd_may_finish_epoch(mdev, epoch, EV_PUT |
			      (mdev->state.conn < C_CONNECTED ? EV_CLEANUP : 0));

	return 1;
}

/**
 * drbd_may_finish_epoch() - Applies an epoch_event to the epoch's state, eventually finishes it.
 * @mdev:	DRBD device.
 * @epoch:	Epoch object.
 * @ev:		Epoch event.
 */
STATIC enum finish_epoch drbd_may_finish_epoch(struct drbd_conf *mdev,
					       struct drbd_epoch *epoch,
					       enum epoch_event ev)
{
	int finish, epoch_size;
	struct drbd_epoch *next_epoch;
	int schedule_flush = 0;
	enum finish_epoch rv = FE_STILL_LIVE;

	spin_lock(&mdev->epoch_lock);
	do {
		next_epoch = NULL;
		finish = 0;

		epoch_size = atomic_read(&epoch->epoch_size);

		switch (ev & ~EV_CLEANUP) {
		case EV_PUT:
			atomic_dec(&epoch->active);
			break;
		case EV_GOT_BARRIER_NR:
			set_bit(DE_HAVE_BARRIER_NUMBER, &epoch->flags);

			/* Special case: If we just switched from WO_bio_barrier to
			   WO_bdev_flush we should not finish the current epoch */
			if (test_bit(DE_CONTAINS_A_BARRIER, &epoch->flags) && epoch_size == 1 &&
			    mdev->write_ordering != WO_bio_barrier &&
			    epoch == mdev->current_epoch)
				clear_bit(DE_CONTAINS_A_BARRIER, &epoch->flags);
			break;
		case EV_BARRIER_DONE:
			set_bit(DE_BARRIER_IN_NEXT_EPOCH_DONE, &epoch->flags);
			break;
		case EV_BECAME_LAST:
			/* nothing to do*/
			break;
		}

		trace_drbd_epoch(mdev, epoch, ev);

		if (epoch_size != 0 &&
		    atomic_read(&epoch->active) == 0 &&
		    test_bit(DE_HAVE_BARRIER_NUMBER, &epoch->flags) &&
		    epoch->list.prev == &mdev->current_epoch->list &&
		    !test_bit(DE_IS_FINISHING, &epoch->flags)) {
			/* Nearly all conditions are met to finish that epoch... */
			if (test_bit(DE_BARRIER_IN_NEXT_EPOCH_DONE, &epoch->flags) ||
			    mdev->write_ordering == WO_none ||
			    (epoch_size == 1 && test_bit(DE_CONTAINS_A_BARRIER, &epoch->flags)) ||
			    ev & EV_CLEANUP) {
				finish = 1;
				set_bit(DE_IS_FINISHING, &epoch->flags);
			} else if (!test_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &epoch->flags) &&
				 mdev->write_ordering == WO_bio_barrier) {
				atomic_inc(&epoch->active);
				schedule_flush = 1;
			}
		}
		if (finish) {
			if (!(ev & EV_CLEANUP)) {
				spin_unlock(&mdev->epoch_lock);
				drbd_send_b_ack(mdev, epoch->barrier_nr, epoch_size);
				spin_lock(&mdev->epoch_lock);
			}
			dec_unacked(mdev);

			if (mdev->current_epoch != epoch) {
				next_epoch = list_entry(epoch->list.next, struct drbd_epoch, list);
				list_del(&epoch->list);
				ev = EV_BECAME_LAST | (ev & EV_CLEANUP);
				mdev->epochs--;
				trace_drbd_epoch(mdev, epoch, EV_TRACE_FREE);
				kfree(epoch);

				if (rv == FE_STILL_LIVE)
					rv = FE_DESTROYED;
			} else {
				epoch->flags = 0;
				atomic_set(&epoch->epoch_size, 0);
				/* atomic_set(&epoch->active, 0); is alrady zero */
				if (rv == FE_STILL_LIVE)
					rv = FE_RECYCLED;
			}
		}

		if (!next_epoch)
			break;

		epoch = next_epoch;
	} while (1);

	spin_unlock(&mdev->epoch_lock);

	if (schedule_flush) {
		struct flush_work *fw;
		fw = kmalloc(sizeof(*fw), GFP_ATOMIC);
		if (fw) {
			trace_drbd_epoch(mdev, epoch, EV_TRACE_FLUSH);
			fw->w.cb = w_flush;
			fw->epoch = epoch;
			drbd_queue_work(&mdev->data.work, &fw->w);
		} else {
			dev_warn(DEV, "Could not kmalloc a flush_work obj\n");
			set_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &epoch->flags);
			/* That is not a recursion, only one level */
			drbd_may_finish_epoch(mdev, epoch, EV_BARRIER_DONE);
			drbd_may_finish_epoch(mdev, epoch, EV_PUT);
		}
	}

	return rv;
}

/**
 * drbd_bump_write_ordering() - Fall back to an other write ordering method
 * @mdev:	DRBD device.
 * @wo:		Write ordering method to try.
 */
void drbd_bump_write_ordering(struct drbd_conf *mdev, enum write_ordering_e wo) __must_hold(local)
{
	enum write_ordering_e pwo;
	static char *write_ordering_str[] = {
		[WO_none] = "none",
		[WO_drain_io] = "drain",
		[WO_bdev_flush] = "flush",
		[WO_bio_barrier] = "barrier",
	};

	pwo = mdev->write_ordering;
	wo = min(pwo, wo);
	if (wo == WO_bio_barrier && mdev->ldev->dc.no_disk_barrier)
		wo = WO_bdev_flush;
	if (wo == WO_bdev_flush && mdev->ldev->dc.no_disk_flush)
		wo = WO_drain_io;
	if (wo == WO_drain_io && mdev->ldev->dc.no_disk_drain)
		wo = WO_none;
	mdev->write_ordering = wo;
	if (pwo != mdev->write_ordering || wo == WO_bio_barrier)
		dev_info(DEV, "Method to ensure write ordering: %s\n", write_ordering_str[mdev->write_ordering]);
}

/**
 * w_e_reissue() - Worker callback; Resubmit a bio, without BIO_RW_BARRIER set
 * @mdev:	DRBD device.
 * @w:		work object.
 * @cancel:	The connection will be closed anyways (unused in this callback)
 */
int w_e_reissue(struct drbd_conf *mdev, struct drbd_work *w, int cancel) __releases(local)
{
	struct drbd_epoch_entry *e = (struct drbd_epoch_entry *)w;
	struct bio *bio = e->private_bio;

	/* We leave DE_CONTAINS_A_BARRIER and EE_IS_BARRIER in place,
	   (and DE_BARRIER_IN_NEXT_EPOCH_ISSUED in the previous Epoch)
	   so that we can finish that epoch in drbd_may_finish_epoch().
	   That is necessary if we already have a long chain of Epochs, before
	   we realize that BIO_RW_BARRIER is actually not supported */

	/* As long as the -ENOTSUPP on the barrier is reported immediately
	   that will never trigger. If it is reported late, we will just
	   print that warning and continue correctly for all future requests
	   with WO_bdev_flush */
	if (previous_epoch(mdev, e->epoch))
		dev_warn(DEV, "Write ordering was not enforced (one time event)\n");

	/* prepare bio for re-submit,
	 * re-init volatile members */
	/* we still have a local reference,
	 * get_ldev was done in receive_Data. */
	bio->bi_bdev = mdev->ldev->backing_bdev;
	bio->bi_sector = e->sector;
	bio->bi_size = e->size;
	bio->bi_idx = 0;

	bio->bi_flags &= ~(BIO_POOL_MASK - 1);
	bio->bi_flags |= 1 << BIO_UPTODATE;

	/* don't know whether this is necessary: */
	bio->bi_phys_segments = 0;
	bio->bi_next = NULL;

	/* these should be unchanged: */
	/* bio->bi_end_io = drbd_endio_write_sec; */
	/* bio->bi_vcnt = whatever; */

	e->w.cb = e_end_block;

	/* This is no longer a barrier request. */
	bio->bi_rw &= ~(1UL << BIO_RW_BARRIER);

	drbd_generic_make_request(mdev, DRBD_FAULT_DT_WR, bio);

	return 1;
}

STATIC int receive_Barrier(struct drbd_conf *mdev, struct p_header *h)
{
	int rv, issue_flush;
	struct p_barrier *p = (struct p_barrier *)h;
	struct drbd_epoch *epoch;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;

	rv = drbd_recv(mdev, h->payload, h->length);
	ERR_IF(rv != h->length) return FALSE;

	inc_unacked(mdev);

	if (mdev->net_conf->wire_protocol != DRBD_PROT_C)
		drbd_kick_lo(mdev);

	mdev->current_epoch->barrier_nr = p->barrier;
	rv = drbd_may_finish_epoch(mdev, mdev->current_epoch, EV_GOT_BARRIER_NR);

	/* P_BARRIER_ACK may imply that the corresponding extent is dropped from
	 * the activity log, which means it would not be resynced in case the
	 * R_PRIMARY crashes now.
	 * Therefore we must send the barrier_ack after the barrier request was
	 * completed. */
	switch (mdev->write_ordering) {
	case WO_bio_barrier:
	case WO_none:
		if (rv == FE_RECYCLED)
			return TRUE;
		break;

	case WO_bdev_flush:
	case WO_drain_io:
		if (rv == FE_STILL_LIVE) {
			set_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &mdev->current_epoch->flags);
			drbd_wait_ee_list_empty(mdev, &mdev->active_ee);
			rv = drbd_flush_after_epoch(mdev, mdev->current_epoch);
		}
		if (rv == FE_RECYCLED)
			return TRUE;

		/* The asender will send all the ACKs and barrier ACKs out, since
		   all EEs moved from the active_ee to the done_ee. We need to
		   provide a new epoch object for the EEs that come in soon */
		break;
	}

	/* receiver context, in the writeout path of the other node.
	 * avoid potential distributed deadlock */
	epoch = kmalloc(sizeof(struct drbd_epoch), GFP_NOIO);
	if (!epoch) {
		dev_warn(DEV, "Allocation of an epoch failed, slowing down\n");
		issue_flush = !test_and_set_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &mdev->current_epoch->flags);
		drbd_wait_ee_list_empty(mdev, &mdev->active_ee);
		if (issue_flush) {
			rv = drbd_flush_after_epoch(mdev, mdev->current_epoch);
			if (rv == FE_RECYCLED)
				return TRUE;
		}

		drbd_wait_ee_list_empty(mdev, &mdev->done_ee);

		return TRUE;
	}

	epoch->flags = 0;
	atomic_set(&epoch->epoch_size, 0);
	atomic_set(&epoch->active, 0);

	spin_lock(&mdev->epoch_lock);
	if (atomic_read(&mdev->current_epoch->epoch_size)) {
		list_add(&epoch->list, &mdev->current_epoch->list);
		mdev->current_epoch = epoch;
		mdev->epochs++;
		trace_drbd_epoch(mdev, epoch, EV_TRACE_ALLOC);
	} else {
		/* The current_epoch got recycled while we allocated this one... */
		kfree(epoch);
	}
	spin_unlock(&mdev->epoch_lock);

	return TRUE;
}

/* used from receive_RSDataReply (recv_resync_read)
 * and from receive_Data */
STATIC struct drbd_epoch_entry *
read_in_block(struct drbd_conf *mdev, u64 id, sector_t sector, int data_size) __must_hold(local)
{
	const sector_t capacity = drbd_get_capacity(mdev->this_bdev);
	struct drbd_epoch_entry *e;
	struct bio_vec *bvec;
	struct page *page;
	struct bio *bio;
	int dgs, ds, i, rr;
	void *dig_in = mdev->int_dig_in;
	void *dig_vv = mdev->int_dig_vv;
	unsigned long *data;

	dgs = (mdev->agreed_pro_version >= 87 && mdev->integrity_r_tfm) ?
		crypto_hash_digestsize(mdev->integrity_r_tfm) : 0;

	if (dgs) {
		rr = drbd_recv(mdev, dig_in, dgs);
		if (rr != dgs) {
			dev_warn(DEV, "short read receiving data digest: read %d expected %d\n",
			     rr, dgs);
			return NULL;
		}
	}

	data_size -= dgs;

	ERR_IF(data_size &  0x1ff) return NULL;
	ERR_IF(data_size >  DRBD_MAX_SEGMENT_SIZE) return NULL;

	/* even though we trust out peer,
	 * we sometimes have to double check. */
	if (sector + (data_size>>9) > capacity) {
		dev_err(DEV, "capacity: %llus < sector: %llus + size: %u\n",
			(unsigned long long)capacity,
			(unsigned long long)sector, data_size);
		return NULL;
	}

	/* GFP_NOIO, because we must not cause arbitrary write-out: in a DRBD
	 * "criss-cross" setup, that might cause write-out on some other DRBD,
	 * which in turn might block on the other node at this very place.  */
	e = drbd_alloc_ee(mdev, id, sector, data_size, GFP_NOIO);
	if (!e)
		return NULL;
	bio = e->private_bio;
	ds = data_size;
	bio_for_each_segment(bvec, bio, i) {
		page = bvec->bv_page;
		data = kmap(page);
		rr = drbd_recv(mdev, data, min_t(int, ds, PAGE_SIZE));
		if (FAULT_ACTIVE(mdev, DRBD_FAULT_RECEIVE)) {
			dev_err(DEV, "Fault injection: Corrupting data on receive\n");
			data[0] = data[0] ^ (unsigned long)-1;
		}
		kunmap(page);
		if (rr != min_t(int, ds, PAGE_SIZE)) {
			drbd_free_ee(mdev, e);
			dev_warn(DEV, "short read receiving data: read %d expected %d\n",
			     rr, min_t(int, ds, PAGE_SIZE));
			return NULL;
		}
		ds -= rr;
	}

	if (dgs) {
		drbd_csum(mdev, mdev->integrity_r_tfm, bio, dig_vv);
		if (memcmp(dig_in, dig_vv, dgs)) {
			dev_err(DEV, "Digest integrity check FAILED.\n");
			drbd_bcast_ee(mdev, "digest failed",
					dgs, dig_in, dig_vv, e);
			drbd_free_ee(mdev, e);
			return NULL;
		}
	}
	mdev->recv_cnt += data_size>>9;
	return e;
}

/* drbd_drain_block() just takes a data block
 * out of the socket input buffer, and discards it.
 */
STATIC int drbd_drain_block(struct drbd_conf *mdev, int data_size)
{
	struct page *page;
	int rr, rv = 1;
	void *data;

	if (!data_size)
		return TRUE;

	page = drbd_pp_alloc(mdev, 1);

	data = kmap(page);
	while (data_size) {
		rr = drbd_recv(mdev, data, min_t(int, data_size, PAGE_SIZE));
		if (rr != min_t(int, data_size, PAGE_SIZE)) {
			rv = 0;
			dev_warn(DEV, "short read receiving data: read %d expected %d\n",
			     rr, min_t(int, data_size, PAGE_SIZE));
			break;
		}
		data_size -= rr;
	}
	kunmap(page);
	drbd_pp_free(mdev, page);
	return rv;
}

STATIC int recv_dless_read(struct drbd_conf *mdev, struct drbd_request *req,
			   sector_t sector, int data_size)
{
	struct bio_vec *bvec;
	struct bio *bio;
	int dgs, rr, i, expect;
	void *dig_in = mdev->int_dig_in;
	void *dig_vv = mdev->int_dig_vv;

	dgs = (mdev->agreed_pro_version >= 87 && mdev->integrity_r_tfm) ?
		crypto_hash_digestsize(mdev->integrity_r_tfm) : 0;

	if (dgs) {
		rr = drbd_recv(mdev, dig_in, dgs);
		if (rr != dgs) {
			dev_warn(DEV, "short read receiving data reply digest: read %d expected %d\n",
			     rr, dgs);
			return 0;
		}
	}

	data_size -= dgs;

	/* optimistically update recv_cnt.  if receiving fails below,
	 * we disconnect anyways, and counters will be reset. */
	mdev->recv_cnt += data_size>>9;

	bio = req->master_bio;
	D_ASSERT(sector == bio->bi_sector);

	bio_for_each_segment(bvec, bio, i) {
		expect = min_t(int, data_size, bvec->bv_len);
		rr = drbd_recv(mdev,
			     kmap(bvec->bv_page)+bvec->bv_offset,
			     expect);
		kunmap(bvec->bv_page);
		if (rr != expect) {
			dev_warn(DEV, "short read receiving data reply: "
			     "read %d expected %d\n",
			     rr, expect);
			return 0;
		}
		data_size -= rr;
	}

	if (dgs) {
		drbd_csum(mdev, mdev->integrity_r_tfm, bio, dig_vv);
		if (memcmp(dig_in, dig_vv, dgs)) {
			dev_err(DEV, "Digest integrity check FAILED. Broken NICs?\n");
			return 0;
		}
	}

	D_ASSERT(data_size == 0);
	return 1;
}

/* e_end_resync_block() is called via
 * drbd_process_done_ee() by asender only */
STATIC int e_end_resync_block(struct drbd_conf *mdev, struct drbd_work *w, int unused)
{
	struct drbd_epoch_entry *e = (struct drbd_epoch_entry *)w;
	sector_t sector = e->sector;
	int ok;

	D_ASSERT(hlist_unhashed(&e->colision));

	if (likely(drbd_bio_uptodate(e->private_bio))) {
		drbd_set_in_sync(mdev, sector, e->size);
		ok = drbd_send_ack(mdev, P_RS_WRITE_ACK, e);
	} else {
		/* Record failure to sync */
		drbd_rs_failed_io(mdev, sector, e->size);

		ok  = drbd_send_ack(mdev, P_NEG_ACK, e);
	}
	dec_unacked(mdev);

	return ok;
}

STATIC int recv_resync_read(struct drbd_conf *mdev, sector_t sector, int data_size) __releases(local)
{
	struct drbd_epoch_entry *e;

	e = read_in_block(mdev, ID_SYNCER, sector, data_size);
	if (!e) {
		put_ldev(mdev);
		return FALSE;
	}

	dec_rs_pending(mdev);

	e->private_bio->bi_end_io = drbd_endio_write_sec;
	e->private_bio->bi_rw = WRITE;
	e->w.cb = e_end_resync_block;

	inc_unacked(mdev);
	/* corresponding dec_unacked() in e_end_resync_block()
	 * respective _drbd_clear_done_ee */

	spin_lock_irq(&mdev->req_lock);
	list_add(&e->w.list, &mdev->sync_ee);
	spin_unlock_irq(&mdev->req_lock);

	trace_drbd_ee(mdev, e, "submitting for (rs)write");
	trace_drbd_bio(mdev, "Sec", e->private_bio, 0, NULL);
	drbd_generic_make_request(mdev, DRBD_FAULT_RS_WR, e->private_bio);
	/* accounting done in endio */

	maybe_kick_lo(mdev);
	return TRUE;
}

STATIC int receive_DataReply(struct drbd_conf *mdev, struct p_header *h)
{
	struct drbd_request *req;
	sector_t sector;
	unsigned int header_size, data_size;
	int ok;
	struct p_data *p = (struct p_data *)h;

	header_size = sizeof(*p) - sizeof(*h);
	data_size   = h->length  - header_size;

	ERR_IF(data_size == 0) return FALSE;

	if (drbd_recv(mdev, h->payload, header_size) != header_size)
		return FALSE;

	sector = be64_to_cpu(p->sector);

	spin_lock_irq(&mdev->req_lock);
	req = _ar_id_to_req(mdev, p->block_id, sector);
	spin_unlock_irq(&mdev->req_lock);
	if (unlikely(!req)) {
		dev_err(DEV, "Got a corrupt block_id/sector pair(1).\n");
		return FALSE;
	}

	/* hlist_del(&req->colision) is done in _req_may_be_done, to avoid
	 * special casing it there for the various failure cases.
	 * still no race with drbd_fail_pending_reads */
	ok = recv_dless_read(mdev, req, sector, data_size);

	if (ok)
		req_mod(req, data_received);
	/* else: nothing. handled from drbd_disconnect...
	 * I don't think we may complete this just yet
	 * in case we are "on-disconnect: freeze" */

	return ok;
}

STATIC int receive_RSDataReply(struct drbd_conf *mdev, struct p_header *h)
{
	sector_t sector;
	unsigned int header_size, data_size;
	int ok;
	struct p_data *p = (struct p_data *)h;

	header_size = sizeof(*p) - sizeof(*h);
	data_size   = h->length  - header_size;

	ERR_IF(data_size == 0) return FALSE;

	if (drbd_recv(mdev, h->payload, header_size) != header_size)
		return FALSE;

	sector = be64_to_cpu(p->sector);
	D_ASSERT(p->block_id == ID_SYNCER);

	if (get_ldev(mdev)) {
		/* data is submitted to disk within recv_resync_read.
		 * corresponding put_ldev done below on error,
		 * or in drbd_endio_write_sec. */
		ok = recv_resync_read(mdev, sector, data_size);
	} else {
		if (DRBD_ratelimit(5*HZ, 5))
			dev_err(DEV, "Can not write resync data to local disk.\n");

		ok = drbd_drain_block(mdev, data_size);

		drbd_send_ack_dp(mdev, P_NEG_ACK, p);
	}

	return ok;
}

/* e_end_block() is called via drbd_process_done_ee().
 * this means this function only runs in the asender thread
 */
STATIC int e_end_block(struct drbd_conf *mdev, struct drbd_work *w, int cancel)
{
	struct drbd_epoch_entry *e = (struct drbd_epoch_entry *)w;
	sector_t sector = e->sector;
	struct drbd_epoch *epoch;
	int ok = 1, pcmd;

	if (e->flags & EE_IS_BARRIER) {
		epoch = previous_epoch(mdev, e->epoch);
		if (epoch)
			drbd_may_finish_epoch(mdev, epoch, EV_BARRIER_DONE + (cancel ? EV_CLEANUP : 0));
	}

	if (mdev->net_conf->wire_protocol == DRBD_PROT_C) {
		if (likely(drbd_bio_uptodate(e->private_bio))) {
			pcmd = (mdev->state.conn >= C_SYNC_SOURCE &&
				mdev->state.conn <= C_PAUSED_SYNC_T &&
				e->flags & EE_MAY_SET_IN_SYNC) ?
				P_RS_WRITE_ACK : P_WRITE_ACK;
			ok &= drbd_send_ack(mdev, pcmd, e);
			if (pcmd == P_RS_WRITE_ACK)
				drbd_set_in_sync(mdev, sector, e->size);
		} else {
			ok  = drbd_send_ack(mdev, P_NEG_ACK, e);
			/* we expect it to be marked out of sync anyways...
			 * maybe assert this?  */
		}
		dec_unacked(mdev);
	}
	/* we delete from the conflict detection hash _after_ we sent out the
	 * P_WRITE_ACK / P_NEG_ACK, to get the sequence number right.  */
	if (mdev->net_conf->two_primaries) {
		spin_lock_irq(&mdev->req_lock);
		D_ASSERT(!hlist_unhashed(&e->colision));
		hlist_del_init(&e->colision);
		spin_unlock_irq(&mdev->req_lock);
	} else {
		D_ASSERT(hlist_unhashed(&e->colision));
	}

	drbd_may_finish_epoch(mdev, e->epoch, EV_PUT + (cancel ? EV_CLEANUP : 0));

	return ok;
}

STATIC int e_send_discard_ack(struct drbd_conf *mdev, struct drbd_work *w, int unused)
{
	struct drbd_epoch_entry *e = (struct drbd_epoch_entry *)w;
	int ok = 1;

	D_ASSERT(mdev->net_conf->wire_protocol == DRBD_PROT_C);
	ok = drbd_send_ack(mdev, P_DISCARD_ACK, e);

	spin_lock_irq(&mdev->req_lock);
	D_ASSERT(!hlist_unhashed(&e->colision));
	hlist_del_init(&e->colision);
	spin_unlock_irq(&mdev->req_lock);

	dec_unacked(mdev);

	return ok;
}

/* Called from receive_Data.
 * Synchronize packets on sock with packets on msock.
 *
 * This is here so even when a P_DATA packet traveling via sock overtook an Ack
 * packet traveling on msock, they are still processed in the order they have
 * been sent.
 *
 * Note: we don't care for Ack packets overtaking P_DATA packets.
 *
 * In case packet_seq is larger than mdev->peer_seq number, there are
 * outstanding packets on the msock. We wait for them to arrive.
 * In case we are the logically next packet, we update mdev->peer_seq
 * ourselves. Correctly handles 32bit wrap around.
 *
 * Assume we have a 10 GBit connection, that is about 1<<30 byte per second,
 * about 1<<21 sectors per second. So "worst" case, we have 1<<3 == 8 seconds
 * for the 24bit wrap (historical atomic_t guarantee on some archs), and we have
 * 1<<9 == 512 seconds aka ages for the 32bit wrap around...
 *
 * returns 0 if we may process the packet,
 * -ERESTARTSYS if we were interrupted (by disconnect signal). */
static int drbd_wait_peer_seq(struct drbd_conf *mdev, const u32 packet_seq)
{
	DEFINE_WAIT(wait);
	unsigned int p_seq;
	long timeout;
	int ret = 0;
	spin_lock(&mdev->peer_seq_lock);
	for (;;) {
		prepare_to_wait(&mdev->seq_wait, &wait, TASK_INTERRUPTIBLE);
		if (seq_le(packet_seq, mdev->peer_seq+1))
			break;
		if (signal_pending(current)) {
			ret = -ERESTARTSYS;
			break;
		}
		p_seq = mdev->peer_seq;
		spin_unlock(&mdev->peer_seq_lock);
		timeout = schedule_timeout(30*HZ);
		spin_lock(&mdev->peer_seq_lock);
		if (timeout == 0 && p_seq == mdev->peer_seq) {
			ret = -ETIMEDOUT;
			dev_err(DEV, "ASSERT FAILED waited 30 seconds for sequence update, forcing reconnect\n");
			break;
		}
	}
	finish_wait(&mdev->seq_wait, &wait);
	if (mdev->peer_seq+1 == packet_seq)
		mdev->peer_seq++;
	spin_unlock(&mdev->peer_seq_lock);
	return ret;
}

/* mirrored write */
STATIC int receive_Data(struct drbd_conf *mdev, struct p_header *h)
{
	sector_t sector;
	struct drbd_epoch_entry *e;
	struct p_data *p = (struct p_data *)h;
	int header_size, data_size;
	int rw = WRITE;
	u32 dp_flags;

	header_size = sizeof(*p) - sizeof(*h);
	data_size   = h->length  - header_size;

	ERR_IF(data_size == 0) return FALSE;

	if (drbd_recv(mdev, h->payload, header_size) != header_size)
		return FALSE;

	if (!get_ldev(mdev)) {
		if (DRBD_ratelimit(5*HZ, 5))
			dev_err(DEV, "Can not write mirrored data block "
			    "to local disk.\n");
		spin_lock(&mdev->peer_seq_lock);
		if (mdev->peer_seq+1 == be32_to_cpu(p->seq_num))
			mdev->peer_seq++;
		spin_unlock(&mdev->peer_seq_lock);

		drbd_send_ack_dp(mdev, P_NEG_ACK, p);
		atomic_inc(&mdev->current_epoch->epoch_size);
		return drbd_drain_block(mdev, data_size);
	}

	/* get_ldev(mdev) successful.
	 * Corresponding put_ldev done either below (on various errors),
	 * or in drbd_endio_write_sec, if we successfully submit the data at
	 * the end of this function. */

	sector = be64_to_cpu(p->sector);
	e = read_in_block(mdev, p->block_id, sector, data_size);
	if (!e) {
		put_ldev(mdev);
		return FALSE;
	}

	e->private_bio->bi_end_io = drbd_endio_write_sec;
	e->w.cb = e_end_block;

	spin_lock(&mdev->epoch_lock);
	e->epoch = mdev->current_epoch;
	atomic_inc(&e->epoch->epoch_size);
	atomic_inc(&e->epoch->active);

	if (mdev->write_ordering == WO_bio_barrier && atomic_read(&e->epoch->epoch_size) == 1) {
		struct drbd_epoch *epoch;
		/* Issue a barrier if we start a new epoch, and the previous epoch
		   was not a epoch containing a single request which already was
		   a Barrier. */
		epoch = list_entry(e->epoch->list.prev, struct drbd_epoch, list);
		if (epoch == e->epoch) {
			set_bit(DE_CONTAINS_A_BARRIER, &e->epoch->flags);
			trace_drbd_epoch(mdev, e->epoch, EV_TRACE_ADD_BARRIER);
			rw |= (1<<BIO_RW_BARRIER);
			e->flags |= EE_IS_BARRIER;
		} else {
			if (atomic_read(&epoch->epoch_size) > 1 ||
			    !test_bit(DE_CONTAINS_A_BARRIER, &epoch->flags)) {
				set_bit(DE_BARRIER_IN_NEXT_EPOCH_ISSUED, &epoch->flags);
				trace_drbd_epoch(mdev, epoch, EV_TRACE_SETTING_BI);
				set_bit(DE_CONTAINS_A_BARRIER, &e->epoch->flags);
				trace_drbd_epoch(mdev, e->epoch, EV_TRACE_ADD_BARRIER);
				rw |= (1<<BIO_RW_BARRIER);
				e->flags |= EE_IS_BARRIER;
			}
		}
	}
	spin_unlock(&mdev->epoch_lock);

	dp_flags = be32_to_cpu(p->dp_flags);
	if (dp_flags & DP_HARDBARRIER) {
		dev_err(DEV, "ASSERT FAILED would have submitted barrier request\n");
		/* rw |= (1<<BIO_RW_BARRIER); */
	}
	if (dp_flags & DP_RW_SYNC)
#ifdef BIO_RW_SYNC
		rw |= (1<<BIO_RW_SYNC);
#else
		/* see upstream commits
		 * 213d9417fec62ef4c3675621b9364a667954d4dd,
		 * 93dbb393503d53cd226e5e1f0088fe8f4dbaa2b8
		 * later, the defines even became an enum ;-) */
		rw |= (1<<BIO_RW_SYNCIO) | (1<<BIO_RW_UNPLUG);
#endif
	if (dp_flags & DP_MAY_SET_IN_SYNC)
		e->flags |= EE_MAY_SET_IN_SYNC;

	/* I'm the receiver, I do hold a net_cnt reference. */
	if (!mdev->net_conf->two_primaries) {
		spin_lock_irq(&mdev->req_lock);
	} else {
		/* don't get the req_lock yet,
		 * we may sleep in drbd_wait_peer_seq */
		const int size = e->size;
		const int discard = test_bit(DISCARD_CONCURRENT, &mdev->flags);
		DEFINE_WAIT(wait);
		struct drbd_request *i;
		struct hlist_node *n;
		struct hlist_head *slot;
		int first;

		D_ASSERT(mdev->net_conf->wire_protocol == DRBD_PROT_C);
		BUG_ON(mdev->ee_hash == NULL);
		BUG_ON(mdev->tl_hash == NULL);

		/* conflict detection and handling:
		 * 1. wait on the sequence number,
		 *    in case this data packet overtook ACK packets.
		 * 2. check our hash tables for conflicting requests.
		 *    we only need to walk the tl_hash, since an ee can not
		 *    have a conflict with an other ee: on the submitting
		 *    node, the corresponding req had already been conflicting,
		 *    and a conflicting req is never sent.
		 *
		 * Note: for two_primaries, we are protocol C,
		 * so there cannot be any request that is DONE
		 * but still on the transfer log.
		 *
		 * unconditionally add to the ee_hash.
		 *
		 * if no conflicting request is found:
		 *    submit.
		 *
		 * if any conflicting request is found
		 * that has not yet been acked,
		 * AND I have the "discard concurrent writes" flag:
		 *	 queue (via done_ee) the P_DISCARD_ACK; OUT.
		 *
		 * if any conflicting request is found:
		 *	 block the receiver, waiting on misc_wait
		 *	 until no more conflicting requests are there,
		 *	 or we get interrupted (disconnect).
		 *
		 *	 we do not just write after local io completion of those
		 *	 requests, but only after req is done completely, i.e.
		 *	 we wait for the P_DISCARD_ACK to arrive!
		 *
		 *	 then proceed normally, i.e. submit.
		 */
		if (drbd_wait_peer_seq(mdev, be32_to_cpu(p->seq_num)))
			goto out_interrupted;

		spin_lock_irq(&mdev->req_lock);

		hlist_add_head(&e->colision, ee_hash_slot(mdev, sector));

#define OVERLAPS overlaps(i->sector, i->size, sector, size)
		slot = tl_hash_slot(mdev, sector);
		first = 1;
		for (;;) {
			int have_unacked = 0;
			int have_conflict = 0;
			prepare_to_wait(&mdev->misc_wait, &wait,
				TASK_INTERRUPTIBLE);
			hlist_for_each_entry(i, n, slot, colision) {
				if (OVERLAPS) {
					/* only ALERT on first iteration,
					 * we may be woken up early... */
					if (first)
						dev_alert(DEV, "%s[%u] Concurrent local write detected!"
						      "	new: %llus +%u; pending: %llus +%u\n",
						      current->comm, current->pid,
						      (unsigned long long)sector, size,
						      (unsigned long long)i->sector, i->size);
					if (i->rq_state & RQ_NET_PENDING)
						++have_unacked;
					++have_conflict;
				}
			}
#undef OVERLAPS
			if (!have_conflict)
				break;

			/* Discard Ack only for the _first_ iteration */
			if (first && discard && have_unacked) {
				dev_alert(DEV, "Concurrent write! [DISCARD BY FLAG] sec=%llus\n",
				     (unsigned long long)sector);
				inc_unacked(mdev);
				e->w.cb = e_send_discard_ack;
				list_add_tail(&e->w.list, &mdev->done_ee);

				spin_unlock_irq(&mdev->req_lock);

				/* we could probably send that P_DISCARD_ACK ourselves,
				 * but I don't like the receiver using the msock */

				put_ldev(mdev);
				wake_asender(mdev);
				finish_wait(&mdev->misc_wait, &wait);
				return TRUE;
			}

			if (signal_pending(current)) {
				hlist_del_init(&e->colision);

				spin_unlock_irq(&mdev->req_lock);

				finish_wait(&mdev->misc_wait, &wait);
				goto out_interrupted;
			}

			spin_unlock_irq(&mdev->req_lock);
			if (first) {
				first = 0;
				dev_alert(DEV, "Concurrent write! [W AFTERWARDS] "
				     "sec=%llus\n", (unsigned long long)sector);
			} else if (discard) {
				/* we had none on the first iteration.
				 * there must be none now. */
				D_ASSERT(have_unacked == 0);
			}
			schedule();
			spin_lock_irq(&mdev->req_lock);
		}
		finish_wait(&mdev->misc_wait, &wait);
	}

	list_add(&e->w.list, &mdev->active_ee);
	spin_unlock_irq(&mdev->req_lock);

	switch (mdev->net_conf->wire_protocol) {
	case DRBD_PROT_C:
		inc_unacked(mdev);
		/* corresponding dec_unacked() in e_end_block()
		 * respective _drbd_clear_done_ee */
		break;
	case DRBD_PROT_B:
		/* I really don't like it that the receiver thread
		 * sends on the msock, but anyways */
		drbd_send_ack(mdev, P_RECV_ACK, e);
		break;
	case DRBD_PROT_A:
		/* nothing to do */
		break;
	}

	if (mdev->state.pdsk == D_DISKLESS) {
		/* In case we have the only disk of the cluster, */
		drbd_set_out_of_sync(mdev, e->sector, e->size);
		e->flags |= EE_CALL_AL_COMPLETE_IO;
		drbd_al_begin_io(mdev, e->sector);
	}

	e->private_bio->bi_rw = rw;
	trace_drbd_ee(mdev, e, "submitting for (data)write");
	trace_drbd_bio(mdev, "Sec", e->private_bio, 0, NULL);
	drbd_generic_make_request(mdev, DRBD_FAULT_DT_WR, e->private_bio);
	/* accounting done in endio */

	maybe_kick_lo(mdev);
	return TRUE;

out_interrupted:
	/* yes, the epoch_size now is imbalanced.
	 * but we drop the connection anyways, so we don't have a chance to
	 * receive a barrier... atomic_inc(&mdev->epoch_size); */
	put_ldev(mdev);
	drbd_free_ee(mdev, e);
	return FALSE;
}

STATIC int receive_DataRequest(struct drbd_conf *mdev, struct p_header *h)
{
	sector_t sector;
	const sector_t capacity = drbd_get_capacity(mdev->this_bdev);
	struct drbd_epoch_entry *e;
	struct digest_info *di = NULL;
	int size, digest_size;
	unsigned int fault_type;
	struct p_block_req *p =
		(struct p_block_req *)h;
	const int brps = sizeof(*p)-sizeof(*h);

	if (drbd_recv(mdev, h->payload, brps) != brps)
		return FALSE;

	sector = be64_to_cpu(p->sector);
	size   = be32_to_cpu(p->blksize);

	if (size <= 0 || (size & 0x1ff) != 0 || size > DRBD_MAX_SEGMENT_SIZE) {
		dev_err(DEV, "%s:%d: sector: %llus, size: %u\n", __FILE__, __LINE__,
				(unsigned long long)sector, size);
		return FALSE;
	}
	if (sector + (size>>9) > capacity) {
		dev_err(DEV, "%s:%d: sector: %llus, size: %u\n", __FILE__, __LINE__,
				(unsigned long long)sector, size);
		return FALSE;
	}

	if (!get_ldev_if_state(mdev, D_UP_TO_DATE)) {
		if (DRBD_ratelimit(5*HZ, 5))
			dev_err(DEV, "Can not satisfy peer's read request, "
			    "no local data.\n");
		drbd_send_ack_rp(mdev, h->command == P_DATA_REQUEST ? P_NEG_DREPLY :
				 P_NEG_RS_DREPLY , p);
		return drbd_drain_block(mdev, h->length - brps);
	}

	/* GFP_NOIO, because we must not cause arbitrary write-out: in a DRBD
	 * "criss-cross" setup, that might cause write-out on some other DRBD,
	 * which in turn might block on the other node at this very place.  */
	e = drbd_alloc_ee(mdev, p->block_id, sector, size, GFP_NOIO);
	if (!e) {
		put_ldev(mdev);
		return FALSE;
	}

	e->private_bio->bi_rw = READ;
	e->private_bio->bi_end_io = drbd_endio_read_sec;

	switch (h->command) {
	case P_DATA_REQUEST:
		e->w.cb = w_e_end_data_req;
		fault_type = DRBD_FAULT_DT_RD;
		break;
	case P_RS_DATA_REQUEST:
		e->w.cb = w_e_end_rsdata_req;
		fault_type = DRBD_FAULT_RS_RD;
		/* Eventually this should become asynchronously. Currently it
		 * blocks the whole receiver just to delay the reading of a
		 * resync data block.
		 * the drbd_work_queue mechanism is made for this...
		 */
		if (!drbd_rs_begin_io(mdev, sector)) {
			/* we have been interrupted,
			 * probably connection lost! */
			D_ASSERT(signal_pending(current));
			goto out_free_e;
		}
		break;

	case P_OV_REPLY:
	case P_CSUM_RS_REQUEST:
		fault_type = DRBD_FAULT_RS_RD;
		digest_size = h->length - brps ;
		di = kmalloc(sizeof(*di) + digest_size, GFP_NOIO);
		if (!di)
			goto out_free_e;

		di->digest_size = digest_size;
		di->digest = (((char *)di)+sizeof(struct digest_info));

		if (drbd_recv(mdev, di->digest, digest_size) != digest_size)
			goto out_free_e;

		e->block_id = (u64)(unsigned long)di;
		if (h->command == P_CSUM_RS_REQUEST) {
			D_ASSERT(mdev->agreed_pro_version >= 89);
			e->w.cb = w_e_end_csum_rs_req;
		} else if (h->command == P_OV_REPLY) {
			e->w.cb = w_e_end_ov_reply;
			dec_rs_pending(mdev);
			break;
		}

		if (!drbd_rs_begin_io(mdev, sector)) {
			/* we have been interrupted, probably connection lost! */
			D_ASSERT(signal_pending(current));
			goto out_free_e;
		}
		break;

	case P_OV_REQUEST:
		if (mdev->state.conn >= C_CONNECTED &&
		    mdev->state.conn != C_VERIFY_T)
			dev_warn(DEV, "ASSERT FAILED: got P_OV_REQUEST while being %s\n",
				drbd_conn_str(mdev->state.conn));
		if (mdev->ov_start_sector == ~(sector_t)0 &&
		    mdev->agreed_pro_version >= 90) {
			mdev->ov_start_sector = sector;
			mdev->ov_position = sector;
			mdev->ov_left = mdev->rs_total - BM_SECT_TO_BIT(sector);
			dev_info(DEV, "Online Verify start sector: %llu\n",
					(unsigned long long)sector);
		}
		e->w.cb = w_e_end_ov_req;
		fault_type = DRBD_FAULT_RS_RD;
		/* Eventually this should become asynchronous. Currently it
		 * blocks the whole receiver just to delay the reading of a
		 * resync data block.
		 * the drbd_work_queue mechanism is made for this...
		 */
		if (!drbd_rs_begin_io(mdev, sector)) {
			/* we have been interrupted,
			 * probably connection lost! */
			D_ASSERT(signal_pending(current));
			goto out_free_e;
		}
		break;


	default:
		dev_err(DEV, "unexpected command (%s) in receive_DataRequest\n",
		    cmdname(h->command));
		fault_type = DRBD_FAULT_MAX;
	}

	spin_lock_irq(&mdev->req_lock);
	list_add(&e->w.list, &mdev->read_ee);
	spin_unlock_irq(&mdev->req_lock);

	inc_unacked(mdev);

	trace_drbd_ee(mdev, e, "submitting for read");
	trace_drbd_bio(mdev, "Sec", e->private_bio, 0, NULL);
	drbd_generic_make_request(mdev, fault_type, e->private_bio);
	maybe_kick_lo(mdev);

	return TRUE;

out_free_e:
	kfree(di);
	put_ldev(mdev);
	drbd_free_ee(mdev, e);
	return FALSE;
}

STATIC int drbd_asb_recover_0p(struct drbd_conf *mdev) __must_hold(local)
{
	int self, peer, rv = -100;
	unsigned long ch_self, ch_peer;

	self = mdev->ldev->md.uuid[UI_BITMAP] & 1;
	peer = mdev->p_uuid[UI_BITMAP] & 1;

	ch_peer = mdev->p_uuid[UI_SIZE];
	ch_self = mdev->comm_bm_set;

	switch (mdev->net_conf->after_sb_0p) {
	case ASB_CONSENSUS:
	case ASB_DISCARD_SECONDARY:
	case ASB_CALL_HELPER:
		dev_err(DEV, "Configuration error.\n");
		break;
	case ASB_DISCONNECT:
		break;
	case ASB_DISCARD_YOUNGER_PRI:
		if (self == 0 && peer == 1) {
			rv = -1;
			break;
		}
		if (self == 1 && peer == 0) {
			rv =  1;
			break;
		}
		/* Else fall through to one of the other strategies... */
	case ASB_DISCARD_OLDER_PRI:
		if (self == 0 && peer == 1) {
			rv = 1;
			break;
		}
		if (self == 1 && peer == 0) {
			rv = -1;
			break;
		}
		/* Else fall through to one of the other strategies... */
		dev_warn(DEV, "Discard younger/older primary did not find a decision\n"
		     "Using discard-least-changes instead\n");
	case ASB_DISCARD_ZERO_CHG:
		if (ch_peer == 0 && ch_self == 0) {
			rv = test_bit(DISCARD_CONCURRENT, &mdev->flags)
				? -1 : 1;
			break;
		} else {
			if (ch_peer == 0) { rv =  1; break; }
			if (ch_self == 0) { rv = -1; break; }
		}
		if (mdev->net_conf->after_sb_0p == ASB_DISCARD_ZERO_CHG)
			break;
	case ASB_DISCARD_LEAST_CHG:
		if	(ch_self < ch_peer)
			rv = -1;
		else if (ch_self > ch_peer)
			rv =  1;
		else /* ( ch_self == ch_peer ) */
		     /* Well, then use something else. */
			rv = test_bit(DISCARD_CONCURRENT, &mdev->flags)
				? -1 : 1;
		break;
	case ASB_DISCARD_LOCAL:
		rv = -1;
		break;
	case ASB_DISCARD_REMOTE:
		rv =  1;
	}

	return rv;
}

STATIC int drbd_asb_recover_1p(struct drbd_conf *mdev) __must_hold(local)
{
	int self, peer, hg, rv = -100;

	self = mdev->ldev->md.uuid[UI_BITMAP] & 1;
	peer = mdev->p_uuid[UI_BITMAP] & 1;

	switch (mdev->net_conf->after_sb_1p) {
	case ASB_DISCARD_YOUNGER_PRI:
	case ASB_DISCARD_OLDER_PRI:
	case ASB_DISCARD_LEAST_CHG:
	case ASB_DISCARD_LOCAL:
	case ASB_DISCARD_REMOTE:
		dev_err(DEV, "Configuration error.\n");
		break;
	case ASB_DISCONNECT:
		break;
	case ASB_CONSENSUS:
		hg = drbd_asb_recover_0p(mdev);
		if (hg == -1 && mdev->state.role == R_SECONDARY)
			rv = hg;
		if (hg == 1  && mdev->state.role == R_PRIMARY)
			rv = hg;
		break;
	case ASB_VIOLENTLY:
		rv = drbd_asb_recover_0p(mdev);
		break;
	case ASB_DISCARD_SECONDARY:
		return mdev->state.role == R_PRIMARY ? 1 : -1;
	case ASB_CALL_HELPER:
		hg = drbd_asb_recover_0p(mdev);
		if (hg == -1 && mdev->state.role == R_PRIMARY) {
			self = drbd_set_role(mdev, R_SECONDARY, 0);
			 /* drbd_change_state() does not sleep while in SS_IN_TRANSIENT_STATE,
			  * we might be here in C_WF_REPORT_PARAMS which is transient.
			  * we do not need to wait for the after state change work either. */
			self = drbd_change_state(mdev, CS_VERBOSE, NS(role, R_SECONDARY));
			if (self != SS_SUCCESS) {
				drbd_khelper(mdev, "pri-lost-after-sb");
			} else {
				dev_warn(DEV, "Successfully gave up primary role.\n");
				rv = hg;
			}
		} else
			rv = hg;
	}

	return rv;
}

STATIC int drbd_asb_recover_2p(struct drbd_conf *mdev) __must_hold(local)
{
	int self, peer, hg, rv = -100;

	self = mdev->ldev->md.uuid[UI_BITMAP] & 1;
	peer = mdev->p_uuid[UI_BITMAP] & 1;

	switch (mdev->net_conf->after_sb_2p) {
	case ASB_DISCARD_YOUNGER_PRI:
	case ASB_DISCARD_OLDER_PRI:
	case ASB_DISCARD_LEAST_CHG:
	case ASB_DISCARD_LOCAL:
	case ASB_DISCARD_REMOTE:
	case ASB_CONSENSUS:
	case ASB_DISCARD_SECONDARY:
		dev_err(DEV, "Configuration error.\n");
		break;
	case ASB_VIOLENTLY:
		rv = drbd_asb_recover_0p(mdev);
		break;
	case ASB_DISCONNECT:
		break;
	case ASB_CALL_HELPER:
		hg = drbd_asb_recover_0p(mdev);
		if (hg == -1) {
			 /* drbd_change_state() does not sleep while in SS_IN_TRANSIENT_STATE,
			  * we might be here in C_WF_REPORT_PARAMS which is transient.
			  * we do not need to wait for the after state change work either. */
			self = drbd_change_state(mdev, CS_VERBOSE, NS(role, R_SECONDARY));
			if (self != SS_SUCCESS) {
				drbd_khelper(mdev, "pri-lost-after-sb");
			} else {
				dev_warn(DEV, "Successfully gave up primary role.\n");
				rv = hg;
			}
		} else
			rv = hg;
	}

	return rv;
}

STATIC void drbd_uuid_dump(struct drbd_conf *mdev, char *text, u64 *uuid,
			   u64 bits, u64 flags)
{
	if (!uuid) {
		dev_info(DEV, "%s uuid info vanished while I was looking!\n", text);
		return;
	}
	dev_info(DEV, "%s %016llX:%016llX:%016llX:%016llX bits:%llu flags:%llX\n",
	     text,
	     (unsigned long long)uuid[UI_CURRENT],
	     (unsigned long long)uuid[UI_BITMAP],
	     (unsigned long long)uuid[UI_HISTORY_START],
	     (unsigned long long)uuid[UI_HISTORY_END],
	     (unsigned long long)bits,
	     (unsigned long long)flags);
}

/*
  100	after split brain try auto recover
    2	C_SYNC_SOURCE set BitMap
    1	C_SYNC_SOURCE use BitMap
    0	no Sync
   -1	C_SYNC_TARGET use BitMap
   -2	C_SYNC_TARGET set BitMap
 -100	after split brain, disconnect
-1000	unrelated data
 */
STATIC int drbd_uuid_compare(struct drbd_conf *mdev, int *rule_nr) __must_hold(local)
{
	u64 self, peer;
	int i, j;

	self = mdev->ldev->md.uuid[UI_CURRENT] & ~((u64)1);
	peer = mdev->p_uuid[UI_CURRENT] & ~((u64)1);

	*rule_nr = 10;
	if (self == UUID_JUST_CREATED && peer == UUID_JUST_CREATED)
		return 0;

	*rule_nr = 20;
	if ((self == UUID_JUST_CREATED || self == (u64)0) &&
	     peer != UUID_JUST_CREATED)
		return -2;

	*rule_nr = 30;
	if (self != UUID_JUST_CREATED &&
	    (peer == UUID_JUST_CREATED || peer == (u64)0))
		return 2;

	if (self == peer) {
		int rct, dc; /* roles at crash time */

		if (mdev->p_uuid[UI_BITMAP] == (u64)0 && mdev->ldev->md.uuid[UI_BITMAP] != (u64)0) {

			if (mdev->agreed_pro_version < 91)
				return -1001;

			if ((mdev->ldev->md.uuid[UI_BITMAP] & ~((u64)1)) == (mdev->p_uuid[UI_HISTORY_START] & ~((u64)1)) &&
			    (mdev->ldev->md.uuid[UI_HISTORY_START] & ~((u64)1)) == (mdev->p_uuid[UI_HISTORY_START + 1] & ~((u64)1))) {
				dev_info(DEV, "was SyncSource, missed the resync finished event, corrected myself:\n");
				drbd_uuid_set_bm(mdev, 0UL);

				drbd_uuid_dump(mdev, "self", mdev->ldev->md.uuid,
					       mdev->state.disk >= D_NEGOTIATING ? drbd_bm_total_weight(mdev) : 0, 0);
				*rule_nr = 34;
			} else {
				dev_info(DEV, "was SyncSource (peer failed to write sync_uuid)\n");
				*rule_nr = 36;
			}

			return 1;
		}

		if (mdev->ldev->md.uuid[UI_BITMAP] == (u64)0 && mdev->p_uuid[UI_BITMAP] != (u64)0) {

			if (mdev->agreed_pro_version < 91)
				return -1001;

			if ((mdev->ldev->md.uuid[UI_HISTORY_START] & ~((u64)1)) == (mdev->p_uuid[UI_BITMAP] & ~((u64)1)) &&
			    (mdev->ldev->md.uuid[UI_HISTORY_START + 1] & ~((u64)1)) == (mdev->p_uuid[UI_HISTORY_START] & ~((u64)1))) {
				dev_info(DEV, "was SyncTarget, peer missed the resync finished event, corrected peer:\n");

				mdev->p_uuid[UI_HISTORY_START + 1] = mdev->p_uuid[UI_HISTORY_START];
				mdev->p_uuid[UI_HISTORY_START] = mdev->p_uuid[UI_BITMAP];
				mdev->p_uuid[UI_BITMAP] = 0UL;

				drbd_uuid_dump(mdev, "peer", mdev->p_uuid, mdev->p_uuid[UI_SIZE], mdev->p_uuid[UI_FLAGS]);
				*rule_nr = 35;
			} else {
				dev_info(DEV, "was SyncTarget (failed to write sync_uuid)\n");
				*rule_nr = 37;
			}

			return -1;
		}

		/* Common power [off|failure] */
		rct = (test_bit(CRASHED_PRIMARY, &mdev->flags) ? 1 : 0) +
			(mdev->p_uuid[UI_FLAGS] & 2);
		/* lowest bit is set when we were primary,
		 * next bit (weight 2) is set when peer was primary */
		*rule_nr = 40;

		switch (rct) {
		case 0: /* !self_pri && !peer_pri */ return 0;
		case 1: /*  self_pri && !peer_pri */ return 1;
		case 2: /* !self_pri &&  peer_pri */ return -1;
		case 3: /*  self_pri &&  peer_pri */
			dc = test_bit(DISCARD_CONCURRENT, &mdev->flags);
			return dc ? -1 : 1;
		}
	}

	*rule_nr = 50;
	peer = mdev->p_uuid[UI_BITMAP] & ~((u64)1);
	if (self == peer)
		return -1;

	*rule_nr = 51;
	peer = mdev->p_uuid[UI_HISTORY_START] & ~((u64)1);
	if (self == peer) {
		self = mdev->ldev->md.uuid[UI_HISTORY_START] & ~((u64)1);
		peer = mdev->p_uuid[UI_HISTORY_START + 1] & ~((u64)1);
		if (self == peer) {
			/* The last P_SYNC_UUID did not get though. Undo the last start of
			   resync as sync source modifications of the peer's UUIDs. */

			if (mdev->agreed_pro_version < 91)
				return -1001;

			mdev->p_uuid[UI_BITMAP] = mdev->p_uuid[UI_HISTORY_START];
			mdev->p_uuid[UI_HISTORY_START] = mdev->p_uuid[UI_HISTORY_START + 1];
			return -1;
		}
	}

	*rule_nr = 60;
	self = mdev->ldev->md.uuid[UI_CURRENT] & ~((u64)1);
	for (i = UI_HISTORY_START; i <= UI_HISTORY_END; i++) {
		peer = mdev->p_uuid[i] & ~((u64)1);
		if (self == peer)
			return -2;
	}

	*rule_nr = 70;
	self = mdev->ldev->md.uuid[UI_BITMAP] & ~((u64)1);
	peer = mdev->p_uuid[UI_CURRENT] & ~((u64)1);
	if (self == peer)
		return 1;

	*rule_nr = 71;
	self = mdev->ldev->md.uuid[UI_HISTORY_START] & ~((u64)1);
	if (self == peer) {
		self = mdev->ldev->md.uuid[UI_HISTORY_START + 1] & ~((u64)1);
		peer = mdev->p_uuid[UI_HISTORY_START] & ~((u64)1);
		if (self == peer) {
			/* The last P_SYNC_UUID did not get though. Undo the last start of
			   resync as sync source modifications of our UUIDs. */

			if (mdev->agreed_pro_version < 91)
				return -1001;

			_drbd_uuid_set(mdev, UI_BITMAP, mdev->ldev->md.uuid[UI_HISTORY_START]);
			_drbd_uuid_set(mdev, UI_HISTORY_START, mdev->ldev->md.uuid[UI_HISTORY_START + 1]);

			dev_info(DEV, "Undid last start of resync:\n");

			drbd_uuid_dump(mdev, "self", mdev->ldev->md.uuid,
				       mdev->state.disk >= D_NEGOTIATING ? drbd_bm_total_weight(mdev) : 0, 0);

			return 1;
		}
	}


	*rule_nr = 80;
	peer = mdev->p_uuid[UI_CURRENT] & ~((u64)1);
	for (i = UI_HISTORY_START; i <= UI_HISTORY_END; i++) {
		self = mdev->ldev->md.uuid[i] & ~((u64)1);
		if (self == peer)
			return 2;
	}

	*rule_nr = 90;
	self = mdev->ldev->md.uuid[UI_BITMAP] & ~((u64)1);
	peer = mdev->p_uuid[UI_BITMAP] & ~((u64)1);
	if (self == peer && self != ((u64)0))
		return 100;

	*rule_nr = 100;
	for (i = UI_HISTORY_START; i <= UI_HISTORY_END; i++) {
		self = mdev->ldev->md.uuid[i] & ~((u64)1);
		for (j = UI_HISTORY_START; j <= UI_HISTORY_END; j++) {
			peer = mdev->p_uuid[j] & ~((u64)1);
			if (self == peer)
				return -100;
		}
	}

	return -1000;
}

/* drbd_sync_handshake() returns the new conn state on success, or
   CONN_MASK (-1) on failure.
 */
STATIC enum drbd_conns drbd_sync_handshake(struct drbd_conf *mdev, enum drbd_role peer_role,
					   enum drbd_disk_state peer_disk) __must_hold(local)
{
	int hg, rule_nr;
	enum drbd_conns rv = C_MASK;
	enum drbd_disk_state mydisk;

	mydisk = mdev->state.disk;
	if (mydisk == D_NEGOTIATING)
		mydisk = mdev->new_state_tmp.disk;

	dev_info(DEV, "drbd_sync_handshake:\n");
	drbd_uuid_dump(mdev, "self", mdev->ldev->md.uuid, mdev->comm_bm_set, 0);
	drbd_uuid_dump(mdev, "peer", mdev->p_uuid,
		       mdev->p_uuid[UI_SIZE], mdev->p_uuid[UI_FLAGS]);

	hg = drbd_uuid_compare(mdev, &rule_nr);

	dev_info(DEV, "uuid_compare()=%d by rule %d\n", hg, rule_nr);

	if (hg == -1000) {
		dev_alert(DEV, "Unrelated data, aborting!\n");
		return C_MASK;
	}
	if (hg == -1001) {
		dev_alert(DEV, "To resolve this both sides have to support at least protocol\n");
		return C_MASK;
	}

	if    ((mydisk == D_INCONSISTENT && peer_disk > D_INCONSISTENT) ||
	    (peer_disk == D_INCONSISTENT && mydisk    > D_INCONSISTENT)) {
		int f = (hg == -100) || abs(hg) == 2;
		hg = mydisk > D_INCONSISTENT ? 1 : -1;
		if (f)
			hg = hg*2;
		dev_info(DEV, "Becoming sync %s due to disk states.\n",
		     hg > 0 ? "source" : "target");
	}

	if (abs(hg) == 100)
		drbd_khelper(mdev, "initial-split-brain");

	if (hg == 100 || (hg == -100 && mdev->net_conf->always_asbp)) {
		int pcount = (mdev->state.role == R_PRIMARY)
			   + (peer_role == R_PRIMARY);
		int forced = (hg == -100);

		switch (pcount) {
		case 0:
			hg = drbd_asb_recover_0p(mdev);
			break;
		case 1:
			hg = drbd_asb_recover_1p(mdev);
			break;
		case 2:
			hg = drbd_asb_recover_2p(mdev);
			break;
		}
		if (abs(hg) < 100) {
			dev_warn(DEV, "Split-Brain detected, %d primaries, "
			     "automatically solved. Sync from %s node\n",
			     pcount, (hg < 0) ? "peer" : "this");
			if (forced) {
				dev_warn(DEV, "Doing a full sync, since"
				     " UUIDs where ambiguous.\n");
				hg = hg*2;
			}
		}
	}

	if (hg == -100) {
		if (mdev->net_conf->want_lose && !(mdev->p_uuid[UI_FLAGS]&1))
			hg = -1;
		if (!mdev->net_conf->want_lose && (mdev->p_uuid[UI_FLAGS]&1))
			hg = 1;

		if (abs(hg) < 100)
			dev_warn(DEV, "Split-Brain detected, manually solved. "
			     "Sync from %s node\n",
			     (hg < 0) ? "peer" : "this");
	}

	if (hg == -100) {
		/* FIXME this log message is not correct if we end up here
		 * after an attempted attach on a diskless node.
		 * We just refuse to attach -- well, we drop the "connection"
		 * to that disk, in a way... */
		dev_alert(DEV, "Split-Brain detected but unresolved, dropping connection!\n");
		drbd_khelper(mdev, "split-brain");
		return C_MASK;
	}

	if (hg > 0 && mydisk <= D_INCONSISTENT) {
		dev_err(DEV, "I shall become SyncSource, but I am inconsistent!\n");
		return C_MASK;
	}

	if (hg < 0 && /* by intention we do not use mydisk here. */
	    mdev->state.role == R_PRIMARY && mdev->state.disk >= D_CONSISTENT) {
		switch (mdev->net_conf->rr_conflict) {
		case ASB_CALL_HELPER:
			drbd_khelper(mdev, "pri-lost");
			/* fall through */
		case ASB_DISCONNECT:
			dev_err(DEV, "I shall become SyncTarget, but I am primary!\n");
			return C_MASK;
		case ASB_VIOLENTLY:
			dev_warn(DEV, "Becoming SyncTarget, violating the stable-data"
			     "assumption\n");
		}
	}

	if (mdev->net_conf->dry_run || test_bit(CONN_DRY_RUN, &mdev->flags)) {
		if (hg == 0)
			dev_info(DEV, "dry-run connect: No resync, would become Connected immediately.\n");
		else
			dev_info(DEV, "dry-run connect: Would become %s, doing a %s resync.",
				 drbd_conn_str(hg > 0 ? C_SYNC_SOURCE : C_SYNC_TARGET),
				 abs(hg) >= 2 ? "full" : "bit-map based");
		return C_MASK;
	}

	if (abs(hg) >= 2) {
		dev_info(DEV, "Writing the whole bitmap, full sync required after drbd_sync_handshake.\n");
		if (drbd_bitmap_io(mdev, &drbd_bmio_set_n_write, "set_n_write from sync_handshake"))
			return C_MASK;
	}

	if (hg > 0) { /* become sync source. */
		rv = C_WF_BITMAP_S;
	} else if (hg < 0) { /* become sync target */
		rv = C_WF_BITMAP_T;
	} else {
		rv = C_CONNECTED;
		if (drbd_bm_total_weight(mdev)) {
			dev_info(DEV, "No resync, but %lu bits in bitmap!\n",
			     drbd_bm_total_weight(mdev));
		}
	}

	return rv;
}

/* returns 1 if invalid */
STATIC int cmp_after_sb(enum drbd_after_sb_p peer, enum drbd_after_sb_p self)
{
	/* ASB_DISCARD_REMOTE - ASB_DISCARD_LOCAL is valid */
	if ((peer == ASB_DISCARD_REMOTE && self == ASB_DISCARD_LOCAL) ||
	    (self == ASB_DISCARD_REMOTE && peer == ASB_DISCARD_LOCAL))
		return 0;

	/* any other things with ASB_DISCARD_REMOTE or ASB_DISCARD_LOCAL are invalid */
	if (peer == ASB_DISCARD_REMOTE || peer == ASB_DISCARD_LOCAL ||
	    self == ASB_DISCARD_REMOTE || self == ASB_DISCARD_LOCAL)
		return 1;

	/* everything else is valid if they are equal on both sides. */
	if (peer == self)
		return 0;

	/* everything es is invalid. */
	return 1;
}

STATIC int receive_protocol(struct drbd_conf *mdev, struct p_header *h)
{
	struct p_protocol *p = (struct p_protocol *)h;
	int header_size, data_size;
	int p_proto, p_after_sb_0p, p_after_sb_1p, p_after_sb_2p;
	int p_want_lose, p_two_primaries, cf;
	char p_integrity_alg[SHARED_SECRET_MAX] = "";

	header_size = sizeof(*p) - sizeof(*h);
	data_size   = h->length  - header_size;

	if (drbd_recv(mdev, h->payload, header_size) != header_size)
		return FALSE;

	p_proto		= be32_to_cpu(p->protocol);
	p_after_sb_0p	= be32_to_cpu(p->after_sb_0p);
	p_after_sb_1p	= be32_to_cpu(p->after_sb_1p);
	p_after_sb_2p	= be32_to_cpu(p->after_sb_2p);
	p_two_primaries = be32_to_cpu(p->two_primaries);
	cf		= be32_to_cpu(p->conn_flags);
	p_want_lose = cf & CF_WANT_LOSE;

	clear_bit(CONN_DRY_RUN, &mdev->flags);

	if (cf & CF_DRY_RUN)
		set_bit(CONN_DRY_RUN, &mdev->flags);

	if (p_proto != mdev->net_conf->wire_protocol) {
		dev_err(DEV, "incompatible communication protocols\n");
		goto disconnect;
	}

	if (cmp_after_sb(p_after_sb_0p, mdev->net_conf->after_sb_0p)) {
		dev_err(DEV, "incompatible after-sb-0pri settings\n");
		goto disconnect;
	}

	if (cmp_after_sb(p_after_sb_1p, mdev->net_conf->after_sb_1p)) {
		dev_err(DEV, "incompatible after-sb-1pri settings\n");
		goto disconnect;
	}

	if (cmp_after_sb(p_after_sb_2p, mdev->net_conf->after_sb_2p)) {
		dev_err(DEV, "incompatible after-sb-2pri settings\n");
		goto disconnect;
	}

	if (p_want_lose && mdev->net_conf->want_lose) {
		dev_err(DEV, "both sides have the 'want_lose' flag set\n");
		goto disconnect;
	}

	if (p_two_primaries != mdev->net_conf->two_primaries) {
		dev_err(DEV, "incompatible setting of the two-primaries options\n");
		goto disconnect;
	}

	if (mdev->agreed_pro_version >= 87) {
		unsigned char *my_alg = mdev->net_conf->integrity_alg;

		if (drbd_recv(mdev, p_integrity_alg, data_size) != data_size)
			return FALSE;

		p_integrity_alg[SHARED_SECRET_MAX-1] = 0;
		if (strcmp(p_integrity_alg, my_alg)) {
			dev_err(DEV, "incompatible setting of the data-integrity-alg\n");
			goto disconnect;
		}
		dev_info(DEV, "data-integrity-alg: %s\n",
		     my_alg[0] ? my_alg : (unsigned char *)"<not-used>");
	}

	return TRUE;

disconnect:
	drbd_force_state(mdev, NS(conn, C_DISCONNECTING));
	return FALSE;
}

/* helper function
 * input: alg name, feature name
 * return: NULL (alg name was "")
 *         ERR_PTR(error) if something goes wrong
 *         or the crypto hash ptr, if it worked out ok. */
struct crypto_hash *drbd_crypto_alloc_digest_safe(const struct drbd_conf *mdev,
		const char *alg, const char *name)
{
	struct crypto_hash *tfm;

	if (!alg[0])
		return NULL;

	tfm = crypto_alloc_hash(alg, 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(tfm)) {
		dev_err(DEV, "Can not allocate \"%s\" as %s (reason: %ld)\n",
			alg, name, PTR_ERR(tfm));
		return tfm;
	}
	if (!drbd_crypto_is_hash(crypto_hash_tfm(tfm))) {
		crypto_free_hash(tfm);
		dev_err(DEV, "\"%s\" is not a digest (%s)\n", alg, name);
		return ERR_PTR(-EINVAL);
	}
	return tfm;
}

STATIC int receive_SyncParam(struct drbd_conf *mdev, struct p_header *h)
{
	int ok = TRUE;
	struct p_rs_param_89 *p = (struct p_rs_param_89 *)h;
	unsigned int header_size, data_size, exp_max_sz;
	struct crypto_hash *verify_tfm = NULL;
	struct crypto_hash *csums_tfm = NULL;
	const int apv = mdev->agreed_pro_version;

	exp_max_sz  = apv <= 87 ? sizeof(struct p_rs_param)
		    : apv == 88 ? sizeof(struct p_rs_param)
					+ SHARED_SECRET_MAX
		    : /* 89 */    sizeof(struct p_rs_param_89);

	if (h->length > exp_max_sz) {
		dev_err(DEV, "SyncParam packet too long: received %u, expected <= %u bytes\n",
		    h->length, exp_max_sz);
		return FALSE;
	}

	if (apv <= 88) {
		header_size = sizeof(struct p_rs_param) - sizeof(*h);
		data_size   = h->length  - header_size;
	} else /* apv >= 89 */ {
		header_size = sizeof(struct p_rs_param_89) - sizeof(*h);
		data_size   = h->length  - header_size;
		D_ASSERT(data_size == 0);
	}

	/* initialize verify_alg and csums_alg */
	memset(p->verify_alg, 0, 2 * SHARED_SECRET_MAX);

	if (drbd_recv(mdev, h->payload, header_size) != header_size)
		return FALSE;

	mdev->sync_conf.rate	  = be32_to_cpu(p->rate);

	if (apv >= 88) {
		if (apv == 88) {
			if (data_size > SHARED_SECRET_MAX) {
				dev_err(DEV, "verify-alg too long, "
				    "peer wants %u, accepting only %u byte\n",
						data_size, SHARED_SECRET_MAX);
				return FALSE;
			}

			if (drbd_recv(mdev, p->verify_alg, data_size) != data_size)
				return FALSE;

			/* we expect NUL terminated string */
			/* but just in case someone tries to be evil */
			D_ASSERT(p->verify_alg[data_size-1] == 0);
			p->verify_alg[data_size-1] = 0;

		} else /* apv >= 89 */ {
			/* we still expect NUL terminated strings */
			/* but just in case someone tries to be evil */
			D_ASSERT(p->verify_alg[SHARED_SECRET_MAX-1] == 0);
			D_ASSERT(p->csums_alg[SHARED_SECRET_MAX-1] == 0);
			p->verify_alg[SHARED_SECRET_MAX-1] = 0;
			p->csums_alg[SHARED_SECRET_MAX-1] = 0;
		}

		if (strcmp(mdev->sync_conf.verify_alg, p->verify_alg)) {
			if (mdev->state.conn == C_WF_REPORT_PARAMS) {
				dev_err(DEV, "Different verify-alg settings. me=\"%s\" peer=\"%s\"\n",
				    mdev->sync_conf.verify_alg, p->verify_alg);
				goto disconnect;
			}
			verify_tfm = drbd_crypto_alloc_digest_safe(mdev,
					p->verify_alg, "verify-alg");
			if (IS_ERR(verify_tfm)) {
				verify_tfm = NULL;
				goto disconnect;
			}
		}

		if (apv >= 89 && strcmp(mdev->sync_conf.csums_alg, p->csums_alg)) {
			if (mdev->state.conn == C_WF_REPORT_PARAMS) {
				dev_err(DEV, "Different csums-alg settings. me=\"%s\" peer=\"%s\"\n",
				    mdev->sync_conf.csums_alg, p->csums_alg);
				goto disconnect;
			}
			csums_tfm = drbd_crypto_alloc_digest_safe(mdev,
					p->csums_alg, "csums-alg");
			if (IS_ERR(csums_tfm)) {
				csums_tfm = NULL;
				goto disconnect;
			}
		}


		spin_lock(&mdev->peer_seq_lock);
		/* lock against drbd_nl_syncer_conf() */
		if (verify_tfm) {
			strcpy(mdev->sync_conf.verify_alg, p->verify_alg);
			mdev->sync_conf.verify_alg_len = strlen(p->verify_alg) + 1;
			crypto_free_hash(mdev->verify_tfm);
			mdev->verify_tfm = verify_tfm;
			dev_info(DEV, "using verify-alg: \"%s\"\n", p->verify_alg);
		}
		if (csums_tfm) {
			strcpy(mdev->sync_conf.csums_alg, p->csums_alg);
			mdev->sync_conf.csums_alg_len = strlen(p->csums_alg) + 1;
			crypto_free_hash(mdev->csums_tfm);
			mdev->csums_tfm = csums_tfm;
			dev_info(DEV, "using csums-alg: \"%s\"\n", p->csums_alg);
		}
		spin_unlock(&mdev->peer_seq_lock);
	}

	return ok;
disconnect:
	/* just for completeness: actually not needed,
	 * as this is not reached if csums_tfm was ok. */
	crypto_free_hash(csums_tfm);
	/* but free the verify_tfm again, if csums_tfm did not work out */
	crypto_free_hash(verify_tfm);
	drbd_force_state(mdev, NS(conn, C_DISCONNECTING));
	return FALSE;
}

STATIC void drbd_setup_order_type(struct drbd_conf *mdev, int peer)
{
	/* sorry, we currently have no working implementation
	 * of distributed TCQ */
}

/* warn if the arguments differ by more than 12.5% */
static void warn_if_differ_considerably(struct drbd_conf *mdev,
	const char *s, sector_t a, sector_t b)
{
	sector_t d;
	if (a == 0 || b == 0)
		return;
	d = (a > b) ? (a - b) : (b - a);
	if (d > (a>>3) || d > (b>>3))
		dev_warn(DEV, "Considerable difference in %s: %llus vs. %llus\n", s,
		     (unsigned long long)a, (unsigned long long)b);
}

STATIC int receive_sizes(struct drbd_conf *mdev, struct p_header *h)
{
	struct p_sizes *p = (struct p_sizes *)h;
	enum determine_dev_size dd = unchanged;
	unsigned int max_seg_s;
	sector_t p_size, p_usize, my_usize;
	int ldsc = 0; /* local disk size changed */
	enum dds_flags ddsf;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;
	if (drbd_recv(mdev, h->payload, h->length) != h->length)
		return FALSE;

	p_size = be64_to_cpu(p->d_size);
	p_usize = be64_to_cpu(p->u_size);

	if (p_size == 0 && mdev->state.disk == D_DISKLESS) {
		dev_err(DEV, "some backing storage is needed\n");
		drbd_force_state(mdev, NS(conn, C_DISCONNECTING));
		return FALSE;
	}

	/* just store the peer's disk size for now.
	 * we still need to figure out whether we accept that. */
	mdev->p_size = p_size;

#define min_not_zero(l, r) (l == 0) ? r : ((r == 0) ? l : min(l, r))
	if (get_ldev(mdev)) {
		warn_if_differ_considerably(mdev, "lower level device sizes",
			   p_size, drbd_get_max_capacity(mdev->ldev));
		warn_if_differ_considerably(mdev, "user requested size",
					    p_usize, mdev->ldev->dc.disk_size);

		/* if this is the first connect, or an otherwise expected
		 * param exchange, choose the minimum */
		if (mdev->state.conn == C_WF_REPORT_PARAMS)
			p_usize = min_not_zero((sector_t)mdev->ldev->dc.disk_size,
					     p_usize);

		my_usize = mdev->ldev->dc.disk_size;

		if (mdev->ldev->dc.disk_size != p_usize) {
			mdev->ldev->dc.disk_size = p_usize;
			dev_info(DEV, "Peer sets u_size to %lu sectors\n",
			     (unsigned long)mdev->ldev->dc.disk_size);
		}

		/* Never shrink a device with usable data during connect.
		   But allow online shrinking if we are connected. */
		if (drbd_new_dev_size(mdev, mdev->ldev, 0) <
		   drbd_get_capacity(mdev->this_bdev) &&
		   mdev->state.disk >= D_OUTDATED &&
		   mdev->state.conn < C_CONNECTED) {
			dev_err(DEV, "The peer's disk size is too small!\n");
			drbd_force_state(mdev, NS(conn, C_DISCONNECTING));
			mdev->ldev->dc.disk_size = my_usize;
			put_ldev(mdev);
			return FALSE;
		}
		put_ldev(mdev);
	}
#undef min_not_zero

	ddsf = be16_to_cpu(p->dds_flags);
	if (get_ldev(mdev)) {
		dd = drbd_determin_dev_size(mdev, ddsf);
		put_ldev(mdev);
		if (dd == dev_size_error)
			return FALSE;
		drbd_md_sync(mdev);
	} else {
		/* I am diskless, need to accept the peer's size. */
		drbd_set_my_capacity(mdev, p_size);
	}

	if (get_ldev(mdev)) {
		if (mdev->ldev->known_size != drbd_get_capacity(mdev->ldev->backing_bdev)) {
			mdev->ldev->known_size = drbd_get_capacity(mdev->ldev->backing_bdev);
			ldsc = 1;
		}

		max_seg_s = be32_to_cpu(p->max_segment_size);
		if (max_seg_s != queue_max_segment_size(mdev->rq_queue))
			drbd_setup_queue_param(mdev, max_seg_s);

		drbd_setup_order_type(mdev, be16_to_cpu(p->queue_order_type));
		put_ldev(mdev);
	}

	if (mdev->state.conn > C_WF_REPORT_PARAMS) {
		if (be64_to_cpu(p->c_size) !=
		    drbd_get_capacity(mdev->this_bdev) || ldsc) {
			/* we have different sizes, probably peer
			 * needs to know my new size... */
			drbd_send_sizes(mdev, 0, ddsf);
		}
		if (test_and_clear_bit(RESIZE_PENDING, &mdev->flags) ||
		    (dd == grew && mdev->state.conn == C_CONNECTED)) {
			if (mdev->state.pdsk >= D_INCONSISTENT &&
			    mdev->state.disk >= D_INCONSISTENT) {
				if (ddsf & DDSF_NO_RESYNC)
					dev_info(DEV, "Resync of new storage suppressed with --assume-clean\n");
				else
					resync_after_online_grow(mdev);
			} else
				set_bit(RESYNC_AFTER_NEG, &mdev->flags);
		}
	}

	return TRUE;
}

STATIC int receive_uuids(struct drbd_conf *mdev, struct p_header *h)
{
	struct p_uuids *p = (struct p_uuids *)h;
	u64 *p_uuid;
	int i;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;
	if (drbd_recv(mdev, h->payload, h->length) != h->length)
		return FALSE;

	p_uuid = kmalloc(sizeof(u64)*UI_EXTENDED_SIZE, GFP_NOIO);

	for (i = UI_CURRENT; i < UI_EXTENDED_SIZE; i++)
		p_uuid[i] = be64_to_cpu(p->uuid[i]);

	kfree(mdev->p_uuid);
	mdev->p_uuid = p_uuid;

	if (mdev->state.conn < C_CONNECTED &&
	    mdev->state.disk < D_INCONSISTENT &&
	    mdev->state.role == R_PRIMARY &&
	    (mdev->ed_uuid & ~((u64)1)) != (p_uuid[UI_CURRENT] & ~((u64)1))) {
		dev_err(DEV, "Can only connect to data with current UUID=%016llX\n",
		    (unsigned long long)mdev->ed_uuid);
		drbd_force_state(mdev, NS(conn, C_DISCONNECTING));
		return FALSE;
	}

	if (get_ldev(mdev)) {
		int skip_initial_sync =
			mdev->state.conn == C_CONNECTED &&
			mdev->agreed_pro_version >= 90 &&
			mdev->ldev->md.uuid[UI_CURRENT] == UUID_JUST_CREATED &&
			(p_uuid[UI_FLAGS] & 8);
		if (skip_initial_sync) {
			dev_info(DEV, "Accepted new current UUID, preparing to skip initial sync\n");
			drbd_bitmap_io(mdev, &drbd_bmio_clear_n_write,
					"clear_n_write from receive_uuids");
			_drbd_uuid_set(mdev, UI_CURRENT, p_uuid[UI_CURRENT]);
			_drbd_uuid_set(mdev, UI_BITMAP, 0);
			_drbd_set_state(_NS2(mdev, disk, D_UP_TO_DATE, pdsk, D_UP_TO_DATE),
					CS_VERBOSE, NULL);
			drbd_md_sync(mdev);
		}
		put_ldev(mdev);
	}

	/* Before we test for the disk state, we should wait until an eventually
	   ongoing cluster wide state change is finished. That is important if
	   we are primary and are detaching from our disk. We need to see the
	   new disk state... */
	wait_event(mdev->misc_wait, !test_bit(CLUSTER_ST_CHANGE, &mdev->flags));
	if (mdev->state.conn >= C_CONNECTED && mdev->state.disk < D_INCONSISTENT)
		drbd_set_ed_uuid(mdev, p_uuid[UI_CURRENT]);

	return TRUE;
}

/**
 * convert_state() - Converts the peer's view of the cluster state to our point of view
 * @ps:		The state as seen by the peer.
 */
STATIC union drbd_state convert_state(union drbd_state ps)
{
	union drbd_state ms;

	static enum drbd_conns c_tab[] = {
		[C_CONNECTED] = C_CONNECTED,

		[C_STARTING_SYNC_S] = C_STARTING_SYNC_T,
		[C_STARTING_SYNC_T] = C_STARTING_SYNC_S,
		[C_DISCONNECTING] = C_TEAR_DOWN, /* C_NETWORK_FAILURE, */
		[C_VERIFY_S]       = C_VERIFY_T,
		[C_MASK]   = C_MASK,
	};

	ms.i = ps.i;

	ms.conn = c_tab[ps.conn];
	ms.peer = ps.role;
	ms.role = ps.peer;
	ms.pdsk = ps.disk;
	ms.disk = ps.pdsk;
	ms.peer_isp = (ps.aftr_isp | ps.user_isp);

	return ms;
}

STATIC int receive_req_state(struct drbd_conf *mdev, struct p_header *h)
{
	struct p_req_state *p = (struct p_req_state *)h;
	union drbd_state mask, val;
	int rv;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;
	if (drbd_recv(mdev, h->payload, h->length) != h->length)
		return FALSE;

	mask.i = be32_to_cpu(p->mask);
	val.i = be32_to_cpu(p->val);

	if (test_bit(DISCARD_CONCURRENT, &mdev->flags) &&
	    test_bit(CLUSTER_ST_CHANGE, &mdev->flags)) {
		drbd_send_sr_reply(mdev, SS_CONCURRENT_ST_CHG);
		return TRUE;
	}

	mask = convert_state(mask);
	val = convert_state(val);

	DRBD_STATE_DEBUG_INIT_VAL(val);
	rv = drbd_change_state(mdev, CS_VERBOSE, mask, val);

	drbd_send_sr_reply(mdev, rv);
	drbd_md_sync(mdev);

	return TRUE;
}

STATIC int receive_state(struct drbd_conf *mdev, struct p_header *h)
{
	struct p_state *p = (struct p_state *)h;
	enum drbd_conns nconn, oconn;
	union drbd_state ns, peer_state;
	enum drbd_disk_state real_peer_disk;
	int rv;

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h)))
		return FALSE;

	if (drbd_recv(mdev, h->payload, h->length) != h->length)
		return FALSE;

	peer_state.i = be32_to_cpu(p->state);

	real_peer_disk = peer_state.disk;
	if (peer_state.disk == D_NEGOTIATING) {
		real_peer_disk = mdev->p_uuid[UI_FLAGS] & 4 ? D_INCONSISTENT : D_CONSISTENT;
		dev_info(DEV, "real peer disk state = %s\n", drbd_disk_str(real_peer_disk));
	}

	spin_lock_irq(&mdev->req_lock);
 retry:
	oconn = nconn = mdev->state.conn;
	spin_unlock_irq(&mdev->req_lock);

	if (nconn == C_WF_REPORT_PARAMS)
		nconn = C_CONNECTED;

	if (mdev->p_uuid && peer_state.disk >= D_NEGOTIATING &&
	    get_ldev_if_state(mdev, D_NEGOTIATING)) {
		int cr; /* consider resync */

		/* if we established a new connection */
		cr  = (oconn < C_CONNECTED);
		/* if we had an established connection
		 * and one of the nodes newly attaches a disk */
		cr |= (oconn == C_CONNECTED &&
		       (peer_state.disk == D_NEGOTIATING ||
			mdev->state.disk == D_NEGOTIATING));
		/* if we have both been inconsistent, and the peer has been
		 * forced to be UpToDate with --overwrite-data */
		cr |= test_bit(CONSIDER_RESYNC, &mdev->flags);
		/* if we had been plain connected, and the admin requested to
		 * start a sync by "invalidate" or "invalidate-remote" */
		cr |= (oconn == C_CONNECTED &&
				(peer_state.conn >= C_STARTING_SYNC_S &&
				 peer_state.conn <= C_WF_BITMAP_T));

		if (cr)
			nconn = drbd_sync_handshake(mdev, peer_state.role, real_peer_disk);

		put_ldev(mdev);
		if (nconn == C_MASK) {
			nconn = C_CONNECTED;
			if (mdev->state.disk == D_NEGOTIATING) {
				drbd_force_state(mdev, NS(disk, D_DISKLESS));
			} else if (peer_state.disk == D_NEGOTIATING) {
				dev_err(DEV, "Disk attach process on the peer node was aborted.\n");
				peer_state.disk = D_DISKLESS;
				real_peer_disk = D_DISKLESS;
			} else {
				if (test_and_clear_bit(CONN_DRY_RUN, &mdev->flags))
					return FALSE;
				D_ASSERT(oconn == C_WF_REPORT_PARAMS);
				drbd_force_state(mdev, NS(conn, C_DISCONNECTING));
				return FALSE;
			}
		}
	}

	spin_lock_irq(&mdev->req_lock);
	if (mdev->state.conn != oconn)
		goto retry;
	clear_bit(CONSIDER_RESYNC, &mdev->flags);
	ns.i = mdev->state.i;
	ns.conn = nconn;
	ns.peer = peer_state.role;
	ns.pdsk = real_peer_disk;
	ns.peer_isp = (peer_state.aftr_isp | peer_state.user_isp);
	if ((nconn == C_CONNECTED || nconn == C_WF_BITMAP_S) && ns.disk == D_NEGOTIATING)
		ns.disk = mdev->new_state_tmp.disk;
	DRBD_STATE_DEBUG_INIT_VAL(ns);
	rv = _drbd_set_state(mdev, ns, CS_VERBOSE | CS_HARD, NULL);
	ns = mdev->state;
	spin_unlock_irq(&mdev->req_lock);

	if (rv < SS_SUCCESS) {
		drbd_force_state(mdev, NS(conn, C_DISCONNECTING));
		return FALSE;
	}

	if (oconn > C_WF_REPORT_PARAMS) {
		if (nconn > C_CONNECTED && peer_state.conn <= C_CONNECTED &&
		    peer_state.disk != D_NEGOTIATING ) {
			/* we want resync, peer has not yet decided to sync... */
			/* Nowadays only used when forcing a node into primary role and
			   setting its disk to UpToDate with that */
			drbd_send_uuids(mdev);
			drbd_send_state(mdev);
		}
	}

	mdev->net_conf->want_lose = 0;

	drbd_md_sync(mdev); /* update connected indicator, la_size, ... */

	return TRUE;
}

STATIC int receive_sync_uuid(struct drbd_conf *mdev, struct p_header *h)
{
	struct p_rs_uuid *p = (struct p_rs_uuid *)h;

	wait_event(mdev->misc_wait,
		   mdev->state.conn == C_WF_SYNC_UUID ||
		   mdev->state.conn < C_CONNECTED ||
		   mdev->state.disk < D_NEGOTIATING);

	/* D_ASSERT( mdev->state.conn == C_WF_SYNC_UUID ); */

	ERR_IF(h->length != (sizeof(*p)-sizeof(*h))) return FALSE;
	if (drbd_recv(mdev, h->payload, h->length) != h->length)
		return FALSE;

	/* Here the _drbd_uuid_ functions are right, current should
	   _not_ be rotated into the history */
	if (get_ldev_if_state(mdev, D_NEGOTIATING)) {
		_drbd_uuid_set(mdev, UI_CURRENT, be64_to_cpu(p->uuid));
		_drbd_uuid_set(mdev, UI_BITMAP, 0UL);

		drbd_start_resync(mdev, C_SYNC_TARGET);

		put_ldev(mdev);
	} else
		dev_err(DEV, "Ignoring SyncUUID packet!\n");

	return TRUE;
}

enum receive_bitmap_ret { OK, DONE, FAILED };

static enum receive_bitmap_ret
receive_bitmap_plain(struct drbd_conf *mdev, struct p_header *h,
	unsigned long *buffer, struct bm_xfer_ctx *c)
{
	unsigned num_words = min_t(size_t, BM_PACKET_WORDS, c->bm_words - c->word_offset);
	unsigned want = num_words * sizeof(long);

	if (want != h->length) {
		dev_err(DEV, "%s:want (%u) != h->length (%u)\n", __func__, want, h->length);
		return FAILED;
	}
	if (want == 0)
		return DONE;
	if (drbd_recv(mdev, buffer, want) != want)
		return FAILED;

	drbd_bm_merge_lel(mdev, c->word_offset, num_words, buffer);

	c->word_offset += num_words;
	c->bit_offset = c->word_offset * BITS_PER_LONG;
	if (c->bit_offset > c->bm_bits)
		c->bit_offset = c->bm_bits;

	return OK;
}

static enum receive_bitmap_ret
recv_bm_rle_bits(struct drbd_conf *mdev,
		struct p_compressed_bm *p,
		struct bm_xfer_ctx *c)
{
	struct bitstream bs;
	u64 look_ahead;
	u64 rl;
	u64 tmp;
	unsigned long s = c->bit_offset;
	unsigned long e;
	int len = p->head.length - (sizeof(*p) - sizeof(p->head));
	int toggle = DCBP_get_start(p);
	int have;
	int bits;

	bitstream_init(&bs, p->code, len, DCBP_get_pad_bits(p));

	bits = bitstream_get_bits(&bs, &look_ahead, 64);
	if (bits < 0)
		return FAILED;

	for (have = bits; have > 0; s += rl, toggle = !toggle) {
		bits = vli_decode_bits(&rl, look_ahead);
		if (bits <= 0)
			return FAILED;

		if (toggle) {
			e = s + rl -1;
			if (e >= c->bm_bits) {
				dev_err(DEV, "bitmap overflow (e:%lu) while decoding bm RLE packet\n", e);
				return FAILED;
			}
			_drbd_bm_set_bits(mdev, s, e);
		}

		if (have < bits) {
			dev_err(DEV, "bitmap decoding error: h:%d b:%d la:0x%08llx l:%u/%u\n",
				have, bits, look_ahead,
				(unsigned int)(bs.cur.b - p->code),
				(unsigned int)bs.buf_len);
			return FAILED;
		}
		look_ahead >>= bits;
		have -= bits;

		bits = bitstream_get_bits(&bs, &tmp, 64 - have);
		if (bits < 0)
			return FAILED;
		look_ahead |= tmp << have;
		have += bits;
	}

	c->bit_offset = s;
	bm_xfer_ctx_bit_to_word_offset(c);

	return (s == c->bm_bits) ? DONE : OK;
}

static enum receive_bitmap_ret
decode_bitmap_c(struct drbd_conf *mdev,
		struct p_compressed_bm *p,
		struct bm_xfer_ctx *c)
{
	if (DCBP_get_code(p) == RLE_VLI_Bits)
		return recv_bm_rle_bits(mdev, p, c);

	/* other variants had been implemented for evaluation,
	 * but have been dropped as this one turned out to be "best"
	 * during all our tests. */

	dev_err(DEV, "receive_bitmap_c: unknown encoding %u\n", p->encoding);
	drbd_force_state(mdev, NS(conn, C_PROTOCOL_ERROR));
	return FAILED;
}

void INFO_bm_xfer_stats(struct drbd_conf *mdev,
		const char *direction, struct bm_xfer_ctx *c)
{
	/* what would it take to transfer it "plaintext" */
	unsigned plain = sizeof(struct p_header) *
		((c->bm_words+BM_PACKET_WORDS-1)/BM_PACKET_WORDS+1)
		+ c->bm_words * sizeof(long);
	unsigned total = c->bytes[0] + c->bytes[1];
	unsigned r;

	/* total can not be zero. but just in case: */
	if (total == 0)
		return;

	/* don't report if not compressed */
	if (total >= plain)
		return;

	/* total < plain. check for overflow, still */
	r = (total > UINT_MAX/1000) ? (total / (plain/1000))
		                    : (1000 * total / plain);

	if (r > 1000)
		r = 1000;

	r = 1000 - r;
	dev_info(DEV, "%s bitmap stats [Bytes(packets)]: plain %u(%u), RLE %u(%u), "
	     "total %u; compression: %u.%u%%\n",
			direction,
			c->bytes[1], c->packets[1],
			c->bytes[0], c->packets[0],
			total, r/10, r % 10);
}

/* Since we are processing the bitfield from lower addresses to higher,
   it does not matter if the process it in 32 bit chunks or 64 bit
   chunks as long as it is little endian. (Understand it as byte stream,
   beginning with the lowest byte...) If we would use big endian
   we would need to process it from the highest address to the lowest,
   in order to be agnostic to the 32 vs 64 bits issue.

   returns 0 on failure, 1 if we successfully received it. */
STATIC int receive_bitmap(struct drbd_conf *mdev, struct p_header *h)
{
	struct bm_xfer_ctx c;
	void *buffer;
	enum receive_bitmap_ret ret;
	int ok = FALSE;

	wait_event(mdev->misc_wait, !atomic_read(&mdev->ap_bio_cnt));

	drbd_bm_lock(mdev, "receive bitmap");

	/* maybe we should use some per thread scratch page,
	 * and allocate that during initial device creation? */
	buffer	 = (unsigned long *) __get_free_page(GFP_NOIO);
	if (!buffer) {
		dev_err(DEV, "failed to allocate one page buffer in %s\n", __func__);
		goto out;
	}

	c = (struct bm_xfer_ctx) {
		.bm_bits = drbd_bm_bits(mdev),
		.bm_words = drbd_bm_words(mdev),
	};

	do {
		if (h->command == P_BITMAP) {
			ret = receive_bitmap_plain(mdev, h, buffer, &c);
		} else if (h->command == P_COMPRESSED_BITMAP) {
			/* MAYBE: sanity check that we speak proto >= 90,
			 * and the feature is enabled! */
			struct p_compressed_bm *p;

			if (h->length > BM_PACKET_PAYLOAD_BYTES) {
				dev_err(DEV, "ReportCBitmap packet too large\n");
				goto out;
			}
			/* use the page buff */
			p = buffer;
			memcpy(p, h, sizeof(*h));
			if (drbd_recv(mdev, p->head.payload, h->length) != h->length)
				goto out;
			if (p->head.length <= (sizeof(*p) - sizeof(p->head))) {
				dev_err(DEV, "ReportCBitmap packet too small (l:%u)\n", p->head.length);
				return FAILED;
			}
			ret = decode_bitmap_c(mdev, p, &c);
		} else {
			dev_warn(DEV, "receive_bitmap: h->command neither ReportBitMap nor ReportCBitMap (is 0x%x)", h->command);
			goto out;
		}

		c.packets[h->command == P_BITMAP]++;
		c.bytes[h->command == P_BITMAP] += sizeof(struct p_header) + h->length;

		if (ret != OK)
			break;

		if (!drbd_recv_header(mdev, h))
			goto out;
	} while (ret == OK);
	if (ret == FAILED)
		goto out;

	INFO_bm_xfer_stats(mdev, "receive", &c);

	if (mdev->state.conn == C_WF_BITMAP_T) {
		ok = !drbd_send_bitmap(mdev);
		if (!ok)
			goto out;
		/* Omit CS_ORDERED with this state transition to avoid deadlocks. */
		ok = _drbd_request_state(mdev, NS(conn, C_WF_SYNC_UUID), CS_VERBOSE);
		D_ASSERT(ok == SS_SUCCESS);
	} else if (mdev->state.conn != C_WF_BITMAP_S) {
		/* admin may have requested C_DISCONNECTING,
		 * other threads may have noticed network errors */
		dev_info(DEV, "unexpected cstate (%s) in receive_bitmap\n",
		    drbd_conn_str(mdev->state.conn));
	}

	ok = TRUE;
 out:
	drbd_bm_unlock(mdev);
	if (ok && mdev->state.conn == C_WF_BITMAP_S)
		drbd_start_resync(mdev, C_SYNC_SOURCE);
	free_page((unsigned long) buffer);
	return ok;
}

STATIC int receive_skip(struct drbd_conf *mdev, struct p_header *h)
{
	/* TODO zero copy sink :) */
	static char sink[128];
	int size, want, r;

	dev_warn(DEV, "skipping unknown optional packet type %d, l: %d!\n",
	     h->command, h->length);

	size = h->length;
	while (size > 0) {
		want = min_t(int, size, sizeof(sink));
		r = drbd_recv(mdev, sink, want);
		ERR_IF(r <= 0) break;
		size -= r;
	}
	return size == 0;
}

STATIC int receive_UnplugRemote(struct drbd_conf *mdev, struct p_header *h)
{
	if (mdev->state.disk >= D_INCONSISTENT)
		drbd_kick_lo(mdev);

	/* Make sure we've acked all the TCP data associated
	 * with the data requests being unplugged */
	drbd_tcp_quickack(mdev->data.socket);

	return TRUE;
}

typedef int (*drbd_cmd_handler_f)(struct drbd_conf *, struct p_header *);

static drbd_cmd_handler_f drbd_default_handler[] = {
	[P_DATA]	    = receive_Data,
	[P_DATA_REPLY]	    = receive_DataReply,
	[P_RS_DATA_REPLY]   = receive_RSDataReply,
	[P_BARRIER]	    = receive_Barrier,
	[P_BITMAP]	    = receive_bitmap,
	[P_COMPRESSED_BITMAP]    = receive_bitmap,
	[P_UNPLUG_REMOTE]   = receive_UnplugRemote,
	[P_DATA_REQUEST]    = receive_DataRequest,
	[P_RS_DATA_REQUEST] = receive_DataRequest,
	[P_SYNC_PARAM]	    = receive_SyncParam,
	[P_SYNC_PARAM89]	   = receive_SyncParam,
	[P_PROTOCOL]        = receive_protocol,
	[P_UUIDS]	    = receive_uuids,
	[P_SIZES]	    = receive_sizes,
	[P_STATE]	    = receive_state,
	[P_STATE_CHG_REQ]   = receive_req_state,
	[P_SYNC_UUID]       = receive_sync_uuid,
	[P_OV_REQUEST]      = receive_DataRequest,
	[P_OV_REPLY]        = receive_DataRequest,
	[P_CSUM_RS_REQUEST]    = receive_DataRequest,
	/* anything missing from this table is in
	 * the asender_tbl, see get_asender_cmd */
	[P_MAX_CMD]	    = NULL,
};

static drbd_cmd_handler_f *drbd_cmd_handler = drbd_default_handler;
static drbd_cmd_handler_f *drbd_opt_cmd_handler;

STATIC void drbdd(struct drbd_conf *mdev)
{
	drbd_cmd_handler_f handler;
	struct p_header *header = &mdev->data.rbuf.header;

	while (get_t_state(&mdev->receiver) == Running) {
		drbd_thread_current_set_cpu(mdev);
		if (!drbd_recv_header(mdev, header)) {
			drbd_force_state(mdev, NS(conn, C_PROTOCOL_ERROR));
			break;
		}

		if (header->command < P_MAX_CMD)
			handler = drbd_cmd_handler[header->command];
		else if (P_MAY_IGNORE < header->command
		     && header->command < P_MAX_OPT_CMD)
			handler = drbd_opt_cmd_handler[header->command-P_MAY_IGNORE];
		else if (header->command > P_MAX_OPT_CMD)
			handler = receive_skip;
		else
			handler = NULL;

		if (unlikely(!handler)) {
			dev_err(DEV, "unknown packet type %d, l: %d!\n",
			    header->command, header->length);
			drbd_force_state(mdev, NS(conn, C_PROTOCOL_ERROR));
			break;
		}
		if (unlikely(!handler(mdev, header))) {
			dev_err(DEV, "error receiving %s, l: %d!\n",
			    cmdname(header->command), header->length);
			drbd_force_state(mdev, NS(conn, C_PROTOCOL_ERROR));
			break;
		}

		trace_drbd_packet(mdev, mdev->data.socket, 2, &mdev->data.rbuf,
				__FILE__, __LINE__);
	}
}

STATIC void drbd_fail_pending_reads(struct drbd_conf *mdev)
{
	struct hlist_head *slot;
	struct hlist_node *pos;
	struct hlist_node *tmp;
	struct drbd_request *req;
	int i;

	/*
	 * Application READ requests
	 */
	spin_lock_irq(&mdev->req_lock);
	for (i = 0; i < APP_R_HSIZE; i++) {
		slot = mdev->app_reads_hash+i;
		hlist_for_each_entry_safe(req, pos, tmp, slot, colision) {
			/* it may (but should not any longer!)
			 * be on the work queue; if that assert triggers,
			 * we need to also grab the
			 * spin_lock_irq(&mdev->data.work.q_lock);
			 * and list_del_init here. */
			D_ASSERT(list_empty(&req->w.list));
			/* It would be nice to complete outside of spinlock.
			 * But this is easier for now. */
			_req_mod(req, connection_lost_while_pending);
		}
	}
	for (i = 0; i < APP_R_HSIZE; i++)
		if (!hlist_empty(mdev->app_reads_hash+i))
			dev_warn(DEV, "ASSERT FAILED: app_reads_hash[%d].first: "
				"%p, should be NULL\n", i, mdev->app_reads_hash[i].first);

	memset(mdev->app_reads_hash, 0, APP_R_HSIZE*sizeof(void *));
	spin_unlock_irq(&mdev->req_lock);
}

void drbd_flush_workqueue(struct drbd_conf *mdev)
{
	struct drbd_wq_barrier barr;

	barr.w.cb = w_prev_work_done;
	init_completion(&barr.done);
	drbd_queue_work(&mdev->data.work, &barr.w);
	wait_for_completion(&barr.done);
}

STATIC void drbd_disconnect(struct drbd_conf *mdev)
{
	enum drbd_fencing_p fp;
	union drbd_state os, ns;
	int rv = SS_UNKNOWN_ERROR;
	unsigned int i;

	if (mdev->state.conn == C_STANDALONE)
		return;
	if (mdev->state.conn >= C_WF_CONNECTION)
		dev_err(DEV, "ASSERT FAILED cstate = %s, expected < WFConnection\n",
				drbd_conn_str(mdev->state.conn));

	/* asender does not clean up anything. it must not interfere, either */
	drbd_thread_stop(&mdev->asender);
	drbd_free_sock(mdev);

	spin_lock_irq(&mdev->req_lock);
	_drbd_wait_ee_list_empty(mdev, &mdev->active_ee);
	_drbd_wait_ee_list_empty(mdev, &mdev->sync_ee);
	_drbd_wait_ee_list_empty(mdev, &mdev->read_ee);
	spin_unlock_irq(&mdev->req_lock);

	/* We do not have data structures that would allow us to
	 * get the rs_pending_cnt down to 0 again.
	 *  * On C_SYNC_TARGET we do not have any data structures describing
	 *    the pending RSDataRequest's we have sent.
	 *  * On C_SYNC_SOURCE there is no data structure that tracks
	 *    the P_RS_DATA_REPLY blocks that we sent to the SyncTarget.
	 *  And no, it is not the sum of the reference counts in the
	 *  resync_LRU. The resync_LRU tracks the whole operation including
	 *  the disk-IO, while the rs_pending_cnt only tracks the blocks
	 *  on the fly. */
	drbd_rs_cancel_all(mdev);
	mdev->rs_total = 0;
	mdev->rs_failed = 0;
	atomic_set(&mdev->rs_pending_cnt, 0);
	wake_up(&mdev->misc_wait);

	/* make sure syncer is stopped and w_resume_next_sg queued */
	del_timer_sync(&mdev->resync_timer);
	set_bit(STOP_SYNC_TIMER, &mdev->flags);
	resync_timer_fn((unsigned long)mdev);

	/* wait for all w_e_end_data_req, w_e_end_rsdata_req, w_send_barrier,
	 * w_make_resync_request etc. which may still be on the worker queue
	 * to be "canceled" */
	drbd_flush_workqueue(mdev);

	/* This also does reclaim_net_ee().  If we do this too early, we might
	 * miss some resync ee and pages.*/
	drbd_process_done_ee(mdev);

	kfree(mdev->p_uuid);
	mdev->p_uuid = NULL;

	if (!mdev->state.susp)
		tl_clear(mdev);

	drbd_fail_pending_reads(mdev);

	dev_info(DEV, "Connection closed\n");

	drbd_md_sync(mdev);

	fp = FP_DONT_CARE;
	if (get_ldev(mdev)) {
		fp = mdev->ldev->dc.fencing;
		put_ldev(mdev);
	}

	if (mdev->state.role == R_PRIMARY) {
		if (fp >= FP_RESOURCE && mdev->state.pdsk >= D_UNKNOWN) {
			enum drbd_disk_state nps = drbd_try_outdate_peer(mdev);
			drbd_request_state(mdev, NS(pdsk, nps));
		}
	}

	spin_lock_irq(&mdev->req_lock);
	os = mdev->state;
	if (os.conn >= C_UNCONNECTED) {
		/* Do not restart in case we are C_DISCONNECTING */
		ns = os;
		ns.conn = C_UNCONNECTED;
		DRBD_STATE_DEBUG_INIT_VAL(ns);
		rv = _drbd_set_state(mdev, ns, CS_VERBOSE, NULL);
	}
	spin_unlock_irq(&mdev->req_lock);

	if (os.conn == C_DISCONNECTING) {
		struct hlist_head *h;
		wait_event(mdev->misc_wait, atomic_read(&mdev->net_cnt) == 0);

		/* we must not free the tl_hash
		 * while application io is still on the fly */
		wait_event(mdev->misc_wait, atomic_read(&mdev->ap_bio_cnt) == 0);

		spin_lock_irq(&mdev->req_lock);
		/* paranoia code */
		for (h = mdev->ee_hash; h < mdev->ee_hash + mdev->ee_hash_s; h++)
			if (h->first)
				dev_err(DEV, "ASSERT FAILED ee_hash[%u].first == %p, expected NULL\n",
						(int)(h - mdev->ee_hash), h->first);
		kfree(mdev->ee_hash);
		mdev->ee_hash = NULL;
		mdev->ee_hash_s = 0;

		/* paranoia code */
		for (h = mdev->tl_hash; h < mdev->tl_hash + mdev->tl_hash_s; h++)
			if (h->first)
				dev_err(DEV, "ASSERT FAILED tl_hash[%u] == %p, expected NULL\n",
						(int)(h - mdev->tl_hash), h->first);
		kfree(mdev->tl_hash);
		mdev->tl_hash = NULL;
		mdev->tl_hash_s = 0;
		spin_unlock_irq(&mdev->req_lock);

		crypto_free_hash(mdev->cram_hmac_tfm);
		mdev->cram_hmac_tfm = NULL;

		kfree(mdev->net_conf);
		mdev->net_conf = NULL;
		drbd_request_state(mdev, NS(conn, C_STANDALONE));
	}

	/* tcp_close and release of sendpage pages can be deferred.  I don't
	 * want to use SO_LINGER, because apparently it can be deferred for
	 * more than 20 seconds (longest time I checked).
	 *
	 * Actually we don't care for exactly when the network stack does its
	 * put_page(), but release our reference on these pages right here.
	 */
	i = drbd_release_ee(mdev, &mdev->net_ee);
	if (i)
		dev_info(DEV, "net_ee not empty, killed %u entries\n", i);
	i = atomic_read(&mdev->pp_in_use);
	if (i)
		dev_info(DEV, "pp_in_use = %u, expected 0\n", i);

	D_ASSERT(list_empty(&mdev->read_ee));
	D_ASSERT(list_empty(&mdev->active_ee));
	D_ASSERT(list_empty(&mdev->sync_ee));
	D_ASSERT(list_empty(&mdev->done_ee));

	/* ok, no more ee's on the fly, it is safe to reset the epoch_size */
	atomic_set(&mdev->current_epoch->epoch_size, 0);
	D_ASSERT(list_empty(&mdev->current_epoch->list));
}

/*
 * We support PRO_VERSION_MIN to PRO_VERSION_MAX. The protocol version
 * we can agree on is stored in agreed_pro_version.
 *
 * feature flags and the reserved array should be enough room for future
 * enhancements of the handshake protocol, and possible plugins...
 *
 * for now, they are expected to be zero, but ignored.
 */
STATIC int drbd_send_handshake(struct drbd_conf *mdev)
{
	/* ASSERT current == mdev->receiver ... */
	struct p_handshake *p = &mdev->data.sbuf.handshake;
	int ok;

	if (mutex_lock_interruptible(&mdev->data.mutex)) {
		dev_err(DEV, "interrupted during initial handshake\n");
		return 0; /* interrupted. not ok. */
	}

	if (mdev->data.socket == NULL) {
		mutex_unlock(&mdev->data.mutex);
		return 0;
	}

	memset(p, 0, sizeof(*p));
	p->protocol_min = cpu_to_be32(PRO_VERSION_MIN);
	p->protocol_max = cpu_to_be32(PRO_VERSION_MAX);
	ok = _drbd_send_cmd( mdev, mdev->data.socket, P_HAND_SHAKE,
			     (struct p_header *)p, sizeof(*p), 0 );
	mutex_unlock(&mdev->data.mutex);
	return ok;
}

/*
 * return values:
 *   1 yes, we have a valid connection
 *   0 oops, did not work out, please try again
 *  -1 peer talks different language,
 *     no point in trying again, please go standalone.
 */
STATIC int drbd_do_handshake(struct drbd_conf *mdev)
{
	/* ASSERT current == mdev->receiver ... */
	struct p_handshake *p = &mdev->data.rbuf.handshake;
	const int expect = sizeof(struct p_handshake)
			  -sizeof(struct p_header);
	int rv;

	rv = drbd_send_handshake(mdev);
	if (!rv)
		return 0;

	rv = drbd_recv_header(mdev, &p->head);
	if (!rv)
		return 0;

	if (p->head.command != P_HAND_SHAKE) {
		dev_err(DEV, "expected HandShake packet, received: %s (0x%04x)\n",
		     cmdname(p->head.command), p->head.command);
		return -1;
	}

	if (p->head.length != expect) {
		dev_err(DEV, "expected HandShake length: %u, received: %u\n",
		     expect, p->head.length);
		return -1;
	}

	rv = drbd_recv(mdev, &p->head.payload, expect);

	if (rv != expect) {
		dev_err(DEV, "short read receiving handshake packet: l=%u\n", rv);
		return 0;
	}

	trace_drbd_packet(mdev, mdev->data.socket, 2, &mdev->data.rbuf,
			__FILE__, __LINE__);

	p->protocol_min = be32_to_cpu(p->protocol_min);
	p->protocol_max = be32_to_cpu(p->protocol_max);
	if (p->protocol_max == 0)
		p->protocol_max = p->protocol_min;

	if (PRO_VERSION_MAX < p->protocol_min ||
	    PRO_VERSION_MIN > p->protocol_max)
		goto incompat;

	mdev->agreed_pro_version = min_t(int, PRO_VERSION_MAX, p->protocol_max);

	dev_info(DEV, "Handshake successful: "
	     "Agreed network protocol version %d\n", mdev->agreed_pro_version);

	return 1;

 incompat:
	dev_err(DEV, "incompatible DRBD dialects: "
	    "I support %d-%d, peer supports %d-%d\n",
	    PRO_VERSION_MIN, PRO_VERSION_MAX,
	    p->protocol_min, p->protocol_max);
	return -1;
}

#if !defined(CONFIG_CRYPTO_HMAC) && !defined(CONFIG_CRYPTO_HMAC_MODULE)
STATIC int drbd_do_auth(struct drbd_conf *mdev)
{
	dev_err(DEV, "This kernel was build without CONFIG_CRYPTO_HMAC.\n");
	dev_err(DEV, "You need to disable 'cram-hmac-alg' in drbd.conf.\n");
	return -1;
}
#else
#define CHALLENGE_LEN 64

/* Return value:
	1 - auth succeeded,
	0 - failed, try again (network error),
	-1 - auth failed, don't try again.
*/

STATIC int drbd_do_auth(struct drbd_conf *mdev)
{
	char my_challenge[CHALLENGE_LEN];  /* 64 Bytes... */
	struct scatterlist sg;
	char *response = NULL;
	char *right_response = NULL;
	char *peers_ch = NULL;
	struct p_header p;
	unsigned int key_len = strlen(mdev->net_conf->shared_secret);
	unsigned int resp_size;
	struct hash_desc desc;
	int rv;

	desc.tfm = mdev->cram_hmac_tfm;
	desc.flags = 0;

	rv = crypto_hash_setkey(mdev->cram_hmac_tfm,
				(u8 *)mdev->net_conf->shared_secret, key_len);
	if (rv) {
		dev_err(DEV, "crypto_hash_setkey() failed with %d\n", rv);
		rv = -1;
		goto fail;
	}

	get_random_bytes(my_challenge, CHALLENGE_LEN);

	rv = drbd_send_cmd2(mdev, P_AUTH_CHALLENGE, my_challenge, CHALLENGE_LEN);
	if (!rv)
		goto fail;

	rv = drbd_recv_header(mdev, &p);
	if (!rv)
		goto fail;

	if (p.command != P_AUTH_CHALLENGE) {
		dev_err(DEV, "expected AuthChallenge packet, received: %s (0x%04x)\n",
		    cmdname(p.command), p.command);
		rv = 0;
		goto fail;
	}

	if (p.length > CHALLENGE_LEN*2) {
		dev_err(DEV, "expected AuthChallenge payload too big.\n");
		rv = -1;
		goto fail;
	}

	peers_ch = kmalloc(p.length, GFP_NOIO);
	if (peers_ch == NULL) {
		dev_err(DEV, "kmalloc of peers_ch failed\n");
		rv = -1;
		goto fail;
	}

	rv = drbd_recv(mdev, peers_ch, p.length);

	if (rv != p.length) {
		dev_err(DEV, "short read AuthChallenge: l=%u\n", rv);
		rv = 0;
		goto fail;
	}

	resp_size = crypto_hash_digestsize(mdev->cram_hmac_tfm);
	response = kmalloc(resp_size, GFP_NOIO);
	if (response == NULL) {
		dev_err(DEV, "kmalloc of response failed\n");
		rv = -1;
		goto fail;
	}

	sg_init_table(&sg, 1);
	sg_set_buf(&sg, peers_ch, p.length);

	rv = crypto_hash_digest(&desc, &sg, sg.length, response);
	if (rv) {
		dev_err(DEV, "crypto_hash_digest() failed with %d\n", rv);
		rv = -1;
		goto fail;
	}

	rv = drbd_send_cmd2(mdev, P_AUTH_RESPONSE, response, resp_size);
	if (!rv)
		goto fail;

	rv = drbd_recv_header(mdev, &p);
	if (!rv)
		goto fail;

	if (p.command != P_AUTH_RESPONSE) {
		dev_err(DEV, "expected AuthResponse packet, received: %s (0x%04x)\n",
		    cmdname(p.command), p.command);
		rv = 0;
		goto fail;
	}

	if (p.length != resp_size) {
		dev_err(DEV, "expected AuthResponse payload of wrong size\n");
		rv = 0;
		goto fail;
	}

	rv = drbd_recv(mdev, response , resp_size);

	if (rv != resp_size) {
		dev_err(DEV, "short read receiving AuthResponse: l=%u\n", rv);
		rv = 0;
		goto fail;
	}

	right_response = kmalloc(resp_size, GFP_NOIO);
	if (right_response == NULL) {
		dev_err(DEV, "kmalloc of right_response failed\n");
		rv = -1;
		goto fail;
	}

	sg_set_buf(&sg, my_challenge, CHALLENGE_LEN);

	rv = crypto_hash_digest(&desc, &sg, sg.length, right_response);
	if (rv) {
		dev_err(DEV, "crypto_hash_digest() failed with %d\n", rv);
		rv = -1;
		goto fail;
	}

	rv = !memcmp(response, right_response, resp_size);

	if (rv)
		dev_info(DEV, "Peer authenticated using %d bytes of '%s' HMAC\n",
		     resp_size, mdev->net_conf->cram_hmac_alg);
	else
		rv = -1;

 fail:
	kfree(peers_ch);
	kfree(response);
	kfree(right_response);

	return rv;
}
#endif

int drbdd_init(struct drbd_thread *thi)
{
	struct drbd_conf *mdev = thi->mdev;
	unsigned int minor = mdev_to_minor(mdev);
	int h;

	sprintf(current->comm, "drbd%d_receiver", minor);

	dev_info(DEV, "receiver (re)started\n");

	do {
		h = drbd_connect(mdev);
		if (h == 0) {
			drbd_disconnect(mdev);
			__set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ);
		}
		if (h == -1) {
			dev_warn(DEV, "Discarding network configuration.\n");
			drbd_force_state(mdev, NS(conn, C_DISCONNECTING));
		}
	} while (h == 0);

	if (h > 0) {
		if (get_net_conf(mdev)) {
			drbdd(mdev);
			put_net_conf(mdev);
		}
	}

	drbd_disconnect(mdev);

	dev_info(DEV, "receiver terminated\n");
	return 0;
}

/* ********* acknowledge sender ******** */

STATIC int got_RqSReply(struct drbd_conf *mdev, struct p_header *h)
{
	struct p_req_state_reply *p = (struct p_req_state_reply *)h;

	int retcode = be32_to_cpu(p->retcode);

	if (retcode >= SS_SUCCESS) {
		set_bit(CL_ST_CHG_SUCCESS, &mdev->flags);
	} else {
		set_bit(CL_ST_CHG_FAIL, &mdev->flags);
		dev_err(DEV, "Requested state change failed by peer: %s (%d)\n",
		    drbd_set_st_err_str(retcode), retcode);
	}
	wake_up(&mdev->state_wait);

	return TRUE;
}

STATIC int got_Ping(struct drbd_conf *mdev, struct p_header *h)
{
	return drbd_send_ping_ack(mdev);

}

STATIC int got_PingAck(struct drbd_conf *mdev, struct p_header *h)
{
	/* restore idle timeout */
	mdev->meta.socket->sk->sk_rcvtimeo = mdev->net_conf->ping_int*HZ;
	if (!test_and_set_bit(GOT_PING_ACK, &mdev->flags))
		wake_up(&mdev->misc_wait);

	return TRUE;
}

STATIC int got_IsInSync(struct drbd_conf *mdev, struct p_header *h)
{
	struct p_block_ack *p = (struct p_block_ack *)h;
	sector_t sector = be64_to_cpu(p->sector);
	int blksize = be32_to_cpu(p->blksize);

	D_ASSERT(mdev->agreed_pro_version >= 89);

	update_peer_seq(mdev, be32_to_cpu(p->seq_num));

	drbd_rs_complete_io(mdev, sector);
	drbd_set_in_sync(mdev, sector, blksize);
	/* rs_same_csums is supposed to count in units of BM_BLOCK_SIZE */
	mdev->rs_same_csum += (blksize >> BM_BLOCK_SHIFT);
	dec_rs_pending(mdev);

	return TRUE;
}

/* when we receive the ACK for a write request,
 * verify that we actually know about it */
static struct drbd_request *_ack_id_to_req(struct drbd_conf *mdev,
	u64 id, sector_t sector)
{
	struct hlist_head *slot = tl_hash_slot(mdev, sector);
	struct hlist_node *n;
	struct drbd_request *req;

	hlist_for_each_entry(req, n, slot, colision) {
		if ((unsigned long)req == (unsigned long)id) {
			if (req->sector != sector) {
				dev_err(DEV, "_ack_id_to_req: found req %p but it has "
				    "wrong sector (%llus versus %llus)\n", req,
				    (unsigned long long)req->sector,
				    (unsigned long long)sector);
				break;
			}
			return req;
		}
	}
	dev_err(DEV, "_ack_id_to_req: failed to find req %p, sector %llus in list\n",
		(void *)(unsigned long)id, (unsigned long long)sector);
	return NULL;
}

typedef struct drbd_request *(req_validator_fn)
	(struct drbd_conf *mdev, u64 id, sector_t sector);

static int validate_req_change_req_state(struct drbd_conf *mdev,
	u64 id, sector_t sector, req_validator_fn validator,
	const char *func, enum drbd_req_event what)
{
	struct drbd_request *req;
	struct bio_and_error m;

	spin_lock_irq(&mdev->req_lock);
	req = validator(mdev, id, sector);
	if (unlikely(!req)) {
		spin_unlock_irq(&mdev->req_lock);
		dev_err(DEV, "%s: got a corrupt block_id/sector pair\n", func);
		return FALSE;
	}
	__req_mod(req, what, &m);
	spin_unlock_irq(&mdev->req_lock);

	if (m.bio)
		complete_master_bio(mdev, &m);
	return TRUE;
}

STATIC int got_BlockAck(struct drbd_conf *mdev, struct p_header *h)
{
	struct p_block_ack *p = (struct p_block_ack *)h;
	sector_t sector = be64_to_cpu(p->sector);
	int blksize = be32_to_cpu(p->blksize);
	enum drbd_req_event what;

	update_peer_seq(mdev, be32_to_cpu(p->seq_num));

	if (is_syncer_block_id(p->block_id)) {
		drbd_set_in_sync(mdev, sector, blksize);
		dec_rs_pending(mdev);
		return TRUE;
	}
	switch (be16_to_cpu(h->command)) {
	case P_RS_WRITE_ACK:
		D_ASSERT(mdev->net_conf->wire_protocol == DRBD_PROT_C);
		what = write_acked_by_peer_and_sis;
		break;
	case P_WRITE_ACK:
		D_ASSERT(mdev->net_conf->wire_protocol == DRBD_PROT_C);
		what = write_acked_by_peer;
		break;
	case P_RECV_ACK:
		D_ASSERT(mdev->net_conf->wire_protocol == DRBD_PROT_B);
		what = recv_acked_by_peer;
		break;
	case P_DISCARD_ACK:
		D_ASSERT(mdev->net_conf->wire_protocol == DRBD_PROT_C);
		what = conflict_discarded_by_peer;
		break;
	default:
		D_ASSERT(0);
		return FALSE;
	}

	return validate_req_change_req_state(mdev, p->block_id, sector,
		_ack_id_to_req, __func__ , what);
}

STATIC int got_NegAck(struct drbd_conf *mdev, struct p_header *h)
{
	struct p_block_ack *p = (struct p_block_ack *)h;
	sector_t sector = be64_to_cpu(p->sector);

	if (DRBD_ratelimit(5*HZ, 5))
		dev_warn(DEV, "Got NegAck packet. Peer is in troubles?\n");

	update_peer_seq(mdev, be32_to_cpu(p->seq_num));

	if (is_syncer_block_id(p->block_id)) {
		int size = be32_to_cpu(p->blksize);
		dec_rs_pending(mdev);
		drbd_rs_failed_io(mdev, sector, size);
		return TRUE;
	}
	return validate_req_change_req_state(mdev, p->block_id, sector,
		_ack_id_to_req, __func__ , neg_acked);
}

STATIC int got_NegDReply(struct drbd_conf *mdev, struct p_header *h)
{
	struct p_block_ack *p = (struct p_block_ack *)h;
	sector_t sector = be64_to_cpu(p->sector);

	update_peer_seq(mdev, be32_to_cpu(p->seq_num));
	dev_err(DEV, "Got NegDReply; Sector %llus, len %u; Fail original request.\n",
	    (unsigned long long)sector, be32_to_cpu(p->blksize));

	return validate_req_change_req_state(mdev, p->block_id, sector,
		_ar_id_to_req, __func__ , neg_acked);
}

STATIC int got_NegRSDReply(struct drbd_conf *mdev, struct p_header *h)
{
	sector_t sector;
	int size;
	struct p_block_ack *p = (struct p_block_ack *)h;

	sector = be64_to_cpu(p->sector);
	size = be32_to_cpu(p->blksize);

	update_peer_seq(mdev, be32_to_cpu(p->seq_num));

	dec_rs_pending(mdev);

	if (get_ldev_if_state(mdev, D_FAILED)) {
		drbd_rs_complete_io(mdev, sector);
		drbd_rs_failed_io(mdev, sector, size);
		put_ldev(mdev);
	}

	return TRUE;
}

STATIC int got_BarrierAck(struct drbd_conf *mdev, struct p_header *h)
{
	struct p_barrier_ack *p = (struct p_barrier_ack *)h;

	tl_release(mdev, p->barrier, be32_to_cpu(p->set_size));

	return TRUE;
}

STATIC int got_OVResult(struct drbd_conf *mdev, struct p_header *h)
{
	struct p_block_ack *p = (struct p_block_ack *)h;
	struct drbd_work *w;
	sector_t sector;
	int size;

	sector = be64_to_cpu(p->sector);
	size = be32_to_cpu(p->blksize);

	update_peer_seq(mdev, be32_to_cpu(p->seq_num));

	if (be64_to_cpu(p->block_id) == ID_OUT_OF_SYNC)
		drbd_ov_oos_found(mdev, sector, size);
	else
		ov_oos_print(mdev);

	drbd_rs_complete_io(mdev, sector);
	dec_rs_pending(mdev);

	if (--mdev->ov_left == 0) {
		w = kmalloc(sizeof(*w), GFP_NOIO);
		if (w) {
			w->cb = w_ov_finished;
			drbd_queue_work_front(&mdev->data.work, w);
		} else {
			dev_err(DEV, "kmalloc(w) failed.");
			ov_oos_print(mdev);
			drbd_resync_finished(mdev);
		}
	}
	return TRUE;
}

struct asender_cmd {
	size_t pkt_size;
	int (*process)(struct drbd_conf *mdev, struct p_header *h);
};

static struct asender_cmd *get_asender_cmd(int cmd)
{
	static struct asender_cmd asender_tbl[] = {
		/* anything missing from this table is in
		 * the drbd_cmd_handler (drbd_default_handler) table,
		 * see the beginning of drbdd() */
	[P_PING]	    = { sizeof(struct p_header), got_Ping },
	[P_PING_ACK]	    = { sizeof(struct p_header), got_PingAck },
	[P_RECV_ACK]	    = { sizeof(struct p_block_ack), got_BlockAck },
	[P_WRITE_ACK]	    = { sizeof(struct p_block_ack), got_BlockAck },
	[P_RS_WRITE_ACK]    = { sizeof(struct p_block_ack), got_BlockAck },
	[P_DISCARD_ACK]	    = { sizeof(struct p_block_ack), got_BlockAck },
	[P_NEG_ACK]	    = { sizeof(struct p_block_ack), got_NegAck },
	[P_NEG_DREPLY]	    = { sizeof(struct p_block_ack), got_NegDReply },
	[P_NEG_RS_DREPLY]   = { sizeof(struct p_block_ack), got_NegRSDReply},
	[P_OV_RESULT]	    = { sizeof(struct p_block_ack), got_OVResult },
	[P_BARRIER_ACK]	    = { sizeof(struct p_barrier_ack), got_BarrierAck },
	[P_STATE_CHG_REPLY] = { sizeof(struct p_req_state_reply), got_RqSReply },
	[P_RS_IS_IN_SYNC]   = { sizeof(struct p_block_ack), got_IsInSync },
	[P_MAX_CMD]	    = { 0, NULL },
	};
	if (cmd > P_MAX_CMD || asender_tbl[cmd].process == NULL)
		return NULL;
	return &asender_tbl[cmd];
}

int drbd_asender(struct drbd_thread *thi)
{
	struct drbd_conf *mdev = thi->mdev;
	struct p_header *h = &mdev->meta.rbuf.header;
	struct asender_cmd *cmd = NULL;

	int rv, len;
	void *buf    = h;
	int received = 0;
	int expect   = sizeof(struct p_header);
	int empty;

	sprintf(current->comm, "drbd%d_asender", mdev_to_minor(mdev));

	current->policy = SCHED_RR;  /* Make this a realtime task! */
	current->rt_priority = 2;    /* more important than all other tasks */

	while (get_t_state(thi) == Running) {
		drbd_thread_current_set_cpu(mdev);
		if (test_and_clear_bit(SEND_PING, &mdev->flags)) {
			ERR_IF(!drbd_send_ping(mdev)) goto reconnect;
			mdev->meta.socket->sk->sk_rcvtimeo =
				mdev->net_conf->ping_timeo*HZ/10;
		}

		/* conditionally cork;
		 * it may hurt latency if we cork without much to send */
		if (!mdev->net_conf->no_cork &&
			3 < atomic_read(&mdev->unacked_cnt))
			drbd_tcp_cork(mdev->meta.socket);
		while (1) {
			clear_bit(SIGNAL_ASENDER, &mdev->flags);
			flush_signals(current);
			if (!drbd_process_done_ee(mdev)) {
				dev_err(DEV, "process_done_ee() = NOT_OK\n");
				goto reconnect;
			}
			/* to avoid race with newly queued ACKs */
			set_bit(SIGNAL_ASENDER, &mdev->flags);
			spin_lock_irq(&mdev->req_lock);
			empty = list_empty(&mdev->done_ee);
			spin_unlock_irq(&mdev->req_lock);
			/* new ack may have been queued right here,
			 * but then there is also a signal pending,
			 * and we start over... */
			if (empty)
				break;
		}
		/* but unconditionally uncork unless disabled */
		if (!mdev->net_conf->no_cork)
			drbd_tcp_uncork(mdev->meta.socket);

		/* short circuit, recv_msg would return EINTR anyways. */
		if (signal_pending(current))
			continue;

		rv = drbd_recv_short(mdev, mdev->meta.socket,
				     buf, expect-received, 0);
		clear_bit(SIGNAL_ASENDER, &mdev->flags);

		flush_signals(current);

		/* Note:
		 * -EINTR	 (on meta) we got a signal
		 * -EAGAIN	 (on meta) rcvtimeo expired
		 * -ECONNRESET	 other side closed the connection
		 * -ERESTARTSYS  (on data) we got a signal
		 * rv <  0	 other than above: unexpected error!
		 * rv == expected: full header or command
		 * rv <  expected: "woken" by signal during receive
		 * rv == 0	 : "connection shut down by peer"
		 */
		if (likely(rv > 0)) {
			received += rv;
			buf	 += rv;
		} else if (rv == 0) {
			dev_err(DEV, "meta connection shut down by peer.\n");
			goto reconnect;
		} else if (rv == -EAGAIN) {
			if (mdev->meta.socket->sk->sk_rcvtimeo ==
			    mdev->net_conf->ping_timeo*HZ/10) {
				dev_err(DEV, "PingAck did not arrive in time.\n");
				goto reconnect;
			}
			set_bit(SEND_PING, &mdev->flags);
			continue;
		} else if (rv == -EINTR) {
			continue;
		} else {
			dev_err(DEV, "sock_recvmsg returned %d\n", rv);
			goto reconnect;
		}

		if (received == expect && cmd == NULL) {
			if (unlikely(h->magic != BE_DRBD_MAGIC)) {
				dev_err(DEV, "magic?? on meta m: 0x%lx c: %d l: %d\n",
				    (long)be32_to_cpu(h->magic),
				    h->command, h->length);
				goto reconnect;
			}
			cmd = get_asender_cmd(be16_to_cpu(h->command));
			len = be16_to_cpu(h->length);
			if (unlikely(cmd == NULL)) {
				dev_err(DEV, "unknown command?? on meta m: 0x%lx c: %d l: %d\n",
				    (long)be32_to_cpu(h->magic),
				    h->command, h->length);
				goto disconnect;
			}
			expect = cmd->pkt_size;
			ERR_IF(len != expect-sizeof(struct p_header)) {
				trace_drbd_packet(mdev, mdev->meta.socket, 1, (void *)h, __FILE__, __LINE__);
				DUMPI(expect);
				goto reconnect;
			}
		}
		if (received == expect) {
			D_ASSERT(cmd != NULL);
			trace_drbd_packet(mdev, mdev->meta.socket, 1, (void *)h, __FILE__, __LINE__);
			if (!cmd->process(mdev, h))
				goto reconnect;

			buf	 = h;
			received = 0;
			expect	 = sizeof(struct p_header);
			cmd	 = NULL;
		}
	}

	if (0) {
reconnect:
		drbd_force_state(mdev, NS(conn, C_NETWORK_FAILURE));
	}
	if (0) {
disconnect:
		drbd_force_state(mdev, NS(conn, C_DISCONNECTING));
	}
	clear_bit(SIGNAL_ASENDER, &mdev->flags);

	D_ASSERT(mdev->state.conn < C_CONNECTED);
	dev_info(DEV, "asender terminated\n");

	return 0;
}
