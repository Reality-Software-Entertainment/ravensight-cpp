// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Tests for the storage helpers, the default storage path and device id
// generation.

#include <cctype>
#include <filesystem>
#include <set>
#include <string>

#include "ravensight/client.hpp"
#include "storage.hpp"
#include "test_runner.hpp"

using namespace ravensight;

namespace {

bool is_well_formed_device_id(const std::string& id) {
    if (id.size() != 36 || id.compare(0, 4, "dev_") != 0) {
        return false;
    }
    for (std::size_t i = 4; i < id.size(); ++i) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

} // namespace

RS_TEST(sha256_known_vectors) {
    CHECK_EQ(detail::sha256_hex(""),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(detail::sha256_hex("abc"),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    // Two block message (56 bytes forces a second padding block).
    CHECK_EQ(detail::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK_EQ(detail::sha256_hex(std::string(1000, 'a')),
             std::string("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"));
}

RS_TEST(os_random_source_works) {
    unsigned char a[16] = {0};
    unsigned char b[16] = {0};
    CHECK(detail::os_random_bytes(a, sizeof(a)));
    CHECK(detail::os_random_bytes(b, sizeof(b)));
    CHECK(std::string(reinterpret_cast<char*>(a), 16) != std::string(reinterpret_cast<char*>(b), 16));
}

RS_TEST(thousand_device_ids_are_distinct_and_well_formed) {
    std::set<std::string> ids;
    for (int i = 0; i < 1000; ++i) {
        const std::string id = generate_device_id();
        CHECK(is_well_formed_device_id(id));
        ids.insert(id);
    }
    CHECK_EQ(ids.size(), std::size_t(1000));
}

RS_TEST(default_storage_dir_is_per_user_and_named_after_the_key) {
    const std::string dir = default_storage_dir("gt_live_example");
    const std::string hash = detail::sha256_hex("gt_live_example").substr(0, 12);
    REQUIRE(!dir.empty());
    CHECK(dir.size() > hash.size());
    CHECK_EQ(dir.substr(dir.size() - hash.size()), hash);
    CHECK(dir.find("ravensight") != std::string::npos);
#if defined(__APPLE__)
    CHECK(dir.find("Library/Application Support/ravensight/") != std::string::npos);
#elif !defined(_WIN32)
    const std::string xdg = detail::env_value("XDG_DATA_HOME");
    if (xdg.empty()) {
        CHECK(dir.find(".local/share/ravensight/") != std::string::npos);
    } else {
        CHECK(dir.compare(0, xdg.size(), xdg) == 0);
    }
#endif
    // Surrounding whitespace in the key does not move the directory.
    CHECK_EQ(default_storage_dir("  gt_live_example \n"), dir);
}

RS_TEST(text_files_round_trip_and_create_parents) {
    const std::string dir = detail::join_path(
        detail::join_path(std::filesystem::temp_directory_path().string(),
                          "ravensight-storage-test-" + generate_device_id()),
        "nested");
    const std::string path = detail::join_path(dir, "file");
    CHECK(detail::write_text_file(path, "hello"));
    std::string text;
    CHECK(detail::read_text_file(path, text));
    CHECK_EQ(text, std::string("hello"));
    CHECK(detail::write_text_file(path, "again"));
    CHECK(detail::read_text_file(path, text));
    CHECK_EQ(text, std::string("again"));
    CHECK(!detail::read_text_file(detail::join_path(dir, "missing"), text));
}
