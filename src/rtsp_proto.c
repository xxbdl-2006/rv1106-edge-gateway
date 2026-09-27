#include "rtsp_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtp_h264.h"

#define BASE64_ALPHABET \
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

static int lower_char(int value)
{
    if (value >= 'A' && value <= 'Z') {
        return value - 'A' + 'a';
    }

    return value;
}

static int starts_with(const char *text, size_t length, const char *prefix)
{
    size_t prefix_length = strlen(prefix);

    if (length < prefix_length) {
        return 0;
    }

    for (size_t index = 0U; index < prefix_length; index++) {
        if (lower_char((int)text[index]) != lower_char((int)prefix[index])) {
            return 0;
        }
    }

    return 1;
}

static void extract_field(const char *buffer,
                          size_t length,
                          const char *name,
                          char *output,
                          size_t output_size,
                          int *found)
{
    size_t index;
    size_t name_length = strlen(name);

    for (index = 0U; index + name_length < length; index++) {
        size_t start;
        size_t value_length;

        if (!starts_with(buffer + index, length - index, name)) {
            continue;
        }
        if (buffer[index + name_length] != ':') {
            continue;
        }

        start = index + name_length + 1U;
        value_length = 0U;

        while (start + value_length < length &&
               buffer[start + value_length] != '\r' &&
               buffer[start + value_length] != '\n') {
            value_length++;
        }

        while (value_length > 0U && buffer[start] == ' ') {
            start++;
            value_length--;
        }

        while (value_length > 0U &&
               (buffer[start + value_length - 1U] == ' ' ||
                buffer[start + value_length - 1U] == '\t')) {
            value_length--;
        }

        if (value_length >= output_size) {
            value_length = output_size - 1U;
        }
        memcpy(output, buffer + start, value_length);
        output[value_length] = '\0';
        *found = 1;
        return;
    }
}

static long parse_long(const char *text, long fallback)
{
    char *end = NULL;
    long value;

    if (text == NULL || text[0] == '\0') {
        return fallback;
    }

    value = strtol(text, &end, 10);
    if (end == text) {
        return fallback;
    }

    return value;
}

int rtsp_proto_parse(const char *buffer,
                     size_t length,
                     struct rtsp_request *request)
{
    size_t body_start;
    size_t header_length;
    size_t index;
    size_t cursor;
    char value[RTSP_PROTO_VALUE_MAX];
    int found;

    if (buffer == NULL || request == NULL || length < 4U) {
        return 0;
    }

    memset(request, 0, sizeof(*request));

    header_length = 0U;
    for (index = 0U; index + 3U < length; index++) {
        if (buffer[index] == '\r' && buffer[index + 1U] == '\n' &&
            buffer[index + 2U] == '\r' && buffer[index + 3U] == '\n') {
            header_length = index;
            break;
        }
    }

    if (header_length == 0U) {
        return 0;
    }

    /* Request line: METHOD SP URI SP VERSION */
    cursor = 0U;
    index = 0U;
    while (cursor < header_length && buffer[cursor] != ' ' &&
           buffer[cursor] != '\t' && index + 1U < RTSP_PROTO_METHOD_MAX) {
        request->method[index++] = buffer[cursor++];
    }
    request->method[index] = '\0';

    while (cursor < header_length &&
           (buffer[cursor] == ' ' || buffer[cursor] == '\t')) {
        cursor++;
    }

    index = 0U;
    while (cursor < header_length && buffer[cursor] != ' ' &&
           buffer[cursor] != '\t' && index + 1U < RTSP_PROTO_PATH_MAX) {
        request->path[index++] = buffer[cursor++];
    }
    request->path[index] = '\0';

    if (request->method[0] == '\0' || request->path[0] == '\0') {
        return -1;
    }

    request->cseq = 0;
    request->has_cseq = 0;
    request->session = 0;
    request->has_session = 0;
    request->session_token[0] = '\0';
    request->has_transport = 0;
    request->content_length = 0;

    found = 0;
    extract_field(buffer, header_length, "CSeq", value, sizeof(value), &found);
    if (found) {
        request->cseq = parse_long(value, 0L);
        request->has_cseq = 1;
    }

    found = 0;
    extract_field(buffer, header_length, "Session", value, sizeof(value),
                  &found);
    if (found) {
        /* Session headers may carry ";timeout=60". */
        char *separator = strchr(value, ';');

        if (separator != NULL) {
            *separator = '\0';
        }
        snprintf(request->session_token, sizeof(request->session_token), "%s",
                 value);
        request->session = parse_long(value, -1L);
        request->has_session = 1;
    }

    found = 0;
    extract_field(buffer, header_length, "Transport", request->transport,
                  sizeof(request->transport), &found);
    request->has_transport = found;

    found = 0;
    extract_field(buffer, header_length, "Content-Length", value,
                  sizeof(value), &found);
    if (found) {
        request->content_length = parse_long(value, 0L);
    }

    if (request->content_length < 0L) {
        return -1;
    }

    body_start = header_length + 4U;
    if (request->content_length > 0L) {
        size_t needed = body_start + (size_t)request->content_length;

        if (length < needed) {
            return 0;
        }
        return (int)needed;
    }

    return (int)body_start;
}

size_t rtsp_proto_base64(const uint8_t *input,
                         size_t input_length,
                         char *output,
                         size_t output_size)
{
    size_t written = 0U;
    size_t index;

    if (input == NULL || output == NULL || output_size == 0U) {
        return 0U;
    }

    for (index = 0U; index < input_length; index += 3U) {
        uint32_t block = 0U;
        size_t remaining = input_length - index;
        size_t part;

        if (remaining > 3U) {
            remaining = 3U;
        }

        for (part = 0U; part < 3U; part++) {
            block = (block << 8) | (part < remaining
                                        ? (uint32_t)input[index + part]
                                        : 0U);
        }

        for (part = 0U; part < 4U; part++) {
            int octet;

            if (written + 1U >= output_size) {
                output[written] = '\0';
                return 0U;
            }

            if (part <= remaining) {
                octet = (int)((block >> (18 - 6 * (int)part)) & 0x3FU);
                output[written++] = BASE64_ALPHABET[octet];
            } else {
                output[written++] = '=';
            }
        }
    }

    output[written] = '\0';
    return written;
}

int rtsp_proto_sdp(char *output,
                   size_t size,
                   const char *stream_path,
                   const uint8_t *sps,
                   size_t sps_length,
                   const uint8_t *pps,
                   size_t pps_length)
{
    int written;
    char encoded_sps[160];
    char encoded_pps[160];

    if (output == NULL || size == 0U || stream_path == NULL) {
        return -1;
    }

    written = snprintf(output, size,
                       "v=0\r\n"
                       "o=- 0 0 IN IP4 0.0.0.0\r\n"
                       "s=RV1106 Edge Gateway\r\n"
                       "c=IN IP4 0.0.0.0\r\n"
                       "t=0 0\r\n"
                       "a=range:npt=0-\r\n"
                       "m=video 0 RTP/AVP %u\r\n"
                       "a=rtpmap:%u H264/%u\r\n",
                       RTP_H264_PAYLOAD_TYPE,
                       RTP_H264_PAYLOAD_TYPE,
                       RTP_H264_CLOCK_RATE);

    if (written < 0 || (size_t)written >= size) {
        return -1;
    }

    if (sps != NULL && sps_length >= 4U && pps != NULL && pps_length > 0U) {
        char fmtp[512];
        size_t encoded_sps_length;
        size_t encoded_pps_length;

        encoded_sps_length = rtsp_proto_base64(sps, sps_length,
                                               encoded_sps,
                                               sizeof(encoded_sps));
        encoded_pps_length = rtsp_proto_base64(pps, pps_length,
                                               encoded_pps,
                                               sizeof(encoded_pps));

        if (encoded_sps_length > 0U && encoded_pps_length > 0U) {
            int added;

            added = snprintf(fmtp, sizeof(fmtp),
                             "a=fmtp:%u packetization-mode=1;"
                             "profile-level-id=%02X%02X%02X;"
                             "sprop-parameter-sets=%s,%s\r\n",
                             RTP_H264_PAYLOAD_TYPE,
                             sps[1], sps[2], sps[3],
                             encoded_sps, encoded_pps);

            if (added > 0 && (size_t)written + (size_t)added < size) {
                memcpy(output + written, fmtp, (size_t)added);
                written += added;
                output[written] = '\0';
            }
        }
    } else {
        int added;

        added = snprintf(output + written, size - (size_t)written,
                         "a=fmtp:%u packetization-mode=1\r\n",
                         RTP_H264_PAYLOAD_TYPE);
        if (added > 0 && (size_t)written + (size_t)added < size) {
            written += added;
            output[written] = '\0';
        }
    }

    {
        int added;
        size_t used = (size_t)written;

        added = snprintf(output + used, size - used,
                         "a=control:%s\r\n", stream_path);
        if (added < 0 || (size_t)written + (size_t)added >= size) {
            return -1;
        }
        written += added;
    }

    return written;
}

int rtsp_proto_response(char *output,
                        size_t size,
                        int code,
                        const char *reason,
                        long cseq,
                        const char *session,
                        const char *extra_headers,
                        const char *body)
{
    size_t written = 0U;
    size_t body_length;
    int added;
    size_t remaining;

    if (output == NULL || size == 0U || reason == NULL) {
        return -1;
    }

    body_length = (body != NULL) ? strlen(body) : 0U;

    added = snprintf(output, size, "RTSP/1.0 %d %s\r\nCSeq: %ld\r\n",
                     code, reason, cseq);
    if (added < 0 || (size_t)added >= size) {
        return -1;
    }
    written = (size_t)added;

    if (session != NULL) {
        written = 0U;
        added = snprintf(output, size,
                         "RTSP/1.0 %d %s\r\nCSeq: %ld\r\nSession: %s\r\n",
                         code, reason, cseq, session);
        if (added < 0 || (size_t)added >= size) {
            return -1;
        }
        written = (size_t)added;
    }

    if (extra_headers != NULL && extra_headers[0] != '\0') {
        remaining = size - written;
        added = snprintf(output + written, remaining, "%s", extra_headers);
        if (added < 0 || (size_t)added >= remaining) {
            return -1;
        }
        written += (size_t)added;
    }

    remaining = size - written;
    added = snprintf(output + written, remaining,
                     "Content-Length: %zu\r\n\r\n", body_length);
    if (added < 0 || (size_t)added >= remaining) {
        return -1;
    }
    written += (size_t)added;

    if (body_length > 0U) {
        remaining = size - written;
        if (body_length >= remaining) {
            return -1;
        }
        memcpy(output + written, body, body_length);
        written += body_length;
    }

    output[written] = '\0';
    return (int)written;
}
