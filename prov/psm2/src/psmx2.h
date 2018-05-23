/*
 * Copyright (c) 2013-2017 Intel Corporation. All rights reserved.
 * Copyright (c) 2016 Cisco Systems, Inc. All rights reserved.
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

#ifndef _FI_PSM2_H
#define _FI_PSM2_H

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <complex.h>
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_atomic.h>
#include <rdma/fi_trigger.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_errno.h>
#include "fi.h"
#include "ofi_atomic.h"
#include "fi_enosys.h"
#include "fi_list.h"
#include "fi_util.h"
#include "rbtree.h"
#include "version.h"

extern struct fi_provider psmx2_prov;

#define PSMX2_VERSION	(FI_VERSION(1,5))

#define PSMX2_OP_FLAGS	(FI_INJECT | FI_MULTI_RECV | FI_COMPLETION | \
			 FI_TRIGGER | FI_INJECT_COMPLETE | \
			 FI_TRANSMIT_COMPLETE | FI_DELIVERY_COMPLETE)

#define PSMX2_CAPS	(FI_TAGGED | FI_MSG | FI_ATOMICS | \
			 FI_RMA | FI_MULTI_RECV | \
                         FI_READ | FI_WRITE | FI_SEND | FI_RECV | \
                         FI_REMOTE_READ | FI_REMOTE_WRITE | \
			 FI_TRIGGER | FI_RMA_EVENT | FI_REMOTE_CQ_DATA | \
			 FI_SOURCE | FI_SOURCE_ERR | FI_DIRECTED_RECV | \
			 FI_NAMED_RX_CTX)

#define PSMX2_SUB_CAPS	(FI_READ | FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE | \
			 FI_SEND | FI_RECV)

#define PSMX2_DOM_CAPS	(FI_LOCAL_COMM | FI_REMOTE_COMM)

#define PSMX2_MAX_TRX_CTXT	(80)
#define PSMX2_ALL_TRX_CTXT	((void *)-1)
#define PSMX2_MAX_MSG_SIZE	((0x1ULL << 32) - 1)
#define PSMX2_INJECT_SIZE	(64)
#define PSMX2_MSG_ORDER		FI_ORDER_SAS
#define PSMX2_COMP_ORDER	FI_ORDER_NONE

#define PSMX2_MSG_BIT	(0x80000000)
#define PSMX2_RMA_BIT	(0x40000000)
#define PSMX2_IOV_BIT	(0x20000000)
#define PSMX2_IMM_BIT	(0x10000000)
#define PSMX2_SEQ_BITS	(0x0FFF0000)
#define PSMX2_SRC_BITS	(0x0000FF00)
#define PSMX2_DST_BITS	(0x000000FF)

#define PSMX2_TAG32(base, src, dst)	((base) | ((src)<<8) | (dst))
#define PSMX2_TAG32_GET_SRC(tag32)	(((tag32) & PSMX2_SRC_BITS) >> 8)
#define PSMX2_TAG32_GET_DST(tag32)	((tag32) & PSMX2_DST_BITS)
#define PSMX2_TAG32_GET_SEQ(tag32)	(((tag32) & PSMX2_SEQ_BITS) >> 16)
#define PSMX2_TAG32_SET_SEQ(tag32,seq)	do { \
						tag32 |= ((seq << 16) & PSMX2_SEQ_BITS); \
					} while (0)

#define PSMX2_SET_TAG(tag96,tag64,tag32) do { \
						tag96.tag0 = (uint32_t)tag64; \
						tag96.tag1 = (uint32_t)(tag64>>32); \
						tag96.tag2 = tag32; \
					} while (0)
#define PSMX2_SET_TAG_FIRST64(tag96,tag64) do { \
						memcpy(&(tag96).tag0, &(tag64), sizeof(uint64_t)); \
					} while (0)
#define PSMX2_SET_TAG_LAST32(tag96,tag32) do { \
						tag96.tag2 = tag32; \
					} while (0)

#define PSMX2_GET_TAG64(tag96)		(psmx2_get_tag64(&(tag96)))

static inline uint64_t psmx2_get_tag64(const psm2_mq_tag_t *tag96)
{
	uint64_t tag64;

	memcpy(&tag64, &tag96->tag0, sizeof(tag64));
	return tag64;
}

/* When using the long RMA protocol, set a bit in the unused SEQ bits to
 * indicate whether or not the operation is a read or a write. This prevents tag
 * collisions. */
#define PSMX2_TAG32_LONG_WRITE(tag32) PSMX2_TAG32_SET_SEQ(tag32, 0x1)
#define PSMX2_TAG32_LONG_READ(tag32)  PSMX2_TAG32_SET_SEQ(tag32, 0x2)

/*
 * Canonical virtual address on X86_64 only uses 48 bits and the higher 16 bits
 * are sign extensions. We can put some extra information into the 16 bits.
 *
 * Here is the layout:  AA-B-C-DDDDDDDDDDDD
 *
 * C == 0xE: scalable endpoint, AAB is context index, DDDDDDDDDDDD is the address
 * C != 0xE: regular endpoint, AA is vlane, BCDDDDDDDDDDDD is epaddr
 */
#define PSMX2_MAX_VL			(0xFF)
#define PSMX2_EP_MASK			(0x00FFFFFFFFFFFFFFUL)
#define PSMX2_SIGN_MASK  		(0x0080000000000000UL)
#define PSMX2_SIGN_EXT			(0xFF00000000000000UL)
#define PSMX2_VL_MASK			(0xFF00000000000000UL)

#define PSMX2_EP_TO_ADDR(ep,vl)		((((uint64_t)vl) << 56) | \
						((uint64_t)ep & PSMX2_EP_MASK))
#define PSMX2_ADDR_TO_VL(addr)		((uint8_t)((addr & PSMX2_VL_MASK) >> 56))
#define PSMX2_ADDR_TO_EP(addr)		((psm2_epaddr_t) \
						((addr & PSMX2_SIGN_MASK) ? \
                                                 (addr | PSMX2_SIGN_EXT) : \
                                                 (addr & PSMX2_EP_MASK)))

#define PSMX2_MAX_RX_CTX_BITS		(12)
#define PSMX2_SEP_ADDR_FLAG		(0x000E000000000000UL)
#define PSMX2_SEP_ADDR_MASK		(0x000F000000000000UL)
#define PSMX2_SEP_CTXT_MASK		(0xFFF0000000000000UL)
#define PSMX2_SEP_IDX_MASK		(0x0000FFFFFFFFFFFFUL)
#define PSMX2_SEP_ADDR_TEST(addr)	(((addr) & PSMX2_SEP_ADDR_MASK) == PSMX2_SEP_ADDR_FLAG)
#define PSMX2_SEP_ADDR_CTXT(addr, ctxt_bits) \
					(((addr) & PSMX2_SEP_CTXT_MASK) >> (64-(ctxt_bits)))
#define PSMX2_SEP_ADDR_IDX(addr)	((addr) & PSMX2_SEP_IDX_MASK)

/* Bits 60 .. 63 of the flag are provider specific */
#define PSMX2_NO_COMPLETION	(1ULL << 60)


enum psmx2_context_type {
	PSMX2_NOCOMP_SEND_CONTEXT = 1,
	PSMX2_NOCOMP_RECV_CONTEXT,
	PSMX2_NOCOMP_WRITE_CONTEXT,
	PSMX2_NOCOMP_READ_CONTEXT,
	PSMX2_SEND_CONTEXT,
	PSMX2_RECV_CONTEXT,
	PSMX2_MULTI_RECV_CONTEXT,
	PSMX2_TSEND_CONTEXT,
	PSMX2_TRECV_CONTEXT,
	PSMX2_WRITE_CONTEXT,
	PSMX2_READ_CONTEXT,
	PSMX2_REMOTE_WRITE_CONTEXT,
	PSMX2_REMOTE_READ_CONTEXT,
	PSMX2_SENDV_CONTEXT,
	PSMX2_IOV_SEND_CONTEXT,
	PSMX2_IOV_RECV_CONTEXT,
	PSMX2_NOCOMP_RECV_CONTEXT_ALLOC
};

struct psmx2_context {
	struct fi_context fi_context;
	struct slist_entry list_entry;
};

union psmx2_pi {
	void	*p;
	uint32_t i[2];
};

#define PSMX2_CTXT_REQ(fi_context)	((fi_context)->internal[0])
#define PSMX2_CTXT_TYPE(fi_context)	(((union psmx2_pi *)&(fi_context)->internal[1])->i[0])
#define PSMX2_CTXT_SIZE(fi_context)	(((union psmx2_pi *)&(fi_context)->internal[1])->i[1])
#define PSMX2_CTXT_USER(fi_context)	((fi_context)->internal[2])
#define PSMX2_CTXT_EP(fi_context)	((fi_context)->internal[3])

#define PSMX2_AM_RMA_HANDLER	0
#define PSMX2_AM_ATOMIC_HANDLER	1
#define PSMX2_AM_SEP_HANDLER	2

#define PSMX2_AM_OP_MASK	0x000000FF
#define PSMX2_AM_DST_MASK	0x0000FF00
#define PSMX2_AM_SRC_MASK	0x00FF0000
#define PSMX2_AM_FLAG_MASK	0xFF000000
#define PSMX2_AM_EOM		0x40000000
#define PSMX2_AM_DATA		0x20000000
#define PSMX2_AM_FORCE_ACK	0x10000000

#define PSMX2_AM_SET_OP(u32w0,op)	do {u32w0 &= ~PSMX2_AM_OP_MASK; u32w0 |= op;} while (0)
#define PSMX2_AM_SET_DST(u32w0,vl)	do {u32w0 &= ~PSMX2_AM_DST_MASK; u32w0 |= ((uint32_t)vl << 8);} while (0)
#define PSMX2_AM_SET_SRC(u32w0,vl)	do {u32w0 &= ~PSMX2_AM_SRC_MASK; u32w0 |= ((uint32_t)vl << 16);} while (0)
#define PSMX2_AM_SET_FLAG(u32w0,flag)	do {u32w0 &= ~PSMX2_AM_FLAG_MASK; u32w0 |= flag;} while (0)
#define PSMX2_AM_GET_OP(u32w0)		(u32w0 & PSMX2_AM_OP_MASK)
#define PSMX2_AM_GET_DST(u32w0)		((uint8_t)((u32w0 & PSMX2_AM_DST_MASK) >> 8))
#define PSMX2_AM_GET_SRC(u32w0)		((uint8_t)((u32w0 & PSMX2_AM_SRC_MASK) >> 16))
#define PSMX2_AM_GET_FLAG(u32w0)	(u32w0 & PSMX2_AM_FLAG_MASK)

enum {
	PSMX2_AM_REQ_WRITE = 1,
	PSMX2_AM_REQ_WRITE_LONG,
	PSMX2_AM_REP_WRITE,
	PSMX2_AM_REQ_READ,
	PSMX2_AM_REQ_READ_LONG,
	PSMX2_AM_REP_READ,
	PSMX2_AM_REQ_ATOMIC_WRITE,
	PSMX2_AM_REP_ATOMIC_WRITE,
	PSMX2_AM_REQ_ATOMIC_READWRITE,
	PSMX2_AM_REP_ATOMIC_READWRITE,
	PSMX2_AM_REQ_ATOMIC_COMPWRITE,
	PSMX2_AM_REP_ATOMIC_COMPWRITE,
	PSMX2_AM_REQ_WRITEV,
	PSMX2_AM_REQ_READV,
	PSMX2_AM_REQ_SEP_QUERY,
	PSMX2_AM_REP_SEP_QUERY,
};

struct psmx2_am_request {
	int op;
	union {
		struct {
			uint8_t	*buf;
			size_t	len;
			uint64_t addr;
			uint64_t key;
			void	*context;
			void	*peer_addr;
			uint8_t	vl;
			uint8_t	peer_vl;
			uint64_t data;
		} write;
		struct {
			union {
				uint8_t	*buf;	   /* for read */
				size_t	iov_count; /* for readv */
			};
			size_t	len;
			uint64_t addr;
			uint64_t key;
			void	*context;
			void	*peer_addr;
			uint8_t	vl;
			uint8_t	peer_vl;
			size_t	len_read;
		} read;
		struct {
			union {
				uint8_t	*buf;	   /* for result_count == 1 */
				size_t	iov_count; /* for result_count > 1 */
			};
			size_t	len;
			uint64_t addr;
			uint64_t key;
			void	*context;
			uint8_t *result;
			int	datatype;
		} atomic;
	};
	uint64_t cq_flags;
	struct fi_context fi_context;
	struct psmx2_fid_ep *ep;
	int no_event;
	int error;
	struct slist_entry list_entry;
	union {
		struct iovec iov[0];	/* for readv, must be the last field */
		struct fi_ioc ioc[0];	/* for atomic read, must be the last field */
	};
};

#define PSMX2_IOV_PROTO_PACK	0
#define PSMX2_IOV_PROTO_MULTI	1
#define PSMX2_IOV_MAX_SEQ_NUM	0x0FFF
#define PSMX2_IOV_BUF_SIZE	PSMX2_INJECT_SIZE
#define PSMX2_IOV_MAX_COUNT	(PSMX2_IOV_BUF_SIZE / sizeof(uint32_t) - 3)

struct psmx2_iov_info {
	uint32_t seq_num;
	uint32_t total_len;
	uint32_t count;
	uint32_t len[PSMX2_IOV_MAX_COUNT];
};

struct psmx2_sendv_request {
	struct fi_context fi_context;
	struct fi_context fi_context_iov;
	void *user_context;
	int iov_protocol;
	int no_completion;
	int comp_flag;
	uint32_t iov_done;
	union {
		struct psmx2_iov_info iov_info;
		char buf[PSMX2_IOV_BUF_SIZE];
	};
};

struct psmx2_sendv_reply {
	struct fi_context fi_context;
	int no_completion;
	int multi_recv;
	uint8_t *buf;
	void *user_context;
	size_t iov_done;
	size_t bytes_received;
	size_t msg_length;
	int error_code;
	int comp_flag;
	struct psmx2_iov_info iov_info;
};

struct psmx2_req_queue {
	fastlock_t	lock;
	struct slist	list;
};

struct psmx2_multi_recv {
	psm2_epaddr_t	src_addr;
	psm2_mq_tag_t	tag;
	psm2_mq_tag_t	tagsel;
	uint8_t		*buf;
	size_t		len;
	size_t		offset;
	int		min_buf_size;
	int		flag;
	void		*context;
};

struct psmx2_fid_fabric {
	struct util_fabric	util_fabric;
	struct psmx2_fid_domain	*active_domain;
	psm2_uuid_t		uuid;
	struct util_ns		name_server;
};

struct psmx2_trx_ctxt {
	psm2_ep_t		psm2_ep;
	psm2_epid_t		psm2_epid;
	psm2_mq_t		psm2_mq;
	int			am_initialized;
	int			id;
	struct psm2_am_parameters psm2_am_param;

	/* ep bound to this tx/rx context, NULL if multiplexed */
	struct psmx2_fid_ep	*ep;

	/* incoming req queue for AM based RMA request. */
	struct psmx2_req_queue	rma_queue;

	/* triggered operations that are ready to be processed */
	struct psmx2_req_queue	trigger_queue;

	/* lock to prevent the sequence of psm2_mq_ipeek and psm2_mq_test be
	 * interleaved in a multithreaded environment.
	 */
	fastlock_t		poll_lock;

	struct dlist_entry	entry;
};

struct psmx2_fid_domain {
	struct util_domain	util_domain;
	struct psmx2_fid_fabric	*fabric;
	uint64_t		mode;
	uint64_t		caps;

	enum fi_mr_mode		mr_mode;
	fastlock_t		mr_lock;
	uint64_t		mr_reserved_key;
	RbtHandle		mr_map;

	/*
	 * A list of all opened hw contexts, including the base hw context.
	 * The list is used for making progress.
	 */
	fastlock_t		trx_ctxt_lock;
	struct dlist_entry	trx_ctxt_list;

	/*
	 * The base hw context is multiplexed for all regular endpoints via
	 * logical "virtual lanes".
	 */
	struct psmx2_trx_ctxt	*base_trx_ctxt;
	fastlock_t		vl_lock;
	uint64_t		vl_map[(PSMX2_MAX_VL+1)/sizeof(uint64_t)];
	int			vl_alloc;
	struct psmx2_fid_ep	*eps[PSMX2_MAX_VL+1];

	ofi_atomic32_t		sep_cnt;
	fastlock_t		sep_lock;
	struct dlist_entry	sep_list;

	int			progress_thread_enabled;
	pthread_t		progress_thread;

	int			addr_format;
};

#define PSMX2_EP_REGULAR	0
#define PSMX2_EP_SCALABLE	1
#define PSMX2_EP_SRC_ADDR	2

#define PSMX2_RESERVED_EPID	(0xFFFFULL)
#define PSMX2_DEFAULT_UNIT	(-1)
#define PSMX2_DEFAULT_PORT	0
#define PSMX2_ANY_SERVICE	0

struct psmx2_ep_name {
	psm2_epid_t		epid;
	uint8_t			type;
	union {
		uint8_t		vlane;		/* for regular ep */
		uint8_t		sep_id;		/* for scalable ep */
		int8_t		unit;		/* for src addr. start from 0. -1 means any */
	};
	uint8_t			port;		/* for src addr. start from 1, 0 means any */
	uint8_t			padding;
	uint32_t		service;	/* for src addr. 0 means any */
};

#define PSMX2_MAX_STRING_NAME_LEN	64	/* "fi_addr_psmx2://<uint64_t>:<uint64_t>"  */

struct psmx2_cq_event {
	union {
		struct fi_cq_entry		context;
		struct fi_cq_msg_entry		msg;
		struct fi_cq_data_entry		data;
		struct fi_cq_tagged_entry	tagged;
		struct fi_cq_err_entry		err;
	} cqe;
	int error;
	int source_is_valid;
	fi_addr_t source;
	struct psmx2_fid_av *source_av;
	struct slist_entry list_entry;
};

#define PSMX2_ERR_DATA_SIZE		64	/* large enough to hold a string address */

struct psmx2_fid_cq {
	struct fid_cq			cq;
	struct psmx2_fid_domain		*domain;
	struct psmx2_trx_ctxt		*trx_ctxt;
	int 				format;
	int				entry_size;
	size_t				event_count;
	struct slist			event_queue;
	struct slist			free_list;
	fastlock_t			lock;
	struct psmx2_cq_event		*pending_error;
	struct util_wait		*wait;
	int				wait_cond;
	int				wait_is_local;
	ofi_atomic32_t			signaled;
	uint8_t				error_data[PSMX2_ERR_DATA_SIZE];
};

enum psmx2_triggered_op {
	PSMX2_TRIGGERED_SEND,
	PSMX2_TRIGGERED_SENDV,
	PSMX2_TRIGGERED_RECV,
	PSMX2_TRIGGERED_TSEND,
	PSMX2_TRIGGERED_TSENDV,
	PSMX2_TRIGGERED_TRECV,
	PSMX2_TRIGGERED_WRITE,
	PSMX2_TRIGGERED_WRITEV,
	PSMX2_TRIGGERED_READ,
	PSMX2_TRIGGERED_READV,
	PSMX2_TRIGGERED_ATOMIC_WRITE,
	PSMX2_TRIGGERED_ATOMIC_WRITEV,
	PSMX2_TRIGGERED_ATOMIC_READWRITE,
	PSMX2_TRIGGERED_ATOMIC_READWRITEV,
	PSMX2_TRIGGERED_ATOMIC_COMPWRITE,
	PSMX2_TRIGGERED_ATOMIC_COMPWRITEV,
};

struct psmx2_trigger {
	enum psmx2_triggered_op	op;
	struct psmx2_fid_cntr	*cntr;
	size_t			threshold;
	union {
		struct {
			struct fid_ep	*ep;
			const void	*buf;
			size_t		len;
			void		*desc;
			fi_addr_t	dest_addr;
			void		*context;
			uint64_t	flags;
			uint64_t	data;
		} send;
		struct {
			struct fid_ep	*ep;
			const struct iovec *iov;
			size_t		count;
			void		**desc;
			fi_addr_t	dest_addr;
			void		*context;
			uint64_t	flags;
			uint64_t	data;
		} sendv;
		struct {
			struct fid_ep	*ep;
			void		*buf;
			size_t		len;
			void		*desc;
			fi_addr_t	src_addr;
			void		*context;
			uint64_t	flags;
		} recv;
		struct {
			struct fid_ep	*ep;
			const void	*buf;
			size_t		len;
			void		*desc;
			fi_addr_t	dest_addr;
			uint64_t	tag;
			void		*context;
			uint64_t	flags;
			uint64_t	data;
		} tsend;
		struct {
			struct fid_ep	*ep;
			const struct iovec *iov;
			size_t		count;
			void		**desc;
			fi_addr_t	dest_addr;
			uint64_t	tag;
			void		*context;
			uint64_t	flags;
			uint64_t	data;
		} tsendv;
		struct {
			struct fid_ep	*ep;
			void		*buf;
			size_t		len;
			void		*desc;
			fi_addr_t	src_addr;
			uint64_t	tag;
			uint64_t	ignore;
			void		*context;
			uint64_t	flags;
		} trecv;
		struct {
			struct fid_ep	*ep;
			const void	*buf;
			size_t		len;
			void		*desc;
			fi_addr_t	dest_addr;
			uint64_t	addr;
			uint64_t	key;
			void		*context;
			uint64_t	flags;
			uint64_t	data;
		} write;
		struct {
			struct fid_ep	*ep;
			const struct iovec *iov;
			size_t		count;
			void		*desc;
			fi_addr_t	dest_addr;
			uint64_t	addr;
			uint64_t	key;
			void		*context;
			uint64_t	flags;
			uint64_t	data;
		} writev;
		struct {
			struct fid_ep	*ep;
			void		*buf;
			size_t		len;
			void		*desc;
			fi_addr_t	src_addr;
			uint64_t	addr;
			uint64_t	key;
			void		*context;
			uint64_t	flags;
		} read;
		struct {
			struct fid_ep	*ep;
			const struct iovec *iov;
			size_t		count;
			void		*desc;
			fi_addr_t	src_addr;
			uint64_t	addr;
			uint64_t	key;
			void		*context;
			uint64_t	flags;
		} readv;
		struct {
			struct fid_ep	*ep;
			const void	*buf;
			size_t		count;
			void		*desc;
			fi_addr_t	dest_addr;
			uint64_t	addr;
			uint64_t	key;
			enum fi_datatype datatype;
			enum fi_op	atomic_op;
			void		*context;
			uint64_t	flags;
		} atomic_write;
		struct {
			struct fid_ep	*ep;
			const struct fi_ioc *iov;
			size_t		count;
			void		*desc;
			fi_addr_t	dest_addr;
			uint64_t	addr;
			uint64_t	key;
			enum fi_datatype datatype;
			enum fi_op	atomic_op;
			void		*context;
			uint64_t	flags;
		} atomic_writev;
		struct {
			struct fid_ep	*ep;
			const void	*buf;
			size_t		count;
			void		*desc;
			void		*result;
			void		*result_desc;
			fi_addr_t	dest_addr;
			uint64_t	addr;
			uint64_t	key;
			enum fi_datatype datatype;
			enum fi_op	atomic_op;
			void		*context;
			uint64_t	flags;
		} atomic_readwrite;
		struct {
			struct fid_ep	*ep;
			const struct fi_ioc *iov;
			size_t		count;
			void		**desc;
			struct fi_ioc	*resultv;
			void		**result_desc;
			size_t		result_count;
			fi_addr_t	dest_addr;
			uint64_t	addr;
			uint64_t	key;
			enum fi_datatype datatype;
			enum fi_op	atomic_op;
			void		*context;
			uint64_t	flags;
		} atomic_readwritev;
		struct {
			struct fid_ep	*ep;
			const void	*buf;
			size_t		count;
			void		*desc;
			const void	*compare;
			void		*compare_desc;
			void		*result;
			void		*result_desc;
			fi_addr_t	dest_addr;
			uint64_t	addr;
			uint64_t	key;
			enum fi_datatype datatype;
			enum fi_op	atomic_op;
			void		*context;
			uint64_t	flags;
		} atomic_compwrite;
		struct {
			struct fid_ep	*ep;
			const struct fi_ioc *iov;
			size_t		count;
			void		**desc;
			const struct fi_ioc *comparev;
			void		**compare_desc;
			size_t		compare_count;
			struct fi_ioc	*resultv;
			void		**result_desc;
			size_t		result_count;
			fi_addr_t	dest_addr;
			uint64_t	addr;
			uint64_t	key;
			enum fi_datatype datatype;
			enum fi_op	atomic_op;
			void		*context;
			uint64_t	flags;
		} atomic_compwritev;
	};
	struct psmx2_trigger *next;	/* used for randomly accessed trigger list */
	struct slist_entry list_entry;	/* used for ready-to-fire trigger queue */
};

struct psmx2_fid_cntr {
	union {
		struct fid_cntr		cntr;
		struct util_cntr	util_cntr; /* for util_poll_run */
	};
	struct psmx2_fid_domain	*domain;
	struct psmx2_trx_ctxt	*trx_ctxt;
	int			events;
	uint64_t		flags;
	ofi_atomic64_t		counter;
	ofi_atomic64_t		error_counter;
	struct util_wait	*wait;
	int			wait_is_local;
	struct psmx2_trigger	*trigger;
	fastlock_t		trigger_lock;
};

struct psmx2_ctxt_addr {
	psm2_epid_t		epid;
	psm2_epaddr_t		*epaddrs;
};

struct psmx2_sep_addr {
	int			ctxt_cnt;
	struct psmx2_ctxt_addr	ctxt_addrs[];
};

struct psmx2_fid_av {
	struct fid_av		av;
	struct psmx2_fid_domain	*domain;
	struct fid_eq		*eq;
	int			type;
	int			addr_format;
	int			rx_ctx_bits;
	uint64_t		flags;
	size_t			addrlen;
	size_t			count;
	size_t			last;
	psm2_epid_t		*epids;
	psm2_epaddr_t		*epaddrs;
	uint8_t			*vlanes;
	uint8_t			*types;
	struct psmx2_sep_addr	**sepaddrs;
};

struct psmx2_fid_ep {
	struct fid_ep		ep;
	int			type;
	struct psmx2_fid_domain	*domain;
	/* above fields are common with sep */

	struct psmx2_trx_ctxt	*trx_ctxt;
	struct psmx2_fid_ep	*base_ep;
	struct psmx2_fid_av	*av;
	struct psmx2_fid_cq	*send_cq;
	struct psmx2_fid_cq	*recv_cq;
	struct psmx2_fid_cntr	*send_cntr;
	struct psmx2_fid_cntr	*recv_cntr;
	struct psmx2_fid_cntr	*write_cntr;
	struct psmx2_fid_cntr	*read_cntr;
	struct psmx2_fid_cntr	*remote_write_cntr;
	struct psmx2_fid_cntr	*remote_read_cntr;
	uint8_t			vlane;
	unsigned		send_selective_completion:1;
	unsigned		recv_selective_completion:1;
	unsigned		enabled:1;
	uint64_t		tx_flags;
	uint64_t		rx_flags;
	uint64_t		caps;
	ofi_atomic32_t		ref;
	struct fi_context	nocomp_send_context;
	struct fi_context	nocomp_recv_context;
	struct slist		free_context_list;
	fastlock_t		context_lock;
	size_t			min_multi_recv;
	uint32_t		iov_seq_num;
	int			service;
};

struct psmx2_sep_ctxt {
	struct psmx2_trx_ctxt	*trx_ctxt;
	struct psmx2_fid_ep	*ep;
};

struct psmx2_fid_sep {
	struct fid_ep		ep;
	int			type;
	struct psmx2_fid_domain	*domain;
	/* above fields are common with regular ep */

	struct dlist_entry	entry;

	ofi_atomic32_t		ref;
	int			service;
	uint8_t			id;
	uint8_t			enabled;
	size_t			ctxt_cnt;
	struct psmx2_sep_ctxt	ctxts[]; /* must be last element */
};

struct psmx2_fid_stx {
	struct fid_stx		stx;
	struct psmx2_fid_domain	*domain;
};

struct psmx2_fid_mr {
	struct fid_mr		mr;
	struct psmx2_fid_domain	*domain;
	struct psmx2_fid_cntr	*cntr;
	uint64_t		access;
	uint64_t		flags;
	uint64_t		offset;
	size_t			iov_count;
	struct iovec		iov[];	/* must be the last field */
};

struct psmx2_epaddr_context {
	struct psmx2_trx_ctxt	*trx_ctxt;
	psm2_epid_t		epid;
};

struct psmx2_env {
	int name_server;
	int tagged_rma;
	char *uuid;
	int delay;
	int timeout;
	int prog_interval;
	char *prog_affinity;
	int sep;
	int max_trx_ctxt;
	int sep_trx_ctxt;
	int num_devunits;
	int inject_size;
	int lock_level;
};

extern struct fi_ops_mr		psmx2_mr_ops;
extern struct fi_ops_cm		psmx2_cm_ops;
extern struct fi_ops_tagged	psmx2_tagged_ops;
extern struct fi_ops_tagged	psmx2_tagged_ops_no_flag_av_map;
extern struct fi_ops_tagged	psmx2_tagged_ops_no_flag_av_table;
extern struct fi_ops_tagged	psmx2_tagged_ops_no_event_av_map;
extern struct fi_ops_tagged	psmx2_tagged_ops_no_event_av_table;
extern struct fi_ops_tagged	psmx2_tagged_ops_no_send_event_av_map;
extern struct fi_ops_tagged	psmx2_tagged_ops_no_send_event_av_table;
extern struct fi_ops_tagged	psmx2_tagged_ops_no_recv_event_av_map;
extern struct fi_ops_tagged	psmx2_tagged_ops_no_recv_event_av_table;
extern struct fi_ops_msg	psmx2_msg_ops;
extern struct fi_ops_msg	psmx2_msg2_ops;
extern struct fi_ops_rma	psmx2_rma_ops;
extern struct fi_ops_atomic	psmx2_atomic_ops;
extern struct psmx2_env		psmx2_env;
extern struct psmx2_fid_fabric	*psmx2_active_fabric;

/*
 * Lock levels:
 *     0 -- always lock
 *     1 -- lock needed if there is more than one thread (including internal threads)
 *     2 -- lock needed if more then one thread accesses the same psm2 ep
 */
static inline void psmx2_lock(fastlock_t *lock, int lock_level)
{
	if (psmx2_env.lock_level >= lock_level)
		fastlock_acquire(lock);
}

static inline int psmx2_trylock(fastlock_t *lock, int lock_level)
{
	if (psmx2_env.lock_level >= lock_level)
		return fastlock_tryacquire(lock);
	else
		return 0;
}

static inline void psmx2_unlock(fastlock_t *lock, int lock_level)
{
	if (psmx2_env.lock_level >= lock_level)
		fastlock_release(lock);
}

#ifdef PSM2_MULTI_EP_CAP

static inline int psmx2_sep_ok(void)
{
	uint64_t caps = PSM2_MULTI_EP_CAP;
	return (psm2_get_capability_mask(caps) == caps);
}

static inline psm2_error_t psmx2_ep_epid_lookup(psm2_ep_t ep, psm2_epid_t epid,
						psm2_epconn_t *epconn)
{
	return psm2_ep_epid_lookup2(ep, epid, epconn);
}

static inline psm2_epid_t psmx2_epaddr_to_epid(psm2_epaddr_t epaddr)
{
	psm2_epid_t epid;

	/* Caller ensures that epaddr is not NULL */
	psm2_epaddr_to_epid(epaddr, &epid);
	return epid;
}

#else

static inline int psmx2_sep_ok(void)
{
	return 0;
}

static inline psm2_error_t psmx2_ep_epid_lookup(psm2_ep_t ep, psm2_epid_t epid,
						psm2_epconn_t *epconn)
{
	return psm2_ep_epid_lookup(epid, epconn);
}

static inline psm2_epid_t psmx2_epaddr_to_epid(psm2_epaddr_t epaddr)
{
	/*
	 * This is a hack based on the fact that the internal representation of
	 * epaddr has epid as the first field. This is a workaround before a PSM2
	 * function is availale to retrieve this information.
	 */
	return *(psm2_epid_t *)epaddr;
}

#endif /* PSM2_MULTI_EP_CAP */

int	psmx2_fabric(struct fi_fabric_attr *attr,
		    struct fid_fabric **fabric, void *context);
int	psmx2_domain_open(struct fid_fabric *fabric, struct fi_info *info,
			 struct fid_domain **domain, void *context);
int	psmx2_ep_open(struct fid_domain *domain, struct fi_info *info,
		     struct fid_ep **ep, void *context);
int	psmx2_sep_open(struct fid_domain *domain, struct fi_info *info,
		       struct fid_ep **sep, void *context);
int	psmx2_stx_ctx(struct fid_domain *domain, struct fi_tx_attr *attr,
		     struct fid_stx **stx, void *context);
int	psmx2_cq_open(struct fid_domain *domain, struct fi_cq_attr *attr,
		     struct fid_cq **cq, void *context);
int	psmx2_av_open(struct fid_domain *domain, struct fi_av_attr *attr,
		     struct fid_av **av, void *context);
int	psmx2_cntr_open(struct fid_domain *domain, struct fi_cntr_attr *attr,
		       struct fid_cntr **cntr, void *context);
int	psmx2_wait_open(struct fid_fabric *fabric, struct fi_wait_attr *attr,
			struct fid_wait **waitset);
int	psmx2_wait_trywait(struct fid_fabric *fabric, struct fid **fids,
			   int count);
int	psmx2_query_atomic(struct fid_domain *doamin, enum fi_datatype datatype,
			   enum fi_op op, struct fi_atomic_attr *attr,
			   uint64_t flags);

static inline void psmx2_fabric_acquire(struct psmx2_fid_fabric *fabric)
{
	ofi_atomic_inc32(&fabric->util_fabric.ref);
}

static inline void psmx2_fabric_release(struct psmx2_fid_fabric *fabric)
{
	ofi_atomic_dec32(&fabric->util_fabric.ref);
}

static inline void psmx2_domain_acquire(struct psmx2_fid_domain *domain)
{
	ofi_atomic_inc32(&domain->util_domain.ref);
}

static inline void psmx2_domain_release(struct psmx2_fid_domain *domain)
{
	ofi_atomic_dec32(&domain->util_domain.ref);
}

int	psmx2_domain_check_features(struct psmx2_fid_domain *domain, int ep_cap);
int	psmx2_domain_enable_ep(struct psmx2_fid_domain *domain, struct psmx2_fid_ep *ep);

void	psmx2_trx_ctxt_free(struct psmx2_trx_ctxt *trx_ctxt);
struct	psmx2_trx_ctxt *psmx2_trx_ctxt_alloc(struct psmx2_fid_domain *domain,
					     struct psmx2_ep_name *src_addr,
					     int sep_ctxt_idx);


static inline
int	psmx2_ns_service_cmp(void *svc1, void *svc2)
{
	int service1 = *(int *)svc1, service2 = *(int *)svc2;
	if (service1 == PSMX2_ANY_SERVICE ||
	    service2 == PSMX2_ANY_SERVICE)
		return 0;
	return (service1 < service2) ?
		-1 : (service1 > service2);
}
static inline
int	psmx2_ns_is_service_wildcard(void *svc)
{
	return (*(int *)svc == PSMX2_ANY_SERVICE);
}
void	psmx2_get_uuid(psm2_uuid_t uuid);
int	psmx2_uuid_to_port(psm2_uuid_t uuid);
char	*psmx2_uuid_to_string(psm2_uuid_t uuid);
void	*psmx2_ep_name_to_string(const struct psmx2_ep_name *name, size_t *len);
struct	psmx2_ep_name *psmx2_string_to_ep_name(const void *s);
int	psmx2_errno(int err);
void	psmx2_query_mpi(void);

struct	fi_context *psmx2_ep_get_op_context(struct psmx2_fid_ep *ep);
void	psmx2_ep_put_op_context(struct psmx2_fid_ep *ep, struct fi_context *fi_context);
void	psmx2_cq_enqueue_event(struct psmx2_fid_cq *cq, struct psmx2_cq_event *event);
struct	psmx2_cq_event *psmx2_cq_create_event(struct psmx2_fid_cq *cq,
					void *op_context, void *buf,
					uint64_t flags, size_t len,
					uint64_t data, uint64_t tag,
					size_t olen, int err);
int	psmx2_cq_poll_mq(struct psmx2_fid_cq *cq, struct psmx2_trx_ctxt *trx_ctxt,
			struct psmx2_cq_event *event, int count, fi_addr_t *src_addr);

psm2_epaddr_t psmx2_av_translate_sep(struct psmx2_fid_av *av,
				     struct psmx2_trx_ctxt *trx_ctxt, fi_addr_t addr);

void	psmx2_am_global_init(void);
void	psmx2_am_global_fini(void);
int	psmx2_am_init(struct psmx2_trx_ctxt *trx_ctxt);
void	psmx2_am_fini(struct psmx2_trx_ctxt *trx_ctxt);
int	psmx2_am_progress(struct psmx2_trx_ctxt *trx_ctxt);
int	psmx2_am_process_send(struct psmx2_trx_ctxt *trx_ctxt,
				struct psmx2_am_request *req);
int	psmx2_am_process_rma(struct psmx2_trx_ctxt *trx_ctxt,
				struct psmx2_am_request *req);
int	psmx2_process_trigger(struct psmx2_trx_ctxt *trx_ctxt,
				struct psmx2_trigger *trigger);
int	psmx2_am_rma_handler_ext(psm2_am_token_t token,
				 psm2_amarg_t *args, int nargs, void *src, uint32_t len,
				 struct psmx2_trx_ctxt *trx_ctxt);
int	psmx2_am_atomic_handler_ext(psm2_am_token_t token,
				    psm2_amarg_t *args, int nargs, void *src, uint32_t len,
				    struct psmx2_trx_ctxt *trx_ctxt);
int	psmx2_am_sep_handler(psm2_am_token_t token, psm2_amarg_t *args, int nargs,
			     void *src, uint32_t len);
void	psmx2_atomic_global_init(void);
void	psmx2_atomic_global_fini(void);

void	psmx2_am_ack_rma(struct psmx2_am_request *req);

struct	psmx2_fid_mr *psmx2_mr_get(struct psmx2_fid_domain *domain, uint64_t key);
int	psmx2_mr_validate(struct psmx2_fid_mr *mr, uint64_t addr, size_t len, uint64_t access);
void	psmx2_cntr_check_trigger(struct psmx2_fid_cntr *cntr);
void	psmx2_cntr_add_trigger(struct psmx2_fid_cntr *cntr, struct psmx2_trigger *trigger);

int	psmx2_handle_sendv_req(struct psmx2_fid_ep *ep, psm2_mq_status2_t *psm2_status,
			       int multi_recv);

static inline void psmx2_cntr_inc(struct psmx2_fid_cntr *cntr)
{
	ofi_atomic_inc64(&cntr->counter);
	psmx2_cntr_check_trigger(cntr);
	if (cntr->wait)
		cntr->wait->signal(cntr->wait);
}

fi_addr_t psmx2_av_translate_source(struct psmx2_fid_av *av, fi_addr_t source);

static inline void psmx2_get_source_name(fi_addr_t source, struct psmx2_ep_name *name)
{
	psm2_epaddr_t epaddr = PSMX2_ADDR_TO_EP(source);

	memset(name, 0, sizeof(*name));
	name->epid = psmx2_epaddr_to_epid(epaddr);
	name->vlane = PSMX2_ADDR_TO_VL(source);
	name->type = PSMX2_EP_REGULAR;
}

static inline void psmx2_get_source_string_name(fi_addr_t source, char *name, size_t *len)
{
	struct psmx2_ep_name ep_name;
	psm2_epaddr_t epaddr = PSMX2_ADDR_TO_EP(source);

	memset(&ep_name, 0, sizeof(ep_name));
	ep_name.epid = psmx2_epaddr_to_epid(epaddr);
	ep_name.vlane = PSMX2_ADDR_TO_VL(source);
	ep_name.type = PSMX2_EP_REGULAR;

	ofi_straddr(name, len, FI_ADDR_PSMX2, &ep_name);
}

static inline void psmx2_progress(struct psmx2_trx_ctxt *trx_ctxt)
{
	if (trx_ctxt) {
		psmx2_cq_poll_mq(NULL, trx_ctxt, NULL, 0, NULL);
		if (trx_ctxt->am_initialized)
			psmx2_am_progress(trx_ctxt);
	}
}

static inline void psmx2_progress_all(struct psmx2_fid_domain *domain)
{
	struct dlist_entry *item;
	struct psmx2_trx_ctxt *trx_ctxt;

	psmx2_lock(&domain->trx_ctxt_lock, 1);
	dlist_foreach(&domain->trx_ctxt_list, item) {
		trx_ctxt = container_of(item, struct psmx2_trx_ctxt, entry);
		psmx2_progress(trx_ctxt);
	}
	psmx2_unlock(&domain->trx_ctxt_lock, 1);
}

/* The following functions are used by triggered operations */

ssize_t psmx2_send_generic(
			struct fid_ep *ep,
			const void *buf, size_t len,
			void *desc, fi_addr_t dest_addr,
			void *context, uint64_t flags,
			uint64_t data);

ssize_t psmx2_sendv_generic(
			struct fid_ep *ep,
			const struct iovec *iov, void *desc,
			size_t count, fi_addr_t dest_addr,
			void *context, uint64_t flags,
			uint64_t data);

ssize_t psmx2_recv_generic(
			struct fid_ep *ep,
			void *buf, size_t len, void *desc,
			fi_addr_t src_addr, void *context,
			uint64_t flags);

ssize_t psmx2_tagged_send_generic(
			struct fid_ep *ep,
			const void *buf, size_t len,
			void *desc, fi_addr_t dest_addr,
			uint64_t tag, void *context,
			uint64_t flags, uint64_t data);

ssize_t psmx2_tagged_sendv_generic(
			struct fid_ep *ep,
			const struct iovec *iov, void *desc,
			size_t count, fi_addr_t dest_addr,
			uint64_t tag, void *context,
			uint64_t flags, uint64_t data);

ssize_t psmx2_tagged_recv_generic(
			struct fid_ep *ep,
			void *buf, size_t len,
			void *desc, fi_addr_t src_addr,
			uint64_t tag, uint64_t ignore,
			void *context, uint64_t flags);

ssize_t psmx2_write_generic(
			struct fid_ep *ep,
			const void *buf, size_t len,
			void *desc, fi_addr_t dest_addr,
			uint64_t addr, uint64_t key,
			void *context, uint64_t flags,
			uint64_t data);

ssize_t psmx2_writev_generic(
			struct fid_ep *ep,
			const struct iovec *iov, void **desc,
			size_t count, fi_addr_t dest_addr,
			uint64_t addr, uint64_t key,
			void *context, uint64_t flags,
			uint64_t data);

ssize_t psmx2_read_generic(
			struct fid_ep *ep,
			void *buf, size_t len,
			void *desc, fi_addr_t src_addr,
			uint64_t addr, uint64_t key,
			void *context, uint64_t flags);

ssize_t psmx2_readv_generic(
			struct fid_ep *ep,
			const struct iovec *iov, void *desc,
			size_t count, fi_addr_t src_addr,
			uint64_t addr, uint64_t key,
			void *context, uint64_t flags);

ssize_t psmx2_atomic_write_generic(
			struct fid_ep *ep,
			const void *buf,
			size_t count, void *desc,
			fi_addr_t dest_addr,
			uint64_t addr, uint64_t key,
			enum fi_datatype datatype,
			enum fi_op op, void *context,
			uint64_t flags);

ssize_t psmx2_atomic_readwrite_generic(
			struct fid_ep *ep,
			const void *buf,
			size_t count, void *desc,
			void *result, void *result_desc,
			fi_addr_t dest_addr,
			uint64_t addr, uint64_t key,
			enum fi_datatype datatype,
			enum fi_op op, void *context,
			uint64_t flags);

ssize_t psmx2_atomic_compwrite_generic(
			struct fid_ep *ep,
			const void *buf,
			size_t count, void *desc,
			const void *compare, void *compare_desc,
			void *result, void *result_desc,
			fi_addr_t dest_addr,
			uint64_t addr, uint64_t key,
			enum fi_datatype datatype,
			enum fi_op op, void *context,
			uint64_t flags);

#ifdef __cplusplus
}
#endif

#endif

