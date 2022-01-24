#ifndef AVCPPSAMPLES_VIDEO_READER_H
#define AVCPPSAMPLES_VIDEO_READER_H

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
#include <unistd.h>
#include "curl/curl.h"
#include "data_types.h"
#include "curl_reader.h"

class ReadVideoBuffer {
public:
  // Setup global variable for convenience

  // Video context variables
  AVFormatContext *fmt_ctx{};
  AVCodecContext *codec_ctx{};
  AVIOContext *avio_ctx{};
  uint8_t *avio_ctx_buffer{};
  // Keep track of video-stream-index
  int video_stream_index = 0;

  // Filtering variables
  AVFilterContext *buffersrc_ctx{};
  AVFilterContext *buffersink_ctx{};
  AVFilterGraph *filter_graph{};
  SwsContext *sws_ctx{};
  uint8_t *dst_data[4]{};
  int dst_linesize[4]{};

  // Frames
  AVPacket *packet{};
  AVFrame *frame{};
  AVFrame *filtered_frame{};

  // Target output
  float fps = 10;
  int dst_w = 160;
  int dst_h = 160;
  int start_second = 0;
  int end_second = 0;
  AVPixelFormat dst_pix_fmt = AV_PIX_FMT_RGB24;

  explicit ReadVideoBuffer(float fps = 10,
                           int dst_w = 160, int dst_h = 160,
                           int start_second = 0, int end_second = 0);

  ~ReadVideoBuffer();

  // This function return non-negative video_stream_index if success
  int open_buffer(BufferData &buffer_data);

  int init_filter(const char *filter_descriptor);

  int init_sws_context();

  std::vector<cv::Mat> decode_data_buffer(BufferData &buffer_data);

  void free_resources();

  std::vector<cv::Mat> decode_from_file(const std::string &file_name, BufferData &buffer_data);
};

#endif //AVCPPSAMPLES_VIDEO_READER_H
