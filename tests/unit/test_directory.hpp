#ifndef SEASIDE_TEST_DIRECTORY_HPP
#define SEASIDE_TEST_DIRECTORY_HPP

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <system_error>

namespace seaside::test {
class Directory {
  public:
    Directory() {
        // The supervising runner owns TMPDIR, including after a killed test
        // cannot run this destructor. Direct invocations use the normal temp root.
        std::string pattern =
            (std::filesystem::temp_directory_path() / "seaside-unit-XXXXXX").string();
        auto *created = ::mkdtemp(pattern.data());
        if (!created)
            throw std::system_error(errno, std::generic_category(), "test directory");
        path_ = created;
    }
    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        if (error)
            ADD_FAILURE() << "test directory cleanup: " << error.message();
    }
    Directory(const Directory &) = delete;
    Directory &operator=(const Directory &) = delete;
    const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};
} // namespace seaside::test
#endif
