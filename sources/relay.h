#pragma once

/*
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

#include <odyssey.h>
#include "status.h"

typedef struct od_relay od_relay_t;

// function may rewrite packet here
typedef od_frontend_status_t (*od_relay_on_packet_t)(od_relay_t *, char *data,
						     int size);
typedef void (*od_relay_on_read_t)(od_relay_t *, int size);

struct od_relay {
	int packet;
	int packet_skip;

	machine_msg_t *packet_full;
	int packet_full_pos;
	int require_full_prep_stmt;
	machine_iov_t *iov;
	machine_cond_t *base;
	od_io_t *src;
	od_io_t *dst;
	od_frontend_status_t error_read;
	od_frontend_status_t error_write;
	od_relay_on_packet_t on_packet;
	void *on_packet_arg;
	od_relay_on_read_t on_read;
	void *on_read_arg;
};

static inline od_frontend_status_t
od_relay_read_pending_aware(od_relay_t *relay);
static inline od_frontend_status_t od_relay_read(od_relay_t *relay);

static inline void od_relay_init(od_relay_t *relay, od_io_t *io)
{
	relay->packet = 0;
	relay->packet_skip = 0;
	relay->packet_full = NULL;
	relay->require_full_prep_stmt = 0;
	relay->packet_full_pos = 0;
	relay->iov = NULL;
	relay->base = NULL;
	relay->src = io;
	relay->dst = NULL;
	relay->error_read = OD_UNDEF;
	relay->error_write = OD_UNDEF;
	relay->on_packet = NULL;
	relay->on_packet_arg = NULL;
	relay->on_read = NULL;
	relay->on_read_arg = NULL;
}

static inline void od_relay_free(od_relay_t *relay)
{
	if (relay->packet_full) {
		machine_msg_free(relay->packet_full);
	}

	if (relay->iov) {
		machine_iov_free(relay->iov);
	}
}

static inline bool od_relay_data_pending(od_relay_t *relay)
{
	char *current = od_readahead_pos_read(&relay->src->readahead);
	char *end = od_readahead_pos(&relay->src->readahead);
	return current < end;
}

static inline od_frontend_status_t
od_relay_start(od_relay_t *relay, machine_cond_t *base,
	       od_frontend_status_t error_read,
	       od_frontend_status_t error_write, od_relay_on_read_t on_read,
	       void *on_read_arg, od_relay_on_packet_t on_packet,
	       void *on_packet_arg, bool reserve_session_server_connection)
{
	relay->error_read = error_read;
	relay->error_write = error_write;
	relay->on_packet = on_packet;
	relay->on_packet_arg = on_packet_arg;
	relay->on_read = on_read;
	relay->on_read_arg = on_read_arg;
	relay->base = base;

	if (relay->iov == NULL) {
		relay->iov = machine_iov_create();
	}
	if (relay->iov == NULL) {
		return OD_EOOM;
	}

	machine_cond_propagate(relay->src->on_read, base);
	machine_cond_propagate(relay->src->on_write, base);

	int rc;
	rc = od_io_read_start(relay->src);
	if (rc == -1)
		return relay->error_read;

	// If there is no new data from client we must reset read condition
	// to avoid attaching to a new server connection

	if (machine_cond_try(relay->src->on_read)) {
		rc = od_relay_read_pending_aware(relay);
		if (rc != OD_OK)
			return rc;
		// signal machine condition immediately if we are not requested for pending data wait
		if (od_likely(!reserve_session_server_connection ||
			      od_relay_data_pending(relay))) {
			// Seems like some data arrived
			machine_cond_signal(relay->src->on_read);
		}
	}

	return OD_OK;
}

static inline void od_relay_attach(od_relay_t *relay, od_io_t *dst)
{
	assert(relay->dst == NULL);
	relay->dst = dst;
}

static inline void od_relay_detach(od_relay_t *relay)
{
	if (!relay->dst)
		return;
	od_io_write_stop(relay->dst);
	relay->dst = NULL;
}

static inline int od_relay_stop(od_relay_t *relay)
{
	od_relay_detach(relay);
	od_io_read_stop(relay->src);
	return 0;
}

static inline int od_relay_full_packet_required(char *data,
						int require_full_prep_stmt)
{
	kiwi_header_t *header;
	header = (kiwi_header_t *)data;

	switch (header->type) {
	case KIWI_BE_PARAMETER_STATUS:
	case KIWI_BE_READY_FOR_QUERY:
	case KIWI_BE_ERROR_RESPONSE:
		return 1;
	case KIWI_FE_QUERY:
		return 1;
	case KIWI_FE_PARSE:
	case KIWI_FE_BIND:
	case KIWI_FE_DESCRIBE:
		return require_full_prep_stmt;
	default:
		return 0;
	}
}

static inline od_frontend_status_t od_relay_on_packet_msg(od_relay_t *relay,
							  machine_msg_t *msg)
{
	int rc;
	od_frontend_status_t status;
	char *data = machine_msg_data(msg);
	int size = machine_msg_size(msg);

	status = relay->on_packet(relay, data, size);

	switch (status) {
	case OD_OK:
	/* fallthrough */
	case OD_DETACH:
		rc = machine_iov_add(relay->iov, msg);
		if (rc == -1)
			return OD_EOOM;
		break;
	case OD_SKIP:
		status = OD_OK;
	/* fallthrough */
	case OD_REQ_SYNC:
	/* fallthrough */
	default:
		machine_msg_free(msg);
		break;
	}
	return status;
}

static inline od_frontend_status_t od_relay_on_packet(od_relay_t *relay,
						      char *data, int size)
{
	int rc;
	od_frontend_status_t status;
	status = relay->on_packet(relay, data, size);

	switch (status) {
	case OD_OK:
		/* fallthrough */
	case OD_DETACH:
		rc = machine_iov_add_pointer(relay->iov, data, size);

		od_dbg_printf_on_dvl_lvl(1, "relay %p advance msg %c\n", relay,
					 *data);
		if (rc == -1)
			return OD_EOOM;
		break;
	case OD_REQ_SYNC:
		/* fallthrough */
		relay->packet_skip = 1;
		break;
	case OD_SKIP:
		relay->packet_skip = 1;
		status = OD_OK;
		break;
	default:
		break;
	}

	return status;
}

__attribute__((hot)) static inline od_frontend_status_t
od_relay_process(od_relay_t *relay, int *progress, char *data, int size)
{
	*progress = 0;

	/* on packet start */
	int rc;
	if (relay->packet == 0) {
		/* If we are parsing beginning of next package, there should be no delayed packet*/
		assert(relay->packet_full == NULL);
		if (size < (int)sizeof(kiwi_header_t))
			return OD_UNDEF;

		uint32_t body;
		rc = kiwi_validate_header(data, sizeof(kiwi_header_t), &body);
		if (rc != 0)
			return OD_ESYNC_BROKEN;

		body -= sizeof(uint32_t);

		int total = sizeof(kiwi_header_t) + body;
		if (size >= total) {
			*progress = total;
			return od_relay_on_packet(relay, data, total);
		}

		*progress = size;

		relay->packet = total - size;
		relay->packet_skip = 0;

		rc = od_relay_full_packet_required(
			data, relay->require_full_prep_stmt);
		if (!rc)
			return od_relay_on_packet(relay, data, size);

		relay->packet_full = machine_msg_create(total);
		if (relay->packet_full == NULL)
			return OD_EOOM;
		char *dest;
		dest = machine_msg_data(relay->packet_full);
		memcpy(dest, data, size);
		relay->packet_full_pos = size;
		return OD_OK;
	}

	/* chunk */
	int to_parse = relay->packet;
	if (to_parse > size)
		to_parse = size;
	*progress = to_parse;
	relay->packet -= to_parse;

	if (relay->packet_full) {
		char *dest;
		dest = machine_msg_data(relay->packet_full);
		memcpy(dest + relay->packet_full_pos, data, to_parse);
		relay->packet_full_pos += to_parse;
		if (relay->packet > 0) {
			return OD_OK;
		}

		machine_msg_t *msg = relay->packet_full;
		relay->packet_full = NULL;
		relay->packet_full_pos = 0;
		return od_relay_on_packet_msg(relay, msg);
	}

	if (relay->packet_skip)
		return OD_OK;

	rc = machine_iov_add_pointer(relay->iov, data, to_parse);

	od_dbg_printf_on_dvl_lvl(1, "relay %p advance msg %c\n", relay, *data);
	if (rc == -1)
		return OD_EOOM;

	return OD_OK;
}

static inline od_frontend_status_t od_relay_pipeline(od_relay_t *relay)
{
	char *current = od_readahead_pos_read(&relay->src->readahead);
	char *end = od_readahead_pos(&relay->src->readahead);
	while (current < end) {
		int progress;
		od_frontend_status_t rc;
		rc = od_relay_process(relay, &progress, current, end - current);
		current += progress;
		od_readahead_pos_read_advance(&relay->src->readahead, progress);
		if (rc == OD_REQ_SYNC) {
			return OD_REQ_SYNC;
		}
		if (rc != OD_OK) {
			if (rc == OD_UNDEF)
				rc = OD_OK;
			return rc;
		}
	}
	return OD_OK;
}

/*
 * This is just like od_relay_read, but we must signal on read cond, when
 * there are pending bytes, otherwise it will be lost
 * (in case for tls cached bytes for example)
*/
static inline od_frontend_status_t
od_relay_read_pending_aware(od_relay_t *relay)
{
	od_frontend_status_t rc = od_relay_read(relay);
	if (rc == OD_READAHEAD_IS_FULL) {
		if (machine_read_pending(relay->src->io)) {
			machine_cond_signal(relay->src->on_read);
		}
		return OD_OK;
	}

	return rc;
}

/*
 * This can lead to lost of relay->src->on_read in case of full readahead
 * and some pending bytes available.
 * Consider using od_relay_read_pending_aware instead
 */
static inline od_frontend_status_t od_relay_read(od_relay_t *relay)
{
	int to_read;
	to_read = od_readahead_left(&relay->src->readahead);
	if (to_read == 0) {
		if (machine_read_pending(relay->src->io)) {
			/*
			 * This is situation, when we can read some bytes
			 * but there is no place in readahead for it.
			 * Therefore, this should be retry later, when readahead will
			 * have free space to place the bytes.
			*/
			return OD_READAHEAD_IS_FULL;
		}
		return OD_OK;
	}

	char *pos;
	pos = od_readahead_pos(&relay->src->readahead);

	int rc;
	rc = machine_read_raw(relay->src->io, pos, to_read);
	if (rc <= 0) {
		/* retry */
		int errno_ = machine_errno();
		if (errno_ == EAGAIN || errno_ == EWOULDBLOCK ||
		    errno_ == EINTR)
			return OD_OK;
		/* error or eof */
		return relay->error_read;
	}

	od_readahead_pos_advance(&relay->src->readahead, rc);

	/* update recv stats */
	relay->on_read(relay, rc);

	return OD_OK;
}

static inline od_frontend_status_t od_relay_write(od_relay_t *relay)
{
	assert(relay->dst);

	if (!machine_iov_pending(relay->iov))
		return OD_OK;

	int rc;
	rc = machine_writev_raw(relay->dst->io, relay->iov);
	if (rc < 0) {
		/* retry or error */
		int errno_ = machine_errno();
		if (errno_ == EAGAIN || errno_ == EWOULDBLOCK ||
		    errno_ == EINTR) {
			machine_sleep(1);
			return OD_OK;
		}
		return relay->error_write;
	}

	return OD_OK;
}

static inline od_frontend_status_t od_relay_step(od_relay_t *relay,
						 bool await_read)
{
	/* on read event */
	od_frontend_status_t retstatus;
	retstatus = OD_OK;
	int rc;
	int should_try_read;
	int pending;
	should_try_read = await_read ? (machine_cond_wait(relay->src->on_read,
							  UINT32_MAX) == 0) :
				       machine_cond_try(relay->src->on_read);

	pending = od_relay_data_pending(relay);
	if (should_try_read || pending) {
		rc = od_relay_read_pending_aware(relay);
		if (rc != OD_OK)
			return rc;
	}

	/* XXX: todo - do check first byte of next package, and
	* if KIWI_FE_QUERY, try to parse attach-time hint */
	if (relay->dst == NULL) {
		return OD_ATTACH;
	}

	rc = od_relay_pipeline(relay);

	if (rc == OD_REQ_SYNC) {
		retstatus = OD_REQ_SYNC;
	} else if (rc != OD_OK)
		return rc;

	if (should_try_read || pending) {
		if (machine_iov_pending(relay->iov)) {
			/* try to optimize write path and handle it right-away */
			machine_cond_signal(relay->dst->on_write);
		} else {
			od_readahead_reuse(&relay->src->readahead);
		}
	}

	if (relay->dst == NULL)
		return retstatus;

	/* on write event */
	if (machine_cond_try(relay->dst->on_write)) {
		rc = od_relay_write(relay);
		if (rc != OD_OK)
			return rc;

		if (!machine_iov_pending(relay->iov)) {
			rc = od_io_write_stop(relay->dst);
			if (rc == -1)
				return relay->error_write;

			od_readahead_reuse(&relay->src->readahead);

			rc = od_io_read_start(relay->src);
			if (rc == -1)
				return relay->error_read;
		} else {
			rc = od_io_write_start(relay->dst);
			if (rc == -1)
				return relay->error_write;
		}
	}

	return retstatus;
}

static inline od_frontend_status_t od_relay_flush(od_relay_t *relay)
{
	if (relay->dst == NULL)
		return OD_OK;

	if (!machine_iov_pending(relay->iov))
		return OD_OK;

	int rc;
	rc = od_relay_write(relay);
	if (rc != OD_OK)
		return rc;

	if (!machine_iov_pending(relay->iov))
		return OD_OK;

	rc = od_io_write_start(relay->dst);
	if (rc == -1)
		return relay->error_write;

	for (;;) {
		if (!machine_iov_pending(relay->iov))
			break;

		machine_cond_wait(relay->dst->on_write, UINT32_MAX);

		rc = od_relay_write(relay);
		if (rc != OD_OK) {
			od_io_write_stop(relay->dst);
			return rc;
		}
	}

	rc = od_io_write_stop(relay->dst);
	if (rc == -1)
		return relay->error_write;

	return OD_OK;
}
