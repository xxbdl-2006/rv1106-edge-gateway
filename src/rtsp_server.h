#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <stdint.h>

#include "packet_queue.h"

/* How many simultaneous viewers the server accepts. */
#define RTSP_MAX_CLIENTS 4

/*
 * Minimal RTSP server for one H.264 track, for up to RTSP_MAX_CLIENTS viewers.
 *
 * Transport is RTP over RTSP (TCP interleaved) only. That matches the handoff
 * requirement of "RTSP over TCP" and avoids NAT and packet loss problems on the
 * RNDIS link.
 *
 * Thread model:
 *   listener thread - accepts connections, reaps finished sessions
 *   reader thread   - the single consumer of the packet queue. It packetizes
 *                     each frame ONCE and fans the resulting RTP packets out to
 *                     every active viewer
 *   client threads  - one per connection, request/response only
 *
 * Why a single reader: the RTP sequence number and timestamp describe the
 * stream, not a connection. If each client pulled from the queue on its own,
 * frames would be split between them and every viewer would see corruption.
 *
 * The reader keeps consuming even when nobody is watching. That keeps the
 * encoder unblocked, makes a late viewer start on live frames instead of a
 * backlog, and keeps the SPS/PPS cache warm so DESCRIBE can advertise them.
 */

struct rtsp_server_config {
    uint16_t port;
    const char *path;
    uint32_t fps;
    uint32_t width;
    uint32_t height;
};

struct rtsp_server;

int rtsp_server_start(struct packet_queue *queue,
                      const struct rtsp_server_config *config,
                      struct rtsp_server **server);

/* Stops the listener, disconnects the client and joins every thread. */
void rtsp_server_stop(struct rtsp_server *server);

#endif
