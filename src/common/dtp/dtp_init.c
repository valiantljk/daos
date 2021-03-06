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
 * This file is part of daos_transport. It implements dtp init and finalize
 * related APIs/handling.
 */

#include <dtp_internal.h>

struct dtp_gdata dtp_gdata;
static pthread_once_t gdata_init_once = PTHREAD_ONCE_INIT;
static volatile int   gdata_init_flag;

/* internally generate a physical address string */
static int
dtp_gen_bmi_phyaddr(dtp_phy_addr_t *phy_addr)
{
	int			socketfd;
	struct sockaddr_in	tmp_socket;
	char			*addrstr;
	char			tmp_addrstr[DTP_ADDR_STR_MAX_LEN];
	struct ifaddrs		*if_addrs = NULL;
	struct ifaddrs		*ifa;
	void			*tmp_ptr;
	char			ip_str[INET_ADDRSTRLEN];
	const char		*ip_str_p = NULL;
	socklen_t		slen = sizeof(struct sockaddr);
	int			rc;

	D_ASSERT(phy_addr != NULL);

	/*
	 * step 1 - get the IP address (cannot get it through socket, always get
	 * 0.0.0.0 by inet_ntoa(tmp_socket.sin_addr).)
	 * Using the IP as listening address is better than using hostname
	 * because:
	 * 1) for the case there are multiple NICs on one host,
	 * 2) mercury is much slow when listening on hostname (not sure why).
	 */
	rc = getifaddrs(&if_addrs);
	if (rc != 0) {
		D_ERROR("cannot getifaddrs, errno: %d(%s).\n",
			errno, strerror(errno));
		D_GOTO(out, rc = -DER_DTP_ADDRSTR_GEN);
	}
	D_ASSERT(if_addrs != NULL);

	/* TODO may from a config file to select one appropriate IP address */
	for (ifa = if_addrs; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL)
			continue;
		memset(ip_str, 0, INET_ADDRSTRLEN);
		if (ifa->ifa_addr->sa_family == AF_INET) {
			/* check it is a valid IPv4 Address */
			tmp_ptr =
			&((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
			ip_str_p = inet_ntop(AF_INET, tmp_ptr, ip_str,
					     INET_ADDRSTRLEN);
			if (ip_str_p == NULL) {
				D_ERROR("inet_ntop failed, errno: %d(%s).\n",
					errno, strerror(errno));
				freeifaddrs(if_addrs);
				D_GOTO(out, rc = -DER_DTP_ADDRSTR_GEN);
			}
			if (strcmp(ip_str_p, "127.0.0.1") == 0) {
				/* D_DEBUG(DF_TP, "bypass 127.0.0.1.\n"); */
				continue;
			}
			D_DEBUG(DF_TP, "Get %s IPv4 Address %s\n",
				ifa->ifa_name, ip_str);
			break;
		} else if (ifa->ifa_addr->sa_family == AF_INET6) {
			/* check it is a valid IPv6 Address */
			/*
			 * tmp_ptr =
			 * &((struct sockaddr_in6 *)ifa->ifa_addr)->sin6_addr;
			 * inet_ntop(AF_INET6, tmp_ptr, ip_str,
			 *           INET6_ADDRSTRLEN);
			 * D_DEBUG(DF_TP, "Get %s IPv6 Address %s\n",
			 *         ifa->ifa_name, ip_str);
			 */
		}
	}
	freeifaddrs(if_addrs);
	if (ip_str_p == NULL) {
		D_ERROR("no IP addr found.\n");
		D_GOTO(out, rc = -DER_DTP_ADDRSTR_GEN);
	}

	/* step 2 - get one available port number */
	socketfd = socket(AF_INET, SOCK_STREAM, 0);
	if (socketfd == -1) {
		D_ERROR("cannot create socket, errno: %d(%s).\n",
			errno, strerror(errno));
		D_GOTO(out, rc = -DER_DTP_ADDRSTR_GEN);
	}
	tmp_socket.sin_family = AF_INET;
	tmp_socket.sin_addr.s_addr = INADDR_ANY;
	tmp_socket.sin_port = 0;

	rc = bind(socketfd, (const struct sockaddr *)&tmp_socket,
		  sizeof(tmp_socket));
	if (rc != 0) {
		D_ERROR("cannot bind socket, errno: %d(%s).\n",
			errno, strerror(errno));
		close(socketfd);
		D_GOTO(out, rc = -DER_DTP_ADDRSTR_GEN);
	}

	rc = getsockname(socketfd, (struct sockaddr *)&tmp_socket, &slen);
	if (rc != 0) {
		D_ERROR("cannot create getsockname, errno: %d(%s).\n",
			errno, strerror(errno));
		close(socketfd);
		D_GOTO(out, rc = -DER_DTP_ADDRSTR_GEN);
	}
	rc = close(socketfd);
	if (rc != 0) {
		D_ERROR("cannot close socket, errno: %d(%s).\n",
			errno, strerror(errno));
		D_GOTO(out, rc = -DER_DTP_ADDRSTR_GEN);
	}

	snprintf(tmp_addrstr, DTP_ADDR_STR_MAX_LEN, "bmi+tcp://%s:%d", ip_str,
		 ntohs(tmp_socket.sin_port));
	addrstr = strndup(tmp_addrstr, DTP_ADDR_STR_MAX_LEN);
	if (addrstr != NULL) {
		D_DEBUG(DF_TP, "generated phyaddr: %s.\n", addrstr);
		*phy_addr = addrstr;
	} else {
		D_ERROR("strndup failed.\n");
		rc = -DER_NOMEM;
	}

out:
	return rc;
}


/* first step init - for initializing dtp_gdata */
static void data_init()
{
	int rc = 0;

	D_DEBUG(DF_TP, "initializing dtp_gdata...\n");

	/*
	 * avoid size mis-matching between client/server side
	 * /see dtp_proc_uuid_t().
	 */
	D_CASSERT(sizeof(uuid_t) == 16);

	DAOS_INIT_LIST_HEAD(&dtp_gdata.dg_ctx_list);

	rc = pthread_rwlock_init(&dtp_gdata.dg_rwlock, NULL);
	D_ASSERT(rc == 0);

	dtp_gdata.dg_ctx_num = 0;
	dtp_gdata.dg_refcount = 0;
	dtp_gdata.dg_inited = 0;
	dtp_gdata.dg_addr = NULL;
	dtp_gdata.dg_verbs = false;
	dtp_gdata.dg_multi_na = false;

	gdata_init_flag = 1;
}

static int
dtp_mcl_init(dtp_phy_addr_t *addr)
{
	struct mcl_set	*tmp_set;
	int		rc;

	D_ASSERT(addr != NULL && strlen(*addr) > 0);

	dtp_gdata.dg_mcl_state = mcl_init(addr);
	if (dtp_gdata.dg_mcl_state == NULL) {
		D_ERROR("mcl_init failed.\n");
		D_GOTO(out, rc = -DER_DTP_MCL);
	}
	D_DEBUG(DF_TP, "mcl_init succeed(server %d), nspace: %s, rank: %d, "
		"univ_size: %d, self_uri: %s.\n",
		dtp_gdata.dg_server,
		dtp_gdata.dg_mcl_state->myproc.nspace,
		dtp_gdata.dg_mcl_state->myproc.rank,
		dtp_gdata.dg_mcl_state->univ_size,
		dtp_gdata.dg_mcl_state->self_uri);
	if (dtp_gdata.dg_server == true) {
		rc = mcl_startup(dtp_gdata.dg_mcl_state,
				 dtp_gdata.dg_hg->dhg_nacla,
				 DTP_GLOBAL_SRV_GROUP_NAME, true,
				 &dtp_gdata.dg_mcl_srv_set);
		tmp_set = dtp_gdata.dg_mcl_srv_set;
	} else {
		rc = mcl_startup(dtp_gdata.dg_mcl_state,
				 dtp_gdata.dg_hg->dhg_nacla,
				 DTP_CLI_GROUP_NAME, false,
				 &dtp_gdata.dg_mcl_cli_set);
		tmp_set = dtp_gdata.dg_mcl_cli_set;
	}
	if (rc != MCL_SUCCESS) {
		D_ERROR("mcl_startup failed(server: %d), rc: %d.\n",
			dtp_gdata.dg_server, rc);
		mcl_finalize(dtp_gdata.dg_mcl_state);
		D_GOTO(out, rc = -DER_DTP_MCL);
	}
	D_DEBUG(DF_TP, "mcl_startup succeed(server: %d), grp_name: %s, "
		"size %d, rank %d, is_local %d, is_service %d, self_uri: %s.\n",
		dtp_gdata.dg_server, tmp_set->name, tmp_set->size,
		tmp_set->self, tmp_set->is_local, tmp_set->is_service,
		tmp_set->state->self_uri);
	if (dtp_gdata.dg_server == true) {
		D_ASSERT(dtp_gdata.dg_mcl_srv_set != NULL);
	} else {
		D_ASSERT(dtp_gdata.dg_mcl_cli_set != NULL);
		/* for client, attach it to service process set. */
		rc = mcl_attach(dtp_gdata.dg_mcl_state,
				DTP_GLOBAL_SRV_GROUP_NAME,
				&dtp_gdata.dg_mcl_srv_set);
		if (rc == MCL_SUCCESS) {
			D_ASSERT(dtp_gdata.dg_mcl_srv_set != NULL);
			tmp_set = dtp_gdata.dg_mcl_srv_set;
			D_DEBUG(DF_TP, "attached to group(name: %s, size %d, "
				"rank %d, is_local %d, is_service %d).\n",
				tmp_set->name, tmp_set->size,
				tmp_set->self, tmp_set->is_local,
				tmp_set->is_service);
		} else {
			D_ERROR("failed to attach to service group, rc: %d.\n",
				rc);
			mcl_set_free(NULL, dtp_gdata.dg_mcl_cli_set);
			mcl_finalize(dtp_gdata.dg_mcl_state);
			D_GOTO(out, rc = -DER_DTP_MCL);
		}
	}
	dtp_gdata.dg_srv_grp_id = DTP_GLOBAL_SRV_GROUP_NAME;
	dtp_gdata.dg_cli_grp_id = DTP_CLI_GROUP_NAME;

out:
	return rc;
}

static int
dtp_mcl_fini()
{
	int rc = 0;

	D_ASSERT(dtp_gdata.dg_mcl_state != NULL);
	D_ASSERT(dtp_gdata.dg_mcl_srv_set != NULL);

	mcl_set_free(dtp_gdata.dg_hg->dhg_nacla,
		     dtp_gdata.dg_mcl_srv_set);
	if (dtp_gdata.dg_server == false) {
		mcl_set_free(dtp_gdata.dg_hg->dhg_nacla,
			     dtp_gdata.dg_mcl_cli_set);
	}

	D_ASSERT(dtp_gdata.dg_mcl_state != NULL);
	rc = mcl_finalize(dtp_gdata.dg_mcl_state);
	if (rc == 0)
		D_DEBUG(DF_TP, "mcl_finalize succeed.\n");
	else
		D_ERROR("mcl_finalize failed, rc: %d.\n", rc);

	return rc;
}

int
dtp_init(bool server)
{
	dtp_phy_addr_t	addr = NULL, addr_env;
	int		rc = 0;

	D_DEBUG(DF_TP, "Enter dtp_init.\n");

	if (gdata_init_flag == 0) {
		rc = pthread_once(&gdata_init_once, data_init);
		if (rc != 0) {
			D_ERROR("dtp_init failed, rc(%d) - %s.\n",
				rc, strerror(rc));
			D_GOTO(out, rc = -rc);
		}
	}
	D_ASSERT(gdata_init_flag == 1);

	pthread_rwlock_wrlock(&dtp_gdata.dg_rwlock);
	if (dtp_gdata.dg_inited == 0) {
		dtp_gdata.dg_server = server;

		if (server == true)
			dtp_gdata.dg_multi_na = true;

		addr_env = (dtp_phy_addr_t)getenv(DTP_PHY_ADDR_ENV);
		if (addr_env == NULL) {
			D_DEBUG(DF_TP, "ENV %s not found.\n", DTP_PHY_ADDR_ENV);
			goto do_init;
		} else{
			D_DEBUG(DF_TP, "EVN %s: %s.\n",
				DTP_PHY_ADDR_ENV, addr_env);
		}
		if (strncmp(addr_env, "bmi+tcp", 7) == 0) {
			if (strcmp(addr_env, "bmi+tcp") == 0) {
				rc = dtp_gen_bmi_phyaddr(&addr);
				if (rc == 0) {
					D_DEBUG(DF_TP, "ENV %s (%s), generated "
						"a BMI phyaddr: %s.\n",
						DTP_PHY_ADDR_ENV, addr_env,
						addr);
				} else {
					D_ERROR("dtp_gen_bmi_phyaddr failed, "
						"rc: %d.\n", rc);
					D_GOTO(out, rc);
				}
			} else {
				D_DEBUG(DF_TP, "ENV %s found, use addr %s.\n",
					DTP_PHY_ADDR_ENV, addr_env);
				addr = strdup(addr_env);
				if (addr == NULL) {
					D_ERROR("strdup failed.\n");
					D_GOTO(out, rc = -DER_NOMEM);
				}
			}
			D_ASSERT(addr != NULL);
			dtp_gdata.dg_multi_na = false;
		} else if (strncmp(addr_env, "cci+verbs", 9) == 0) {
			dtp_gdata.dg_verbs = true;
		}

do_init:
		/*
		 * For client unset the CCI_CONFIG ENV, then client-side process
		 * will use random port number and will not conflict with server
		 * side. As when using orterun to load both server and client it
		 * possibly will lead them share the same ENV.
		 */
		if (server == false)
			unsetenv("CCI_CONFIG");

		rc = dtp_hg_init(&addr, server);
		if (rc != 0) {
			D_ERROR("dtp_hg_init failed rc: %d.\n", rc);
			D_GOTO(unlock, rc);
		}
		D_ASSERT(addr != NULL);
		dtp_gdata.dg_addr = addr;
		dtp_gdata.dg_addr_len = strlen(addr);

		rc = dtp_mcl_init(&addr);
		if (rc != 0) {
			D_ERROR("dtp_mcl_init failed, rc: %d.\n", rc);
			dtp_hg_fini();
			free(dtp_gdata.dg_addr);
			D_GOTO(unlock, rc = -DER_DTP_MCL);
		}

		rc = dtp_opc_map_create(DTP_OPC_MAP_BITS);
		if (rc != 0) {
			D_ERROR("dtp_opc_map_create failed rc: %d.\n", rc);
			dtp_hg_fini();
			dtp_mcl_fini();
			free(dtp_gdata.dg_addr);
			D_GOTO(unlock, rc);
		}
		D_ASSERT(dtp_gdata.dg_opc_map != NULL);

		dtp_gdata.dg_inited = 1;
	} else {
		if (dtp_gdata.dg_server == false && server == true) {
			D_ERROR("DTP initialized as client, cannot set as "
				"server again.\n");
			D_GOTO(unlock, rc = -DER_INVAL);
		}
	}

	dtp_gdata.dg_refcount++;

unlock:
	pthread_rwlock_unlock(&dtp_gdata.dg_rwlock);
out:
	D_DEBUG(DF_TP, "Exit dtp_init, rc: %d.\n", rc);
	return rc;
}

bool
dtp_initialized()
{
	return (gdata_init_flag == 1) && (dtp_gdata.dg_inited == 1);
}

int
dtp_finalize(void)
{
	int rc = 0;

	D_DEBUG(DF_TP, "Enter dtp_finalize.\n");

	pthread_rwlock_wrlock(&dtp_gdata.dg_rwlock);

	if (!dtp_initialized()) {
		D_ERROR("cannot finalize before initializing.\n");
		pthread_rwlock_unlock(&dtp_gdata.dg_rwlock);
		D_GOTO(out, rc = -DER_UNINIT);
	}
	if (dtp_gdata.dg_ctx_num > 0) {
		D_ASSERT(!dtp_context_empty(DTP_LOCKED));
		D_ERROR("cannot finalize, current ctx_num(%d).\n",
			dtp_gdata.dg_ctx_num);
		pthread_rwlock_unlock(&dtp_gdata.dg_rwlock);
		D_GOTO(out, rc = -DER_NO_PERM);
	} else {
		D_ASSERT(dtp_context_empty(DTP_LOCKED));
	}

	dtp_gdata.dg_refcount--;
	if (dtp_gdata.dg_refcount == 0) {
		rc = dtp_mcl_fini();
		/* mcl finalize failure cause state unstable, just assert it */
		D_ASSERT(rc == 0);

		rc = dtp_hg_fini();
		if (rc != 0) {
			D_ERROR("dtp_hg_fini failed rc: %d.\n", rc);
			dtp_gdata.dg_refcount++;
			pthread_rwlock_unlock(&dtp_gdata.dg_rwlock);
			D_GOTO(out, rc);
		}

		D_ASSERT(dtp_gdata.dg_addr != NULL);
		free(dtp_gdata.dg_addr);
		dtp_gdata.dg_server = false;

		dtp_opc_map_destroy(dtp_gdata.dg_opc_map);

		pthread_rwlock_unlock(&dtp_gdata.dg_rwlock);

		rc = pthread_rwlock_destroy(&dtp_gdata.dg_rwlock);
		if (rc != 0) {
			D_ERROR("failed to destroy dg_rwlock, rc: %d.\n", rc);
			D_GOTO(out, rc = -rc);
		}

		/* allow the same program to re-initialize */
		dtp_gdata.dg_refcount = 0;
		dtp_gdata.dg_inited = 0;
		gdata_init_once = PTHREAD_ONCE_INIT;
		gdata_init_flag = 0;
	} else {
		pthread_rwlock_unlock(&dtp_gdata.dg_rwlock);
	}

out:
	D_DEBUG(DF_TP, "Exit dtp_finalize, rc: %d.\n", rc);
	return rc;
}
