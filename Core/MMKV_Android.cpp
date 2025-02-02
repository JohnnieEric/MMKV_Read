/*
 * Tencent is pleased to support the open source community by making
 * MMKV available.
 *
 * Copyright (C) 2019 THL A29 Limited, a Tencent company.
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

#include "MMKV.h"
// #include <bits/alltypes.h>

#ifdef MMKV_ANDROID

#    include "InterProcessLock.h"
#    include "KeyValueHolder.h"
#    include "MMKVLog.h"
#    include "MMKVMetaInfo.hpp"
#    include "MemoryFile.h"
#    include "ScopedLock.hpp"
#    include "ThreadLock.h"
#    include <unistd.h>
#    include "MMKV_IO.h"

using namespace std;
using namespace mmkv;

extern unordered_map<string, MMKV *> *g_instanceDic;
extern ThreadLock *g_instanceLock;

/**
 * MMKV_android构造方法
 * @param mmapID 
 * @param size 
 * @param mode 
 * @param cryptKey 
 * @param rootPath 
 * @param expectedCapacity 
 */
MMKV::MMKV(const string &mmapID, int size, MMKVMode mode, const string *cryptKey, const string *rootPath, size_t expectedCapacity)
    : m_mmapID(mmapID)
    , m_mode(mode)
    , m_path(mappedKVPathWithID(m_mmapID, mode, rootPath))
    , m_crcPath(crcPathWithID(m_mmapID, mode, rootPath))
    , m_dic(nullptr)
    , m_dicCrypt(nullptr)
    , m_expectedCapacity(std::max<size_t>(DEFAULT_MMAP_SIZE, roundUp<size_t>(expectedCapacity, DEFAULT_MMAP_SIZE)))
    //(mode & MMKV_ASHMEM) ? MMFILE_TYPE_ASHMEM : MMFILE_TYPE_FILE 这里mode如果填的是MMKV_SINGLE_PROCESS或者是MMKV_MULTI_PROCESS，
    //最后fileType都是MMFILE_TYPE_FILE  这是为什么？ASHMEN机制呢？？？？
    , m_file(new MemoryFile(m_path, size, (mode & MMKV_ASHMEM) ? MMFILE_TYPE_ASHMEM : MMFILE_TYPE_FILE, m_expectedCapacity, isReadOnly()))
    , m_metaFile(new MemoryFile(m_crcPath, DEFAULT_MMAP_SIZE, m_file->m_fileType, 0, isReadOnly()))
    , m_metaInfo(new MMKVMetaInfo())
    , m_crypter(nullptr)
    , m_lock(new ThreadLock())
    , m_fileLock(new FileLock(m_metaFile->getFd(), (mode & MMKV_ASHMEM)))
    , m_sharedProcessLock(new InterProcessLock(m_fileLock, SharedLockType))
    , m_exclusiveProcessLock(new InterProcessLock(m_fileLock, ExclusiveLockType)) {
    m_actualSize = 0;
    m_output = nullptr;

    // force use fcntl(), otherwise will conflict with MemoryFile::reloadFromFile()
    m_fileModeLock = new FileLock(m_file->getFd(), true);
    m_sharedProcessModeLock = new InterProcessLock(m_fileModeLock, SharedLockType);
    m_exclusiveProcessModeLock = nullptr;

#    ifndef MMKV_DISABLE_CRYPT
    if (cryptKey && cryptKey->length() > 0) {
        m_dicCrypt = new MMKVMapCrypt();
        m_crypter = new AESCrypt(cryptKey->data(), cryptKey->length());
    } else
#    endif
    {
        m_dic = new MMKVMap();
    }

    m_needLoadFromFile = true;
    m_hasFullWriteback = false;

    m_crcDigest = 0;

    m_sharedProcessLock->m_enable = isMultiProcess();
    m_exclusiveProcessLock->m_enable = isMultiProcess();

    // sensitive zone
    /*{
        SCOPED_LOCK(m_sharedProcessLock);
        loadFromFile();
    }*/
}

MMKV::MMKV(const string &mmapID, int ashmemFD, int ashmemMetaFD, const string *cryptKey)
    : m_mmapID(mmapID)
    , m_mode(MMKV_ASHMEM)
    , m_path(mappedKVPathWithID(m_mmapID, MMKV_ASHMEM, nullptr))
    , m_crcPath(crcPathWithID(m_mmapID, MMKV_ASHMEM, nullptr))
    , m_dic(nullptr)
    , m_dicCrypt(nullptr)
    , m_file(new MemoryFile(ashmemFD))
    , m_metaFile(new MemoryFile(ashmemMetaFD))
    , m_metaInfo(new MMKVMetaInfo())
    , m_crypter(nullptr)
    , m_lock(new ThreadLock())
    , m_fileLock(new FileLock(m_metaFile->getFd(), true))
    , m_sharedProcessLock(new InterProcessLock(m_fileLock, SharedLockType))
    , m_exclusiveProcessLock(new InterProcessLock(m_fileLock, ExclusiveLockType)) {

    m_actualSize = 0;
    m_output = nullptr;

    // force use fcntl(), otherwise will conflict with MemoryFile::reloadFromFile()
    m_fileModeLock = new FileLock(m_file->getFd(), true);
    m_sharedProcessModeLock = new InterProcessLock(m_fileModeLock, SharedLockType);
    m_exclusiveProcessModeLock = nullptr;

#    ifndef MMKV_DISABLE_CRYPT
    if (cryptKey && cryptKey->length() > 0) {
        m_dicCrypt = new MMKVMapCrypt();
        m_crypter = new AESCrypt(cryptKey->data(), cryptKey->length());
    } else
#    endif
    {
        m_dic = new MMKVMap();
    }
    //初始化的时候设定需要loadFormFile，在第一次添加数值或者时候loadFormFile，保证仅仅loadFormFile一次
    m_needLoadFromFile = true;
    m_hasFullWriteback = false;

    m_crcDigest = 0;

    m_sharedProcessLock->m_enable = true;
    m_exclusiveProcessLock->m_enable = true;

    // sensitive zone
    /*{
        SCOPED_LOCK(m_sharedProcessLock);
        loadFromFile();
    }*/
}

/**
 * 
 * @param mmapID  "mmkv.default"
 * @param size PageSize
 * @param mode  进程
 * @param cryptKey  加密密钥
 * @param rootPath  
 * @param expectedCapacity 
 * @return 
 */
MMKV *MMKV::mmkvWithID(const string &mmapID, int size, MMKVMode mode, const string *cryptKey, const string *rootPath, size_t expectedCapacity) {
    if (mmapID.empty() || !g_instanceLock) {
        return nullptr;
    }
    //加锁
    SCOPED_LOCK(g_instanceLock);
    //根据mmapId和rootPath构成mmapKey
    auto mmapKey = mmapedKVKey(mmapID, rootPath);
    //在map中查找相关的mmkv
    auto itr = g_instanceDic->find(mmapKey);
    //找到了直接返回
    if (itr != g_instanceDic->end()) {
        MMKV *kv = itr->second;
        return kv;
    }
    //如果没有找到就构建MMKV并放入g_instanceDic内
    if (rootPath) {
        if (!isFileExist(*rootPath)) {
            if (!mkPath(*rootPath)) {
                return nullptr;
            }
        }
        MMKVInfo("prepare to load %s (id %s) from rootPath %s", mmapID.c_str(), mmapKey.c_str(), rootPath->c_str());
    }

    string realID;
    auto correctPath = mappedKVPathWithID(mmapID, mode, rootPath);
    //生成地址
    if ((mode & MMKV_BACKUP) || (rootPath && isFileExist(correctPath))) {
        // it's successfully migrated to the correct path by newer version of MMKV
        realID = mmapID;
    } else {
        // historically Android mistakenly use mmapKey as mmapID
        realID = mmapKey;
    }
    //构造函数
    auto kv = new MMKV(realID, size, mode, cryptKey, rootPath, expectedCapacity);
    kv->m_mmapKey = mmapKey;
    //放入Map g_instanceDic内
    (*g_instanceDic)[mmapKey] = kv;
    return kv;
}

MMKV *MMKV::mmkvWithAshmemFD(const string &mmapID, int fd, int metaFD, const string *cryptKey) {

    if (fd < 0 || !g_instanceLock) {
        return nullptr;
    }
    SCOPED_LOCK(g_instanceLock);

    auto itr = g_instanceDic->find(mmapID);
    if (itr != g_instanceDic->end()) {
        MMKV *kv = itr->second;
#    ifndef MMKV_DISABLE_CRYPT
        kv->checkReSetCryptKey(fd, metaFD, cryptKey);
#    endif
        return kv;
    }
    auto kv = new MMKV(mmapID, fd, metaFD, cryptKey);
    kv->m_mmapKey = mmapID;
    (*g_instanceDic)[mmapID] = kv;
    return kv;
}

int MMKV::ashmemFD() {
    return (m_file->m_fileType & mmkv::MMFILE_TYPE_ASHMEM) ? m_file->getFd() : -1;
}

int MMKV::ashmemMetaFD() {
    return (m_file->m_fileType & mmkv::MMFILE_TYPE_ASHMEM) ? m_metaFile->getFd() : -1;
}

#    ifndef MMKV_DISABLE_CRYPT
void MMKV::checkReSetCryptKey(int fd, int metaFD, const string *cryptKey) {
    SCOPED_LOCK(m_lock);

    checkReSetCryptKey(cryptKey);

    if (m_file->m_fileType & MMFILE_TYPE_ASHMEM) {
        if (m_file->getFd() != fd) {
            ::close(fd);
        }
        if (m_metaFile->getFd() != metaFD) {
            ::close(metaFD);
        }
    }
}
#    endif // MMKV_DISABLE_CRYPT

/**
 * 检查进程
 * 
 * @return 
 */
bool MMKV::checkProcessMode() {
    // avoid exception on open() error
    if (!m_file->isFileValid()) {
        return true;
    }

    if (isMultiProcess()) {
        if (!m_exclusiveProcessModeLock) {
            //InterProcessLock 是一个用于 跨进程锁 的类，
            // 通常用于确保多个进程之间对共享资源的访问是互斥的。
            // 在多进程环境下，如果多个进程访问同一个资源或文件，
            // 可能会导致数据竞态（race conditions），从而引发不可预测的行为。
            // 为了避免这种情况，我们需要使用锁机制来控制进程间的同步和互斥。
            m_exclusiveProcessModeLock = new InterProcessLock(m_fileModeLock, ExclusiveLockType);
        }
        // avoid multiple processes get shared lock at the same time, https://github.com/Tencent/MMKV/issues/523
        auto tryAgain = false;
        //获取exclusive进程锁
        auto exclusiveLocked = m_exclusiveProcessModeLock->try_lock(&tryAgain);
        //获取成功返回
        if (exclusiveLocked) {
            return true;
        }
        //此时tryAgain的值是什么？？？
        //如果exclusive进程锁获取失败，获取shared进程锁
        auto shareLocked = m_sharedProcessModeLock->try_lock();
        if (!shareLocked) {
            // this call will fail on most case, just do it to make sure
            m_exclusiveProcessModeLock->try_lock();
            return true;
        } else {
            //获取shared进程锁成功，再次尝试获取exclusive进程锁
            //此时tryAgain的值是什么？？？
            if (!tryAgain) {
                // something wrong with the OS/filesystem, let's try again
                exclusiveLocked = m_exclusiveProcessModeLock->try_lock(&tryAgain);
                if (!exclusiveLocked && !tryAgain) {
                    // still something wrong, we have to give up and assume it passed the test
                    MMKVWarning("Got a shared lock, but fail to exclusive lock [%s], assume it's ok", m_mmapID.c_str());
                    exclusiveLocked = true;
                }
            }
            if (!exclusiveLocked) {
                MMKVError("Got a shared lock, but fail to exclusive lock [%s]", m_mmapID.c_str());
            }
            return exclusiveLocked;
        }
    } else {
        auto tryAgain = false;
        auto shareLocked = m_sharedProcessModeLock->try_lock(&tryAgain);
        if (!shareLocked && !tryAgain) {
            // something wrong with the OS/filesystem, we have to give up and assume it passed the test
            MMKVWarning("Fail to shared lock [%s], assume it's ok", m_mmapID.c_str());
            shareLocked = true;
        }
        if (!shareLocked) {
            MMKVError("Fail to share lock [%s]", m_mmapID.c_str());
        }
        return shareLocked;
    }
}

#endif // MMKV_ANDROID
