/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "save.h"
#include "config/config.h"
#include "encoder/encoder.h"
#include "error.h"
#include "path.h"
#include "pktcircle.h"
#include "record.h"
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/bprint.h>
#include <libavutil/opt.h>
#include <time.h>

static struct {
   /**
    * Used for creating a copy of the currently encoded packets for saving.
    */
   RSPacketCircle pktCircle;
} priv;

void rsSaveInit(void) {
   rsPacketCircleCreate(&priv.pktCircle);
}

void rsSaveExit(void) {
   rsPacketCircleDestroy(&priv.pktCircle);
}

void rsSave(void) {
   AVBPrint outputFile;
   av_bprint_init(&outputFile, 0, AV_BPRINT_SIZE_AUTOMATIC);
   rsPathJoin(&outputFile, rsConfig.outputFile, RS_PATH_STRFTIME);
   av_log(NULL, AV_LOG_INFO, "Saving as '%s'...\n", outputFile.str);

   AVFormatContext* formatCtx;
   // Forcing it to be mp4 makes life alot easier
   rsCheck(avformat_alloc_output_context2(&formatCtx, NULL, "mp4", outputFile.str));
   // `faststart` does a second pass of the encoding that puts the `MOOV` atom at the
   // start. Does not take long and highly recommended for sharing since the video plays
   // faster without downloading the whole file.
   rsCheck(av_opt_set(formatCtx, "movflags", "+faststart", AV_OPT_SEARCH_CHILDREN));
   rsCheck(avio_open(&formatCtx->pb, outputFile.str, AVIO_FLAG_WRITE));

   const RSEncoder* encoder = rsRecordVideo();
   AVStream* stream = avformat_new_stream(formatCtx, encoder->codecCtx->codec);
   // Share the parameters from the encoder.
   avcodec_parameters_from_context(stream->codecpar, encoder->codecCtx);
   av_dump_format(formatCtx, 0, outputFile.str, 1);

   rsCheck(avformat_write_header(formatCtx, NULL));
   rsPacketCircleCopy(&priv.pktCircle, &encoder->pktCircle);

   // Due to the encoder constantly running, the packets are very likely not starting at
   // 0. So we get the timestamps of the first packet and shift all of them by that. If
   // the value is `AV_NOPTS_VALUE`, the rest are likely the same so we should not modify
   // them.
   int64_t ptsOffset = priv.pktCircle.packets[0].pts;
   if (ptsOffset == AV_NOPTS_VALUE) ptsOffset = 0;
   int64_t dtsOffset = priv.pktCircle.packets[0].dts;
   if (dtsOffset == AV_NOPTS_VALUE) dtsOffset = 0;

   for (size_t i = 0; i < priv.pktCircle.tail; ++i) {
      AVPacket* packet = &priv.pktCircle.packets[i];
      packet->pts -= ptsOffset;
      packet->dts -= dtsOffset;

      // Despite copying the timebase from the encoder, the muxer may decide to choose a
      // better timebase.
      av_packet_rescale_ts(packet, encoder->codecCtx->time_base, stream->time_base);
      rsCheck(av_write_frame(formatCtx, packet));
   }
   rsPacketCircleClear(&priv.pktCircle);
   rsCheck(av_write_trailer(formatCtx));

   avio_closep(&formatCtx->pb);
   avformat_free_context(formatCtx);
   av_log(NULL, AV_LOG_INFO, "Successfully saved!\n");
   av_bprint_finalize(&outputFile, NULL);
}
