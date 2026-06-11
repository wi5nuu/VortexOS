// SPDX-License-Identifier: GPL-2.0-or-later
// === vortex/kernel/vfs.cpp ===
// VortexOS Kernel — Virtual File System (VFS) Implementation
//
// Phase 4.1: VFS core + tmpfs (rootfs) + devfs
//
// Provides:
//   - tmpfs: in-memory filesystem as root
//   - devfs: /dev/null, /dev/zero, /dev/console, /dev/rtc
//   - Path resolution (absolute/relative, . and .. support)
//   - Mount point traversal via mount table
//   - Directory operations (readdir, mkdir)
//   - File operations (read, write, open, close)
//
// Reference: task.md Phase 4.1, POSIX.1-2017 §12

#include "vortex/kernel/vfs.hpp"
#include "vortex/kernel/mm.hpp"
#include "vortex/kernel/heap.hpp"
#include "vortex/arch/x86_64/serial.hpp"

namespace vortex::kernel::vfs {

using arch::x86_64::serial_write;
using kernel::mm::PAGE_SIZE;
using kernel::mm::pmm_alloc_page;
using kernel::mm::pmm_get_hhdm_offset;
using PhysAddr = ::PhysAddr;
using kernel::heap::kmalloc;

// ─── String Helpers ──────────────────────────────────────────────────────────

size_t str_len(const char* s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}

int str_cmp(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (static_cast<unsigned char>(*a) -
            static_cast<unsigned char>(*b));
}

void str_cpy(char* dst, const char* src, size_t max_len) {
    size_t i = 0;
    while (src[i] && i < max_len - 1) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

// ─── Node Management ────────────────────────────────────────────────────────

VfsNode* create_node(const char* name, NodeType type) {
    VfsNode* node = static_cast<VfsNode*>(kmalloc(sizeof(VfsNode)));
    if (!node) return nullptr;

    str_cpy(node->name, name, sizeof(node->name));
    node->type = type;
    node->size = 0;
    node->uid = 0;
    node->gid = 0;
    node->permissions = 0755;
    node->ops = nullptr;
    node->private_data = nullptr;
    node->parent = nullptr;
    node->children = nullptr;
    node->next_sibling = nullptr;

    return node;
}

void dir_add_child(VfsNode* parent, VfsNode* child) {
    child->parent = parent;
    child->next_sibling = parent->children;
    parent->children = child;
}

// ─── tmpfs Operations ───────────────────────────────────────────────────────

static size_t tmpfs_file_read(VfsNode* node, size_t offset, size_t size, void* buffer) {
    if (!node || !buffer || offset >= node->size) return 0;

    size_t available = node->size - offset;
    size_t to_read = (size < available) ? size : available;

    const uint8_t* src = static_cast<const uint8_t*>(node->private_data) + offset;
    uint8_t* dst = static_cast<uint8_t*>(buffer);

    for (size_t i = 0; i < to_read; ++i) {
        dst[i] = src[i];
    }
    return to_read;
}

static size_t tmpfs_file_write(VfsNode* node, size_t offset, size_t size, const void* buffer) {
    if (!node || !buffer) return 0;

    if (!node->private_data) {
        uint64_t hhdm = pmm_get_hhdm_offset();
        PhysAddr page = pmm_alloc_page();
        if (page.raw() == 0) return 0;
        node->private_data = reinterpret_cast<void*>(page.raw() + hhdm);
    }

    size_t end = offset + size;
    if (end > PAGE_SIZE) {
        end = PAGE_SIZE;
        size = (offset < PAGE_SIZE) ? (PAGE_SIZE - offset) : 0;
    }

    if (offset > node->size) {
        uint8_t* data = static_cast<uint8_t*>(node->private_data);
        for (size_t i = node->size; i < offset; ++i) {
            data[i] = 0;
        }
    }

    const uint8_t* src = static_cast<const uint8_t*>(buffer);
    uint8_t* dst = static_cast<uint8_t*>(node->private_data) + offset;
    for (size_t i = 0; i < size; ++i) {
        dst[i] = src[i];
    }

    if (end > node->size) {
        node->size = end;
    }
    return size;
}

static VfsNode* tmpfs_dir_open(VfsNode* node, const char* name) {
    if (!node || !name) return nullptr;

    if (str_cmp(name, ".") == 0) return node;
    if (str_cmp(name, "..") == 0) {
        return node->parent ? node->parent : node;
    }

    VfsNode* child = node->children;
    while (child) {
        if (str_cmp(child->name, name) == 0) return child;
        child = child->next_sibling;
    }
    return nullptr;
}

static void tmpfs_node_close(VfsNode* node) {
    (void)node;
}

VfsOps kTmpDirOps = {
    .read = nullptr,
    .write = nullptr,
    .open = tmpfs_dir_open,
    .close = tmpfs_node_close
};

VfsOps kTmpFileOps = {
    .read = tmpfs_file_read,
    .write = tmpfs_file_write,
    .open = nullptr,
    .close = tmpfs_node_close
};

// ─── devfs Operations ───────────────────────────────────────────────────────

// /dev/null — reads return 0, writes discard
static size_t dev_null_read(VfsNode* node, size_t offset, size_t size, void* buffer) {
    (void)node; (void)offset; (void)buffer;
    return 0; // Always return EOF — null device provides no data
}

static size_t dev_null_write(VfsNode* node, size_t offset, size_t size, const void* buffer) {
    (void)node; (void)offset; (void)buffer;
    return size;
}

// /dev/zero — reads fill buffer with zeros, writes discard
static size_t dev_zero_read(VfsNode* node, size_t offset, size_t size, void* buffer) {
    (void)node; (void)offset;
    if (!buffer) return 0;
    uint8_t* dst = static_cast<uint8_t*>(buffer);
    for (size_t i = 0; i < size; ++i) {
        dst[i] = 0;
    }
    return size;
}

static size_t dev_zero_write(VfsNode* node, size_t offset, size_t size, const void* buffer) {
    (void)node; (void)offset; (void)buffer;
    return size;
}

// /dev/console — reads unsupported, writes to serial port
static size_t dev_console_read(VfsNode* node, size_t offset, size_t size, void* buffer) {
    (void)node; (void)offset; (void)buffer;
    return 0; // Console input not implemented yet (polled serial only)
}

static size_t dev_console_write(VfsNode* node, size_t offset, size_t size, const void* buffer) {
    (void)node; (void)offset;
    if (!buffer || size == 0) return 0;
    const char* str = static_cast<const char*>(buffer);
    serial_write(str);
    return size;
}

// /dev/rtc — returns a fixed timestamp until real CMOS driver exists
struct RtcTime {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  _pad;
};

static size_t dev_rtc_read(VfsNode* node, size_t offset, size_t size, void* buffer) {
    (void)node;
    if (!buffer || offset > 0) return 0;

    RtcTime time;
    time.year   = 2026;
    time.month  = 6;
    time.day    = 11;
    time.hour   = 12;
    time.minute = 0;
    time.second = 0;
    time._pad   = 0;

    size_t copy_size = (size < sizeof(RtcTime)) ? size : sizeof(RtcTime);
    uint8_t* dst = static_cast<uint8_t*>(buffer);
    const uint8_t* src = reinterpret_cast<const uint8_t*>(&time);
    for (size_t i = 0; i < copy_size; ++i) {
        dst[i] = src[i];
    }
    return copy_size;
}

static size_t dev_rtc_write(VfsNode* node, size_t offset, size_t size, const void* buffer) {
    (void)node; (void)offset; (void)buffer;
    return size;
}

static VfsNode* devfs_dir_open(VfsNode* node, const char* name) {
    if (!node || !name) return nullptr;

    if (str_cmp(name, ".") == 0) return node;
    if (str_cmp(name, "..") == 0) {
        return node->parent ? node->parent : node;
    }

    VfsNode* child = node->children;
    while (child) {
        if (str_cmp(child->name, name) == 0) return child;
        child = child->next_sibling;
    }
    return nullptr;
}

static void devfs_node_close(VfsNode* node) {
    (void)node;
}

VfsOps kDevDirOps = {
    .read = nullptr,
    .write = nullptr,
    .open = devfs_dir_open,
    .close = devfs_node_close
};

VfsOps kDevNullOps = {
    .read = dev_null_read,
    .write = dev_null_write,
    .open = nullptr,
    .close = devfs_node_close
};

VfsOps kDevZeroOps = {
    .read = dev_zero_read,
    .write = dev_zero_write,
    .open = nullptr,
    .close = devfs_node_close
};

VfsOps kDevConsoleOps = {
    .read = dev_console_read,
    .write = dev_console_write,
    .open = nullptr,
    .close = devfs_node_close
};

VfsOps kDevRtcOps = {
    .read = dev_rtc_read,
    .write = dev_rtc_write,
    .open = nullptr,
    .close = devfs_node_close
};

// ─── Mount Table ────────────────────────────────────────────────────────────

struct MountEntry {
    VfsNode*   mount_point;   // Directory node in parent fs
    VfsNode*   mounted_root;  // Root node of mounted fs
    MountEntry* next;
};

static MountEntry* kMountList = nullptr;

static bool mount_add(VfsNode* mount_point, VfsNode* mounted_root) {
    MountEntry* entry = static_cast<MountEntry*>(kmalloc(sizeof(MountEntry)));
    if (!entry) return false;

    entry->mount_point  = mount_point;
    entry->mounted_root = mounted_root;
    entry->next         = kMountList;
    kMountList          = entry;

    // Ensure ".." from mounted root resolves to the mount point
    mounted_root->parent = mount_point;

    return true;
}

static VfsNode* mount_resolve(VfsNode* node) {
    MountEntry* entry = kMountList;
    while (entry) {
        if (entry->mount_point == node) {
            return entry->mounted_root;
        }
        entry = entry->next;
    }
    return node;
}

// ─── Path Resolution ────────────────────────────────────────────────────────

static VfsNode* resolve_path(VfsNode* base, const char* path) {
    if (!base || !path) return nullptr;

    VfsNode* current = base;
    char component[256];

    while (*path) {
        while (*path == '/') ++path;
        if (*path == 0) break;

        size_t i = 0;
        while (path[i] && path[i] != '/' && i < sizeof(component) - 1) {
            component[i] = path[i];
            ++i;
        }
        component[i] = 0;
        path += i;

        if (str_cmp(component, ".") == 0) continue;

        if (str_cmp(component, "..") == 0) {
            if (current->parent) current = current->parent;
            continue;
        }

        if (!current->ops || !current->ops->open) return nullptr;

        VfsNode* next = current->ops->open(current, component);
        if (!next) return nullptr;

        current = mount_resolve(next);
    }

    return current;
}

// ─── Public API ─────────────────────────────────────────────────────────────

static VfsNode* kRootNode = nullptr;

void vfs_init() {
    serial_write("[VFS] Initializing Virtual File System...\n");

    // ── Create tmpfs root ──────────────────────────────────────
    kRootNode = create_node("/", NodeType::DIRECTORY);
    if (!kRootNode) {
        serial_write("[VFS] PANIC: Failed to create root node\n");
        return;
    }
    kRootNode->ops = &kTmpDirOps;
    kRootNode->parent = kRootNode;  // Root's parent is itself for ".."

    // ── Mount point: /dev ──────────────────────────────────────
    VfsNode* dev_mount_point = create_node("dev", NodeType::DIRECTORY);
    if (!dev_mount_point) {
        serial_write("[VFS] PANIC: Failed to create /dev mount point\n");
        return;
    }
    dev_mount_point->ops = &kTmpDirOps;
    dir_add_child(kRootNode, dev_mount_point);

    // ── devfs root (separate subtree with devfs ops) ──────────
    VfsNode* dev_root = create_node("dev", NodeType::DIRECTORY);
    if (!dev_root) {
        serial_write("[VFS] PANIC: Failed to create devfs root\n");
        return;
    }
    dev_root->ops = &kDevDirOps;

    // Children of devfs
    auto make_dev = [](const char* name, VfsOps* ops, size_t size, uint32_t perms) -> VfsNode* {
        VfsNode* n = create_node(name, NodeType::CHAR_DEVICE);
        if (!n) return nullptr;
        n->ops = ops;
        n->size = size;
        n->permissions = perms;
        return n;
    };

    VfsNode* null_dev    = make_dev("null",    &kDevNullOps,    0, 0666);
    VfsNode* zero_dev    = make_dev("zero",    &kDevZeroOps,    0, 0666);
    VfsNode* console_dev = make_dev("console", &kDevConsoleOps, 0, 0666);
    VfsNode* rtc_dev     = make_dev("rtc",     &kDevRtcOps,     sizeof(RtcTime), 0644);

    if (null_dev)    dir_add_child(dev_root, null_dev);
    if (zero_dev)    dir_add_child(dev_root, zero_dev);
    if (console_dev) dir_add_child(dev_root, console_dev);
    if (rtc_dev)     dir_add_child(dev_root, rtc_dev);

    // Register mount: /dev → devfs root
    mount_add(dev_mount_point, dev_root);

    serial_write("[VFS] Root filesystem: tmpfs\n");
    serial_write("[VFS] devfs mounted at /dev (null, zero, console, rtc)\n");
    serial_write("[VFS] Initialization complete\n");
}

VfsNode* vfs_root() {
    return kRootNode;
}

VfsNode* vfs_open(const char* path) {
    if (!path || !kRootNode) return nullptr;

    if (path[0] == '/') {
        return resolve_path(kRootNode, path + 1);
    }
    return resolve_path(kRootNode, path);
}

VfsNode* vfs_readdir(VfsNode* dir, size_t index) {
    if (!dir || dir->type != NodeType::DIRECTORY) return nullptr;

    VfsNode* child = dir->children;
    size_t i = 0;
    while (child) {
        if (i == index) return child;
        child = child->next_sibling;
        ++i;
    }
    return nullptr;
}

VfsNode* vfs_mkdir(VfsNode* parent, const char* name) {
    if (!parent || !name) return nullptr;
    if (parent->type != NodeType::DIRECTORY) return nullptr;

    if (parent->ops && parent->ops->open) {
        VfsNode* existing = parent->ops->open(parent, name);
        if (existing) return nullptr;
    }

    VfsNode* dir = create_node(name, NodeType::DIRECTORY);
    if (!dir) return nullptr;

    dir->ops = parent->ops;
    dir_add_child(parent, dir);
    return dir;
}

} // namespace vortex::kernel::vfs
