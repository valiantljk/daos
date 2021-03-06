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
 * object shard operations.
 */

#include <daos/container.h>
#include <daos/pool_map.h>
#include <daos/transport.h>
#include "obj_rpc.h"
#include "obj_internal.h"

static void
obj_shard_free(struct dc_obj_shard *shard)
{
	D_FREE_PTR(shard);
}

static struct dc_obj_shard *
obj_shard_alloc(daos_rank_t rank, daos_unit_oid_t id, uint32_t part_nr)
{
	struct dc_obj_shard *shard;

	D_ALLOC_PTR(shard);
	if (shard == NULL)
		return NULL;

	shard->do_rank	  = rank;
	shard->do_part_nr = part_nr;
	shard->do_id	  = id;
	DAOS_INIT_LIST_HEAD(&shard->do_co_list);

	return shard;
}

static void
obj_shard_decref(struct dc_obj_shard *shard)
{
	D_ASSERT(shard->do_ref > 0);
	shard->do_ref--;
	if (shard->do_ref == 0)
		obj_shard_free(shard);
}

static void
obj_shard_addref(struct dc_obj_shard *shard)
{
	shard->do_ref++;
}

static void
obj_shard_hdl_link(struct dc_obj_shard *shard)
{
	obj_shard_addref(shard);
}

static void
obj_shard_hdl_unlink(struct dc_obj_shard *shard)
{
	obj_shard_decref(shard);
}

static struct dc_obj_shard*
obj_shard_hdl2ptr(daos_handle_t hdl)
{
	struct dc_obj_shard *shard;

	if (hdl.cookie == 0)
		return NULL;

	shard = (struct dc_obj_shard *)hdl.cookie;
	obj_shard_addref(shard);
	return shard;
}

static daos_handle_t
obj_shard_ptr2hdl(struct dc_obj_shard *shard)
{
	daos_handle_t oh;

	oh.cookie = (uint64_t)shard;
	return oh;
}

int
dc_obj_shard_open(daos_handle_t coh, uint32_t tgt, daos_unit_oid_t id,
		  unsigned int mode, daos_handle_t *oh)
{
	struct dc_obj_shard	*dobj;
	struct pool_target	*map_tgt;
	int			rc;

	rc = dc_cont_tgt_idx2ptr(coh, tgt, &map_tgt);
	if (rc != 0)
		return rc;

	dobj = obj_shard_alloc(map_tgt->ta_comp.co_rank, id,
			       map_tgt->ta_comp.co_nr);
	if (dobj == NULL)
		return -DER_NOMEM;

	dobj->do_co_hdl = coh;
	obj_shard_hdl_link(dobj);
	*oh = obj_shard_ptr2hdl(dobj);

	return 0;
}

int
dc_obj_shard_close(daos_handle_t oh)
{
	struct dc_obj_shard *dobj;

	dobj = obj_shard_hdl2ptr(oh);
	if (dobj == NULL)
		return -DER_NO_HDL;

	obj_shard_hdl_unlink(dobj);
	obj_shard_decref(dobj);
	return 0;
}

static void
obj_shard_rw_bulk_fini(dtp_rpc_t *rpc)
{
	struct obj_rw_in	*orw;
	dtp_bulk_t		*bulks;
	unsigned int		nr;
	int			i;

	orw = dtp_req_get(rpc);
	bulks = orw->orw_bulks.da_arrays;
	if (bulks == NULL)
		return;

	nr = orw->orw_bulks.da_count;
	for (i = 0; i < nr; i++)
		dtp_bulk_free(bulks[i]);

	D_FREE(bulks, nr * sizeof(dtp_bulk_t));
	orw->orw_bulks.da_arrays = NULL;
	orw->orw_bulks.da_count = 0;
}

struct rw_async_arg {
	daos_sg_list_t *rwaa_sgls;
	uint32_t       rwaa_nr;
};

static int
dc_obj_shard_sgl_copy(daos_sg_list_t *dst_sgl, uint32_t dst_nr,
		      daos_sg_list_t *src_sgl, uint32_t src_nr)
{
	int i;
	int j;

	if (src_nr > dst_nr) {
		D_ERROR("%u > %u\n", src_nr, dst_nr);
		return -DER_INVAL;
	}

	for (i = 0; i < src_nr; i++) {
		if (src_sgl[i].sg_nr.num == 0)
			continue;

		if (src_sgl[i].sg_nr.num > dst_sgl[i].sg_nr.num) {
			D_ERROR("%d : %u > %u\n", i,
				src_sgl[i].sg_nr.num, dst_sgl[i].sg_nr.num);
			return -DER_INVAL;
		}

		for (j = 0; j < src_sgl[i].sg_nr.num; j++) {
			if (src_sgl[i].sg_iovs[j].iov_len == 0)
				continue;

			if (src_sgl[i].sg_iovs[j].iov_len >
			    dst_sgl[i].sg_iovs[j].iov_buf_len) {
				D_ERROR("%d:%d "DF_U64" > "DF_U64"\n",
					i, j, src_sgl[i].sg_iovs[j].iov_len,
					src_sgl[i].sg_iovs[j].iov_buf_len);
				return -DER_INVAL;
			}
			memcpy(dst_sgl[i].sg_iovs[j].iov_buf,
			       src_sgl[i].sg_iovs[j].iov_buf,
			       src_sgl[i].sg_iovs[j].iov_len);
			dst_sgl[i].sg_iovs[j].iov_len =
				src_sgl[i].sg_iovs[j].iov_len;
		}
	}
	return 0;
}

static int
obj_rw_cp(struct daos_task *task, int rc)
{
	struct daos_op_sp	*sp = daos_task2sp(task);
	struct obj_rw_in	*orw;
	int			ret;

	orw = dtp_req_get(sp->sp_rpc);
	D_ASSERT(orw != NULL);
	if (rc) {
		D_ERROR("RPC %d failed: %d\n", opc_get(sp->sp_rpc->dr_opc), rc);
		D_GOTO(out, rc);
	}

	ret = obj_reply_get_status(sp->sp_rpc);
	if (ret != 0) {
		if (ret == -DER_STALE &&
		    orw->orw_map_ver < obj_reply_map_version_get(sp->sp_rpc)) {
			D_ERROR("update ver %u ---> %u\n", orw->orw_map_ver,
				obj_reply_map_version_get(sp->sp_rpc));
			/* XXX Push new tasks to update the map version */
		} else {
			D_ERROR("DAOS_OBJ_RPC_UPDATE/FETCH failed, rc: %d\n",
				ret);
		}
		D_GOTO(out, rc = ret);
	}

	if (opc_get(sp->sp_rpc->dr_opc) == DAOS_OBJ_RPC_FETCH) {
		struct obj_rw_out *orwo;
		daos_vec_iod_t	*iods;
		uint64_t	*sizes;
		int		j;
		int		k;
		int		idx = 0;

		orwo = dtp_reply_get(sp->sp_rpc);
		iods = orw->orw_iods.da_arrays;
		sizes = orwo->orw_sizes.da_arrays;

		/* update the sizes in iods */
		for (j = 0; j < orw->orw_nr; j++) {
			for (k = 0; k < iods[j].vd_nr; k++) {
				if (idx == orwo->orw_sizes.da_count) {
					D_ERROR("Invalid return size %d\n",
						idx);
					D_GOTO(out, rc = -DER_PROTO);
				}
				iods[j].vd_recxs[k].rx_rsize = sizes[idx];
				idx++;
			}
		}

		/* inline transfer */
		if (sp->sp_arg != NULL) {
			struct rw_async_arg	*arg = sp->sp_arg;

			rc = dc_obj_shard_sgl_copy(arg->rwaa_sgls, arg->rwaa_nr,
						   orwo->orw_sgls.da_arrays,
						   orwo->orw_sgls.da_count);
			D_FREE_PTR(arg);
		}
	}
out:
	obj_shard_rw_bulk_fini(sp->sp_rpc);
	dtp_req_decref(sp->sp_rpc);
	return rc;
}

static inline bool
obj_shard_io_check(unsigned int nr, daos_vec_iod_t *iods, daos_sg_list_t *sgls)
{
	int i;

	for (i = 0; i < nr; i++) {
		if (iods[i].vd_name.iov_buf == NULL ||
		    iods[i].vd_recxs == NULL)
			/* XXX checksum & eprs should not be mandatory */
			return false;
	}

	return true;
}

/**
 * XXX: Only use dkey to distribute the data among targets for
 * now, and eventually, it should use dkey + akey, but then
 * it means the I/O vector might needs to be split into
 * mulitple requests in obj_shard_rw()
 */
static uint32_t
obj_shard_dkey2tag(struct dc_obj_shard *dobj, daos_dkey_t *dkey)
{
	uint64_t hash;

	/** XXX hash is calculated twice (see cli_obj_dkey2shard) */
	hash = daos_hash_murmur64((unsigned char *)dkey->iov_buf,
				  dkey->iov_len, 5731);
	hash %= dobj->do_part_nr;

	return hash;
}

int
dc_obj_shard_rpc_cb(const struct dtp_cb_info *cb_info)
{
	struct daos_task	*task = cb_info->dci_arg;

	if (cb_info->dci_rc == -DER_TIMEDOUT)
		/** TODO */
		;

	if (task->dt_result == 0)
		task->dt_result = cb_info->dci_rc;

	task->dt_comp_cb(task);
	return 0;
}

uint64_t
iod_total_len(daos_vec_iod_t *iods, int nr)
{
	uint64_t iod_length = 0;
	int i;

	if (iods == NULL)
		return 0;

	for (i = 0; i < nr; i++) {
		int j;

		if (iods[i].vd_recxs == NULL)
			continue;

		for (j = 0; j < iods[i].vd_nr; j++)
			iod_length += iods[i].vd_recxs[j].rx_rsize;
	}

	return iod_length;
}

uint32_t
sgls_get_len(daos_sg_list_t *sgls, int nr)
{
	uint32_t sgls_len = 0;
	int i;

	if (sgls == NULL)
		return 0;

	/* create bulk transfer for daos_sg_list */
	for (i = 0; i < nr; i++) {
		int j;

		if (sgls[i].sg_iovs == NULL)
			continue;

		for (j = 0; j < sgls[i].sg_nr.num; j++)
			sgls_len += sgls[i].sg_iovs[j].iov_len;
	}

	return sgls_len;
}


static int
obj_shard_rw_bulk_prep(dtp_rpc_t *rpc, unsigned int nr, daos_sg_list_t *sgls,
		       struct daos_task *task)
{
	struct obj_rw_in	*orw;
	dtp_bulk_t		*bulks;
	dtp_bulk_perm_t		bulk_perm;
	int			i;
	int			rc = 0;

	bulk_perm = (opc_get(rpc->dr_opc) == DAOS_OBJ_RPC_UPDATE) ?
		    DTP_BULK_RO : DTP_BULK_RW;
	D_ALLOC(bulks, nr * sizeof(*bulks));
	if (bulks == NULL)
		D_GOTO(out, rc = -DER_NOMEM);

	/* create bulk transfer for daos_sg_list */
	for (i = 0; i < nr; i++) {
		if (sgls != NULL && sgls[i].sg_iovs != NULL &&
		    sgls[i].sg_iovs[0].iov_buf != NULL) {
			rc = dtp_bulk_create(daos_task2ctx(task), &sgls[i],
					     bulk_perm, &bulks[i]);
			if (rc < 0) {
				int j;

				for (j = 0; j < i; j++)
					dtp_bulk_free(bulks[j]);

				D_GOTO(out, rc);
			}
		}
	}

	orw = dtp_req_get(rpc);
	D_ASSERT(orw != NULL);
	orw->orw_bulks.da_count = nr;
	orw->orw_bulks.da_arrays = bulks;
out:
	if (rc != 0 && bulks != NULL)
		D_FREE(bulks, nr * sizeof(*bulks));

	return rc;
}

static int
obj_shard_rw(daos_handle_t oh, enum obj_rpc_opc opc, daos_epoch_t epoch,
	     daos_dkey_t *dkey, unsigned int nr, daos_vec_iod_t *iods,
	     daos_sg_list_t *sgls, struct daos_task *task)
{
	struct dc_obj_shard	*dobj;
	struct rw_async_arg	*rwaa = NULL;
	dtp_rpc_t		*req;
	struct obj_rw_in	*orw;
	struct daos_op_sp	*sp;
	dtp_endpoint_t		tgt_ep;
	uuid_t			cont_hdl_uuid;
	uint32_t		map_version;
	daos_size_t		total_len;
	int			rc;

	/** sanity check input parameters */
	if (dkey == NULL || dkey->iov_buf == NULL || nr == 0 ||
	    !obj_shard_io_check(nr, iods, sgls))
		return -DER_INVAL;

	dobj = obj_shard_hdl2ptr(oh);
	if (dobj == NULL)
		return -DER_NO_HDL;

	rc = dc_cont_hdl2uuid_map_ver(dobj->do_co_hdl, &cont_hdl_uuid,
				      &map_version);
	if (rc != 0) {
		obj_shard_decref(dobj);
		return rc;
	}

	tgt_ep.ep_rank = dobj->do_rank;
	tgt_ep.ep_tag = obj_shard_dkey2tag(dobj, dkey);
	rc = obj_req_create(daos_task2ctx(task), tgt_ep, opc, &req);
	if (rc != 0) {
		obj_shard_decref(dobj);
		return rc;
	}

	orw = dtp_req_get(req);
	D_ASSERT(orw != NULL);

	orw->orw_map_ver = map_version;
	orw->orw_oid = dobj->do_id;
	uuid_copy(orw->orw_co_hdl, cont_hdl_uuid);

	obj_shard_decref(dobj);
	dobj = NULL;

	orw->orw_epoch = epoch;
	orw->orw_nr = nr;
	/** FIXME: large dkey should be transferred via bulk */
	orw->orw_dkey = *dkey;

	/* FIXME: if iods is too long, then we needs to do bulk transfer
	 * as well, but then we also needs to serialize the iods
	 **/
	orw->orw_iods.da_count = nr;
	orw->orw_iods.da_arrays = iods;

	if (opc == DAOS_OBJ_RPC_FETCH)
		total_len = sgls_get_len(sgls, nr);
	else
		total_len = iod_total_len(iods, nr);

	if (total_len >= OBJ_BULK_LIMIT) {
		/* Transfer data by bulk */
		rc = obj_shard_rw_bulk_prep(req, nr, sgls, task);
		if (rc != 0)
			D_GOTO(out_bulk, rc);
		orw->orw_sgls.da_count = 0;
		orw->orw_sgls.da_arrays = NULL;
	} else {
		/* Transfer data inline */
		orw->orw_sgls.da_count = nr;
		orw->orw_sgls.da_arrays = sgls;
		orw->orw_bulks.da_count = 0;
		orw->orw_bulks.da_arrays = NULL;
	}

	sp = daos_task2sp(task);
	dtp_req_addref(req);
	sp->sp_rpc = req;
	sp->sp_callback = obj_rw_cp;

	if (opc == DAOS_OBJ_RPC_FETCH && orw->orw_sgls.da_arrays != NULL) {
		/* remember the sgl to copyout the data inline for fetch */
		D_ALLOC_PTR(rwaa);
		if (rwaa == NULL)
			D_GOTO(out_bulk, rc);
		rwaa->rwaa_nr = nr;
		rwaa->rwaa_sgls = sgls;
		sp->sp_arg = rwaa;
	}

	rc = dtp_req_send(req, dc_obj_shard_rpc_cb, task);
	if (rc != 0)
		D_GOTO(out_bulk, rc);

	return rc;
out_bulk:
	obj_shard_rw_bulk_fini(req);
	if (rwaa != NULL)
		D_FREE_PTR(rwaa);
	dtp_req_decref(req);
	return rc;
}

int
dc_obj_shard_update(daos_handle_t oh, daos_epoch_t epoch,
		    daos_dkey_t *dkey, unsigned int nr,
		    daos_vec_iod_t *iods, daos_sg_list_t *sgls,
		    struct daos_task *task)
{
	return obj_shard_rw(oh, DAOS_OBJ_RPC_UPDATE, epoch, dkey, nr, iods,
			    sgls, task);
}

int
dc_obj_shard_fetch(daos_handle_t oh, daos_epoch_t epoch,
		   daos_dkey_t *dkey, unsigned int nr,
		   daos_vec_iod_t *iods, daos_sg_list_t *sgls,
		   daos_vec_map_t *maps, struct daos_task *task)
{
	return obj_shard_rw(oh, DAOS_OBJ_RPC_FETCH, epoch, dkey, nr, iods,
			    sgls, task);
}

struct enum_async_arg {
	uint32_t	*eaa_nr;
	daos_key_desc_t *eaa_kds;
	daos_hash_out_t *eaa_anchor;
	struct dc_obj_shard *eaa_obj;
	daos_sg_list_t	*eaa_sgl;
};

static int
enumerate_cp(struct daos_task *task, int rc)
{
	struct daos_op_sp	*sp = daos_task2sp(task);
	struct obj_key_enum_in	*oei;
	struct obj_key_enum_out	*oeo;
	struct enum_async_arg	*eaa;
	int			tgt_tag;

	oei = dtp_req_get(sp->sp_rpc);
	D_ASSERT(oei != NULL);
	eaa = sp->sp_arg;
	D_ASSERT(eaa != NULL);
	if (rc) {
		D_ERROR("RPC error: %d\n", rc);
		D_GOTO(out, rc);
	}

	oeo = dtp_reply_get(sp->sp_rpc);
	if (oeo->oeo_ret < 0) {
		if (oeo->oeo_ret == -DER_STALE &&
		    oei->oei_map_ver < obj_reply_map_version_get(sp->sp_rpc)) {
			D_ERROR("update ver %u ---> %u\n", oei->oei_map_ver,
				obj_reply_map_version_get(sp->sp_rpc));
			/* XXX Push new tasks to update the map version */
		} else {
			D_ERROR("enumerate failed, rc: %d\n", oeo->oeo_ret);
		}
		D_GOTO(out, rc = oeo->oeo_ret);
	}

	if (*eaa->eaa_nr < oeo->oeo_kds.da_count) {
		D_ERROR("DAOS_OBJ_RPC_ENUMERATE return more kds, rc: %d\n",
			-DER_PROTO);
		D_GOTO(out, rc = -DER_PROTO);
	}

	*(eaa->eaa_nr) = oeo->oeo_kds.da_count;
	memcpy(eaa->eaa_kds, oeo->oeo_kds.da_arrays,
	       sizeof(*eaa->eaa_kds) * oeo->oeo_kds.da_count);

	enum_anchor_copy(eaa->eaa_anchor, &oeo->oeo_anchor);
	if (daos_hash_is_eof(&oeo->oeo_anchor) &&
	    opc_get(sp->sp_rpc->dr_opc) == DAOS_OBJ_DKEY_RPC_ENUMERATE) {
		tgt_tag = enum_anchor_get_tag(eaa->eaa_anchor);
		if (tgt_tag < eaa->eaa_obj->do_part_nr - 1) {
			memset(eaa->eaa_anchor, 0, sizeof(*eaa->eaa_anchor));
			enum_anchor_set_tag(eaa->eaa_anchor, ++tgt_tag);
		}
	}

	if (oeo->oeo_sgl.sg_nr.num > 0 && oeo->oeo_sgl.sg_iovs != NULL)
		rc = dc_obj_shard_sgl_copy(eaa->eaa_sgl, 1, &oeo->oeo_sgl, 1);
out:
	if (eaa->eaa_obj != NULL)
		obj_shard_decref(eaa->eaa_obj);

	D_FREE_PTR(eaa);

	if (oei->oei_bulk != NULL)
		dtp_bulk_free(oei->oei_bulk);

	dtp_req_decref(sp->sp_rpc);
	return rc;
}

int
dc_obj_shard_list_key(daos_handle_t oh, enum obj_rpc_opc opc,
		      daos_epoch_t epoch, daos_key_t *key, uint32_t *nr,
		      daos_key_desc_t *kds, daos_sg_list_t *sgl,
		      daos_hash_out_t *anchor, struct daos_task *task)
{
	dtp_endpoint_t		tgt_ep;
	dtp_rpc_t		*req;
	struct dc_obj_shard	*dobj;
	uuid_t			cont_hdl_uuid;
	struct obj_key_enum_in	*oei;
	struct enum_async_arg	*eaa;
	struct daos_op_sp	*sp;
	daos_size_t		sgl_len;
	uint32_t		map_version;
	int			rc;

	dobj = obj_shard_hdl2ptr(oh);
	if (dobj == NULL)
		return -DER_NO_HDL;

	rc = dc_cont_hdl2uuid_map_ver(dobj->do_co_hdl, &cont_hdl_uuid,
				      &map_version);
	if (rc != 0)
		D_GOTO(out_put, rc);

	tgt_ep.ep_grp = NULL;
	tgt_ep.ep_rank = dobj->do_rank;
	if (opc == DAOS_OBJ_AKEY_RPC_ENUMERATE) {
		D_ASSERT(key != NULL);
		tgt_ep.ep_tag = obj_shard_dkey2tag(dobj, key);
	} else {
		tgt_ep.ep_tag = enum_anchor_get_tag(anchor);
	}

	rc = obj_req_create(daos_task2ctx(task), tgt_ep, opc, &req);
	if (rc != 0)
		D_GOTO(out_put, rc);

	oei = dtp_req_get(req);
	if (key != NULL)
		oei->oei_key = *key;
	else
		memset(&oei->oei_key, 0, sizeof(oei->oei_key));

	D_ASSERT(oei != NULL);
	oei->oei_oid = dobj->do_id;
	uuid_copy(oei->oei_co_hdl, cont_hdl_uuid);

	oei->oei_map_ver = map_version;
	oei->oei_epoch = epoch;
	oei->oei_nr = *nr;

	enum_anchor_copy(&oei->oei_anchor, anchor);
	oei->oei_sgl = *sgl;
	sgl_len = sgls_get_len(sgl, 1);
	if (sgl_len >= OBJ_BULK_LIMIT) {
		/* Create bulk */
		rc = dtp_bulk_create(daos_task2ctx(task), sgl, DTP_BULK_RW,
				     &oei->oei_bulk);
		if (rc < 0)
			D_GOTO(out_req, rc);
	}

	sp = daos_task2sp(task);
	dtp_req_addref(req);
	sp->sp_rpc = req;
	D_ALLOC_PTR(eaa);
	if (eaa == NULL)
		D_GOTO(out_bulk, rc = -DER_NOMEM);

	eaa->eaa_nr = nr;
	eaa->eaa_kds = kds;
	eaa->eaa_anchor = anchor;
	eaa->eaa_obj = dobj;
	eaa->eaa_sgl = sgl;
	sp->sp_arg = eaa;
	sp->sp_callback = enumerate_cp;

	rc = dtp_req_send(req, dc_obj_shard_rpc_cb, task);
	if (rc != 0)
		D_GOTO(out_eaa, rc);

	return rc;
out_eaa:
	D_FREE_PTR(eaa);
out_bulk:
	dtp_bulk_free(oei->oei_bulk);
out_req:
	dtp_req_decref(req);
out_put:
	obj_shard_decref(dobj);
	return rc;
}
