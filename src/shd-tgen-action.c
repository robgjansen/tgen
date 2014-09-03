/*
 * See LICENSE for licensing information
 */

#include <arpa/inet.h>
#include <math.h>

#include "shd-tgen.h"

typedef struct _TGenActionStartData {
    guint64 time;
    guint64 serverport;
    TGenPeer socksproxy;
    TGenPool* peers;
} TGenActionStartData;

typedef struct _TGenActionEndData {
    guint64 time;
    guint64 count;
    guint64 size;
} TGenActionEndData;

typedef struct _TGenActionPauseData {
    guint64 time;
} TGenActionPauseData;

typedef struct _TGenActionTransferData {
    TGenTransferType type;
    TGenTransferProtocol protocol;
    guint64 size;
    TGenPool* peers;
} TGenActionTransferData;

struct _TGenAction {
    TGenActionType type;
    gpointer key;
    gboolean hasKey;
    gpointer data;
    guint magic;
};

static in_addr_t _tgengraph_toAddress(const gchar* hostname) {
    in_addr_t address = INADDR_NONE;

    /* get the address in network order */
    if (!g_ascii_strncasecmp(hostname, "localhost", 9)) {
        address = htonl(INADDR_LOOPBACK);
    } else if (!g_ascii_strncasecmp(hostname, "0.0.0.0", 7)) {
        address = htonl(INADDR_ANY);
    } else {
        struct addrinfo* info;
        if (!getaddrinfo((gchar*) hostname, NULL, NULL, &info)) {
            address = ((struct sockaddr_in*) (info->ai_addr))->sin_addr.s_addr;
            freeaddrinfo(info);
        } else {
            tgen_warning("error in getaddrinfo for host '%s'", hostname);
        }
    }

    return address;
}

static GError* _tgengraph_handlePeer(const gchar* attributeName,
        const gchar* peerStr, TGenPeer* peer) {
    g_assert(attributeName && peerStr);

    GError* error = NULL;

    /* split peer into host and port parts */
    gchar** tokens = g_strsplit(peerStr, (const gchar*) ":", 2);

    if (!tokens[0] || !tokens[1]) {
        error = g_error_new(G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                "expected peer syntax 'hostname:port' for attribute '%s'",
                attributeName);
        return error;
    }

    /* dont add my own address to the server pool */
    char myname[128];
    if (!gethostname(&myname[0], 128)
            && !g_ascii_strcasecmp(myname, tokens[0])) {
        tgen_info("refusing to place my address in server pool for attribute '%s'", attributeName);
        return NULL;
    }

    in_addr_t address = _tgengraph_toAddress(tokens[0]);
    in_port_t port = 0;
    guint64 portNum = g_ascii_strtoull(tokens[1], NULL, 10);
    g_strfreev(tokens);

    /* validate values */
    if (address == INADDR_ANY || address == INADDR_NONE) {
        error = g_error_new(G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                "invalid peer '%s' for host part of attribute '%s', "
                "expected 'localhost', '127.0.0.1', or valid node hostname",
                peerStr, attributeName);
    }
    if (portNum > UINT16_MAX) {
        error = g_error_new(G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                "invalid peer '%s' for port part of attribute '%s', "
                 "expected 16 bit unsigned integer", peerStr,
                attributeName);
    } else {
        port = (in_port_t) portNum;
    }

    if (!error) {
        gchar* ipStringBuffer = g_malloc0(INET6_ADDRSTRLEN + 1);
        const gchar* ipString = inet_ntop(AF_INET, &address, ipStringBuffer,
                INET6_ADDRSTRLEN);
        tgen_debug("parsed address '%s' and port '%u' from string '%s'", ipString, port, peerStr);
        g_free(ipStringBuffer);

        if (peer) {
            peer->address = address;
            peer->port = port;
        }
    }

    return error;
}

static GError* _tgengraph_handlePeers(const gchar* attributeName,
        const gchar* peersStr, TGenPool* peerPool) {
    g_assert(attributeName && peersStr);

    GError* error = NULL;

    /* split into peers */
    gchar** tokens = g_strsplit(peersStr, (const gchar*) ",", 0);

    /* handle each peer */
    for (int i = 0; tokens[i] != NULL; i++) {
        TGenPeer* peer = g_new0(TGenPeer, 1);

        error = _tgengraph_handlePeer(attributeName, tokens[i], peer);

        if (!error && peer->port > 0 && peerPool) {
            tgenpool_add(peerPool, peer);
        } else {
            /* didn't add the peer */
            g_free(peer);
            if (error) {
                /* some validation error */
                break;
            }
        }
    }

    g_strfreev(tokens);

    return error;
}

static GError* _tgengraph_handleBytes(const gchar* attributeName,
        const gchar* byteStr, guint64* bytesOut) {
    g_assert(attributeName && byteStr);

    GError* error = NULL;

    /* split into parts (format example: "10 MiB") */
    gchar** tokens = g_strsplit(byteStr, (const gchar*) " ", 2);
    gchar* bytesToken = tokens[0];
    gchar* suffixToken = tokens[1];

    glong bytesTokenLength = g_utf8_strlen(bytesToken, -1);
    for (glong i = 0; i < bytesTokenLength; i++) {
        gchar c = bytesToken[i];
        if (!g_ascii_isdigit(c)) {
            error = g_error_new(G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                    "non-digit byte '%c' in byte string '%s' for attribute '%s', "
                    "expected format like '10240' or '10 KiB'",
                    c, byteStr, attributeName);
            break;
        }
    }

    if (!error) {
        guint64 bytes = g_ascii_strtoull(bytesToken, NULL, 10);

        if (suffixToken) {
            gint base = 0, exponent = 0;

            if (!g_ascii_strcasecmp(suffixToken, "kb")) {
                base = 10, exponent = 3;
            } else if (!g_ascii_strcasecmp(suffixToken, "mb")) {
                base = 10, exponent = 6;
            } else if (!g_ascii_strcasecmp(suffixToken, "gb")) {
                base = 10, exponent = 9;
            } else if (!g_ascii_strcasecmp(suffixToken, "tb")) {
                base = 10, exponent = 12;
            } else if (!g_ascii_strcasecmp(suffixToken, "kib")) {
                base = 2, exponent = 10;
            } else if (!g_ascii_strcasecmp(suffixToken, "mib")) {
                base = 2, exponent = 20;
            } else if (!g_ascii_strcasecmp(suffixToken, "gib")) {
                base = 2, exponent = 30;
            } else if (!g_ascii_strcasecmp(suffixToken, "tib")) {
                base = 2, exponent = 40;
            } else {
                error = g_error_new(G_MARKUP_ERROR,
                        G_MARKUP_ERROR_INVALID_CONTENT,
                        "invalid bytes suffix '%s' in byte string '%s' for attribute '%s', "
                        "expected one of: 'kib','mib','gib','tib','kb','mb','gb', or 'tb'",
                        suffixToken, byteStr, attributeName);
            }

            if (!error && base && exponent) {
                bytes = (guint64) (bytes
                        * pow((gdouble) base, (gdouble) exponent));
            }
        }

        tgen_debug("parsed %lu bytes from string %s", bytes, byteStr);

        if (bytesOut) {
            *bytesOut = bytes;
        }
    }

    g_strfreev(tokens);

    return error;
}

static GError* _tgengraph_handleBoolean(const gchar* attributeName,
        const gchar* booleanStr, gboolean* booleanOut, gboolean* isFoundOut) {
    g_assert(attributeName && booleanStr);

    GError* error = NULL;
    gboolean boolean = FALSE;
    gboolean isFound = FALSE;

    if (!g_ascii_strcasecmp(booleanStr, "true")
            || !g_ascii_strcasecmp(booleanStr, "1")) {
        isFound = TRUE;
        boolean = TRUE;
    } else if (!g_ascii_strcasecmp(booleanStr, "false")
            || !g_ascii_strcasecmp(booleanStr, "0")) {
        isFound = TRUE;
        boolean = FALSE;
    } else {
        error = g_error_new(G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                "invalid content for attribute '%s', "
                "expected boolean value 'true' or 'false'",
                attributeName);
    }

    if (isFound) {
        tgen_debug("parsed boolean %i from string %s", *booleanOut, booleanStr);
        if (booleanOut) {
            *booleanOut = boolean;
        }
        if (isFoundOut) {
            *isFoundOut = isFound;
        }
    }

    return error;
}

void tgenaction_free(TGenAction* action) {
    TGEN_ASSERT(action);

    if(action->type == TGEN_ACTION_START) {
        TGenActionStartData* data = (TGenActionStartData*) action->data;
        if(data->peers) {
            tgenpool_unref(data->peers);
        }
    } else if(action->type == TGEN_ACTION_TRANSFER) {
        TGenActionTransferData* data = (TGenActionTransferData*) action->data;
        if(data->peers) {
            tgenpool_unref(data->peers);
        }
    }

    if(action->data) {
        g_free(action->data);
    }

    action->magic = 0;
    g_free(action);
}

TGenAction* tgenaction_newStartAction(const gchar* timeStr,
        const gchar* serverPortStr, const gchar* peersStr,
        const gchar* socksProxyStr, GError** error) {
    g_assert(error);

    /* the time is required */
    if (!g_ascii_strncasecmp(timeStr, "\0", (gsize) 1)) {
        *error = g_error_new(G_MARKUP_ERROR, G_MARKUP_ERROR_MISSING_ATTRIBUTE,
                "start action missing required attribute 'time'");
        return NULL;
    }
    /* a serverport is required */
    if (!g_ascii_strncasecmp(serverPortStr, "\0", (gsize) 1)) {
        *error = g_error_new(G_MARKUP_ERROR, G_MARKUP_ERROR_MISSING_ATTRIBUTE,
                "start action missing required attribute 'serverport'");
        return NULL;
    }
    /* a list of peers is required */
    if (!g_ascii_strncasecmp(peersStr, "\0", (gsize) 1)) {
        *error = g_error_new(G_MARKUP_ERROR, G_MARKUP_ERROR_MISSING_ATTRIBUTE,
                "start action missing required attribute 'peers'");
        return NULL;
    }

    /* a socks proxy address is optional */
    TGenPeer socksproxy;
    if (g_ascii_strncasecmp(socksProxyStr, "\0", (gsize) 1)) {
        *error = _tgengraph_handlePeer("socksproxy", socksProxyStr,
                &socksproxy);
        if (*error) {
            return NULL;
        }
    }

    /* validate the peer pool */
    TGenPool* peerPool = tgenpool_new();
    *error = _tgengraph_handlePeers("peers", peersStr, peerPool);
    if (*error) {
        tgenpool_unref(peerPool);
        return NULL;
    }

    /* if we get here, we have what we need and validated it */
    TGenAction* action = g_new0(TGenAction, 1);
    action->magic = TGEN_MAGIC;

    action->type = TGEN_ACTION_START;

    TGenActionStartData* data = g_new0(TGenActionStartData, 1);

    data->time = g_ascii_strtoull(timeStr, NULL, 10);
    data->serverport = g_ascii_strtoull(serverPortStr, NULL, 10);
    data->peers = peerPool;
    data->socksproxy = socksproxy;

    action->data = data;

    return action;
}

TGenAction* tgenaction_newEndAction(const gchar* timeStr, const gchar* countStr,
        const gchar* sizeStr, GError** error) {
    g_assert(error);

    /* time, count, and size are termination conditions, and all are optional */

    guint64 size = 0;
    if (g_ascii_strncasecmp(sizeStr, "\0", (gsize) 1)) {
        *error = _tgengraph_handleBytes("size", sizeStr, &size);
        if (*error) {
            return NULL;
        }
    }

    TGenAction* action = g_new0(TGenAction, 1);
    action->magic = TGEN_MAGIC;

    action->type = TGEN_ACTION_END;

    TGenActionEndData* data = g_new0(TGenActionEndData, 1);
    data->size = size;
    if (g_ascii_strncasecmp(timeStr, "\0", (gsize) 1)) {
        data->time = g_ascii_strtoull(timeStr, NULL, 10);
    }
    if (g_ascii_strncasecmp(countStr, "\0", (gsize) 1)) {
        data->count = g_ascii_strtoull(countStr, NULL, 10);
    }

    action->data = data;

    return action;
}

TGenAction* tgenaction_newPauseAction(const gchar* timeStr, GError** error) {
    g_assert(error);

    /* the time is required */
    if (!g_ascii_strncasecmp(timeStr, "\0", (gsize) 1)) {
        *error = g_error_new(G_MARKUP_ERROR, G_MARKUP_ERROR_MISSING_ATTRIBUTE,
                "pause action missing required attribute 'time'");
        return NULL;
    }

    TGenAction* action = g_new0(TGenAction, 1);
    action->magic = TGEN_MAGIC;

    action->type = TGEN_ACTION_PAUSE;

    TGenActionPauseData* data = g_new0(TGenActionPauseData, 1);
    data->time = g_ascii_strtoull(timeStr, NULL, 10);;

    action->data = data;

    return action;
}

TGenAction* tgenaction_newSynchronizeAction(GError** error) {
    TGenAction* action = g_new0(TGenAction, 1);
    action->magic = TGEN_MAGIC;

    action->type = TGEN_ACTION_SYNCHR0NIZE;

    return action;
}

TGenAction* tgenaction_newTransferAction(const gchar* typeStr,
        const gchar* protocolStr, const gchar* sizeStr, const gchar* peersStr,
        GError** error) {
    g_assert(error);

    /* type is required */
    TGenTransferType type = TGEN_TYPE_NONE;
    if (!g_ascii_strncasecmp(typeStr, "\0", (gsize) 1)) {
        *error = g_error_new(G_MARKUP_ERROR, G_MARKUP_ERROR_MISSING_ATTRIBUTE,
                "transfer action missing required attribute 'type'");
        return NULL;
    } else if (!g_ascii_strcasecmp(typeStr, "get")) {
        type = TGEN_TYPE_GET;
    } else if (!g_ascii_strcasecmp(typeStr, "put")) {
        type = TGEN_TYPE_PUT;
    } else {
        *error = g_error_new(G_MARKUP_ERROR, G_MARKUP_ERROR_UNKNOWN_ATTRIBUTE,
                "transfer action has unknown value '%s' for 'type' attribute",
                typeStr);
        return NULL;
    }

    /* protocol is required */
    TGenTransferProtocol protocol = TGEN_PROTOCOL_NONE;
    if (!g_ascii_strncasecmp(protocolStr, "\0", (gsize) 1)) {
        *error = g_error_new(G_MARKUP_ERROR, G_MARKUP_ERROR_MISSING_ATTRIBUTE,
                "transfer action missing required attribute 'protocol'");
        return NULL;
    } else if (!g_ascii_strcasecmp(protocolStr, "tcp")) {
        protocol = TGEN_PROTOCOL_TCP;
    } else if (!g_ascii_strcasecmp(protocolStr, "udp")) {
        protocol = TGEN_PROTOCOL_UDP;
    } else if (!g_ascii_strcasecmp(protocolStr, "pipe")) {
        protocol = TGEN_PROTOCOL_PIPE;
    } else if (!g_ascii_strcasecmp(protocolStr, "socketpair")) {
        protocol = TGEN_PROTOCOL_SOCKETPAIR;
    } else {
        *error = g_error_new(G_MARKUP_ERROR, G_MARKUP_ERROR_UNKNOWN_ATTRIBUTE,
                "transfer action has unknown value '%s' for 'protocol' attribute",
                protocolStr);
        return NULL;
    }

    /* size is required */
    if (!g_ascii_strncasecmp(sizeStr, "\0", (gsize) 1)) {
        *error = g_error_new(G_MARKUP_ERROR, G_MARKUP_ERROR_MISSING_ATTRIBUTE,
                "transfer action missing required attribute 'size'");
        return NULL;
    }
    guint64 size = 0;
    *error = _tgengraph_handleBytes("size", sizeStr, &size);
    if (*error) {
        return NULL;
    }

    /* peers are optional */
    TGenPool* peerPool = NULL;
    if (g_ascii_strncasecmp(peersStr, "\0", (gsize) 1)) {
        peerPool = tgenpool_new();
        *error = _tgengraph_handlePeers("peers", peersStr, peerPool);
        if (*error) {
            tgenpool_unref(peerPool);
            return NULL;
        }
    }

    TGenAction* action = g_new0(TGenAction, 1);
    action->magic = TGEN_MAGIC;

    action->type = TGEN_ACTION_TRANSFER;

    TGenActionTransferData* data = g_new0(TGenActionTransferData, 1);
    data->protocol = protocol;
    data->type = type;
    data->size = size;
    data->peers = peerPool;

    action->data = data;

    return action;

}

void tgenaction_setKey(TGenAction* action, gpointer key) {
    TGEN_ASSERT(action);
    action->key = key;
    action->hasKey = TRUE;
}

gpointer tgenaction_getKey(TGenAction* action) {
    TGEN_ASSERT(action);
    g_assert(action->hasKey);
    return action->key;
}

TGenActionType tgenaction_getType(TGenAction* action) {
    TGEN_ASSERT(action);
    return action->type;
}

guint64 tgenaction_getServerPort(TGenAction* action) {
    TGEN_ASSERT(action);
    g_assert(action->data && action->type == TGEN_ACTION_START);
    return ((TGenActionStartData*)action->data)->serverport;
}

TGenPeer tgenaction_getSocksProxy(TGenAction* action) {
    TGEN_ASSERT(action);
    g_assert(action->data && action->type == TGEN_ACTION_START);
    return ((TGenActionStartData*)action->data)->socksproxy;
}

guint64 tgenaction_getPauseTimeMillis(TGenAction* action) {
    TGEN_ASSERT(action);
    g_assert(action->data && action->type == TGEN_ACTION_PAUSE);
    return 1000 * ((TGenActionPauseData*)action->data)->time;
}

void tgenaction_getTransferParameters(TGenAction* action, TGenTransferType* typeOut,
        TGenTransferProtocol* protocolOut, guint64* sizeOut) {
    TGEN_ASSERT(action);
    g_assert(action->data && action->type == TGEN_ACTION_TRANSFER);

    if(typeOut) {
        *typeOut = ((TGenActionTransferData*)action->data)->type;
    }
    if(protocolOut) {
        *protocolOut = ((TGenActionTransferData*)action->data)->protocol;
    }
    if(sizeOut) {
        *sizeOut = ((TGenActionTransferData*)action->data)->size;
    }
}

TGenPool* tgenaction_getPeers(TGenAction* action) {
    TGEN_ASSERT(action);
    g_assert(action->data);

    if(action->type == TGEN_ACTION_TRANSFER) {
        return ((TGenActionTransferData*)action->data)->peers;
    } else if(action->type == TGEN_ACTION_START) {
        return ((TGenActionStartData*)action->data)->peers;
    } else {
        return NULL;
    }
}

guint64 tgenaction_getEndTimeMillis(TGenAction* action) {
    TGEN_ASSERT(action);
    g_assert(action->data && action->type == TGEN_ACTION_END);
    return 1000 * ((TGenActionEndData*)action->data)->time;
}

guint64 tgenaction_getEndCount(TGenAction* action) {
    TGEN_ASSERT(action);
    g_assert(action->data && action->type == TGEN_ACTION_END);
    return ((TGenActionEndData*)action->data)->count;
}

guint64 tgenaction_getEndSize(TGenAction* action) {
    TGEN_ASSERT(action);
    g_assert(action->data && action->type == TGEN_ACTION_END);
    return ((TGenActionEndData*)action->data)->size;
}
