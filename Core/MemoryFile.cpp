/*
 * Tencent is pleased to support the open source community by making
 * MMKV available.
 *
 * Copyright (C) 2018 THL A29 Limited, a Tencent company.
 * All rights reserved.
 *
 * Licensed under the BSD 3-Clause License (the "License"); you may not use
 * this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *       https://opensource.org/licenses/BSD-3-Clause
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MemoryFile.h"

#ifndef MMKV_WIN32

#    include "InterProcessLock.h"
#    include "MMBuffer.h"
#    include "MMKVLog.h"
#    include "ScopedLock.hpp"
#    include <cerrno>
#    include <utility>
#    include <fcntl.h>
#    include <sys/mman.h>
#    include <sys/stat.h>
#    include <unistd.h>
#    include <sys/file.h>
#    include <dirent.h>
#    include <cstring>
#    include <unistd.h>

using namespace std;

namespace mmkv {

extern bool getFileSize(int fd, size_t &size);

#    ifdef MMKV_ANDROID
extern size_t ASharedMemory_getSize(int fd);
#    else
File::File(MMKVPath_t path, OpenFlag flag) : m_path(std::move(path)), m_fd(-1), m_flag(flag) {
    open();
}

MemoryFile::MemoryFile(MMKVPath_t path, size_t expectedCapacity, bool readOnly)
    : m_diskFile(std::move(path), readOnly ? OpenFlag::ReadOnly : (OpenFlag::ReadWrite | OpenFlag::Create))
    , m_ptr(nullptr), m_size(0), m_readOnly(readOnly)
{
    reloadFromFile(expectedCapacity);
}
#    endif // !defined(MMKV_ANDROID)

#    ifdef MMKV_IOS
void tryResetFileProtection(const string &path);
#    endif

static int OpenFlag2NativeFlag(OpenFlag flag) {
    int native = O_CLOEXEC;
    if ((flag & OpenFlagRWMask) == OpenFlag::ReadWrite) {
        native |= O_RDWR;
    } else if (flag & OpenFlag::ReadOnly) {
        native |= O_RDONLY;
    } else if (flag & OpenFlag::WriteOnly) {
        native |= O_WRONLY;
    }

    if (flag & OpenFlag::Create) {
        native |= O_CREAT;
    }
    if (flag & OpenFlag::Excel) {
        native |= O_EXCL;
    }
    if (flag & OpenFlag::Truncate) {
        native |= O_TRUNC;
    }
    return native;
}

bool File::open() {
#    ifdef MMKV_ANDROID
    if (m_fileType == MMFILE_TYPE_ASHMEM) {
        return isFileValid();
    }
#    endif
    if (isFileValid()) {
        return true;
    }
    m_fd = ::open(m_path.c_str(), OpenFlag2NativeFlag(m_flag), S_IRWXU);
    if (!isFileValid()) {
        MMKVError("fail to open [%s], flag %x, %d(%s)", m_path.c_str(), m_flag, errno, strerror(errno));
        return false;
    }
    MMKVInfo("open fd[%p], flag %x, %s", m_fd, m_flag, m_path.c_str());
    return true;
}

void File::close() {
    if (isFileValid()) {
        MMKVInfo("closing fd[%p], %s", m_fd, m_path.c_str());
        if (::close(m_fd) == 0) {
            m_fd = -1;
        } else {
            MMKVError("fail to close [%s], %d(%s)", m_path.c_str(), errno, strerror(errno));
        }
    }
}

size_t File::getActualFileSize() const {
#    ifdef MMKV_ANDROID
    if (m_fileType == MMFILE_TYPE_ASHMEM) {
        return ASharedMemory_getSize(m_fd);
    }
#    endif
    size_t size = 0;
    mmkv::getFileSize(m_fd, size);
    return size;
}

/**
 * 扩展文件
 * @param size 
 * @return 
 */
bool MemoryFile::truncate(size_t size) {
    if (!m_diskFile.isFileValid()) {
        return false;
    }
    if (size == m_size) {
        return true;
    }
    if (m_readOnly) {
        // truncate readonly file not allow
        return false;
    }
#    ifdef MMKV_ANDROID
    if (m_diskFile.m_fileType == MMFILE_TYPE_ASHMEM) {
        if (size > m_size) {
            MMKVError("ashmem %s reach size limit:%zu, consider configure with larger size", m_diskFile.m_path.c_str(), m_size);
        } else {
            MMKVInfo("no way to trim ashmem %s from %zu to smaller size %zu", m_diskFile.m_path.c_str(), m_size, size);
        }
        return false;
    }
#    endif // MMKV_ANDROID

    auto oldSize = m_size;
    m_size = size;
    // round up to (n * pagesize)
    if (m_size < DEFAULT_MMAP_SIZE || (m_size % DEFAULT_MMAP_SIZE != 0)) {
        m_size = ((m_size / DEFAULT_MMAP_SIZE) + 1) * DEFAULT_MMAP_SIZE;
    }
    //::ftruncate方法扩展或者截断文件，扩展文件是在末尾补0，方法返回0，等于成功，方法返回-1，等于失败
    if (::ftruncate(m_diskFile.m_fd, static_cast<off_t>(m_size)) != 0) {
        MMKVError("fail to truncate [%s] to size %zu, %s", m_diskFile.m_path.c_str(), m_size, strerror(errno));
        m_size = oldSize;
        return false;
    }
    //扩展文件成功
    if (m_size > oldSize) {
        //往扩展的文件部分写0
        if (!zeroFillFile(m_diskFile.m_fd, oldSize, m_size - oldSize)) {
            MMKVError("fail to zeroFile [%s] to size %zu, %s", m_diskFile.m_path.c_str(), m_size, strerror(errno));
            m_size = oldSize;
            //失败了就把文件恢复原貌
            // redo ftruncate to its previous size
            int status = ::ftruncate(m_diskFile.m_fd, static_cast<off_t>(m_size));
            if (status != 0) {
                MMKVError("failed to truncate back [%s] to size %zu, %s", m_diskFile.m_path.c_str(), m_size, strerror(errno));
            } else {
                MMKVError("success to truncate [%s] back to size %zu", m_diskFile.m_path.c_str(), m_size);
                MMKVError("after truncate, file size = %zu", getActualFileSize());
            }

            return false;
        }
    }

    if (m_ptr) {
        //释放 mmap 创建的内存映射，解除进程地址空间和文件的映射关系。
        //addr：要解除映射的起始地址（必须是 mmap 返回的地址）。
        //length：映射的大小（应该和 mmap 时的大小一致）。
        //成功返回0
        if (munmap(m_ptr, oldSize) != 0) {
            MMKVError("fail to munmap [%s], %s", m_diskFile.m_path.c_str(), strerror(errno));
        }
    }
    auto ret = mmap();
    if (!ret) {
        doCleanMemoryCache(true);
    }
    return ret;
}

bool MemoryFile::msync(SyncFlag syncFlag) {
    if (m_readOnly) {
        // there's no point in msync() readonly memory
        return true;
    }
    if (m_ptr) {
        auto ret = ::msync(m_ptr, m_size, syncFlag ? MS_SYNC : MS_ASYNC);
        if (ret == 0) {
            return true;
        }
        MMKVError("fail to msync [%s], %s", m_diskFile.m_path.c_str(), strerror(errno));
    }
    return false;
}

bool MemoryFile::mmap() {
    //将当前的m_ptr值保存到oldPtr，可能是上一次映射的地址或者是初始值
    auto oldPtr = m_ptr;
    //设置映射权限模式，MMKV_SINGLE_PROCESS MMKV_MULTI_PROCESS 都等于0 返回false 可读写
    auto mode = m_readOnly ? PROT_READ : (PROT_READ | PROT_WRITE);
    //调用mmap创建映射
    //m_ptr表示想要映射到的地址，可以传入nullptr让系统传入合适地址 
    // m_size表示映射的字节数 
    // MAP_SHARED表示映射区域对所有映射该文件的进程共享，写入修改会同步回文件 
    // m_diskFile.m_fd表示映射的文件 
    // offset表示映射文件偏移量
    m_ptr = (char *) ::mmap(m_ptr, m_size, mode, MAP_SHARED, m_diskFile.m_fd, 0);
    if (m_ptr == MAP_FAILED) {
        MMKVError("fail to mmap [%s], mode %x, %s", m_diskFile.m_path.c_str(), mode, strerror(errno));
        m_ptr = nullptr;
        return false;
    }
    MMKVInfo("mmap to address [%p], oldPtr [%p], [%s]", m_ptr, oldPtr, m_diskFile.m_path.c_str());
    return true;
}

void MemoryFile::reloadFromFile(size_t expectedCapacity) {
    //如果是android平台，且m_fileType为ashmen 那就不需要reload了
    //根据前面的代码提示，目前应该都不是MMFILE_TYPE_ASHMEM
    //那么什么时候用MMKVMode.MMKV_ASHMEM呢？
#    ifdef MMKV_ANDROID
    if (m_fileType == MMFILE_TYPE_ASHMEM) {
        return;
    }
    //结束条件编译的范围
#    endif
    //如果文件有效清除缓存
    if (isFileValid()) {
        MMKVWarning("calling reloadFromFile while the cache [%s] is still valid", m_diskFile.m_path.c_str());
        MMKV_ASSERT(0);
        doCleanMemoryCache(false);
    }

    if (!m_diskFile.open()) {
        MMKVError("fail to open:%s, %s", m_diskFile.m_path.c_str(), strerror(errno));
    } else {
        FileLock fileLock(m_diskFile.m_fd);
        InterProcessLock lock(&fileLock, SharedLockType);
        SCOPED_LOCK(&lock);

        mmkv::getFileSize(m_diskFile.m_fd, m_size);
        // 将文件大小对齐到页大小的整数倍，用 0 填充不足的部分
        //roundUp<size_t>(expectedCapacity, DEFAULT_MMAP_SIZE) 把 expectedCapacity 按 DEFAULT_MMAP_SIZE 进行对齐,
        //确保 expectedCapacity 是 DEFAULT_MMAP_SIZE 的整数倍，以优化内存映射。
        //std::max<size_t>(A, B)：返回 A 和 B 之间的较大值，保证 expectedSize 至少是 DEFAULT_MMAP_SIZE
        size_t expectedSize = std::max<size_t>(DEFAULT_MMAP_SIZE, roundUp<size_t>(expectedCapacity, DEFAULT_MMAP_SIZE));
        // round up to (n * pagesize)
        if (!m_readOnly && (m_size < expectedSize || (m_size % DEFAULT_MMAP_SIZE != 0))) {
            InterProcessLock exclusiveLock(&fileLock, ExclusiveLockType);
            SCOPED_LOCK(&exclusiveLock);

            size_t roundSize = ((m_size / DEFAULT_MMAP_SIZE) + 1) * DEFAULT_MMAP_SIZE;;
            roundSize = std::max<size_t>(expectedSize, roundSize);
            truncate(roundSize);
        } else {
            auto ret = mmap();
            if (!ret) {
                doCleanMemoryCache(true);
            }
        }
#    ifdef MMKV_IOS
        if (!m_readOnly) {
            tryResetFileProtection(m_diskFile.m_path);
        }
#    endif
    }
}

void MemoryFile::doCleanMemoryCache(bool forceClean) {
#    ifdef MMKV_ANDROID
    if (m_diskFile.m_fileType == MMFILE_TYPE_ASHMEM && !forceClean) {
        return;
    }
#    endif
    if (m_ptr && m_ptr != MAP_FAILED) {
        if (munmap(m_ptr, m_size) != 0) {
            MMKVError("fail to munmap [%s], %s", m_diskFile.m_path.c_str(), strerror(errno));
        }
    }
    m_ptr = nullptr;

    m_diskFile.close();
    m_size = 0;
}

bool isFileExist(const string &nsFilePath) {
    if (nsFilePath.empty()) {
        return false;
    }

    return access(nsFilePath.c_str(), F_OK) == 0;
}

#ifndef MMKV_APPLE
extern bool mkPath(const MMKVPath_t &str) {
    char *path = strdup(str.c_str());

    struct stat sb = {};
    bool done = false;
    char *slash = path;

    while (!done) {
        slash += strspn(slash, "/");
        slash += strcspn(slash, "/");

        done = (*slash == '\0');
        *slash = '\0';

        if (stat(path, &sb) != 0) {
            if (errno != ENOENT || mkdir(path, 0777) != 0) {
                MMKVWarning("%s : %s", path, strerror(errno));
                // there's report that some Android devices might not have access permission on parent dir
                if (done) {
                    free(path);
                    return false;
                }
                goto LContinue;
            }
        } else if (!S_ISDIR(sb.st_mode)) {
            MMKVWarning("%s: %s", path, strerror(ENOTDIR));
            free(path);
            return false;
        }
LContinue:
        *slash = '/';
    }
    free(path);

    return true;
}
#else
// avoid using so-called privacy API
extern bool mkPath(const MMKVPath_t &str) {
    auto path = [NSString stringWithUTF8String:str.c_str()];
    NSError *error = nil;
    auto ret = [[NSFileManager defaultManager] createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:&error];
    if (!ret) {
        MMKVWarning("%s", error.localizedDescription.UTF8String);
        return false;
    }
    return true;
}
#endif

MMBuffer *readWholeFile(const MMKVPath_t &path) {
    MMBuffer *buffer = nullptr;
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        auto fileLength = lseek(fd, 0, SEEK_END);
        if (fileLength > 0) {
            buffer = new MMBuffer(static_cast<size_t>(fileLength));
            lseek(fd, 0, SEEK_SET);
            auto readSize = read(fd, buffer->getPtr(), static_cast<size_t>(fileLength));
            if (readSize != -1) {
                //fileSize = readSize;
            } else {
                MMKVWarning("fail to read %s: %s", path.c_str(), strerror(errno));

                delete buffer;
                buffer = nullptr;
            }
        }
        close(fd);
    } else {
        MMKVWarning("fail to open %s: %s", path.c_str(), strerror(errno));
    }
    return buffer;
}

/**
 * 补零
 * @param fd 
 * @param startPos 
 * @param size 
 * @return 
 */
bool zeroFillFile(int fd, size_t startPos, size_t size) {
    if (fd < 0) {
        return false;
    }
    
    //lseek方法修改普通文件的指针位置，参数offset为偏移量，
    // whence为基准点：SEEK_SET：从文件开头计算偏移量
    //               SEEK_CUR：从当前指针位置计算偏移量
    //               SEEK_END：从文件末尾计算偏移量（一般 offset 设为负数）
    // 成功：返回新的文件偏移量（即新的读写位置）失败：返回 -1
    if (lseek(fd, static_cast<off_t>(startPos), SEEK_SET) < 0) {
        MMKVError("fail to lseek fd[%d], error:%s", fd, strerror(errno));
        return false;
    }
    //创建一个4096字节（4KB）的全0缓冲区，用于写入文件
    //静态的，每次方法调用不会重复创建
    static const char zeros[4096] = {};
    //按 4KB 写入，直到 size 小于 4KB
    while (size >= sizeof(zeros)) {
        if (write(fd, zeros, sizeof(zeros)) < 0) {
            MMKVError("fail to write fd[%d], error:%s", fd, strerror(errno));
            return false;
        }
        size -= sizeof(zeros);
    }
    //处理不足4KB的部分
    if (size > 0) {
        if (write(fd, zeros, size) < 0) {
            MMKVError("fail to write fd[%d], error:%s", fd, strerror(errno));
            return false;
        }
    }
    return true;
}

#ifndef MMKV_APPLE
bool getFileSize(int fd, size_t &size) {
    struct stat st = {};
    if (fstat(fd, &st) != -1) {
        size = (size_t) st.st_size;
        return true;
    }
    return false;
}
#else
// avoid using so-called privacy API
bool getFileSize(int fd, size_t &size) {
    auto cur = lseek(fd, 0, SEEK_CUR);
    if (cur == -1) {
        return false;
    }
    auto end = lseek(fd, 0, SEEK_END);
    if (end == -1) {
        return false;
    }
    size = (size_t) end;

    lseek(fd, cur, SEEK_SET);
    return true;
}
#endif

size_t getPageSize() {
    return static_cast<size_t>(getpagesize());
}

#ifndef MMKV_APPLE

static pair<MMKVPath_t, int> createUniqueTempFile(const char *prefix) {
    char path[PATH_MAX];
#ifdef MMKV_ANDROID
    snprintf(path, PATH_MAX, "%s/%s.XXXXXX", g_android_tmpDir.c_str(), prefix);
#else
    snprintf(path, PATH_MAX, "%s/%s.XXXXXX", P_tmpdir, prefix);
#endif

    auto fd = mkstemp(path);
    if (fd < 0) {
        MMKVError("fail to create unique temp file [%s], %d(%s)", path, errno, strerror(errno));
        return {"", fd};
    }
    MMKVDebug("create unique temp file [%s] with fd[%d]", path, fd);
    return {MMKVPath_t(path), fd};
}

#if !defined(MMKV_ANDROID) && !defined(MMKV_LINUX)

bool tryAtomicRename(const MMKVPath_t &srcPath, const MMKVPath_t &dstPath) {
    if (::rename(srcPath.c_str(), dstPath.c_str()) != 0) {
        MMKVError("fail to rename [%s] to [%s], %d(%s)", srcPath.c_str(), dstPath.c_str(), errno, strerror(errno));
        return false;
    }
    return true;
}

bool copyFileContent(const MMKVPath_t &srcPath, MMKVFileHandle_t dstFD, bool needTruncate) {
    if (dstFD < 0) {
        return false;
    }
    bool ret = false;
    File srcFile(srcPath, OpenFlag::ReadOnly);
    if (!srcFile.isFileValid()) {
        return false;
    }
    auto bufferSize = getPageSize();
    auto buffer = (char *) malloc(bufferSize);
    if (!buffer) {
        MMKVError("fail to malloc size %zu, %d(%s)", bufferSize, errno, strerror(errno));
        goto errorOut;
    }
    lseek(dstFD, 0, SEEK_SET);

    // the POSIX standard don't have sendfile()/fcopyfile() equivalent, do it the hard way
    while (true) {
        auto sizeRead = read(srcFile.getFd(), buffer, bufferSize);
        if (sizeRead < 0) {
            MMKVError("fail to read file [%s], %d(%s)", srcPath.c_str(), errno, strerror(errno));
            goto errorOut;
        }

        size_t totalWrite = 0;
        do {
            auto sizeWrite = write(dstFD, buffer + totalWrite, sizeRead - totalWrite);
            if (sizeWrite < 0) {
                MMKVError("fail to write fd [%d], %d(%s)", dstFD, errno, strerror(errno));
                goto errorOut;
            }
            totalWrite += sizeWrite;
        } while (totalWrite < sizeRead);

        if (sizeRead < bufferSize) {
            break;
        }
    }
    if (needTruncate) {
        size_t dstFileSize = 0;
        getFileSize(dstFD, dstFileSize);
        auto srcFileSize = srcFile.getActualFileSize();
        if ((dstFileSize != srcFileSize) && (::ftruncate(dstFD, static_cast<off_t>(srcFileSize)) != 0)) {
            MMKVError("fail to truncate [%d] to size [%zu], %d(%s)", dstFD, srcFileSize, errno, strerror(errno));
            goto errorOut;
        }
    }

    ret = true;
    MMKVInfo("copy content from %s to fd[%d] finish", srcPath.c_str(), dstFD);

errorOut:
    free(buffer);
    return ret;
}

#endif // !defined(MMKV_ANDROID) && !defined(MMKV_LINUX)

// copy to a temp file then rename it
// this is the best we can do under the POSIX standard
bool copyFile(const MMKVPath_t &srcPath, const MMKVPath_t &dstPath) {
    auto pair = createUniqueTempFile("MMKV");
    auto tmpFD = pair.second;
    auto &tmpPath = pair.first;
    if (tmpFD < 0) {
        return false;
    }

    bool renamed = false;
    if (copyFileContent(srcPath, tmpFD, false)) {
        MMKVInfo("copyfile [%s] to [%s]", srcPath.c_str(), tmpPath.c_str());
        renamed = tryAtomicRename(tmpPath, dstPath);
        if (!renamed) {
            MMKVInfo("rename fail, try copy file content instead.");
            if (copyFileContent(tmpPath, dstPath)) {
                renamed = true;
                ::unlink(tmpPath.c_str());
            }
        }
        if (renamed) {
            MMKVInfo("copyfile [%s] to [%s] finish.", srcPath.c_str(), dstPath.c_str());
        }
    }

    ::close(tmpFD);
    if (!renamed) {
        ::unlink(tmpPath.c_str());
    }
    return renamed;
}

bool copyFileContent(const MMKVPath_t &srcPath, const MMKVPath_t &dstPath) {
    File dstFile(dstPath, OpenFlag::WriteOnly | OpenFlag::Create | OpenFlag::Truncate);
    if (!dstFile.isFileValid()) {
        return false;
    }
    auto ret = copyFileContent(srcPath, dstFile.getFd(), false);
    if (!ret) {
        MMKVError("fail to copyfile(): target file %s", dstPath.c_str());
    } else {
        MMKVInfo("copy content from %s to [%s] finish", srcPath.c_str(), dstPath.c_str());
    }
    return ret;
}

bool copyFileContent(const MMKVPath_t &srcPath, MMKVFileHandle_t dstFD) {
    return copyFileContent(srcPath, dstFD, true);
}

#endif // !defined(MMKV_APPLE)

void walkInDir(const MMKVPath_t &dirPath, WalkType type, const function<void(const MMKVPath_t&, WalkType)> &walker) {
    auto folderPathStr = dirPath.data();
    DIR *dir = opendir(folderPathStr);
    if (!dir) {
        MMKVError("opendir failed: %d(%s), %s", errno, strerror(errno), dirPath.c_str());
        return;
    }

    char childPath[PATH_MAX];
    size_t folderPathLength = dirPath.size();
    strncpy(childPath, folderPathStr, folderPathLength + 1);
    if (folderPathStr[folderPathLength - 1] != '/') {
        childPath[folderPathLength] = '/';
        folderPathLength++;
    }

    while (auto child = readdir(dir)) {
        if ((child->d_type & DT_REG) && (type & WalkFile)) {
#if defined(_DIRENT_HAVE_D_NAMLEN) || defined(__APPLE__)
            stpcpy(childPath + folderPathLength, child->d_name);
            childPath[folderPathLength + child->d_namlen] = 0;
#else
            strcpy(childPath + folderPathLength, child->d_name);
#endif
            walker(childPath, WalkFile);
        } else if ((child->d_type & DT_DIR) && (type & WalkFolder)) {
#if defined(_DIRENT_HAVE_D_NAMLEN) || defined(__APPLE__)
            if ((child->d_namlen == 1 && child->d_name[0] == '.') ||
                (child->d_namlen == 2 && child->d_name[0] == '.' && child->d_name[1] == '.')) {
                continue;
            }
            stpcpy(childPath + folderPathLength, child->d_name);
            childPath[folderPathLength + child->d_namlen] = 0;
#else
            if (strcmp(child->d_name, ".") == 0 || strcmp(child->d_name, "..") == 0) {
                continue;
            }
            strcpy(childPath + folderPathLength, child->d_name);
#endif
            walker(childPath, WalkFolder);
        }
    }

    closedir(dir);
}

} // namespace mmkv

#endif // !defined(MMKV_WIN32)
