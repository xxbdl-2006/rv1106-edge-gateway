#ifndef SINK_QUEUE_H
#define SINK_QUEUE_H

#include "packet_queue.h"
#include "packet_sink.h"

#include <stdbool.h>

/*
 * Bridge from the encoder into a packet queue.
 *
 * Today the only consumer is the file drain below, which exists so the queue
 * can be validated in the real pipeline against the file sink. When the RTSP
 * thread lands, the drain is replaced by the network consumer and this sink
 * stays exactly as it is.
 */
int sink_queue_open(struct packet_queue *queue, struct packet_sink **sink);

struct queue_file_drain;

int queue_file_drain_start(struct packet_queue *queue,
                           const char *path,
                           struct queue_file_drain **drain);

void queue_file_drain_stop(struct queue_file_drain *drain);

#endif
