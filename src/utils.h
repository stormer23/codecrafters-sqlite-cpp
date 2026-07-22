#ifndef UTILS_H
#define UTILS_H

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <algorithm>

// Converts bytes to a numeric value (Big-Endian)
uint64_t big_endian(std::ifstream &stream, int n) {
    uint64_t val = 0;
    for (int i = 0; i < n; i++) {
        val = (val << 8) | static_cast<unsigned char>(stream.get());
    }
    return val;
}

// SQLite Variable-Length Integer (Varint) Parser
std::pair<uint64_t, int> parse_varint(std::ifstream &stream) {
    uint64_t val = 0;
    int bytes = 0;
    for (int i = 0; i < 9; i++) {
        unsigned char byte = stream.get();
        bytes++;
        if (i < 8) {
            val = (val << 7) | (byte & 0x7F);
            if (!(byte & 0x80)) break;
        } else {
            val = (val << 8) | byte;
        }
    }
    return {val, bytes};
}

// Parses a row based on SQLite record format
std::vector<std::string> parse_record(std::ifstream &stream) {
    auto start = stream.tellg();
    
    // 1. Read Header Size
    auto header_size_data = parse_varint(stream);
    uint64_t header_size = header_size_data.first;
    
    std::vector<uint64_t> serial_types;
    uint64_t bytes_read = header_size_data.second;
    
    // 2. Extract all Serial Types from the header
    while (bytes_read < header_size) {
        auto type_data = parse_varint(stream);
        serial_types.push_back(type_data.first);
        bytes_read += type_data.second;
    }
    
    // 3. Extract the actual payload data based on the Serial Types
    std::vector<std::string> row;
    for (auto type : serial_types) {
        if (type == 0) row.push_back("NULL"); 
        else if (type == 1) row.push_back(std::to_string((int8_t)big_endian(stream, 1)));
        else if (type == 2) row.push_back(std::to_string((int16_t)big_endian(stream, 2)));
        else if (type == 3) row.push_back(std::to_string((int32_t)big_endian(stream, 3)));
        else if (type == 4) row.push_back(std::to_string((int32_t)big_endian(stream, 4)));
        else if (type == 5) row.push_back(std::to_string((int64_t)big_endian(stream, 6))); // 6-byte int
        else if (type == 6) row.push_back(std::to_string((int64_t)big_endian(stream, 8))); // 8-byte int
        else if (type == 7) {
            uint64_t val = big_endian(stream, 8);
            double d;
            std::memcpy(&d, &val, sizeof(double));
            row.push_back(std::to_string(d));
        }
        else if (type == 8) row.push_back("0");
        else if (type == 9) row.push_back("1");
        else if (type >= 12 && (type % 2 == 0)) { // BLOB
            int len = (type - 12) / 2;
            stream.seekg(len, std::ios::cur); // Skip blobs for now
            row.push_back("[BLOB]");
        }
        else if (type >= 13 && (type % 2 != 0)) { // TEXT
            int len = (type - 13) / 2;
            std::string s(len, ' ');
            stream.read(&s[0], len);
            row.push_back(s);
        }
    }
    return row;
}

#endif