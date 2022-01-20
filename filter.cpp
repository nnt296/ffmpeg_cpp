/*
 * Copyright (c) 2010 Nicolas George
 * Copyright (c) 2011 Stefano Sabatini
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * API example for decoding and filtering
 * @example filtering_video.c
 */

#include <unistd.h>


extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavutil/file.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
}

#include "opencv2/opencv.hpp"
#include <thread>

//may return 0 when not able to detect
const auto processor_count = (int) std::thread::hardware_concurrency();

const char *filter_descr = "fps=10";
/* other way:
   scale=78:24 [scl]; [scl] transpose=cclock // assumes "[in]" and "[out]" to be input output pads respectively
 */

struct buffer_data {
  uint8_t *ptr;             // Store pointer to current byte
  uint8_t *ori_ptr;         // Store pointer to the first byte
  size_t size;              // Size left in the buffer
  size_t file_size;         // Total size of video buffer
};

static AVFormatContext *fmt_ctx;
static AVCodecContext *dec_ctx;
static AVIOContext *avio_ctx;
uint8_t *buffer, *avio_ctx_buffer;
size_t buffer_size = 4096;
int avio_ctx_buffer_size = 4096;
static buffer_data bufferData{};
AVFilterContext *buffersink_ctx;
AVFilterContext *buffersrc_ctx;
AVFilterGraph *filter_graph;
static int video_stream_index = -1;
long st, en;

int src_w = -1, src_h = -1, dst_w = 320, dst_h = 320;
enum AVPixelFormat src_pix_fmt = AV_PIX_FMT_NONE;
enum AVPixelFormat dst_pix_fmt = AV_PIX_FMT_RGB24;
SwsContext *sws_ctx;
uint8_t *dst_data[4];
int dst_linesize[4];

static int read_packet(void *opaque, uint8_t *buf, int buf_size) {
  auto *bd = (struct buffer_data *) opaque;
  buf_size = FFMIN(buf_size, bd->size);
  if (!buf_size)
    return AVERROR_EOF;
  // std::cout << "ptr: " << bd->ptr << " size: " << bd->size << std::endl;

  // Copy internal buffer data to buf
  memcpy(buf, bd->ptr, buf_size);
  bd->ptr += buf_size;
  bd->size -= buf_size;

  return buf_size;
}

static int64_t seek_in_buffer(void *opaque, int64_t offset, int whence) {
  auto *bd = (buffer_data *) opaque;
  // printf("whence=%d , offset=%lld , file_size=%ld\n", whence, offset, bd->file_size);

  switch (whence) {
    case AVSEEK_SIZE:
      return (int64_t) bd->file_size;
    case SEEK_SET:
      if (bd->file_size > offset) {
        bd->ptr = bd->ori_ptr + offset;
        bd->size = bd->file_size - offset;
      } else
        return AVERROR_EOF;
      break;
    case SEEK_CUR:
      if (bd->file_size > offset) {
        bd->ptr += offset;
        bd->size -= offset;
      } else
        return AVERROR_EOF;
      break;
    case SEEK_END:
      if (bd->file_size > offset) {
        bd->ptr = (bd->ori_ptr + bd->file_size) - offset;
        size_t cur_pos = bd->ptr - bd->ori_ptr;
        bd->size = bd->file_size - cur_pos;
      } else
        return AVERROR_EOF;
      break;
    default:
      /* On error, do nothing, return current position of file. */
      std::cerr << "Could not process buffer seek: " << whence << std::endl;
      break;
  }
  return bd->ptr - bd->ori_ptr;
}

static int open_input_file(const char *filename) {
  AVCodec *dec;
  int ret;

  // Slurp file content into buffer
  if ((ret = av_file_map(filename, &buffer, &buffer_size, 0, nullptr)) < 0) {
    std::cerr << "file map error\n";
    return ret;
  }

  bufferData.ptr = buffer;
  bufferData.ori_ptr = buffer;
  bufferData.size = buffer_size;
  bufferData.file_size = buffer_size;

  if (!fmt_ctx) {
    if (!(fmt_ctx = avformat_alloc_context())) {
      std::cerr << "fmt_ctx alloc error\n";
      ret = AVERROR(ENOMEM);
      return ret;
    }
  }

  if (!avio_ctx_buffer) {
    avio_ctx_buffer = static_cast<uint8_t *>(av_malloc(avio_ctx_buffer_size));
    if (!avio_ctx_buffer) {
      std::cerr << "avio_ctx_buffer alloc error\n";
      ret = AVERROR(ENOMEM);
      return ret;
    }
  }
  avio_ctx = avio_alloc_context(avio_ctx_buffer, avio_ctx_buffer_size,
                                0, &bufferData,
                                &read_packet, nullptr, &seek_in_buffer);
  if (!avio_ctx) {
    std::cerr << "avio_ctx alloc error\n";
    ret = AVERROR(ENOMEM);
    return ret;
  }
  fmt_ctx->pb = avio_ctx;

  if ((ret = avformat_open_input(&fmt_ctx, nullptr, nullptr, nullptr)) < 0) {
    std::cerr << "open input error\n";
    return ret;
  }

  if ((ret = avformat_find_stream_info(fmt_ctx, nullptr)) < 0) {
    std::cerr << "find stream error\n";
    return ret;
  }

  for (int i = 0; i < fmt_ctx->nb_streams; i++) {
    AVCodecParameters *codec_params;
    codec_params = fmt_ctx->streams[i]->codecpar;

    if (codec_params->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream_index = i;
      dec = avcodec_find_decoder(codec_params->codec_id);
      if (!dec_ctx) {
        dec_ctx = avcodec_alloc_context3(dec);
        if (!dec_ctx) {
          std::cerr << "dec_ctx alloc error\n";
          return AVERROR(ENOMEM);
        }
      }

      if ((ret = avcodec_parameters_to_context(dec_ctx, codec_params)) < 0) {
        std::cerr << "Failed to copy codec params to codec context" << std::endl;
        return ret;
      }
      break;
    }
  }
  dec_ctx->thread_count = processor_count;
  dec_ctx->thread_type = FF_THREAD_FRAME;
  src_pix_fmt = dec_ctx->pix_fmt;
  src_w = dec_ctx->width;
  src_h = dec_ctx->height;

  /* init the video decoder */
  if ((ret = avcodec_open2(dec_ctx, dec, nullptr)) < 0) {
    std::cerr << "open2 error\n";
    return ret;
  }

  return 0;
}

static int init_filters(const char *filters_descr) {
  char args[512];
  int ret;
  const AVFilter *buffersrc = avfilter_get_by_name("buffer");
  const AVFilter *buffersink = avfilter_get_by_name("buffersink");
  AVFilterInOut *outputs = avfilter_inout_alloc();
  AVFilterInOut *inputs = avfilter_inout_alloc();
  AVRational time_base = fmt_ctx->streams[video_stream_index]->time_base;
  enum AVPixelFormat pix_fmts[] = {dec_ctx->pix_fmt, AV_PIX_FMT_NONE};

  filter_graph = avfilter_graph_alloc();
  if (!outputs || !inputs || !filter_graph) {
    ret = AVERROR(ENOMEM);
    goto end;
  }

  /* buffer video source: the decoded frames from the decoder will be inserted here. */
  snprintf(args, sizeof(args),
           "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
           dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
           time_base.num, time_base.den,
           dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);

  std::cout << "HERE : " << args << std::endl;

  ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                     args, nullptr, filter_graph);
  if (ret < 0) {
    av_log(nullptr, AV_LOG_ERROR, "Cannot create buffer source\n");
    goto end;
  }

  /* buffer video sink: to terminate the filter chain. */
  ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                     nullptr, nullptr, filter_graph);
  if (ret < 0) {
    av_log(nullptr, AV_LOG_ERROR, "Cannot create buffer sink\n");
    goto end;
  }

  ret = av_opt_set_int_list(buffersink_ctx, "pix_fmts", pix_fmts,
                            AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
  if (ret < 0) {
    av_log(nullptr, AV_LOG_ERROR, "Cannot set output pixel format\n");
    goto end;
  }

  /*
   * Set the endpoints for the filter graph. The filter_graph will
   * be linked to the graph described by filters_descr.
   */

  /*
   * The buffer source output must be connected to the input pad of
   * the first filter described by filters_descr; since the first
   * filter input label is not specified, it is set to "in" by
   * default.
   */
  outputs->name = av_strdup("in");
  outputs->filter_ctx = buffersrc_ctx;
  outputs->pad_idx = 0;
  outputs->next = nullptr;

  /*
   * The buffer sink input must be connected to the output pad of
   * the last filter described by filters_descr; since the last
   * filter output label is not specified, it is set to "out" by
   * default.
   */
  inputs->name = av_strdup("out");
  inputs->filter_ctx = buffersink_ctx;
  inputs->pad_idx = 0;
  inputs->next = nullptr;

  if ((ret = avfilter_graph_parse_ptr(filter_graph, filters_descr,
                                      &inputs, &outputs, nullptr)) < 0)
    goto end;

  if ((ret = avfilter_graph_config(filter_graph, nullptr)) < 0)
    goto end;

  end:
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);

  return ret;
}

static int init_sws_context() {
  int ret;
  if ((ret = av_image_alloc(dst_data, dst_linesize, dst_w, dst_h, dst_pix_fmt, 1)) < 0) {
    ret = AVERROR(ENOMEM);
    return ret;
  }

  // Init sws context
  sws_ctx = sws_getContext(src_w, src_h, src_pix_fmt,
                           dst_w, dst_h, dst_pix_fmt,
                           SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!sws_ctx) {
    ret = AVERROR(EINVAL);
    return ret;
  }

  return ret;
}

int main(int argc, char **argv) {
  st = cv::getTickCount();

  int ret;
  AVPacket *packet;
  AVFrame *frame;
  AVFrame *filt_frame;
  int num_frame = 0;
  std::vector<cv::Mat> v_rgb{};

  if (argc != 2) {
    fprintf(stderr, "Usage: %s file\n", argv[0]);
    exit(1);
  }

  frame = av_frame_alloc();
  filt_frame = av_frame_alloc();
  packet = av_packet_alloc();
  if (!frame || !filt_frame || !packet) {
    fprintf(stderr, "Could not allocate frame or packet\n");
    exit(1);
  }

  if ((ret = open_input_file(argv[1])) < 0)
    goto end;
  if ((ret = init_filters(filter_descr)) < 0)
    goto end;
  if ((ret = init_sws_context()) < 0)
    goto end;

  /* read all packets */
  while (true) {
    if ((ret = av_read_frame(fmt_ctx, packet)) < 0)
      break;

    if (packet->stream_index == video_stream_index) {
      ret = avcodec_send_packet(dec_ctx, packet);
      if (ret < 0) {
        av_log(nullptr, AV_LOG_ERROR,
               "Error while sending a packet to the decoder\n");
        break;
      }

      while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
          break;
        } else if (ret < 0) {
          av_log(nullptr, AV_LOG_ERROR,
                 "Error while receiving a frame from the decoder\n");
          goto end;
        }

        // frame->pts = frame->best_effort_timestamp;

        /* push the decoded frame into the filtergraph */
        if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame,
                                         AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
          av_log(nullptr, AV_LOG_ERROR,
                 "Error while feeding the filtergraph\n");
          break;
        }

        /* pull filtered frames from the filtergraph */
        while (true) {
          ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
          if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
          if (ret < 0)
            goto end;
          sws_scale(sws_ctx,
                    filt_frame->data, filt_frame->linesize, 0,
                    src_h, dst_data, dst_linesize);

          // snprintf(frame_filename, sizeof(frame_filename), "%s-%d.png", "frame", dec_ctx->frame_number);
          // cv::imwrite(frame_filename, im);
          cv::Mat im(dst_h, dst_w, CV_8UC3, dst_data[0], dst_linesize[0]);
          v_rgb.emplace_back(im.clone());
          av_frame_unref(filt_frame);
          num_frame++;
        }
        av_frame_unref(frame);
      }
    }
    av_packet_unref(packet);
  }

  en = cv::getTickCount();
  std::cout << "Elapsed: " << double(en - st) / cv::getTickFrequency() << std::endl;
  std::cout << "Num frames: " << num_frame << std::endl;

  end:
  avfilter_graph_free(&filter_graph);
  avcodec_free_context(&dec_ctx);
  avformat_close_input(&fmt_ctx);
  av_frame_free(&frame);
  av_frame_free(&filt_frame);
  av_packet_free(&packet);
  av_freep(&dst_data[0]);
  sws_freeContext(sws_ctx);

  if (ret < 0 && ret != AVERROR_EOF) {
    char error[AV_ERROR_MAX_STRING_SIZE];
    av_make_error_string(error, AV_ERROR_MAX_STRING_SIZE, ret);
    fprintf(stderr, "Error occurred: %s\n", error);
    exit(1);
  }

  exit(0);
}