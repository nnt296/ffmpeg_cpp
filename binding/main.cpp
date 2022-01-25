#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <opencv2/opencv.hpp>
#include "ndarray_converter.h"
#include "data_types.h"
#include "curl_reader.h"
#include "video_reader.h"

namespace py = pybind11;

/**
 * Get list of cv::Mat from FTP url
 *
 * @param url: ftp://speedtest:speedtest@ftp.otenet.gr/test1Mb.db
 *             Work with http as well
 *             https://vnno-zn-1-tf-baomoi-video.zadn.vn/8a51fe65a17a75a4ab3612d96d48e36f/62658871/streaming.baomoi.com/2018/05/24/61/26152207/14597479.mp4
 * @param max_size: max target size
 * @param video_max_length: max video length (in seconds)
 * @param max_num_frames: max number of frame to output
 * @param timeout_seconds: Timeout for downloading file
 * @return list of cv::Mat
 */
std::vector<cv::Mat> read_url(const std::string &url,
                              int max_size = 720,
                              int video_max_length = 300,
                              int max_num_frames = 4000,
                              int timeout_seconds = 0) {
  BufferData bd{};
  InMemVideoReader vid_reader(max_size, video_max_length, max_num_frames);
  get_ftp_buffer_data(url, bd, timeout_seconds);

  if (bd.size == 0) {
    std::cerr << "[ERROR] Empty file " << url << std::endl;
    return {};
  }

  auto v_rgb = vid_reader.decode_data_buffer(bd);
  free_bd(bd);

  return v_rgb;
}

/* Decode RGB frames from file*/
std::vector<cv::Mat> read_file(const std::string &file_path,
                               int max_size = 720,
                               int video_max_length = 300,
                               int max_num_frames = 4000) {
  BufferData bd{};
  InMemVideoReader vid_reader(max_size, video_max_length, max_num_frames);

  auto v_rgb = vid_reader.decode_from_file(file_path, bd);
  free_bd(bd);

  return v_rgb;
}

PYBIND11_MODULE(av_reader_module, m) {
  NDArrayConverter::init_numpy();

  using namespace pybind11::literals;

  m.def("read_url", &read_url, "Get list of frame from ftp url",
        "url"_a = "",
        "max_size"_a = 720,
        "video_max_length"_a = 300,
        "max_num_frames"_a = 4000,
        "timeout_seconds"_a = 0);

  m.def("read_file", &read_file, "Get list of frame from filesystem",
        "file_path"_a = "",
        "max_size"_a = 720,
        "video_max_length"_a = 300,
        "max_num_frames"_a = 4000);
}
