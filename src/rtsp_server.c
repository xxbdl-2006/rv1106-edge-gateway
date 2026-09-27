#include "rtsp_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "h264_util.h"
#include "rtp_h264.h"
#include "rtsp_proto.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/*
 * How long a session may stay completely silent before we drop it.
 *
 * This must be comfortably above the keep-alive interval of common clients:
 * live555 (VLC) sends its keep-alive roughly every 60 seconds, so a 60 second
 * limit here killed VLC sessions at almost exactly one minute while ffplay and
 * ffmpeg, which keep alive more often, kept running.
 *
 * Receiving video also counts as activity, see the reader thread.
 */
#define RTSP_IDLE_TIMEOUT_SECONDS 300

/* Advertised to the client so it keeps alive well inside our real limit. */
#define RTSP_ADVERTISED_TIMEOUT_SECONDS 60

/* Pace of the client request poll. Must not be zero, see client_thread_main. */
#define RTSP_REQUEST_POLL_MS 20

#define RTSP_REQUEST_LIMIT 2048
#define RTSP_TRACK_SUFFIX "/trackID=0"

/* Why a client connection ended. */
#define SOCKET_OK 0
#define SOCKET_CLOSED 1 /* peer sent a FIN or TEARDOWN; its own decision */
#define SOCKET_ERROR 2
#define SOCKET_HUP 3

/*
 * Multiple viewers share one encoded stream, so the RTP sequence number and
 * timestamp are properties of the stream and not of a connection: every client
 * receives the very same packets. That is only true if the frame is packetized
 * once and then fanned out, which is what the reader thread below does. Letting
 * each client thread pull from the packet queue instead would split the frames
 * between them and every viewer would see corruption.
 */

struct rtsp_client {
    struct rtsp_server *server;
    pthread_t thread;
    int socket;

    char session[24];
    unsigned session_number;

    /* All of the mutable state below is guarded by server->mutex. */
    int playing;
    int joining; /* waiting for the next IDR before the first packet */
    int broken;  /* a send failed, stop feeding this client */
    int running; /* cleared by the client thread as it exits */
    unsigned long frames_sent;

    int stop;
    time_t last_activity;
    char request[RTSP_REQUEST_LIMIT];
    size_t request_length;
};

struct rtsp_server {
    struct packet_queue *queue;
    struct rtp_h264 rtp;
    struct rtsp_server_config config;

    /*
     * Guards the RTP state and everything marked as guarded above. Never held
     * across a blocking receive: the client threads may only take it for short
     * bookkeeping and for writing the RTSP reply.
     */
    pthread_mutex_t mutex;

    pthread_t listener;
    pthread_t reader;
    int listener_running;
    int reader_running;

    int socket;
    int stop_flag;
    uint32_t next_session;

    struct rtsp_client *clients[RTSP_MAX_CLIENTS];
    size_t client_count;

    unsigned long frames_packetized;
    unsigned long empty_frames;
};

/* ---------------------------------------------------------------- helpers */

static void lock_server(struct rtsp_server *server)
{
    (void)pthread_mutex_lock(&server->mutex);
}

static void unlock_server(struct rtsp_server *server)
{
    (void)pthread_mutex_unlock(&server->mutex);
}

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags != -1) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

/*
 * Disable Nagle on the streaming connection.
 *
 * One video frame is written as several small RTP packets. With Nagle enabled
 * those writes get held back waiting for an ACK, and the interaction with
 * delayed ACKs can add tens of milliseconds to every frame. This is one of the
 * classic latency traps for RTP over TCP and it costs nothing to avoid.
 */
static void set_no_delay(int fd)
{
    int enabled = 1;

    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                     (unsigned int)sizeof(enabled));
}

static void set_receive_timeout(int fd, int seconds)
{
    struct timeval timeout;

    timeout.tv_sec = (time_t)seconds;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

static int send_all(int fd, const void *data, size_t length)
{
    const uint8_t *cursor = data;
    size_t sent = 0U;

    while (sent < length) {
        ssize_t result = send(fd, cursor + sent, length - sent, MSG_NOSIGNAL);

        if (result < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd wait_fd;

                wait_fd.fd = fd;
                wait_fd.events = POLLOUT;
                wait_fd.revents = 0;

                if (poll(&wait_fd, 1, 200) > 0) {
                    continue;
                }
                return -1;
            }
            return -1;
        }

        if (result == 0) {
            return -1;
        }

        sent += (size_t)result;
    }

    return 0;
}

/*
 * Strip scheme, host and port off an RTSP URL, leaving the absolute path.
 * rtsp://172.32.0.93:8554/live/0  ->  /live/0
 */
static void extract_uri_path(const char *uri, char *out, size_t size)
{
    const char *slash;

    if (uri == NULL || size == 0U) {
        return;
    }

    if (strncmp(uri, "rtsp://", 7U) == 0) {
        slash = strchr(uri + 7U, '/');
    } else if (uri[0] == '/') {
        slash = uri;
    } else {
        slash = NULL;
    }

    if (slash == NULL) {
        snprintf(out, size, "%s", uri);
        return;
    }

    snprintf(out, size, "%s", slash);
}

/*
 * Normalise an RTSP URL to the stream base: drop a trailing slash and a
 * trailing "/trackID=0".
 *
 * Clients are inconsistent here: VLC sends PLAY as ".../live/0/" while other
 * clients use ".../live/0" or ".../live/0/trackID=0". Building anything on top
 * of the raw path produced doubled slashes such as ".../live/0//trackID=0" in
 * RTP-Info, which live555 has to match against the track URL.
 */
static void build_base_url(const char *uri, char *out, size_t size)
{
    size_t length;
    size_t suffix_length;

    if (uri == NULL || size == 0U) {
        return;
    }

    snprintf(out, size, "%s", uri);
    length = strlen(out);
    suffix_length = strlen(RTSP_TRACK_SUFFIX);

    while (length > 0U && out[length - 1U] == '/') {
        out[--length] = '\0';
    }

    if (length >= suffix_length &&
        strcmp(out + length - suffix_length, RTSP_TRACK_SUFFIX) == 0) {
        out[length - suffix_length] = '\0';
    }
}

/*
 * Accept anything that still names the configured stream.
 *
 * Clients do not agree on how to turn "a=control" into a SETUP URL: some use
 * the bare path, some append a trailing slash, some concatenate the control
 * value onto Content-Base producing a doubled path. Being permissive here costs
 * nothing on a single stream server and avoids a whole class of 404s.
 */
static int path_matches(const char *requested, const char *base)
{
    if (base == NULL || base[0] == '\0') {
        return 1;
    }

    return strstr(requested, base) != NULL;
}

/* -------------------------------------------------------------- streaming */

/* One frame, already serialized, on its way to every viewer. */
struct fanout {
    struct rtsp_client *targets[RTSP_MAX_CLIENTS];
    size_t count;
};

/*
 * Both this and client_send_response write to the same socket, and they run on
 * different threads: the reader pushes video while the client thread answers
 * keep-alives. Interleaved TCP framing is not self synchronising, so two
 * concurrent writers corrupt the stream and the client's parser loses its place
 * (it then answers what it thinks is a request, and the conversation degrades
 * into nonsense). Every write therefore goes through the server lock.
 *
 * The whole packet is built first and sent in one call, which also removes the
 * window between the interleaved header and its payload.
 */
static int send_rtp_packet(struct rtsp_client *client,
                           const void *packet,
                           size_t length)
{
    uint8_t framed[4U + RTP_H264_MAX_PACKET];

    if (client->socket < 0 || length > RTP_H264_MAX_PACKET ||
        length > 0xFFFFU) {
        return -1;
    }

    framed[0] = '$';
    framed[1] = 0U;
    framed[2] = (uint8_t)((length >> 8) & 0xFFU);
    framed[3] = (uint8_t)(length & 0xFFU);
    memcpy(framed + 4U, packet, length);

    return send_all(client->socket, framed, 4U + length);
}

/*
 * Called once per RTP packet, for all viewers at once.
 *
 * A failing client must never abort the frame for the others, so failures are
 * only recorded here; the caller drops that client afterwards.
 */
static int fanout_emit(void *user, const void *packet, size_t length)
{
    struct fanout *fanout = user;
    size_t index;

    for (index = 0U; index < fanout->count; index++) {
        struct rtsp_client *client = fanout->targets[index];

        if (client->broken != 0) {
            continue;
        }

        if (send_rtp_packet(client, packet, length) != 0) {
            fprintf(stderr, "RTSP: session %s stalled, dropping it\n",
                    client->session);
            client->broken = 1;
        }
    }

    return 0;
}

static void *reader_thread_main(void *argument)
{
    struct rtsp_server *server = argument;

    while (server->stop_flag == 0) {
        const struct packet_queue_slot *slot = NULL;
        struct fanout fanout;
        size_t index;
        int result;
        int packets;

        result = packet_queue_acquire(server->queue, &slot, 100);
        if (result == PACKET_QUEUE_STOPPED) {
            break;
        }
        if (result != PACKET_QUEUE_OK) {
            continue;
        }

        lock_server(server);

        fanout.count = 0U;
        for (index = 0U; index < RTSP_MAX_CLIENTS; index++) {
            struct rtsp_client *client = server->clients[index];

            if (client == NULL || client->running == 0 ||
                client->playing == 0) {
                continue;
            }

            /*
             * A viewer that just pressed PLAY starts at the next IDR. Frames
             * before it would reference pictures it never received.
             */
            if (client->joining != 0) {
                if ((slot->flags & H264_FLAG_KEY) == 0U) {
                    continue;
                }
                client->joining = 0;
            }

            client->broken = 0;
            fanout.targets[fanout.count] = client;
            fanout.count++;
        }

        /*
         * Packetize even with no viewer: it keeps the SPS and PPS cache warm so
         * a client that attaches later gets them in the SDP.
         */
        packets = rtp_h264_packetize(&server->rtp, slot->data, slot->length,
                                     fanout_emit, &fanout);

        for (index = 0U; index < fanout.count; index++) {
            struct rtsp_client *client = fanout.targets[index];

            if (client->broken != 0) {
                /* Wake the request loop so it can tear the session down. */
                (void)shutdown(client->socket, SHUT_RDWR);
                continue;
            }

            client->frames_sent++;
            /* Receiving video is activity too, otherwise a healthy viewer
             * that simply never sends a request looks idle to us. */
            client->last_activity = time(NULL);
        }

        if (packets == 0) {
            if (server->empty_frames == 0UL) {
                fprintf(stderr,
                        "RTSP: warning: no NAL units in a %zu byte frame, "
                        "first bytes %02X %02X %02X %02X\n",
                        slot->length,
                        slot->length > 0U ? slot->data[0] : 0U,
                        slot->length > 1U ? slot->data[1] : 0U,
                        slot->length > 2U ? slot->data[2] : 0U,
                        slot->length > 3U ? slot->data[3] : 0U);
            }
            server->empty_frames++;
        } else {
            server->frames_packetized++;
        }

        unlock_server(server);
        packet_queue_release(server->queue);
    }

    return NULL;
}

/* ------------------------------------------------------------ request loop */

/*
 * Session handling is central here on purpose.
 *
 * RFC 2326 requires the Session header on every reply that belongs to an
 * established session, including keep-alives. Handlers used to add it by hand
 * and the ones for OPTIONS, error replies and friends did not, so live555
 * (VLC) never got a confirmation for its OPTIONS keep-alive, retried three
 * times at its 60 second interval and then dropped the session at about three
 * minutes. ffplay and ffmpeg are more forgiving about the missing header, which
 * is why only VLC was affected.
 */
static int client_send_response(struct rtsp_client *client,
                                const struct rtsp_request *request,
                                int code,
                                const char *reason,
                                const char *extra,
                                const char *body)
{
    char buffer[2048];
    char session_value[48];
    const char *session_header = NULL;
    int length;
    int result;

    if (client->session_number != 0U &&
        (request->has_session != 0 ||
         strcmp(request->method, "SETUP") == 0)) {
        snprintf(session_value, sizeof(session_value), "%s;timeout=%d",
                 client->session, RTSP_ADVERTISED_TIMEOUT_SECONDS);
        session_header = session_value;
    }

    if (code >= 300) {
        fprintf(stderr, "RTSP: reply %d %s to %s\n", code, reason,
                request->method);
    }

    length = rtsp_proto_response(buffer, sizeof(buffer), code, reason,
                                 request->cseq, session_header, extra, body);
    if (length < 0) {
        return -1;
    }

    /*
     * Serialised against the reader thread, which is writing RTP to the same
     * socket. Without this the two writers interleave and the client's RTSP
     * parser desynchronises. See send_rtp_packet.
     *
     * No handler may hold the lock when it calls this, or the lock would be
     * taken twice.
     */
    lock_server(client->server);
    result = send_all(client->socket, buffer, (size_t)length);
    unlock_server(client->server);

    return result;
}

static int handle_options(struct rtsp_client *client,
                          const struct rtsp_request *request)
{
    return client_send_response(
        client, request, 200, "OK",
        "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, "
        "GET_PARAMETER\r\n",
        NULL);
}

static int handle_describe(struct rtsp_client *client,
                           const struct rtsp_request *request)
{
    struct rtsp_server *server = client->server;
    char sdp[1024];
    char base_url[RTSP_PROTO_PATH_MAX];
    char control_url[RTSP_PROTO_PATH_MAX + 32U];
    char extra[640];
    uint8_t sps[RTP_H264_MAX_SPS_BYTES];
    uint8_t pps[RTP_H264_MAX_PPS_BYTES];
    size_t sps_length;
    size_t pps_length;
    int have_parameters;
    int length;

    /*
     * Derive the track URL from the URI the client actually used. That
     * authority is reachable from the client by definition, which is not
     * something we could guess from the local side. Emitting an absolute
     * control URL also removes any ambiguity about how a relative one would be
     * resolved against Content-Base.
     */
    build_base_url(request->path, base_url, sizeof(base_url));

    snprintf(control_url, sizeof(control_url), "%s%s", base_url,
             RTSP_TRACK_SUFFIX);

    /* Copy the parameter sets under the lock, then format without it. */
    lock_server(server);
    have_parameters = server->rtp.parameters_cached != 0 ? 1 : 0;
    sps_length = server->rtp.sps_length;
    pps_length = server->rtp.pps_length;
    if (have_parameters != 0) {
        memcpy(sps, server->rtp.sps, sizeof(sps));
        memcpy(pps, server->rtp.pps, sizeof(pps));
    }
    unlock_server(server);

    length = rtsp_proto_sdp(sdp, sizeof(sdp), control_url,
                            have_parameters != 0 ? sps : NULL,
                            sps_length,
                            have_parameters != 0 ? pps : NULL,
                            pps_length);
    if (length < 0) {
        return -1;
    }

    fprintf(stderr, "RTSP: SDP control=%s parameter_sets=%s\n", control_url,
            have_parameters != 0 ? "yes" : "no");

    snprintf(extra, sizeof(extra),
             "Content-Base: %s/\r\n"
             "Content-Type: application/sdp\r\n",
             base_url);

    return client_send_response(client, request, 200, "OK", extra, sdp);
}

static int handle_setup(struct rtsp_client *client,
                        const struct rtsp_request *request)
{
    struct rtsp_server *server = client->server;
    int supported;
    unsigned ssrc;
    char extra[512];

    if (!request->has_transport) {
        return client_send_response(client, request, 400, "Bad Request", NULL,
                                    NULL);
    }

    /* Only interleaved TCP is implemented, matching the RTSP over TCP goal. */
    supported = (strstr(request->transport, "TCP") != NULL) &&
                (strstr(request->transport, "interleaved") != NULL);

    if (!supported) {
        return client_send_response(client, request, 461,
                                    "Unsupported Transport", NULL, NULL);
    }

    lock_server(server);
    ssrc = server->rtp.ssrc;
    unlock_server(server);

    snprintf(extra, sizeof(extra),
             "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;ssrc=%08X\r\n",
             ssrc);

    return client_send_response(client, request, 200, "OK", extra, NULL);
}

static int handle_play(struct rtsp_client *client,
                       const struct rtsp_request *request)
{
    struct rtsp_server *server = client->server;
    char extra[512];
    char base_url[RTSP_PROTO_PATH_MAX];
    unsigned sequence;
    unsigned timestamp;
    int result;

    lock_server(server);
    sequence = server->rtp.sequence;
    timestamp = server->rtp.timestamp;
    unlock_server(server);

    /* Same normalisation as DESCRIBE, otherwise the URL grows a double slash. */
    build_base_url(request->path, base_url, sizeof(base_url));

    snprintf(extra, sizeof(extra),
             "Range: npt=0.000-\r\n"
             "RTP-Info: url=%s%s;seq=%u;rtptime=%u\r\n",
             base_url, RTSP_TRACK_SUFFIX, sequence, timestamp);

    result = client_send_response(client, request, 200, "OK", extra, NULL);
    if (result != 0) {
        return result;
    }

    /*
     * Start from the next IDR so this viewer never sees a broken GOP. The RTP
     * timeline is deliberately NOT reset: it belongs to the shared stream, and
     * rewinding it would disturb whoever is already watching.
     */
    lock_server(server);
    client->joining = 1;
    client->playing = 1;
    unlock_server(server);

    return 0;
}

static int handle_pause(struct rtsp_client *client,
                        const struct rtsp_request *request)
{
    lock_server(client->server);
    client->playing = 0;
    unlock_server(client->server);

    return client_send_response(client, request, 200, "OK", NULL, NULL);
}

static int handle_get_parameter(struct rtsp_client *client,
                                const struct rtsp_request *request)
{
    return client_send_response(client, request, 200, "OK", NULL, NULL);
}

static int dispatch_request(struct rtsp_client *client)
{
    struct rtsp_request request;
    struct rtsp_server *server = client->server;
    char requested_path[RTSP_PROTO_PATH_MAX];
    int consumed;

    consumed = rtsp_proto_parse(client->request, client->request_length,
                                &request);
    if (consumed == 0) {
        return 0;
    }
    if (consumed < 0) {
        return -1;
    }

    client->last_activity = time(NULL);

    fprintf(stderr, "RTSP: %s %s\n", request.method, request.path);

    memmove(client->request, client->request + consumed,
            client->request_length - (size_t)consumed);
    client->request_length -= (size_t)consumed;
    client->request[client->request_length] = '\0';

    extract_uri_path(request.path, requested_path, sizeof(requested_path));

    if (!path_matches(requested_path, server->config.path)) {
        fprintf(stderr, "RTSP: rejecting path %s\n", requested_path);
        return client_send_response(client, &request, 404, "Not Found", NULL,
                                    NULL);
    }

    if (!request.has_cseq) {
        return -1;
    }

    /*
     * Everything after SETUP must belong to this session. Session ids are
     * opaque text, so compare the token the client echoed back with the string
     * we handed out. Rejecting unknown sessions keeps a stale client from
     * disturbing an active one.
     */
    if (request.has_session != 0 &&
        strcmp(request.session_token, client->session) != 0) {
        fprintf(stderr, "RTSP: session mismatch, got '%s', expected '%s'\n",
                request.session_token, client->session);
        return client_send_response(client, &request, 454, "Session Not Found",
                                    NULL, NULL);
    }

    if (strcmp(request.method, "OPTIONS") == 0) {
        return handle_options(client, &request);
    }
    if (strcmp(request.method, "DESCRIBE") == 0) {
        return handle_describe(client, &request);
    }
    if (strcmp(request.method, "SETUP") == 0) {
        return handle_setup(client, &request);
    }
    if (strcmp(request.method, "PLAY") == 0) {
        return handle_play(client, &request);
    }
    if (strcmp(request.method, "PAUSE") == 0) {
        return handle_pause(client, &request);
    }
    if (strcmp(request.method, "GET_PARAMETER") == 0) {
        return handle_get_parameter(client, &request);
    }
    if (strcmp(request.method, "TEARDOWN") == 0) {
        (void)client_send_response(client, &request, 200, "OK", NULL, NULL);
        return -1;
    }

    return client_send_response(client, &request, 501, "Not Implemented", NULL,
                                NULL);
}

/*
 * Read whatever is pending on the RTSP connection and answer every complete
 * request.
 *
 * Returns SOCKET_OK, or a SOCKET_* code describing why the connection is gone.
 * The reason matters: "the client hung up" and "we dropped the client" look
 * identical from the outside but need completely different fixes.
 */
static int service_socket(struct rtsp_client *client, int timeout_ms)
{
    struct pollfd wait_fd;
    int result;

    wait_fd.fd = client->socket;
    wait_fd.events = POLLIN;
    wait_fd.revents = 0;

    result = poll(&wait_fd, 1, timeout_ms);
    if (result < 0) {
        return errno == EINTR ? SOCKET_OK : SOCKET_ERROR;
    }
    if (result == 0) {
        return SOCKET_OK;
    }

    if ((wait_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return SOCKET_HUP;
    }
    if ((wait_fd.revents & POLLIN) == 0) {
        return 0;
    }

    for (;;) {
        ssize_t received;
        size_t space = sizeof(client->request) - 1U - client->request_length;

        if (space == 0U) {
            client->request_length = 0U;
            return SOCKET_ERROR;
        }

        received = recv(client->socket,
                        client->request + client->request_length,
                        space, 0);

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return SOCKET_ERROR;
        }
        if (received == 0) {
            return SOCKET_CLOSED;
        }

        client->request_length += (size_t)received;
        client->request[client->request_length] = '\0';

        if (strstr(client->request, "\r\n\r\n") == NULL) {
            break;
        }

        for (;;) {
            if (dispatch_request(client) != 0) {
                return SOCKET_CLOSED;
            }
            if (client->request_length == 0U) {
                break;
            }
            if (strstr(client->request, "\r\n\r\n") == NULL) {
                break;
            }
        }

        break;
    }

    return SOCKET_OK;
}

static void *client_thread_main(void *argument)
{
    struct rtsp_client *client = argument;
    struct rtsp_server *server = client->server;

    /*
     * This thread only owns the request/response side of the connection. Video
     * is pushed by the reader thread, so nothing here may touch the queue.
     */
    while (client->stop == 0 && server->stop_flag == 0) {
        time_t now;
        time_t last;

        /*
         * Poll at a fixed pace. The reader thread pushes the video, so this
         * loop only has to notice keep-alives and TEARDOWN in reasonable time.
         * A zero timeout here used to spin this thread at 100% CPU for as long
         * as the client was playing, on a board with a single core.
         */
        const char *reason;
        int status;

        status = service_socket(client, RTSP_REQUEST_POLL_MS);

        if (status == SOCKET_CLOSED) {
            reason = "the client closed it";
        } else if (status == SOCKET_HUP) {
            reason = "the connection hung up";
        } else if (status == SOCKET_ERROR) {
            reason = "a socket error";
        } else {
            reason = NULL;
        }

        if (reason != NULL) {
            fprintf(stderr, "RTSP: session %s ended because %s\n",
                    client->session, reason);
            break;
        }

        lock_server(server);
        now = time(NULL);
        last = client->last_activity;
        unlock_server(server);

        if (difftime(now, last) > (double)RTSP_IDLE_TIMEOUT_SECONDS) {
            fprintf(stderr, "RTSP: session %s timed out\n", client->session);
            break;
        }
    }

    fprintf(stderr, "RTSP: session %s closed after %lu frames\n",
            client->session, client->frames_sent);

    /* Unpublish first: the reader must stop touching the socket. */
    lock_server(server);
    client->running = 0;
    client->playing = 0;
    unlock_server(server);

    (void)shutdown(client->socket, SHUT_RDWR);
    (void)close(client->socket);
    client->socket = -1;

    return NULL;
}

/* --------------------------------------------------------------- listener */

/*
 * Join and free finished sessions.
 *
 * The join happens outside the lock: a client thread takes the lock on its way
 * out, so holding it here would deadlock. The pointer stays published until
 * the thread is gone, which is safe because running == 0 makes the reader skip
 * it.
 */
static void reap_clients(struct rtsp_server *server)
{
    for (;;) {
        struct rtsp_client *dead = NULL;
        size_t index;
        size_t slot = 0U;

        lock_server(server);
        for (index = 0U; index < RTSP_MAX_CLIENTS; index++) {
            struct rtsp_client *client = server->clients[index];

            if (client != NULL && client->running == 0) {
                dead = client;
                slot = index;
                break;
            }
        }
        unlock_server(server);

        if (dead == NULL) {
            return;
        }

        (void)pthread_join(dead->thread, NULL);

        lock_server(server);
        server->clients[slot] = NULL;
        server->client_count--;
        unlock_server(server);

        free(dead);
    }
}

static void *listener_thread_main(void *argument)
{
    struct rtsp_server *server = argument;

    while (server->stop_flag == 0) {
        struct pollfd wait_fd;
        int client_fd;
        size_t slot;
        int free_slot = -1;

        wait_fd.fd = server->socket;
        wait_fd.events = POLLIN;
        wait_fd.revents = 0;

        if (poll(&wait_fd, 1, 100) <= 0) {
            reap_clients(server);
            continue;
        }

        reap_clients(server);

        if ((wait_fd.revents & POLLIN) == 0) {
            continue;
        }

        client_fd = accept(server->socket, NULL, NULL);
        if (client_fd < 0) {
            continue;
        }

        lock_server(server);
        for (slot = 0U; slot < RTSP_MAX_CLIENTS; slot++) {
            if (server->clients[slot] == NULL) {
                free_slot = (int)slot;
                break;
            }
        }
        unlock_server(server);

        if (free_slot < 0) {
            const char busy[] = "RTSP/1.0 453 Not Enough Bandwidth\r\n"
                                "CSeq: 0\r\n\r\n";

            fprintf(stderr, "RTSP: rejecting client, all %d slots busy\n",
                    RTSP_MAX_CLIENTS);
            (void)send_all(client_fd, busy, sizeof(busy) - 1U);
            (void)close(client_fd);
            continue;
        }

        {
            struct rtsp_client *client = calloc(1U, sizeof(*client));

            if (client == NULL) {
                (void)close(client_fd);
                continue;
            }

            client->server = server;
            client->socket = client_fd;
            client->running = 1;
            client->playing = 0;
            client->joining = 1;
            client->last_activity = time(NULL);

            lock_server(server);
            client->session_number = server->next_session++;
            unlock_server(server);

            snprintf(client->session, sizeof(client->session), "%08X",
                     client->session_number);

            set_nonblocking(client_fd);
            set_no_delay(client_fd);
            set_receive_timeout(client_fd, 2);

            if (pthread_create(&client->thread, NULL, client_thread_main,
                               client) != 0) {
                fprintf(stderr, "RTSP: failed to start client thread\n");
                (void)close(client_fd);
                free(client);
                continue;
            }

            lock_server(server);
            server->clients[(size_t)free_slot] = client;
            server->client_count++;
            unlock_server(server);

            fprintf(stderr,
                    "RTSP: client connected, session %s (slot %d, %zu active)\n",
                    client->session, free_slot, server->client_count);
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------- api */

int rtsp_server_start(struct packet_queue *queue,
                      const struct rtsp_server_config *config,
                      struct rtsp_server **server)
{
    struct rtsp_server *created;
    struct sockaddr_in address;
    int reuse = 1;
    int fd;

    if (queue == NULL || config == NULL || server == NULL) {
        return -1;
    }
    *server = NULL;

    /* A client disappearing mid write must not kill the whole process. */
    (void)signal(SIGPIPE, SIG_IGN);

    created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        perror("rtsp_server_start: calloc");
        return -1;
    }

    created->queue = queue;
    created->socket = -1;
    created->config = *config;
    created->next_session = (uint32_t)(time(NULL) & 0x00FFFFFFU) + 1U;

    if (created->config.path == NULL || created->config.path[0] == '\0') {
        created->config.path = "/live/0";
    }
    if (created->config.port == 0U) {
        created->config.port = 8554U;
    }
    if (created->config.fps == 0U) {
        created->config.fps = 30U;
    }

    rtp_h264_init(&created->rtp, created->config.fps, 0U);

    if (pthread_mutex_init(&created->mutex, NULL) != 0) {
        fprintf(stderr, "rtsp_server_start: mutex init failed\n");
        free(created);
        return -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("rtsp_server_start: socket");
        (void)pthread_mutex_destroy(&created->mutex);
        free(created);
        return -1;
    }

    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(created->config.port);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("rtsp_server_start: bind");
        (void)close(fd);
        (void)pthread_mutex_destroy(&created->mutex);
        free(created);
        return -1;
    }

    if (listen(fd, RTSP_MAX_CLIENTS) != 0) {
        perror("rtsp_server_start: listen");
        (void)close(fd);
        (void)pthread_mutex_destroy(&created->mutex);
        free(created);
        return -1;
    }

    created->socket = fd;

    if (pthread_create(&created->reader, NULL, reader_thread_main,
                       created) != 0) {
        fprintf(stderr, "rtsp_server_start: reader thread failed\n");
        (void)close(fd);
        (void)pthread_mutex_destroy(&created->mutex);
        free(created);
        return -1;
    }
    created->reader_running = 1;

    if (pthread_create(&created->listener, NULL, listener_thread_main,
                       created) != 0) {
        fprintf(stderr, "rtsp_server_start: listener thread failed\n");
        created->stop_flag = 1;
        packet_queue_stop(queue);
        (void)pthread_join(created->reader, NULL);
        (void)close(fd);
        (void)pthread_mutex_destroy(&created->mutex);
        free(created);
        return -1;
    }
    created->listener_running = 1;

    *server = created;

    fprintf(stderr, "RTSP listening on port %u, path %s, up to %d clients\n",
            (unsigned)created->config.port, created->config.path,
            RTSP_MAX_CLIENTS);

    return 0;
}

void rtsp_server_stop(struct rtsp_server *server)
{
    size_t index;

    if (server == NULL) {
        return;
    }

    server->stop_flag = 1;
    (void)shutdown(server->socket, SHUT_RDWR);

    if (server->listener_running != 0) {
        (void)pthread_join(server->listener, NULL);
        server->listener_running = 0;
    }

    /* Wake every session so the joins below cannot hang. */
    lock_server(server);
    for (index = 0U; index < RTSP_MAX_CLIENTS; index++) {
        struct rtsp_client *client = server->clients[index];

        if (client != NULL) {
            client->stop = 1;
            client->playing = 0;
            if (client->socket >= 0) {
                (void)shutdown(client->socket, SHUT_RDWR);
            }
        }
    }
    unlock_server(server);

    reap_clients(server);

    if (server->reader_running != 0) {
        (void)pthread_join(server->reader, NULL);
        server->reader_running = 0;
    }

    if (server->socket >= 0) {
        (void)close(server->socket);
        server->socket = -1;
    }

    fprintf(stderr, "RTSP: packetized %lu frames, %lu empty\n",
            server->frames_packetized, server->empty_frames);

    (void)pthread_mutex_destroy(&server->mutex);
    free(server);
}
