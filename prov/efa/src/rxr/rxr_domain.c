/*
 * Copyright (c) 2019 Amazon.com, Inc. or its affiliates.
 * All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <ofi_util.h>
#include <ofi_recvwin.h>

#include "rxr.h"
#include "rxr_cntr.h"

static struct fi_ops_domain rxr_domain_ops = {
	.size = sizeof(struct fi_ops_domain),
	.av_open = rxr_av_open,
	.cq_open = rxr_cq_open,
	.endpoint = rxr_endpoint,
	.scalable_ep = fi_no_scalable_ep,
	.cntr_open = rxr_cntr_open,
	.poll_open = fi_poll_create,
	.stx_ctx = fi_no_stx_context,
	.srx_ctx = fi_no_srx_context,
	.query_atomic = fi_no_query_atomic,
};

static int rxr_domain_close(fid_t fid)
{
	int ret;
	struct rxr_domain *rxr_domain;

	rxr_domain = container_of(fid, struct rxr_domain,
				  util_domain.domain_fid.fid);

	ret = fi_close(&rxr_domain->rdm_domain->fid);
	if (ret)
		return ret;

	ret = ofi_domain_close(&rxr_domain->util_domain);
	if (ret)
		return ret;

	free(rxr_domain);
	return 0;
}

static struct fi_ops rxr_domain_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = rxr_domain_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

static int rxr_mr_close(fid_t fid)
{
	struct rxr_domain *rxr_domain;
	struct rxr_mr *rxr_mr;
	int ret;

	rxr_mr = container_of(fid, struct rxr_mr, mr_fid.fid);
	rxr_domain = rxr_mr->domain;

	ret = ofi_mr_map_remove(&rxr_domain->util_domain.mr_map,
				rxr_mr->mr_fid.key);
	if (ret)
		FI_WARN(&rxr_prov, FI_LOG_MR,
			"Unable to remove MR entry from util map (%s)\n",
			fi_strerror(-ret));

	ret = fi_close(&rxr_mr->msg_mr->fid);
	if (ret)
		FI_WARN(&rxr_prov, FI_LOG_MR,
			"Unable to close MR\n");
	free(rxr_mr);
	return ret;
}

static struct fi_ops rxr_mr_ops = {
	.size = sizeof(struct fi_ops),
	.close = rxr_mr_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

static int rxr_rma_verify_iov(struct rxr_ep *ep, struct ofi_rma_iov *rma,
			      size_t count, uint32_t type, struct iovec *iov)
{
	struct util_domain *util_domain;
	int i, ret;

	util_domain = &rxr_ep_domain(ep)->util_domain;

	for (i = 0; i < count; i++) {
		ret = ofi_mr_verify(&util_domain->mr_map,
				    rma[i].len,
				    (uintptr_t *)(&rma[i].addr),
				    rma[i].key,
				    ofi_rx_mr_reg_flags(type, 0));
		if (ret) {
			FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
				"MR verification failed (%s)\n",
				fi_strerror(-ret));
			return -FI_EACCES;
		}

		iov[i].iov_base = (void *)rma[i].addr;
		iov[i].iov_len = rma[i].len;
	}
	return 0;
}

int rxr_mr_regattr(struct fid *domain_fid, const struct fi_mr_attr *attr,
		   uint64_t flags, struct fid_mr **mr)
{
	struct rxr_domain *rxr_domain;
	struct fi_mr_attr *core_attr;
	struct rxr_mr *rxr_mr;
	int ret;

	rxr_domain = container_of(domain_fid, struct rxr_domain,
				  util_domain.domain_fid.fid);

	rxr_mr = calloc(1, sizeof(*rxr_mr));
	if (!rxr_mr)
		return -FI_ENOMEM;

	/* discard const qualifier to override access registered with EFA */
	core_attr = (struct fi_mr_attr *)attr;
	core_attr->access = FI_SEND | FI_RECV;

	ret = fi_mr_regattr(rxr_domain->rdm_domain, core_attr, flags,
			    &rxr_mr->msg_mr);
	if (ret) {
		FI_WARN(&rxr_prov, FI_LOG_MR,
			"Unable to register MR buf (%s): %p len: %lu\n",
			fi_strerror(-ret), attr->mr_iov->iov_base,
			attr->mr_iov->iov_len);
		goto err;
	}

	rxr_mr->mr_fid.fid.fclass = FI_CLASS_MR;
	rxr_mr->mr_fid.fid.context = attr->context;
	rxr_mr->mr_fid.fid.ops = &rxr_mr_ops;
	rxr_mr->mr_fid.mem_desc = rxr_mr->msg_mr;
	rxr_mr->mr_fid.key = fi_mr_key(rxr_mr->msg_mr);
	rxr_mr->domain = rxr_domain;
	*mr = &rxr_mr->mr_fid;

	assert(rxr_mr->mr_fid.key != FI_KEY_NOTAVAIL);
	ret = ofi_mr_map_insert(&rxr_domain->util_domain.mr_map, attr,
				&rxr_mr->mr_fid.key, mr);
	if (ret) {
		FI_WARN(&rxr_prov, FI_LOG_MR,
			"Unable to add MR to map buf (%s): %p len: %lu\n",
			fi_strerror(-ret), attr->mr_iov->iov_base,
			attr->mr_iov->iov_len);
		goto err;
	}

	return 0;
err:
	free(rxr_mr);
	return ret;
}

int rxr_mr_regv(struct fid *domain_fid, const struct iovec *iov,
		size_t count, uint64_t access, uint64_t offset,
		uint64_t requested_key, uint64_t flags,
		struct fid_mr **mr_fid, void *context)
{
	struct fi_mr_attr attr;

	attr.mr_iov = iov;
	attr.iov_count = count;
	attr.access = access;
	attr.offset = offset;
	attr.requested_key = requested_key;
	attr.context = context;
	return rxr_mr_regattr(domain_fid, &attr, flags, mr_fid);
}

static int rxr_mr_reg(struct fid *domain_fid, const void *buf, size_t len,
		      uint64_t access, uint64_t offset,
		      uint64_t requested_key, uint64_t flags,
		      struct fid_mr **mr, void *context)
{
	struct iovec iov;

	iov.iov_base = (void *)buf;
	iov.iov_len = len;
	return rxr_mr_regv(domain_fid, &iov, 1, access, offset, requested_key,
			   flags, mr, context);
}

static struct fi_ops_mr rxr_domain_mr_ops = {
	.size = sizeof(struct fi_ops_mr),
	.reg = rxr_mr_reg,
	.regv = rxr_mr_regv,
	.regattr = rxr_mr_regattr,
};

int rxr_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		    struct fid_domain **domain, void *context)
{
	int ret, retv;
	struct fi_info *rdm_info;
	struct rxr_domain *rxr_domain;
	struct rxr_fabric *rxr_fabric;

	rxr_fabric = container_of(fabric, struct rxr_fabric,
				  util_fabric.fabric_fid);

	if (info->ep_attr->type == FI_EP_DGRAM)
		return fi_domain(rxr_fabric->lower_fabric, info, domain,
				 context);

	rxr_info.addr_format = info->addr_format;

	/*
	 * Set the RxR's tx/rx size here based on core provider the user
	 * selected so that ofi_prov_check_info succeeds.
	 *
	 * TODO: handle the case where a single process opens multiple domains
	 * with different core providers
	 */
	rxr_info.tx_attr->size = info->tx_attr->size;
	rxr_info.rx_attr->size = info->rx_attr->size;

	rxr_domain = calloc(1, sizeof(*rxr_domain));
	if (!rxr_domain)
		return -FI_ENOMEM;

	ret = rxr_get_lower_rdm_info(fabric->api_version, NULL, NULL, 0,
				     &rxr_util_prov, info, &rdm_info);
	if (ret)
		goto err_free_domain;

	ret = fi_domain(rxr_fabric->lower_fabric, rdm_info,
			&rxr_domain->rdm_domain, context);
	if (ret)
		goto err_free_core_info;

	rxr_domain->rdm_mode = rdm_info->mode;
	rxr_domain->addrlen = (info->src_addr) ?
				info->src_addrlen : info->dest_addrlen;
	rxr_domain->cq_size = MAX(info->rx_attr->size + info->tx_attr->size,
				  rxr_env.cq_size);
	rxr_domain->mr_local = ofi_mr_local(rdm_info);
	rxr_domain->resource_mgmt = rdm_info->domain_attr->resource_mgmt;

	ret = ofi_domain_init(fabric, info, &rxr_domain->util_domain, context);
	if (ret)
		goto err_close_core_domain;

	rxr_domain->do_progress = 0;

	/*
	 * ofi_domain_init() would have stored the RxR mr_modes in the map, but
	 * we need the rbtree insertions and lookups to use the lower-provider
	 * specific key, since the latter can not support application keys
	 * (FI_MR_PROV_KEY only). Storing the lower provider's mode in the map
	 * instead.
	 */
	rxr_domain->util_domain.mr_map.mode |=
				OFI_MR_BASIC_MAP | FI_MR_LOCAL | FI_MR_BASIC;

	*domain = &rxr_domain->util_domain.domain_fid;
	(*domain)->fid.ops = &rxr_domain_fi_ops;
	(*domain)->ops = &rxr_domain_ops;
	(*domain)->mr = &rxr_domain_mr_ops;
	fi_freeinfo(rdm_info);
	return 0;

err_close_core_domain:
	retv = fi_close(&rxr_domain->rdm_domain->fid);
	if (retv)
		FI_WARN(&rxr_prov, FI_LOG_DOMAIN,
			"Unable to close domain: %s\n", fi_strerror(-retv));
err_free_core_info:
	fi_freeinfo(rdm_info);
err_free_domain:
	free(rxr_domain);
	return ret;
}
