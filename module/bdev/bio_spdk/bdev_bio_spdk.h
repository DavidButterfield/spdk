#ifndef	SPDK_BDEV_BIO_SPDK_H
#define	SPDK_BDEV_BIO_SPDK_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"

typedef void (*delete_bio_spdk_complete)(void *cb_arg, int bdeverrno);

int create_bio_spdk(const char *name);

void bio_spdk_delete(const char *name, delete_bio_spdk_complete cb_fn, void *cb_arg);

#endif	/* SPDK_BDEV_BIO_SPDK_H */
