/* bdev_tcmur.c -- adapted from bdev_aio.c */
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
#include "spdk/config.h" 	/* for SPDK_CONFIG_DRBD */

#define impl_name_str			"tcmur"
#define IMPL_NAME_STR			"TCMUR"
#define create_impl_bdev		create_tcmur_bdev
#define bdev_impl_delete		bdev_tcmur_delete
#define SPDK_LOG_IMPL			SPDK_LOG_TCMUR

extern int UMC_init(const char * procname);
extern int UMC_exit(void);
extern void DRBD_init(void);

/* Hacks to work around kernel/user #include conflicts */
#define  NO_UMC_SOCKETS
#include "libtcmur.h"
#undef htonl
#undef htons
#undef ntohl
#undef ntohs
#include "bdev_tcmur.h"
#ifdef SPDK_CONFIG_DRBD
#include "mtelib.h"
#endif

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
	struct libtcmur_task		tcmur_task;
	struct bdev_impl_io_channel	*ch;
	struct iovec			*iov;
	int				sts;	/* return status from tcmur handler */
	TAILQ_ENTRY(bdev_impl_task)	link;
};

/* tcmur disk */
struct file_disk {
	struct bdev_impl_task	*reset_task;
	struct spdk_poller	*reset_retry_timer;
	struct spdk_bdev	disk;
	char			*cfgstr;	    /* tcmur config string */
	int			fd;		    /* tcmur minor number */
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

SPDK_BDEV_MODULE_REGISTER(tcmur, &impl_if)

/******************************************************************************/
/* call the tcmur operations */

static int
bdev_impl_open(struct file_disk *disk)
{
	int fd;

	fd = tcmur_open(disk->disk.name, O_RDWR | O_DIRECT);
	if (fd < 0) {
		/* Try without O_DIRECT */
		fd = tcmur_open(disk->disk.name, O_RDWR);
		if (fd < 0) {
			SPDK_ERRLOG("open() failed (file:%s), errno %d: %s\n",
				    disk->disk.name, errno, spdk_strerror(errno));
			disk->fd = -1;
			return -1;
		}
	}

	disk->fd = fd;

	return 0;
}

static int
bdev_impl_close(struct file_disk *disk)
{
	int rc;

	if (disk->fd == -1) {
		return 0;
	}

	rc = tcmur_close(disk->fd);
	if (rc < 0) {
		SPDK_ERRLOG("close() failed (fd=%d), errno %d: %s\n",
			    disk->fd, errno, spdk_strerror(errno));
		return -1;
	}

	disk->fd = -1;

	return 0;
}

/* Issue callback to our requestor */
static inline void
do_callback(void * arg)
{
	struct tcmur_cmd * cmd = arg;
	int status;
	struct bdev_impl_task * bdev_task;
	bdev_task = container_of(cmd, struct bdev_impl_task, tcmur_task.tcmur_cmd);

	status = bdev_task->sts == TCMU_STS_OK          ? SPDK_BDEV_IO_STATUS_SUCCESS :
		 bdev_task->sts == TCMU_STS_NO_RESOURCE ? SPDK_BDEV_IO_STATUS_NOMEM :
					                  SPDK_BDEV_IO_STATUS_FAILED;

	bdev_task->ch->io_inflight--;

	SPDK_DEBUGLOG(SPDK_LOG_IMPL, "%s: %p SPDK status %d\n", __func__, bdev_task, status);
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_task), status);
}

/* Completion callback from libtcmur, possibly on a non-SPDK thread */
static void
cmd_done(struct tcmu_device * tcmu_dev, struct tcmur_cmd * cmd, int sts)
{
	struct bdev_impl_task * bdev_task =
		container_of(cmd, struct bdev_impl_task, tcmur_task.tcmur_cmd);
	struct spdk_io_channel *ch = spdk_io_channel_from_ctx(bdev_task->ch);
	struct spdk_thread * thr = spdk_io_channel_get_thread(ch);
	bdev_task->sts = sts;

#ifdef TOO_NOISY
	if (thr == spdk_get_thread())	/* on the requesting SPDK thread */
#elif USE_UMC
	if (current == NULL)		/* on some SPDK thread */
#else
	if (0)
#endif
		do_callback(cmd);
	else {
		/* Defer completion callback to the SPDK thread the request came in on */
		SPDK_DEBUGLOG(SPDK_LOG_IMPL, "%s: %p handoff TCMUR status %d\n", __func__, bdev_task, sts);
		spdk_thread_send_msg(thr, do_callback, cmd);
	}

	if (bdev_task->iov)
		free(bdev_task->iov);
}

static void
bdev_impl_readv(struct file_disk *fdisk, struct spdk_io_channel *ch,
	       struct bdev_impl_task *bdev_task,
	       struct iovec *iov_in, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct bdev_impl_io_channel *impl_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	//XXX Somebody above (iscsi?) cannot tolerate consumption of the
	//    passed iovec, which is done by, e.g., tcmu_memcpy_into_iovec()
	struct iovec * iov = calloc(iovcnt, sizeof(*iov));
	memcpy(iov, iov_in, iovcnt * sizeof(*iov));

	SPDK_DEBUGLOG(SPDK_LOG_IMPL, "read(%d) %p %d iovs size %lu to off: %#lx\n",
		      fdisk->fd, bdev_task, iovcnt, nbytes, offset);

	bdev_task->iov = iov;
	bdev_task->ch = impl_ch;
	bdev_task->tcmur_task.tcmur_cmd.done = cmd_done;

	impl_ch->io_inflight++;

	rc = tcmur_read(fdisk->fd, &bdev_task->tcmur_task, iov, iovcnt, nbytes, offset);
	if (rc < 0) {
		if (rc == -ENOMEM) {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_task), SPDK_BDEV_IO_STATUS_NOMEM);
			SPDK_DEBUGLOG(SPDK_LOG_IMPL, "%s: tcmur_read %p returned ENOMEM\n", __func__, bdev_task);
		} else {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_task), SPDK_BDEV_IO_STATUS_FAILED);
			SPDK_ERRLOG("%s: tcmur_read %p returned ERROR %d\n", __func__, bdev_task, rc);
		}
		impl_ch->io_inflight--;
		free(iov);
	}
}

static void
bdev_impl_writev(struct file_disk *fdisk, struct spdk_io_channel *ch,
		struct bdev_impl_task *bdev_task,
		struct iovec *iov_in, int iovcnt, size_t nbytes, uint64_t offset)
{
	struct bdev_impl_io_channel *impl_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	struct iovec * iov = calloc(iovcnt, sizeof(*iov));
	memcpy(iov, iov_in, iovcnt * sizeof(*iov));

	SPDK_DEBUGLOG(SPDK_LOG_IMPL, "write(%d) %p %d iovs size %lu from off: %#lx\n",
		      fdisk->fd, bdev_task, iovcnt, nbytes, offset);

	bdev_task->iov = iov;
	bdev_task->ch = impl_ch;
	bdev_task->tcmur_task.tcmur_cmd.done = cmd_done;

	impl_ch->io_inflight++;

	rc = tcmur_write(fdisk->fd, &bdev_task->tcmur_task, iov, iovcnt, nbytes, offset);
	if (rc < 0) {
		impl_ch->io_inflight--;
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(SPDK_LOG_IMPL, "%s: tcmur_write %p returned ENOMEM\n", __func__, bdev_task);
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_task), SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_task), SPDK_BDEV_IO_STATUS_FAILED);
			SPDK_ERRLOG("%s: tcmur_write %p returned ERROR %d\n", __func__, bdev_task, rc);
		}
		free(iov);
	}
}

static void
bdev_impl_flush(struct file_disk *fdisk, struct spdk_io_channel *ch, struct bdev_impl_task *bdev_task)
{
	struct bdev_impl_io_channel *impl_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_IMPL, "flush(%d) %p\n", fdisk->fd, bdev_task);

	bdev_task->iov = NULL;
	bdev_task->ch = impl_ch;
	bdev_task->tcmur_task.tcmur_cmd.done = cmd_done;

	impl_ch->io_inflight++;

	rc = tcmur_flush(fdisk->fd, &bdev_task->tcmur_task);
	if (rc < 0) {
		impl_ch->io_inflight--;
		if (rc == -ENOMEM) {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_task), SPDK_BDEV_IO_STATUS_NOMEM);
			SPDK_DEBUGLOG(SPDK_LOG_IMPL, "%s: tcmur_flush %p returned ENOMEM\n", __func__, bdev_task);
		} else {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_task), SPDK_BDEV_IO_STATUS_FAILED);
			SPDK_ERRLOG("%s: tcmur_flush %p returned ERROR %d\n", __func__, bdev_task, rc);
		}
	}
}

static uint64_t
bdev_impl_get_size(struct file_disk *fdisk)
{
	return tcmur_get_size(fdisk->fd);
}

static uint32_t
bdev_impl_get_blocklen(struct file_disk *fdisk)
{
	return tcmur_get_block_size(fdisk->fd);
}

/******************************************************************************/

static void impl_free_disk(struct file_disk *fdisk)
{
	if (fdisk == NULL) {
		return;
	}
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

	//XXX what info is supposed to go here?
	spdk_json_write_named_string(w, "cfgstr", fdisk->cfgstr);

	spdk_json_write_object_end(w);

	return 0;
}

static void
bdev_impl_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct file_disk *fdisk = bdev->ctxt;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_tcmur_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_uint32(w, "minor", fdisk->fd);
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_string(w, "cfgstr", fdisk->cfgstr);
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
create_impl_bdev(const char * name, int minor, const char * cfgstr)
{
	struct file_disk *fdisk;
	uint32_t detected_block_size;
	uint32_t block_size = 0;
	uint64_t disk_size;
	int rc;

	if (*cfgstr != '/') {
		SPDK_ERRLOG("config string '%s' does not start with '/'\n", cfgstr);
		return -EINVAL;
	}

	/* Load the tcmu-runner handler */
	const char * p = cfgstr + 1;
	const char * q = strchrnul(p, '/');
	char subtype[q-p+1];
	memcpy(subtype, p, q-p);
	subtype[sizeof(subtype)-1] = '\0';
	rc = tcmur_handler_load(subtype);
	if (rc && rc != -EEXIST) {
		SPDK_ERRLOG("Unable to load handler for subtype '%s', rc=%d\n",
			    subtype, rc);
		return rc;
	}

	/* Add the tcmu-runner device instance */
	rc = tcmur_device_add(minor, name, cfgstr);
	if (rc) {
		if (rc == -ENODEV)
		    SPDK_ERRLOG("minor number %d out of range\n", minor);
		else if (rc == -EBUSY)
		    SPDK_ERRLOG("minor number %d already in use\n", minor);
		return rc;
	}

	fdisk = calloc(1, sizeof(*fdisk));
	if (!fdisk) {
		SPDK_ERRLOG("Unable to allocate memory for "impl_name_str" backend\n");
		return -ENOMEM;
	}

	fdisk->cfgstr = strdup(cfgstr);
	if (!fdisk->cfgstr) {
		rc = -ENOMEM;
		goto error_free;
	}

	fdisk->disk.name = strdup(name);
	if (!fdisk->disk.name) {
		rc = -ENOMEM;
		goto error_free;
	}

	fdisk->disk.product_name = IMPL_NAME_STR" disk";
	fdisk->disk.module = &impl_if;
	fdisk->disk.write_cache = 1;

	if (bdev_impl_open(fdisk)) {
		SPDK_ERRLOG("Unable to open %s. fd: %d errno: %d\n", name, fdisk->fd, errno);
		rc = -errno;
		goto error_free;
	}

	disk_size = bdev_impl_get_size(fdisk);
	detected_block_size = bdev_impl_get_blocklen(fdisk);

	if (block_size == 0) {
		/* User did not specify block size - use autodetected block size. */
		if (detected_block_size == 0) {
			SPDK_ERRLOG("Block size could not be auto-detected\n");
			rc = -EINVAL;
			goto error_close;
		}
		fdisk->block_size_override = false;
		block_size = detected_block_size;
	} else {
		if (block_size < detected_block_size) {
			SPDK_ERRLOG("Specified block size %" PRIu32 " is smaller than "
				    "auto-detected block size %" PRIu32 "\n",
				    block_size, detected_block_size);
			rc = -EINVAL;
			goto error_close;
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
		goto error_close;
	}

	if (!spdk_u32_is_pow2(block_size)) {
		SPDK_ERRLOG("Invalid block size %" PRIu32 " (must be a power of 2.)\n", block_size);
		rc = -EINVAL;
		goto error_close;
	}

	fdisk->disk.blocklen = block_size;
	fdisk->disk.required_alignment = 0;	//512	//XXX spdk_u32log2(block_size);

	if (disk_size % fdisk->disk.blocklen != 0) {
		SPDK_ERRLOG("Disk size %" PRIu64 " is not a multiple of block size %" PRIu32 "\n",
			    disk_size, fdisk->disk.blocklen);
		rc = -EINVAL;
		goto error_close;
	}

	fdisk->disk.blockcnt = disk_size / fdisk->disk.blocklen;
	fdisk->disk.ctxt = fdisk;

	fdisk->disk.fn_table = &impl_fn_table;

	spdk_io_device_register(fdisk, bdev_impl_create_cb, bdev_impl_destroy_cb,
				sizeof(struct bdev_impl_io_channel),
				fdisk->disk.name);
	rc = spdk_bdev_register(&fdisk->disk);
	if (rc) {
		SPDK_ERRLOG("Failed spdk_bdev_register(%s)\n", fdisk->disk.name);
		spdk_io_device_unregister(fdisk, NULL);
		goto error_close;
	}

	TAILQ_INSERT_TAIL(&g_impl_disk_head, fdisk, link);
	return 0;

error_close:
	bdev_impl_close(fdisk);
error_free:
	impl_free_disk(fdisk);
	return rc;
}

struct delete_impl_bdev_ctx {
	delete_tcmur_bdev_complete cb_fn;
	void *cb_arg;
	int tcmur_minor;
};

static void
impl_bdev_unregister_cb(void *arg, int bdeverrno)
{
	struct delete_impl_bdev_ctx *ctx = arg;

	ctx->cb_fn(ctx->cb_arg, bdeverrno);
	tcmur_device_remove(ctx->tcmur_minor);
	free(ctx);
}

void
bdev_impl_delete(struct spdk_bdev *bdev, delete_tcmur_bdev_complete cb_fn, void *cb_arg)
{
	struct delete_impl_bdev_ctx *ctx;
	struct file_disk *fdisk = bdev->ctxt;

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
	ctx->tcmur_minor = fdisk->fd;
	spdk_bdev_unregister(bdev, impl_bdev_unregister_cb, ctx);
}

static void *
do_UMC_init(void * arg)
{
#ifdef SPDK_CONFIG_DRBD
	int rc;

	/* Initialize the multithreaded engine used by UMC */
	sys_service_set(MTE_sys_service_get());
	sys_service_init(NULL);

	/* Initialize usermode-compatibility for kernel modules */
	rc = UMC_init("/UMCfuse");	/* mount point for fuse tree */
	if (rc) {
		SPDK_ERRLOG("UMC_init() returned %d\n", rc);
		return ERR_PTR(rc);
	}

	if (!getenv("DISABLE_DRBD")) {
		DRBD_init();
	}
#endif

	return 0;
}

static int
bdev_impl_initialize(void)
{
	size_t i;
	struct spdk_conf_section *sp;

	void * ret = spdk_call_unaffinitized(do_UMC_init, NULL);
	if ((uintptr_t)ret)
		return (uintptr_t)ret;

	TAILQ_INIT(&g_impl_disk_head);

	sp = spdk_conf_find_section(NULL, IMPL_NAME_STR);
	if (!sp) {
		return 0;
	}

	i = 0;
	while (true) {
		const char *name;
		const char *cfg_str;
		const char *minor_str;
		int32_t minor;
		int rc;

		minor_str = spdk_conf_section_get_nmval(sp, IMPL_NAME_STR, i, 0);
		if (!minor_str) {
			break;
		}
		minor = spdk_strtol(minor_str, 10);
		if (minor < 0) {
			SPDK_ERRLOG("Invalid minor number %s for "IMPL_NAME_STR"\n", minor_str);
			i++;
			continue;
		}

		name = spdk_conf_section_get_nmval(sp, IMPL_NAME_STR, i, 1);
		if (!name) {
			SPDK_ERRLOG("No name provided for "IMPL_NAME_STR" minor %d\n", minor);
			i++;
			continue;
		}

		cfg_str = spdk_conf_section_get_nmval(sp, IMPL_NAME_STR, i, 2);
		if (!cfg_str) {
			SPDK_ERRLOG("No config string provided for "IMPL_NAME_STR" minor %d (%s)\n", minor, name);
			i++;
			continue;
		}

		rc = create_impl_bdev(name, minor, cfg_str);
		if (rc) {
			SPDK_ERRLOG("Unable to create "IMPL_NAME_STR" minor %d, err is %s\n", minor, spdk_strerror(-rc));
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
	//XXX UMC_exit();
}

static void
bdev_impl_get_spdk_running_config(FILE *fp)
{
	char			*name;
	char			*cfgstr;
	struct file_disk	*fdisk;
	int			minor;

	fprintf(fp,
		"\n"
		"# Format for tcmu-runner handler devices:\n"
		"#	TCMUR <tcmur-minor-number> <dev-name> <tcmur-cfg-str>\n"
		"# tcmur-minor-number should be less than 256 and unique across TCMUR instances.\n"
		"# dev-name is arbitrary but should be unique across instances.\n"
		"# tcmu-runner handler name is derived from the first segment of tcmur-cfg-str.\n"
		"["IMPL_NAME_STR"]\n");

	TAILQ_FOREACH(fdisk, &g_impl_disk_head, link) {
		minor = fdisk->fd;
		name = fdisk->disk.name;
		cfgstr = fdisk->cfgstr;
		fprintf(fp, "  "IMPL_NAME_STR" %d %s %s\n", minor, name, cfgstr);
	}

	fprintf(fp, "\n");
}

SPDK_LOG_REGISTER_COMPONENT(impl_name_str, SPDK_LOG_TCMUR)
