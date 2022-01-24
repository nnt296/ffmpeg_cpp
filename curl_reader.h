#ifndef AVCPPSAMPLES_CURL_READER_H
#define AVCPPSAMPLES_CURL_READER_H

#include "string"
#include "curl/curl.h"
#include "iostream"
#include "data_types.h"

void get_ftp_info(const std::string &url, FileInfo &fileInfo, long timeout_seconds = 20);

void get_ftp_buffer_data(const std::string &url, BufferData &bd, long timeout_seconds = 0);


#endif //AVCPPSAMPLES_CURL_READER_H
