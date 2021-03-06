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
 * dmgs: Internal Declarations
 *
 * This file contains all declarations that are only used by dmgss.
 * All external variables and functions must have a "dmgs_" prefix.
 */

#ifndef __DMGS_INTERNAL_H__
#define __DMGS_INTERNAL_H__

#include <daos/list.h>
#include <daos/transport.h>
#include <daos/rpc.h>
#include <daos/common.h>
#include <daos_srv/daos_server.h>
#include "dmg_rpc.h"

/** dmgs_pool.c */
int dmgs_hdlr_pool_create(dtp_rpc_t *rpc_req);
int dmgs_hdlr_pool_destroy(dtp_rpc_t *rpc_req);

/** dmgs_target.c */
int dmgs_tgt_init(void);
int dmgs_hdlr_tgt_create(dtp_rpc_t *rpc_req);
int dmgs_hdlr_tgt_destroy(dtp_rpc_t *rpc_req);
#endif /* __DMGS_INTERNAL_H__ */
