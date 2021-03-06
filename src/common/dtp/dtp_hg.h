/**
 * (C) Copyright 2016 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */
/**
 * This file is part of daos_transport. It is the header file of bridging to
 * mercury.
 */
#ifndef __DTP_MERCURY_H__
#define __DTP_MERCURY_H__

#include <daos/list.h>

#include <mercury.h>
#include <mercury_types.h>
#include <mercury_macros.h>
#include <mercury_proc.h>
#include <mercury_proc_string.h>
#include <na.h>
#include <na_error.h>

/** change to 0 to disable the low-level unpack */
#define DTP_HG_LOWLEVEL_UNPACK	(1)

/** the shared HG RPC ID used for all DAOS opc */
#define DTP_HG_RPCID	(0xDA036868)

struct dtp_rpc_priv;
struct dtp_common_hdr;

/** HG context */
struct dtp_hg_context {
	bool			dhc_shared_na; /* flag for shared na_class */
	na_class_t		*dhc_nacla; /* NA class */
	hg_class_t		*dhc_hgcla; /* HG class */
	hg_context_t		*dhc_hgctx; /* HG context */
	hg_class_t		*dhc_bulkcla; /* bulk class */
	hg_context_t		*dhc_bulkctx; /* bulk context */
};

/** HG level global data */
struct dtp_hg_gdata {
	na_class_t		*dhg_nacla; /* NA class */
	hg_class_t		*dhg_hgcla; /* HG class */
};

/* dtp_hg.c */
int dtp_hg_init(dtp_phy_addr_t *addr, bool server);
int dtp_hg_fini();
int dtp_hg_ctx_init(struct dtp_hg_context *hg_ctx, int idx);
int dtp_hg_ctx_fini(struct dtp_hg_context *hg_ctx);
int dtp_hg_req_create(struct dtp_hg_context *hg_ctx, dtp_endpoint_t tgt_ep,
		      struct dtp_rpc_priv *rpc_priv);
int dtp_hg_req_destroy(struct dtp_rpc_priv *rpc_priv);
int dtp_hg_req_send(struct dtp_rpc_priv *rpc_priv);
int dtp_hg_reply_send(struct dtp_rpc_priv *rpc_priv);
int dtp_hg_req_cancel(struct dtp_rpc_priv *rpc_priv);
int dtp_hg_progress(struct dtp_hg_context *hg_ctx, int64_t timeout);

int dtp_rpc_handler_common(hg_handle_t hg_hdl);

/* dtp_hg_proc.c */
int dtp_proc_common_hdr(dtp_proc_t proc, struct dtp_common_hdr *hdr);
int dtp_hg_unpack_header(struct dtp_rpc_priv *rpc_priv, dtp_proc_t *proc);
void dtp_hg_unpack_cleanup(dtp_proc_t proc);
int dtp_proc_internal(struct drf_field *drf, dtp_proc_t proc, void *data);
int dtp_proc_input(struct dtp_rpc_priv *rpc_priv, dtp_proc_t proc);
int dtp_proc_output(struct dtp_rpc_priv *rpc_priv, dtp_proc_t proc);
int dtp_hg_unpack_body(struct dtp_rpc_priv *rpc_priv, dtp_proc_t proc);
int dtp_proc_in_common(dtp_proc_t proc, dtp_rpc_input_t *data);
int dtp_proc_out_common(dtp_proc_t proc, dtp_rpc_output_t *data);

/* some simple helper functions */
typedef hg_rpc_cb_t dtp_hg_rpc_cb_t;
static inline int
dtp_hg_reg(hg_class_t *hg_class, hg_id_t rpcid, dtp_proc_cb_t in_proc_cb,
	   dtp_proc_cb_t out_proc_cb, dtp_hg_rpc_cb_t rpc_cb)
{
	hg_return_t hg_ret;
	int         rc = 0;

	D_ASSERT(hg_class != NULL);

	hg_ret = HG_Register(hg_class, rpcid, (hg_proc_cb_t)in_proc_cb,
			     (hg_proc_cb_t)out_proc_cb, rpc_cb);
	if (hg_ret != HG_SUCCESS) {
		D_ERROR("HG_Register(rpcid: 0x%x) failed, hg_ret: %d.\n",
			rpcid, hg_ret);
		rc = -DER_DTP_HG;
	}
	return rc;
}

static inline int
dtp_hg_bulk_free(dtp_bulk_t bulk_hdl)
{
	hg_return_t	hg_ret;
	int		rc = 0;

	hg_ret = HG_Bulk_free(bulk_hdl);
	if (hg_ret != HG_SUCCESS) {
		D_ERROR("HG_Bulk_free failed, hg_ret: %d.\n", hg_ret);
		rc = -DER_DTP_HG;
	}

	return rc;
}

static inline int
dtp_hg_bulk_get_len(dtp_bulk_t bulk_hdl, dtp_size_t *bulk_len)
{
	hg_size_t	hg_size;

	D_ASSERT(bulk_len != NULL);
	hg_size = HG_Bulk_get_size(bulk_hdl);
	*bulk_len = hg_size;

	return 0;
}

static inline int
dtp_hg_bulk_get_sgnum(dtp_bulk_t bulk_hdl, unsigned int *bulk_sgnum)
{
	hg_uint32_t	hg_sgnum;

	D_ASSERT(bulk_sgnum != NULL);
	hg_sgnum = HG_Bulk_get_segment_count(bulk_hdl);
	*bulk_sgnum = hg_sgnum;

	return 0;
}

int dtp_hg_bulk_create(struct dtp_hg_context *hg_ctx, dtp_sg_list_t *sgl,
		       dtp_bulk_perm_t bulk_perm, dtp_bulk_t *bulk_hdl);
int dtp_hg_bulk_access(dtp_bulk_t bulk_hdl, dtp_sg_list_t *sgl);
int dtp_hg_bulk_transfer(struct dtp_bulk_desc *bulk_desc,
			 dtp_bulk_cb_t complete_cb,
			 void *arg, dtp_bulk_opid_t *opid);
static inline int
dtp_hg_bulk_cancel(dtp_bulk_opid_t opid)
{
	return HG_Bulk_cancel(opid);
}

#endif /* __DTP_MERCURY_H__ */
