extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavutil/file.h"
}
#include "iostream"

struct buffer_data {
  uint8_t *ptr;
  size_t size;    // Size left in the buffer
};

static int read_packet(void *opaque, uint8_t *buf, int buf_size) {
  auto *bd = (struct buffer_data *) opaque;
  buf_size = FFMIN(buf_size, bd->size);
  if (!buf_size)
    return AVERROR_EOF;
  std::cout << "ptr: " << bd->ptr << " size: " << bd->size << std::endl;

  // Copy internal buffer data to buf
  memcpy(buf, bd->ptr, buf_size);
  bd->ptr += buf_size;
  bd->size -= buf_size;

  return buf_size;
}

int main(int argc, char *argv[]) {
  AVFormatContext *fmt_ctx = nullptr;
  AVIOContext *avio_ctx = nullptr;
  uint8_t *buffer, *avio_ctx_buffer;
  size_t buffer_size = 4096;
  int avio_ctx_buffer_size = 4096;
  int ret;
  struct buffer_data bd{};

  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " input file" << std::endl;
    return 1;
  }

  std::string input_filename = "/home/local/BM/video_thumbnail/data/named_video/corrupt.mp4";

  // Slurp file content into buffer
  ret = av_file_map(input_filename.c_str(), &buffer, &buffer_size, 0, nullptr);
  if (ret < 0)
    goto end;

  bd.ptr  = buffer;
  bd.size = buffer_size;

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
                                0, &bd, &read_packet, nullptr, nullptr);
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

  av_dump_format(fmt_ctx, 0, input_filename.c_str(), 0);

end:
  avformat_close_input(&fmt_ctx);

  // Note: the internal buffer could have changed, and be != avio_ctx_buffer
  if (avio_ctx)
    av_freep(&avio_ctx->buffer);
  avio_context_free(&avio_ctx);

  av_file_unmap(buffer, buffer_size);

  if (ret < 0) {
    char error[AV_ERROR_MAX_STRING_SIZE];
    av_make_error_string(error, AV_ERROR_MAX_STRING_SIZE, ret);
    std::cerr << "Error occurred: " << error;
    return 1;
  }

  return 0;
}
