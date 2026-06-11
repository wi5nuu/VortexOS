// SPDX-License-Identifier: GPL-2.0-or-later
// === vortex/tests/test_vfs.cpp ===
// VortexOS Kernel — VFS Unit Tests
//
// Phase 4.1: VFS validation — tmpfs, devfs, path resolution, mount points
//
// Reference: task.md §R46 — Minimal 3 tests per module

#include "vortex/kernel/vfs.hpp"
#include "vortex/kernel/mm.hpp"
#include "vortex/kernel/heap.hpp"
#include "vortex/kernel/ktest.hpp"
#include "vortex/arch/x86_64/serial.hpp"

namespace vfs_test {

using namespace vortex::kernel::vfs;
using namespace vortex::test;
using vortex::arch::x86_64::serial_write;
using vortex::arch::x86_64::serial_write_dec;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static int buf_cmp(const uint8_t* a, const uint8_t* b, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (a[i] != b[i]) return static_cast<int>(a[i]) - static_cast<int>(b[i]);
    }
    return 0;
}

static void buf_fill(uint8_t* buf, uint8_t val, size_t len) {
    for (size_t i = 0; i < len; ++i) buf[i] = val;
}

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            serial_write("  [FAIL] "); \
            serial_write(msg); \
            serial_write("\n"); \
            return; \
        } \
    } while (0)

#define TEST_PASS(msg) \
    serial_write("  [PASS] "); \
    serial_write(msg); \
    serial_write("\n")

// ─── Tests ────────────────────────────────────────────────────────────────────

KTEST(vfs, init) {
    TEST_ASSERT(vfs_root() != nullptr, "vfs_root should not be null after init");
    TEST_PASS("vfs_init + vfs_root");
}

KTEST(vfs, root_properties) {
    VfsNode* root = vfs_root();
    TEST_ASSERT(root != nullptr, "root node exists");
    TEST_ASSERT(root->type == NodeType::DIRECTORY, "root is a directory");
    TEST_ASSERT(root->ops != nullptr, "root has ops");
    TEST_ASSERT(root->parent == root, "root parent is itself");
    TEST_PASS("root node properties");
}

KTEST(vfs, open_root) {
    VfsNode* root = vfs_open("/");
    TEST_ASSERT(root != nullptr, "vfs_open(\"/\") succeeds");
    TEST_ASSERT(root == vfs_root(), "opened root is vfs_root()");
    TEST_PASS("open root path");
}

KTEST(vfs, open_dev_null) {
    VfsNode* null_dev = vfs_open("/dev/null");
    TEST_ASSERT(null_dev != nullptr, "vfs_open(\"/dev/null\") succeeds");
    TEST_ASSERT(null_dev->type == NodeType::CHAR_DEVICE, "/dev/null is CHAR_DEVICE");
    TEST_ASSERT(null_dev->ops != nullptr, "/dev/null has ops");

    uint8_t buf[16];
    buf_fill(buf, 0xAA, sizeof(buf));
    size_t nread = null_dev->ops->read(null_dev, 0, sizeof(buf), buf);
    TEST_ASSERT(nread == 0, "/dev/null read returns 0");
    TEST_PASS("open and read /dev/null");
}

KTEST(vfs, dev_null_write_discard) {
    VfsNode* null_dev = vfs_open("/dev/null");
    TEST_ASSERT(null_dev != nullptr, "/dev/null exists");

    const char data[] = "hello world";
    size_t nwritten = null_dev->ops->write(null_dev, 0, sizeof(data), data);
    TEST_ASSERT(nwritten == sizeof(data), "/dev/null write discards and returns size");
    TEST_PASS("write to /dev/null discarded");
}

KTEST(vfs, dev_zero_read) {
    VfsNode* zero_dev = vfs_open("/dev/zero");
    TEST_ASSERT(zero_dev != nullptr, "/dev/zero exists");

    uint8_t buf[32];
    buf_fill(buf, 0xFF, sizeof(buf));

    size_t nread = zero_dev->ops->read(zero_dev, 0, sizeof(buf), buf);
    TEST_ASSERT(nread == sizeof(buf), "/dev/zero read returns requested size");

    for (size_t i = 0; i < sizeof(buf); ++i) {
        if (buf[i] != 0) {
            serial_write("  [FAIL] /dev/zero byte "); serial_write_dec(i);
            serial_write(" = "); serial_write_dec(buf[i]); serial_write(" != 0\n");
            return;
        }
    }
    TEST_PASS("read /dev/zero returns zeros");
}

KTEST(vfs, dev_console_write) {
    VfsNode* console_dev = vfs_open("/dev/console");
    TEST_ASSERT(console_dev != nullptr, "/dev/console exists");

    const char msg[] = "VFS test: console write OK\n";
    size_t nwritten = console_dev->ops->write(console_dev, 0, sizeof(msg), msg);
    TEST_ASSERT(nwritten == sizeof(msg), "/dev/console write returns size");
    TEST_PASS("write to /dev/console");
}

KTEST(vfs, dev_rtc_read) {
    VfsNode* rtc_dev = vfs_open("/dev/rtc");
    TEST_ASSERT(rtc_dev != nullptr, "/dev/rtc exists");
    TEST_ASSERT(rtc_dev->size == 8, "/dev/rtc size is 8 bytes (RtcTime)");

    uint8_t buf[8];
    buf_fill(buf, 0x00, sizeof(buf));

    size_t nread = rtc_dev->ops->read(rtc_dev, 0, sizeof(buf), buf);
    TEST_ASSERT(nread == sizeof(buf), "/dev/rtc read returns 8 bytes");
    TEST_PASS("read /dev/rtc");
}

KTEST(vfs, dev_rtc_read_offset_past) {
    VfsNode* rtc_dev = vfs_open("/dev/rtc");
    TEST_ASSERT(rtc_dev != nullptr, "/dev/rtc exists");

    uint8_t buf[4];
    size_t nread = rtc_dev->ops->read(rtc_dev, 8, sizeof(buf), buf);
    TEST_ASSERT(nread == 0, "read with offset >= size returns 0");
    TEST_PASS("read /dev/rtc past end returns 0");
}

KTEST(vfs, open_dev_zero) {
    VfsNode* zero_dev = vfs_open("/dev/zero");
    TEST_ASSERT(zero_dev != nullptr, "/dev/zero exists");
    TEST_ASSERT(zero_dev->type == NodeType::CHAR_DEVICE, "/dev/zero is CHAR_DEVICE");
    TEST_PASS("open /dev/zero");
}

KTEST(vfs, open_dev_console) {
    VfsNode* console_dev = vfs_open("/dev/console");
    TEST_ASSERT(console_dev != nullptr, "/dev/console exists");
    TEST_ASSERT(console_dev->type == NodeType::CHAR_DEVICE, "/dev/console is CHAR_DEVICE");
    TEST_PASS("open /dev/console");
}

KTEST(vfs, open_dev_rtc) {
    VfsNode* rtc_dev = vfs_open("/dev/rtc");
    TEST_ASSERT(rtc_dev != nullptr, "/dev/rtc exists");
    TEST_ASSERT(rtc_dev->type == NodeType::CHAR_DEVICE, "/dev/rtc is CHAR_DEVICE");
    TEST_PASS("open /dev/rtc");
}

KTEST(vfs, open_nonexistent) {
    VfsNode* node = vfs_open("/nonexistent/path");
    TEST_ASSERT(node == nullptr, "non-existent path returns nullptr");
    TEST_PASS("open non-existent path");
}

KTEST(vfs, open_empty) {
    VfsNode* node = vfs_open("");
    TEST_ASSERT(node != nullptr, "empty path resolves to root");
    TEST_ASSERT(node == vfs_root(), "empty path is root");
    TEST_PASS("open empty path");
}

KTEST(vfs, path_dot) {
    VfsNode* root = vfs_open("/.");
    TEST_ASSERT(root != nullptr, "/. resolves");
    TEST_ASSERT(root == vfs_root(), "/. is root");
    TEST_PASS("path with \".\"");
}

KTEST(vfs, path_dotdot) {
    VfsNode* dev = vfs_open("/dev");
    TEST_ASSERT(dev != nullptr, "/dev resolves");

    VfsNode* dotdot = vfs_open("/dev/..");
    TEST_ASSERT(dotdot != nullptr, "/dev/.. resolves");
    TEST_ASSERT(dotdot == vfs_root(), "/dev/.. is root");
    TEST_PASS("path with \"..\" from /dev");
}

KTEST(vfs, path_trailing_slash) {
    VfsNode* dev = vfs_open("/dev/");
    TEST_ASSERT(dev != nullptr, "/dev/ resolves");
    TEST_ASSERT(dev->type == NodeType::DIRECTORY, "/dev/ is a directory");
    TEST_PASS("path with trailing slash");
}

KTEST(vfs, mkdir_and_readdir) {
    VfsNode* root = vfs_root();
    TEST_ASSERT(root != nullptr, "root exists");

    VfsNode* new_dir = vfs_mkdir(root, "testdir");
    TEST_ASSERT(new_dir != nullptr, "mkdir(\"testdir\") succeeds");
    TEST_ASSERT(new_dir->type == NodeType::DIRECTORY, "new dir is DIRECTORY");
    TEST_ASSERT(new_dir->parent == root, "new dir parent is root");

    VfsNode* reopened = vfs_open("/testdir");
    TEST_ASSERT(reopened != nullptr, "vfs_open(\"/testdir\") finds it");
    TEST_ASSERT(reopened == new_dir, "reopened node matches mkdir result");

    VfsNode* entry0 = vfs_readdir(root, 0);
    TEST_ASSERT(entry0 != nullptr, "readdir returns at least one entry");
    bool found = false;
    size_t count = 0;
    VfsNode* child = root->children;
    while (child) {
        if (child == new_dir) found = true;
        ++count;
        child = child->next_sibling;
    }
    TEST_ASSERT(found, "new directory appears in children list");
    TEST_PASS("mkdir and readdir");
}

KTEST(vfs, mkdir_duplicate) {
    VfsNode* root = vfs_root();
    VfsNode* d1 = vfs_mkdir(root, "dupdir");
    TEST_ASSERT(d1 != nullptr, "first mkdir succeeds");

    VfsNode* d2 = vfs_mkdir(root, "dupdir");
    TEST_ASSERT(d2 == nullptr, "duplicate mkdir returns nullptr");
    TEST_PASS("mkdir duplicate rejected");
}

KTEST(vfs, mkdir_on_file_fails) {
    VfsNode* null_dev = vfs_open("/dev/null");
    TEST_ASSERT(null_dev != nullptr, "/dev/null exists");

    VfsNode* result = vfs_mkdir(null_dev, "fail");
    TEST_ASSERT(result == nullptr, "mkdir on CHAR_DEVICE returns nullptr");
    TEST_PASS("mkdir on non-directory fails");
}

KTEST(vfs, tmpfs_file_write_read) {
    VfsNode* root = vfs_root();

    // Create a test file via mkdir + file creation in tmpfs directory
    // Since tmpfs file creation is done via open callback with write,
    // we test by creating a child and writing to it directly
    VfsNode* dir = vfs_mkdir(root, "filerdir");
    TEST_ASSERT(dir != nullptr, "test dir created");

    VfsNode* file = create_node("test.txt", NodeType::FILE);
    TEST_ASSERT(file != nullptr, "test file node created");
    file->ops = &kTmpFileOps;
    dir_add_child(dir, file);

    const char* test_str = "Hello, VortexOS VFS!";
    size_t len = 22;  // strlen(test_str) = 22

    size_t nwritten = file->ops->write(file, 0, len, test_str);
    TEST_ASSERT(nwritten == len, "write returns full length");
    TEST_ASSERT(file->size == len, "file size updated after write");

    uint8_t read_buf[32];
    size_t nread = file->ops->read(file, 0, sizeof(read_buf), read_buf);
    TEST_ASSERT(nread == len, "read returns written length");
    TEST_ASSERT(buf_cmp(read_buf, reinterpret_cast<const uint8_t*>(test_str), len) == 0,
                "read data matches written data");
    TEST_PASS("tmpfs file write + read roundtrip");
}

KTEST(vfs, tmpfs_file_write_grow) {
    VfsNode* root = vfs_root();
    VfsNode* dir = vfs_mkdir(root, "growdir");
    TEST_ASSERT(dir != nullptr, "test dir created");

    VfsNode* file = create_node("grow.txt", NodeType::FILE);
    TEST_ASSERT(file != nullptr, "test file created");
    file->ops = &kTmpFileOps;
    dir_add_child(dir, file);

    // Write first block
    const char* part1 = "AAAABBBB";
    file->ops->write(file, 0, 8, part1);
    TEST_ASSERT(file->size == 8, "size is 8 after first write");

    // Write second block at offset 8 (append)
    const char* part2 = "CCCCDDDD";
    file->ops->write(file, 8, 8, part2);
    TEST_ASSERT(file->size == 16, "size is 16 after append");

    // Write at offset 4 (overwrite middle)
    const char* part3 = "1234";
    file->ops->write(file, 4, 4, part3);
    TEST_ASSERT(file->size == 16, "size unchanged after overwrite");

    // Verify
    uint8_t expected[16] = {
        'A','A','A','A','1','2','3','4',
        'C','C','C','C','D','D','D','D'
    };
    uint8_t buf[16];
    file->ops->read(file, 0, 16, buf);
    TEST_ASSERT(buf_cmp(buf, expected, 16) == 0, "data correct after partial overwrite");
    TEST_PASS("tmpfs file write with growth and overwrite");
}

KTEST(vfs, tmpfs_file_write_past_end_zero_fill) {
    VfsNode* root = vfs_root();
    VfsNode* dir = vfs_mkdir(root, "zerofill");
    TEST_ASSERT(dir != nullptr, "test dir created");

    VfsNode* file = create_node("zfill.txt", NodeType::FILE);
    TEST_ASSERT(file != nullptr, "test file created");
    file->ops = &kTmpFileOps;
    dir_add_child(dir, file);

    const char* data = "HELLO";
    file->ops->write(file, 10, 5, data);
    TEST_ASSERT(file->size == 15, "size is 15 after write at offset 10");

    // Bytes 0-9 should be zero-filled
    uint8_t buf[15];
    file->ops->read(file, 0, 15, buf);
    for (size_t i = 0; i < 10; ++i) {
        if (buf[i] != 0) {
            serial_write("  [FAIL] byte "); serial_write_dec(i);
            serial_write(" not zero-filled\n");
            return;
        }
    }
    TEST_ASSERT(buf_cmp(buf + 10, reinterpret_cast<const uint8_t*>(data), 5) == 0,
                "data at offset 10 matches");
    TEST_PASS("tmpfs file zero-fill on gap write");
}

KTEST(vfs, readdir_enumeration) {
    VfsNode* root = vfs_root();

    // Count existing children
    size_t before = 0;
    VfsNode* child = root->children;
    while (child) { ++before; child = child->next_sibling; }

    // Should have "dev" and any from previous tests
    VfsNode* entry = vfs_readdir(root, 0);
    TEST_ASSERT(entry != nullptr, "readdir(0) returns entry");

    // Add a dir and verify count increases
    vfs_mkdir(root, "readdirtest");
    size_t after = 0;
    child = root->children;
    while (child) { ++after; child = child->next_sibling; }
    TEST_ASSERT(after == before + 1, "child count increased by 1");
    TEST_PASS("readdir enumeration");
}

KTEST(vfs, devfs_children) {
    VfsNode* dev = vfs_open("/dev");
    TEST_ASSERT(dev != nullptr, "/dev exists");

    // Verify all four device nodes are children
    bool found_null    = false;
    bool found_zero    = false;
    bool found_console = false;
    bool found_rtc     = false;

    VfsNode* child = dev->children;
    while (child) {
        if (str_cmp(child->name, "null")    == 0) found_null    = true;
        if (str_cmp(child->name, "zero")    == 0) found_zero    = true;
        if (str_cmp(child->name, "console") == 0) found_console = true;
        if (str_cmp(child->name, "rtc")     == 0) found_rtc     = true;
        child = child->next_sibling;
    }

    TEST_ASSERT(found_null,    "/dev/null present");
    TEST_ASSERT(found_zero,    "/dev/zero present");
    TEST_ASSERT(found_console, "/dev/console present");
    TEST_ASSERT(found_rtc,     "/dev/rtc present");
    TEST_PASS("all four devfs children present");
}

KTEST(vfs, mount_traversal) {
    // Verify mount point traversal: /dev resolves to devfs root
    VfsNode* dev = vfs_open("/dev");
    TEST_ASSERT(dev != nullptr, "/dev resolves");
    // The dev node should have devfs dir ops (not tmpfs dir ops)
    TEST_ASSERT(dev->ops == &kDevDirOps, "/dev uses devfs ops");
    TEST_PASS("mount traversal works");
}

// ─── Test Runner ──────────────────────────────────────────────────────────────

struct TestCase {
    const char* name;
    void (*func)();
};

#define TEST_CASE(name) { #name, test_##name }

extern "C" void run_vfs_tests() {
    serial_write("\n[KTEST] ===== VFS Test Suite =====\n");

    vfs_init();

    TestCase tests[] = {
        TEST_CASE(vfs_init),
        TEST_CASE(vfs_root_properties),
        TEST_CASE(vfs_open_root),
        TEST_CASE(vfs_open_dev_null),
        TEST_CASE(vfs_dev_null_write_discard),
        TEST_CASE(vfs_dev_zero_read),
        TEST_CASE(vfs_dev_console_write),
        TEST_CASE(vfs_dev_rtc_read),
        TEST_CASE(vfs_dev_rtc_read_offset_past),
        TEST_CASE(vfs_open_dev_zero),
        TEST_CASE(vfs_open_dev_console),
        TEST_CASE(vfs_open_dev_rtc),
        TEST_CASE(vfs_open_nonexistent),
        TEST_CASE(vfs_open_empty),
        TEST_CASE(vfs_path_dot),
        TEST_CASE(vfs_path_dotdot),
        TEST_CASE(vfs_path_trailing_slash),
        TEST_CASE(vfs_devfs_children),
        TEST_CASE(vfs_mount_traversal),
        TEST_CASE(vfs_mkdir_and_readdir),
        TEST_CASE(vfs_mkdir_duplicate),
        TEST_CASE(vfs_mkdir_on_file_fails),
        TEST_CASE(vfs_tmpfs_file_write_read),
        TEST_CASE(vfs_tmpfs_file_write_grow),
        TEST_CASE(vfs_tmpfs_file_write_past_end_zero_fill),
        TEST_CASE(vfs_readdir_enumeration),
    };

    size_t num_tests = sizeof(tests) / sizeof(tests[0]);

    for (size_t i = 0; i < num_tests; ++i) {
        serial_write("[KTEST] Running: ");
        serial_write(tests[i].name);
        serial_write("\n");

        // Run test with a simple try/catch-like pattern
        // (no exceptions — just call and rely on test assertions)
        tests[i].func();
    }

    serial_write("\n[KTEST] ===== VFS Test Suite Complete =====\n");
}

} // namespace vfs_test
