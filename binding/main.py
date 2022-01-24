import time
from build import av_reader_module

if __name__ == '__main__':
    start = time.time()
    url = "https://vnno-zn-1-tf-baomoi-video.zadn.vn/" \
          "8a51fe65a17a75a4ab3612d96d48e36f/62658871/" \
          "streaming.baomoi.com/2018/05/24/61/26152207/14597479.mp4"
    frames = av_reader_module.read_url(url)
    print(f"Number of frame: {len(frames)}")
    print(f"Elapsed: {time.time() - start:.3f}")
