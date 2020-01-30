#include <filesystem>
#include <fstream>
#include "wad.hpp"
#include <miniz.h>
#include <zstd.h>
#include <charconv>
#include "utility.hpp"

using namespace LCS;

Wad::Wad(const std::filesystem::path& path, const std::string& name)
    : path_(fs::absolute(path)), size_(fs::file_size(path_)), name_(name)  {

    file_.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    file_.open(path_, std::ios::binary);
    file_.read((char*)&header_, sizeof(header_));
    if (header_.version != std::array{'R', 'W', '\x3', '\x0'}) {
        throw std::ifstream::failure("Not a wad file");
    }
    dataBegin_ = header_.filecount * sizeof(Entry) + sizeof(header_);
    dataEnd_ = size_;
    if (dataBegin_ > dataEnd_) {
        throw std::ifstream::failure("Failed to read TOC");
    }
    entries_.resize(header_.filecount);
    file_.read((char*)entries_.data(), header_.filecount * sizeof(Entry));
    for(auto const& entry: entries_) {
        if (entry.dataOffset > dataEnd_ || entry.dataOffset < dataBegin_) {
            throw std::ifstream::failure("Entry not in data region!");
        }
        if (entry.dataOffset + entry.sizeCompressed > dataEnd_) {
            throw std::ifstream::failure("Entry outside of data!");
        }
    }
}


void Wad::extract(fs::path const& dest, HashTable const& hashtable, Progress& progress) const {
    size_t totalSize = 0;
    uint32_t maxCompressed = 0;
    uint32_t maxUncompressed = 0;
    for(auto const& entry: entries_) {
        totalSize += entry.sizeUncompressed;
        maxCompressed = std::max(maxCompressed, entry.sizeCompressed);
        maxUncompressed = std::max(maxUncompressed, entry.sizeUncompressed);
    }
    progress.startItem(path_, totalSize);
    std::vector<char> compressedBuffer;
    std::vector<char> uncompressedBuffer;
    compressedBuffer.reserve((size_t)(maxCompressed));
    uncompressedBuffer.reserve((size_t)(maxUncompressed));

    for(auto const& entry: entries_) {
        file_.seekg((std::streampos)entry.dataOffset, std::ios::beg);
        if (entry.type == Entry::Uncompressed) {
            file_.read(uncompressedBuffer.data(), entry.sizeUncompressed);
        } else if(entry.type == Entry::ZlibCompressed) {
            file_.read(compressedBuffer.data(), entry.sizeCompressed);
            mz_stream strm = {};
            if(mz_inflateInit2(&strm, 16 + MAX_WBITS) != MZ_OK) {
               throw std::runtime_error("Failed to initialize zlib uncompress stream!");
            }
            strm.next_in = (unsigned char const*)compressedBuffer.data();
            strm.avail_in = entry.sizeCompressed;
            strm.next_out = (unsigned char*)uncompressedBuffer.data();
            strm.avail_out = entry.sizeUncompressed;
            mz_inflate(&strm, MZ_FINISH);
            mz_inflateEnd(&strm);
        } else if(entry.type == Entry::ZStandardCompressed) {
            file_.read(compressedBuffer.data(), entry.sizeCompressed);
            ZSTD_decompress(uncompressedBuffer.data(), entry.sizeUncompressed,
                            compressedBuffer.data(), entry.sizeCompressed);
        } else if(entry.type == Entry::FileRedirection) {
            // file_.read(uncompressedBuffer.data(), entry.sizeUncompressed);
        }

        if (entry.type != Entry::FileRedirection) {
            fs::path outpath = dest;
            if (auto p = hashtable.find(entry.xxhash); p) {
                outpath /= *p;
            } else {
                char hex[16];
                auto result = std::to_chars(hex, hex + sizeof(hex), entry.xxhash, 16);
                outpath /= std::string(hex, result.ptr);
                outpath.replace_extension(ScanExtension(uncompressedBuffer.data(), entry.sizeUncompressed));
            }
            fs::create_directories(outpath.parent_path());
            std::ofstream outfile;
            outfile.exceptions(std::ofstream::failbit | std::ofstream::badbit);
            outfile.open(outpath, std::ios::binary);
            outfile.write(uncompressedBuffer.data(), entry.sizeUncompressed);
        }
        progress.consumeData(entry.sizeUncompressed);
    }

    progress.finishItem();
}
