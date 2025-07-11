
/*
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

#include <kiwi.h>
#include <machinarium.h>
#include <odyssey.h>

void od_router_init(od_router_t *router, od_global_t *global)
{
	pthread_mutex_init(&router->lock, NULL);
	od_rules_init(&router->rules);
	od_list_init(&router->servers);
	od_route_pool_init(&router->route_pool);
	router->clients = 0;
	router->clients_routing = 0;
	router->servers_routing = 0;

	router->global = global;

	router->router_err_logger = od_err_logger_create_default();
}

static inline int od_router_immed_close_server_cb(od_server_t *server,
						  void **argv)
{
	(void)argv;

	/* remove server for server pool */
	od_server_set_pool_state(server, OD_SERVER_UNDEF);

	server->route = NULL;

	od_backend_close_connection(server);
	od_backend_close(server);

	return 0;
}

static inline int od_router_immed_close_cb(od_route_t *route, void **argv)
{
	od_route_lock(route);
	od_multi_pool_foreach(route->server_pools, OD_SERVER_IDLE,
			      od_router_immed_close_server_cb, argv);
	od_route_unlock(route);
	return 0;
}

void od_router_free(od_router_t *router)
{
	od_list_t *i;
	od_list_t *n;
	od_list_foreach_safe(&router->servers, i, n)
	{
		od_system_server_t *server;
		server = od_container_of(i, od_system_server_t, link);
		od_system_server_free(server);
	}

	od_router_foreach(router, od_router_immed_close_cb, NULL);
	od_route_pool_free(&router->route_pool);
	od_rules_free(&router->rules);
	pthread_mutex_destroy(&router->lock);
	od_err_logger_free(router->router_err_logger);
}

inline int od_router_foreach(od_router_t *router, od_route_pool_cb_t callback,
			     void **argv)
{
	od_router_lock(router);
	int rc;
	rc = od_route_pool_foreach(&router->route_pool, callback, argv);
	od_router_unlock(router);
	return rc;
}

static inline int od_router_reload_cb(od_route_t *route, void **argv)
{
	(void)argv;

	if (!route->rule->obsolete) {
		return 0;
	}

	od_route_lock(route);
	od_route_reload_pool(route);
	od_route_unlock(route);
	return 1;
}

static inline int od_drop_obsolete_rule_connections_cb(od_route_t *route,
						       void **argv)
{
	od_list_t *i;
	od_rule_t *rule = route->rule;
	od_list_t *obsolete_rules = argv[0];
	od_list_foreach(obsolete_rules, i)
	{
		od_rule_key_t *obsolete_rule;
		obsolete_rule = od_container_of(i, od_rule_key_t, link);
		assert(rule);
		assert(obsolete_rule);
		if (strcmp(rule->user_name, obsolete_rule->usr_name) == 0 &&
		    strcmp(rule->db_name, obsolete_rule->db_name) == 0 &&
		    od_address_range_equals(&rule->address_range,
					    &obsolete_rule->address_range)) {
			od_route_kill_client_pool(route);
			return 0;
		}
	}
	return 0;
}

int od_router_reconfigure(od_router_t *router, od_rules_t *rules)
{
	od_instance_t *instance = router->global->instance;
	od_router_lock(router);

	int updates;
	od_list_t added;
	od_list_t deleted;
	od_list_t to_drop;
	od_list_init(&added);
	od_list_init(&deleted);
	od_list_init(&to_drop);

	updates = od_rules_merge(&router->rules, rules, &added, &deleted,
				 &to_drop);

	if (updates > 0) {
		od_extension_t *extensions = router->global->extensions;
		od_list_t *i;
		od_list_t *j;
		od_module_t *modules = extensions->modules;

		od_list_foreach(&added, i)
		{
			od_rule_key_t *rk;
			rk = od_container_of(i, od_rule_key_t, link);
			od_log(&instance->logger, "reload config", NULL, NULL,
			       "added rule: %s %s %s", rk->usr_name,
			       rk->db_name, rk->address_range.string_value);
		}

		od_list_foreach(&deleted, i)
		{
			od_rule_key_t *rk;
			rk = od_container_of(i, od_rule_key_t, link);
			od_log(&instance->logger, "reload config", NULL, NULL,
			       "deleted rule: %s %s %s", rk->usr_name,
			       rk->db_name, rk->address_range.string_value);
		}

		{
			void *argv[] = { &to_drop };
			od_route_pool_foreach(
				&router->route_pool,
				od_drop_obsolete_rule_connections_cb, argv);
		}

		/* reloadcallback */
		od_list_foreach(&modules->link, i)
		{
			od_module_t *module;
			module = od_container_of(i, od_module_t, link);
			if (module->od_config_reload_cb == NULL)
				continue;

			if (module->od_config_reload_cb(&added, &deleted) ==
			    OD_MODULE_CB_FAIL_RETCODE) {
				break;
			}
		}

		od_list_foreach_safe(&added, i, j)
		{
			od_rule_key_t *rk;
			rk = od_container_of(i, od_rule_key_t, link);
			od_rule_key_free(rk);
		}

		od_list_foreach_safe(&deleted, i, j)
		{
			od_rule_key_t *rk;
			rk = od_container_of(i, od_rule_key_t, link);
			od_rule_key_free(rk);
		}

		od_list_foreach_safe(&to_drop, i, j)
		{
			od_rule_key_t *rk;
			rk = od_container_of(i, od_rule_key_t, link);
			od_rule_key_free(rk);
		}

		od_route_pool_foreach(&router->route_pool, od_router_reload_cb,
				      NULL);
	}

	od_router_unlock(router);
	return updates;
}

static inline int od_router_expire_server_cb(od_server_t *server, void **argv)
{
	od_list_t *expire_list = argv[0];
	int *count = argv[1];

	/* remove server for server pool */
	od_server_set_pool_state(server, OD_SERVER_UNDEF);

	od_list_append(expire_list, &server->link);
	(*count)++;

	return 0;
}

static inline int od_router_pool_ttl_expired(od_server_t *server)
{
	od_route_t *route = server->route;

	if (route->rule->pool->ttl == 0 /* disabled */) {
		return 0;
	}

	return server->idle_time >= route->rule->pool->ttl;
}

static inline int od_router_lifetime_expired(od_server_t *server,
					     uint64_t *now_us)
{
	od_route_t *route = server->route;
	uint64_t max_lifetime = route->rule->server_lifetime_us;

	if (max_lifetime == 0 /* disabled */) {
		return 0;
	}

	uint64_t server_life = *now_us - server->init_time_us;

	return server_life >= max_lifetime;
}

static inline int od_router_server_expired(od_server_t *server,
					   uint64_t *now_us)
{
	return od_router_pool_ttl_expired(server) ||
	       od_router_lifetime_expired(server, now_us);
}

static inline int od_router_expire_server_tick_cb(od_server_t *server,
						  void **argv)
{
	od_route_t *route = server->route;
	od_list_t *expire_list = argv[0];
	int *count = argv[1];
	uint64_t *now_us = argv[2];

	if (!server->offline) {
		int expired = od_router_server_expired(server, now_us);

		/* advance idle time for 1 sec */
		if (!expired) {
			server->idle_time++;
			return 0;
		}

		/*
		 * Do not expire more servers than we are allowed to connect at one time
		 * This avoids need to re-launch lot of connections together
		 */
		if (*count > route->rule->storage->server_max_routing)
			return 0;
	} // else remove server because we are forced to

	/* remove server for server pool */
	od_server_set_pool_state(server, OD_SERVER_UNDEF);

	/* add to expire list */
	od_list_append(expire_list, &server->link);
	(*count)++;

	return 0;
}

static inline int od_router_expire_cb(od_route_t *route, void **argv)
{
	od_route_lock(route);

	/* expire by config obsoletion */
	if (route->rule->obsolete &&
	    !od_client_pool_total(&route->client_pool)) {
		od_multi_pool_foreach(route->server_pools, OD_SERVER_IDLE,
				      od_router_expire_server_cb, argv);

		od_route_unlock(route);
		return 0;
	}

	int pool_ttl_disabled = route->rule->pool->ttl == 0;
	int server_lifetime_disabled = route->rule->server_lifetime_us == 0;

	if (pool_ttl_disabled && server_lifetime_disabled) {
		od_route_unlock(route);
		return 0;
	}

	od_multi_pool_foreach(route->server_pools, OD_SERVER_IDLE,
			      od_router_expire_server_tick_cb, argv);

	od_route_unlock(route);
	return 0;
}

int od_router_expire(od_router_t *router, od_list_t *expire_list)
{
	int count = 0;
	uint64_t now_us = machine_time_us();
	void *argv[] = { expire_list, &count, &now_us };
	od_router_foreach(router, od_router_expire_cb, argv);
	return count;
}

static inline int od_router_gc_cb(od_route_t *route, void **argv)
{
	od_route_pool_t *pool = argv[0];
	od_route_lock(route);

	if (od_multi_pool_total(route->server_pools) > 0 ||
	    od_client_pool_total(&route->client_pool) > 0)
		goto done;

	if (!od_route_is_dynamic(route) && !route->rule->obsolete)
		goto done;

	/* remove route from route pool */
	assert(pool->count > 0);
	pool->count--;
	od_list_unlink(&route->link);

	od_route_unlock(route);

	/* unref route rule and free route object */
	od_rules_unref(route->rule);
	od_route_free(route);
	return 0;
done:
	od_route_unlock(route);
	return 0;
}

void od_router_gc(od_router_t *router)
{
	void *argv[] = { &router->route_pool };
	od_router_foreach(router, od_router_gc_cb, argv);
}

void od_router_stat(od_router_t *router, uint64_t prev_time_us,
#ifdef PROM_FOUND
		    od_prom_metrics_t *metrics,
#endif
		    od_route_pool_stat_cb_t callback, void **argv)
{
	od_router_lock(router);
	od_route_pool_stat(&router->route_pool, prev_time_us,
#ifdef PROM_FOUND
			   metrics,
#endif
			   callback, argv);
	od_router_unlock(router);
}

od_router_status_t od_router_route(od_router_t *router, od_client_t *client)
{
	kiwi_be_startup_t *startup = &client->startup;
	od_instance_t *instance = router->global->instance;

	struct sockaddr_storage sa;
	int salen;
	struct sockaddr *saddr;
	int rc;
	salen = sizeof(sa);
	saddr = (struct sockaddr *)&sa;
	if (client->type == OD_POOL_CLIENT_EXTERNAL) {
		rc = machine_getpeername(client->io.io, saddr, &salen);
		if (rc == -1) {
			return OD_ROUTER_ERROR;
		}
	}

	/* match route */
	assert(startup->database.value_len);
	assert(startup->user.value_len);

	od_router_lock(router);

	/* match latest version of route rule */
	od_rule_t *rule =
		NULL; // initialize rule for (line 365) and flag '-Wmaybe-uninitialized'

	int sequential = instance->config.sequential_routing;
	switch (client->type) {
	case OD_POOL_CLIENT_INTERNAL:
		rule = od_rules_forward(&router->rules, startup->database.value,
					startup->user.value, NULL, 1,
					sequential);
		break;
	case OD_POOL_CLIENT_EXTERNAL:
		rule = od_rules_forward(&router->rules, startup->database.value,
					startup->user.value, &sa, 0,
					sequential);
		break;
	case OD_POOL_CLIENT_UNDEF: // create that case for correct work of '-Wswitch' flag
		break;
	}

	if (rule == NULL) {
		od_router_unlock(router);
		return OD_ROUTER_ERROR_NOT_FOUND;
	}
	od_debug(&instance->logger, "routing", NULL, NULL,
		 "matching rule: %s %s %s with %s routing type to %s client",
		 rule->db_name, rule->user_name,
		 rule->address_range.string_value,
		 rule->pool->routing_type == NULL ? "client visible" :
						    rule->pool->routing_type,
		 client->type == OD_POOL_CLIENT_INTERNAL ? "internal" :
							   "external");
	if (!od_rule_matches_client(rule->pool, client->type)) {
		// emulate not found error
		od_router_unlock(router);
		return OD_ROUTER_ERROR_NOT_FOUND;
	}

	/* force settings required by route */
	od_route_id_t id = { .database = startup->database.value,
			     .user = startup->user.value,
			     .database_len = startup->database.value_len,
			     .user_len = startup->user.value_len,
			     .physical_rep = false,
			     .logical_rep = false };
	if (rule->storage_db) {
		id.database = rule->storage_db;
		id.database_len = strlen(rule->storage_db) + 1;
	}
	if (rule->storage_user) {
		id.user = rule->storage_user;
		id.user_len = strlen(rule->storage_user) + 1;
	}
	if (startup->replication.value_len != 0) {
		if (strcmp(startup->replication.value, "database") == 0)
			id.logical_rep = true;
		else if (!parse_bool(startup->replication.value,
				     &id.physical_rep)) {
			od_router_unlock(router);
			return OD_ROUTER_ERROR_REPLICATION;
		}
	}
#ifdef LDAP_FOUND
	if (rule->ldap_storage_credentials_attr) {
		od_ldap_server_t *ldap_server = NULL;
		ldap_server =
			od_ldap_server_pull(&instance->logger, rule, false);
		if (ldap_server == NULL) {
			od_error(&instance->logger, "routing", client, NULL,
				 "failed to get ldap connection");
			od_router_unlock(router);
			return OD_ROUTER_ERROR_NOT_FOUND;
		}
		int ldap_rc = od_ldap_server_prepare(&instance->logger,
						     ldap_server, rule, client);
		switch (ldap_rc) {
		case OK_RESPONSE: {
			od_ldap_endpoint_lock(rule->ldap_endpoint);
			ldap_server->idle_timestamp = (int)time(NULL);
#if USE_POOL
			od_ldap_server_pool_set(
				rule->ldap_endpoint->ldap_search_pool,
				ldap_server, OD_SERVER_IDLE);
#else
			od_ldap_server_free(ldap_server);
#endif

			od_ldap_endpoint_unlock(rule->ldap_endpoint);
			id.user = client->ldap_storage_username;
			id.user_len = client->ldap_storage_username_len + 1;
			rule->storage_user = client->ldap_storage_username;
			rule->storage_user_len =
				client->ldap_storage_username_len;
			rule->storage_password = client->ldap_storage_password;
			rule->storage_password_len =
				client->ldap_storage_password_len;
			od_debug(&instance->logger, "routing", client, NULL,
				 "route->id.user changed to %s", id.user);
			break;
		}
		case LDAP_INSUFFICIENT_ACCESS: {
			od_ldap_endpoint_lock(rule->ldap_endpoint);
			ldap_server->idle_timestamp = (int)time(NULL);
#if USE_POOL
			od_ldap_server_pool_set(
				rule->ldap_endpoint->ldap_search_pool,
				ldap_server, OD_SERVER_IDLE);
#else
			od_ldap_server_free(ldap_server);
#endif

			od_ldap_endpoint_unlock(rule->ldap_endpoint);
			od_router_unlock(router);
			return OD_ROUTER_INSUFFICIENT_ACCESS;
		}
		default: {
			od_debug(&instance->logger, "routing", client, NULL,
				 "closing bad ldap connection, need relogin");
			od_ldap_endpoint_lock(rule->ldap_endpoint);
#if USE_POOl
			od_ldap_server_pool_set(
				rule->ldap_endpoint->ldap_search_pool,
				ldap_server, OD_SERVER_UNDEF);
#endif
			od_ldap_endpoint_unlock(rule->ldap_endpoint);
			od_ldap_server_free(ldap_server);
			od_router_unlock(router);
			return OD_ROUTER_ERROR_NOT_FOUND;
		}
		}
	}
#endif
	/* match or create dynamic route */
	od_route_t *route;
	route = od_route_pool_match(&router->route_pool, &id, rule);
	if (route == NULL) {
		route = od_route_pool_new(&router->route_pool, &id, rule);
		//od_debug()
		if (route == NULL) {
			od_router_unlock(router);
			return OD_ROUTER_ERROR;
		}
	}
	od_rules_ref(rule);

	od_route_lock(route);

	/* increase counter of new tot tcp connections */
	++route->tcp_connections;

	/* ensure route client_max limit */
	if (rule->client_max_set &&
	    od_client_pool_total(&route->client_pool) >= rule->client_max) {
		od_rules_unref(rule);
		od_route_unlock(route);
		od_router_unlock(router);

		/*
		 * we assign client's rule to pass connection limit to the place where
		 * error is handled Client does not actually belong to the pool
		 */
		client->rule = rule;

		od_router_status_t ret = OD_ROUTER_ERROR_LIMIT_ROUTE;
		if (route->extra_logging_enabled) {
			od_error_logger_store_err(route->err_logger, ret);
		}
		return ret;
	}
	od_router_unlock(router);

	/* add client to route client pool */
	od_client_pool_set(&route->client_pool, client, OD_CLIENT_PENDING);
	client->rule = rule;
	client->route = route;

	od_route_unlock(route);
	return OD_ROUTER_OK;
}

void od_router_unroute(od_router_t *router, od_client_t *client)
{
	(void)router;
	/* detach client from route */
	assert(client->route);
	assert(client->server == NULL);

	od_route_t *route = client->route;
	od_route_lock(route);
	od_client_pool_set(&route->client_pool, client, OD_CLIENT_UNDEF);
	client->route = NULL;
	od_route_unlock(route);
}

bool od_should_not_spun_connection_yet(int connections_in_pool, int pool_size,
				       int currently_routing, int max_routing)
{
	if (pool_size == 0)
		return currently_routing >= max_routing;
	/*
	 * This routine controls ramping of server connections.
	 * When we have a lot of server connections we try to avoid opening new
	 * in parallel. Meanwhile when we have no server connections we go at
	 * maximum configured parallelism.
	 *
	 * This equation means that we gradually reduce parallelism until we reach
	 * half of possible connections in the pool.
	 */
	max_routing =
		max_routing * (pool_size - connections_in_pool * 2) / pool_size;
	if (max_routing <= 0)
		max_routing = 1;
	return currently_routing >= max_routing;
}

#define MAX_BUZYLOOP_RETRY 10

od_router_status_t od_router_attach(od_router_t *router, od_client_t *client,
				    bool wait_for_idle,
				    const od_address_t *address)
{
	(void)router;
	od_route_t *route = client->route;
	assert(route != NULL);

	od_route_lock(route);

	od_multi_pool_element_t *pool_element =
		od_multi_pool_get_or_create(route->server_pools, address);
	if (pool_element == NULL) {
		return OD_ROUTER_ERROR;
	}

	od_server_pool_t *pool = &pool_element->pool;

	/* enqueue client (pending -> queue) */
	od_client_pool_set(&route->client_pool, client, OD_CLIENT_QUEUE);

	/* get client server from route server pool */
	bool restart_read = false;
	od_server_t *server;
	int busyloop_sleep = 0;
	int busyloop_retry = 0;
	for (;;) {
		server = od_pg_server_pool_next(pool, OD_SERVER_IDLE);
		if (server)
			goto attach;

		if (wait_for_idle) {
			/* special case, when we are interested only in an idle connection
			 * and do not want to start a new one */
			if (pool->count_active == 0) {
				od_route_unlock(route);
				return OD_ROUTER_ERROR_TIMEDOUT;
			}
		} else {
			/* Maybe start new connection, if pool_size is zero */
			/* Maybe start new connection, if we still have capacity for it */
			int connections_in_pool = od_server_pool_total(pool);
			int pool_size = route->rule->pool->size;
			uint32_t currently_routing =
				od_atomic_u32_of(&router->servers_routing);
			uint32_t max_routing = (uint32_t)route->rule->storage
						       ->server_max_routing;
			if (pool_size == 0 || connections_in_pool < pool_size) {
				if (od_should_not_spun_connection_yet(
					    connections_in_pool, pool_size,
					    (int)currently_routing,
					    (int)max_routing)) {
					// concurrent server connection in progress.
					od_route_unlock(route);
					machine_sleep(busyloop_sleep);
					busyloop_retry++;
					// TODO: support this opt in configure file
					if (busyloop_retry > MAX_BUZYLOOP_RETRY)
						busyloop_sleep = 1;
					od_route_lock(route);
					continue;
				} else {
					// We are allowed to spun new server connection
					break;
				}
			}
		}

		/*
		 * unsubscribe from pending client read events during the time we wait
		 * for an available server
		 */
		restart_read =
			restart_read || (bool)od_io_read_active(&client->io);
		od_route_unlock(route);

		int rc = od_io_read_stop(&client->io);
		if (rc == -1) {
			return OD_ROUTER_ERROR;
		}

		/*
		 * Wait wakeup condition for pool_timeout milliseconds.
		 *
		 * The condition triggered when a server connection
		 * put into idle state by DETACH events.
		 */
		uint32_t timeout = route->rule->pool->timeout;
		if (timeout == 0)
			timeout = UINT32_MAX;
		rc = od_route_wait(route, timeout);
		if (rc == -1) {
			return OD_ROUTER_ERROR_TIMEDOUT;
		}

		od_route_lock(route);
	}

	od_route_unlock(route);

	/* create new server object */
	server = od_server_allocate(
		route->rule->pool->reserve_prepared_statement);
	if (server == NULL)
		return OD_ROUTER_ERROR;
	od_id_generate(&server->id, "s");
	od_dbg_printf_on_dvl_lvl(1, "server %s%.*s has relay %p\n",
				 server->id.id_prefix,
				 (signed)sizeof(server->id.id), server->id.id,
				 &server->relay);
	server->global = client->global;
	server->route = route;
	server->pool_element = pool_element;

	od_route_lock(route);

attach:
	od_server_set_pool_state(server, OD_SERVER_ACTIVE);
	od_client_pool_set(&route->client_pool, client, OD_CLIENT_ACTIVE);

	client->server = server;
	server->client = client;
	server->idle_time = 0;
	server->key_client = client->key;

	assert(od_server_synchronized(server));

	/*
	* XXX: this logic breaks some external solutions that use
 	* PostgreSQL logical replication. Need to tests this and fix
	*
	 * walsender connections are in "graceful shutdown" mode since we cannot
	 * reuse it.
	 *
	if (route->id.physical_rep || route->id.logical_rep)
		server->offline = 1;

	*/

	od_route_unlock(route);

	/* attach server io to clients machine context */
	if (server->io.io) {
		od_io_attach(&server->io);
	}

	/* maybe restore read events subscription */
	if (restart_read)
		od_io_read_start(&client->io);

	return OD_ROUTER_OK;
}

void od_router_detach(od_router_t *router, od_client_t *client)
{
	(void)router;
	od_route_t *route = client->route;
	assert(route != NULL);

	/* detach from current machine event loop */
	od_server_t *server = client->server;

	assert(server != NULL);
	assert(od_server_synchronized(server));
	od_io_detach(&server->io);

	od_route_lock(route);

	client->server = NULL;
	server->client = NULL;

	machine_cond_propagate(server->io.on_read, NULL);
	machine_cond_propagate(server->io.on_write, NULL);

	if (od_likely(!server->offline)) {
		od_instance_t *instance = server->global->instance;
		if (route->id.physical_rep || route->id.logical_rep) {
			od_debug(&instance->logger, "expire-replication", NULL,
				 server, "closing replication connection");
			server->route = NULL;
			od_backend_close_connection(server);
			od_server_set_pool_state(server, OD_SERVER_UNDEF);
			od_backend_close(server);
		} else {
			od_server_set_pool_state(server, OD_SERVER_IDLE);
		}
	} else {
		od_instance_t *instance = server->global->instance;
		od_debug(&instance->logger, "expire", NULL, server,
			 "closing obsolete server connection");
		server->route = NULL;
		od_backend_close_connection(server);
		od_server_set_pool_state(server, OD_SERVER_UNDEF);
		od_backend_close(server);
	}
	od_client_pool_set(&route->client_pool, client, OD_CLIENT_PENDING);

	int signal = route->client_pool.count_queue > 0;
	od_route_unlock(route);

	/* notify waiters */
	if (signal) {
		od_route_signal(route);
	}
}

void od_router_close(od_router_t *router, od_client_t *client)
{
	(void)router;
	od_route_t *route = client->route;
	assert(route != NULL);

	od_server_t *server = client->server;

	od_backend_close_connection(server);

	od_route_lock(route);

	od_client_pool_set(&route->client_pool, client, OD_CLIENT_PENDING);
	od_server_set_pool_state(server, OD_SERVER_UNDEF);
	client->server = NULL;
	server->client = NULL;
	server->route = NULL;

	od_route_unlock(route);

	assert(server->io.io == NULL);
	od_server_free(server);
}

static inline int od_router_cancel_cmp(od_server_t *server, void **argv)
{
	/* check that server is attached and has corresponding cancellation key */
	return (server->client != NULL) &&
	       kiwi_key_cmp(&server->key_client, argv[0]);
}

static inline int od_router_cancel_cb(od_route_t *route, void **argv)
{
	od_route_lock(route);

	od_server_t *server;
	server = od_multi_pool_foreach(route->server_pools, OD_SERVER_ACTIVE,
				       od_router_cancel_cmp, argv);
	if (server) {
		od_router_cancel_t *cancel = argv[1];
		cancel->id = server->id;
		cancel->key = server->key;
		cancel->storage = od_rules_storage_copy(route->rule->storage);
		cancel->address = od_server_pool_address(server);
		od_route_unlock(route);
		if (cancel->storage == NULL)
			return -1;
		return 1;
	}

	od_route_unlock(route);
	return 0;
}

od_router_status_t od_router_cancel(od_router_t *router, kiwi_key_t *key,
				    od_router_cancel_t *cancel)
{
	/* match server by client forged key */
	void *argv[] = { key, cancel };
	int rc;
	rc = od_router_foreach(router, od_router_cancel_cb, argv);
	if (rc <= 0)
		return OD_ROUTER_ERROR_NOT_FOUND;
	return OD_ROUTER_OK;
}

static inline int od_router_kill_cb(od_route_t *route, void **argv)
{
	od_route_lock(route);
	od_route_kill_client(route, argv[0]);
	od_route_unlock(route);
	return 0;
}

void od_router_kill(od_router_t *router, od_id_t *id)
{
	void *argv[] = { id };
	od_router_foreach(router, od_router_kill_cb, argv);
}
