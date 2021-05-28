/*
 * rtpenc_rfc4175.c
 *
 *  Created on: Jul 27, 2019
 *      Author: naveenc
 */
#include "avformat.h"
#include "rtpenc.h"
void ff_rtp_send_raw_rfc4175(AVFormatContext *ctx, const uint8_t *frame_buf, int frame_size)
{
    RTPMuxContext *rtp_ctx = ctx->priv_data;
    uint32_t pgroup;
    uint32_t width, height, line, offset, field ;
    uint8_t xinc, yinc;
    uint32_t stride;

    //printf ("width = %u, height = %u and format = %u\n",
       // ctx->streams[0]->codecpar->width, ctx->streams[0]->codecpar->height, ctx->streams[0]->codecpar->format);

    width =  ctx->streams[0]->codecpar->width;
    height =  ctx->streams[0]->codecpar->height;
    //width = height = 224;

    switch (AV_PIX_FMT_BGR24/*ctx->streams[0]->codecpar->format*/) {
      case AV_PIX_FMT_RGB24:
      case AV_PIX_FMT_BGR24:
        xinc = yinc = 1;
        pgroup = 3;
        break;
      case AV_PIX_FMT_NV12:
        pgroup = 6;
        xinc = yinc = 2;
        break;
      default:
        printf ("\nERROR : pixel format = %u not handled\n", ctx->streams[0]->codecpar->format);
        break;
    }

    //width = height = 224;
    //pgroup = 3;
    stride = width * pgroup;
    //xinc = yinc = 1;
    field = 0; // we deal with progressive so field is zero always

    /* use the default 90 KHz time stamp */
    rtp_ctx->timestamp = rtp_ctx->cur_timestamp;
    //printf ("Received frame with timestamp = %u\n", rtp_ctx->cur_timestamp);
    //printf ("packet size = %d, frame size = %d, max_payload_size = %d\n", ctx->packet_size, frame_size, rtp_ctx->max_payload_size);

    line = 0;
    offset = 0;

    /* write all lines */
    while (line < height) {
      uint32_t left = rtp_ctx->max_payload_size;
      uint8_t *outdata, *headers;
      uint8_t next_line, complete = 0;
      uint32_t length, cont, pixels;

      memset (rtp_ctx->buf, 0, ctx->packet_size);
      outdata = rtp_ctx->buf;

      /*
       *   0                   1                   2                   3
       *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
       *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       *  |   Extended Sequence Number    |            Length             |
       *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       *  |F|          Line No            |C|           Offset            |
       *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       *  |            Length             |F|          Line No            |
       *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
       *  |C|           Offset            |                               .
       *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               .
       *  .                                                               .
       *  .                 Two (partial) lines of video data             .
       *  .                                                               .
       *  +---------------------------------------------------------------+
       */

      /* need 2 bytes for the extended sequence number */
      *outdata++ = 0;
      *outdata++ = 0;
      left -= 2;

      /* the headers start here */
      headers = outdata;

      /* make sure we can fit at least *one* header and pixel */
      if (!(left > (6 + pgroup))) {
          printf ("ERROR : buffer is too small\n");
          return;
      }

      /* while we can fit at least one header and one pixel */
      while (left > (6 + pgroup)) {
          /* we need a 6 bytes header */
          left -= 6;

          /* get how may bytes we need for the remaining pixels */
          pixels = width - offset;
          length = (pixels * pgroup) / xinc;

          if (left >= length) {
              /* pixels and header fit completely, we will write them and skip to the
               * next line. */
            next_line = 1;
        } else {
          /* line does not fit completely, see how many pixels fit */
          pixels = (left / pgroup) * xinc;
          length = (pixels * pgroup) / xinc;
          next_line = 0;
        }
        //printf ("filling %u bytes in %u pixels\n", length, pixels);
        left -= length;

        /* write length */
        *outdata++ = (length >> 8) & 0xff;
        *outdata++ = length & 0xff;

        /* write line no */
        *outdata++ = ((line >> 8) & 0x7f) | ((field << 7) & 0x80);
        *outdata++ = line & 0xff;

        if (next_line) {
          /* go to next line we do this here to make the check below easier */
          line += yinc;
        }

        /* calculate continuation marker */
        cont = (left > (6 + pgroup) && line < height) ? 0x80 : 0x00;

        /* write offset and continuation marker */
        *outdata++ = ((offset >> 8) & 0x7f) | cont;
        *outdata++ = offset & 0xff;

        if (next_line) {
          /* reset offset */
          offset = 0;
          //printf ("go to next line %u\n", line);
        } else {
          offset += pixels;
          //printf("next offset %u\n", offset);
        }

        if (!cont)
          break;
      }
      //printf ("consumed %u bytes\n",  (uint32_t) (outdata - headers));

      /* second pass, read headers and write the data */
      while (1) {
        uint32_t offs, lin;

        /* read length and cont */
        length = (headers[0] << 8) | headers[1];
        lin = ((headers[2] & 0x7f) << 8) | headers[3];
        offs = ((headers[4] & 0x7f) << 8) | headers[5];
        cont = headers[4] & 0x80;
        pixels = length / pgroup;
        headers += 6;

        //printf ("writing length %u, line %u, offset %u, cont %d\n", length, lin, offs, cont);

        offs /= xinc;
        memcpy (outdata, frame_buf + (lin * stride) + (offs * pgroup), length);
        outdata += length;

        if (!cont)
          break;
      }

      if (line >= height) {
        //printf("field/frame complete, set marker\n");
        complete = 1;
      }

      //printf ("we have %u bytes left\n",  left);
      ff_rtp_send_data (ctx, rtp_ctx->buf,  rtp_ctx->max_payload_size - left, complete);
    }
}


