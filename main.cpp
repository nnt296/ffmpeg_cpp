#include "data_types.h"
#include "curl_reader.h"
#include "video_reader.h"

int main(int argc, char **argv) {
  InMemVideoReader reader(720, 300, 4000);
  // std::string url = "https://vnno-zn-10-tf-baomoi-video.zadn.vn/179c78ef8ec345232393fb13af6d4f85/62619d21/streaming.baomoi.com/2018/10/01/119/27968791/5255385.mp4";
  std::string url = "https://vnno-zn-1-tf-baomoi-video.zadn.vn/8a51fe65a17a75a4ab3612d96d48e36f/62658871/streaming.baomoi.com/2018/05/24/61/26152207/14597479.mp4";

  for (int i = 0; i < 1; i++) {
    BufferData bd{};
    get_ftp_buffer_data(url, bd, 100);
    auto v_rgb = reader.decode_data_buffer(bd);
    std::cout << "HERE: " << v_rgb.size() << std::endl;
    free_bd(bd);

    sleep(2);
  }

  return 0;
}