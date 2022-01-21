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

#define AVIO_CONTEXT_BUFFER_SIZE 4096

// May return 0 when not able to detect
// If detected 0, then ffmpeg will try to detect the appropriate number
const auto processor_count = (int) std::thread::hardware_concurrency();

struct BufferData {
  uint8_t *ptr;
  uint8_t *ori_ptr;
  size_t size;
  size_t file_size;
};

static int read_packet(void *opaque, uint8_t *buf, int buf_size) {
  auto *bd = (BufferData *) opaque;
  buf_size = FFMIN(buf_size, bd->size);
  if (!buf_size)
    return AVERROR_EOF;

  // Copy internal buffer data to buf
  memcpy(buf, bd->ptr, buf_size);
  bd->ptr += buf_size;
  bd->size -= buf_size;

  return buf_size;
}

static int64_t seek_buffer(void *opaque, int64_t offset, int whence) {
  auto *bd = (BufferData *) opaque;

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

// Setup global variable for convenience
AVFormatContext *fmt_ctx;
AVCodecContext *codec_ctx;
AVIOContext *avio_ctx;
uint8_t *avio_ctx_buffer;
AVFilterContext *buffersrc_ctx;
AVFilterContext *buffersink_ctx;
AVFilterGraph *filter_graph;
SwsContext *sws_ctx;
uint8_t *dst_data[4];
int dst_linesize[4];
AVPixelFormat dst_pix_fmt = AV_PIX_FMT_RGB24;

AVPacket *packet;
AVFrame *frame;
AVFrame *filtered_frame;

// Keep track of video-stream-index
int video_stream_index = 0;

// This function return non-negative video_stream_index if success
static int open_buffer(BufferData &bd) {
  AVCodec *codec;
  int ret;

  fmt_ctx = avformat_alloc_context();
  if (!fmt_ctx) {
    std::cerr << "Cannot allocate memory for av format context" << std::endl;
    return AVERROR(ENOMEM);
  }

  avio_ctx_buffer = static_cast<uint8_t *>(av_malloc(AVIO_CONTEXT_BUFFER_SIZE));
  if (!avio_ctx_buffer) {
    std::cerr << "Cannot allocate memory for avio context buffer" << std::endl;
    return AVERROR(ENOMEM);
  }

  avio_ctx = avio_alloc_context(avio_ctx_buffer, AVIO_CONTEXT_BUFFER_SIZE,
                                0, &bd,
                                &read_packet, nullptr, &seek_buffer);
  if (!avio_ctx) {
    std::cerr << "Cannot allocate memory for avio context" << std::endl;
    return AVERROR(ENOMEM);
  }

  fmt_ctx->pb = avio_ctx;

  if ((ret = avformat_open_input(&fmt_ctx, nullptr, nullptr, nullptr)) < 0) {
    std::cerr << "Cannot opencv avformat" << std::endl;
    return ret;
  }

  if ((ret = avformat_find_stream_info(fmt_ctx, nullptr)) < 0) {
    std::cerr << "Cannot find stream information" << std::endl;
    return ret;
  }

  for (int i = 0; i < fmt_ctx->nb_streams; i++) {
    AVCodecParameters *codec_params = fmt_ctx->streams[i]->codecpar;

    if (codec_params->codec_type == AVMEDIA_TYPE_VIDEO) {
      codec = avcodec_find_decoder(codec_params->codec_id);
      codec_ctx = avcodec_alloc_context3(codec);
      if (!codec_ctx) {
        std::cerr << "Cannot allocate memory for codec context" << std::endl;
        return AVERROR(ENOMEM);
      }

      if ((ret = avcodec_parameters_to_context(codec_ctx, codec_params)) < 0) {
        std::cerr << "Failed to copy codec params to codec context" << std::endl;
        return ret;
      }

      video_stream_index = i;
      break;
    }
  }

  // Setup threading
  codec_ctx->thread_count = processor_count;
  codec_ctx->thread_type = FF_THREAD_FRAME;

  // Init the video decoder
  if ((ret = avcodec_open2(codec_ctx, codec, nullptr)) < 0) {
    std::cerr << "Cannot open decoder" << std::endl;
    return ret;
  }

  return ret;
}

static int init_filter(const char *filter_descriptor) {
  char args[512];
  int ret;
  const AVFilter *buffersrc = avfilter_get_by_name("buffer");
  const AVFilter *buffersink = avfilter_get_by_name("buffersink");
  AVFilterInOut *outputs = avfilter_inout_alloc();
  AVFilterInOut *inputs = avfilter_inout_alloc();
  AVRational time_base = fmt_ctx->streams[video_stream_index]->time_base;
  enum AVPixelFormat pix_fmts[] = {codec_ctx->pix_fmt, AV_PIX_FMT_NONE};

  filter_graph = avfilter_graph_alloc();
  if (!outputs || !inputs || !filter_graph) {
    ret = AVERROR(ENOMEM);
    goto end;
  }

  /* buffer video source: the decoded frames from the decoder will be inserted here. */
  snprintf(args, sizeof(args),
           "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
           codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
           time_base.num, time_base.den,
           codec_ctx->sample_aspect_ratio.num, codec_ctx->sample_aspect_ratio.den);

  if ((ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                          args, nullptr, filter_graph)) < 0) {
    std::cerr << "Cannot create buffer source" << std::endl;
    goto end;
  }

  if ((ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                          nullptr, nullptr, filter_graph)) < 0) {
    std::cerr << "Cannot create buffer sink" << std::endl;
    goto end;
  }

  if ((ret = av_opt_set_int_list(buffersink_ctx, "pix_fmts", pix_fmts,
                                 AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0) {
    std::cerr << "Cannot set output pixel format" << std::endl;
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

  if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_descriptor,
                                      &inputs, &outputs, nullptr)) < 0) {
    std::cerr << "Cannot parse filter graph" << std::endl;
    goto end;
  }

  if ((ret = avfilter_graph_config(filter_graph, nullptr)) < 0) {
    std::cerr << "Cannot config filter graph" << std::endl;
    goto end;
  }

  end:
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);

  return ret;
}

static int init_sws_context(int dst_w, int dst_h) {
  int ret;
  if ((ret = av_image_alloc(dst_data, dst_linesize, dst_w, dst_h, dst_pix_fmt, 1)) < 0) {
    return AVERROR(ENOMEM);
  }

  // Init sws context
  sws_ctx = sws_getContext(codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
                           dst_w, dst_h, dst_pix_fmt, SWS_BILINEAR,
                           nullptr, nullptr, nullptr);
  if (!sws_ctx) {
    return AVERROR(EINVAL);
  }

  return ret;
}

static std::vector<cv::Mat> decode_data_buffer(BufferData &bd,
                                               float fps = 10,
                                               int dst_w = 320,
                                               int dst_h = 320,
                                               int start_second = 0,
                                               int end_second = 0) {
  int ret;

  char filter_descriptor[128];

  if (end_second > 0)
    snprintf(filter_descriptor, sizeof(filter_descriptor),
             "trim=end=%d:start=%d,fps=%.1f",
             end_second, start_second, fps);
  else
    snprintf(filter_descriptor, sizeof(filter_descriptor), "fps=%.1f", fps);

  std::vector<cv::Mat> v_rgb{};
  int num_frame = 0;

  frame = av_frame_alloc();
  filtered_frame = av_frame_alloc();
  packet = av_packet_alloc();

  if (!frame || !filtered_frame || !packet) {
    ret = AVERROR(ENOMEM);
    goto end;
  }

  if ((ret = open_buffer(bd)) < 0)
    goto end;

  if ((ret = init_filter(filter_descriptor)) < 0)
    goto end;

  if ((ret = init_sws_context(dst_w, dst_h)) < 0)
    goto end;

  // Read all packets
  while (true) {
    if ((ret = av_read_frame(fmt_ctx, packet)) < 0)
      break;

    if (packet->stream_index == video_stream_index) {
      if ((ret = avcodec_send_packet(codec_ctx, packet)) < 0) {
        std::cerr << "Error sending packet to the decoder" << std::endl;
        goto end;
      }

      while (ret >= 0) {
        ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
          break;
        else if (ret < 0) {
          std::cerr << "Error while receiving a frame from the decoder" << std::endl;
          goto end;
        }
        // Got a frame
        // Push the decoded frame into the filtergraph
        if (av_buffersrc_add_frame_flags(
            buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
          std::cerr << "Error while feeding the filtergraph" << std::endl;
          break;
        }

        // Pull filtered frames from the filtergraph
        while (true) {
          ret = av_buffersink_get_frame(buffersink_ctx, filtered_frame);
          if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
          if (ret < 0) {
            std::cerr << "Error while pulling the filtergraph" << std::endl;
            goto end;
          }

          // Convert pixel format & scale image
          sws_scale(sws_ctx, filtered_frame->data, filtered_frame->linesize, 0,
                    codec_ctx->height, dst_data, dst_linesize);

          // Create cv::Mat
          cv::Mat im(dst_h, dst_w, CV_8UC3, dst_data[0], dst_linesize[0]);
          v_rgb.emplace_back(im.clone());   // clone to avoid memory messed up
          num_frame++;
          av_frame_unref(filtered_frame);
        }
        av_frame_unref(frame);
      }
    }
    av_packet_unref(packet);
  }

  end:
  // Free ffmpeg context
  avfilter_graph_free(&filter_graph);
  avcodec_free_context(&codec_ctx);
  avformat_close_input(&fmt_ctx);
  if (avio_ctx)
    av_freep(&avio_ctx->buffer);
  avio_context_free(&avio_ctx);
  // FFmpeg example does not free buffersrc_ctx and buffersink_ctx

  // free frame, packet, sws' stuffs
  av_frame_free(&frame);
  av_frame_free(&filtered_frame);
  av_packet_free(&packet);
  av_freep(&dst_data[0]);
  sws_freeContext(sws_ctx);

  if (ret < 0 && ret != AVERROR_EOF) {
    char error[AV_ERROR_MAX_STRING_SIZE];
    av_make_error_string(error, AV_ERROR_MAX_STRING_SIZE, ret);
    std::cerr << "Error occurred: " << error << std::endl;
    return {};
  }

  return v_rgb;
}

std::vector<cv::Mat> decode_from_file(const std::string &file_name,
                                      float fps = 10,
                                      int dst_w = 320,
                                      int dst_h = 320,
                                      int start_second = 0,
                                      int end_second = 0) {
  uint8_t *buffer;
  size_t buffer_size;

  if (av_file_map(file_name.c_str(), &buffer, &buffer_size, 0, nullptr) < 0) {
    std::cerr << "file map error\n";
    return {};
  }

  BufferData bd{};
  bd.ptr = buffer;
  bd.ori_ptr = buffer;
  bd.size = buffer_size;
  bd.file_size = buffer_size;

  auto v_rgb = decode_data_buffer(bd, fps,
                                  dst_w, dst_h,
                                  start_second, end_second);

  av_file_unmap(buffer, buffer_size);
  return v_rgb;
}

int main(int argc, char **argv) {
  for (int n = 0; n < 20; n++) {
    auto begin = cv::getTickCount();

    std::string file_name{argv[1]};
    auto v_rgb = decode_from_file(file_name, 10, 160, 160, 0, 0);

    std::cout << "Num decoded frames: " << v_rgb.size() << std::endl;

    auto end = cv::getTickCount();

    std::cout << "Elapsed: " << double(end - begin) / cv::getTickFrequency() << std::endl;
  }
}