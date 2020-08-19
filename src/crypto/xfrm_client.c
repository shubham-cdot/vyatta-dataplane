/*
 * Copyright (c) 2020 AT&T Intellectual Property.  All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#include <czmq.h>
#include <libmnl/libmnl.h>
#include <linux/xfrm.h>
#include <rte_common.h>
#include <rte_log.h>
#include <rte_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_internal.h"
#include "control.h"
#include "event_internal.h"
#include "controller.h"
#include "netlink.h"
#include "xfrm_client.h"
#include "vplane_debug.h"
#include "vplane_log.h"
#include "zmq_dp.h"
#include "crypto/crypto_policy.h"

zsock_t *xfrm_pull_socket;
zsock_t *xfrm_push_socket;

/*
 * xfrm_direct indcates that xfrm messages are coming direct from
 * strongswan, rather than via vplaned. Updates from vplaned are not
 * batched and do not need to the acked, and the ack channel will not
 * be initialised.
 */
bool xfrm_direct;

/*
 * Build a message back to strongswan to indicates if the
 * xfrm message, with sequenece id 'seq', was successfully
 * processed or not.
 *
 * Strongswan expects a netlink error message, and result of the xfrm
 * processing is passed in the error field.
 */
int xfrm_client_send_ack(uint32_t seq, int err)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	struct nlmsgerr *err_msg;
	zframe_t *frame;
	int rc;

	nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_seq =  seq;
	nlh->nlmsg_type = NLMSG_ERROR;
	err_msg = mnl_nlmsg_put_extra_header(nlh, sizeof(*err_msg));
	if (!err_msg) {
		DP_DEBUG(CRYPTO, ERR, DATAPLANE,
			 "Failed to alloc xfrm ack error frame\n");
		return -1;
	}

	err_msg->error = (err == MNL_CB_OK) ? 0 : -EPERM;

	frame = zframe_new(nlh, nlh->nlmsg_len);
	if (!frame)
		return -1;

	rc = zframe_send(&frame, xfrm_push_socket, 0);
	if (rc < 0)
		zframe_destroy(&frame);

	return rc;
}

static int
dp_xfrm_msg_recv(zsock_t *sock, zmq_msg_t *msg)
{
	zmq_msg_init(msg);

	if (zmq_msg_recv(msg, zsock_resolve(sock), 0) <= 0)
		goto error;

	int more = zmq_msg_get(msg, ZMQ_MORE);
	while (more) {
		zmq_msg_t sink;
		zmq_msg_init(&sink);
		zmq_msg_recv(&sink, zsock_resolve(sock), 0);
		more = zmq_msg_get(&sink, ZMQ_MORE);
		zmq_msg_close(&sink);
	}

	return 0;
error:
	zmq_msg_close(msg);
	return -1;
}

static int xfrm_netlink_recv(void *arg)
{
	zmq_msg_t xfrm_msg;
	zsock_t *sock = arg;
	const struct nlmsghdr *nlh;
	uint32_t len;

	errno = 0;

	int rc = dp_xfrm_msg_recv(sock, &xfrm_msg);

	if (rc != 0) {
		if (errno == 0)
			return 0;
		return -1;
	}

	nlh = zmq_msg_data(&xfrm_msg);
	len = zmq_msg_size(&xfrm_msg);

	vrfid_t vrf_id = VRF_DEFAULT_ID;

	switch (nlh->nlmsg_type) {
	case XFRM_MSG_NEWPOLICY:
	case XFRM_MSG_UPDPOLICY:
	case XFRM_MSG_POLEXPIRE:
	case XFRM_MSG_DELPOLICY:
		rc = mnl_cb_run(nlh, len, 0, 0, rtnl_process_xfrm, &vrf_id);
		/* Policy acks are batched */
		break;

	case XFRM_MSG_NEWSA: /* fall through */
	case XFRM_MSG_UPDSA:
	case XFRM_MSG_DELSA:
	case XFRM_MSG_EXPIRE:
		rc = mnl_cb_run(nlh, len, 0, 0, rtnl_process_xfrm_sa, &vrf_id);
		break;
	default:
		rc = MNL_CB_ERROR;
	}

	if (rc != MNL_CB_OK) {
		DP_DEBUG(CRYPTO, ERR, DATAPLANE,
			 "XFRM netlink msg not handled\n");
	}
	xfrm_client_send_ack(nlh->nlmsg_seq, rc);

	zmq_msg_close(&xfrm_msg);

	return 0;
}

void xfrm_client_unsubscribe(void)
{
	if (xfrm_push_socket) {
		zsock_destroy(&xfrm_push_socket);
		xfrm_push_socket = NULL;
	}
	if (xfrm_pull_socket) {
		zsock_destroy(&xfrm_pull_socket);
		xfrm_pull_socket = NULL;
	}
}

int xfrm_client_init(void)
{
	/* Ensure we are not restarting  without cleanup */
	if (xfrm_pull_socket || xfrm_push_socket)
		rte_panic("Open xfrm socket");

	if (!config.xfrm_pull_url || !config.xfrm_push_url) {
		RTE_LOG(ERR, DATAPLANE, "No xfrm url");
		/* Once the cut over to the xfrm direct path
		 * is complete need to return -1
		 */
		return 0;
	}

	xfrm_pull_socket = zsock_new(ZMQ_PULL);
	if (!xfrm_pull_socket)
		rte_panic("failed to open xfrm socket");
	if (zsock_connect(xfrm_pull_socket, "%s", config.xfrm_pull_url) < 0)
		rte_panic("failed to open xfrm pull socket");

	xfrm_push_socket = zsock_new(ZMQ_PUSH);
	if (!xfrm_push_socket)
		rte_panic("failed to open xfrm socket");
	if (zsock_connect(xfrm_push_socket, "%s", config.xfrm_push_url) < 0)
		rte_panic("failed to open xfrm push socket");

	dp_register_event_socket(
		zsock_resolve(xfrm_pull_socket),
		xfrm_netlink_recv,
		xfrm_pull_socket);

	xfrm_direct = true;

	return 0;
}