#ifndef	SPDK_BDEV_BIO_H
#define	SPDK_BDEV_BIO_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"

typedef void (*delete_bdev_bio_complete)(void *cb_arg, int bdeverrno);

int create_bdev_bio(const char *name, const char *filename);

void bdev_bio_delete(struct spdk_bdev *bdev, delete_bdev_bio_complete cb_fn, void *cb_arg);

#endif	/* SPDK_BDEV_BIO_H */
