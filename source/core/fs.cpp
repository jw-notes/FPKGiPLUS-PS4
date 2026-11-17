// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of FPKGi+. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

#include "core/fs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include <new>

#include "core/text.h"
#include "core/usb_ftp.h"

// The toolchain's <dirent.h> renames d_fileno to d_ino (musl's spelling), but
// its struct dirent, which matches the console's, calls the field d_fileno.
#if !defined(LIB_HOST) && defined(d_fileno)
#undef d_fileno
#endif

#ifdef LIB_HOST
#include <sys/statvfs.h>
#endif

namespace lib {

FILE* open_file(const std::string& path, const char* mode) {
    if (usb_ftp::handles(path))
        return mode[0] == 'w' || mode[0] == 'a' ? usb_ftp::open_write(path, mode[0] == 'a') : usb_ftp::open_read(path);
    return fopen(path.c_str(), mode);
}

bool rename_file(const std::string& from, const std::string& to) {
    if (usb_ftp::handles(from)) return usb_ftp::rename(from, to);
    return rename(from.c_str(), to.c_str()) == 0;
}

bool read_file(const std::string& path, std::string& out, uint64_t max_bytes) {
    out.clear();
    if (usb_ftp::handles(path)) return usb_ftp::read_all(path, out, max_bytes);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    // Download/resume runs on a small native thread stack. A 64 KB automatic
    // buffer consumes that whole stack before the first filesystem call.
    constexpr size_t capacity = 64 * 1024;
    std::unique_ptr<char[]> buffer(new (std::nothrow) char[capacity]);
    if (!buffer) { fclose(f); return false; }
    size_t n;
    while ((n = fread(buffer.get(), 1, capacity, f)) > 0) {
        out.append(buffer.get(), n);
        if (out.size() > max_bytes) {
            fclose(f);
            out.clear();
            return false;
        }
    }
    const bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool write_file_atomic(const std::string& path, const std::string& data) {
    const std::string temp = path + ".tmp";
    if (usb_ftp::handles(path)) return usb_ftp::write_all(temp, data) && usb_ftp::rename(temp, path);
    FILE* f = fopen(temp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fflush(f) == 0 && ok;
    ok = fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        unlink(temp.c_str());
        return false;
    }
    return rename(temp.c_str(), path.c_str()) == 0;
}

// On the console, stat() cannot be trusted: the toolchain's struct stat does
// not match the kernel's (st_size read 65984 for a 33 MB file).
// Folders are told apart by opendir, sizes are found by seeking to the end,
// and directory listings use readdir's d_type, whose layout does match.
#ifdef LIB_HOST

bool file_exists(const std::string& path) {
    if (usb_ftp::handles(path)) {
        bool found = false;
        usb_ftp::size(path, &found);
        return found;
    }
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool dir_exists(const std::string& path) {
    if (usb_ftp::handles(path)) return usb_ftp::is_dir(path);
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

uint64_t file_size(const std::string& path) {
    if (usb_ftp::handles(path)) return usb_ftp::size(path);
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
}

#else

bool dir_exists(const std::string& path) {
    if (usb_ftp::handles(path)) return usb_ftp::is_dir(path);
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;
    closedir(dir);
    return true;
}

bool file_exists(const std::string& path) {
    if (usb_ftp::handles(path)) {
        bool found = false;
        usb_ftp::size(path, &found);
        return found;
    }
    if (dir_exists(path)) return false;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

uint64_t file_size(const std::string& path) {
    if (usb_ftp::handles(path)) return usb_ftp::size(path);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    const bool ok = fseeko(f, 0, SEEK_END) == 0;
    const off_t size = ok ? ftello(f) : 0;
    fclose(f);
    return size > 0 ? (uint64_t)size : 0;
}

#endif

bool make_dirs(const std::string& path) {
    if (path.empty() || dir_exists(path)) return true;
    const std::string parent = parent_dir(path);
    if (!parent.empty() && parent != path && !make_dirs(parent)) return false;
    if (usb_ftp::handles(path)) return usb_ftp::make_dir(path) || usb_ftp::is_dir(path);
    return mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
}

bool remove_file(const std::string& path) {
    if (usb_ftp::handles(path)) return usb_ftp::remove_file(path);
    return unlink(path.c_str()) == 0;
}

bool remove_dir(const std::string& path) {
    if (usb_ftp::handles(path)) return usb_ftp::remove_dir(path);
    return rmdir(path.c_str()) == 0;
}

bool list_dir(const std::string& path, std::vector<DirEntry>& out, bool sizes) {
    out.clear();
    // Over FTP the listing carries sizes, so `sizes` costs nothing there.
    if (usb_ftp::handles(path)) return usb_ftp::list(path, out);
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;
    while (dirent* e = readdir(dir)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        DirEntry entry;
        entry.name = name;
        entry.inode = (uint64_t)e->d_fileno;
        const std::string full = join_path(path, name);
#ifdef LIB_HOST
        struct stat st;
        if (stat(full.c_str(), &st) == 0) {
            entry.is_dir = S_ISDIR(st.st_mode);
            entry.size = (uint64_t)st.st_size;
            entry.modified = (int64_t)st.st_mtime;
        } else {
            entry.is_dir = e->d_type == DT_DIR;
        }
#else
        // No modification times on the console (see above); sizes by seeking.
        entry.is_dir = e->d_type == DT_DIR || (e->d_type == DT_UNKNOWN && dir_exists(full));
        if (!entry.is_dir && sizes) entry.size = file_size(full);
#endif
        out.push_back(std::move(entry));
    }
    closedir(dir);
    return true;
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a.back() == '/' ? a + b : a + "/" + b;
}

std::string file_name(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string file_stem(const std::string& path) {
    const std::string name = file_name(path);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

std::string file_extension(const std::string& path) {
    const std::string name = file_name(path);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? std::string() : lower(name.substr(dot + 1));
}

std::string parent_dir(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return std::string();
    return slash == 0 ? "/" : path.substr(0, slash);
}

std::string safe_file_name(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
            c == '|' || (unsigned char)c < 32)
            out += '_';
        else
            out += c;
    }
    out = trim(out);
    while (!out.empty() && out.back() == '.') out.pop_back();
    if (out.size() > 120) out.resize(utf8_prefix(out, 100));
    return out.empty() ? "untitled" : out;
}

#ifdef LIB_HOST

namespace {
std::function<bool(const std::string&, uint64_t&)> g_fake_free_space;
}
void host_set_free_space(std::function<bool(const std::string& path, uint64_t& bytes)> fake) {
    g_fake_free_space = std::move(fake);
}

bool free_space(const std::string& path, uint64_t& bytes) {
    bytes = 0;
    if (usb_ftp::handles(path)) return false;
    if (g_fake_free_space) return g_fake_free_space(path, bytes);
    struct statvfs st;
    if (statvfs(path.c_str(), &st) != 0 || st.f_frsize == 0) return false;
    bytes = (uint64_t)st.f_bavail * st.f_frsize;
    return true;
}

#else

namespace {
// The toolchain's headers describe Linux's struct statfs, not the console
// kernel's, so the kernel's own layout (FreeBSD 9: version 0x20030518) is
// spelled out here and the system call is reached by its symbol.
struct KernelStatfs {
    uint32_t version, type;
    uint64_t flags, block_size, io_size, blocks, blocks_free;
    int64_t blocks_available;
    uint64_t files;
    int64_t files_free;
    uint64_t sync_writes, async_writes, sync_reads, async_reads, spare[10];
    uint32_t name_max, owner;
    int32_t fsid[2];
    char char_spare[80], type_name[16], mounted_from[88], mounted_on[88];
};
static_assert(sizeof(KernelStatfs) == 472, "FreeBSD 9 struct statfs");
}  // namespace

extern "C" int lib_kernel_statfs(const char* path, void* out) __asm__("statfs");

bool free_space(const std::string& path, uint64_t& bytes) {
    bytes = 0;
    if (usb_ftp::handles(path)) return false;
    // Room to spare, should a firmware's structure be longer than expected.
    union {
        KernelStatfs fs;
        unsigned char raw[1024];
    } answer;
    memset(&answer, 0, sizeof answer);
    if (lib_kernel_statfs(path.c_str(), &answer) != 0) return false;
    const KernelStatfs& fs = answer.fs;
    // An answer in any other layout is "unknown", never a wrong number.
    if (fs.version != 0x20030518u || fs.block_size == 0 || fs.block_size > (64u << 20) ||
        (fs.block_size & (fs.block_size - 1)) != 0 || fs.blocks == 0 || fs.blocks_free > fs.blocks ||
        fs.blocks > UINT64_MAX / fs.block_size)
        return false;
    // Negative when only the reserve is left: nothing is free for the app.
    const uint64_t available = fs.blocks_available > 0 ? (uint64_t)fs.blocks_available : 0;
    if (available > fs.blocks) return false;
    bytes = available * fs.block_size;
    return true;
}

#endif

}  // namespace lib
