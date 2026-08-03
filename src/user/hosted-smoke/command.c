/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include <errno.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/klog.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct loopback_server
{
    int listener;
    int phase;
    int failure;
    uint8_t received;
    int saw_eof;
};

enum loopback_phase
{
    loopback_accept_ready = 1,
    loopback_receive_ready = 2,
};

enum
{
    loopback_phase_yields = 4096,
    loopback_blocking_window_yields = 32,
    compute_probe_sleep_us = 500000,
    compute_probe_fallback_ns = 2000000000ULL,
    compute_probe_latest_wake_ns = 1500000000ULL,
};

struct compute_preemption_probe
{
    int stop;
    int fallback;
    uintptr_t parent_self;
    uintptr_t child_self[2];
    int child_syscall_ok[2];
    uint64_t counters[2];
};

struct compute_preemption_worker
{
    struct compute_preemption_probe *probe;
    int index;
};

struct detached_exit_probe
{
    int tid_word;
    int ready;
    int go;
    int failure;
};

static uint64_t monotonic_nanoseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0;
    }
    return ((uint64_t) now.tv_sec * 1000000000ULL) + now.tv_nsec;
}

static void *run_compute_preemption_worker(void *parameter)
{
    struct compute_preemption_worker *worker = parameter;
    struct compute_preemption_probe *probe = worker->probe;
    __atomic_store_n(
        &probe->child_self[worker->index], (uintptr_t) pthread_self(),
        __ATOMIC_RELEASE);
    const uint64_t started = monotonic_nanoseconds();
    __atomic_store_n(
        &probe->child_syscall_ok[worker->index], started != 0,
        __ATOMIC_RELEASE);
    const uint64_t deadline = started + compute_probe_fallback_ns;

    while (!__atomic_load_n(&probe->stop, __ATOMIC_ACQUIRE))
    {
        const uint64_t count = __atomic_add_fetch(
            &probe->counters[worker->index], 1, __ATOMIC_RELAXED);
        if ((count & 0x3fff) == 0)
        {
            const uint64_t now = monotonic_nanoseconds();
            if (!now || now >= deadline)
            {
                __atomic_store_n(&probe->fallback, 1, __ATOMIC_RELEASE);
                sched_yield();
            }
        }
    }

    return 0;
}

static int run_compute_preemption_test(void)
{
    struct compute_preemption_probe probe;
    memset(&probe, 0, sizeof(probe));
    probe.parent_self = (uintptr_t) pthread_self();
    struct compute_preemption_worker workers[2] = {
        {&probe, 0},
        {&probe, 1},
    };
    pthread_t threads[2];

    if (pthread_create(
            &threads[0], 0, run_compute_preemption_worker, &workers[0]))
    {
        return 1;
    }
    if (pthread_create(
            &threads[1], 0, run_compute_preemption_worker, &workers[1]))
    {
        __atomic_store_n(&probe.stop, 1, __ATOMIC_RELEASE);
        pthread_join(threads[0], 0);
        return 2;
    }
    klog(LOG_INFO, "HOSTED-SMOKE: PASS pthread-clone-state-switch");

    const uint64_t started = monotonic_nanoseconds();
    const int sleep_result = usleep(compute_probe_sleep_us);
    const uint64_t elapsed = monotonic_nanoseconds() - started;
    __atomic_store_n(&probe.stop, 1, __ATOMIC_RELEASE);

    const uintptr_t first_self = __atomic_load_n(
        &probe.child_self[0], __ATOMIC_ACQUIRE);
    const uintptr_t second_self = __atomic_load_n(
        &probe.child_self[1], __ATOMIC_ACQUIRE);
    const int child_contract =
        first_self && second_self && first_self != second_self &&
        first_self != probe.parent_self && second_self != probe.parent_self &&
        __atomic_load_n(&probe.child_syscall_ok[0], __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&probe.child_syscall_ok[1], __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&probe.counters[0], __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&probe.counters[1], __ATOMIC_ACQUIRE);
    if (child_contract)
    {
        klog(
            LOG_INFO,
            "HOSTED-SMOKE: PASS pthread-child-tls-args-syscall");
    }

    const int first_join = pthread_join(threads[0], 0);
    const int second_join = pthread_join(threads[1], 0);
    if (!first_join && !second_join)
    {
        klog(LOG_INFO, "HOSTED-SMOKE: PASS pthread-clear-tid-join");
    }
    if (
        sleep_result || first_join || second_join || !child_contract ||
        __atomic_load_n(&probe.fallback, __ATOMIC_ACQUIRE) ||
        elapsed > compute_probe_latest_wake_ns)
    {
        return 3;
    }

    return 0;
}

static void *run_detached_exit_worker(void *parameter)
{
    struct detached_exit_probe *probe = parameter;
    const long tid = syscall(SYS_set_tid_address, &probe->tid_word);
    if (tid <= 0)
    {
        __atomic_store_n(&probe->failure, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&probe->ready, 1, __ATOMIC_RELEASE);
        return 0;
    }

    __atomic_store_n(&probe->tid_word, (int) tid, __ATOMIC_RELEASE);
    __atomic_store_n(&probe->ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&probe->go, __ATOMIC_ACQUIRE))
    {
        sched_yield();
    }

    syscall(SYS_exit, 0);
    __atomic_store_n(&probe->failure, 2, __ATOMIC_RELEASE);
    return 0;
}

static int run_detached_clear_tid_test(void)
{
    struct detached_exit_probe probe;
    memset(&probe, 0, sizeof(probe));

    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes))
    {
        return 1;
    }
    if (pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED))
    {
        pthread_attr_destroy(&attributes);
        return 2;
    }

    pthread_t thread;
    const int create_result = pthread_create(
        &thread, &attributes, run_detached_exit_worker, &probe);
    pthread_attr_destroy(&attributes);
    if (create_result)
    {
        return 3;
    }

    for (int i = 0; i < 100000; ++i)
    {
        if (__atomic_load_n(&probe.ready, __ATOMIC_ACQUIRE))
        {
            break;
        }
        sched_yield();
    }
    if (
        !__atomic_load_n(&probe.ready, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&probe.failure, __ATOMIC_ACQUIRE))
    {
        return 4;
    }

    __atomic_store_n(&probe.go, 1, __ATOMIC_RELEASE);
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const int observed =
            __atomic_load_n(&probe.tid_word, __ATOMIC_ACQUIRE);
        if (!observed)
        {
            break;
        }

        const struct timespec timeout = {1, 0};
        syscall(SYS_futex, &probe.tid_word, 0, observed, &timeout);
    }

    if (
        __atomic_load_n(&probe.tid_word, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&probe.failure, __ATOMIC_ACQUIRE))
    {
        return 5;
    }

    return 0;
}

static int wait_for_phase(struct loopback_server *server, int phase)
{
    for (int i = 0; i < loopback_phase_yields; ++i)
    {
        if (__atomic_load_n(&server->phase, __ATOMIC_ACQUIRE) >= phase)
        {
            return 1;
        }
        sched_yield();
    }
    return 0;
}

static void yield_blocking_window(void)
{
    for (int i = 0; i < loopback_blocking_window_yields; ++i)
    {
        sched_yield();
    }
}

static void shutdown_and_close(int descriptor)
{
    if (descriptor >= 0)
    {
        shutdown(descriptor, SHUT_RDWR);
        close(descriptor);
    }
}

static void *run_loopback_server(void *parameter)
{
    struct loopback_server *server = parameter;
    __atomic_store_n(
        &server->phase, loopback_accept_ready, __ATOMIC_RELEASE);
    int connection = accept(server->listener, 0, 0);
    if (connection < 0)
    {
        server->failure = 1;
        return 0;
    }

    __atomic_store_n(
        &server->phase, loopback_receive_ready, __ATOMIC_RELEASE);
    ssize_t received = recv(connection, &server->received, 1, 0);
    if (received != 1)
    {
        server->failure = 2;
        close(connection);
        return 0;
    }

    received = recv(connection, &server->received, 1, 0);
    server->saw_eof = received == 0;
    if (!server->saw_eof)
    {
        server->failure = 3;
    }

    close(connection);
    return 0;
}

static int run_loopback_test(void)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
    {
        return 10;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *) &address, sizeof(address)) != 0)
    {
        close(listener);
        return 11;
    }

    socklen_t address_length = sizeof(address);
    if (
        getsockname(
            listener, (struct sockaddr *) &address, &address_length) != 0 ||
        !address.sin_port)
    {
        close(listener);
        return 12;
    }
    if (listen(listener, 1) != 0)
    {
        close(listener);
        return 13;
    }

    int client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0)
    {
        close(listener);
        return 14;
    }

    struct loopback_server server;
    memset(&server, 0, sizeof(server));
    server.listener = listener;
    pthread_t server_thread;
    int thread_error =
        pthread_create(&server_thread, 0, run_loopback_server, &server);
    if (thread_error)
    {
        close(client);
        close(listener);
        return 15;
    }

    if (!wait_for_phase(&server, loopback_accept_ready))
    {
        shutdown_and_close(client);
        shutdown_and_close(listener);
        pthread_join(server_thread, 0);
        return 16;
    }

    // Give the server a bounded opportunity to enter accept before connecting.
    yield_blocking_window();
    if (
        connect(
            client, (struct sockaddr *) &address, address_length) != 0)
    {
        shutdown_and_close(client);
        shutdown_and_close(listener);
        pthread_join(server_thread, 0);
        return 17;
    }

    if (!wait_for_phase(&server, loopback_receive_ready))
    {
        shutdown_and_close(client);
        shutdown_and_close(listener);
        pthread_join(server_thread, 0);
        return 18;
    }

    // Likewise, let the server reach recv while the client has sent no data.
    yield_blocking_window();
    const uint8_t expected = 0xA5;
    if (
        send(client, &expected, sizeof(expected), 0) !=
            (ssize_t) sizeof(expected) ||
        shutdown(client, SHUT_WR) != 0)
    {
        shutdown_and_close(client);
        shutdown_and_close(listener);
        pthread_join(server_thread, 0);
        return 19;
    }

    close(client);
    if (pthread_join(server_thread, 0) != 0)
    {
        close(listener);
        return 20;
    }
    close(listener);

    if (
        server.failure || server.received != expected ||
        !server.saw_eof)
    {
        return 30 + server.failure;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *stage = argc > 1 ? argv[1] : "shutdown";

    klog(LOG_INFO, "HOSTED-SMOKE: simple userspace command ran");
    if (!strcmp(stage, "command"))
    {
        const int preemption_result = run_compute_preemption_test();
        if (preemption_result)
        {
            klog(
                LOG_ERR,
                "HOSTED-SMOKE: FAIL userspace-compute-preemption: %d",
                preemption_result);
        }
        else
        {
            klog(
                LOG_INFO,
                "HOSTED-SMOKE: PASS userspace-compute-preemption");
        }

        const int detached_result = run_detached_clear_tid_test();
        if (detached_result)
        {
            klog(
                LOG_ERR,
                "HOSTED-SMOKE: FAIL pthread-clear-tid-detached: %d",
                detached_result);
        }
        else
        {
            klog(
                LOG_INFO,
                "HOSTED-SMOKE: PASS pthread-clear-tid-detached");
        }

        const int loopback_result = run_loopback_test();
        if (loopback_result)
        {
            klog(
                LOG_ERR,
                "HOSTED-SMOKE: FAIL posix-lwip-loopback-roundtrip: %d "
                "(errno %d)",
                loopback_result, errno);
        }
        else
        {
            klog(
                LOG_INFO,
                "HOSTED-SMOKE: PASS posix-lwip-loopback-roundtrip");
        }
    }
    if (!strcmp(stage, "shutdown"))
    {
        klog(LOG_INFO, "HOSTED-SMOKE: requesting clean shutdown");
    }

    if (reboot(0) != 0)
    {
        klog(LOG_ERR, "HOSTED-SMOKE: shutdown request failed: %d", errno);
        return 1;
    }

    return 0;
}
