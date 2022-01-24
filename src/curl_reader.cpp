#include <cstring>
#include "curl_reader.h"

static size_t throw_away(void *ptr, size_t size, size_t nmemb, void *data) {
/* Example from libcurl page: https://curl.se/libcurl/c/example.html */
  (void) ptr;
  (void) data;
  /* we are not interested in the headers itself,
     so we only return the size we would have saved ... */
  return (size_t) (size * nmemb);
}

static size_t write_memory_callback(void *contents, size_t size, size_t nmemb, void *user_ptr) {
  /* Example from https://curl.se/libcurl/c/getinmemory.html */

  size_t real_size = size * nmemb;
  auto *mem = (BufferData *) user_ptr;

  auto *ptr = static_cast<uint8_t *>(realloc(mem->ori_ptr, mem->file_size + real_size + 1));

  if (!ptr) {
    // Out of memory
    std::cerr << "Not enough memory (re-alloc returned NULL)" << std::endl;
    return 0;
  }

  mem->ori_ptr = ptr;
  memcpy(&(mem->ori_ptr)[mem->file_size], contents, real_size);
  mem->file_size += real_size;
  mem->ori_ptr[mem->file_size] = 0;

  return real_size;
}

void get_ftp_info(const std::string &url, FileInfo &fileInfo, long timeout_seconds) {
  size_t pos = url.find_last_of('/');
  fileInfo.filename = url.substr(pos + 1, url.size() - pos);

  CURL *curl_handler;
  CURLcode res;

  curl_global_init(CURL_GLOBAL_DEFAULT);
  curl_handler = curl_easy_init();

  if (curl_handler) {
    curl_easy_setopt(curl_handler, CURLOPT_URL, url.c_str());
    /* No download if the file */
    curl_easy_setopt(curl_handler, CURLOPT_NOBODY, 1L);
    /* Ask for filetime */
    curl_easy_setopt(curl_handler, CURLOPT_FILETIME, 1L);
    curl_easy_setopt(curl_handler, CURLOPT_HEADERFUNCTION, throw_away);
    curl_easy_setopt(curl_handler, CURLOPT_HEADER, 0L);
    /* Switch on full protocol/debug output */
    /* curl_easy_setopt(curl_handler, CURLOPT_VERBOSE, 1L); */

    /* Timeout the call */
    /* complete within N seconds */
    if (timeout_seconds > 0)
      curl_easy_setopt(curl_handler, CURLOPT_TIMEOUT, timeout_seconds);

    res = curl_easy_perform(curl_handler);

    if (CURLE_OK == res) {
      /* https://curl.se/libcurl/c/curl_easy_getinfo.html */
      res = curl_easy_getinfo(curl_handler, CURLINFO_FILETIME, &fileInfo.filetime);
      if ((CURLE_OK == res) && (fileInfo.filetime >= 0)) {
        auto file_time = (time_t) fileInfo.filetime;
        std::cout << "[INFO] filetime \"" << fileInfo.filename << "\" : " << ctime(&file_time) << std::endl;
      }
      res = curl_easy_getinfo(curl_handler, CURLINFO_CONTENT_LENGTH_DOWNLOAD,
                              &fileInfo.filesize);
      if ((CURLE_OK == res) && (fileInfo.filesize > 0.0))
        std::cout << "[INFO] filesize \"" << fileInfo.filename << "\" : " << fileInfo.filesize << " bytes" << std::endl;
    } else {
      /* we failed */
      std::cerr << "[ERROR] Failed to get info with curl_handler " << curl_easy_strerror(res) << std::endl;
    }

    /* always cleanup */
    curl_easy_cleanup(curl_handler);
  }
}

void get_ftp_buffer_data(const std::string &url, BufferData &bd, long timeout_seconds) {
  // Clear bd before getting new data, newly created BufferData is not affected
  free_bd(bd);

  size_t pos = url.find_last_of('/');
  std::string filename = url.substr(pos + 1, url.size() - pos);

  CURL *curl_handler;
  CURLcode res;

  bd.ori_ptr = static_cast<uint8_t *>(malloc(1));
  bd.file_size = 0;

  curl_global_init(CURL_GLOBAL_ALL);

  /* init the curl session */
  curl_handler = curl_easy_init();

  /* specify URL to get */
  curl_easy_setopt(curl_handler, CURLOPT_URL, url.c_str());

  /* send all data to this function  */
  curl_easy_setopt(curl_handler, CURLOPT_WRITEFUNCTION, write_memory_callback);

  /* we pass our 'chunk' struct to the callback function */
  curl_easy_setopt(curl_handler, CURLOPT_WRITEDATA, (void *) &bd);

  /* some servers do not like requests that are made without a user-agent
     field, so we provide one */
  curl_easy_setopt(curl_handler, CURLOPT_USERAGENT, "libcurl-agent/1.0");

  /* Timeout the call */
  if (timeout_seconds > 0)
    curl_easy_setopt(curl_handler, CURLOPT_TIMEOUT, timeout_seconds);

  /* get it! */
  res = curl_easy_perform(curl_handler);

  /* check for errors */
  if (res != CURLE_OK)
    std::cerr << "[ERROR] Curl FTP failed: " << curl_easy_strerror(res) << std::endl;
  else
    std::cout << "[INFO] Retrieved \"" << filename << "\" : " << bd.file_size << " bytes" << std::endl;

  /* cleanup curl stuff */
  curl_easy_cleanup(curl_handler);

  bd.ptr = bd.ori_ptr;
  bd.size = bd.file_size;

  /* we are done with libcurl, so clean it up */
  curl_global_cleanup();
}