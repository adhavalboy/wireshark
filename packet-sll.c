/* packet-sll.c
 * Routines for disassembly of packets from Linux "cooked mode" captures
 *
 * $Id: packet-sll.c,v 1.4 2001/01/09 06:31:43 guy Exp $
 *
 * Ethereal - Network traffic analyzer
 * By Gerald Combs <gerald@zing.org>
 * Copyright 1998 Gerald Combs
 *
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif

#include <stdio.h>
#include <string.h>
#include <glib.h>
#include "packet.h"
#include "packet-ipx.h"
#include "packet-llc.h"
#include "resolv.h"

static int proto_sll = -1;
static int hf_sll_pkttype = -1;
static int hf_sll_hatype = -1;
static int hf_sll_halen = -1;
static int hf_sll_src_eth = -1;
static int hf_sll_src_other = -1;
static int hf_sll_ltype = -1;
static int hf_sll_etype = -1;
static int hf_sll_trailer = -1;

static gint ett_sll = -1;

/*
 * A DLT_LINUX_SLL fake link-layer header.
 */
#define SLL_HEADER_SIZE	16		/* total header length */
#define SLL_ADDRLEN	8		/* length of address field */

/*
 * The LINUX_SLL_ values for "sll_pkttype".
 */
#define LINUX_SLL_HOST		0
#define LINUX_SLL_BROADCAST	1
#define LINUX_SLL_MULTICAST	2
#define LINUX_SLL_OTHERHOST	3
#define LINUX_SLL_OUTGOING	4

static const value_string packet_type_vals[] = {
	{ LINUX_SLL_HOST,	"Unicast to us" },
	{ LINUX_SLL_BROADCAST,	"Broadcast" },
	{ LINUX_SLL_MULTICAST,	"Multicast" },
	{ LINUX_SLL_OTHERHOST,	"Unicast to another host" },
	{ LINUX_SLL_OUTGOING,	"Sent by us" },
	{ 0,			NULL }
};

/*
 * The LINUX_SLL_ values for "sll_protocol".
 */
#define LINUX_SLL_P_802_3	0x0001	/* Novell 802.3 frames without 802.2 LLC header */
#define LINUX_SLL_P_802_2	0x0004	/* 802.2 frames (not D/I/X Ethernet) */

static const value_string ltype_vals[] = {
	{ LINUX_SLL_P_802_3,	"Raw 802.3" },
	{ LINUX_SLL_P_802_2,	"802.2 LLC" },
	{ 0,			NULL }
};

static dissector_handle_t llc_handle;

void
capture_sll(const u_char *pd, packet_counts *ld)
{
	guint16 protocol;

	if (!BYTES_ARE_IN_FRAME(0, SLL_HEADER_SIZE)) {
		ld->other++;
		return;
	}
	protocol = pntohs(&pd[14]);
	if (protocol <= 1536) {	/* yes, 1536 - that's how Linux does it */
		/*
		 * "proto" is *not* a length field, it's a Linux internal
		 * protocol type.
		 */
		switch (protocol) {

		case LINUX_SLL_P_802_2:
			/*
			 * 802.2 LLC.
			 */
			capture_llc(pd, SLL_HEADER_SIZE, ld);
			break;

		case LINUX_SLL_P_802_3:
			/*
			 * Novell IPX inside 802.3 with no 802.2 LLC
			 * header.
			 */
			capture_ipx(pd, SLL_HEADER_SIZE, ld);
			break;

		default:
			ld->other++;
			break;
		}
	} else
		capture_ethertype(protocol, SLL_HEADER_SIZE, pd, ld);
}

static void
dissect_sll(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
	guint16 pkttype;
	guint16 protocol;
	guint16 hatype, halen;
	guint8 *src;
	proto_item *ti;
	volatile guint16 length;
	tvbuff_t *volatile next_tvb;
	tvbuff_t *volatile trailer_tvb;
	proto_tree *volatile fh_tree = NULL;
	guint length_before;

	CHECK_DISPLAY_AS_DATA(proto_sll, tvb, pinfo, tree);

	pinfo->current_proto = "SLL";
	if (check_col(pinfo->fd, COL_PROTOCOL))
		col_set_str(pinfo->fd, COL_PROTOCOL, "SLL");

	pkttype = tvb_get_ntohs(tvb, 0);

	if (check_col(pinfo->fd, COL_INFO))
		col_add_str(pinfo->fd, COL_INFO,
		    val_to_str(pkttype, packet_type_vals, "Unknown (%u)"));

	if (tree) {
		ti = proto_tree_add_protocol_format(tree, proto_sll, tvb, 0,
		    SLL_HEADER_SIZE, "Linux cooked capture");
		fh_tree = proto_item_add_subtree(ti, ett_sll);
		proto_tree_add_item(fh_tree, hf_sll_pkttype, tvb, 0, 2, FALSE);
	}

	/*
	 * XXX - check the link-layer address type value?
	 * For now, we just assume 6 means Ethernet.
	 */
	hatype = tvb_get_ntohs(tvb, 2);
	halen = tvb_get_ntohs(tvb, 4);
	if (tree) {
		proto_tree_add_uint(fh_tree, hf_sll_hatype, tvb, 2, 2, hatype);
		proto_tree_add_uint(fh_tree, hf_sll_halen, tvb, 4, 2, halen);
	}
	if (halen == 6) {
		src = tvb_get_ptr(tvb, 6, 6);
		SET_ADDRESS(&pinfo->dl_src, AT_ETHER, 6, src);
		SET_ADDRESS(&pinfo->src, AT_ETHER, 6, src);
		if (tree) {
			proto_tree_add_ether(fh_tree, hf_sll_src_eth, tvb,
			    6, 6, src);
		}
	} else {
		if (tree) {
			proto_tree_add_bytes(fh_tree, hf_sll_src_other, tvb,
			    6, halen, tvb_get_ptr(tvb, 6, halen));
		}
	}

	protocol = tvb_get_ntohs(tvb, 14);
	if (protocol <= 1536) {	/* yes, 1536 - that's how Linux does it */
		/*
		 * "proto" is *not* a length field, it's a Linux internal
		 * protocol type.
		 * We therefore cannot say how much of the packet will
		 * be trailer data.
		 * XXX - do the same thing we do for packets with Ethertypes?
		 */
		proto_tree_add_uint(fh_tree, hf_sll_ltype, tvb, 14, 2,
		    protocol);

		next_tvb = tvb_new_subset(tvb, SLL_HEADER_SIZE, -1, -1);
		trailer_tvb = NULL;
		switch (protocol) {

		case LINUX_SLL_P_802_2:
			/*
			 * 802.2 LLC.
			 */
			call_dissector(llc_handle, next_tvb, pinfo, tree);
			break;

		case LINUX_SLL_P_802_3:
			/*
			 * Novell IPX inside 802.3 with no 802.2 LLC
			 * header.
			 */
			dissect_ipx(next_tvb, pinfo, tree);
			break;

		default:
			dissect_data(next_tvb, 0, pinfo, tree);
			break;
		}
	} else {
		length_before = tvb_reported_length(tvb);
		length = ethertype(protocol, tvb, SLL_HEADER_SIZE, pinfo, tree,
		    fh_tree, hf_sll_etype) + SLL_HEADER_SIZE;
		if (length < length_before) {
			/*
			 * Create a tvbuff for the padding.
			 */
			TRY {
				trailer_tvb = tvb_new_subset(tvb, length, -1,
				    -1);
			}
			CATCH2(BoundsError, ReportedBoundsError) {
				/* The packet doesn't have "length" bytes
				   worth of captured data left in it.  No
				   trailer to display. */
				trailer_tvb = NULL;
			}
			ENDTRY;
		} else {
			/*
			 * There is no padding.
			 */
			trailer_tvb = NULL;
		}
	}

	/* If there's some bytes left over, mark them. */
	if (trailer_tvb && tree) {
		guint trailer_length;

		trailer_length = tvb_length(trailer_tvb);
		if (trailer_length != 0) {
			proto_tree_add_item(fh_tree, hf_sll_trailer,
			    trailer_tvb, 0, trailer_length, FALSE);
		}
	}
}

void
proto_register_sll(void)
{
	static hf_register_info hf[] = {
		{ &hf_sll_pkttype,
		{ "Packet type",	"sll.pkttype", FT_UINT16, BASE_DEC,
		  VALS(packet_type_vals), 0x0, "Packet type" }},

		/* ARP hardware type?  With Linux extensions? */
		{ &hf_sll_hatype,
		{ "Link-layer address type",	"sll.hatype", FT_UINT16, BASE_DEC,
		  NULL, 0x0, "Link-layer address type" }},

		{ &hf_sll_halen,
		{ "Link-layer address length",	"sll.halen", FT_UINT16, BASE_DEC,
		  NULL, 0x0, "Link-layer address length" }},

		/* Source address if it's an Ethernet-type address */
		{ &hf_sll_src_eth,
		{ "Source",	"sll.src.eth", FT_ETHER, BASE_NONE, NULL, 0x0,
			"Source link-layer address" }},

		/* Source address if it's not an Ethernet-type address */
		{ &hf_sll_src_other,
		{ "Source",	"sll.src.other", FT_BYTES, BASE_HEX, NULL, 0x0,
			"Source link-layer address" }},

		/* if the protocol field is an internal Linux protocol type */
		{ &hf_sll_ltype,
		{ "Protocol",	"sll.ltype", FT_UINT16, BASE_HEX,
		   VALS(ltype_vals), 0x0, "Linux protocol type" }},

		/* registered here but handled in ethertype.c */
		{ &hf_sll_etype,
		{ "Protocol",	"sll.etype", FT_UINT16, BASE_HEX,
		   VALS(etype_vals), 0x0, "Ethernet protocol type" }},

                { &hf_sll_trailer,
		{ "Trailer", "sll.trailer", FT_BYTES, BASE_NONE, NULL, 0x0,
			"Trailer" }},
	};
	static gint *ett[] = {
		&ett_sll,
	};

	proto_sll = proto_register_protocol("Linux cooked-mode capture",
	    "SLL", "sll" );
	proto_register_field_array(proto_sll, hf, array_length(hf));
	proto_register_subtree_array(ett, array_length(ett));
}

void
proto_reg_handoff_sll(void)
{
	/*
	 * Get a handle for the LLC dissector.
	 */
	llc_handle = find_dissector("llc");

	dissector_add("wtap_encap", WTAP_ENCAP_SLL, dissect_sll, proto_sll);
}
