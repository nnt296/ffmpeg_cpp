#include "video_reader.h"

// May return 0 when not able to detect
// If detected 0, then ffmpeg will try to detect the appropriate number
const auto processor_count = (int) std::thread::hardware_concurrency();

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

InMemVideoReader::InMemVideoReader(double fps,
                                   int dst_w, int dst_h,
                                   int start_second, int end_second) {
  this->fps = fps;
  this->dst_w = dst_w;
  this->dst_h = dst_h;
  this->start_second = start_second;
  this->end_second = end_second;
}


InMemVideoReader::InMemVideoReader(int max_size, int video_max_length, int max_num_frames) {
  this->video_max_length = video_max_length;
  this->max_num_frames = max_num_frames;
  this->max_size = max_size;
}

InMemVideoReader::~InMemVideoReader() = default;

// This function return non-negative video_stream_index if success
int InMemVideoReader::open_buffer(BufferData &buffer_data) {
  const AVCodec *codec;
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
                                0, &buffer_data,
                                &read_packet, nullptr, &seek_buffer);
  if (!avio_ctx) {
    std::cerr << "Cannot allocate memory for avio context" << std::endl;
    return AVERROR(ENOMEM);
  }

  fmt_ctx->pb = avio_ctx;

  if ((ret = avformat_open_input(&fmt_ctx, nullptr, nullptr, nullptr)) < 0) {
    std::cerr << "Cannot open avformat" << std::endl;
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

int InMemVideoReader::init_filter(const char *filter_descriptor) {
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

int InMemVideoReader::init_sws_context() {
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

std::vector<cv::Mat> InMemVideoReader::decode_data_buffer(BufferData &buffer_data) {
  int ret;
  char filter_descriptor[128];      // FFmpeg filter string

  std::vector<cv::Mat> v_rgb{};
  int num_frame = 0;

  frame = av_frame_alloc();
  filtered_frame = av_frame_alloc();
  packet = av_packet_alloc();

  if (!frame || !filtered_frame || !packet) {
    ret = AVERROR(ENOMEM);
    goto end;
  }

  if ((ret = open_buffer(buffer_data)) < 0)
    goto end;

  init_output_params();

  // Setup output params
  if (end_second > 0)
    snprintf(filter_descriptor, sizeof(filter_descriptor),
             "trim=end=%d:start=%d,fps=%.1f",
             end_second, start_second, fps);
  else
    snprintf(filter_descriptor, sizeof(filter_descriptor), "fps=%.1f", fps);

  if ((ret = init_filter(filter_descriptor)) < 0)
    goto end;

  if ((ret = init_sws_context()) < 0)
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
  free_resources();

  if (ret < 0 && ret != AVERROR_EOF) {
    char error[AV_ERROR_MAX_STRING_SIZE];
    av_make_error_string(error, AV_ERROR_MAX_STRING_SIZE, ret);
    std::cerr << "Error occurred: " << error << std::endl;
    return {};
  }

  return v_rgb;
}

void InMemVideoReader::free_resources() {
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
}

std::vector<cv::Mat> InMemVideoReader::decode_from_file
    (const std::string &file_name, BufferData &buffer_data) {
  uint8_t *buffer;
  size_t buffer_size;

  if (av_file_map(file_name.c_str(), &buffer, &buffer_size, 0, nullptr) < 0) {
    std::cerr << "file map error\n";
    return {};
  }

  buffer_data.ptr = buffer;
  buffer_data.ori_ptr = buffer;
  buffer_data.size = buffer_size;
  buffer_data.file_size = buffer_size;

  auto v_rgb = decode_data_buffer(buffer_data);

  av_file_unmap(buffer, buffer_size);
  free_bd(buffer_data);

  return v_rgb;
}

void InMemVideoReader::init_output_params() {
  if (!fmt_ctx or !codec_ctx) {
    std::cout << "[WARNING] Init context before setting output params" << std::endl;
    return;
  }


  // Config output info
  // video max length
  double duration = double(fmt_ctx->duration) / 1e6;
  if (duration > this->video_max_length and this->video_max_length > 0)
    duration = this->video_max_length;
  this->end_second = (int) duration;

  // fps
  auto raw_fps = av_q2d(this->fmt_ctx->streams[video_stream_index]->r_frame_rate);
  if (double(duration) * raw_fps > this->max_num_frames and this->max_num_frames > 0)
    this->fps = double(this->max_num_frames) / double(duration);
  else
    this->fps = raw_fps;

  // Output size
  int max_sz = std::max(this->codec_ctx->width, this->codec_ctx->height);
  if (max_sz > this->max_size) {
    auto ratio = double(this->max_size) / max_sz;
    this->dst_w = int(std::floor(double(this->codec_ctx->width) * ratio / 16) * 16);
    this->dst_h = int(std::floor(double(this->codec_ctx->height) * ratio / 16) * 16);
  } else {
    this->dst_w = this->codec_ctx->width;
    this->dst_h = this->codec_ctx->height;
  }

  std::cout << "[INFO] Output parameters: " << std::endl;
  std::cout << "\t\tFPS: " << this->fps << std::endl;
  std::cout << "\t\tStart Seconds: " << this->start_second << std::endl;
  std::cout << "\t\tEnd Seconds: " << this->end_second << std::endl;
  std::cout << "\t\tWidth: " << this->dst_w << std::endl;
  std::cout << "\t\tHeight: " << this->dst_h << std::endl;
}
