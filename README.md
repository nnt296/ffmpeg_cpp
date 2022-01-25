# Decoding video with ffmpeg
  This project aims to download video from internet using libcurl and decode/filter/scale/formatRGB24 
  video using ffmpeg, and outputs list of cv::Mat

  Custom `root/src` for different usages

## Prerequisites

- Build and install ffmpeg (better from source)
- Install libcurl-dev:
  ```
  sudo apt-get install libcurl4-openssl-dev
  ```
- Build and install Opencv
- Tested with ubuntu-20.04, gcc 9.3.0, ffmpeg 4.2.4


## Run 

- cpp
  ```
  mkdir build && cd build
  cmake .. && make -j4
  ./AvReader
  ```

- pybind (tested with python3.8 and opencv-python==4.5.4.60)
  ```
  cd binding
  mkdir build && cd build
  cmake .. && make -j4
  
  cd ../
  python main.py
  ```
