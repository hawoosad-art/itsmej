#pragma once

#include "KittyUtils.hpp"

#define KT_IO_BUFFER_SIZE ((size_t)(1024 * 1024))

class KittyIOFile
{
private:
    int _fd;
    std::string _filePath;
    int _flags;
    mode_t _mode;
    int _error;
    size_t _bufferSize;

public:
    KittyIOFile() : _fd(-1), _flags(0), _mode(0), _error(0), _bufferSize(KT_IO_BUFFER_SIZE)
    {
    }



    KittyIOFile(const std::string &filePath, int flags, mode_t mode)
        : _fd(-1), _filePath(filePath), _flags(flags), _mode(mode), _error(0), _bufferSize(KT_IO_BUFFER_SIZE)
    {
    }



    KittyIOFile(const std::string &filePath, int flags)
        : _fd(-1), _filePath(filePath), _flags(flags), _mode(0), _error(0), _bufferSize(KT_IO_BUFFER_SIZE)
    {
    }

    ~KittyIOFile()
    {
        if (_fd >= 0)
        {
            ::close(_fd);
        }
    }



    bool open();



    bool close();



    inline int lastError() const
    {
        return _error;
    }



    inline std::string lastStrError() const
    {
        return _error ? strerror(_error) : "";
    }



    inline size_t bufferSize() const
    {
        return _bufferSize;
    }



    inline void setBufferSize(size_t size)
    {
        _bufferSize = size;
    }



    inline int fd() const
    {
        return _fd;
    }



    inline std::string path() const
    {
        return _filePath;
    }



    inline int flags() const
    {
        return _flags;
    }



    inline mode_t mode() const
    {
        return _mode;
    }



    ssize_t read(void *buffer, size_t len);



    ssize_t write(const void *buffer, size_t len);



    ssize_t pread(uintptr_t offset, void *buffer, size_t len);



    ssize_t pwrite(uintptr_t offset, const void *buffer, size_t len);



    inline bool exists() const
    {
        return access(_filePath.c_str(), F_OK) != -1;
    }



    inline bool canRead() const
    {
        return access(_filePath.c_str(), R_OK) != -1;
    }



    inline bool canWrite() const
    {
        return access(_filePath.c_str(), W_OK) != -1;
    }



    inline bool canExecute() const
    {
        return access(_filePath.c_str(), X_OK) != -1;
    }



    inline bool remove()
    {
        _error = (unlink(_filePath.c_str()) == -1) ? errno : 0;
        return _error == 0;
    }

#ifdef __APPLE__


    inline struct stat info()
    {
        struct stat s = {};
        _error = (stat(_filePath.c_str(), &s) == -1) ? errno : 0;
        return s;
    }
#else


    inline struct stat64 info()
    {
        struct stat64 s = {};
        _error = (stat64(_filePath.c_str(), &s) == -1) ? errno : 0;
        return s;
    }
#endif



    inline bool isFile()
    {
        auto s = info();
        return _error == 0 && S_ISREG(s.st_mode);
    }



    bool readToString(std::string *str);



    bool readToBuffer(std::vector<char> *buf);



    bool writeOffsetToFile(uintptr_t offset, size_t len, const std::string &filePath);



    bool writeToFile(const std::string &filePath)
    {
        KittyIOFile f(filePath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        return f.open() && writeToFd(f.fd());
    }



    bool writeToFd(int fd);



    inline static bool readFileToString(const std::string &filePath, std::string *str)
    {
        KittyIOFile f(filePath, O_RDONLY | O_CLOEXEC);
        return f.open() && f.readToString(str);
    }



    inline static bool readFileToBuffer(const std::string &filePath, std::vector<char> *buf)
    {
        KittyIOFile f(filePath, O_RDONLY | O_CLOEXEC);
        return f.open() && f.readToBuffer(buf);
    }



    inline static bool copy(const std::string &srcFilePath, const std::string &dstFilePath)
    {
        KittyIOFile f(srcFilePath, O_RDONLY | O_CLOEXEC);
        return f.open() && f.writeToFile(dstFilePath);
    }



    static void listFilesCallback(const std::string &dir, std::function<bool(const std::string &)> cb);



    static bool createDirectoryRecursive(const std::string &path, mode_t mode = 0755);
};
