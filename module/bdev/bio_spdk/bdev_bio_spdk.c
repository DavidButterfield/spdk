/* bdev_bio_spdk.c adapted from bdev_aio.c
 * Provides translation from bio bdev protocol to SPDK bdev protocol
 *-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#define  NO_UMC_SOCKETS
#include "UMC_bio.h"
#undef ntohl
#undef ntohs
#undef htonl
#undef htons
#include "bdev_bio_spdk.h"

#include "spdk/stdinc.h"
#include "spdk/barrier.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/fd.h"
#include "spdk/likely.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/util.h"
#include "spdk/string.h"

#include "spdk_internal/log.h"

struct bio_spdk_op {
    struct work_struct		work_entry;
    struct bio		      * bio;
    struct iovec		iovec[0];  /* keep last */
};

struct bio_spdk_dev {
    struct list_head		list_entry;	/* bio_spdk_bdevs list */
    struct workqueue_struct   *	workq;		/* reply handoff SPDK->bio */
    struct block_device	      *	bio_bdev;
    struct spdk_bdev	      *	spdk_bdev;
    struct spdk_bdev_desc     *	spdkfd;
    struct spdk_io_channel    *	ch;
    struct spdk_thread	      *	thr;
    char		      * name;
    size_t			dev_size;
};

#define spdk_bdev_of_bio_bdev(bio_bdev) ((bio_bdev)->bd_disk->private_data)

//XXXXX needs a lock
static struct list_head bio_spdk_bdevs = { &bio_spdk_bdevs, &bio_spdk_bdevs };
static int bio_spdk_nextminor;
static int bio_spdk_major = 17;		/* arbitrary */

static int bio_spdk_init(void);
static void bio_spdk_fini(void);
static void bio_spdk_get_spdk_running_config(FILE *fp);

static int
bio_spdk_get_ctx_size(void)
{
	return 0; //XXX sizeof(struct bdev_spdk_op);
}

static struct spdk_bdev_module bio_spdk_if = {
	.name		= "bio_spdk",
	.module_init	= bio_spdk_init,
	.module_fini	= bio_spdk_fini,
	.config_text	= bio_spdk_get_spdk_running_config,
	.get_ctx_size	= bio_spdk_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(bio, &bio_spdk_if)

/* Runs on MTE/UMC work queue service thread */
static void
op_reply_onthread(struct work_struct * work)
{
    struct bio_spdk_op * op = container_of(work, struct bio_spdk_op, work_entry);
    bio_endio(op->bio, op->bio->bi_error);
    vfree(op);
}

/* Runs on SPDK thread */
static inline void
op_reply(struct bio_spdk_op * op, error_t err)
{
    struct bio * bio = op->bio;
    struct bio_spdk_dev * dev = spdk_bdev_of_bio_bdev(bio->bi_bdev);
    bio->bi_error = err;
    INIT_WORK(&op->work_entry, op_reply_onthread);
    queue_work(dev->workq, &op->work_entry);
}

/* Completion callback from SPDK runs on SPDK thread */
static void
io_done(struct spdk_bdev_io * bdev_io, bool ok, void * v_op)
{
    struct bio_spdk_op * op = v_op;
    op_reply(op, ok ? 0 : -EIO);
    if (bdev_io)
	spdk_bdev_free_io(bdev_io);
}

static error_t _make_request(struct bio_spdk_op *);

/* Here on SPDK thread after a sync that also has an I/O with it */
static void
io_continue(struct spdk_bdev_io * bdev_io, bool ok, void * v_op)
{
    struct bio_spdk_op * op = v_op;
    struct bio * bio = op->bio;
    assert(op_is_sync(UMC_bio_op(bio)));

    if (!ok)
	io_done(bdev_io, ok, op);
    else {
	bio->bi_rw &= ~REQ_BARRIER;
	assert(!op_is_sync(UMC_bio_op(bio)));
	_make_request(op);		/* issue the I/O op */
	if (bdev_io)
	    spdk_bdev_free_io(bdev_io);	/* free the sync op */
    }
}

/* Runs on SPDK thread */
static error_t
_make_request(struct bio_spdk_op * op)
{
    struct bio * bio = op->bio;
    struct bio_spdk_dev * dev = spdk_bdev_of_bio_bdev(bio->bi_bdev);
    bool is_sync = op_is_sync(UMC_bio_op(bio));
    bool is_write = op_is_write(UMC_bio_op(bio));
    unsigned short iovn = bio->bi_idx;	    //XXX start here, right?
    uint64_t seekpos = bio->bi_sector << 9;
    size_t iov_nbytes = 0;
    size_t cmdlen = 0;
    void (*cb)(struct spdk_bdev_io *, bool, void *) = io_done;

    #define BITS_OK ((1ul<<BIO_RW_FAILFAST)|REQ_META|REQ_SYNC|REQ_BARRIER)
    expect_eq(bio->bi_rw & ~(WRITE|BITS_OK), 0,
		"Unexpected bi_rw bits 0x%lx/0x%lx",
		bio->bi_rw & ~(WRITE|BITS_OK), bio->bi_rw);

    if (seekpos >= dev->dev_size) {
	pr_warning("attempt to seek device %s to %lu outside bound %lu\n",
			dev->name, seekpos, dev->dev_size);
	bio_set_flag(bio, BIO_EOF);
	return -EIO;
    }

    //XXX Figure out intended sync semantics
    if (is_sync) {
	if (!bio_empty_barrier(bio))
	    cb = io_continue;	/* come back for the I/O after the sync */

	/* if there's an I/O to do, io_continue() will call us back */
	//XXXXXXXX return spdk_bdev_flush(dev->spdkfd, dev->ch, 0, dev->dev_size, cb, op);
	cb(NULL, true, op);
	return 0;
    }

    cmdlen = bio->bi_size;

    if (seekpos + cmdlen > dev->dev_size)
	cmdlen = (unsigned int)(dev->dev_size - seekpos);

    /* Translate the segments of the bio data I/O buffer into iovec entries,
     * coalescing adjacent buffer segments.  (It is OK that coalescing means we
     * might not use all of the iovec array)
     */
    while (bio->bi_idx < bio->bi_vcnt) {
	size_t seglen = bio->bi_io_vec[bio->bi_idx].bv_len; /* get next sg segment */
	uint8_t * segaddr = (uint8_t *)bio->bi_io_vec[bio->bi_idx].bv_page->vaddr
				+ bio->bi_io_vec[bio->bi_idx].bv_offset;

	if (iovn > 0 && segaddr == (uint8_t *)op->iovec[iovn-1].iov_base
					    + op->iovec[iovn-1].iov_len) {
	    op->iovec[iovn-1].iov_len += seglen;    /* coalesce with previous entry */
	} else {
	    assert_lt(iovn, bio->bi_vcnt, "iovn=%d bio->bi_vcnt=%d", iovn, bio->bi_vcnt);
	    op->iovec[iovn].iov_base = segaddr;	    /* fill in a new entry */
	    op->iovec[iovn].iov_len = seglen;
	    ++iovn;
	}
	iov_nbytes += seglen;
	++bio->bi_idx;
    }

    if (iov_nbytes < cmdlen)
	return -EINVAL;

    if (is_write)
	return spdk_bdev_writev(dev->spdkfd, dev->ch, op->iovec, iovn, seekpos, cmdlen, cb, op);
    else
	return spdk_bdev_readv(dev->spdkfd, dev->ch, op->iovec, iovn, seekpos, cmdlen, cb, op);
}

/* Runs on SPDK thread */
static void
make_request_onthread(void * v_op)
{
    error_t err;
    struct bio_spdk_op * op = v_op;
    struct bio_spdk_dev * dev = spdk_bdev_of_bio_bdev(op->bio->bi_bdev);
    assert(dev->ch);
    assert_eq(dev->thr, spdk_get_thread());

    err = _make_request(op);
    if (err)
	op_reply(op, err);
}

/* Runs on requesting MTE/UMC bio thread */
static error_t
make_request(struct request_queue * unused, struct bio * bio)
{
    struct bio_spdk_dev * dev = spdk_bdev_of_bio_bdev(bio->bi_bdev);
    struct bio_spdk_op * op = vzalloc(sizeof(*op) +
				bio->bi_vcnt * sizeof(struct iovec));
    if (!op)
	return -ENOMEM;

    /* Handoff the request to an SPDK thread */
    assert(dev->thr);
    op->bio = bio;
    spdk_thread_send_msg(dev->thr, make_request_onthread, op);

    return 0;
}

/******************************************************************************/
#if 0
static void bio_free_disk(struct bio_spdk_dev *dev)
{
	if (dev == NULL) {
		return;
	}
	if (dev->name)
	    free(dev->name);
	free(dev);
}

static int
bio_spdk_destruct(void *ctx)
{
	struct bio_spdk_dev *dev = ctx;
	int rc = 0;

	TAILQ_REMOVE(&g_bio_spdk_head, dev, link);
	rc = bio_spdk_close(dev);
	if (rc < 0) {
		SPDK_ERRLOG("bio_spdk_close() failed\n");
	}
	spdk_io_device_unregister(dev, NULL);
	bio_free_disk(dev);
	return rc;
}

static int
bio_spdk_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct bio_spdk_dev *dev = ctx;
	spdk_json_write_named_object_begin(w, "bio_spdk");
	spdk_json_write_named_string(w, "name", dev->name);
	spdk_json_write_object_end(w);
	return 0;
}

static void
bio_spdk_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct bio_spdk_dev *dev = bdev->ctxt;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bio_spdk_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", dev->name);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table bio_spdk_fn_table = {
	.dump_info_json		= bio_spdk_dump_info_json,
	.write_config_json	= bio_spdk_write_json_config,
};
#endif

static int
bio_spdk_open(struct block_device * bdev, fmode_t fmode)
{
    return 0;
}

static int
bio_spdk_release(struct gendisk * disk, fmode_t fmode)
{
    return 0;
}

struct block_device_operations bio_spdk_ops = {
    .open = bio_spdk_open,
    .release = bio_spdk_release,
};

static void
event_cb(enum spdk_bdev_event_type type, struct spdk_bdev * bdev, void * v_dev)
{
    //struct bio_spdk_dev * dev = v_dev;
    //XXX
    pr_warning("UNEXPECTED call to %s\n", __func__);
}

error_t
create_bio_spdk(const char * bdev_name)
{
    struct block_device * bio_bdev;
    size_t dev_size;
    size_t block_size;
    int blkbits;
    struct bio_spdk_dev * dev;

    struct spdk_bdev * spdk_bdev = spdk_bdev_get_by_name(bdev_name);
    if (!spdk_bdev)
	return -ENOENT;

    block_size = spdk_bdev_get_data_block_size(spdk_bdev);
    dev_size = block_size * spdk_bdev_get_num_blocks(spdk_bdev);
    blkbits = ilog2(block_size);

    pr_notice("%s name=%s size=%ld blkbits=%d block_size=%ld\n",
			__func__, bdev_name, dev_size, blkbits, block_size);

    if (block_size != 1u << blkbits) {
	pr_err("%s: bad block size=%ld not a power of two (%d)\n",
			bdev_name, block_size, blkbits);
	return -EINVAL;
    }

    if (block_size < 512 || block_size > UINT_MAX) {
	pr_err("%s: bad block size=%ld\n", bdev_name, block_size);
	return -EINVAL;
    }

    if (dev_size < block_size) {
	pr_err("%s: bad device size=%"PRIu64"\n", bdev_name, dev_size);
	return -EINVAL;
    }

    bio_bdev = bdev_complex(bdev_name, bio_spdk_major, bio_spdk_nextminor++,
			make_request, blkbits, dev_size);

    bio_bdev->bd_disk->fops = &bio_spdk_ops;
    set_capacity(bio_bdev->bd_disk, dev_size>>9);

    dev = calloc(1, sizeof(*dev));
    if (!dev)
	return -ENOMEM;

    bio_bdev->bd_disk->private_data = dev;

    dev->name = strdup(bdev_name);
    dev->spdk_bdev = spdk_bdev;
    dev->bio_bdev = bio_bdev;
    dev->dev_size = dev_size;
    dev->workq = create_workqueue(bdev_name);
    list_add(&dev->list_entry, &bio_spdk_bdevs);

    struct spdk_bdev_desc * spdkfd;
    error_t err = spdk_bdev_open_ext(dev->name, true, event_cb, dev, &spdkfd);
    if (!err) {
	dev->spdkfd = spdkfd;
	dev->ch = spdk_bdev_get_io_channel(dev->spdkfd);
    }
    dev->thr = spdk_get_thread();	//XXXX

    return 0;
}

void
bio_spdk_delete(const char *name, delete_bio_spdk_complete cb_fn, void *cb_arg)
{
	struct bio_spdk_dev * dev;
	list_for_each_entry(dev, &bio_spdk_bdevs, list_entry)
		if (!strcmp(name, dev->name))
			break;

	if (!dev) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	list_del(&dev->list_entry);
	bdev_complex_free(dev->bio_bdev);
	spdk_bdev_close(dev->spdkfd);
	free(dev->name);
	free(dev);
	cb_fn(cb_arg, 0);
}

static error_t
bio_spdk_init(void)
{
	error_t err;
	size_t i;
	struct spdk_conf_section *sp;

	sp = spdk_conf_find_section(NULL, "BIO_SPDK");
	if (!sp)
		return 0;

	for (i = 0; ; i++) {
		const char *name;
		name = spdk_conf_section_get_nmval(sp, "BIO_SPDK", i, 0);
		if (!name)
			break;

		err = create_bio_spdk(name);
		if (err)
			SPDK_ERRLOG("Unable to create BIO_SPDK %s, err is %s (%d)\n",
				    name, spdk_strerror(-err), err);
	}

	return 0;
}

static void
bio_spdk_fini(void)
{
	spdk_io_device_unregister(&bio_spdk_if, NULL);
}

static void
fake_write_json_config(struct bio_spdk_dev *dev, FILE *fp)
{
	#define _PREFIX_ "\n#        "
	fprintf(fp, _PREFIX_ "{");
	fprintf(fp, _PREFIX_ "  \"method\": \"bio_spdk_create\",");
	fprintf(fp, _PREFIX_ "  \"params\": {");
	fprintf(fp, _PREFIX_ "    \"name\": \"%s\"", dev->name);
	fprintf(fp, _PREFIX_ "  }");
	fprintf(fp, _PREFIX_ "}");
	fprintf(fp, "\n");
}

static void
bio_spdk_get_spdk_running_config(FILE *fp)
{
	struct bio_spdk_dev *dev;

	fprintf(fp,
		"\n"
		"# Translate from kernel bio bdev protocol to SPDK bdev protocol\n"
		"# (UMC bio bdev front-end interface for SPDK bdev devices)\n"
		"# bio name will be '/UMCfuse/dev/spdk-name'\n"
		"# Format:\n"
		"# BIO_SPDK <spdk-name>\n"
		"[BIO_SPDK]\n");

	list_for_each_entry(dev, &bio_spdk_bdevs, list_entry)
		fprintf(fp, "  BIO_SPDK %s\n", dev->name);

	fprintf(fp, "\n");

	//XXX Dump the json here because we don't have SPDK device entry points
	fprintf(fp, "#XXX JSON for load_config:");
	list_for_each_entry(dev, &bio_spdk_bdevs, list_entry)
		fake_write_json_config(dev, fp);

	fprintf(fp, "\n");
}

SPDK_LOG_REGISTER_COMPONENT("bio_spdk", SPDK_LOG_BIO_SPDK)
