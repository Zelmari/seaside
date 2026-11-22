#ifndef SEASIDE_STORAGE_FD_HPP
#define SEASIDE_STORAGE_FD_HPP

#include <unistd.h>
#include <utility>

namespace seaside {

class StorageFd {
  public:
    explicit StorageFd(int value) noexcept : value_(value) {}
    ~StorageFd() {
        if (value_ >= 0) {
            ::close(value_);
        }
    }
    StorageFd(const StorageFd &) = delete;
    StorageFd &operator=(const StorageFd &) = delete;
    int get() const noexcept { return value_; }
    int close() noexcept { return ::close(std::exchange(value_, -1)); }

  private:
    int value_;
};

} // namespace seaside
#endif
