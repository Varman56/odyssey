#pragma once

/*
 * machinarium.
 *
 * cooperative multitasking engine.
 */

#ifdef __cplusplus
extern "C" {
#endif
#include "c.h"
#include "zpq_stream.h"
#include "bind.h"

#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>

#include "macro.h"
#include "channel_limit.h"

#if __GNUC__ >= 4
#define MACHINE_API __attribute__((visibility("default")))
#else
#define MACHINE_API
#endif

typedef void (*machine_coroutine_t)(void *arg);

#define mm_yield machine_sleep(0);

#define MM_CERT_HASH_LEN EVP_MAX_MD_SIZE

/* library handles */

typedef struct machine_cond_private machine_cond_t;
typedef struct machine_msg_private machine_msg_t;
typedef struct machine_channel_private machine_channel_t;
typedef struct machine_tls_private machine_tls_t;
typedef struct machine_iov_private machine_iov_t;
typedef struct machine_io_private machine_io_t;
typedef struct machine_wait_list machine_wait_list_t;
typedef struct machine_wait_flag machine_wait_flag_t;
typedef struct machine_wait_group machine_wait_group_t;
typedef struct machine_mutex machine_mutex_t;

/* configuration */

MACHINE_API void machinarium_set_stack_size(int size);

MACHINE_API void machinarium_set_pool_size(int size);

MACHINE_API void machinarium_set_coroutine_cache_size(int size);

MACHINE_API void machinarium_set_msg_cache_gc_size(int size);

/* main */

MACHINE_API int machinarium_init(void);

MACHINE_API void machinarium_free(void);

/* machine control */

MACHINE_API int64_t machine_create(char *name, machine_coroutine_t, void *arg);

MACHINE_API void machine_stop_current(void);

MACHINE_API int machine_active(void);

MACHINE_API uint64_t machine_self(void);

MACHINE_API void **machine_thread_private(void);

MACHINE_API int machine_wait(uint64_t machine_id);

MACHINE_API int machine_stop(uint64_t machine_id);

/* time */

MACHINE_API uint64_t machine_time_ms(void);

MACHINE_API uint64_t machine_time_us(void);

MACHINE_API uint32_t machine_timeofday_sec(void);

MACHINE_API void
machine_stat(uint64_t *coroutine_count, uint64_t *coroutine_cache_count,
	     uint64_t *msg_allocated, uint64_t *msg_cache_count,
	     uint64_t *msg_cache_gc_count, uint64_t *msg_cache_size);

/* signals */

MACHINE_API int machine_signal_init(sigset_t *, sigset_t *);

MACHINE_API int machine_signal_wait(uint32_t time_ms);

/* coroutine */

MACHINE_API int64_t machine_coroutine_create(machine_coroutine_t, void *arg);

MACHINE_API int64_t machine_coroutine_create_named(machine_coroutine_t, void *,
						   const char *);

MACHINE_API void machine_sleep(uint32_t time_ms);

MACHINE_API int machine_join(uint64_t coroutine_id);

MACHINE_API int machine_cancel(uint64_t coroutine_id);

MACHINE_API int machine_cancelled(void);

MACHINE_API int machine_timedout(void);

MACHINE_API int machine_errno(void);

MACHINE_API const char *machine_coroutine_get_name(uint64_t coroutine_id);

/* condition */

MACHINE_API machine_cond_t *machine_cond_create(void);

MACHINE_API void machine_cond_free(machine_cond_t *);

MACHINE_API void machine_cond_propagate(machine_cond_t *, machine_cond_t *);

MACHINE_API void machine_cond_signal(machine_cond_t *);

MACHINE_API int machine_cond_try(machine_cond_t *);

MACHINE_API int machine_cond_wait(machine_cond_t *, uint32_t time_ms);

/* msg */

MACHINE_API machine_msg_t *machine_msg_create(int reserve);

MACHINE_API machine_msg_t *machine_msg_create_or_advance(machine_msg_t *,
							 int size);

MACHINE_API void machine_msg_free(machine_msg_t *);

MACHINE_API void machine_msg_set_type(machine_msg_t *, int type);

MACHINE_API int machine_msg_type(machine_msg_t *);

MACHINE_API void *machine_msg_data(machine_msg_t *);

MACHINE_API int machine_msg_size(machine_msg_t *);

MACHINE_API int machine_msg_write(machine_msg_t *, void *buf, int size);

/* channel */

MACHINE_API machine_channel_t *machine_channel_create();

MACHINE_API void machine_channel_free(machine_channel_t *);

MACHINE_API void
machine_channel_assign_limit_policy(machine_channel_t *obj, int limit,
				    mm_channel_limit_policy_t policy);

MACHINE_API mm_retcode_t machine_channel_write(machine_channel_t *,
					       machine_msg_t *);

MACHINE_API machine_msg_t *machine_channel_read(machine_channel_t *,
						uint32_t time_ms);

MACHINE_API machine_msg_t *machine_channel_read_back(machine_channel_t *,
						     uint32_t time_ms);

MACHINE_API size_t machine_channel_get_size(machine_channel_t *chan);

/* tls */

MACHINE_API machine_tls_t *machine_tls_create(void);

MACHINE_API void machine_tls_free(machine_tls_t *);

MACHINE_API int machine_tls_set_verify(machine_tls_t *, char *);

MACHINE_API int machine_tls_set_server(machine_tls_t *, char *);

MACHINE_API int machine_tls_set_protocols(machine_tls_t *, char *);

MACHINE_API int machine_tls_set_ca_path(machine_tls_t *, char *);

MACHINE_API int machine_tls_set_ca_file(machine_tls_t *, char *);

MACHINE_API int machine_tls_set_cert_file(machine_tls_t *, char *);

MACHINE_API int machine_tls_set_key_file(machine_tls_t *, char *);

/* io control */

MACHINE_API machine_io_t *machine_io_create(void);

MACHINE_API void machine_io_free(machine_io_t *);

MACHINE_API int machine_io_attach(machine_io_t *);

MACHINE_API int machine_io_detach(machine_io_t *);

MACHINE_API char *machine_error(machine_io_t *);

MACHINE_API int machine_fd(machine_io_t *);

MACHINE_API int machine_set_nodelay(machine_io_t *, int enable);

MACHINE_API int machine_set_keepalive(machine_io_t *, int enable, int delay,
				      int interval, int probes,
				      int usr_timeout);

MACHINE_API int machine_advice_keepalive_usr_timeout(int delay, int interval,
						     int probes);

MACHINE_API int machine_set_tls(machine_io_t *, machine_tls_t *, uint32_t);
MACHINE_API int machine_io_is_tls(machine_io_t *);
MACHINE_API int machine_set_compression(machine_io_t *, char algorithm);

MACHINE_API int machine_io_verify(machine_io_t *, char *common_name);

/* dns */

MACHINE_API int machine_getsockname(machine_io_t *, struct sockaddr *, int *);

MACHINE_API int machine_getpeername(machine_io_t *, struct sockaddr *, int *);

MACHINE_API int machine_getaddrinfo(char *addr, char *service,
				    struct addrinfo *hints,
				    struct addrinfo **res, uint32_t time_ms);

/* io */

MACHINE_API int machine_connect(machine_io_t *, struct sockaddr *,
				uint32_t time_ms);

MACHINE_API int machine_connected(machine_io_t *);

MACHINE_API int machine_bind(machine_io_t *, struct sockaddr *, int);

MACHINE_API int machine_accept(machine_io_t *, machine_io_t **, int backlog,
			       int attach, uint32_t time_ms);

MACHINE_API int machine_eventfd(machine_io_t *);

MACHINE_API int machine_close(machine_io_t *);

MACHINE_API int machine_shutdown(machine_io_t *);

MACHINE_API int machine_shutdown_receptions(machine_io_t *);

/* iov */

MACHINE_API machine_iov_t *machine_iov_create(void);

MACHINE_API void machine_iov_free(machine_iov_t *);

MACHINE_API int machine_iov_add_pointer(machine_iov_t *, void *, int);

MACHINE_API int machine_iov_add(machine_iov_t *, machine_msg_t *);

MACHINE_API int machine_iov_pending(machine_iov_t *);

/* read */

MACHINE_API int machine_read_active(machine_io_t *);

MACHINE_API int machine_read_start(machine_io_t *, machine_cond_t *);

MACHINE_API int machine_read_stop(machine_io_t *);

MACHINE_API ssize_t machine_read_raw(machine_io_t *, void *, size_t);

/*
 * Returns 1 if there are some bytes in io, that are ready to be read.
 */
MACHINE_API int machine_read_pending(machine_io_t *);

MACHINE_API machine_msg_t *machine_read(machine_io_t *, size_t,
					uint32_t time_ms);

/* write */

MACHINE_API int machine_write_start(machine_io_t *, machine_cond_t *);

MACHINE_API int machine_write_stop(machine_io_t *);

MACHINE_API ssize_t machine_write_raw(machine_io_t *, void *, size_t, size_t *);

MACHINE_API ssize_t machine_writev_raw(machine_io_t *, machine_iov_t *);

MACHINE_API int machine_write(machine_io_t *, machine_msg_t *,
			      uint32_t time_ms);

/* lrand48 */
MACHINE_API long int machine_lrand48(void);

/* stat/debug */

MACHINE_API int machine_io_sysfd(machine_io_t *);

MACHINE_API int machine_io_mask(machine_io_t *);

MACHINE_API int machine_io_mm_fd(machine_io_t *);

/* tls cert hash */

MACHINE_API ssize_t machine_tls_cert_hash(
	machine_io_t *obj, unsigned char (*cert_hash)[MM_CERT_HASH_LEN],
	unsigned int *len);

/* compression */
MACHINE_API char
machine_compression_choose_alg(char *client_compression_algorithms);

/* debug tools */

// note: backtrace functions are currently slow
// if you want bt collection to be fast, impl should be rewritten
MACHINE_API const char *machine_get_backtrace_string();
MACHINE_API int machine_get_backtrace(void **entries, int max);

/* wait list */

/* 
A wait list is a structure that is similar to a futex (see futex(2) and futex(7) for details). 
It allows a coroutine to wait until a specific condition is met.
Wait lists can be shared among different workers and are suitable for implementing other thread synchronization primitives.

If compare-and-wait functionality is not needed, you can pass NULL when creating a wait list and simply use the wait(...) method. 
However, this may result in lost wake-ups, so do it only if acceptable.

Local progress is guaranteed (no coroutine starvation) but a FIFO ordering is not.
Spurious wake-ups are possible.  
*/

/* 
The `word` argument in create(...) is analogous to a futex word.
Pass NULL if compare_wait functionality isn't needed.
*/
MACHINE_API machine_wait_list_t *
machine_wait_list_create(atomic_uint_fast64_t *word);
MACHINE_API void machine_wait_list_destroy(machine_wait_list_t *wait_list);
MACHINE_API int machine_wait_list_wait(machine_wait_list_t *wait_list,
				       uint32_t timeout_ms);
MACHINE_API int machine_wait_list_compare_wait(machine_wait_list_t *wait_list,
					       uint64_t value,
					       uint32_t timeout_ms);
MACHINE_API void machine_wait_list_notify(machine_wait_list_t *wait_list);
MACHINE_API void machine_wait_list_notify_all(machine_wait_list_t *wait_list);

/* wait group */
MACHINE_API machine_wait_group_t *machine_wait_group_create();
MACHINE_API void machine_wait_group_destroy(machine_wait_group_t *group);
MACHINE_API void machine_wait_group_add(machine_wait_group_t *group);
MACHINE_API uint64_t machine_wait_group_count(machine_wait_group_t *group);
MACHINE_API void machine_wait_group_done(machine_wait_group_t *group);
MACHINE_API int machine_wait_group_wait(machine_wait_group_t *group,
					uint32_t timeout_ms);

/* wait flag */

/* 
A wait flag is a simple synchronization primitive. It is one-shot, in other words, it can't be unset.

It is safe to use from multiple workers. Both set and wait functions can be called multiple times.
*/
machine_wait_flag_t *machine_wait_flag_create();
void machine_wait_flag_destroy(machine_wait_flag_t *flag);
void machine_wait_flag_set(machine_wait_flag_t *flag);
int machine_wait_flag_wait(machine_wait_flag_t *flag, uint32_t timeout_ms);

/* mutex */
MACHINE_API machine_mutex_t *machine_mutex_create();
MACHINE_API void machine_mutex_destroy(machine_mutex_t *mutex);
/* returns 1 if mutex is locked, 0 otherwise */
MACHINE_API int machine_mutex_lock(machine_mutex_t *mutex, uint32_t timeout_ms);
MACHINE_API void machine_mutex_unlock(machine_mutex_t *mutex);

/* memory */
MACHINE_API void *machine_malloc(size_t size);
MACHINE_API void machine_free(void *ptr);
MACHINE_API void *machine_calloc(size_t nmemb, size_t size);
MACHINE_API void *machine_realloc(void *ptr, size_t size);

#ifdef __cplusplus
}
#endif
