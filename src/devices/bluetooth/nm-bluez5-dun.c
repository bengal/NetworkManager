/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2014 Red Hat, Inc.
 */

#include <config.h>
#include <sys/socket.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <bluetooth/rfcomm.h>
#include <net/ethernet.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "nm-bluez5-dun.h"
#include "nm-bt-error.h"
#include "nm-logging.h"
#include "NetworkManagerUtils.h"

void
nm_bluez5_dun_cleanup (int sk, int rfcomm_id)
{
	struct rfcomm_dev_req req = { 0 };

	g_return_val_if_fail (sk >= 0, FALSE);

	if (rfcomm_id >= 0) {
		req.dev_id = rfcomm_id;
		ioctl (sk, RFCOMMRELEASEDEV, &req);
	}
	close (sk);
}

static void
setba (bdaddr_t *dst, const guint8 src[ETH_ALEN])
{
	register guint8 *d = (guint8 *) dst;
	register const guint8 *s = (const guint8 *) src;
	register int i;

	memcpy (dst, src, ETH_ALEN);
	for (i = 0; i < 6; i++)
		d[i] = s[5-i];
}

typedef struct {
	sdp_session_t              *session;
	guint                       watch_id;
	NMBluez5DunFindChannelFunc  callback;
	gpointer                    user_data;
} SdpInfo;

static void
sdp_search_cleanup (SdpInfo *info)
{
	sdp_close (info->session);
	if (info->watch_id)
		g_source_remove (info->watch_id);
	g_free (info);
}

static gboolean
sdp_connect_watch (GIOChannel *channel, GIOCondition condition, gpointer user_data)
{
	SdpInfo *info = user_data;
	sdp_list_t *search, *attrs;
	uuid_t svclass;
	uint16_t attr;
	int fd, err, fd_err = 0;
	socklen_t len = sizeof (fd_err);

	info->watch_id = 0;

	fd = g_io_channel_unix_get_fd (channel);
	if (getsockopt (fd, SOL_SOCKET, SO_ERROR, &fd_err, &len) < 0)
		err = -errno;
	else
		err = -fd_err;

	if (err != 0)
		goto failed;

	if (sdp_set_notify (info->session, search_completed_cb, ctxt) < 0) {
		err = -EIO;
		goto failed;
	}

	sdp_uuid16_create (&svclass, DIALUP_NET_SVCLASS_ID);
	search = sdp_list_append (NULL, &svclass);
	attr = SDP_ATTR_PROTO_DESC_LIST;
	attrs = sdp_list_append (NULL, &attr);

	if (!sdp_service_search_attr_async (info->session, search, SDP_ATTR_REQ_INDIVIDUAL, attrs)) {
		/* Set callback responsible for update the internal SDP transaction */
		info->watch_id = g_io_add_watch (channel,
		                                 G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
		                                 sdp_search_process_cb,
		                                 info);
	}

done:
	sdp_list_free (attrs, NULL);
	sdp_list_free (search, NULL);
	return G_SOURCE_REMOVE;
}

gconstpointer
nm_bluez5_dun_find_channel (const guint8 adapter[ETH_ALEN],
                            const guint8 remote[ETH_ALEN],
                            NMBluez5DunFindChannelFunc callback,
                            gpointer user_data,
                            GError **error)
{
	SdpInfo *info;
	sdp_session_t *s;
	GError *error = NULL;
	bdaddr_t src, dst;
	GSimpleAsyncResult *simple;
	GIOChannel *channel;

	setba (&src, adapter);
	setba (&dst, remote);
	s = sdp_connect (&src, &dst, SDP_NON_BLOCKING);
	if (!s) {
		g_set_error (error, NM_BT_ERROR, NM_BT_ERROR_DUN_CONNECT_FAILED,
		             "(" MAC_FMT "): failed to connect to the SDP server: (%d) %s",
		             MAC_ARG (adapter), errno, strerror (errno));
		return NULL;
	}

	info = g_slice_new0 (SdpInfo);
	info->session = s;
	info->callback = callback;
	info->user_data = user_data;
	channel = g_io_channel_unix_new (sdp_get_socket (s));
	info->watch_id = g_io_add_watch (channel,
	                                 G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
	                                 sdp_connect_watch,
	                                 info);
	g_io_channel_unref (channel);
	return info;
}

static gboolean
dun_create_tty (int sk,
                const guint8 adapter[ETH_ALEN],
                const guint8 remote[ETH_ALEN],
                int channel,
                char **out_rfcomm_tty,
                int *out_rfcomm_id,
                GError **error)
{
	struct stat st;
	int devid, try = 30;
	char tty[100];
	const int ttylen = sizeof (tty) - 1;

	struct rfcomm_dev_req req = {
		.flags = (1 << RFCOMM_REUSE_DLC) | (1 << RFCOMM_RELEASE_ONHUP),
		.dev_id = -1,
		.channel = channel
	};

	g_return_val_if_fail (channel >= 0, FALSE);
	g_return_val_if_fail (out_rfcomm_tty, FALSE);
	g_return_val_if_fail (*out_rfcomm_tty == NULL, FALSE);
	g_return_val_if_fail (out_rfcomm_id, FALSE);

	setba (&req.src, adapter);
	setba (&req.dst, remote);
	devid = ioctl (sk, RFCOMMCREATEDEV, &req);
	if (devid < 0) {
		g_set_error (error, NM_BT_ERROR, NM_BT_ERROR_DUN_CONNECT_FAILED,
		             "(" MAC_FMT "): failed to create rfcomm device: (%d) %s",
		             MAC_ARG (adapter), errno, strerror (errno));
		return FALSE;
	}

	snprintf (tty, ttylen, "/dev/rfcomm%d", devid);
	while (stat (tty, &st) < 0 && try--) {
		snprintf (tty, ttylen, "/dev/rfcomm%d", devid);
		if (try--) {
			g_usleep (100 * 1000);
			continue;
		}

		g_set_error (error, NM_BT_ERROR, NM_BT_ERROR_DUN_CONNECT_FAILED,
		             "(" MAC_FMT "): failed to find rfcomm device",
		             MAC_ARG (adapter));
		return FALSE;
	}

	*out_rfcomm_tty = g_strdup (tty);
	return TRUE;
}

	sdp_list_t *srch, *attrs, *rsp;
	uuid_t svclass;
	uint16_t attr;
	int ch = 0;

	sdp_uuid16_create (&svclass, DIALUP_NET_SVCLASS_ID);
	srch = sdp_list_append (NULL, &svclass);
	attr = SDP_ATTR_PROTO_DESC_LIST;
	attrs = sdp_list_append (NULL, &attr);

	if (sdp_service_search_attr_req (s, srch, SDP_ATTR_REQ_INDIVIDUAL, attrs, &rsp) == 0) {
		for(; rsp; rsp = rsp->next) {
			sdp_record_t *rec = (sdp_record_t *) rsp->data;
			sdp_list_t *protos;

			if (sdp_get_access_protos (rec, &protos) == 0) {
				ch = sdp_get_proto_port (protos, RFCOMM_UUID);
				if (ch > 0)
					break;
			}
		}
	}

	sdp_close (s);
	return MAX (0, ch);
}

gboolean
nm_bluez5_dun_connect (const guint8 adapter[ETH_ALEN],
                       const guint8 remote[ETH_ALEN],
                       int rfcomm_channel,
                       int *out_rfcomm_fd,
                       char **out_rfcomm_dev,
                       int *out_rfcomm_id,
                       GError **error)
{
	struct sockaddr_rc sa;
	int sk;

	g_return_val_if_fail (rfcomm_channel >= 0), FALSE);

	sk = socket (AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
	if (sk < 0) {
		g_set_error (error, NM_BT_ERROR, NM_BT_ERROR_DUN_CONNECT_FAILED,
		             "Failed to create RFCOMM socket: (%d) %s",
		             errno, strerror (errno));
		return FALSE;
	}

	sa.rc_family = AF_BLUETOOTH;
	sa.rc_channel = 0;
	setba (&sa.rc_bdaddr, adapter);
	if (bind (sk, (struct sockaddr *) &sa, sizeof(sa))) {
		nm_log_dbg (LOGD_BT, "(" MAC_FMT "): failed to bind socket: (%d) %s",
		            MAC_ARG (adapter), errno, strerror (errno));
	}

	sa.rc_channel = rfcomm_channel;
	setba (&sa.rc_bdaddr, remote);
	if (connect (sk, (struct sockaddr *) &sa, sizeof (sa)) ) {
		g_set_error (error, NM_BT_ERROR, NM_BT_ERROR_DUN_CONNECT_FAILED,
		             "Failed to connect to remote device: (%d) %s",
		             errno, strerror (errno));
		goto error;
	} 

	nm_log_dbg (LOGD_BT, "(" MAC_FMT "): connected to " MAC_FMT "on channel %d",
	            MAC_ARG (adapter), MAC_ARG (remote), ch);

	if (!dun_create_tty (sk, adapter, remote, rfcomm_channel, out_rfcomm_dev, out_rfcomm_id, error))
		goto error;

	*out_rfcomm_fd = sk;
	return TRUE;

error:
	close (sk);
	return FALSE;
}
