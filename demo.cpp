extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavutil/file.h"
}
#include "iostream"
#include "opencv2/opencv.hpp"

#define IO_CTX_BUFFER_SIZE 4096 * 4;      // Tobe used later

struct buffer_data {
  uint8_t *ptr;             // Store pointer to current byte
  uint8_t *ori_ptr;         // Store pointer to the first byte
  size_t size;              // Size left in the buffer
  size_t file_size;         // Total size of video buffer
};

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

// print out the steps and errors
static void logging(const char *fmt, ...);

// decode packets into frames
static int decode_packet(AVPacket *pPacket, AVCodecContext *pCodecContext, AVFrame *pFrame);

// save a frame into a .pgm file
static void save_gray_frame(unsigned char *buf, int wrap, int xsize, int ysize, char *filename);

// print out the steps and errors
static void logging(const char *fmt, ...) {
  va_list args;
  fprintf(stderr, "[LOG]: ");
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fprintf(stderr, "\n");
}

static void save_gray_frame(unsigned char *buf, int wrap, int xsize, int ysize, char *filename) {
  FILE *f;
  int i;
  f = fopen(filename, "w");
  // writing the minimal required header for a pgm file format
  // portable gray-map format -> https://en.wikipedia.org/wiki/Netpbm_format#PGM_example
  fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);

  // writing line by line
  for (i = 0; i < ysize; i++)
    fwrite(buf + i * wrap, 1, xsize, f);
  fclose(f);
}

static int decode_packet(AVPacket *pPacket, AVCodecContext *pCodecContext, AVFrame *pFrame) {
  // Supply raw packet data as input to a decoder
  // https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga58bc4bf1e0ac59e27362597e467efff3
  int response = avcodec_send_packet(pCodecContext, pPacket);

  if (response < 0) {
    char err_str[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(response, err_str, AV_ERROR_MAX_STRING_SIZE);
    logging("Error while sending a packet to the decoder: %s", err_str);
    return response;
  }

  while (response >= 0) {
    // Return decoded output data (into a frame) from a decoder
    // https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga11e6542c4e66d3028668788a1a74217c
    response = avcodec_receive_frame(pCodecContext, pFrame);
    if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
      break;
    } else if (response < 0) {
      char err_str[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(response, err_str, AV_ERROR_MAX_STRING_SIZE);
      logging("Error while receiving a frame from the decoder: %s", err_str);
      return response;
    }

    if (response >= 0) {
      logging(
          "Frame %d (type=%c, size=%d bytes, format=%d) pts %d key_frame %d [DTS %d]",
          pCodecContext->frame_number,
          av_get_picture_type_char(pFrame->pict_type),
          pFrame->pkt_size,
          pFrame->format,
          pFrame->pts,
          pFrame->key_frame,
          pFrame->coded_picture_number
      );

      char frame_filename[1024];
      snprintf(frame_filename, sizeof(frame_filename), "%s-%d.png", "frame", pCodecContext->frame_number);
      // Check if the frame is a planar YUV 4:2:0, 12bpp
      // That is the format of the provided .mp4 file
      // RGB formats will definitely not give a gray image
      // Other YUV image may do so, but untested, so give a warning
      if (pFrame->format != AV_PIX_FMT_YUV420P) {
        logging(
            "Warning: the generated file may not be a grayscale image, but could e.g. be just the R component if the video format is RGB");
      }
      // save a grayscale frame into a .pgm file
      // save_gray_frame(pFrame->data[0], pFrame->linesize[0], pFrame->width, pFrame->height, frame_filename);
      cv::Mat im(pFrame->height, pFrame->width, CV_8UC1, pFrame->data[0], pFrame->linesize[0]);
      cv::imwrite(frame_filename, im);
    }
  }
  return 0;
}

int main(int argc, char *argv[]) {
  AVFormatContext *fmt_ctx = nullptr;
  AVIOContext *avio_ctx = nullptr;
  uint8_t *buffer, *avio_ctx_buffer;
  size_t buffer_size = 4096;
  int avio_ctx_buffer_size = 4096;
  int ret;
  buffer_data bd{};
  int video_idx = 1;

  AVCodecContext *codec_ctx;
  AVCodec *codec;
  AVFrame *frame;
  AVPacket *packet;

  int response = 0;
  int how_many_packets_to_process = 8;

  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " input file" << std::endl;
    return 1;
  }

  std::string input_filename = "/home/local/BM/video_thumbnail/data/named_video/corrupt.mp4";

  // Slurp file content into buffer
  ret = av_file_map(input_filename.c_str(), &buffer, &buffer_size, 0, nullptr);
  if (ret < 0)
    goto end;

  bd.ptr        = buffer;
  bd.ori_ptr    = buffer;
  bd.size       = buffer_size;
  bd.file_size  = buffer_size;

  if (!(fmt_ctx = avformat_alloc_context())) {
    ret = AVERROR(ENOMEM);
    goto end;
  }

  avio_ctx_buffer = static_cast<uint8_t *>(av_malloc(avio_ctx_buffer_size));
  if (!avio_ctx_buffer) {
    ret = AVERROR(ENOMEM);
    goto end;
  }
  avio_ctx = avio_alloc_context(avio_ctx_buffer, avio_ctx_buffer_size,
                                0, &bd, &read_packet, nullptr, &seek_in_buffer);
  if (!avio_ctx) {
    ret = AVERROR(ENOMEM);
    goto end;
  }
  fmt_ctx->pb = avio_ctx;

  ret = avformat_open_input(&fmt_ctx, nullptr, nullptr, nullptr);
  if (ret < 0) {
    std::cerr << "Could not open input" << std::endl;
    goto end;
  }

  ret = avformat_find_stream_info(fmt_ctx, nullptr);
  if (ret < 0) {
    std::cerr << "Could not find stream information" << std::endl;
    goto end;
  }

  for (int i = 0; i < fmt_ctx->nb_streams; i++) {
    AVCodecParameters *codec_params = nullptr;
    codec_params = fmt_ctx->streams[i]->codecpar;

    if (codec_params->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_idx = i;
      codec = avcodec_find_decoder(codec_params->codec_id);
      codec_ctx = avcodec_alloc_context3(codec);

      if (avcodec_parameters_to_context(codec_ctx, codec_params) < 0) {
        std::cerr << "Failed to copy codec params to codec context" << std::endl;
        goto end;
      }

      break;
    }
  }

  if (video_idx == -1) {
    std::cerr << "Could not find a video stream" << std::endl;
    goto end;
  }

  ret = avcodec_open2(codec_ctx, codec, nullptr);
  if (ret < 0) {
    std::cerr << "Could open codec" << std::endl;
    goto end;
  }

  frame = av_frame_alloc();
  if (!frame) {
    ret = AVERROR(ENOMEM);
    goto end;
  }

  packet = av_packet_alloc();
  if (!packet) {
    ret = AVERROR(ENOMEM);
    goto end;
  }

  while (av_read_frame(fmt_ctx, packet) >= 0) {
    if (packet->stream_index == video_idx) {
      logging("AVPacket->pts %" PRId64, packet->pts);
      response = decode_packet(packet, codec_ctx, frame);
      if (response < 0)
        break;
      // stop it, otherwise we'll be saving hundreds of frames
      if (--how_many_packets_to_process <= 0) break;
    }

    av_packet_unref(packet);
  }
  // av_dump_format(fmt_ctx, 0, input_filename.c_str(), 0);

end:
  avformat_close_input(&fmt_ctx);

  // Note: the internal buffer could have changed, and be != avio_ctx_buffer
  if (avio_ctx)
    av_freep(&avio_ctx->buffer);
  avio_context_free(&avio_ctx);

  av_frame_free(&frame);
  avcodec_free_context(&codec_ctx);
  av_packet_free(&packet);
  av_file_unmap(buffer, buffer_size);

  if (ret < 0) {
    char error[AV_ERROR_MAX_STRING_SIZE];
    av_make_error_string(error, AV_ERROR_MAX_STRING_SIZE, ret);
    std::cerr << "Error occurred: " << error;
    return 1;
  }

  return 0;
}
