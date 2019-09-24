/* bdev_bio.c adapted from bdev_aio.c */
/*-
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
#include "bdev_bio.h"

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

struct bdev_bio_io_channel {
	uint64_t			io_inflight;
};

struct bdev_bio_task {
	struct bdev_bio_io_channel	*ch;
	struct page			*pages;
	TAILQ_ENTRY(bdev_bio_task)	link;
};

/* bio device */
struct file_disk {
	struct bdev_bio_task	*reset_task;
	struct spdk_poller	*reset_retry_timer;
	struct spdk_bdev	disk;
	char			*filename;	/* bio device name */
	struct block_device	*bdev;
	TAILQ_ENTRY(file_disk)  link;
	bool			block_size_override;
};

static int bdev_bio_initialize(void);
static void bdev_bio_fini(void);
static void bdev_bio_get_spdk_running_config(FILE *fp);
static TAILQ_HEAD(, file_disk) g_bio_disk_head;

static int
bdev_bio_get_ctx_size(void)
{
	return sizeof(struct bdev_bio_task);
}

static struct spdk_bdev_module bio_if = {
	.name		= "bio",
	.module_init	= bdev_bio_initialize,
	.module_fini	= bdev_bio_fini,
	.config_text	= bdev_bio_get_spdk_running_config,
	.get_ctx_size	= bdev_bio_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(bio, &bio_if)

#define BIO_OPEN_MODE (O_RDWR | O_DIRECT)

static int
bdev_bio_open(struct file_disk *fdisk)
{
	struct block_device * bdev_bio;

	bdev_bio = open_bdev_exclusive(fdisk->filename, BIO_OPEN_MODE, NULL);
	if (IS_ERR_OR_NULL(bdev_bio)) {
		SPDK_ERRLOG("open() failed (file:%s), ret=%ld: %s\n", fdisk->filename,
				PTR_ERR(bdev_bio), spdk_strerror(-PTR_ERR(bdev_bio)));
		return PTR_ERR(bdev_bio);
	}

	fdisk->bdev = bdev_bio;

	return 0;
}

static int
bdev_bio_close(struct file_disk *fdisk)
{
	error_t rc;

	if (!fdisk->bdev)
		return 0;

	rc = 0;	close_bdev_exclusive(fdisk->bdev, BIO_OPEN_MODE);
	if (rc < 0) {
		SPDK_ERRLOG("close() failed on %s, rc=%d: %s\n",
			    fdisk->filename, rc, spdk_strerror(-rc));
		return -1;
	}

	fdisk->bdev = NULL;

	return 0;
}

/* Issue completion callback on our requesting SPDK thread */
static inline void
do_callback(void * arg)
{
	struct bio * bio = arg;
	struct bdev_bio_task * bdev_task = bio->bi_private;
	int status = SPDK_BDEV_IO_STATUS_SUCCESS;

	if (bio->bi_error == -ENOMEM) {
		status = SPDK_BDEV_IO_STATUS_NOMEM;
		SPDK_DEBUGLOG(SPDK_LOG_BIO, "%s: got/returning ENOMEM\n", __func__);
	} else if (bio->bi_error) {
		status = SPDK_BDEV_IO_STATUS_FAILED;
		SPDK_ERRLOG("%s: bdev_bio got/returning ERROR %d\n", __func__, bio->bi_error);
	}

	bio_put(bio);
	if (bdev_task->pages) {
		free(bdev_task->pages);
		bdev_task->pages = NULL;
	}

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_task), status);
}

/* Callback from bio implementor */
static void
cmd_done(struct bio * bio, error_t err)
{
	struct bdev_bio_task * bdev_task = bio->bi_private;
	bio->bi_error = err;

	bdev_task->ch->io_inflight--;

	/* Defer completion callback to the SPDK thread the request came in on */
	spdk_thread_send_msg(
		spdk_io_channel_get_thread(spdk_io_channel_from_ctx(bdev_task->ch)),
		do_callback, bio);
}

static struct bio *
bdev_bio_alloc_set(struct block_device * bdev, struct iovec * iov, int iovcnt,
			size_t nbytes, off_t offset, struct bdev_bio_task * bdev_task)
{
	int iovn;
	struct bio * bio;
	struct page * page;
	unsigned int npage;
	uint64_t dev_size = bdev->bd_inode->i_size;

	memset(bdev_task, 0, sizeof(*bdev_task));  //XXX provided space apparently not zeroed?

	assert_imply(nbytes, iov);
	assert_imply(nbytes, iovcnt);
	assert_eq(offset % 512, 0, "unaligned offset on minor %d", bdev->bd_disk->first_minor);
	assert_eq(nbytes % 512, 0, "unaligned nbytes on minor %d", bdev->bd_disk->first_minor);

	if ((uint64_t)offset >= dev_size)
		return ERR_PTR(-EINVAL);
	if ((uint64_t)offset + nbytes > dev_size)
		nbytes = dev_size - offset;

	/* npage is upper bound on entries needed */
	npage = 2 * iovcnt + (unsigned int)(nbytes / PAGE_SIZE);

	bio = bio_alloc(0, npage);
	bio_set_dev(bio, bdev);
	bio->bi_sector = offset >> 9;
	bio->bi_end_io = cmd_done;
	bio->bi_private = bdev_task;

	if (npage) {
	    page = bdev_task->pages = calloc(npage, sizeof(*page));
	    if (!page)
		return ERR_PTR(-ENOMEM);

	    /* Each iovec entry may comprise multiple pages, but bio wants pages separate (XXX really?) */
	    for (iovn = 0; iovn < iovcnt; iovn++) {
		size_t size = iov[iovn].iov_len;
		char * p = iov[iovn].iov_base;
		off_t page_off = offset_in_page(p);
		assert_lt(page_off, PAGE_SIZE);

		/* Fill in the bio page list with the pages of the current iov entry */
		while (size) {
			size_t page_datalen = min((size_t)(PAGE_SIZE - page_off), size);
			assert_lt(page, bdev_task->pages + npage);
			page->vaddr = (void *)((uintptr_t)p & PAGE_MASK);
			page->order = 1;
			bio_add_page(bio, page, (unsigned int)page_datalen, (unsigned int)page_off);
			p += page_datalen;
			size -= page_datalen;
			page++;
			page_off = 0;	/* non-first pages of each iov entry start at offset zero */
		}
	    }
	}

	assert_ge(bio->bi_size, nbytes);
	return bio;
}

static void
bdev_bio_readv(struct file_disk *fdisk, struct spdk_io_channel *ch,
	       struct bdev_bio_task *bdev_task,
	       struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct bdev_bio_io_channel *bio_ch = spdk_io_channel_get_ctx(ch);
	error_t err;

	struct bio * bio = bdev_bio_alloc_set(fdisk->bdev, iov, iovcnt, nbytes, offset, bdev_task);
	if (IS_ERR(bio)) {
		int status = PTR_ERR(bio) == -ENOMEM
				? SPDK_BDEV_IO_STATUS_NOMEM
			  	: SPDK_BDEV_IO_STATUS_FAILED;
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_task), status);
		return;
	}

	bdev_task->ch = bio_ch;

	SPDK_DEBUGLOG(SPDK_LOG_BIO, "read %d iovs size %lu at off: %#lx\n",
		      iovcnt, nbytes, offset);

	err = submit_bio(READ, bio);
	if (err) {
		bio->bi_error = err;
		do_callback(bio);
	} else
		bio_ch->io_inflight++;
}

static void
bdev_bio_writev(struct file_disk *fdisk, struct spdk_io_channel *ch,
	       struct bdev_bio_task *bdev_task,
	       struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct bdev_bio_io_channel *bio_ch = spdk_io_channel_get_ctx(ch);
	error_t err;

	struct bio * bio = bdev_bio_alloc_set(fdisk->bdev, iov, iovcnt, nbytes, offset, bdev_task);
	if (IS_ERR(bio)) {
		int status = PTR_ERR(bio) == -ENOMEM
				? SPDK_BDEV_IO_STATUS_NOMEM
			  	: SPDK_BDEV_IO_STATUS_FAILED;
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_task), status);
		return;
	}

	bdev_task->ch = bio_ch;

	SPDK_DEBUGLOG(SPDK_LOG_BIO, "write %d iovs size %lu at off: %#lx\n",
		      iovcnt, nbytes, offset);

	err = submit_bio(WRITE, bio);
	if (err) {
		bio->bi_error = err;
		do_callback(bio);
	} else
		bio_ch->io_inflight++;
}

static void
bdev_bio_flush(struct file_disk *fdisk, struct spdk_io_channel *ch, struct bdev_bio_task *bdev_task)
{
#if 0
	struct bdev_bio_io_channel *bio_ch = spdk_io_channel_get_ctx(ch);
	error_t err;
#endif

	struct bio * bio = bdev_bio_alloc_set(fdisk->bdev, NULL, 0, 0, 0, bdev_task);
	if (IS_ERR(bio)) {
		int status = PTR_ERR(bio) == -ENOMEM
				? SPDK_BDEV_IO_STATUS_NOMEM
			  	: SPDK_BDEV_IO_STATUS_FAILED;
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_task), status);
		return;
	}

#if 0	//XXXXXX flush disabled
	bdev_task->ch = bio_ch;

	SPDK_DEBUGLOG(SPDK_LOG_BIO, "flush\n");

	err = submit_bio(WRITE|REQ_BARRIER, bio);	//XXX check this!
	if (err) {
		bio->bi_error = err;
		do_callback(bio);
	} else
		bio_ch->io_inflight++;
#else
	do_callback(bio);
#endif
}

static uint64_t
bdev_bio_get_size(struct file_disk *fdisk)
{
	return fdisk->bdev->bd_inode->i_size;
}

static uint32_t
bdev_bio_get_blocklen(struct file_disk *fdisk)
{
	return 1u << fdisk->bdev->bd_inode->i_blkbits;
}

/******************************************************************************/

static void bio_free_disk(struct file_disk *fdisk)
{
	if (fdisk == NULL) {
		return;
	}
	if (fdisk->filename)
	    free(fdisk->filename);
	if(fdisk->disk.name)
	    free(fdisk->disk.name);
	free(fdisk);
}

static int
bdev_bio_destruct(void *ctx)
{
	struct file_disk *fdisk = ctx;
	int rc = 0;

	TAILQ_REMOVE(&g_bio_disk_head, fdisk, link);
	rc = bdev_bio_close(fdisk);
	if (rc < 0) {
		SPDK_ERRLOG("bdev_bio_close() failed\n");
	}
	spdk_io_device_unregister(fdisk, NULL);
	bio_free_disk(fdisk);
	return rc;
}

static void
_bdev_bio_get_io_inflight(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct bdev_bio_io_channel *bio_ch = spdk_io_channel_get_ctx(ch);

	if (bio_ch->io_inflight) {
		spdk_for_each_channel_continue(i, -1);
		return;
	}

	spdk_for_each_channel_continue(i, 0);
}

static int bdev_bio_reset_retry_timer(void *arg);

static void
_bdev_bio_get_io_inflight_done(struct spdk_io_channel_iter *i, int status)
{
	struct file_disk *fdisk = spdk_io_channel_iter_get_ctx(i);

	if (status == -1) {
		fdisk->reset_retry_timer = spdk_poller_register(bdev_bio_reset_retry_timer, fdisk, 500);
		return;
	}

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(fdisk->reset_task), SPDK_BDEV_IO_STATUS_SUCCESS);
}

static int
bdev_bio_reset_retry_timer(void *arg)
{
	struct file_disk *fdisk = arg;

	if (fdisk->reset_retry_timer) {
		spdk_poller_unregister(&fdisk->reset_retry_timer);
	}

	spdk_for_each_channel(fdisk,
			      _bdev_bio_get_io_inflight,
			      fdisk,
			      _bdev_bio_get_io_inflight_done);

	return -1;
}

static void
bdev_bio_reset(struct file_disk *fdisk, struct bdev_bio_task *bdev_task)
{
	fdisk->reset_task = bdev_task;

	bdev_bio_reset_retry_timer(fdisk);
}

static void
bdev_bio_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		    bool success)
{
	if (!success) {
		SPDK_ERRLOG("spdk_bdev_io_get_buf failed\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		bdev_bio_readv((struct file_disk *)bdev_io->bdev->ctxt,
			       ch,
			       (struct bdev_bio_task *)bdev_io->driver_ctx,
			       bdev_io->u.bdev.iovs,
			       bdev_io->u.bdev.iovcnt,
			       bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
			       bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		bdev_bio_writev((struct file_disk *)bdev_io->bdev->ctxt,
				ch,
				(struct bdev_bio_task *)bdev_io->driver_ctx,
			        bdev_io->u.bdev.iovs,
				bdev_io->u.bdev.iovcnt,
				bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
				bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);
		break;
	default:
		SPDK_ERRLOG("Wrong io type\n");
		break;
	}
}

static int _bdev_bio_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	/* Read and write operations must be performed on buffers aligned to
	 * bdev->required_alignment. If user specified unaligned buffers,
	 * get the aligned buffer from the pool by calling spdk_bdev_io_get_buf. */
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		spdk_bdev_io_get_buf(bdev_io, bdev_bio_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		bdev_bio_flush((struct file_disk *)bdev_io->bdev->ctxt,
				ch,
			       (struct bdev_bio_task *)bdev_io->driver_ctx);
		return 0;

	case SPDK_BDEV_IO_TYPE_RESET:
		bdev_bio_reset((struct file_disk *)bdev_io->bdev->ctxt,
			       (struct bdev_bio_task *)bdev_io->driver_ctx);
		return 0;
	default:
		return -1;
	}
}

static void bdev_bio_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if (_bdev_bio_submit_request(ch, bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
bdev_bio_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
		return true;

	default:
		return false;
	}
}

static int
bdev_bio_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
bdev_bio_destroy_cb(void *io_device, void *ctx_buf)
{
}

static struct spdk_io_channel *
bdev_bio_get_io_channel(void *ctx)
{
	struct file_disk *fdisk = ctx;

	return spdk_get_io_channel(fdisk);
}


static int
bdev_bio_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct file_disk *fdisk = ctx;

	spdk_json_write_named_object_begin(w, "bio");

	spdk_json_write_named_string(w, "filename", fdisk->filename);

	spdk_json_write_object_end(w);

	return 0;
}

static void
bdev_bio_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct file_disk *fdisk = bdev->ctxt;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_bio_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_string(w, "filename", fdisk->filename);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table bio_fn_table = {
	.destruct		= bdev_bio_destruct,
	.submit_request		= bdev_bio_submit_request,
	.io_type_supported	= bdev_bio_io_type_supported,
	.get_io_channel		= bdev_bio_get_io_channel,
	.dump_info_json		= bdev_bio_dump_info_json,
	.write_config_json	= bdev_bio_write_json_config,
};

int
create_bdev_bio(const char * spdk_name, const char * bio_name)
{
	struct file_disk *fdisk;
	uint32_t detected_block_size;
	uint32_t block_size = 0;
	uint64_t disk_size;
	int rc;

	fdisk = calloc(1, sizeof(*fdisk));
	if (!fdisk) {
		SPDK_ERRLOG("Unable to allocate memory for bio backend\n");
		return -ENOMEM;
	}

	fdisk->filename = strdup(bio_name);
	if (!fdisk->filename) {
		rc = -ENOMEM;
		goto error_return;
	}

	rc = bdev_bio_open(fdisk);
	if (rc) {
		SPDK_ERRLOG("Unable to open %s (%s)\n", bio_name, fdisk->filename);
		goto error_return;
	}

	disk_size = bdev_bio_get_size(fdisk);

	fdisk->disk.name = strdup(spdk_name);
	if (!fdisk->disk.name) {
		rc = -ENOMEM;
		goto error_return;
	}
	fdisk->disk.product_name = "BIO disk";
	fdisk->disk.module = &bio_if;

	fdisk->disk.write_cache = 1;

	detected_block_size = bdev_bio_get_blocklen(fdisk);
	if (block_size == 0) {
		/* User did not specify block size - use autodetected block size. */
		if (detected_block_size == 0) {
			SPDK_ERRLOG("Block size could not be auto-detected\n");
			rc = -EINVAL;
			goto error_return;
		}
		fdisk->block_size_override = false;
		block_size = detected_block_size;
	} else {
		if (block_size < detected_block_size) {
			SPDK_ERRLOG("Specified block size %" PRIu32 " is smaller than "
				    "auto-detected block size %" PRIu32 "\n",
				    block_size, detected_block_size);
			rc = -EINVAL;
			goto error_return;
		} else if (detected_block_size != 0 && block_size != detected_block_size) {
			SPDK_WARNLOG("Specified block size %" PRIu32 " does not match "
				     "auto-detected block size %" PRIu32 "\n",
				     block_size, detected_block_size);
		}
		fdisk->block_size_override = true;
	}

	if (block_size < 512) {
		SPDK_ERRLOG("Invalid block size %" PRIu32 " (must be at least 512).\n", block_size);
		rc = -EINVAL;
		goto error_return;
	}

	if (!spdk_u32_is_pow2(block_size)) {
		SPDK_ERRLOG("Invalid block size %" PRIu32 " (must be a power of 2.)\n", block_size);
		rc = -EINVAL;
		goto error_return;
	}

	fdisk->disk.blocklen = block_size;
	fdisk->disk.required_alignment = spdk_u32log2(block_size);

	if (disk_size % fdisk->disk.blocklen != 0) {
		SPDK_ERRLOG("Disk size %" PRIu64 " is not a multiple of block size %" PRIu32 "\n",
			    disk_size, fdisk->disk.blocklen);
		rc = -EINVAL;
		goto error_return;
	}

	fdisk->disk.blockcnt = disk_size / fdisk->disk.blocklen;
	fdisk->disk.ctxt = fdisk;

	fdisk->disk.fn_table = &bio_fn_table;

	spdk_io_device_register(fdisk, bdev_bio_create_cb, bdev_bio_destroy_cb,
				sizeof(struct bdev_bio_io_channel),
				fdisk->disk.name);
	rc = spdk_bdev_register(&fdisk->disk);
	if (rc) {
		SPDK_ERRLOG("Failed spdk_bdev_register(%s, %s)\n", fdisk->disk.name, fdisk->filename);
		spdk_io_device_unregister(fdisk, NULL);
		goto error_return;
	}

	TAILQ_INSERT_TAIL(&g_bio_disk_head, fdisk, link);
	return 0;

error_return:
	bdev_bio_close(fdisk);
	bio_free_disk(fdisk);
	return rc;
}

struct delete_bdev_bio_ctx {
	delete_bdev_bio_complete cb_fn;
	void *cb_arg;
};

static void
bdev_bio_unregister_cb(void *arg, int bdeverrno)
{
	struct delete_bdev_bio_ctx *ctx = arg;

	ctx->cb_fn(ctx->cb_arg, bdeverrno);
	free(ctx);
}

void
bdev_bio_delete(struct spdk_bdev *bdev, delete_bdev_bio_complete cb_fn, void *cb_arg)
{
	struct delete_bdev_bio_ctx *ctx;

	if (!bdev || bdev->module != &bio_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	spdk_bdev_unregister(bdev, bdev_bio_unregister_cb, ctx);
}

static int
bdev_bio_initialize(void)
{
	size_t i;
	struct spdk_conf_section *sp;

	TAILQ_INIT(&g_bio_disk_head);

	sp = spdk_conf_find_section(NULL, "BIO");
	if (!sp) {
		return 0;
	}

	for (i = 0 ; ; i++) {
		int rc;
		const char *file;
		const char *name;

		name = spdk_conf_section_get_nmval(sp, "BIO", i, 0);
		if (!name) {
			break;
		}

		file = spdk_conf_section_get_nmval(sp, "BIO", i, 1);
		if (!file) {
			SPDK_ERRLOG("No bio_name name provided for BIO %s\n", name);
			continue;
		}

		rc = create_bdev_bio(name, file);
		if (rc)
			SPDK_ERRLOG("Unable to create BIO %s, err is %s\n", name, spdk_strerror(-rc));
	}

	return 0;
}

static void
bdev_bio_fini(void)
{
	spdk_io_device_unregister(&bio_if, NULL);
}

static void
bdev_bio_get_spdk_running_config(FILE *fp)
{
	char			*file;
	char			*name;
	struct file_disk	*fdisk;

	fprintf(fp,
		"\n"
		"# Translate from SPDK bdev protocol to kernel bio bdev protocol\n"
		"# (SPDK front-end shim for kernel BIO modules ported to usermode)\n"
		"# bio-dev-name typically begins with '/UMCfuse/dev/'\n"
		"# Format:\n"
		"# BIO <spdk-bdev-name> <bio-dev-name>\n"
		"[BIO]\n");

	TAILQ_FOREACH(fdisk, &g_bio_disk_head, link) {
		file = fdisk->filename;
		name = fdisk->disk.name;
		fprintf(fp, "  BIO %s %s\n", name, file);
	}

	fprintf(fp, "\n");
}

SPDK_LOG_REGISTER_COMPONENT("bio", SPDK_LOG_BIO)
