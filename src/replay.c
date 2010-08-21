/* $Id$ */

/*
 * Copyright (c) 2009 Aaron Turner.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright owners nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <unistd.h>

#include "config.h"
#include "defines.h"
#include "common.h"
#include "tcpreplay_api.h"
#include "send_packets.h"

static int replay_file(tcpreplay_t *ctx, int idx);
static int replay_two_files(tcpreplay_t *ctx, int idx1, int idx2);
static int replay_cache(tcpreplay_t *ctx, int idx);
static int replay_two_caches(tcpreplay_t *ctx, int idx1, int idx2);
static int replay_fd(tcpreplay_t *ctx, int idx);
static int replay_two_fds(tcpreplay_t *ctx, int idx1, int idx2);

/**
 * \brief Internal tcpreplay method to replay a given index
 *
 * This is used by tcpreplay_replay() to actually send the packets
 */
int 
tcpr_replay_index(tcpreplay_t *ctx, int idx)
{
    int rcode = 0;
    assert(ctx);

    /* only process a single file */
    if (! ctx->options->dualfile) {
        /* process each pcap file in order */
        for (idx = 0; idx < ctx->options->source_cnt; idx++) {
            /* reset cache markers for each iteration */
            ctx->cache_byte = 0;
            ctx->cache_bit = 0;
            switch(ctx->options->sources[idx].type) {
                case source_filename:
                    rcode = replay_file(ctx, idx);
                    break;
                case source_fd:
                    rcode = replay_fd(ctx, idx);
                    break;
                case source_cache:
                    rcode = replay_cache(ctx, idx);
                    break;
                default:
                    tcpreplay_seterr(ctx, "Invalid source type: %d", ctx->options->sources[idx].type);
                    rcode = -1;
            }
        }
    }

    /* dual file mode: two files, two interfaces */
    else {
        /* process each pcap file in order */
        for (idx = 0; idx < ctx->options->source_cnt; idx += 2) {
            if (ctx->options->sources[idx].type != ctx->options->sources[(idx+1)].type) {
                tcpreplay_seterr(ctx, "Both source indexes (%d, %d) must be of the same type", idx, (idx+1));
                return -1;
            }
            switch(ctx->options->sources[idx].type) {
                case source_filename:
                    rcode = replay_two_files(ctx, idx, (idx+1));
                    break;
                case source_fd:
                    rcode = replay_two_fds(ctx, idx, (idx+1));
                    break;
                case source_cache:
                    rcode = replay_two_caches(ctx, idx, (idx+1));
                    break;
                default:
                    tcpreplay_seterr(ctx, "Invalid source type: %d", ctx->options->sources[idx].type);
                    rcode = -1;
            }

        }

    }
    if (rcode < 0) {
        ctx->running = false;
        return -1;
    }

    return rcode;
}


/**
 * \brief replay a pcap file out interface(s)
 *
 * Internal to tcpreplay.  Does the heavy lifting.
 */
static int
replay_file(tcpreplay_t *ctx, int idx)
{
    char *path;
    pcap_t *pcap = NULL;
    char ebuf[PCAP_ERRBUF_SIZE];
    int dlt;

    assert(ctx);
    assert(ctx->options->sources[idx].type = source_filename);

    path = ctx->options->sources[idx].filename;

    /* close stdin if reading from it (needed for some OS's) */
    if (strncmp(path, "-", 1) == 0)
        close(1);

    /* read from pcap file if we haven't cached things yet */
    if (!(ctx->options->enable_file_cache || ctx->options->preload_pcap)) {
        if ((pcap = pcap_open_offline(path, ebuf)) == NULL) {
            tcpreplay_seterr(ctx, "Error opening pcap file: %s", ebuf);
            return -1;
        }

#ifdef HAVE_PCAP_SNAPSHOT
        if (pcap_snapshot(pcap) < 65535)
            warnx("%s was captured using a snaplen of %d bytes.  This may mean you have truncated packets.",
                    path, pcap_snapshot(pcap));
#endif

    } else {
        if (!ctx->options->file_cache[idx].cached)
            if ((pcap = pcap_open_offline(path, ebuf)) == NULL) {
                tcpreplay_seterr(ctx, "Error opening pcap file: %s", ebuf);
                return -1;
            }
    }

#if 0
/*
 * this API is broken right now.  This needs to be handled via a pipe or 
 * something else so we can pass the output up to the calling programm 
 */
#ifdef ENABLE_VERBOSE
    if (ctx->options->verbose) {
        /* in cache mode, we may not have opened the file */
        if (pcap == NULL)
            if ((pcap = pcap_open_offline(path, ebuf)) == NULL) {
               tcpreplay_seterr("Error opening pcap file: %s", ebuf);
               return -1;
            }

        /* init tcpdump */
        tcpdump_open(ctx->options->tcpdump, pcap);
    }
#endif
#endif

    if (pcap != NULL) {
        dlt = sendpacket_get_dlt(ctx->intf1);
#if 0
        if ((dlt > 0) && (dlt != pcap_datalink(pcap)))
            warnx("%s DLT (%s) does not match that of the outbound interface: %s (%s)",
                    path, pcap_datalink_val_to_name(pcap_datalink(pcap)),
                    ctx->options->intf1->device, pcap_datalink_val_to_name(dlt));
#endif
    }

    ctx->stats.active_pcap = ctx->options->sources[idx].filename;
    send_packets(ctx, pcap, idx);

    if (pcap != NULL)
        pcap_close(pcap);

#if 0
#ifdef ENABLE_VERBOSE
    tcpdump_close(ctx->options->tcpdump);
#endif
#endif
    return 0;
}


/**
 * \brief replay two pcap files out two interfaces
 *
 * Internal to tcpreplay, does the heavy lifting for --dualfile
 */
static int
replay_two_files(tcpreplay_t *ctx, int idx1, int idx2)
{
    char *path1, *path2;
    pcap_t *pcap1  = NULL, *pcap2 = NULL;
    char ebuf[PCAP_ERRBUF_SIZE];
    int dlt1, dlt2, rcode = 0;

    assert(ctx);
    assert(ctx->options->sources[idx1].type = source_filename);
    assert(ctx->options->sources[idx2].type = source_filename);

    path1 = ctx->options->sources[idx1].filename;
    path2 = ctx->options->sources[idx2].filename;


    /* can't use stdin in dualfile mode */
    if ((strncmp(path1, "-", strlen(path1)) == 0) || 
        (strncmp(path2, "-", strlen(path2)) == 0)) {
        tcpreplay_seterr(ctx, "%s", "Invalid use of STDIN '-' in dual file mode");
        return -1;
    }

    /* read from first pcap file if we haven't cached things yet */
    if (! (ctx->options->enable_file_cache || ctx->options->preload_pcap)) {
        if ((pcap1 = pcap_open_offline(path1, ebuf)) == NULL) {
            tcpreplay_seterr(ctx, "Error opening pcap file: %s", ebuf);
            return -1;
        }
        if ((pcap2 = pcap_open_offline(path2, ebuf)) == NULL) {
            tcpreplay_seterr(ctx, "Error opening pcap file: %s", ebuf);
            return -1;
        }
    } else {
        if (!ctx->options->file_cache[idx1].cached) {
            if ((pcap1 = pcap_open_offline(path1, ebuf)) == NULL) {
                tcpreplay_seterr(ctx, "Error opening pcap file: %s", ebuf);
                return -1;
            }
        }
        if (!ctx->options->file_cache[idx2].cached) {
            if ((pcap2 = pcap_open_offline(path2, ebuf)) == NULL) {
                tcpreplay_seterr(ctx, "Error opening pcap file: %s", ebuf);
                return -1;
            }
        }
    }
#ifdef HAVE_PCAP_SNAPSHOT
    if (pcap_snapshot(pcap1) < 65535) {
        tcpreplay_setwarn(ctx, "%s was captured using a snaplen of %d bytes.  This may mean you have truncated packets.",
                path1, pcap_snapshot(pcap1));
        rcode = -2;
    }

    if (pcap_snapshot(pcap2) < 65535) {
        tcpreplay_setwarn(ctx, "%s was captured using a snaplen of %d bytes.  This may mean you have truncated packets.",
                path2, pcap_snapshot(pcap2));
        rcode = -2;
    }

#endif


    if (pcap1 != NULL) {
        dlt1 = sendpacket_get_dlt(ctx->intf1);
        if ((dlt1 > 0) && (dlt1 != pcap_datalink(pcap1))) {
            tcpreplay_setwarn(ctx, "%s DLT (%s) does not match that of the outbound interface: %s (%s)", 
                path1, pcap_datalink_val_to_name(pcap_datalink(pcap1)), 
                ctx->intf1->device, pcap_datalink_val_to_name(dlt1));
            rcode = -2;
        }

        dlt2 = sendpacket_get_dlt(ctx->intf2);
        if ((dlt2 > 0) && (dlt2 != pcap_datalink(pcap2))) {
            tcpreplay_setwarn(ctx, "%s DLT (%s) does not match that of the outbound interface: %s (%s)", 
                path2, pcap_datalink_val_to_name(pcap_datalink(pcap2)), 
                ctx->intf2->device, pcap_datalink_val_to_name(dlt2));
            rcode = -2;
        }

        if (dlt1 != dlt2) {
            tcpreplay_seterr(ctx, "DLT missmatch for %s (%d) and %s (%d)", path1, dlt1, path2, dlt2);
            return -1;
        }
    }

#ifdef ENABLE_VERBOSE
    if (ctx->options->verbose) {

        /* in cache mode, we may not have opened the file */
        if (pcap1 == NULL)
            if ((pcap1 = pcap_open_offline(path1, ebuf)) == NULL) {
                tcpreplay_seterr(ctx, "Error opening pcap file: %s", ebuf);
                return -1;
            }

        /* init tcpdump */
        tcpdump_open(ctx->options->tcpdump, pcap1);
    }
#endif


    send_dual_packets(ctx, pcap1, idx1, pcap2, idx2);

    if (pcap1 != NULL)
        pcap_close(pcap1);

    if (pcap2 != NULL)
        pcap_close(pcap2);

#ifdef ENABLE_VERBOSE
    tcpdump_close(ctx->options->tcpdump);
#endif

    return rcode;
}


/**
 * \brief Replay index using existing memory cache 
 *
 * FIXME
 */
static int
replay_cache(tcpreplay_t *ctx, int idx)
{

    assert(ctx);
    assert(ctx->options->sources[idx].type = source_cache);
    return 0;
}

/**
 * \brief Replay two indexes using existing memory cache 
 *
 * FIXME
 */
static int
replay_two_caches(tcpreplay_t *ctx, int idx1, int idx2)
{

    assert(ctx);
    assert(ctx->options->sources[idx1].type = source_cache);
    assert(ctx->options->sources[idx2].type = source_cache);
    return 0;
}

/**
 * \brief Replay index which is a file descriptor 
 *
 * FIXME
 */
static int
replay_fd(tcpreplay_t *ctx, int idx)
{

    assert(ctx);
    assert(ctx->options->sources[idx].type = source_fd);
    return 0;
}

/**
 * \brief Replay two indexes which are a file descriptor 
 *
 * FIXME
 */
static int
replay_two_fds(tcpreplay_t *ctx, int idx1, int idx2)
{

    assert(ctx);
    assert(ctx->options->sources[idx1].type = source_fd);
    assert(ctx->options->sources[idx2].type = source_fd);
    return 0;
}