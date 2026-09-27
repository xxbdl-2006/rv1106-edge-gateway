#ifndef SINK_FILE_H
#define SINK_FILE_H

#include "packet_sink.h"

/*
 * Reference sink: writes every encoded frame straight to an Annex-B file.
 * Behaviour is byte for byte what the previous inline FILE * code did, which
 * makes it the baseline that any other sink is compared against.
 */
int sink_file_open(const char *path, struct packet_sink **sink);

#endif
