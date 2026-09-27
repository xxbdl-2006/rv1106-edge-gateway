#ifndef RTSP_PROTO_H
#define RTSP_PROTO_H

#include <stddef.h>
#include <stdint.h>

/*
 * RTSP parsing, SDP generation and response formatting.
 *
 * Deliberately free of any socket dependency: requests arrive as a byte buffer
 * and replies are formatted into a caller owned buffer. That keeps the whole
 * protocol layer unit testable on the host.
 */

#define RTSP_PROTO_METHOD_MAX 16U
#define RTSP_PROTO_PATH_MAX 256U
#define RTSP_PROTO_VALUE_MAX 256U
#define RTSP_PROTO_SESSION_MAX 32U

struct rtsp_request {
    char method[RTSP_PROTO_METHOD_MAX];
    char path[RTSP_PROTO_PATH_MAX];
    long cseq;
    int has_cseq;
    /*
     * RTSP session identifiers are opaque strings: the server picks the text
     * and the client echoes it verbatim. Always compare session_token, never
     * the numeric value, otherwise a hexadecimal id cannot round trip.
     */
    char session_token[RTSP_PROTO_SESSION_MAX];
    long session;
    int has_session;
    char transport[RTSP_PROTO_VALUE_MAX];
    int has_transport;
    long content_length;
};

/*
 * Parse one RTSP request from <buffer>.
 *
 * Returns the number of bytes consumed, 0 when more data is needed, -1 when
 * the input is malformed. The caller must drop the consumed bytes.
 */
int rtsp_proto_parse(const char *buffer,
                     size_t length,
                     struct rtsp_request *request);

/* Returns bytes written to <out> excluding the terminating NUL, or 0 on overflow. */
size_t rtsp_proto_base64(const uint8_t *input,
                         size_t input_length,
                         char *output,
                         size_t output_size);

/*
 * Build the SDP body for a single H.264 track. <sps>/<pps> may be NULL when the
 * parameter sets are not known yet; the corresponding fmtp field is then left
 * out and the decoder falls back to the in band sets.
 */
int rtsp_proto_sdp(char *output,
                   size_t size,
                   const char *stream_path,
                   const uint8_t *sps,
                   size_t sps_length,
                   const uint8_t *pps,
                   size_t pps_length);

/*
 * Build a full RTSP reply. <session> and <extra_headers> may be NULL.
 * Returns the reply length, or -1 if it would not fit.
 */
int rtsp_proto_response(char *output,
                        size_t size,
                        int code,
                        const char *reason,
                        long cseq,
                        const char *session,
                        const char *extra_headers,
                        const char *body);

#endif
