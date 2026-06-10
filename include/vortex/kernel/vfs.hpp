// SPDX-License-Identifier: GPL-2.0-or-later
// VortexOS Kernel — Virtual File System (VFS)
//
// Phase 4.1: VFS Abstraction Layer
//
// Design:
//   - VfsNode: Represents a file or directory
//   - VfsFilesystem: Represents a filesystem type (RAMFS, FAT32, etc)
//   - VfsMount: Represents a mounted instance
//
// Reference: task.md [Phase 4]

#pragma once

#include "vortex/types.hpp"

namespace vortex::kernel::vfs {

enum class NodeType {
    FILE,
    DIRECTORY,
    CHAR_DEVICE,
    BLOCK_DEVICE,
    PIPE,
    SOCKET
};

struct VfsNode;

struct VfsOps {
    size_t (*read)(VfsNode* node, size_t offset, size_t size, void* buffer);
    size_t (*write)(VfsNode* node, size_t offset, size_t size, const void* buffer);
    VfsNode* (*open)(VfsNode* node, const char* name);
    void (*close)(VfsNode* node);
    // TODO: Add readdir, mkdir, unlink, etc.
};

struct VfsNode {
    char name[256];
    NodeType type;
    size_t size;
    uint32_t uid, gid, permissions;
    
    VfsOps* ops;
    void* private_data; // FS-specific data
    
    VfsNode* parent;
    // For directories: linked list of children
    VfsNode* children;
    VfsNode* next_sibling;
};

// ─── API ────────────────────────────────────────────────────────────────────

void vfs_init();
VfsNode* vfs_root();
VfsNode* vfs_open(const char* path);

} // namespace vortex::kernel::vfs
