/*****************************************************************************
 * lwlibav_dec.h
 *****************************************************************************
 * Copyright (C) 2012-2015 L-SMASH Works project
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *****************************************************************************/
#include <stdio.h>

/* This file is available under an ISC license. */

#ifdef _WIN32
#include <Windows.h>
#include "osdep.h"
#endif // _WIN32

#define SEEK_DTS_BASED      0x00000001
#define SEEK_PTS_BASED      0x00000002
#define SEEK_POS_BASED      0x00000004
#define SEEK_POS_CORRECTION 0x00000008
#define SEEK_PTS_GENERATED  0x00000010

typedef struct
{
    char   *file_path;
    char   *format_name;
    int     format_flags;
    int     raw_demuxer;
    int     threads;
    int64_t av_gap;
} lwlibav_file_handler_t;

typedef struct
{
    uint8_t            *extradata;
    int                 extradata_size;
    /* Codec identifier */
    enum AVCodecID      codec_id;
    unsigned int        codec_tag;
    /* Video */
    int                 width;
    int                 height;
    enum AVPixelFormat  pixel_format;
    /* Audio */
    uint64_t            channel_layout;
    enum AVSampleFormat sample_format;
    int                 sample_rate;
    int                 bits_per_sample;
    int                 block_align;
} lwlibav_extradata_t;

typedef struct
{
    int                  current_index;
    int                  entry_count;
    lwlibav_extradata_t *entries;
    uint32_t             delay_count;
    int (*get_buffer)( struct AVCodecContext *, AVFrame *, int );
} lwlibav_extradata_handler_t;

typedef struct
{
    /* common */
    AVFormatContext            *format;
    int                         stream_index;
    int                         error;
    lw_log_handler_t            lh;
    lwlibav_extradata_handler_t exh;
    AVCodecContext             *ctx;
    AVIndexEntry               *index_entries;
    int                         index_entries_count;
    int                         lw_seek_flags;
    int                         av_seek_flags;
    int                         dv_in_avi;
    enum AVCodecID              codec_id;
    const char                **preferred_decoder_names;
    int                         prefer_hw_decoder;
    AVRational                  time_base;
    uint32_t                    frame_count;
    AVFrame                    *frame_buffer;
    void                       *frame_list;
} lwlibav_decode_handler_t;

static inline int64_t lavf_skip_tc_code
(
    AVFormatContext *ctx,
    int64_t          timestamp // must be byte offset
)
{
    // mpegts read_packet won't skip the TC code field, and if the TC word happens
    // to contain a 0x47 sync byte, read_packet might be confused and lock to an
    // incorrect synchronization point.
    // https://github.com/FFmpeg/FFmpeg/blob/n4.4/libavformat/mpegts.c#L2898
    if (strcmp(ctx->iformat->name, "mpegts") == 0) {
        // However, not all mpegts files are BD, e.g. TV ts does not have such a 4B
        // TP_extra_header.
        //
        // There is no easy way to differentiate between the two (using file extension
        // will be too fragile.) Fortunately, the top 2-bit of TP_extra_header is
        // the copy permission indication, which all sources seem to set to 0, so
        // the first byte of TP_extra_header should not be 0x47, and we can use
        // this to detect these two cases.
        //
        // We do the offset compensation only when the 1st byte is not 0x47 and the
        // 5th byte is 0x47.
        // This test should not affect the performance much as av_seek_frame is going
        // to read at least 188 bytes from position timestamp anyway.
        unsigned char buf[5];
        const char sync_byte = 0x47;
        avio_seek(ctx->pb, timestamp, SEEK_SET);
        avio_read(ctx->pb, buf, sizeof buf);
        if (buf[0] != sync_byte && buf[4] == sync_byte) {
            timestamp += 4; // skip the TC header
            avio_seek(ctx->pb, timestamp, SEEK_SET);
        }
    }
    return timestamp;
}

static inline int lavf_open_file
(
    AVFormatContext **format_ctx,
    const char       *file_path,
    lw_log_handler_t *lhp
)
{
    // The default of 5MB is not sufficient for UHD clips, e.g. https://4kmedia.org/lg-new-york-hdr-uhd-4k-demo/.
    AVDictionary* prob_size = NULL;
    av_dict_set( &prob_size, "probesize", "52428800", 0 );
    if( avformat_open_input( format_ctx, file_path, NULL, &prob_size) )
    {
#ifdef _WIN32
        wchar_t* wname;
        if (lw_string_to_wchar(CP_ACP, file_path, &wname))
        {
            char* name;
            if (lw_string_from_wchar(CP_UTF8, wname, &name))
            {
                lw_free(wname);
                const int open = avformat_open_input(format_ctx, name, NULL, &prob_size);
                lw_free(name);
                if (open)
                    goto fail_open;
            }
            else
            {
                lw_free(wname);
                goto fail_open;
            }
        }
        else
#endif // _WIN32
        goto fail_open;
    }
    lavf_skip_tc_code( *format_ctx, 0 );
    if( avformat_find_stream_info( *format_ctx, NULL ) < 0 )
    {
        lw_log_show( lhp, LW_LOG_FATAL, "Failed to avformat_find_stream_info." );
        return -1;
    }
    av_dict_free( &prob_size );
    return 0;

fail_open:
    lw_log_show(lhp, LW_LOG_FATAL, "Failed to avformat_open_input.");
    return -1;
}

static inline void lavf_close_file( AVFormatContext **format_ctx )
{
    avformat_close_input( format_ctx );
}

static inline int read_av_frame
(
    AVFormatContext *format_ctx,
    AVPacket        *pkt
)
{
    do
    {
        int ret = av_read_frame( format_ctx, pkt );
        /* Don't confuse with EAGAIN with EOF. */
        if( ret != AVERROR( EAGAIN ) )
            return ret;
    } while( 1 );
}

int find_and_open_decoder
(
    AVCodecContext         **ctx,
    const AVCodecParameters *codecpar,
    const char             **preferred_decoder_names,
    const int                prefer_hw_decoder,
    const int                thread_count
);

void lwlibav_flush_buffers
(
    lwlibav_decode_handler_t *dhp
);

int lwlibav_get_av_frame
(
    AVFormatContext *format_ctx,
    int              stream_index,
    uint32_t         frame_number,
    AVPacket        *pkt
);

void lwlibav_update_configuration
(
    lwlibav_decode_handler_t *dhp,
    uint32_t                  frame_number,
    int                       extradata_index,
    int64_t                   rap_pos
);

void set_video_basic_settings
(
    lwlibav_decode_handler_t *dhp,
    const AVCodec            *codec,
    uint32_t                  frame_number
);

void set_audio_basic_settings
(
    lwlibav_decode_handler_t *dhp,
    const AVCodec            *codec,
    uint32_t                  frame_number
);

int try_decode_video_frame
(
    lwlibav_decode_handler_t *dhp,
    uint32_t                  frame_number,
    int64_t                   rap_pos,
    char                     *error_string
);

int try_decode_audio_frame
(
    lwlibav_decode_handler_t *dhp,
    uint32_t                  frame_number,
    char                     *error_string
);
