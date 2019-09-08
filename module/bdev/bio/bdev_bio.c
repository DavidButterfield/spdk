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
#define _impl_name			bio
#define impl_name_str			"bio"
#define IMPL_NAME_STR			"BIO"
#define create_impl_bdev		create_bio_bdev
#define bdev_impl_delete		bdev_bio_delete
#define SPDK_LOG_IMPL			SPDK_LOG_BIO
#include "bdev_bio.h"
typedef delete_bio_bdev_complete 	delete_impl_bdev_complete;
#include <stdlib.h>	/* system(3) */

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

struct bdev_impl_io_channel {
	uint64_t			io_inflight;
};

struct bdev_impl_task {
	struct bio			*bio;
	struct bdev_impl_io_channel	*ch;
	TAILQ_ENTRY(bdev_impl_task)	link;
};

/* bio device */
struct file_disk {
	struct bdev_impl_task	*reset_task;
	struct spdk_poller	*reset_retry_timer;
	struct spdk_bdev	disk;
	char			*filename;	/* bio device name */
	const char		*helper_cmd;
	struct block_device	*bdev;
	TAILQ_ENTRY(file_disk)  link;
	bool			block_size_override;
};

static int bdev_impl_initialize(void);
static void bdev_impl_fini(void);
static void bdev_impl_get_spdk_running_config(FILE *fp);
static TAILQ_HEAD(, file_disk) g_impl_disk_head;

static int
bdev_impl_get_ctx_size(void)
{
	return sizeof(struct bdev_impl_task);
}

static struct spdk_bdev_module impl_if = {
	.name		= impl_name_str,
	.module_init	= bdev_impl_initialize,
	.module_fini	= bdev_impl_fini,
	.config_text	= bdev_impl_get_spdk_running_config,
	.get_ctx_size	= bdev_impl_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(_impl_name, &impl_if)

#define BIO_OPEN_MODE (O_RDWR | O_DIRECT)

static int
bdev_impl_open(struct file_disk *fdisk)
{
	struct block_device * bio_bdev;

	bio_bdev = open_bdev_exclusive(fdisk->filename, BIO_OPEN_MODE, NULL);
	if (IS_ERR_OR_NULL(bio_bdev)) {
		SPDK_ERRLOG("open() failed (file:%s), ret=%ld: %s\n", fdisk->filename,
				PTR_ERR(bio_bdev), spdk_strerror(-PTR_ERR(bio_bdev)));
		return PTR_ERR(bio_bdev);
	}

	fdisk->bdev = bio_bdev;

	return 0;
}

static int
bdev_impl_close(struct file_disk *fdisk)
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

/* Issue callback to our requestor */
static inline void
do_callback(void * arg)
{
	struct bio * bio = arg;
	struct bdev_impl_task * bdev_task = bio->bi_private;
	int status = bio->bi_error ? SPDK_BDEV_IO_STATUS_FAILED : SPDK_BDEV_IO_STATUS_SUCCESS;
	bio_put(bio);
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_task), status);
}

/* Callback from bio implementor */
static void
cmd_done(struct bio * bio, error_t err)
{
	struct bdev_impl_task * bdev_task;
	bio->bi_error = err;

	bdev_task = bio->bi_private;
	bdev_task->ch->io_inflight--;

	/* Defer completion callback to the SPDK thread the request came in on */
	spdk_thread_send_msg(
		spdk_io_channel_get_thread(spdk_io_channel_from_ctx(bdev_task->ch)),
		do_callback, bio);
}

static struct bio *
bdev_bio_alloc_set(struct block_device * bdev, struct iovec * iov, int iovcnt,
					size_t nbytes, off_t offset)
{
	int iovn;
	struct bio * bio;
	unsigned int npage = 2 * iovcnt + (unsigned int)(nbytes / PAGE_SIZE);
	struct page * pages = calloc(npage, sizeof(*pages));
	struct page * page = pages;
	uint64_t dev_size = bdev->bd_inode->i_size;

	assert_imply(nbytes, iov);
	assert_imply(nbytes, iovcnt);
	assert_eq(offset % 512, 0, "unaligned offset on minor %d", bdev->bd_disk->first_minor);
	assert_eq(nbytes % 512, 0, "unaligned nbytes on minor %d", bdev->bd_disk->first_minor);

	if ((uint64_t)offset >= dev_size)
		return NULL;
	if ((uint64_t)offset + nbytes > dev_size)
		nbytes = dev_size - offset;	//XXX right?

	bio = bio_alloc(0, npage);
	bio_set_dev(bio, bdev);
	bio->bi_sector = offset >> 9;
	bio->bi_end_io = cmd_done;

	/* Each iovec entry may comprise multiple pages, but bio wants pages separate (XXX really?) */
	for (iovn = 0; iovn < iovcnt; iovn++) {
		size_t size = iov[iovn].iov_len;
		char * p = iov[iovn].iov_base;
		off_t page_off = offset_in_page(p);
		assert_lt(page_off, PAGE_SIZE);

		/* Fill in the bio page list with the pages of the current iov entry */
		while (size) {
			size_t page_datalen = min((size_t)(PAGE_SIZE - page_off), size);
			assert_lt(page, pages + npage);
			page->vaddr = (void *)((uintptr_t)p & PAGE_MASK);
			page->order = 1;
			bio_add_page(bio, page, (unsigned int)page_datalen, (unsigned int)page_off);
			p += page_datalen;
			size -= page_datalen;
			page++;
			page_off = 0;	/* non-first pages of each iov entry start at offset zero */
		}
	}

	assert_ge(bio->bi_size, nbytes);
	return bio;
}

static int64_t
bdev_impl_readv(struct file_disk *fdisk, struct spdk_io_channel *ch,
	       struct bdev_impl_task *bdev_task,
	       struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct bdev_impl_io_channel *impl_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	struct bio * bio = bdev_bio_alloc_set(fdisk->bdev, iov, iovcnt, nbytes, offset);
	if (!bio)
		return -1;

	bio->bi_private = bdev_task;

	bdev_task->ch = impl_ch;

	SPDK_DEBUGLOG(SPDK_LOG_IMPL, "read %d iovs size %lu at off: %#lx\n",
		      iovcnt, nbytes, offset);

	rc = submit_bio(READ, bio);
	if (rc < 0) {
		if (rc == -ENOMEM) {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_task), SPDK_BDEV_IO_STATUS_NOMEM);
			SPDK_DEBUGLOG(SPDK_LOG_IMPL, "%s: submit_bio(READ) returned ENOMEM\n", __func__);
		} else {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_task), SPDK_BDEV_IO_STATUS_FAILED);
			SPDK_ERRLOG("%s: submit_bio(READ) returned %d\n", __func__, rc);
		}
		//XXXXX free the page array
		return -1;
	}
	impl_ch->io_inflight++;
	return nbytes;
}

static int64_t
bdev_impl_writev(struct file_disk *fdisk, struct spdk_io_channel *ch,
		struct bdev_impl_task *bdev_task,
		struct iovec *iov, int iovcnt, size_t nbytes, uint64_t offset)
{
	struct bdev_impl_io_channel *impl_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	struct bio * bio = bdev_bio_alloc_set(fdisk->bdev, iov, iovcnt, nbytes, offset);
	bio->bi_private = bdev_task;

	bdev_task->ch = impl_ch;

	SPDK_DEBUGLOG(SPDK_LOG_IMPL, "write %d iovs size %lu at off: %#lx\n",
		      iovcnt, nbytes, offset);

	rc = submit_bio(WRITE, bio);
	if (rc < 0) {
		if (rc == -ENOMEM) {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_task), SPDK_BDEV_IO_STATUS_NOMEM);
			SPDK_DEBUGLOG(SPDK_LOG_IMPL, "%s: submit_bio(WRITE) returned ENOMEM\n", __func__);
		} else {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_task), SPDK_BDEV_IO_STATUS_FAILED);
			SPDK_ERRLOG("%s: submit_bio(WRITE) returned %d\n", __func__, rc);
		}
		//XXXXX free the page array
		return -1;
	}
	impl_ch->io_inflight++;
	return nbytes;
}

static int
bdev_impl_flush(struct file_disk *fdisk, struct spdk_io_channel *ch, struct bdev_impl_task *bdev_task)
{
	struct bdev_impl_io_channel *impl_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	struct bio * bio = bdev_bio_alloc_set(fdisk->bdev, NULL, 0, 0, 0);
	bio->bi_private = bdev_task;

	bdev_task->ch = impl_ch;

	SPDK_DEBUGLOG(SPDK_LOG_IMPL, "flush\n");

	rc = submit_bio(WRITE|REQ_BARRIER, bio);	//XXX check this!
	if (rc < 0) {
		if (rc == -ENOMEM) {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_task), SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_task), SPDK_BDEV_IO_STATUS_FAILED);
			SPDK_ERRLOG("%s: submit_bio(FLUSH) returned %d\n", __func__, rc);
		}
		return -1;
	}
	impl_ch->io_inflight++;
	return 0;
}

static uint64_t
bdev_impl_get_size(struct file_disk *fdisk)
{
	return fdisk->bdev->bd_inode->i_size;
}

static uint32_t
bdev_impl_get_blocklen(struct file_disk *fdisk)
{
	return 1u << fdisk->bdev->bd_inode->i_blkbits;
}

/******************************************************************************/

static void impl_free_disk(struct file_disk *fdisk)
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
bdev_impl_destruct(void *ctx)
{
	struct file_disk *fdisk = ctx;
	int rc = 0;

	TAILQ_REMOVE(&g_impl_disk_head, fdisk, link);
	rc = bdev_impl_close(fdisk);
	if (rc < 0) {
		SPDK_ERRLOG("bdev_impl_close() failed\n");
	}
	spdk_io_device_unregister(fdisk, NULL);
	impl_free_disk(fdisk);
	return rc;
}

static void
_bdev_impl_get_io_inflight(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct bdev_impl_io_channel *impl_ch = spdk_io_channel_get_ctx(ch);

	if (impl_ch->io_inflight) {
		spdk_for_each_channel_continue(i, -1);
		return;
	}

	spdk_for_each_channel_continue(i, 0);
}

static int bdev_impl_reset_retry_timer(void *arg);

static void
_bdev_impl_get_io_inflight_done(struct spdk_io_channel_iter *i, int status)
{
	struct file_disk *fdisk = spdk_io_channel_iter_get_ctx(i);

	if (status == -1) {
		fdisk->reset_retry_timer = spdk_poller_register(bdev_impl_reset_retry_timer, fdisk, 500);
		return;
	}

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(fdisk->reset_task), SPDK_BDEV_IO_STATUS_SUCCESS);
}

static int
bdev_impl_reset_retry_timer(void *arg)
{
	struct file_disk *fdisk = arg;

	if (fdisk->reset_retry_timer) {
		spdk_poller_unregister(&fdisk->reset_retry_timer);
	}

	spdk_for_each_channel(fdisk,
			      _bdev_impl_get_io_inflight,
			      fdisk,
			      _bdev_impl_get_io_inflight_done);

	return -1;
}

static void
bdev_impl_reset(struct file_disk *fdisk, struct bdev_impl_task *bdev_task)
{
	fdisk->reset_task = bdev_task;

	bdev_impl_reset_retry_timer(fdisk);
}

static void
bdev_impl_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		    bool success)
{
	if (!success) {
		SPDK_ERRLOG("spdk_bdev_io_get_buf failed\n");
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		bdev_impl_readv((struct file_disk *)bdev_io->bdev->ctxt,
			       ch,
			       (struct bdev_impl_task *)bdev_io->driver_ctx,
			       bdev_io->u.bdev.iovs,
			       bdev_io->u.bdev.iovcnt,
			       bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
			       bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		bdev_impl_writev((struct file_disk *)bdev_io->bdev->ctxt,
				ch,
				(struct bdev_impl_task *)bdev_io->driver_ctx,
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

static int _bdev_impl_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	/* Read and write operations must be performed on buffers aligned to
	 * bdev->required_alignment. If user specified unaligned buffers,
	 * get the aligned buffer from the pool by calling spdk_bdev_io_get_buf. */
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		spdk_bdev_io_get_buf(bdev_io, bdev_impl_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		bdev_impl_flush((struct file_disk *)bdev_io->bdev->ctxt,
				ch,
			       (struct bdev_impl_task *)bdev_io->driver_ctx);
		return 0;

	case SPDK_BDEV_IO_TYPE_RESET:
		bdev_impl_reset((struct file_disk *)bdev_io->bdev->ctxt,
			       (struct bdev_impl_task *)bdev_io->driver_ctx);
		return 0;
	default:
		return -1;
	}
}

static void bdev_impl_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if (_bdev_impl_submit_request(ch, bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
bdev_impl_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
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
bdev_impl_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
bdev_impl_destroy_cb(void *io_device, void *ctx_buf)
{
}

static struct spdk_io_channel *
bdev_impl_get_io_channel(void *ctx)
{
	struct file_disk *fdisk = ctx;

	return spdk_get_io_channel(fdisk);
}


static int
bdev_impl_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct file_disk *fdisk = ctx;

	spdk_json_write_named_object_begin(w, impl_name_str);

	spdk_json_write_named_string(w, "filename", fdisk->filename);

	spdk_json_write_object_end(w);

	return 0;
}

static void
bdev_impl_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct file_disk *fdisk = bdev->ctxt;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_impl_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_string(w, "filename", fdisk->filename);
	if (fdisk->helper_cmd)
		spdk_json_write_named_string(w, "helper_cmd", fdisk->helper_cmd);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table impl_fn_table = {
	.destruct		= bdev_impl_destruct,
	.submit_request		= bdev_impl_submit_request,
	.io_type_supported	= bdev_impl_io_type_supported,
	.get_io_channel		= bdev_impl_get_io_channel,
	.dump_info_json		= bdev_impl_dump_info_json,
	.write_config_json	= bdev_impl_write_json_config,
};

int
create_impl_bdev(const char * spdk_name, const char * impl_name, const char * helper_cmd)
{
	struct file_disk *fdisk;
	uint32_t detected_block_size;
	uint32_t block_size = 0;
	uint64_t disk_size;
	int rc;

	fdisk = calloc(1, sizeof(*fdisk));
	if (!fdisk) {
		SPDK_ERRLOG("Unable to allocate memory for "impl_name_str" backend\n");
		return -ENOMEM;
	}

	fdisk->helper_cmd = helper_cmd;
	fdisk->filename = strdup(impl_name);
	if (!fdisk->filename) {
		rc = -ENOMEM;
		goto error_return;
	}

	rc = bdev_impl_open(fdisk);
	if (rc) {
		SPDK_ERRLOG("Unable to open %s (%s)\n", impl_name, fdisk->filename);
		goto error_return;
	}

	disk_size = bdev_impl_get_size(fdisk);

	fdisk->disk.name = strdup(spdk_name);
	if (!fdisk->disk.name) {
		rc = -ENOMEM;
		goto error_return;
	}
	fdisk->disk.product_name = IMPL_NAME_STR" disk";
	fdisk->disk.module = &impl_if;

	fdisk->disk.write_cache = 1;

	detected_block_size = bdev_impl_get_blocklen(fdisk);
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

	fdisk->disk.fn_table = &impl_fn_table;

	spdk_io_device_register(fdisk, bdev_impl_create_cb, bdev_impl_destroy_cb,
				sizeof(struct bdev_impl_io_channel),
				fdisk->disk.name);
	rc = spdk_bdev_register(&fdisk->disk);
	if (rc) {
		SPDK_ERRLOG("Failed spdk_bdev_register(%s, %s)\n", fdisk->disk.name, fdisk->filename);
		spdk_io_device_unregister(fdisk, NULL);
		goto error_return;
	}

	TAILQ_INSERT_TAIL(&g_impl_disk_head, fdisk, link);
	return 0;

error_return:
	bdev_impl_close(fdisk);
	impl_free_disk(fdisk);
	return rc;
}

struct delete_impl_bdev_ctx {
	delete_impl_bdev_complete cb_fn;
	void *cb_arg;
};

static void
impl_bdev_unregister_cb(void *arg, int bdeverrno)
{
	struct delete_impl_bdev_ctx *ctx = arg;

	ctx->cb_fn(ctx->cb_arg, bdeverrno);
	free(ctx);
}

void
bdev_impl_delete(struct spdk_bdev *bdev, delete_impl_bdev_complete cb_fn, void *cb_arg)
{
	struct delete_impl_bdev_ctx *ctx;

	if (!bdev || bdev->module != &impl_if) {
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
	spdk_bdev_unregister(bdev, impl_bdev_unregister_cb, ctx);
}

static int
bdev_impl_initialize(void)
{
	size_t i;
	struct spdk_conf_section *sp;

	TAILQ_INIT(&g_impl_disk_head);

	sp = spdk_conf_find_section(NULL, IMPL_NAME_STR);
	if (!sp) {
		return 0;
	}

	i = 0;
	while (true) {
		int rc;
		const char *file;
		const char *name;
		const char *helper;
		char *cmd = NULL;

		name = spdk_conf_section_get_nmval(sp, IMPL_NAME_STR, i, 0);
		if (!name) {
			break;
		}

		file = spdk_conf_section_get_nmval(sp, IMPL_NAME_STR, i, 1);
		if (!file) {
			SPDK_ERRLOG("No bio_name name provided for "IMPL_NAME_STR" %s\n", name);
			i++;
			continue;
		}

		/* Helper can issue commands to bio module via its native admin interface */
		//XXX probably a better way to just get the rest of the line?
		helper = spdk_conf_section_get_nmval(sp, IMPL_NAME_STR, i, 2);
		if (helper) {
			const char * arg1 = spdk_conf_section_get_nmval(sp, IMPL_NAME_STR, i, 3);
			const char * arg2 = spdk_conf_section_get_nmval(sp, IMPL_NAME_STR, i, 4);
			if (asprintf(&cmd, "%s %s %s", helper, arg1?:"", arg2?:"") > 0) {
				SPDK_NOTICELOG("Invoking helper: %s\n", cmd);
				rc = system(cmd);
				if (rc)
					SPDK_ERRLOG("%s returned 0x%x\n", cmd, rc);
			}
		}

		rc = create_impl_bdev(name, file, cmd);
		if (rc) {
			SPDK_ERRLOG("Unable to create "IMPL_NAME_STR" %s, err is %s\n", name, spdk_strerror(-rc));
			i++;
			continue;
		}

		i++;
	}

	return 0;
}

static void
bdev_impl_fini(void)
{
	spdk_io_device_unregister(&impl_if, NULL);
}

static void
bdev_impl_get_spdk_running_config(FILE *fp)
{
	char			*file;
	char			*name;
	const char		*helper_cmd;
	struct file_disk	*fdisk;

	fprintf(fp,
		"\n"
		"# Translate from SPDK BDEV protocol to kernel BIO protocol\n"
		"# (SPDK front-end shim for kernel BIO modules ported to usermode)\n"
		"# The helper allows external configuration of the BIO module\n"
		"# Format:\n"
		"# BIO <spdk-bdev-name> <bio-dev-name> [ helper [ arg1 [ arg2 ] ] ]\n"
		"["IMPL_NAME_STR"]\n");

	TAILQ_FOREACH(fdisk, &g_impl_disk_head, link) {
		file = fdisk->filename;
		name = fdisk->disk.name;
		helper_cmd = fdisk->helper_cmd;
		fprintf(fp, "  "IMPL_NAME_STR" %s %s", name, file);
		if (helper_cmd)
			fprintf(fp, " %s", helper_cmd);
		fprintf(fp, "\n");
	}

	fprintf(fp, "\n");
}

SPDK_LOG_REGISTER_COMPONENT(impl_name_str, SPDK_LOG_BIO)
