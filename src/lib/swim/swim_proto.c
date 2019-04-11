/*
 * Copyright 2010-2019, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "swim_proto.h"
#include "msgpuck.h"
#include "say.h"
#include "version.h"
#include "diag.h"
#include <sys/socket.h> /* AF_INET for FreeBSD. */

const char *swim_member_status_strs[] = {
	"alive",
	"dead",
	"left",
};

const char *swim_fd_msg_type_strs[] = {
	"ping",
	"ack",
};

int
swim_decode_map(const char **pos, const char *end, uint32_t *size,
		const char *prefix, const char *param_name)
{
	if (mp_typeof(**pos) != MP_MAP || *pos == end ||
	    mp_check_map(*pos, end) > 0) {
		diag_set(SwimError, "%s %s should be a map", prefix,
			 param_name);
		return -1;
	}
	*size = mp_decode_map(pos);
	return 0;
}

int
swim_decode_array(const char **pos, const char *end, uint32_t *size,
		  const char *prefix, const char *param_name)
{
	if (mp_typeof(**pos) != MP_ARRAY || *pos == end ||
	    mp_check_array(*pos, end) > 0) {
		diag_set(SwimError, "%s %s should be an array", prefix,
			 param_name);
		return -1;
	}
	*size = mp_decode_array(pos);
	return 0;
}

int
swim_decode_uint(const char **pos, const char *end, uint64_t *value,
		 const char *prefix, const char *param_name)
{
	if (mp_typeof(**pos) != MP_UINT || *pos == end ||
	    mp_check_uint(*pos, end) > 0) {
		diag_set(SwimError, "%s %s should be a uint", prefix,
			 param_name);
		return -1;
	}
	*value = mp_decode_uint(pos);
	return 0;
}

static inline int
swim_decode_ip(struct sockaddr_in *address, const char **pos, const char *end,
	       const char *prefix, const char *param_name)
{
	uint64_t ip;
	if (swim_decode_uint(pos, end, &ip, prefix, param_name) != 0)
		return -1;
	if (ip > UINT32_MAX) {
		diag_set(SwimError, "%s %s is an invalid IP address", prefix,
			 param_name);
		return -1;
	}
	address->sin_addr.s_addr = htonl(ip);
	return 0;
}

static inline int
swim_decode_port(struct sockaddr_in *address, const char **pos, const char *end,
		 const char *prefix, const char *param_name)
{
	uint64_t port;
	if (swim_decode_uint(pos, end, &port, prefix, param_name) != 0)
		return -1;
	if (port > UINT16_MAX) {
		diag_set(SwimError, "%s %s is an invalid port", prefix,
			 param_name);
		return -1;
	}
	address->sin_port = htons(port);
	return 0;
}

int
swim_decode_uuid(struct tt_uuid *uuid, const char **pos, const char *end,
		 const char *prefix, const char *param_name)
{
	if (mp_typeof(**pos) != MP_BIN || *pos == end ||
	    mp_check_binl(*pos, end) > 0) {
		diag_set(SwimError, "%s %s should be bin", prefix,
			 param_name);
		return -1;
	}
	if (mp_decode_binl(pos) != UUID_LEN || *pos + UUID_LEN > end) {
		diag_set(SwimError, "%s %s is invalid", prefix, param_name);
		return -1;
	}
	memcpy(uuid, *pos, UUID_LEN);
	*pos += UUID_LEN;
	return 0;
}

void
swim_member_def_create(struct swim_member_def *def)
{
	memset(def, 0, sizeof(*def));
	def->addr.sin_family = AF_INET;
	def->status = MEMBER_ALIVE;
}

/**
 * Decode a MessagePack value of @a key and store it in @a def.
 * @param key Key to read value of.
 * @param[in][out] pos Where a value is stored.
 * @param end End of the buffer.
 * @param prefix Error message prefix.
 * @param[out] def Where to store the value.
 *
 * @retval 0 Success.
 * @retval -1 Error.
 */
static int
swim_decode_member_key(enum swim_member_key key, const char **pos,
		       const char *end, const char *prefix,
		       struct swim_member_def *def)
{
	uint64_t tmp;
	switch (key) {
	case SWIM_MEMBER_STATUS:
		if (swim_decode_uint(pos, end, &tmp, prefix,
				     "member status") != 0)
			return -1;
		if (tmp >= swim_member_status_MAX) {
			diag_set(SwimError, "%s unknown member status", prefix);
			return -1;
		}
		def->status = (enum swim_member_status) tmp;
		break;
	case SWIM_MEMBER_ADDRESS:
		if (swim_decode_ip(&def->addr, pos, end, prefix,
				   "member address") != 0)
			return -1;
		break;
	case SWIM_MEMBER_PORT:
		if (swim_decode_port(&def->addr, pos, end, prefix,
				     "member port") != 0)
			return -1;
		break;
	case SWIM_MEMBER_UUID:
		if (swim_decode_uuid(&def->uuid, pos, end, prefix,
				     "member uuid") != 0)
			return -1;
		break;
	case SWIM_MEMBER_INCARNATION:
		if (swim_decode_uint(pos, end, &def->incarnation, prefix,
				     "member incarnation") != 0)
			return -1;
		break;
	default:
		unreachable();
	}
	return 0;
}

int
swim_member_def_decode(struct swim_member_def *def, const char **pos,
		       const char *end, const char *prefix)
{
	uint32_t size;
	if (swim_decode_map(pos, end, &size, prefix, "member") != 0)
		return -1;
	swim_member_def_create(def);
	for (uint32_t j = 0; j < size; ++j) {
		uint64_t key;
		if (swim_decode_uint(pos, end, &key, prefix,
				     "member key") != 0)
			return -1;
		if (key >= swim_member_key_MAX) {
			diag_set(SwimError, "%s unknown member key", prefix);
			return -1;
		}
		if (swim_decode_member_key(key, pos, end, prefix, def) != 0)
			return -1;
	}
	if (def->addr.sin_port == 0 || def->addr.sin_addr.s_addr == 0) {
		diag_set(SwimError, "%s member address is mandatory", prefix);
		return -1;
	}
	if (tt_uuid_is_nil(&def->uuid)) {
		diag_set(SwimError, "%s member uuid is mandatory", prefix);
		return -1;
	}
	return 0;
}

void
swim_src_uuid_bin_create(struct swim_src_uuid_bin *header,
			 const struct tt_uuid *uuid)
{
	header->k_uuid = SWIM_SRC_UUID;
	header->m_uuid = 0xc4;
	header->m_uuid_len = UUID_LEN;
	memcpy(header->v_uuid, uuid, UUID_LEN);
}

void
swim_fd_header_bin_create(struct swim_fd_header_bin *header,
			  enum swim_fd_msg_type type, uint64_t incarnation)
{
	header->k_header = SWIM_FAILURE_DETECTION;
	header->m_header = 0x82;

	header->k_type = SWIM_FD_MSG_TYPE;
	header->v_type = type;

	header->k_incarnation = SWIM_FD_INCARNATION;
	header->m_incarnation = 0xcf;
	header->v_incarnation = mp_bswap_u64(incarnation);
}

int
swim_failure_detection_def_decode(struct swim_failure_detection_def *def,
				  const char **pos, const char *end,
				  const char *prefix)
{
	uint32_t size;
	if (swim_decode_map(pos, end, &size, prefix, "root") != 0)
		return -1;
	memset(def, 0, sizeof(*def));
	def->type = swim_fd_msg_type_MAX;
	if (size != 2) {
		diag_set(SwimError, "%s root map should have two keys - "\
			 "message type and incarnation", prefix);
		return -1;
	}
	for (int i = 0; i < (int) size; ++i) {
		uint64_t key;
		if (swim_decode_uint(pos, end, &key, prefix, "a key") != 0)
			return -1;
		switch(key) {
		case SWIM_FD_MSG_TYPE:
			if (swim_decode_uint(pos, end, &key, prefix,
					     "message type") != 0)
				return -1;
			if (key >= swim_fd_msg_type_MAX) {
				diag_set(SwimError, "%s unknown message type",
					 prefix);
				return -1;
			}
			def->type = key;
			break;
		case SWIM_FD_INCARNATION:
			if (swim_decode_uint(pos, end, &def->incarnation,
					     prefix, "incarnation") != 0)
				return -1;
			break;
		default:
			diag_set(SwimError, "%s unexpected key", prefix);
			return -1;
		}
	}
	if (def->type == swim_fd_msg_type_MAX) {
		diag_set(SwimError, "%s message type should be specified",
			 prefix);
		return -1;
	}
	return 0;
}

void
swim_anti_entropy_header_bin_create(struct swim_anti_entropy_header_bin *header,
				    uint16_t batch_size)
{
	header->k_anti_entropy = SWIM_ANTI_ENTROPY;
	header->m_anti_entropy = 0xdc;
	header->v_anti_entropy = mp_bswap_u16(batch_size);
}

static inline void
swim_passport_bin_create(struct swim_passport_bin *passport)
{
	passport->k_status = SWIM_MEMBER_STATUS;
	passport->k_addr = SWIM_MEMBER_ADDRESS;
	passport->m_addr = 0xce;
	passport->k_port = SWIM_MEMBER_PORT;
	passport->m_port = 0xcd;
	passport->k_uuid = SWIM_MEMBER_UUID;
	passport->m_uuid = 0xc4;
	passport->m_uuid_len = UUID_LEN;
	passport->k_incarnation = SWIM_MEMBER_INCARNATION;
	passport->m_incarnation = 0xcf;
}

static inline void
swim_passport_bin_fill(struct swim_passport_bin *passport,
		       const struct sockaddr_in *addr,
		       const struct tt_uuid *uuid,
		       enum swim_member_status status, uint64_t incarnation)
{
	passport->v_status = status;
	passport->v_addr = mp_bswap_u32(ntohl(addr->sin_addr.s_addr));
	passport->v_port = mp_bswap_u16(ntohs(addr->sin_port));
	memcpy(passport->v_uuid, uuid, UUID_LEN);
	passport->v_incarnation = mp_bswap_u64(incarnation);
}

void
swim_member_bin_fill(struct swim_member_bin *header,
		     const struct sockaddr_in *addr, const struct tt_uuid *uuid,
		     enum swim_member_status status, uint64_t incarnation)
{
	swim_passport_bin_fill(&header->passport, addr, uuid, status,
			       incarnation);
}

void
swim_member_bin_create(struct swim_member_bin *header)
{
	header->m_header = 0x85;
	swim_passport_bin_create(&header->passport);
}

void
swim_diss_header_bin_create(struct swim_diss_header_bin *header,
			    uint16_t batch_size)
{
	header->k_header = SWIM_DISSEMINATION;
	header->m_header = 0xdc;
	header->v_header = mp_bswap_u16(batch_size);
}

void
swim_event_bin_create(struct swim_event_bin *header)
{
	swim_passport_bin_create(&header->passport);
}

void
swim_event_bin_fill(struct swim_event_bin *header,
		    enum swim_member_status status,
		    const struct sockaddr_in *addr, const struct tt_uuid *uuid,
		    uint64_t incarnation)
{
	header->m_header = 0x85;
	swim_passport_bin_fill(&header->passport, addr, uuid, status,
			       incarnation);
}

void
swim_meta_header_bin_create(struct swim_meta_header_bin *header,
			    const struct sockaddr_in *src)
{
	header->m_header = 0x83;
	header->k_version = SWIM_META_TARANTOOL_VERSION;
	header->m_version = 0xce;
	header->v_version = mp_bswap_u32(tarantool_version_id());
	header->k_addr = SWIM_META_SRC_ADDRESS;
	header->m_addr = 0xce;
	header->v_addr = mp_bswap_u32(ntohl(src->sin_addr.s_addr));
	header->k_port = SWIM_META_SRC_PORT;
	header->m_port = 0xcd;
	header->v_port = mp_bswap_u16(ntohs(src->sin_port));
}

int
swim_meta_def_decode(struct swim_meta_def *def, const char **pos,
		     const char *end)
{
	const char *prefix = "invalid meta section:";
	uint32_t size;
	if (swim_decode_map(pos, end, &size, prefix, "root") != 0)
		return -1;
	memset(def, 0, sizeof(*def));
	def->src.sin_family = AF_INET;
	for (uint32_t i = 0; i < size; ++i) {
		uint64_t key;
		if (swim_decode_uint(pos, end, &key, prefix, "a key") != 0)
			return -1;
		switch (key) {
		case SWIM_META_TARANTOOL_VERSION:
			if (swim_decode_uint(pos, end, &key, prefix,
					     "version") != 0)
				return -1;
			if (key > UINT32_MAX) {
				diag_set(SwimError, "%s invalid version, too "\
					 "big", prefix);
				return -1;
			}
			def->version = key;
			break;
		case SWIM_META_SRC_ADDRESS:
			if (swim_decode_ip(&def->src, pos, end, prefix,
					   "source address") != 0)
				return -1;
			break;
		case SWIM_META_SRC_PORT:
			if (swim_decode_port(&def->src, pos, end, prefix,
					     "source port") != 0)
				return -1;
			break;
		default:
			diag_set(SwimError, "%s unknown key", prefix);
			return -1;
		}
	}
	if (def->version == 0) {
		diag_set(SwimError, "%s version is mandatory", prefix);
		return -1;
	}
	if (def->src.sin_port == 0 || def->src.sin_addr.s_addr == 0) {
		diag_set(SwimError, "%s source address is mandatory", prefix);
		return -1;
	}
	return 0;
}

void
swim_quit_bin_create(struct swim_quit_bin *header, uint64_t incarnation)
{
	header->k_quit = SWIM_QUIT;
	header->m_quit = 0x81;
	header->k_incarnation = SWIM_QUIT_INCARNATION;
	header->m_incarnation = 0xcf;
	header->v_incarnation = mp_bswap_u64(incarnation);
}