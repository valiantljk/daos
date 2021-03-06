/**
 * (C) Copyright 2015, 2016 Intel Corporation.
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
 * ds_cont: Container Server API
 */

#ifndef __DAOS_SRV_CONTAINER_H__
#define __DAOS_SRV_CONTAINER_H__

#include <daos_types.h>
#include <daos_srv/pool.h>

/*
 * Per-thread container (memory) object
 *
 * Stores per-thread, per-container information, such as the vos container
 * handle.
 */
struct ds_cont {
	struct daos_llink	sc_list;
	daos_handle_t		sc_hdl;
	uuid_t			sc_uuid;
};

/*
 * Per-thread container handle (memory) object
 *
 * Stores per-thread, per-handle information, such as the container
 * capabilities. References the ds_cont and the ds_pool_child objects.
 */
struct ds_cont_hdl {
	daos_list_t		sch_entry;
	uuid_t			sch_uuid;	/* of the container handle */
	uint64_t		sch_capas;
	struct ds_pool_child   *sch_pool;
	struct ds_cont	       *sch_cont;
	int			sch_ref;
};

struct ds_cont_hdl *ds_cont_hdl_lookup(const uuid_t uuid);
void ds_cont_hdl_put(struct ds_cont_hdl *hdl);

#endif /* ___DAOS_SRV_CONTAINER_H_ */
