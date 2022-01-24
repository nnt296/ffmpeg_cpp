#ifndef AVCPPSAMPLES_DATA_TYPES_H
#define AVCPPSAMPLES_DATA_TYPES_H

#define AVIO_CONTEXT_BUFFER_SIZE 4096

struct FileInfo {
  std::string filename;
  double filesize;
  long filetime;
};

struct BufferData {
  uint8_t *ptr;
  uint8_t *ori_ptr;
  size_t size;
  size_t file_size;
};

static void free_bd(BufferData &buffer_data) {
  // Free the original ptr and set the current ptr to NULL
  free(buffer_data.ori_ptr);
  buffer_data.ori_ptr = nullptr;
  buffer_data.ptr = nullptr;
  buffer_data.size = 0;
  buffer_data.file_size = 0;
}


#endif //AVCPPSAMPLES_DATA_TYPES_H
