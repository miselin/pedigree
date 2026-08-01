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
#include <string.h>
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
};

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
