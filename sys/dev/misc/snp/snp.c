/*
 * (MPSAFE)
 *
 * Copyright (c) 1995 Ugen J.S.Antsilevich
 *
 * Redistribution and use in source forms, with and without modification,
 * are permitted provided that this entire comment appears intact.
 *
 * Redistribution in binary form may occur without any restrictions.
 * Obviously, it would be nice if you gave credit where credit is due
 * but requiring it would be too onerous.
 *
 * This software is provided ``AS IS'' without any warranties of any kind.
 *
 * Snoop stuff.
 *
 * $FreeBSD: src/sys/dev/snp/snp.c,v 1.69.2.2 2002/05/06 07:30:02 dd Exp $
 */

#include "use_snp.h"
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/filio.h>
#include <sys/malloc.h>
#include <sys/tty.h>
#include <sys/conf.h>
#include <sys/event.h>
#include <sys/kernel.h>
#include <sys/queue.h>
#include <sys/snoop.h>
#include <sys/thread2.h>
#include <sys/vnode.h>
#include <sys/device.h>
#include <sys/devfs.h>

static	l_close_t	snplclose;
static	l_write_t	snplwrite;
static	d_open_t	snpopen;
static	d_close_t	snpclose;
static	d_read_t	snpread;
static	d_write_t	snpwrite;
static	d_ioctl_t	snpioctl;
static	d_kqfilter_t	snpkqfilter;
static d_clone_t	snpclone;
DEVFS_DEFINE_CLONE_BITMAP(snp);

static void snpfilter_detach(struct knote *);
static int snpfilter_rd(struct knote *, long);
static int snpfilter_wr(struct knote *, long);

#if NSNP <= 1
#define SNP_PREALLOCATED_UNITS	4
#else
#define SNP_PREALLOCATED_UNITS	NSNP
#endif

static struct dev_ops snp_ops = {
	{ "snp", 0, 0 },
	.d_open =	snpopen,
	.d_close =	snpclose,
	.d_read =	snpread,
	.d_write =	snpwrite,
	.d_ioctl =	snpioctl,
	.d_kqfilter =	snpkqfilter
};

static struct linesw snpdisc = {
	ttyopen,	snplclose,	ttread,		snplwrite,
	l_nullioctl,	ttyinput,	ttstart,	ttymodem
};

/*
 * This is the main snoop per-device structure.
 */
struct snoop {
	LIST_ENTRY(snoop)	snp_list;	/* List glue. */
	cdev_t			snp_target;	/* Target tty device. */
	struct tty		*snp_tty;	/* Target tty pointer. */
	u_long			 snp_len;	/* Possible length. */
	u_long			 snp_base;	/* Data base. */
	u_long			 snp_blen;	/* Used length. */
	caddr_t			 snp_buf;	/* Allocation pointer. */
	int			 snp_flags;	/* Flags. */
	struct kqinfo		 snp_kq;	/* Kqueue info. */
	int			 snp_olddisc;	/* Old line discipline. */
};

/*
 * Possible flags.
 */
#define SNOOP_ASYNC		0x0002
#define SNOOP_OPEN		0x0004
#define SNOOP_RWAIT		0x0008
#define SNOOP_OFLOW		0x0010
#define SNOOP_DOWN		0x0020

/*
 * Other constants.
 */
#define SNOOP_MINLEN		(4*1024)	/* This should be power of 2.
						 * 4K tested to be the minimum
						 * for which on normal tty
						 * usage there is no need to
						 * allocate more.
						 */
#define SNOOP_MAXLEN		(64*1024)	/* This one also,64K enough
						 * If we grow more,something
						 * really bad in this world..
						 */

static MALLOC_DEFINE(M_SNP, "snp", "Snoop device data");
/*
 * The number of the "snoop" line discipline.  This gets determined at
 * module load time.
 */
static int snooplinedisc;


static LIST_HEAD(, snoop) snp_sclist = LIST_HEAD_INITIALIZER(&snp_sclist);
static struct lwkt_token  snp_token = LWKT_TOKEN_INITIALIZER(snp_token);

static struct tty	*snpdevtotty (cdev_t dev);
static int		snp_detach (struct snoop *snp);
static int		snp_down (struct snoop *snp);
static int		snp_in (struct snoop *snp, char *buf, int n);
static int		snp_modevent (module_t mod, int what, void *arg);

static int
snplclose(struct tty *tp, int flag)
{
	struct snoop *snp;
	int error;

	lwkt_gettoken(&snp_token);
	snp = tp->t_sc;
	error = snp_down(snp);
	if (error != 0) {
		lwkt_reltoken(&snp_token);
		return (error);
	}
	lwkt_gettoken(&tp->t_token);
	error = ttylclose(tp, flag);
	lwkt_reltoken(&tp->t_token);
	lwkt_reltoken(&snp_token);

	return (error);
}

static int
snplwrite(struct tty *tp, struct uio *uio, int flag)
{
	struct iovec iov;
	struct uio uio2;
	struct snoop *snp;
	int error, ilen;
	char *ibuf;

	lwkt_gettoken(&tp->t_token);
	error = 0;
	ibuf = NULL;
	snp = tp->t_sc;
	while (uio->uio_resid > 0) {
		ilen = (int)szmin(512, uio->uio_resid);
		ibuf = kmalloc(ilen, M_SNP, M_WAITOK);
		error = uiomove(ibuf, (size_t)ilen, uio);
		if (error != 0)
			break;
		snp_in(snp, ibuf, ilen);
		/* Hackish, but probably the least of all evils. */
		iov.iov_base = ibuf;
		iov.iov_len = ilen;
		uio2.uio_iov = &iov;
		uio2.uio_iovcnt = 1;
		uio2.uio_offset = 0;
		uio2.uio_resid = ilen;
		uio2.uio_segflg = UIO_SYSSPACE;
		uio2.uio_rw = UIO_WRITE;
		uio2.uio_td = uio->uio_td;
		error = ttwrite(tp, &uio2, flag);
		if (error != 0)
			break;
		kfree(ibuf, M_SNP);
		ibuf = NULL;
	}
	if (ibuf != NULL)
		kfree(ibuf, M_SNP);
	lwkt_reltoken(&tp->t_token);
	return (error);
}

static struct tty *
snpdevtotty(cdev_t dev)
{
	if ((dev_dflags(dev) & D_TTY) == 0)
		return (NULL);
	return (dev->si_tty);
}

#define SNP_INPUT_BUF	5	/* This is even too much, the maximal
				 * interactive mode write is 3 bytes
				 * length for function keys...
				 */

static int
snpwrite(struct dev_write_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct uio *uio = ap->a_uio;
	struct snoop *snp;
	struct tty *tp;
	int error, i, len;
	unsigned char c[SNP_INPUT_BUF];

	snp = dev->si_drv1;
	tp = snp->snp_tty;
	if (tp == NULL)
		return (EIO);
	lwkt_gettoken(&tp->t_token);
	if ((tp->t_sc == snp) && (tp->t_state & TS_SNOOP) &&
	    tp->t_line == snooplinedisc)
		goto tty_input;

	kprintf("Snoop: attempt to write to bad tty.\n");
	lwkt_reltoken(&tp->t_token);
	return (EIO);

tty_input:
	if (!(tp->t_state & TS_ISOPEN)) {
		lwkt_reltoken(&tp->t_token);
		return (EIO);
	}

	while (uio->uio_resid > 0) {
		len = (int)szmin(uio->uio_resid, SNP_INPUT_BUF);
		if ((error = uiomove(c, (size_t)len, uio)) != 0) {
			lwkt_reltoken(&tp->t_token);
			return (error);
		}
		for (i=0; i < len; i++) {
			if (ttyinput(c[i], tp)) {
				lwkt_reltoken(&tp->t_token);
				return (EIO);
			}
		}
	}
	lwkt_reltoken(&tp->t_token);
	return (0);
}


static int
snpread(struct dev_read_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct uio *uio = ap->a_uio;
	struct snoop *snp;
	struct tty *tp;
	int error, len, n, nblen;
	caddr_t from;
	char *nbuf;

	snp = dev->si_drv1;
	tp = snp->snp_tty;
	lwkt_gettoken(&tp->t_token);
	KASSERT(snp->snp_len + snp->snp_base <= snp->snp_blen,
	    ("snoop buffer error"));

	if (snp->snp_tty == NULL) {
		lwkt_reltoken(&tp->t_token);
		return (EIO);
	}

	snp->snp_flags &= ~SNOOP_RWAIT;

	do {
		if (snp->snp_len == 0) {
			if (ap->a_ioflag & IO_NDELAY) {
				lwkt_reltoken(&tp->t_token);
				return (EWOULDBLOCK);
			}
			snp->snp_flags |= SNOOP_RWAIT;
			error = tsleep((caddr_t)snp, PCATCH, "snprd", 0);
			if (error != 0) {
				lwkt_reltoken(&tp->t_token);
				return (error);
			}
		}
	} while (snp->snp_len == 0);

	n = snp->snp_len;

	error = 0;
	while (snp->snp_len > 0 && uio->uio_resid > 0 && error == 0) {
		len = (int)szmin(uio->uio_resid, snp->snp_len);
		from = (caddr_t)(snp->snp_buf + snp->snp_base);
		if (len == 0)
			break;

		error = uiomove(from, (size_t)len, uio);
		snp->snp_base += len;
		snp->snp_len -= len;
	}
	if ((snp->snp_flags & SNOOP_OFLOW) && (n < snp->snp_len)) {
		snp->snp_flags &= ~SNOOP_OFLOW;
	}
	nblen = snp->snp_blen;
	if (((nblen / 2) >= SNOOP_MINLEN) && (nblen / 2) >= snp->snp_len) {
		while (nblen / 2 >= snp->snp_len && nblen / 2 >= SNOOP_MINLEN)
			nblen = nblen / 2;
		if ((nbuf = kmalloc(nblen, M_SNP, M_NOWAIT)) != NULL) {
			bcopy(snp->snp_buf + snp->snp_base, nbuf, snp->snp_len);
			kfree(snp->snp_buf, M_SNP);
			snp->snp_buf = nbuf;
			snp->snp_blen = nblen;
			snp->snp_base = 0;
		}
	}
	lwkt_reltoken(&tp->t_token);

	return (error);
}

/*
 * NOTE: Must be called with tp->t_token held
 */
static int
snp_in(struct snoop *snp, char *buf, int n)
{
	int s_free, s_tail;
	int len, nblen;
	caddr_t from, to;
	char *nbuf;

	KASSERT(n >= 0, ("negative snoop char count"));

	if (n == 0)
		return (0);

	if (snp->snp_flags & SNOOP_DOWN) {
		kprintf("Snoop: more data to down interface.\n");
		return (0);
	}

	if (snp->snp_flags & SNOOP_OFLOW) {
		kprintf("Snoop: buffer overflow.\n");
		/*
		 * On overflow we just repeat the standart close
		 * procedure...yes , this is waste of space but.. Then next
		 * read from device will fail if one would recall he is
		 * snooping and retry...
		 */

		return (snp_down(snp));
	}
	s_tail = snp->snp_blen - (snp->snp_len + snp->snp_base);
	s_free = snp->snp_blen - snp->snp_len;


	if (n > s_free) {
		nblen = snp->snp_blen;
		while ((n > s_free) && ((nblen * 2) <= SNOOP_MAXLEN)) {
			nblen = snp->snp_blen * 2;
			s_free = nblen - (snp->snp_len + snp->snp_base);
		}
		if ((n <= s_free) && (nbuf = kmalloc(nblen, M_SNP, M_NOWAIT))) {
			bcopy(snp->snp_buf + snp->snp_base, nbuf, snp->snp_len);
			kfree(snp->snp_buf, M_SNP);
			snp->snp_buf = nbuf;
			snp->snp_blen = nblen;
			snp->snp_base = 0;
		} else {
			snp->snp_flags |= SNOOP_OFLOW;
			if (snp->snp_flags & SNOOP_RWAIT) {
				snp->snp_flags &= ~SNOOP_RWAIT;
				wakeup((caddr_t)snp);
			}
			return (0);
		}
	}
	if (n > s_tail) {
		from = (caddr_t)(snp->snp_buf + snp->snp_base);
		to = (caddr_t)(snp->snp_buf);
		len = snp->snp_len;
		bcopy(from, to, len);
		snp->snp_base = 0;
	}
	to = (caddr_t)(snp->snp_buf + snp->snp_base + snp->snp_len);
	bcopy(buf, to, n);
	snp->snp_len += n;

	if (snp->snp_flags & SNOOP_RWAIT) {
		snp->snp_flags &= ~SNOOP_RWAIT;
		wakeup((caddr_t)snp);
	}
	KNOTE(&snp->snp_kq.ki_note, 0);

	return (n);
}

static int
snpopen(struct dev_open_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct snoop *snp;

	lwkt_gettoken(&snp_token);
	if (dev->si_drv1 == NULL) {
#if 0
		make_dev(&snp_ops, minor(dev), UID_ROOT, GID_WHEEL,
		    0600, "snp%d", minor(dev));
#endif
		dev->si_drv1 = snp = kmalloc(sizeof(*snp), M_SNP,
					     M_WAITOK | M_ZERO);
	} else {
		lwkt_reltoken(&snp_token);
		return (EBUSY);
	}

	/*
	 * We intentionally do not OR flags with SNOOP_OPEN, but set them so
	 * all previous settings (especially SNOOP_OFLOW) will be cleared.
	 */
	snp->snp_flags = SNOOP_OPEN;

	snp->snp_buf = kmalloc(SNOOP_MINLEN, M_SNP, M_WAITOK);
	snp->snp_blen = SNOOP_MINLEN;
	snp->snp_base = 0;
	snp->snp_len = 0;

	/*
	 * snp_tty == NULL  is for inactive snoop devices.
	 */
	snp->snp_tty = NULL;
	snp->snp_target = NULL;

	LIST_INSERT_HEAD(&snp_sclist, snp, snp_list);
	lwkt_reltoken(&snp_token);
	return (0);
}

/*
 * NOTE: Must be called with snp_token held
 */
static int
snp_detach(struct snoop *snp)
{
	struct tty *tp;

	ASSERT_LWKT_TOKEN_HELD(&snp_token);
	snp->snp_base = 0;
	snp->snp_len = 0;

	/*
	 * If line disc. changed we do not touch this pointer, SLIP/PPP will
	 * change it anyway.
	 */
	tp = snp->snp_tty;
	if (tp == NULL)
		goto detach_notty;

	lwkt_gettoken(&tp->t_token);
	if ((tp->t_sc == snp) && (tp->t_state & TS_SNOOP) &&
	    tp->t_line == snooplinedisc) {
		tp->t_sc = NULL;
		tp->t_state &= ~TS_SNOOP;
		tp->t_line = snp->snp_olddisc;
	} else {
		kprintf("Snoop: bad attached tty data.\n");
	}
	lwkt_reltoken(&tp->t_token);

	snp->snp_tty = NULL;
	snp->snp_target = NULL;

detach_notty:
	KNOTE(&snp->snp_kq.ki_note, 0);
	if ((snp->snp_flags & SNOOP_OPEN) == 0) 
		kfree(snp, M_SNP);

	return (0);
}

static int
snpclose(struct dev_close_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct snoop *snp;
	int unit;
	int ret;

	lwkt_gettoken(&snp_token);
	snp = dev->si_drv1;
	snp->snp_blen = 0;
	LIST_REMOVE(snp, snp_list);
	kfree(snp->snp_buf, M_SNP);
	snp->snp_flags &= ~SNOOP_OPEN;
	dev->si_drv1 = NULL;
	unit = dev->si_uminor;
	if (unit >= SNP_PREALLOCATED_UNITS) {
		destroy_dev(dev);
		devfs_clone_bitmap_put(&DEVFS_CLONE_BITMAP(snp), unit);
	}
	ret = snp_detach(snp);
	lwkt_reltoken(&snp_token);

	return ret;
}

/*
 * NOTE: Must be called with snp_token held
 */
static int
snp_down(struct snoop *snp)
{
	ASSERT_LWKT_TOKEN_HELD(&snp_token);
	if (snp->snp_blen != SNOOP_MINLEN) {
		kfree(snp->snp_buf, M_SNP);
		snp->snp_buf = kmalloc(SNOOP_MINLEN, M_SNP, M_WAITOK);
		snp->snp_blen = SNOOP_MINLEN;
	}
	snp->snp_flags |= SNOOP_DOWN;

	return (snp_detach(snp));
}

static int
snpioctl(struct dev_ioctl_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct snoop *snp;
	struct tty *tp, *tpo;
	cdev_t tdev;
	int ret;

	lwkt_gettoken(&snp_token);
	snp = dev->si_drv1;

	switch (ap->a_cmd) {
	case SNPSTTY:
		tdev = udev2dev(*((udev_t *)ap->a_data), 0);
		if (tdev == NULL) {
			lwkt_reltoken(&snp_token);
			ret = snp_down(snp);
			return ret;
		}

		tp = snpdevtotty(tdev);
		if (!tp) {
			lwkt_reltoken(&snp_token);
			return (EINVAL);
		}
		lwkt_gettoken(&tp->t_token);
		if (tp->t_state & TS_SNOOP) {
			lwkt_reltoken(&tp->t_token);
			lwkt_reltoken(&snp_token);
			return (EBUSY);
		}

		if (snp->snp_target == NULL) {
			tpo = snp->snp_tty;
			if (tpo)
				tpo->t_state &= ~TS_SNOOP;
		}

		tp->t_sc = (caddr_t)snp;
		tp->t_state |= TS_SNOOP;
		snp->snp_olddisc = tp->t_line;
		tp->t_line = snooplinedisc;
		snp->snp_tty = tp;
		snp->snp_target = tdev;

		/*
		 * Clean overflow and down flags -
		 * we'll have a chance to get them in the future :)))
		 */
		snp->snp_flags &= ~SNOOP_OFLOW;
		snp->snp_flags &= ~SNOOP_DOWN;
		lwkt_reltoken(&tp->t_token);
		break;

	case SNPGTTY:
		/*
		 * We keep snp_target field specially to make
		 * SNPGTTY happy, else we can't know what is device
		 * major/minor for tty.
		 */
		*((cdev_t *)ap->a_data) = snp->snp_target;
		break;

	case FIOASYNC:
		if (*(int *)ap->a_data)
			snp->snp_flags |= SNOOP_ASYNC;
		else
			snp->snp_flags &= ~SNOOP_ASYNC;
		break;

	case FIONREAD:
		if (snp->snp_tty != NULL) {
			*(int *)ap->a_data = snp->snp_len;
		} else {
			if (snp->snp_flags & SNOOP_DOWN) {
				if (snp->snp_flags & SNOOP_OFLOW)
					*(int *)ap->a_data = SNP_OFLOW;
				else
					*(int *)ap->a_data = SNP_TTYCLOSE;
			} else {
				*(int *)ap->a_data = SNP_DETACH;
			}
		}
		break;

	default:
		lwkt_reltoken(&snp_token);
		return (ENOTTY);
	}
	lwkt_reltoken(&snp_token);

	return (0);
}

static struct filterops snpfiltops_rd =
        { FILTEROP_ISFD, NULL, snpfilter_detach, snpfilter_rd };
static struct filterops snpfiltops_wr =
        { FILTEROP_ISFD, NULL, snpfilter_detach, snpfilter_wr };

static int
snpkqfilter(struct dev_kqfilter_args *ap)
{
	cdev_t dev = ap->a_head.a_dev;
	struct snoop *snp = dev->si_drv1;
	struct knote *kn = ap->a_kn;
	struct klist *klist;
	struct tty *tp = snp->snp_tty;

	lwkt_gettoken(&tp->t_token);
	ap->a_result = 0;

	switch (kn->kn_filter) {
	case EVFILT_READ:
		kn->kn_fop = &snpfiltops_rd;
		kn->kn_hook = (caddr_t)snp;
		break;
	case EVFILT_WRITE:
		kn->kn_fop = &snpfiltops_wr;
		kn->kn_hook = (caddr_t)snp;
		break;
	default:
		ap->a_result = EOPNOTSUPP;
		lwkt_reltoken(&tp->t_token);
		return (0);
	}

	klist = &snp->snp_kq.ki_note;
	knote_insert(klist, kn);
	lwkt_reltoken(&tp->t_token);

	return (0);
}

static void
snpfilter_detach(struct knote *kn)
{
	struct snoop *snp = (struct snoop *)kn->kn_hook;
	struct klist *klist;

	klist = &snp->snp_kq.ki_note;
	knote_remove(klist, kn);
}

static int
snpfilter_rd(struct knote *kn, long hint)
{
	struct snoop *snp = (struct snoop *)kn->kn_hook;
	struct tty *tp = snp->snp_tty;
	int ready = 0;

	lwkt_gettoken(&tp->t_token);
	/*
	 * If snoop is down, we don't want to poll forever so we return 1.
	 * Caller should see if we down via FIONREAD ioctl().  The last should
	 * return -1 to indicate down state.
	 */
	if (snp->snp_flags & SNOOP_DOWN || snp->snp_len > 0)
		ready = 1;
	lwkt_reltoken(&tp->t_token);

	return (ready);
}

static int
snpfilter_wr(struct knote *kn, long hint)
{
	/* Writing is always OK */
	return (1);
}

static int
snpclone(struct dev_clone_args *ap)
{
	int unit;

	lwkt_gettoken(&snp_token);
	unit = devfs_clone_bitmap_get(&DEVFS_CLONE_BITMAP(snp), 0);
	ap->a_dev = make_only_dev(&snp_ops, unit, UID_ROOT, GID_WHEEL, 0600,
				  "snp%d", unit);
	lwkt_reltoken(&snp_token);

	return 0;
}

static int
snp_modevent(module_t mod, int type, void *data)
{
	int i;

	lwkt_gettoken(&snp_token);

	switch (type) {
	case MOD_LOAD:
		snooplinedisc = ldisc_register(LDISC_LOAD, &snpdisc);
		make_autoclone_dev(&snp_ops, &DEVFS_CLONE_BITMAP(snp),
			snpclone, UID_ROOT, GID_WHEEL, 0600, "snp");

		for (i = 0; i < SNP_PREALLOCATED_UNITS; i++) {
			make_dev(&snp_ops, i, UID_ROOT, GID_WHEEL, 0600,
				 "snp%d", i);
			devfs_clone_bitmap_set(&DEVFS_CLONE_BITMAP(snp), i);
		}
		break;
	case MOD_UNLOAD:
		if (!LIST_EMPTY(&snp_sclist)) {
			lwkt_reltoken(&snp_token);
			return (EBUSY);
		}
		ldisc_deregister(snooplinedisc);
		devfs_clone_handler_del("snp");
		dev_ops_remove_all(&snp_ops);
		devfs_clone_bitmap_uninit(&DEVFS_CLONE_BITMAP(snp));
		break;
	default:
		break;
	}
	lwkt_reltoken(&snp_token);

	return (0);
}

static moduledata_t snp_mod = {
        "snp",
        snp_modevent,
        NULL
};
DECLARE_MODULE(snp, snp_mod, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
