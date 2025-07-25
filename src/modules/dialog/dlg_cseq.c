/*
 * Copyright (C) 2014 Daniel-Constantin Mierla (asipto.com)
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


/*!
 * \file
 * \brief Cseq handling
 * \ingroup dialog
 * Module: \ref dialog
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/events.h"
#include "../../core/ut.h"
#include "../../core/trim.h"
#include "../../core/data_lump.h"
#include "../../core/srapi.h"
#include "../../core/parser/parse_to.h"
#include "../../core/parser/parse_from.h"
#include "../../core/parser/parse_cseq.h"
#include "../../modules/tm/tm_load.h"

#include "dlg_handlers.h"
#include "dlg_var.h"
#include "dlg_cseq.h"
#include "dlg_hash.h"
#include "dlg_db_handler.h"

extern struct tm_binds d_tmb;

/**
 *
 */
static int dlg_cseq_prepare_msg(sip_msg_t *msg)
{
	LM_DBG("prepare msg for cseq update operations\n");

	if(msg->first_line.type==SIP_REQUEST) {
		if(!IS_SIP(msg)) {
			LM_DBG("non sip request message\n");
			return 1;
		}
	} else if(msg->first_line.type==SIP_REPLY) {
		if(!IS_SIP_REPLY(msg)) {
			LM_DBG("non sip reply message\n");
			return 1;
		}
	} else {
		LM_DBG("non sip message\n");
		return 1;
	}

	if((!msg->cseq && (parse_headers(msg,HDR_CSEQ_F,0)<0 || !msg->cseq))
		|| !msg->cseq->parsed){
		LM_DBG("parsing cseq header failed\n");
		return 2;
	}

	if(msg->first_line.type==SIP_REPLY) {
		/* reply to local transaction -- nothing to do */
		if (parse_headers(msg, HDR_VIA2_F, 0)==-1
				|| (msg->via2==0) || (msg->via2->error!=PARSE_OK)) {
			if(get_cseq(msg)->method_id != METHOD_CANCEL) {
				LM_DBG("no second via in this message \n");
				return 3;
			}
		}
	}

	if(parse_from_header(msg)<0) {
		LM_ERR("cannot parse FROM header\n");
		return 3;
	}

	if(parse_to_header(msg)<0 || msg->to==NULL) {
		LM_ERR("cannot parse TO header\n");
		return 3;
	}

	if(get_to(msg)==NULL) {
		LM_ERR("cannot get TO header\n");
		return 3;
	}

	return 0;
}

/**
 *
 */
static int dlg_cseq_prepare_new_msg(sip_msg_t *msg)
{
	LM_DBG("prepare new msg for cseq update operations\n");
	if (parse_msg(msg->buf, msg->len, msg)!=0) {
		LM_DBG("outbuf buffer parsing failed!\n");
		return 1;
	}
	return dlg_cseq_prepare_msg(msg);
}

/**
 *
 */
int dlg_cseq_update(sip_msg_t *msg)
{
	dlg_cell_t *dlg = NULL;
	unsigned int direction;
	unsigned int ninc = 0;
	str cseq;
	str nval;
	sr_cfgenv_t *cenv = NULL;

	if(dlg_cseq_prepare_msg(msg)!=0) {
		return -1;
	}
	if(msg->first_line.type==SIP_REPLY) {
		/* nothing to do for outgoing replies */
		goto done;
	}

	LM_DBG("initiating cseq updates\n");

	direction = DLG_DIR_NONE;
	dlg = dlg_lookup_msg_dialog(msg, &direction);

	if(dlg == NULL) {
		LM_DBG("no dialog for this request\n");
		goto done;
	}

	cenv = sr_cfgenv_get();

	/*local sequence number*/
	cseq = dlg->cseq[direction];

	str2int(&cseq, &ninc);
	ninc += 1;
	nval.s = int2str(ninc, &nval.len);
	trim(&nval);

	nval.s = int2str(ninc, &nval.len);
	if (nval.len != cseq.len) {
		shm_free(cseq.s);
		cseq.s = shm_malloc(nval.len);
		cseq.len = nval.len;
	}
	memcpy(cseq.s, nval.s, nval.len);

	dlg->dflags |= DLG_FLAG_CHANGED;
	if(dlg_db_mode==DB_MODE_REALTIME && (dlg->dflags&DLG_FLAG_CHANGED)) {
		update_dialog_dbinfo(dlg);
	}

	LM_DBG("adding auth cseq header value: %.*s\n", nval.len, nval.s);
	if(parse_headers(msg, HDR_EOH_F, 0)==-1) {
		LM_ERR("failed to parse all headers\n");
	}
	sr_hdr_add_zs(msg, cenv->uac_cseq_auth.s, &nval);

done:
	if(dlg!=NULL) dlg_release(dlg);
	return 0;
}


/**
 *
 */
int dlg_cseq_refresh(sip_msg_t *msg, dlg_cell_t *dlg,
		unsigned int direction)
{
	sr_cfgenv_t *cenv = NULL;

	if(dlg_cseq_prepare_msg(msg)!=0) {
		goto error;
	}
	if(msg->first_line.type==SIP_REPLY) {
		/* nothing to do for outgoing replies */
		goto done;
	}

	LM_DBG("initiating cseq refresh\n");

	/* take the increment value from dialog */
	if(!((dlg->iflags&DLG_IFLAG_CSEQ_DIFF)==DLG_IFLAG_CSEQ_DIFF)) {
		LM_DBG("no cseq refresh required\n");
		goto done;
	}

	LM_DBG("adding cseq refresh header value: %.*s\n", dlg->cseq[direction].len, dlg->cseq[direction].s);
	if(parse_headers(msg, HDR_EOH_F, 0)==-1) {
		LM_ERR("failed to parse all headers\n");
	}
	cenv = sr_cfgenv_get();
	sr_hdr_add_zs(msg, cenv->uac_cseq_refresh.s, &dlg->cseq[direction]);

done:
	return 0;

error:
	return -1;
}

/**
 *
 */
int dlg_cseq_msg_received(sr_event_param_t *evp)
{
	sip_msg_t msg;
	str *obuf;
	struct via_body *via;
	str vcseq;

	obuf = (str*)evp->data;
	memset(&msg, 0, sizeof(sip_msg_t));
	msg.buf = obuf->s;
	msg.len = obuf->len;

	if(dlg_cseq_prepare_new_msg(&msg)!=0) {
		goto done;
	}

	if(msg.first_line.type==SIP_REQUEST) {
		/* nothing to do for incoming requests */
		goto done;
	}

	via = (struct via_body*)msg.h_via1->parsed;

	if(via->branch==NULL || via->branch->value.len<=0) {
		LM_DBG("no branch parameter in top via\n");
		goto done;
	}
	vcseq.s = via->branch->value.s + via->branch->value.len - 1;
	vcseq.len = 0;
	while(vcseq.s>via->branch->value.s) {
		if(*vcseq.s=='.') {
			if(vcseq.len<3) {
				LM_DBG("no matching the starting point length\n");
				goto done;
			}
			if(vcseq.s[1]!='c' || vcseq.s[2]!='s') {
				LM_DBG("no matching the starting placeholder\n");
				goto done;
			}
			vcseq.len++;
			break;
		}
		vcseq.len++;
		vcseq.s--;
	}
	LM_DBG("via cseq cookie [%.*s] val [%.*s]\n", vcseq.len, vcseq.s,
			vcseq.len-3, vcseq.s+3);
	if(vcseq.len-3>get_cseq(&msg)->number.len) {
		/* higher length to update - wrong */
		LM_DBG("cseq in message (%d) shorter than in via (%d)\n",
				get_cseq(&msg)->number.len, vcseq.len-3);
		goto done;
	}
	if(vcseq.len-3==get_cseq(&msg)->number.len) {
		/* same length - overwrite in the buffer */
		memcpy(get_cseq(&msg)->number.s, vcseq.s+3, vcseq.len-3);
	} else {
		/* pad beginning with space */
		strncpy(get_cseq(&msg)->number.s, "                  ",
				get_cseq(&msg)->number.len - vcseq.len + 3);
		memcpy(get_cseq(&msg)->number.s + get_cseq(&msg)->number.len
				- vcseq.len + 3, vcseq.s+3, vcseq.len-3);
	}
	/* our via, will be removed anyhow - let's change cseq part of branch
	 * to a z parameter instead of shifting content to remove it */
	vcseq.s[0] = ';';
	vcseq.s[1] = 'z';
	vcseq.s[2] = '=';

done:
	free_sip_msg(&msg);
	return 0;
}

/**
 *
 */
int dlg_cseq_msg_sent(sr_event_param_t *evp)
{
	sip_msg_t msg;
	str *obuf;
	unsigned int direction;
	dlg_cell_t *dlg = NULL;
	str nval = STR_NULL;
	char tbuf[BUF_SIZE];
	int tbuf_len = 0;
	struct via_body *via;
	hdr_field_t *hfk = NULL;
	sr_cfgenv_t *cenv = NULL;
	str nbuf = STR_NULL;

	obuf = (str*)evp->data;
	memset(&msg, 0, sizeof(sip_msg_t));
	msg.buf = obuf->s;
	msg.len = obuf->len;
	cenv = sr_cfgenv_get();

	if(dlg_cseq_prepare_new_msg(&msg)!=0) {
		goto done;
	}

	if(msg.first_line.type==SIP_REPLY) {
		/* nothing to do for outgoing replies */
		goto done;
	}

	if(!IS_SIP(&msg)) {
		/* nothing to do for non-sip requests */
		goto done;
	}

	if(get_to(&msg)->tag_value.len<=0) {
		/* initial request - handle only INVITEs, ACKs and CANCELs */
		if(!(msg.first_line.u.request.method_value
					& (METHOD_INVITE|METHOD_ACK|METHOD_CANCEL))) {
			goto done;
		}
	}

	LM_DBG("tracking cseq updates\n");
	via = (struct via_body*)msg.h_via1->parsed;

	if(via->branch==NULL || via->branch->value.len<=0) {
		LM_DBG("no branch parameter in top via\n");
		goto done;
	}

	direction = DLG_DIR_NONE;
	dlg = dlg_lookup_msg_dialog(&msg, &direction);

	if(dlg == NULL) {
		LM_DBG("no dialog for this request\n");
		goto done;
	}

	/* supported only for downstream direction */
	if(direction != DLG_DIR_DOWNSTREAM) {
		LM_DBG("request not going downstream (%u)\n", direction);
		goto done;
	}

	if(parse_headers(&msg, HDR_EOH_F, 0)==-1) {
		LM_ERR("failed to parse all headers\n");
	}

	/* check if transaction is marked for a new increment */
	hfk = sr_hdr_get_z(&msg, cenv->uac_cseq_auth.s);
	if(hfk!=NULL) {
		LM_DBG("new cseq inc requested\n");
		nval = hfk->body;
		trim(&nval);
	} else {
		LM_DBG("new cseq inc not requested\n");
	}

	if(nval.len<=0) {
		hfk = sr_hdr_get_z(&msg, cenv->uac_cseq_refresh.s);
		if(hfk!=NULL) {
			LM_DBG("cseq refresh requested\n");
			nval = hfk->body;
			trim(&nval);
		} else {
			LM_DBG("cseq refresh not requested\n");
		}
	}
	if(nval.len<=0) {
		LM_DBG("cseq refresh requested, but no new value found\n");
		goto done;
	}

	if(msg.len + 3 + 2*nval.len>=BUF_SIZE) {
		LM_ERR("new messages is too big\n");
		goto done;
	}
	LM_DBG("updating cseq to: %.*s\n", nval.len, nval.s);

	/* new cseq value */
	dlg->iflags |= DLG_IFLAG_CSEQ_DIFF;

	if(via->branch->value.s<get_cseq(&msg)->number.s) {
		/* Via is before CSeq */
		/* copy first part till after via branch */
		tbuf_len = via->branch->value.s + via->branch->value.len - msg.buf;
		memcpy(tbuf, msg.buf, tbuf_len);
		/* complete via branch */
		tbuf[tbuf_len++] = '.';
		tbuf[tbuf_len++] = 'c';
		tbuf[tbuf_len++] = 's';
		memcpy(tbuf+tbuf_len, get_cseq(&msg)->number.s, get_cseq(&msg)->number.len);
		tbuf_len += get_cseq(&msg)->number.len;
		/* copy till beginning of cseq number */
		memcpy(tbuf+tbuf_len, via->branch->value.s + via->branch->value.len,
				get_cseq(&msg)->number.s - via->branch->value.s
				- via->branch->value.len);
		tbuf_len += get_cseq(&msg)->number.s - via->branch->value.s
					- via->branch->value.len;
		/* add new value */
		memcpy(tbuf+tbuf_len, nval.s, nval.len);
		tbuf_len += nval.len;
		if(hfk && hfk->name.s > get_cseq(&msg)->number.s) {
			/* copy from after cseq number to the beginning of hfk */
			memcpy(tbuf+tbuf_len, get_cseq(&msg)->number.s+get_cseq(&msg)->number.len,
					hfk->name.s - get_cseq(&msg)->number.s
					- get_cseq(&msg)->number.len);
			tbuf_len += hfk->name.s - get_cseq(&msg)->number.s
					- get_cseq(&msg)->number.len;
			/* copy from after hfk to the end of sip message */
			memcpy(tbuf+tbuf_len,  hfk->name.s + hfk->len,
					msg.buf + msg.len - hfk->name.s - hfk->len);
			tbuf_len += msg.buf + msg.len - hfk->name.s - hfk->len;
		} else {
			/* copy from after cseq number to the end of sip message */
			memcpy(tbuf+tbuf_len, get_cseq(&msg)->number.s+get_cseq(&msg)->number.len,
					msg.buf + msg.len - get_cseq(&msg)->number.s
					- get_cseq(&msg)->number.len);
			tbuf_len += msg.buf+msg.len - get_cseq(&msg)->number.s
					- get_cseq(&msg)->number.len;
		}
	} else {
		/* CSeq is before Via */
		/* copy till beginning of cseq number */
		tbuf_len = get_cseq(&msg)->number.s - msg.buf;
		memcpy(tbuf, msg.buf, tbuf_len);
		/* add new value */
		memcpy(tbuf+tbuf_len, nval.s, nval.len);
		tbuf_len += nval.len;
		/* copy from after cseq number to the after via branch */
		memcpy(tbuf+tbuf_len, get_cseq(&msg)->number.s+get_cseq(&msg)->number.len,
				via->branch->value.s + via->branch->value.len
				- get_cseq(&msg)->number.s - get_cseq(&msg)->number.len);
		tbuf_len += via->branch->value.s + via->branch->value.len
				- get_cseq(&msg)->number.s - get_cseq(&msg)->number.len;
		/* complete via branch */
		tbuf[tbuf_len++] = '.';
		tbuf[tbuf_len++] = 'c';
		tbuf[tbuf_len++] = 's';
		memcpy(tbuf+tbuf_len, get_cseq(&msg)->number.s, get_cseq(&msg)->number.len);
		tbuf_len += get_cseq(&msg)->number.len;
		if(hfk && hfk->name.s > get_cseq(&msg)->number.s) {
			/* copy from after via to the beginning of hfk */
			memcpy(tbuf+tbuf_len, via->branch->value.s + via->branch->value.len,
					hfk->name.s - via->branch->value.s
					- via->branch->value.len);
			tbuf_len += hfk->name.s - via->branch->value.s
					- via->branch->value.len;
			/* copy from after hfk to the end of sip message */
			memcpy(tbuf+tbuf_len,  hfk->name.s + hfk->len,
					msg.buf + msg.len - hfk->name.s - hfk->len);
			tbuf_len += msg.buf + msg.len - hfk->name.s - hfk->len;
		} else {
			/* copy from after via to the end of sip message */
			memcpy(tbuf+tbuf_len, via->branch->value.s + via->branch->value.len,
					msg.buf + msg.len - via->branch->value.s
					- via->branch->value.len);
			tbuf_len += msg.buf+msg.len - via->branch->value.s
					- via->branch->value.len;
		}
	}
	/* replace old msg content */
	nbuf.s = pkg_malloc((tbuf_len+1)*sizeof(char));
	if(nbuf.s==NULL) {
		LM_ERR("not enough memory for new message\n");
		goto done;
	}
	pkg_free(obuf->s);
	obuf->s = nbuf.s;
	memcpy(obuf->s, tbuf, tbuf_len);
	obuf->s[tbuf_len] = 0;
	obuf->len = tbuf_len;

done:
	if(dlg!=NULL) dlg_release(dlg);
	free_sip_msg(&msg);
	return 0;
}

/**
 *
 */
int dlg_register_cseq_callbacks(void)
{
	sr_event_register_cb(SREV_NET_DATA_IN, dlg_cseq_msg_received);
	sr_event_register_cb(SREV_NET_DATA_OUT, dlg_cseq_msg_sent);
	return 0;
}
