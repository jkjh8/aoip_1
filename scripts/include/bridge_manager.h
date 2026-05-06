#pragma once
#include "alsa_device.h"
#include "rtp_stream.h"

void bridge_start    (Device    *d);
void bridge_stop     (Device    *d);
int  rtp_stream_open (RtpStream *r, int is_out);
void rtp_stream_close(RtpStream *r);
