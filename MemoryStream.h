#pragma once
#include <AudioFileSource.h>
#include <algorithm>
#include <string.h>

class MemoryStream : public AudioFileSource {
public:
  MemoryStream(uint8_t* data, size_t len)
    : data_(data), length_(len), position_(0) {}

  virtual ~MemoryStream() {}

  virtual uint32_t read(void* dest, uint32_t len) override {
    uint32_t toRead = std::min(len, static_cast<uint32_t>(length_ - position_));
    memcpy(dest, data_ + position_, toRead);
    position_ += toRead;
    return toRead;
  }

  uint32_t read() {  // Not an override
    if (position_ >= length_) return 0;
    return data_[position_++];
  }

  bool seek(uint32_t pos, int dir) {  // Not an override
    if (dir == SEEK_SET) position_ = pos;
    else if (dir == SEEK_CUR) position_ += pos;
    else if (dir == SEEK_END) position_ = length_ + pos;
    if (position_ > length_) position_ = length_;
    return true;
  }

  virtual bool isOpen() override { return true; }
  virtual bool close() override { return true; }
  virtual uint32_t getSize() override { return length_; }
  virtual uint32_t getPos() override { return position_; }
  bool isFinished() const {
  return position_ >= length_;
}

private:
  uint8_t* data_;
  size_t length_;
  size_t position_;
};