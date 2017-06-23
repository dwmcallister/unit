
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Valentin V. Bartenev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_runtime.h>
#include <nxt_port.h>
#include <nxt_master_process.h>


static nxt_int_t nxt_runtime_inherited_listen_sockets(nxt_task_t *task,
    nxt_runtime_t *rt);
static nxt_int_t nxt_runtime_systemd_listen_sockets(nxt_task_t *task,
    nxt_runtime_t *rt);
static nxt_int_t nxt_runtime_event_engines(nxt_task_t *task, nxt_runtime_t *rt);
static nxt_int_t nxt_runtime_thread_pools(nxt_thread_t *thr, nxt_runtime_t *rt);
static void nxt_runtime_start(nxt_task_t *task, void *obj, void *data);
static void nxt_runtime_initial_start(nxt_task_t *task);
static void nxt_single_process_start(nxt_thread_t *thr, nxt_task_t *task,
    nxt_runtime_t *rt);
static void nxt_runtime_close_idle_connections(nxt_event_engine_t *engine);
static void nxt_runtime_exit(nxt_task_t *task, void *obj, void *data);
static nxt_int_t nxt_runtime_event_engine_change(nxt_task_t *task,
    nxt_runtime_t *rt);
static nxt_int_t nxt_runtime_conf_init(nxt_task_t *task, nxt_runtime_t *rt);
static nxt_int_t nxt_runtime_conf_read_cmd(nxt_task_t *task, nxt_runtime_t *rt);
static nxt_sockaddr_t *nxt_runtime_sockaddr_parse(nxt_task_t *task,
    nxt_mp_t *mp, nxt_str_t *addr);
static nxt_sockaddr_t *nxt_runtime_sockaddr_unix_parse(nxt_task_t *task,
    nxt_mp_t *mp, nxt_str_t *addr);
static nxt_sockaddr_t *nxt_runtime_sockaddr_inet6_parse(nxt_task_t *task,
    nxt_mp_t *mp, nxt_str_t *addr);
static nxt_sockaddr_t *nxt_runtime_sockaddr_inet_parse(nxt_task_t *task,
    nxt_mp_t *mp, nxt_str_t *addr);
static nxt_int_t nxt_runtime_hostname(nxt_task_t *task, nxt_runtime_t *rt);
static nxt_int_t nxt_runtime_log_files_init(nxt_runtime_t *rt);
static nxt_int_t nxt_runtime_log_files_create(nxt_task_t *task,
    nxt_runtime_t *rt);
static nxt_int_t nxt_runtime_pid_file_create(nxt_task_t *task,
    nxt_file_name_t *pid_file);

#if (NXT_THREADS)
static void nxt_runtime_thread_pool_destroy(nxt_task_t *task, nxt_runtime_t *rt,
    nxt_runtime_cont_t cont);
#endif


nxt_int_t
nxt_runtime_create(nxt_task_t *task)
{
    nxt_mp_t       *mp;
    nxt_int_t      ret;
    nxt_array_t    *listen_sockets;
    nxt_runtime_t  *rt;

    mp = nxt_mp_create(1024, 128, 256, 32);

    if (nxt_slow_path(mp == NULL)) {
        return NXT_ERROR;
    }

    rt = nxt_mp_zget(mp, sizeof(nxt_runtime_t));
    if (nxt_slow_path(rt == NULL)) {
        return NXT_ERROR;
    }

    task->thread->runtime = rt;
    rt->mem_pool = mp;

    rt->prefix = nxt_current_directory(mp);
    if (nxt_slow_path(rt->prefix == NULL)) {
        goto fail;
    }

    rt->conf_prefix = rt->prefix;

    rt->services = nxt_services_init(mp);
    if (nxt_slow_path(rt->services == NULL)) {
        goto fail;
    }

    listen_sockets = nxt_array_create(mp, 1, sizeof(nxt_listen_socket_t));
    if (nxt_slow_path(listen_sockets == NULL)) {
        goto fail;
    }

    rt->listen_sockets = listen_sockets;

    ret = nxt_runtime_inherited_listen_sockets(task, rt);
    if (nxt_slow_path(ret != NXT_OK)) {
        goto fail;
    }

    if (nxt_runtime_hostname(task, rt) != NXT_OK) {
        goto fail;
    }

    if (nxt_slow_path(nxt_runtime_log_files_init(rt) != NXT_OK)) {
        goto fail;
    }

    if (nxt_runtime_event_engines(task, rt) != NXT_OK) {
        goto fail;
    }

    if (nxt_slow_path(nxt_runtime_thread_pools(task->thread, rt) != NXT_OK)) {
        goto fail;
    }

    rt->start = nxt_runtime_initial_start;

    nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                       nxt_runtime_start, task, rt, NULL);

    return NXT_OK;

fail:

    nxt_mp_destroy(mp);

    return NXT_ERROR;
}


static nxt_int_t
nxt_runtime_inherited_listen_sockets(nxt_task_t *task, nxt_runtime_t *rt)
{
    u_char               *v, *p;
    nxt_int_t            type;
    nxt_array_t          *inherited_sockets;
    nxt_socket_t         s;
    nxt_listen_socket_t  *ls;

    v = (u_char *) getenv("NGINX");

    if (v == NULL) {
        return nxt_runtime_systemd_listen_sockets(task, rt);
    }

    nxt_log(task, NXT_LOG_CRIT, "using inherited listen sockets: %s", v);

    inherited_sockets = nxt_array_create(rt->mem_pool,
                                         1, sizeof(nxt_listen_socket_t));
    if (inherited_sockets == NULL) {
        return NXT_ERROR;
    }

    rt->inherited_sockets = inherited_sockets;

    for (p = v; *p != '\0'; p++) {

        if (*p == ';') {
            s = nxt_int_parse(v, p - v);

            if (nxt_slow_path(s < 0)) {
                nxt_log(task, NXT_LOG_CRIT, "invalid socket number "
                        "\"%s\" in NGINX environment variable, "
                        "ignoring the rest of the variable", v);
                return NXT_ERROR;
            }

            v = p + 1;

            ls = nxt_array_zero_add(inherited_sockets);
            if (nxt_slow_path(ls == NULL)) {
                return NXT_ERROR;
            }

            ls->socket = s;

            ls->sockaddr = nxt_getsockname(task, rt->mem_pool, s);
            if (nxt_slow_path(ls->sockaddr == NULL)) {
                return NXT_ERROR;
            }

            type = nxt_socket_getsockopt(task, s, SOL_SOCKET, SO_TYPE);
            if (nxt_slow_path(type == -1)) {
                return NXT_ERROR;
            }

            ls->sockaddr->type = (uint16_t) type;
        }
    }

    return NXT_OK;
}


static nxt_int_t
nxt_runtime_systemd_listen_sockets(nxt_task_t *task, nxt_runtime_t *rt)
{
    u_char               *nfd, *pid;
    nxt_int_t            n;
    nxt_array_t          *inherited_sockets;
    nxt_socket_t         s;
    nxt_listen_socket_t  *ls;

    /*
     * Number of listening sockets passed.  The socket
     * descriptors start from number 3 and are sequential.
     */
    nfd = (u_char *) getenv("LISTEN_FDS");
    if (nfd == NULL) {
        return NXT_OK;
    }

    /* The pid of the service process. */
    pid = (u_char *) getenv("LISTEN_PID");
    if (pid == NULL) {
        return NXT_OK;
    }

    n = nxt_int_parse(nfd, nxt_strlen(nfd));
    if (n < 0) {
        return NXT_OK;
    }

    if (nxt_pid != nxt_int_parse(pid, nxt_strlen(pid))) {
        return NXT_OK;
    }

    nxt_log(task, NXT_LOG_INFO, "using %s systemd listen sockets", n);

    inherited_sockets = nxt_array_create(rt->mem_pool,
                                         n, sizeof(nxt_listen_socket_t));
    if (inherited_sockets == NULL) {
        return NXT_ERROR;
    }

    rt->inherited_sockets = inherited_sockets;

    for (s = 3; s < n; s++) {
        ls = nxt_array_zero_add(inherited_sockets);
        if (nxt_slow_path(ls == NULL)) {
            return NXT_ERROR;
        }

        ls->socket = s;

        ls->sockaddr = nxt_getsockname(task, rt->mem_pool, s);
        if (nxt_slow_path(ls->sockaddr == NULL)) {
            return NXT_ERROR;
        }

        ls->sockaddr->type = SOCK_STREAM;
    }

    return NXT_OK;
}


static nxt_int_t
nxt_runtime_event_engines(nxt_task_t *task, nxt_runtime_t *rt)
{
    nxt_thread_t                 *thread;
    nxt_event_engine_t           *engine;
    const nxt_event_interface_t  *interface;

    interface = nxt_service_get(rt->services, "engine", NULL);

    if (nxt_slow_path(interface == NULL)) {
        /* TODO: log */
        return NXT_ERROR;
    }

    engine = nxt_event_engine_create(task, interface,
                                     nxt_master_process_signals, 0, 0);

    if (nxt_slow_path(engine == NULL)) {
        return NXT_ERROR;
    }

    thread = task->thread;
    thread->engine = engine;
    thread->fiber = &engine->fibers->fiber;

    engine->id = rt->last_engine_id++;

    nxt_queue_init(&rt->engines);
    nxt_queue_insert_tail(&rt->engines, &engine->link);

    return NXT_OK;
}


static nxt_int_t
nxt_runtime_thread_pools(nxt_thread_t *thr, nxt_runtime_t *rt)
{
#if (NXT_THREADS)
    nxt_int_t    ret;
    nxt_array_t  *thread_pools;

    thread_pools = nxt_array_create(rt->mem_pool, 1,
                                    sizeof(nxt_thread_pool_t *));

    if (nxt_slow_path(thread_pools == NULL)) {
        return NXT_ERROR;
    }

    rt->thread_pools = thread_pools;
    ret = nxt_runtime_thread_pool_create(thr, rt, 2, 60000 * 1000000LL);

    if (nxt_slow_path(ret != NXT_OK)) {
        return NXT_ERROR;
    }

#endif

    return NXT_OK;
}


static void
nxt_runtime_start(nxt_task_t *task, void *obj, void *data)
{
    nxt_uint_t     i;
    nxt_runtime_t  *rt;

    rt = obj;

    nxt_debug(task, "rt conf done");

    task->thread->log->ctx_handler = NULL;
    task->thread->log->ctx = NULL;

    if (nxt_runtime_conf_init(task, rt) != NXT_OK) {
        goto fail;
    }

    for (i = 0; i < nxt_init_modules_n; i++) {
        if (nxt_init_modules[i](task->thread, rt) != NXT_OK) {
            goto fail;
        }
    }

    if (nxt_runtime_log_files_create(task, rt) != NXT_OK) {
        goto fail;
    }

    if (nxt_runtime_event_engine_change(task, rt) != NXT_OK) {
        goto fail;
    }

#if (NXT_THREADS)

    /*
     * Thread pools should be destroyed before starting worker
     * processes, because thread pool semaphores will stick in
     * locked state in new processes after fork().
     */
    nxt_runtime_thread_pool_destroy(task, rt, rt->start);

#else

    rt->start(task->thread, rt);

#endif

    return;

fail:

    nxt_runtime_quit(task);
}


static void
nxt_runtime_initial_start(nxt_task_t *task)
{
    nxt_int_t                    ret;
    nxt_thread_t                 *thr;
    nxt_runtime_t                *rt;
    const nxt_event_interface_t  *interface;

    thr = task->thread;
    rt = thr->runtime;

    if (rt->inherited_sockets == NULL && rt->daemon) {

        if (nxt_process_daemon(task) != NXT_OK) {
            goto fail;
        }

        /*
         * An event engine should be updated after fork()
         * even if an event facility was not changed because:
         * 1) inherited kqueue descriptor is invalid,
         * 2) the signal thread is not inherited.
         */
        interface = nxt_service_get(rt->services, "engine", rt->engine);
        if (interface == NULL) {
            goto fail;
        }

        ret = nxt_event_engine_change(task->thread->engine, interface,
                                      rt->batch);
        if (ret != NXT_OK) {
            goto fail;
        }
    }

    ret = nxt_runtime_pid_file_create(task, rt->pid_file);
    if (ret != NXT_OK) {
        goto fail;
    }

    if (nxt_runtime_event_engine_change(task, rt) != NXT_OK) {
        goto fail;
    }

    thr->engine->max_connections = rt->engine_connections;

    if (rt->master_process) {
        if (nxt_master_process_start(thr, task, rt) != NXT_ERROR) {
            return;
        }

    } else {
        nxt_single_process_start(thr, task, rt);
        return;
    }

fail:

    nxt_runtime_quit(task);
}


static void
nxt_single_process_start(nxt_thread_t *thr, nxt_task_t *task, nxt_runtime_t *rt)
{
#if (NXT_THREADS)
    nxt_int_t  ret;

    ret = nxt_runtime_thread_pool_create(thr, rt, rt->auxiliary_threads,
                                       60000 * 1000000LL);

    if (nxt_slow_path(ret != NXT_OK)) {
        nxt_runtime_quit(task);
        return;
    }

#endif

    rt->type = NXT_PROCESS_SINGLE;

    nxt_runtime_listen_sockets_enable(task, rt);

    return;
}


void
nxt_runtime_quit(nxt_task_t *task)
{
    nxt_bool_t          done;
    nxt_runtime_t       *rt;
    nxt_event_engine_t  *engine;

    rt = task->thread->runtime;
    engine = task->thread->engine;

    nxt_debug(task, "exiting");

    done = 1;

    if (!engine->shutdown) {
        engine->shutdown = 1;

#if (NXT_THREADS)

        if (!nxt_array_is_empty(rt->thread_pools)) {
            nxt_runtime_thread_pool_destroy(task, rt, nxt_runtime_quit);
            done = 0;
        }

#endif

        if (rt->type == NXT_PROCESS_MASTER) {
            nxt_master_stop_worker_processes(task, rt);
            done = 0;
        }
    }

    nxt_runtime_close_idle_connections(engine);

    if (done) {
        nxt_work_queue_add(&engine->fast_work_queue, nxt_runtime_exit,
                           task, rt, engine);
    }
}


static void
nxt_runtime_close_idle_connections(nxt_event_engine_t *engine)
{
    nxt_conn_t        *c;
    nxt_queue_t       *idle;
    nxt_queue_link_t  *link, *next;

    nxt_debug(&engine->task, "close idle connections");

    idle = &engine->idle_connections;

    for (link = nxt_queue_head(idle);
         link != nxt_queue_tail(idle);
         link = next)
    {
        next = nxt_queue_next(link);
        c = nxt_queue_link_data(link, nxt_conn_t, link);

        if (!c->socket.read_ready) {
            nxt_queue_remove(link);
            nxt_conn_close(engine, c);
        }
    }
}


static void
nxt_runtime_exit(nxt_task_t *task, void *obj, void *data)
{
    nxt_runtime_t         *rt;
    nxt_event_engine_t  *engine;

    rt = obj;
    engine = data;

#if (NXT_THREADS)

    nxt_debug(task, "thread pools: %d", rt->thread_pools->nelts);

    if (!nxt_array_is_empty(rt->thread_pools)) {
        return;
    }

#endif

    if (rt->type <= NXT_PROCESS_MASTER) {
        if (rt->pid_file != NULL) {
            nxt_file_delete(rt->pid_file);
        }
    }

    if (!engine->event.signal_support) {
        nxt_event_engine_signals_stop(engine);
    }

    nxt_debug(task, "exit");

    exit(0);
    nxt_unreachable();
}


static nxt_int_t
nxt_runtime_event_engine_change(nxt_task_t *task, nxt_runtime_t *rt)
{
    nxt_event_engine_t           *engine;
    const nxt_event_interface_t  *interface;

    engine = task->thread->engine;

    if (engine->batch == rt->batch
        && nxt_strcmp(engine->event.name, rt->engine) == 0)
    {
        return NXT_OK;
    }

    interface = nxt_service_get(rt->services, "engine", rt->engine);

    if (interface != NULL) {
        return nxt_event_engine_change(engine, interface, rt->batch);
    }

    return NXT_ERROR;
}


void
nxt_runtime_event_engine_free(nxt_runtime_t *rt)
{
    nxt_queue_link_t    *link;
    nxt_event_engine_t  *engine;

    link = nxt_queue_first(&rt->engines);
    nxt_queue_remove(link);

    engine = nxt_queue_link_data(link, nxt_event_engine_t, link);
    nxt_event_engine_free(engine);
}


#if (NXT_THREADS)

static void nxt_runtime_thread_pool_init(void);
static void nxt_runtime_thread_pool_exit(nxt_task_t *task, void *obj,
    void *data);


nxt_int_t
nxt_runtime_thread_pool_create(nxt_thread_t *thr, nxt_runtime_t *rt,
    nxt_uint_t max_threads, nxt_nsec_t timeout)
{
    nxt_thread_pool_t   *thread_pool, **tp;

    tp = nxt_array_add(rt->thread_pools);
    if (tp == NULL) {
        return NXT_ERROR;
    }

    thread_pool = nxt_thread_pool_create(max_threads, timeout,
                                         nxt_runtime_thread_pool_init,
                                         thr->engine,
                                         nxt_runtime_thread_pool_exit);

    if (nxt_fast_path(thread_pool != NULL)) {
        *tp = thread_pool;
    }

    return NXT_OK;
}


static void
nxt_runtime_thread_pool_destroy(nxt_task_t *task, nxt_runtime_t *rt,
    nxt_runtime_cont_t cont)
{
    nxt_uint_t         n;
    nxt_thread_pool_t  **tp;

    rt->continuation = cont;

    n = rt->thread_pools->nelts;

    if (n == 0) {
        cont(task);
        return;
    }

    tp = rt->thread_pools->elts;

    do {
        nxt_thread_pool_destroy(*tp);

        tp++;
        n--;
    } while (n != 0);
}


static void
nxt_runtime_thread_pool_init(void)
{
#if (NXT_REGEX)
    nxt_regex_init(0);
#endif
}


static void
nxt_runtime_thread_pool_exit(nxt_task_t *task, void *obj, void *data)
{
    nxt_uint_t           i, n;
    nxt_runtime_t        *rt;
    nxt_thread_pool_t    *tp, **thread_pools;
    nxt_thread_handle_t  handle;

    tp = obj;

    if (data != NULL) {
        handle = (nxt_thread_handle_t) (uintptr_t) data;
        nxt_thread_wait(handle);
    }

    rt = task->thread->runtime;

    thread_pools = rt->thread_pools->elts;
    n = rt->thread_pools->nelts;

    nxt_debug(task, "thread pools: %ui", n);

    for (i = 0; i < n; i++) {

        if (tp == thread_pools[i]) {
            nxt_array_remove(rt->thread_pools, &thread_pools[i]);

            if (n == 1) {
                /* The last thread pool. */
                rt->continuation(task);
            }

            return;
        }
    }
}

#endif


static nxt_int_t
nxt_runtime_conf_init(nxt_task_t *task, nxt_runtime_t *rt)
{
    nxt_int_t                    ret;
    nxt_str_t                    *prefix;
    nxt_file_t                   *file;
    nxt_file_name_str_t          file_name;
    const nxt_event_interface_t  *interface;

    rt->daemon = 1;
    rt->master_process = 1;
    rt->engine_connections = 256;
    rt->worker_processes = 1;
    rt->auxiliary_threads = 2;
    rt->user_cred.user = "nobody";
    rt->group = NULL;
    rt->pid = "nginext.pid";
    rt->error_log = "error.log";

    if (nxt_runtime_conf_read_cmd(task, rt) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_runtime_controller_socket(task, rt) != NXT_OK) {
        return NXT_ERROR;
    }

    if (nxt_user_cred_get(task, &rt->user_cred, rt->group) != NXT_OK) {
        return NXT_ERROR;
    }

    /* An engine's parameters. */

    interface = nxt_service_get(rt->services, "engine", rt->engine);
    if (interface == NULL) {
        return NXT_ERROR;
    }

    rt->engine = interface->name;

    prefix = nxt_file_name_is_absolute(rt->pid) ? NULL : rt->prefix;

    ret = nxt_file_name_create(rt->mem_pool, &file_name, "%V%s%Z",
                               prefix, rt->pid);
    if (nxt_slow_path(ret != NXT_OK)) {
        return NXT_ERROR;
    }

    rt->pid_file = file_name.start;

    prefix = nxt_file_name_is_absolute(rt->error_log) ? NULL : rt->prefix;

    ret = nxt_file_name_create(rt->mem_pool, &file_name, "%V%s%Z",
                               prefix, rt->error_log);
    if (nxt_slow_path(ret != NXT_OK)) {
        return NXT_ERROR;
    }

    file = nxt_list_first(rt->log_files);
    file->name = file_name.start;

    return NXT_OK;
}


static nxt_int_t
nxt_runtime_conf_read_cmd(nxt_task_t *task, nxt_runtime_t *rt)
{
    char            *p, **argv;
    nxt_int_t       n;
    nxt_str_t       addr;
    nxt_sockaddr_t  *sa;

    argv = nxt_process_argv;

    while (*argv != NULL) {
        p = *argv++;

        if (nxt_strcmp(p, "--listen") == 0) {
            if (*argv == NULL) {
                nxt_log(task, NXT_LOG_CRIT,
                         "no argument for option \"--listen\"");
                return NXT_ERROR;
            }

            p = *argv++;

            addr.length = nxt_strlen(p);
            addr.start = (u_char *) p;

            sa = nxt_runtime_sockaddr_parse(task, rt->mem_pool, &addr);

            if (sa == NULL) {
                return NXT_ERROR;
            }

            rt->controller_listen = sa;

            continue;
        }

        if (nxt_strcmp(p, "--upstream") == 0) {
            if (*argv == NULL) {
                nxt_log(task, NXT_LOG_CRIT,
                              "no argument for option \"--upstream\"");
                return NXT_ERROR;
            }

            p = *argv++;

            rt->upstream.length = nxt_strlen(p);
            rt->upstream.start = (u_char *) p;

            continue;
        }

        if (nxt_strcmp(p, "--workers") == 0) {
            if (*argv == NULL) {
                nxt_log(task, NXT_LOG_CRIT,
                        "no argument for option \"--workers\"");
                return NXT_ERROR;
            }

            p = *argv++;
            n = nxt_int_parse((u_char *) p, nxt_strlen(p));

            if (n < 1) {
                nxt_log(task, NXT_LOG_CRIT,
                        "invalid number of workers: \"%s\"", p);
                return NXT_ERROR;
            }

            rt->worker_processes = n;

            continue;
        }

        if (nxt_strcmp(p, "--user") == 0) {
            if (*argv == NULL) {
                nxt_log(task, NXT_LOG_CRIT,
                        "no argument for option \"--user\"");
                return NXT_ERROR;
            }

            p = *argv++;

            rt->user_cred.user = p;

            continue;
        }

        if (nxt_strcmp(p, "--group") == 0) {
            if (*argv == NULL) {
                nxt_log(task, NXT_LOG_CRIT,
                        "no argument for option \"--group\"");
                return NXT_ERROR;
            }

            p = *argv++;

            rt->group = p;

            continue;
        }

        if (nxt_strcmp(p, "--pid") == 0) {
            if (*argv == NULL) {
                nxt_log(task, NXT_LOG_CRIT,
                        "no argument for option \"--pid\"");
                return NXT_ERROR;
            }

            p = *argv++;

            rt->pid = p;

            continue;
        }

        if (nxt_strcmp(p, "--log") == 0) {
            if (*argv == NULL) {
                nxt_log(task, NXT_LOG_CRIT,
                        "no argument for option \"--log\"");
                return NXT_ERROR;
            }

            p = *argv++;

            rt->error_log = p;

            continue;
        }

        if (nxt_strcmp(p, "--no-daemonize") == 0) {
            rt->daemon = 0;
            continue;
        }
    }

    return NXT_OK;
}


static nxt_sockaddr_t *
nxt_runtime_sockaddr_parse(nxt_task_t *task, nxt_mp_t *mp, nxt_str_t *addr)
{
    u_char  *p;
    size_t  length;

    length = addr->length;
    p = addr->start;

    if (length >= 5 && nxt_memcmp(p, "unix:", 5) == 0) {
        return nxt_runtime_sockaddr_unix_parse(task, mp, addr);
    }

    if (length != 0 && *p == '[') {
        return nxt_runtime_sockaddr_inet6_parse(task, mp, addr);
    }

    return nxt_runtime_sockaddr_inet_parse(task, mp, addr);
}


static nxt_sockaddr_t *
nxt_runtime_sockaddr_unix_parse(nxt_task_t *task, nxt_mp_t *mp, nxt_str_t *addr)
{
#if (NXT_HAVE_UNIX_DOMAIN)
    u_char          *p;
    size_t          length, socklen;
    nxt_sockaddr_t  *sa;

    /*
     * Actual sockaddr_un length can be lesser or even larger than defined
     * struct sockaddr_un length (see comment in unix/nxt_socket.h).  So
     * limit maximum Unix domain socket address length by defined sun_path[]
     * length because some OSes accept addresses twice larger than defined
     * struct sockaddr_un.  Also reserve space for a trailing zero to avoid
     * ambiguity, since many OSes accept Unix domain socket addresses
     * without a trailing zero.
     */
    const size_t max_len = sizeof(struct sockaddr_un)
                           - offsetof(struct sockaddr_un, sun_path) - 1;

    /* cutting "unix:" */
    length = addr->length - 5;
    p = addr->start + 5;

    if (length == 0) {
        nxt_log(task, NXT_LOG_CRIT,
                "unix domain socket \"%V\" name is invalid", addr);
        return NULL;
    }

    if (length > max_len) {
        nxt_log(task, NXT_LOG_CRIT,
                "unix domain socket \"%V\" name is too long", addr);
        return NULL;
    }

    socklen = offsetof(struct sockaddr_un, sun_path) + length + 1;

#if (NXT_LINUX)

    /*
     * Linux unix(7):
     *
     *   abstract: an abstract socket address is distinguished by the fact
     *   that sun_path[0] is a null byte ('\0').  The socket's address in
     *   this namespace is given by the additional bytes in sun_path that
     *   are covered by the specified length of the address structure.
     *   (Null bytes in the name have no special significance.)
     */
    if (p[0] == '@') {
        p[0] = '\0';
        socklen--;
    }

#endif

    sa = nxt_sockaddr_alloc(mp, socklen, addr->length);

    if (nxt_slow_path(sa == NULL)) {
        return NULL;
    }

    sa->type = SOCK_STREAM;

    sa->u.sockaddr_un.sun_family = AF_UNIX;
    nxt_memcpy(sa->u.sockaddr_un.sun_path, p, length);

    return sa;

#else  /* !(NXT_HAVE_UNIX_DOMAIN) */

    nxt_log(task, NXT_LOG_CRIT, "unix domain socket \"%V\" is not supported",
            addr);

    return NULL;

#endif
}


static nxt_sockaddr_t *
nxt_runtime_sockaddr_inet6_parse(nxt_task_t *task, nxt_mp_t *mp,
    nxt_str_t *addr)
{
#if (NXT_INET6)
    u_char           *p, *addr, *addr_end;
    size_t           length;
    nxt_mp_t         *mp;
    nxt_int_t        port;
    nxt_sockaddr_t   *sa;
    struct in6_addr  *in6_addr;

    length = addr->length - 1;
    p = addr->start + 1;

    addr_end = nxt_memchr(p, ']', length);

    if (addr_end == NULL) {
        goto invalid_address;
    }

    sa = nxt_sockaddr_alloc(mp, sizeof(struct sockaddr_in6));

    if (nxt_slow_path(sa == NULL)) {
        return NULL;
    }

    in6_addr = &sa->u.sockaddr_in6.sin6_addr;

    if (nxt_inet6_addr(in6_addr, p, addr_end - p) != NXT_OK) {
        goto invalid_address;
    }

    p = addr_end + 1;
    length = (p + length) - p;

    if (length == 0) {
        goto found;
    }

    if (*p == ':') {
        port = nxt_int_parse(p + 1, length - 1);

        if (port >= 1 && port <= 65535) {
            goto found;
        }
    }

    nxt_log(task, NXT_LOG_CRIT, "invalid port in \"%V\"", addr);

    return NULL;

found:

    sa->type = SOCK_STREAM;

    sa->u.sockaddr_in6.sin6_family = AF_INET6;
    sa->u.sockaddr_in6.sin6_port = htons((in_port_t) port);

    return sa;

invalid_address:

    nxt_log(task, NXT_LOG_CRIT, "invalid IPv6 address in \"%V\"", addr);

    return NULL;

#else

    nxt_log(task, NXT_LOG_CRIT, "IPv6 socket \"%V\" is not supported", addr);

    return NULL;

#endif
}


static nxt_sockaddr_t *
nxt_runtime_sockaddr_inet_parse(nxt_task_t *task, nxt_mp_t *mp,
    nxt_str_t *string)
{
    u_char          *p, *ip;
    size_t          length;
    in_addr_t       addr;
    nxt_int_t       port;
    nxt_sockaddr_t  *sa;

    addr = INADDR_ANY;

    length = string->length;
    ip = string->start;

    p = nxt_memchr(ip, ':', length);

    if (p == NULL) {

        /* single value port, or address */

        port = nxt_int_parse(ip, length);

        if (port > 0) {
            /* "*:XX" */

            if (port < 1 || port > 65535) {
                goto invalid_port;
            }

        } else {
            /* "x.x.x.x" */

            addr = nxt_inet_addr(ip, length);

            if (addr == INADDR_NONE) {
                goto invalid_port;
            }

            port = 8080;
        }

    } else {

        /* x.x.x.x:XX */

        p++;
        length = (ip + length) - p;
        port = nxt_int_parse(p, length);

        if (port < 1 || port > 65535) {
            goto invalid_port;
        }

        length = (p - 1) - ip;

        if (length != 1 || ip[0] != '*') {
            addr = nxt_inet_addr(ip, length);

            if (addr == INADDR_NONE) {
                goto invalid_addr;
            }

            /* "x.x.x.x:XX" */
        }
    }

    sa = nxt_sockaddr_alloc(mp, sizeof(struct sockaddr_in),
                            NXT_INET_ADDR_STR_LEN);
    if (nxt_slow_path(sa == NULL)) {
        return NULL;
    }

    sa->type = SOCK_STREAM;

    sa->u.sockaddr_in.sin_family = AF_INET;
    sa->u.sockaddr_in.sin_port = htons((in_port_t) port);
    sa->u.sockaddr_in.sin_addr.s_addr = addr;

    return sa;

invalid_port:

    nxt_log(task, NXT_LOG_CRIT, "invalid port in \"%V\"", string);

    return NULL;

invalid_addr:

    nxt_log(task, NXT_LOG_CRIT, "invalid address in \"%V\"", string);

    return NULL;
}


nxt_listen_socket_t *
nxt_runtime_listen_socket_add(nxt_runtime_t *rt, nxt_sockaddr_t *sa)
{
    nxt_mp_t             *mp;
    nxt_listen_socket_t  *ls;

    ls = nxt_array_zero_add(rt->listen_sockets);
    if (ls == NULL) {
        return NULL;
    }

    mp = rt->mem_pool;

    ls->sockaddr = nxt_sockaddr_create(mp, &sa->u.sockaddr, sa->socklen,
                                       sa->length);
    if (ls->sockaddr == NULL) {
        return NULL;
    }

    ls->sockaddr->type = sa->type;

    nxt_sockaddr_text(ls->sockaddr);

    ls->socket = -1;
    ls->backlog = NXT_LISTEN_BACKLOG;

    return ls;
}


static nxt_int_t
nxt_runtime_hostname(nxt_task_t *task, nxt_runtime_t *rt)
{
    size_t  length;
    char    hostname[NXT_MAXHOSTNAMELEN + 1];

    if (gethostname(hostname, NXT_MAXHOSTNAMELEN) != 0) {
        nxt_log(task, NXT_LOG_CRIT, "gethostname() failed %E", nxt_errno);
        return NXT_ERROR;
    }

    /*
     * Linux gethostname(2):
     *
     *    If the null-terminated hostname is too large to fit,
     *    then the name is truncated, and no error is returned.
     *
     * For this reason an additional byte is reserved in the buffer.
     */
    hostname[NXT_MAXHOSTNAMELEN] = '\0';

    length = nxt_strlen(hostname);
    rt->hostname.length = length;

    rt->hostname.start = nxt_mp_nget(rt->mem_pool, length);

    if (rt->hostname.start != NULL) {
        nxt_memcpy_lowcase(rt->hostname.start, (u_char *) hostname, length);
        return NXT_OK;
    }

    return NXT_ERROR;
}


static nxt_int_t
nxt_runtime_log_files_init(nxt_runtime_t *rt)
{
    nxt_file_t  *file;
    nxt_list_t  *log_files;

    log_files = nxt_list_create(rt->mem_pool, 1, sizeof(nxt_file_t));

    if (nxt_fast_path(log_files != NULL)) {
        rt->log_files = log_files;

        /* Preallocate the main error_log.  This allocation cannot fail. */
        file = nxt_list_zero_add(log_files);

        file->fd = NXT_FILE_INVALID;
        file->log_level = NXT_LOG_CRIT;

        return NXT_OK;
    }

    return NXT_ERROR;
}


nxt_file_t *
nxt_runtime_log_file_add(nxt_runtime_t *rt, nxt_str_t *name)
{
    nxt_int_t            ret;
    nxt_str_t            *prefix;
    nxt_file_t           *file;
    nxt_file_name_str_t  file_name;

    prefix = nxt_file_name_is_absolute(name->start) ? NULL : rt->prefix;

    ret = nxt_file_name_create(rt->mem_pool, &file_name, "%V%V%Z",
                               prefix, name);

    if (nxt_slow_path(ret != NXT_OK)) {
        return NULL;
    }

    nxt_list_each(file, rt->log_files) {

        /* STUB: hardecoded case sensitive/insensitive. */

        if (file->name != NULL
            && nxt_file_name_eq(file->name, file_name.start))
        {
            return file;
        }

    } nxt_list_loop;

    file = nxt_list_zero_add(rt->log_files);

    if (nxt_slow_path(file == NULL)) {
        return NULL;
    }

    file->fd = NXT_FILE_INVALID;
    file->log_level = NXT_LOG_CRIT;
    file->name = file_name.start;

    return file;
}


static nxt_int_t
nxt_runtime_log_files_create(nxt_task_t *task, nxt_runtime_t *rt)
{
    nxt_int_t   ret;
    nxt_file_t  *file;

    nxt_list_each(file, rt->log_files) {

        ret = nxt_file_open(task, file, O_WRONLY | O_APPEND, O_CREAT,
                            NXT_FILE_OWNER_ACCESS);

        if (ret != NXT_OK) {
            return NXT_ERROR;
        }

    } nxt_list_loop;

    file = nxt_list_first(rt->log_files);

    return nxt_file_stderr(file);
}


nxt_int_t
nxt_runtime_listen_sockets_create(nxt_task_t *task, nxt_runtime_t *rt)
{
    nxt_int_t            ret;
    nxt_uint_t           c, p, ncurr, nprev;
    nxt_listen_socket_t  *curr, *prev;

    curr = rt->listen_sockets->elts;
    ncurr = rt->listen_sockets->nelts;

    if (rt->inherited_sockets != NULL) {
        prev = rt->inherited_sockets->elts;
        nprev = rt->inherited_sockets->nelts;

    } else {
        prev = NULL;
        nprev = 0;
    }

    for (c = 0; c < ncurr; c++) {

        for (p = 0; p < nprev; p++) {

            if (nxt_sockaddr_cmp(curr[c].sockaddr, prev[p].sockaddr)) {

                ret = nxt_listen_socket_update(task, &curr[c], &prev[p]);
                if (ret != NXT_OK) {
                    return NXT_ERROR;
                }

                goto next;
            }
        }

        if (nxt_listen_socket_create(task, &curr[c], 0) != NXT_OK) {
            return NXT_ERROR;
        }

    next:

        continue;
    }

    return NXT_OK;
}


nxt_int_t
nxt_runtime_listen_sockets_enable(nxt_task_t *task, nxt_runtime_t *rt)
{
    nxt_uint_t           i, n;
    nxt_listen_socket_t  *ls;

    ls = rt->listen_sockets->elts;
    n = rt->listen_sockets->nelts;

    for (i = 0; i < n; i++) {
        if (ls[i].flags == NXT_NONBLOCK) {
            if (nxt_listen_event(task, &ls[i]) == NULL) {
                return NXT_ERROR;
            }
        }
    }

    return NXT_OK;
}


nxt_str_t *
nxt_current_directory(nxt_mp_t *mp)
{
    size_t     length;
    u_char     *p;
    nxt_str_t  *name;
    char       buf[NXT_MAX_PATH_LEN];

    length = nxt_dir_current(buf, NXT_MAX_PATH_LEN);

    if (nxt_fast_path(length != 0)) {
        name = nxt_str_alloc(mp, length + 1);

        if (nxt_fast_path(name != NULL)) {
            p = nxt_cpymem(name->start, buf, length);
            *p = '/';

            return name;
        }
    }

    return NULL;
}


static nxt_int_t
nxt_runtime_pid_file_create(nxt_task_t *task, nxt_file_name_t *pid_file)
{
    ssize_t     length;
    nxt_int_t   n;
    nxt_file_t  file;
    u_char      pid[NXT_INT64_T_LEN + NXT_LINEFEED_SIZE];

    nxt_memzero(&file, sizeof(nxt_file_t));

    file.name = pid_file;

    n = nxt_file_open(task, &file, O_WRONLY, O_CREAT | O_TRUNC,
                      NXT_FILE_DEFAULT_ACCESS);

    if (n != NXT_OK) {
        return NXT_ERROR;
    }

    length = nxt_sprintf(pid, pid + sizeof(pid), "%PI%n", nxt_pid) - pid;

    if (nxt_file_write(&file, pid, length, 0) != length) {
        return NXT_ERROR;
    }

    nxt_file_close(task, &file);

    return NXT_OK;
}


nxt_process_t *
nxt_runtime_process_new(nxt_runtime_t *rt)
{
    nxt_process_t  *process;

    /* TODO: memory failures. */

    process = nxt_mp_zalloc(rt->mem_pool, sizeof(nxt_process_t));
    if (nxt_slow_path(process == NULL)) {
        return NULL;
    }

    nxt_queue_init(&process->ports);

    /* TODO each process should have it's own mem_pool for ports allocation */
    process->mem_pool = rt->mem_pool;

    return process;
}


static nxt_int_t
nxt_runtime_lvlhsh_pid_test(nxt_lvlhsh_query_t *lhq, void *data)
{
    nxt_process_t  *process;

    process = data;

    if (lhq->key.length == sizeof(nxt_pid_t) &&
        *(nxt_pid_t *) lhq->key.start == process->pid) {
        return NXT_OK;
    }

    return NXT_DECLINED;
}

static const nxt_lvlhsh_proto_t  lvlhsh_processes_proto  nxt_aligned(64) = {
    NXT_LVLHSH_DEFAULT,
    nxt_runtime_lvlhsh_pid_test,
    nxt_lvlhsh_alloc,
    nxt_lvlhsh_free,
};

// Explicitly using 32 bit types to avoid possible alignment.
typedef struct {
    int32_t   pid;
    uint32_t  port_id;
} nxt_pid_port_id_t;

static nxt_int_t
nxt_runtime_lvlhsh_port_test(nxt_lvlhsh_query_t *lhq, void *data)
{
    nxt_port_t         *port;
    nxt_pid_port_id_t  *pid_port_id;

    port = data;
    pid_port_id = (nxt_pid_port_id_t *) lhq->key.start;

    if (lhq->key.length == sizeof(nxt_pid_port_id_t) &&
        pid_port_id->pid == port->pid &&
        pid_port_id->port_id == port->id) {
        return NXT_OK;
    }

    return NXT_DECLINED;
}

static const nxt_lvlhsh_proto_t  lvlhsh_ports_proto  nxt_aligned(64) = {
    NXT_LVLHSH_DEFAULT,
    nxt_runtime_lvlhsh_port_test,
    nxt_lvlhsh_alloc,
    nxt_lvlhsh_free,
};


nxt_process_t *
nxt_runtime_process_find(nxt_runtime_t *rt, nxt_pid_t pid)
{
    nxt_lvlhsh_query_t  lhq;

    lhq.key_hash = nxt_murmur_hash2(&pid, sizeof(pid));
    lhq.key.length = sizeof(pid);
    lhq.key.start = (u_char *) &pid;
    lhq.proto = &lvlhsh_processes_proto;

    /* TODO lock processes */

    if (nxt_lvlhsh_find(&rt->processes, &lhq) == NXT_OK) {
        nxt_thread_log_debug("process %PI found", pid);
        return lhq.value;
    }

    nxt_thread_log_debug("process %PI not found", pid);

    return NULL;
}


nxt_process_t *
nxt_runtime_process_get(nxt_runtime_t *rt, nxt_pid_t pid)
{
    nxt_process_t       *process;
    nxt_lvlhsh_query_t  lhq;

    lhq.key_hash = nxt_murmur_hash2(&pid, sizeof(pid));
    lhq.key.length = sizeof(pid);
    lhq.key.start = (u_char *) &pid;
    lhq.proto = &lvlhsh_processes_proto;

    /* TODO lock processes */

    if (nxt_lvlhsh_find(&rt->processes, &lhq) == NXT_OK) {
        nxt_thread_log_debug("process %PI found", pid);
        return lhq.value;
    }

    process = nxt_runtime_process_new(rt);
    if (nxt_slow_path(process == NULL)) {
        return NULL;
    }

    process->pid = pid;

    lhq.replace = 0;
    lhq.value = process;
    lhq.pool = rt->mem_pool;

    switch (nxt_lvlhsh_insert(&rt->processes, &lhq)) {

    case NXT_OK:
        if (rt->nprocesses == 0) {
            rt->mprocess = process;
        }

        rt->nprocesses++;

        nxt_thread_log_debug("process %PI insert", pid);
        break;

    default:
        nxt_thread_log_debug("process %PI insert failed", pid);
        break;
    }

    return process;
}


void
nxt_runtime_process_add(nxt_runtime_t *rt, nxt_process_t *process)
{
    nxt_port_t          *port;
    nxt_lvlhsh_query_t  lhq;

    lhq.key_hash = nxt_murmur_hash2(&process->pid, sizeof(process->pid));
    lhq.key.length = sizeof(process->pid);
    lhq.key.start = (u_char *) &process->pid;
    lhq.proto = &lvlhsh_processes_proto;
    lhq.replace = 0;
    lhq.value = process;
    lhq.pool = rt->mem_pool;

    /* TODO lock processes */

    switch (nxt_lvlhsh_insert(&rt->processes, &lhq)) {

    case NXT_OK:
        if (rt->nprocesses == 0) {
            rt->mprocess = process;
        }

        rt->nprocesses++;

        nxt_process_port_each(process, port) {

            nxt_runtime_port_add(rt, port);

        } nxt_process_port_loop;

        break;

    default:
        break;
    }
}


void
nxt_runtime_process_remove(nxt_runtime_t *rt, nxt_process_t *process)
{
    nxt_port_t          *port;
    nxt_lvlhsh_query_t  lhq;

    lhq.key_hash = nxt_murmur_hash2(&process->pid, sizeof(process->pid));
    lhq.key.length = sizeof(process->pid);
    lhq.key.start = (u_char *) &process->pid;
    lhq.proto = &lvlhsh_processes_proto;
    lhq.replace = 0;
    lhq.value = process;
    lhq.pool = rt->mem_pool;

    /* TODO lock processes */

    switch (nxt_lvlhsh_delete(&rt->processes, &lhq)) {

    case NXT_OK:
        rt->nprocesses--;

        nxt_process_port_each(process, port) {

            nxt_runtime_port_remove(rt, port);

        } nxt_process_port_loop;

        break;

    default:
        break;
    }
}


nxt_process_t *
nxt_runtime_process_first(nxt_runtime_t *rt, nxt_lvlhsh_each_t *lhe)
{
    nxt_memzero(lhe, sizeof(nxt_lvlhsh_each_t));

    lhe->proto = &lvlhsh_processes_proto;

    return nxt_runtime_process_next(rt, lhe);
}


nxt_port_t *
nxt_runtime_port_first(nxt_runtime_t *rt, nxt_lvlhsh_each_t *lhe)
{
    nxt_memzero(lhe, sizeof(nxt_lvlhsh_each_t));

    lhe->proto = &lvlhsh_ports_proto;

    return nxt_runtime_port_next(rt, lhe);
}


void
nxt_runtime_port_add(nxt_runtime_t *rt, nxt_port_t *port)
{
    nxt_pid_port_id_t   pid_port;
    nxt_lvlhsh_query_t  lhq;

    pid_port.pid = port->pid;
    pid_port.port_id = port->id;

    lhq.key_hash = nxt_murmur_hash2(&pid_port, sizeof(pid_port));
    lhq.key.length = sizeof(pid_port);
    lhq.key.start = (u_char *) &pid_port;
    lhq.proto = &lvlhsh_ports_proto;
    lhq.replace = 0;
    lhq.value = port;
    lhq.pool = rt->mem_pool;

    /* TODO lock ports */

    switch (nxt_lvlhsh_insert(&rt->ports, &lhq)) {

    case NXT_OK:
        break;

    default:
        nxt_thread_log_error(NXT_LOG_WARN, "port #%d for pid %PI add failed",
                             port->id, port->pid);
        break;
    }
}


void
nxt_runtime_port_remove(nxt_runtime_t *rt, nxt_port_t *port)
{
    nxt_pid_port_id_t   pid_port;
    nxt_lvlhsh_query_t  lhq;

    pid_port.pid = port->pid;
    pid_port.port_id = port->id;

    lhq.key_hash = nxt_murmur_hash2(&pid_port, sizeof(pid_port));
    lhq.key.length = sizeof(pid_port);
    lhq.key.start = (u_char *) &pid_port;
    lhq.proto = &lvlhsh_ports_proto;
    lhq.replace = 0;
    lhq.value = port;
    lhq.pool = rt->mem_pool;

    /* TODO lock ports */

    switch (nxt_lvlhsh_delete(&rt->ports, &lhq)) {

    case NXT_OK:
        break;

    default:
        break;
    }
}


nxt_port_t *
nxt_runtime_port_find(nxt_runtime_t *rt, nxt_pid_t pid,
    nxt_port_id_t port_id)
{
    nxt_pid_port_id_t   pid_port;
    nxt_lvlhsh_query_t  lhq;

    pid_port.pid = pid;
    pid_port.port_id = port_id;

    lhq.key_hash = nxt_murmur_hash2(&pid_port, sizeof(pid_port));
    lhq.key.length = sizeof(pid_port);
    lhq.key.start = (u_char *) &pid_port;
    lhq.proto = &lvlhsh_ports_proto;

    /* TODO lock ports */

    if (nxt_lvlhsh_find(&rt->ports, &lhq) == NXT_OK) {
        nxt_thread_log_debug("process port (%PI, %d) found", pid, port_id);
        return lhq.value;
    }

    nxt_thread_log_debug("process port (%PI, %d) not found", pid, port_id);

    return NULL;
}
