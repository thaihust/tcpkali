/*
 * Copyright (c) 2014, 2015  Machine Zone, Inc.
 * 
 * Original author: Lev Walkin <lwalkin@machinezone.com>
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.

 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <sysexits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>  /* gethostbyname(3) */
#include <errno.h>
#include <assert.h>

#include "tcpkali.h"
#include "tcpkali_iface.h"

/*
 * Note: struct sockaddr_in6 is larger than struct sockaddr, hence
 * the storage should be bigger. However, we shall not dereference
 * the AF_INET (struct sockaddr_in *) as it were a larger structure.
 * Therefore this code is rather complex.
 */
void address_add(struct addresses *aseq, struct sockaddr *sa) {
    /* Reallocate a bigger list and continue. Don't laugh. */
    aseq->addrs = realloc(aseq->addrs,
                          (aseq->n_addrs + 1) * sizeof(aseq->addrs[0]));
    assert(aseq->addrs);
    switch(sa->sa_family) {
    case AF_INET:
        *(struct sockaddr_in *)&aseq->addrs[aseq->n_addrs]
                = *(struct sockaddr_in *)sa;
        aseq->n_addrs++;
        break;
    case AF_INET6:
        *(struct sockaddr_in6 *)&aseq->addrs[aseq->n_addrs]
                = *(struct sockaddr_in6 *)sa;
        aseq->n_addrs++;
        break;
    default:
        assert(!"Not IPv4 and not IPv6");
        break;
    }
}

/*
 * Display destination addresses with a given prefix, separator and suffix.
 */
void fprint_addresses(FILE *fp, char *prefix, char *separator, char *suffix, struct addresses addresses) {
    for(size_t n = 0; n < addresses.n_addrs; n++) {
        if(n == 0) {
            fprintf(fp, "%s", prefix);
        } else {
            fprintf(fp, "%s", separator);
        }
        char buf[INET6_ADDRSTRLEN+64];
        fprintf(stderr, "%s",
            format_sockaddr(&addresses.addrs[n], buf, sizeof(buf)));
        if(n == addresses.n_addrs - 1) {
            fprintf(fp, "%s", suffix);
        }
    }
}

/*
 * Printable representation of a sockaddr.
 */
const char *format_sockaddr(struct sockaddr_storage *ss, char *buf, size_t size) {
    void *in_addr;
    uint16_t nport;
    switch(ss->ss_family) {
    case AF_INET:
        in_addr = &((struct sockaddr_in *)ss)->sin_addr;
        nport = ((struct sockaddr_in *)ss)->sin_port;
        break;
    case AF_INET6:
        in_addr = &((struct sockaddr_in6 *)ss)->sin6_addr;
        nport = ((struct sockaddr_in6 *)ss)->sin6_port;
        break;
    default:
        assert(!"ipv4 or ipv6 expected");
        return "<unknown>";
    }
    char ipbuf[INET6_ADDRSTRLEN];
    const char *ip = inet_ntop(ss->ss_family, in_addr, ipbuf, sizeof(ipbuf));
    snprintf(buf, size, "[%s]:%d", ip, ntohs(nport));
    return buf;
}

/*
 * Given a port, detect which addresses we can listen on, using this port.
 */
struct addresses detect_listen_addresses(int listen_port) {
    struct addresses addresses = { 0, 0 };
    struct addrinfo hints = {
            .ai_family = AF_UNSPEC,
            .ai_socktype = SOCK_STREAM,
            .ai_protocol = IPPROTO_TCP,
            .ai_flags = AI_PASSIVE | AI_NUMERICSERV | AI_ADDRCONFIG };
    char service[32];
    snprintf(service, sizeof(service), "%d", listen_port);

    struct addrinfo *res;
    int err = getaddrinfo(NULL, service, &hints, &res);
    if(err != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
        exit(EXIT_FAILURE);
    }

    /* Move all of the addresses into the separate storage */
    for(struct addrinfo *tmp = res; tmp; tmp = tmp->ai_next) {
        address_add(&addresses, tmp->ai_addr);
    }

    freeaddrinfo(res);

    fprint_addresses(stderr, "Listen on: ",
                               "\nListen on: ", "\n",
                               addresses);

    return addresses;
}

/*
 * Check whether we can bind to a specified IP.
 */
static int check_if_bindable_ip(struct sockaddr_storage *ss) {
    int rc;
    int lsock = socket(ss->ss_family, SOCK_STREAM, IPPROTO_TCP);
    assert(lsock != -1);
    rc = bind(lsock, (struct sockaddr *)ss, sockaddr_len(ss));
    close(lsock);
    if(rc == -1) {
        char buf[256];
        fprintf(stderr, "%s is not local: %s\n",
                        format_sockaddr(ss, buf, sizeof(buf)),
                        strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * Parse the specified IP as it were a source IP, and add it to the list.
 */
int add_source_ip(struct addresses *addresses, const char *optarg) {
    struct addrinfo hints = {
            .ai_family = AF_UNSPEC,
            .ai_socktype = SOCK_STREAM,
            .ai_protocol = IPPROTO_TCP,
            .ai_flags = AI_PASSIVE | AI_ADDRCONFIG };

    struct addrinfo *res;
    int err = getaddrinfo(optarg, NULL, &hints, &res);
    if(err != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
        return -1;
    }

    /* Move all of the addresses into the separate storage */
    for(struct addrinfo *tmp = res; tmp; tmp = tmp->ai_next) {
        address_add(addresses, tmp->ai_addr);
        if(check_if_bindable_ip(&addresses->addrs[addresses->n_addrs-1]) < 0) {
            freeaddrinfo(res);
            return -1;
        }
    }

    freeaddrinfo(res);

    return 0;
}
