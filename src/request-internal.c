/**
 *
 * /brief getdns contect management functions
 *
 * This is the meat of the API
 * Originally taken from the getdns API description pseudo implementation.
 *
 */
/*
 * Copyright (c) 2013, NLnet Labs, Verisign, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * * Neither the names of the copyright holders nor the
 *   names of its contributors may be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Verisign, Inc. BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "types-internal.h"
#include "util-internal.h"
#include "gldns/rrdef.h"
#include "gldns/str2wire.h"
#include "gldns/gbuffer.h"
#include "gldns/pkthdr.h"
#include "dict.h"
#include "debug.h"

/* MAXIMUM_TSIG_SPACE = TSIG name      (dname)    : 256
 *                      TSIG type      (uint16_t) :   2
 *                      TSIG class     (uint16_t) :   2
 *                      TSIG TTL       (uint32_t) :   4
 *                      RdLen          (uint16_t) :   2
 *                      Algorithm name (dname)    : 256
 *                      Time Signed    (uint48_t) :   6
 *                      Fudge          (uint16_t) :   2
 *                      Mac Size       (uint16_t) :   2
 *                      Mac            (variable) :   EVP_MAX_MD_SIZE
 *                      Original Id    (uint16_t) :   2
 *                      Error          (uint16_t) :   2
 *                      Other Len      (uint16_t) :   2
 *                      Other Data     (nothing)  :   0
 *                                                 ---- +
 *                                                  538 + EVP_MAX_MD_SIZE
 */
#define MAXIMUM_TSIG_SPACE (538 + EVP_MAX_MD_SIZE)

getdns_dict  dnssec_ok_checking_disabled_spc = {
	{ RBTREE_NULL, 0, (int (*)(const void *, const void *)) strcmp },
	{ 0 }
};
getdns_dict *dnssec_ok_checking_disabled = &dnssec_ok_checking_disabled_spc;

getdns_dict  dnssec_ok_checking_disabled_roadblock_avoidance_spc = {
	{ RBTREE_NULL, 0, (int (*)(const void *, const void *)) strcmp },
	{ 0 }
};
getdns_dict *dnssec_ok_checking_disabled_roadblock_avoidance
    = &dnssec_ok_checking_disabled_roadblock_avoidance_spc;

getdns_dict  dnssec_ok_checking_disabled_avoid_roadblocks_spc = {
	{ RBTREE_NULL, 0, (int (*)(const void *, const void *)) strcmp },
	{ 0 }
};
getdns_dict *dnssec_ok_checking_disabled_avoid_roadblocks
    = &dnssec_ok_checking_disabled_avoid_roadblocks_spc;


static int
is_extension_set(getdns_dict *extensions, const char *extension)
{
	getdns_return_t r;
	uint32_t value;

	if (! extensions)
		return 0;
	else if (extensions == dnssec_ok_checking_disabled
	    || extensions == dnssec_ok_checking_disabled_roadblock_avoidance
	    || extensions == dnssec_ok_checking_disabled_avoid_roadblocks)
		return 0;

	r = getdns_dict_get_int(extensions, extension, &value);
	return r == GETDNS_RETURN_GOOD && value == GETDNS_EXTENSION_TRUE;
}

static void
network_req_cleanup(getdns_network_req *net_req)
{
	assert(net_req);

	if (net_req->response && (net_req->response < net_req->wire_data ||
	    net_req->response > net_req->wire_data+ net_req->wire_data_sz))
		GETDNS_FREE(net_req->owner->my_mf, net_req->response);
}

static int
network_req_init(getdns_network_req *net_req, getdns_dns_req *owner,
    const char *name, uint16_t request_type, uint16_t request_class,
    int dnssec_extension_set, int with_opt,
    int edns_maximum_udp_payload_size,
    uint8_t edns_extended_rcode, uint8_t edns_version, int edns_do_bit,
    uint16_t opt_options_size, size_t noptions, getdns_list *options,
    size_t wire_data_sz, size_t max_query_sz)
{
	uint8_t *buf;
	size_t dname_len;
	getdns_dict    *option;
	uint32_t        option_code;
	getdns_bindata *option_data;
	size_t i;
	int r = 0;

	net_req->request_type = request_type;
	net_req->request_class = request_class;
	net_req->unbound_id = -1;
	net_req->state = NET_REQ_NOT_SENT;
	net_req->owner = owner;

	net_req->dnssec_status = GETDNS_DNSSEC_INDETERMINATE;

	net_req->upstream = NULL;
	net_req->fd = -1;
    net_req->transport_count = owner->context->dns_transport_count;
	net_req->transport_current = 0;
    memcpy(net_req->transports, owner->context->dns_transports,
           net_req->transport_count * sizeof(getdns_transport_list_t));
	net_req->tls_auth_min = owner->context->tls_auth_min;
	memset(&net_req->event, 0, sizeof(net_req->event));
	memset(&net_req->tcp, 0, sizeof(net_req->tcp));
	net_req->query_id = 0;
	net_req->edns_maximum_udp_payload_size = edns_maximum_udp_payload_size;
	net_req->max_udp_payload_size = edns_maximum_udp_payload_size != -1
	                              ? edns_maximum_udp_payload_size : 1432;
	net_req->write_queue_tail = NULL;
	net_req->response_len = 0;
        net_req->base_query_option_sz = opt_options_size;

	/* Some fields to record info for return_call_reporting */
	net_req->debug_start_time = 0;
	net_req->debug_end_time = 0;
	net_req->debug_tls_auth_status = 0;
	net_req->debug_udp = 0;

	net_req->wire_data_sz = wire_data_sz;
	if (max_query_sz) {
		/* first two bytes will contain query length (for tcp) */
		buf = net_req->query = net_req->wire_data + 2;

		gldns_write_uint16(buf + 2, 0); /* reset all flags */
		GLDNS_RD_SET(buf);
		if (dnssec_extension_set) /* We will do validation ourselves */
			GLDNS_CD_SET(buf);
		GLDNS_OPCODE_SET(buf, GLDNS_PACKET_QUERY);
		gldns_write_uint16(buf + GLDNS_QDCOUNT_OFF, 1); /* 1 query */
		gldns_write_uint16(buf + GLDNS_ANCOUNT_OFF, 0); /* 0 answers */
		gldns_write_uint16(buf + GLDNS_NSCOUNT_OFF, 0); /* 0 authorities */
		gldns_write_uint16(buf + GLDNS_ARCOUNT_OFF, with_opt ? 1 : 0);

		buf += GLDNS_HEADER_SIZE;
		dname_len = max_query_sz - GLDNS_HEADER_SIZE;
		if ((r = gldns_str2wire_dname_buf(name, buf, &dname_len))) {
			net_req->opt = NULL;
			return r;
		}

		buf += dname_len;

		gldns_write_uint16(buf, request_type);
		gldns_write_uint16(buf + 2, request_class);
		buf += 4;

		if (with_opt) {
			net_req->opt = buf;
			buf[0] = 0; /* dname for . */
			gldns_write_uint16(buf + 1, GLDNS_RR_TYPE_OPT);
			gldns_write_uint16(net_req->opt + 3,
			    net_req->max_udp_payload_size);
			buf[5] = edns_extended_rcode;
			buf[6] = edns_version;
			buf[7] = edns_do_bit ? 0x80 : 0;
			buf[8] = 0;
			gldns_write_uint16(buf + 9, opt_options_size);
			buf += 11;
			for (i = 0; i < noptions; i++) {
				if (getdns_list_get_dict(options, i, &option))
					continue;
				if (getdns_dict_get_int(
				    option, "option_code", &option_code))
					continue;
				if (getdns_dict_get_bindata(
				    option, "option_data", &option_data))
					continue;

				gldns_write_uint16(buf, (uint16_t) option_code);
				gldns_write_uint16(buf + 2,
				    (uint16_t) option_data->size);
				(void) memcpy(buf + 4, option_data->data,
				    option_data->size);

				buf += option_data->size + 4;
			}
		} else
			net_req->opt = NULL;
		net_req->response = buf;
		gldns_write_uint16(net_req->wire_data, net_req->response - net_req->query);
	} else {
		net_req->query    = NULL;
		net_req->opt      = NULL;
		net_req->response = net_req->wire_data;
	}
	return r;
}

/* req->opt + 9 is the length; req->opt + 11 is the start of the
   options. 

   clear_upstream_options() goes back to the per-query options.
 */

void
_getdns_network_req_clear_upstream_options(getdns_network_req * req)
{
  size_t pktlen;
  if (req->opt) {
	  gldns_write_uint16(req->opt + 9, req->base_query_option_sz);
	  req->response = req->opt + 11 + req->base_query_option_sz;
	  pktlen = req->response - req->query;
	  gldns_write_uint16(req->query - 2, pktlen);
  }
}

/* add_upstream_option appends an option that is derived at send time.
    (you can send data as NULL and it will fill with all zeros) */
getdns_return_t
_getdns_network_req_add_upstream_option(getdns_network_req * req, uint16_t code, uint16_t sz, const void* data)
{
  uint16_t oldlen;
  uint32_t newlen;
  uint32_t pktlen;
  size_t cur_upstream_option_sz;

  /* if no options are set, we can't add upstream options */
  if (!req->opt)
	  return GETDNS_RETURN_GENERIC_ERROR;
  
  /* if TCP, no overflow allowed for length field
     https://tools.ietf.org/html/rfc1035#section-4.2.2 */
  pktlen = req->response - req->query;
  pktlen += 4 + sz;
  if (pktlen > UINT16_MAX)
    return GETDNS_RETURN_GENERIC_ERROR;
  
  /* no overflow allowed for OPT size either (maybe this is overkill
     given the above check?) */
  oldlen = gldns_read_uint16(req->opt + 9);
  newlen = oldlen + 4 + sz;
  if (newlen > UINT16_MAX)
    return GETDNS_RETURN_GENERIC_ERROR;

  /* avoid overflowing MAXIMUM_UPSTREAM_OPTION_SPACE */
  cur_upstream_option_sz = (size_t)oldlen - req->base_query_option_sz;
  if (cur_upstream_option_sz  + 4 + sz > MAXIMUM_UPSTREAM_OPTION_SPACE)
    return GETDNS_RETURN_GENERIC_ERROR;

  /* actually add the option: */
  gldns_write_uint16(req->opt + 11 + oldlen, code);
  gldns_write_uint16(req->opt + 11 + oldlen + 2, sz);
  if (data != NULL)
	  memcpy(req->opt + 11 + oldlen + 4, data, sz);
  else
	  memset(req->opt + 11 + oldlen + 4, 0, sz);
  gldns_write_uint16(req->opt + 9, newlen);

  /* the response should start right after the options end: */
  req->response = req->opt + 11 + newlen;
  
  /* for TCP, adjust the size of the wire format itself: */
  gldns_write_uint16(req->query - 2, pktlen);
  
  return GETDNS_RETURN_GOOD;
}

size_t
_getdns_network_req_add_tsig(getdns_network_req *req)
{
	getdns_upstream *upstream = req->upstream;
	gldns_buffer gbuf;
	uint16_t arcount;
	const getdns_tsig_info *tsig_info;
	uint8_t md_buf[EVP_MAX_MD_SIZE];
	unsigned int md_len = EVP_MAX_MD_SIZE;
	const EVP_MD *digester;

	if (upstream->tsig_alg == GETDNS_NO_TSIG || !upstream->tsig_dname_len)
		return req->response - req->query;

	arcount = gldns_read_uint16(req->query + 10);

#if defined(STUB_DEBUG) && STUB_DEBUG
	/* TSIG should not have been written yet. */
	if (req->opt) {
		assert(arcount == 1);
		assert(req->opt + 11 + gldns_read_uint16(req->opt + 9)
		    == req->response);
	} else
		assert(arcount == 0);
#endif
	tsig_info = _getdns_get_tsig_info(upstream->tsig_alg);

	gldns_buffer_init_frm_data(&gbuf, req->response, MAXIMUM_TSIG_SPACE);
	gldns_buffer_write(&gbuf,
	    upstream->tsig_dname, upstream->tsig_dname_len);	/* Name */
	gldns_buffer_write_u16(&gbuf, GETDNS_RRCLASS_ANY);	/* Class */
	gldns_buffer_write_u32(&gbuf, 0);			/* TTL */
	gldns_buffer_write(&gbuf,
	    tsig_info->dname, tsig_info->dname_len);	/* Algorithm Name */
	gldns_buffer_write_u48(&gbuf, time(NULL));	/* Time Signed */
	gldns_buffer_write_u16(&gbuf, 300);		/* Fudge */
	gldns_buffer_write_u16(&gbuf, 0);		/* Error */
	gldns_buffer_write_u16(&gbuf, 0);		/* Other len */

	switch (upstream->tsig_alg) {
#ifdef HAVE_EVP_MD5
	case GETDNS_HMAC_MD5   : digester = EVP_md5()   ; break;
#endif
#ifdef HAVE_EVP_SHA1
	case GETDNS_HMAC_SHA1  : digester = EVP_sha1()  ; break;
#endif
#ifdef HAVE_EVP_SHA224
	case GETDNS_HMAC_SHA224: digester = EVP_sha224(); break;
#endif
#ifdef HAVE_EVP_SHA256
	case GETDNS_HMAC_SHA256: digester = EVP_sha256(); break;
#endif
#ifdef HAVE_EVP_SHA384
	case GETDNS_HMAC_SHA384: digester = EVP_sha384(); break;
#endif
#ifdef HAVE_EVP_SHA512
	case GETDNS_HMAC_SHA512: digester = EVP_sha512(); break;
#endif
	default                : return req->response - req->query;
	}

	(void) HMAC(digester, upstream->tsig_key, upstream->tsig_size,
	    (void *)req->query, gldns_buffer_current(&gbuf) - req->query,
	    md_buf, &md_len);

	gldns_buffer_rewind(&gbuf);
	gldns_buffer_write(&gbuf,
	    upstream->tsig_dname, upstream->tsig_dname_len);	/* Name */
	gldns_buffer_write_u16(&gbuf, GETDNS_RRTYPE_TSIG);	/* Type*/
	gldns_buffer_write_u16(&gbuf, GETDNS_RRCLASS_ANY);	/* Class */
	gldns_buffer_write_u32(&gbuf, 0);			/* TTL */
	gldns_buffer_write_u16(&gbuf,
	    tsig_info->dname_len + 10 + md_len + 6);	/* RdLen */
	gldns_buffer_write(&gbuf,
	    tsig_info->dname, tsig_info->dname_len);	/* Algorithm Name */
	gldns_buffer_write_u48(&gbuf, time(NULL));	/* Time Signed */
	gldns_buffer_write_u16(&gbuf, 300);		/* Fudge */
	gldns_buffer_write_u16(&gbuf, md_len);		/* MAC Size */
	gldns_buffer_write(&gbuf, md_buf, md_len);	/* MAC*/
	gldns_buffer_write(&gbuf, req->query, 2);	/* Original ID */
	gldns_buffer_write_u16(&gbuf, 0);		/* Error */
	gldns_buffer_write_u16(&gbuf, 0);		/* Other len */

	if (gldns_buffer_position(&gbuf) > gldns_buffer_limit(&gbuf))
		return req->response - req->query;

	DEBUG_STUB("Sending with TSIG, mac length: %d\n", (int)md_len);
	gldns_write_uint16(req->query + 10, arcount + 1);
	req->response = gldns_buffer_current(&gbuf);
	return req->response - req->query;
}

void
_getdns_dns_req_free(getdns_dns_req * req)
{
	getdns_network_req **net_req;
	if (!req) {
		return;
	}

	_getdns_upstreams_dereference(req->upstreams);

	/* cleanup network requests */
	for (net_req = req->netreqs; *net_req; net_req++)
		network_req_cleanup(*net_req);

	/* clear timeout event */
	if (req->timeout.timeout_cb) {
		req->loop->vmt->clear(req->loop, &req->timeout);
		req->timeout.timeout_cb = NULL;
	}

	GETDNS_FREE(req->my_mf, req);
}

/* create a new dns req to be submitted */
getdns_dns_req *
_getdns_dns_req_new(getdns_context *context, getdns_eventloop *loop,
    const char *name, uint16_t request_type, getdns_dict *extensions)
{
	int dnssec_return_status
	    =  context->return_dnssec_status == GETDNS_EXTENSION_TRUE
	    || is_extension_set(extensions, "dnssec_return_status");
	int dnssec_return_only_secure
	    =  is_extension_set(extensions, "dnssec_return_only_secure");
	int dnssec_return_validation_chain
	    =  is_extension_set(extensions, "dnssec_return_validation_chain");
	int edns_cookies
	    =  is_extension_set(extensions, "edns_cookies");
#ifdef DNSSEC_ROADBLOCK_AVOIDANCE
	int avoid_dnssec_roadblocks
	    =  (extensions == dnssec_ok_checking_disabled_avoid_roadblocks);
	int dnssec_roadblock_avoidance
	    = is_extension_set(extensions, "dnssec_roadblock_avoidance")
	    || (extensions == dnssec_ok_checking_disabled_roadblock_avoidance)
	    || avoid_dnssec_roadblocks;
#endif

	int dnssec_extension_set = dnssec_return_status
	    || dnssec_return_only_secure || dnssec_return_validation_chain
	    || (extensions == dnssec_ok_checking_disabled)
	    || (extensions == dnssec_ok_checking_disabled_roadblock_avoidance)
	    || (extensions == dnssec_ok_checking_disabled_avoid_roadblocks)
#ifdef DNSSEC_ROADBLOCK_AVOIDANCE
	    || dnssec_roadblock_avoidance
#endif
	    ;

	uint32_t edns_do_bit;
	int      edns_maximum_udp_payload_size;
	uint32_t get_edns_maximum_udp_payload_size;
	uint32_t edns_extended_rcode;
	uint32_t edns_version;

	getdns_dict *add_opt_parameters;
	int     have_add_opt_parameters;

	getdns_list *options;
	size_t      noptions = 0;
	size_t       i;

	getdns_dict    *option;
	uint32_t        option_code;
	getdns_bindata *option_data;
	size_t opt_options_size = 0;

	int with_opt;

	getdns_dns_req *result = NULL;
        uint32_t klass = GLDNS_RR_CLASS_IN;
	int a_aaaa_query =
	    is_extension_set(extensions, "return_both_v4_and_v6") &&
	    ( request_type == GETDNS_RRTYPE_A ||
	      request_type == GETDNS_RRTYPE_AAAA );
	/* Reserve for the buffer at least one more byte
	 * (to test for udp overflow) (hence the + 1),
	 * And align on the 8 byte boundry  (hence the (x + 7) / 8 * 8)
	 */
	size_t max_query_sz, max_response_sz, netreq_sz, dnsreq_base_sz;
	uint8_t *region;
	
	if (extensions == dnssec_ok_checking_disabled ||
	    extensions == dnssec_ok_checking_disabled_roadblock_avoidance ||
	    extensions == dnssec_ok_checking_disabled_avoid_roadblocks)
		extensions = NULL;

	have_add_opt_parameters = getdns_dict_get_dict(extensions,
	    "add_opt_parameters", &add_opt_parameters) == GETDNS_RETURN_GOOD;

	if (dnssec_extension_set) {
		edns_maximum_udp_payload_size = -1;
		edns_extended_rcode = 0;
		edns_version = 0;
		edns_do_bit = 1;
	} else {
		edns_maximum_udp_payload_size =
		    context->edns_maximum_udp_payload_size;
		edns_extended_rcode = context->edns_extended_rcode;
		edns_version = context->edns_version;
		edns_do_bit = context->edns_do_bit;

		if (have_add_opt_parameters) {
			if (!getdns_dict_get_int(add_opt_parameters,
			    "maximum_udp_payload_size",
			    &get_edns_maximum_udp_payload_size))
				edns_maximum_udp_payload_size =
				    get_edns_maximum_udp_payload_size;
			(void) getdns_dict_get_int(add_opt_parameters,
			    "extended_rcode", &edns_extended_rcode);
			(void) getdns_dict_get_int(add_opt_parameters,
			    "version", &edns_version);
			(void) getdns_dict_get_int(add_opt_parameters,
			    "do_bit", &edns_do_bit);
		}
	}
	if (have_add_opt_parameters && getdns_dict_get_list(
	    add_opt_parameters, "options", &options) == GETDNS_RETURN_GOOD)
		(void) getdns_list_get_length(options, &noptions);

	with_opt = edns_do_bit != 0 || edns_maximum_udp_payload_size != 512 ||
	    edns_extended_rcode != 0 || edns_version != 0 || noptions ||
	    edns_cookies || context->edns_client_subnet_private ||
	    context->tls_query_padding_blocksize > 1;

	edns_maximum_udp_payload_size = with_opt &&
	    ( edns_maximum_udp_payload_size == -1 ||
	      edns_maximum_udp_payload_size > 512 )
	    ? edns_maximum_udp_payload_size : 512;

	/* (x + 7) / 8 * 8 to align on 8 byte boundries */
#ifdef DNSSEC_ROADBLOCK_AVOIDANCE
	if (context->resolution_type == GETDNS_RESOLUTION_RECURSING
	    && (!dnssec_roadblock_avoidance || avoid_dnssec_roadblocks)) 
#else
	if (context->resolution_type == GETDNS_RESOLUTION_RECURSING)
#endif
		max_query_sz = 0;
	else {
		for (i = 0; i < noptions; i++) {
			if (getdns_list_get_dict(options, i, &option)) continue;
			if (getdns_dict_get_int(
			    option, "option_code", &option_code)) continue;
			if (getdns_dict_get_bindata(
			    option, "option_data", &option_data)) continue;

			opt_options_size += option_data->size
			    + 2 /* option-code   */
			    + 2 /* option-length */
			    ;
		}
		max_query_sz = ( GLDNS_HEADER_SIZE
		    + strlen(name) + 1 + 4 /* dname always smaller then strlen(name) + 1 */
		    + 12 + opt_options_size /* space needed for OPT (if needed) */
		    + MAXIMUM_UPSTREAM_OPTION_SPACE
		    + MAXIMUM_TSIG_SPACE
		    + 7) / 8 * 8;
	}
	max_response_sz = (( edns_maximum_udp_payload_size != -1
	                   ? edns_maximum_udp_payload_size : 1432
	                   ) + 1 /* +1 for udp overflow detection */
	                     + 7 ) / 8 * 8;

	netreq_sz = ( sizeof(getdns_network_req)
	            + max_query_sz + max_response_sz  + 7 ) / 8 * 8;
	dnsreq_base_sz = (( sizeof(getdns_dns_req) 
	                  + (a_aaaa_query ? 3 : 2) * sizeof(getdns_network_req*)
			  ) + 7) / 8 * 8;

	if (! (region = GETDNS_XMALLOC(context->mf, uint8_t, 
	    dnsreq_base_sz + (a_aaaa_query ? 2 : 1) * netreq_sz)))
		return NULL;

	result = (getdns_dns_req *)region;
	result->netreqs[0] = (getdns_network_req *)(region + dnsreq_base_sz);
	if (a_aaaa_query) {
		result->netreqs[1] = (getdns_network_req *)
		    (region + dnsreq_base_sz + netreq_sz);
		result->netreqs[2] = NULL;
	} else
		result->netreqs[1] = NULL;

	result->my_mf = context->mf;

	result->name_len = sizeof(result->name);
	if (gldns_str2wire_dname_buf(name, result->name, &result->name_len)) {
		GETDNS_FREE(result->my_mf, result);
		return NULL;
	}
	result->context = context;
	result->loop = loop;
	result->canceled = 0;
	result->trans_id = (((uint64_t)arc4random()) << 32) |
	                    ((uint64_t)arc4random());
	result->dnssec_return_status           = dnssec_return_status;
	result->dnssec_return_only_secure      = dnssec_return_only_secure;
	result->dnssec_return_validation_chain = dnssec_return_validation_chain;
	result->edns_cookies                   = edns_cookies;
#ifdef DNSSEC_ROADBLOCK_AVOIDANCE
	result->dnssec_roadblock_avoidance     = dnssec_roadblock_avoidance;
	result->avoid_dnssec_roadblocks        = avoid_dnssec_roadblocks;
#endif
	result->edns_client_subnet_private     = context->edns_client_subnet_private;
	result->tls_query_padding_blocksize    = context->tls_query_padding_blocksize;
	result->return_call_reporting
		= is_extension_set(extensions, "return_call_reporting");
	
	/* will be set by caller */
	result->user_pointer = NULL;
	result->user_callback = NULL;
	memset(&result->timeout, 0, sizeof(result->timeout));

        /* check the specify_class extension */
        (void) getdns_dict_get_int(extensions, "specify_class", &klass);
        
	result->upstreams = context->upstreams;
	if (result->upstreams)
		result->upstreams->referenced++;

	network_req_init(result->netreqs[0], result,
	    name, request_type, klass,
	    dnssec_extension_set, with_opt,
	    edns_maximum_udp_payload_size,
	    edns_extended_rcode, edns_version, edns_do_bit,
	    opt_options_size, noptions, options,
	    netreq_sz - sizeof(getdns_network_req), max_query_sz);

	if (a_aaaa_query)
		network_req_init(result->netreqs[1], result, name,
		    ( request_type == GETDNS_RRTYPE_A
		    ? GETDNS_RRTYPE_AAAA : GETDNS_RRTYPE_A ), klass,
		    dnssec_extension_set, with_opt,
		    edns_maximum_udp_payload_size,
		    edns_extended_rcode, edns_version, edns_do_bit,
		    opt_options_size, noptions, options,
		    netreq_sz - sizeof(getdns_network_req), max_query_sz);

	return result;
}
