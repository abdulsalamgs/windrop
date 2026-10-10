// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

namespace windrop {
enum class Message : uint32_t { Hello=1, Frame=2, Channel=3, Transmit=4, Stop=5, Fault=6, ChannelAck=7 };
struct Record { Message type; std::vector<uint8_t> bytes; };
constexpr uint32_t max_record = 32768;
inline void binary_stdio() { _setmode(_fileno(stdin),_O_BINARY); _setmode(_fileno(stdout),_O_BINARY); }
inline bool read_exact(FILE* file, void* data, size_t size) {
    auto p=static_cast<uint8_t*>(data);
    while(size) { size_t n=fread(p,1,size,file); if(!n) return false; p+=n; size-=n; }
    return true;
}
inline bool read_record(FILE* file, Record& record) {
    uint32_t header[2];
    size_t n=fread(header,1,sizeof(header),file);
    if(n==0 && feof(file)) return false;
    if(n!=sizeof(header) || header[1]>max_record || header[0]<1 || header[0]>7)
        throw std::runtime_error("Invalid or truncated radio record");
    record.type=static_cast<Message>(header[0]); record.bytes.resize(header[1]);
    if(!read_exact(file,record.bytes.data(),record.bytes.size())) throw std::runtime_error("Truncated radio payload");
    return true;
}
inline void write_record(FILE* file, Message type, const void* data, size_t size) {
    if(size>max_record) throw std::runtime_error("Radio record too large");
    uint32_t header[2]={static_cast<uint32_t>(type),static_cast<uint32_t>(size)};
    if(fwrite(header,1,sizeof(header),file)!=sizeof(header) || (size && fwrite(data,1,size,file)!=size) || fflush(file))
        throw std::runtime_error("Radio pipe closed");
}
}
