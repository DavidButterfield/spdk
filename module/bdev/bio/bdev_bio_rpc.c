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

/* Adapted from bdev_aio.c */
#define IMPL_NAME_STR			"bio"
#include "bdev_bio.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk_internal/log.h"

struct rpc_construct_impl {
	char *name;
	char *filename;
//	uint32_t block_size;
};

static void
free_rpc_construct_impl(struct rpc_construct_impl *req)
{
	free(req->name);
	free(req->filename);
}

static const struct spdk_json_object_decoder rpc_construct_impl_decoders[] = {
	{"name", offsetof(struct rpc_construct_impl, name), spdk_json_decode_string},
	{"filename", offsetof(struct rpc_construct_impl, filename), spdk_json_decode_string},
//	{"block_size", offsetof(struct rpc_construct_aio, block_size), spdk_json_decode_uint32, true},
};

static void
spdk_rpc_bdev_impl_create(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_construct_impl req = {};
	struct spdk_json_write_ctx *w;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_construct_impl_decoders,
				    SPDK_COUNTOF(rpc_construct_impl_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	// rc = create_bio_bdev(req.name, req.filename, req.block_size);
	rc = create_bio_bdev(req.name, req.filename);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}


	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, req.name);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_construct_impl(&req);
}
SPDK_RPC_REGISTER("bdev_"IMPL_NAME_STR"_create", spdk_rpc_bdev_impl_create, SPDK_RPC_RUNTIME)

struct rpc_delete_impl {
	char *name;
};

static void
free_rpc_delete_impl(struct rpc_delete_impl *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_delete_impl_decoders[] = {
	{"name", offsetof(struct rpc_delete_impl, name), spdk_json_decode_string},
};

static void
_spdk_rpc_bdev_impl_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_bool(w, bdeverrno == 0);
	spdk_jsonrpc_end_result(request, w);
}

static void
spdk_rpc_bdev_impl_delete(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_delete_impl req = {NULL};
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_delete_impl_decoders,
				    SPDK_COUNTOF(rpc_delete_impl_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	bdev_bio_delete(bdev, _spdk_rpc_bdev_impl_delete_cb, request);

	free_rpc_delete_impl(&req);

	return;

cleanup:
	free_rpc_delete_impl(&req);
}
SPDK_RPC_REGISTER("bdev_"IMPL_NAME_STR"_delete", spdk_rpc_bdev_impl_delete, SPDK_RPC_RUNTIME)
