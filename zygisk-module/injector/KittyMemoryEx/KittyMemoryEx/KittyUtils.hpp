#pragma once

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <regex.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include <errno.h>
#include <inttypes.h>

#include <cstring>
#include <cstdint>
#include <cstdarg>

#include <string>
#include <sstream>
#include <iomanip>
#include <memory>
#include <algorithm>
#include <vector>
#include <utility>
#include <map>
#include <random>
#include <functional>
#include <mutex>
#include <cctype>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

inline size_t KTGetPageSize()
{
    static size_t pageSize = 0;
    if (pageSize == 0)
        pageSize = (sysconf(_SC_PAGE_SIZE));

    return pageSize;
}

#define KT_PAGE_SIZE (KTGetPageSize())
#define KT_PAGE_START(x) (uintptr_t(x) & ~(KT_PAGE_SIZE - 1))
#define KT_PAGE_END(x) (KT_PAGE_START(uintptr_t(x) + KT_PAGE_SIZE - 1))
#define KT_PAGE_OFFSET(x) (uintptr_t(x) - KT_PAGE_START(x))
#define KT_PAGE_LEN(x) (size_t(KT_PAGE_SIZE - KT_PAGE_OFFSET(x)))

#define KT_ALIGN_UP(ptr, align) (((uintptr_t)(ptr) + (align) - 1) & ~((align) - 1))
#define KT_ALIGN_DOWN(ptr, align) (((uintptr_t)(ptr)) & ~((uintptr_t)(align) - 1))

#if defined(__ANDROID__) && defined(kUSE_LOGCAT)

#include <android/log.h>
#define KITTY_LOG_TAG "KittyMemoryEx"

#ifdef kITTYMEMORY_DEBUG
#define KITTY_LOGD(fmt, ...) ((void)__android_log_print(ANDROID_LOG_DEBUG, KITTY_LOG_TAG, fmt, ##__VA_ARGS__))
#else
#define KITTY_LOGD(fmt, ...)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
    } while (0)
#endif

#define KITTY_LOGI(fmt, ...) ((void)__android_log_print(ANDROID_LOG_INFO, KITTY_LOG_TAG, fmt, ##__VA_ARGS__))
#define KITTY_LOGE(fmt, ...) ((void)__android_log_print(ANDROID_LOG_ERROR, KITTY_LOG_TAG, fmt, ##__VA_ARGS__))
#define KITTY_LOGW(fmt, ...) ((void)__android_log_print(ANDROID_LOG_WARN, KITTY_LOG_TAG, fmt, ##__VA_ARGS__))

#else

#ifdef kITTYMEMORY_DEBUG
#define KITTY_LOGD(fmt, ...) printf("D: " fmt "\n", ##__VA_ARGS__)
#else
#define KITTY_LOGD(fmt, ...)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
    } while (0)
#endif

#define KITTY_LOGI(fmt, ...) printf("I: " fmt "\n", ##__VA_ARGS__)
#define KITTY_LOGE(fmt, ...) fprintf(stderr, "E: " fmt "\n", ##__VA_ARGS__)
#define KITTY_LOGW(fmt, ...) printf("W: " fmt "\n", ##__VA_ARGS__)

#endif

#define KT_EINTR_RETRY(exp)                                                                                            \
    ({                                                                                                                 \
        __typeof__(exp) _rc;                                                                                           \
        do                                                                                                             \
        {                                                                                                              \
            _rc = (exp);                                                                                               \
        } while (_rc == -1 && errno == EINTR);                                                                         \
        _rc;                                                                                                           \
    })

#include <elf.h>
#ifdef __LP64__
#define KT_ELFCLASS_BITS 64
#define KT_ELF_EICLASS 2
#define KT_ElfW(x) Elf64_##x
#define KT_ELFW(x) ELF64_##x
#else
#define KT_ELFCLASS_BITS 32
#define KT_ELF_EICLASS 1
#define KT_ElfW(x) Elf32_##x
#define KT_ELFW(x) ELF32_##x
#endif
#define KT_ELF_ST_BIND(val) (((unsigned char)(val)) >> 4)
#define KT_ELF_ST_TYPE(val) ((val) & 0xf)
#define KT_ELF_ST_INFO(bind, type) (((bind) << 4) + ((type) & 0xf))
#define KT_ELF_ST_VISIBILITY(o) ((o) & 0x03)

namespace KittyUtils
{

#ifdef __ANDROID__


    namespace Android
    {
        inline int getUserId()
        {
            uid_t uid = getuid();
            return uid / 100000;
        }



        template <typename T>
        inline T getSystemProperty(const std::string &key, T defaultValue)
        {
            static_assert(
                std::is_same_v<T, std::string> || std::is_same_v<T, bool> || std::is_integral_v<T> ||
                    std::is_floating_point_v<T>,
                "getSystemProperty: unsupported type. Supported types: string, bool, integral, floating-point.");

            char value[PROP_VALUE_MAX] = {0};
            int len = __system_property_get(key.c_str(), value);
            if (len <= 0)
                return defaultValue;

            if constexpr (std::is_same_v<T, std::string>)
            {
                return std::string(value, len);
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                for (int i = 0; i < len; ++i)
                    value[i] = std::tolower(value[i]);

                if (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 || std::strcmp(value, "y") == 0 ||
                    std::strcmp(value, "yes") == 0)
                    return true;

                if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 || std::strcmp(value, "n") == 0 ||
                    std::strcmp(value, "no") == 0)
                    return false;

                return defaultValue;
            }
            else if constexpr (std::is_integral_v<T>)
            {
                char *end = nullptr;
                long long result = std::strtoll(value, &end, 0);
                if (end == value)
                    return defaultValue;
                return static_cast<T>(result);
            }
            else if constexpr (std::is_floating_point_v<T>)
            {
                char *end = nullptr;
                double result = std::strtod(value, &end);
                if (end == value)
                    return defaultValue;
                return static_cast<T>(result);
            }


            return defaultValue;
        }



        int getVersion();



        int getSDK();



        bool is64BitSupported();



        inline std::string getExternalStorage()
        {
            const char *storage = std::getenv("EXTERNAL_STORAGE");
            return (storage && storage[0] != '\0') ? storage : "/sdcard";
        }



        std::string getAppInternalDataDir(const std::string &packageName);



        std::string getAppInternalFilesDir(const std::string &packageName);



        std::string getAppInternalCacheDir(const std::string &packageName);



        inline std::string getAppExternalDataDir(const std::string &packageName)
        {
            return getExternalStorage() + "/Android/data/" + packageName;
        }



        inline std::string getAppExternalFilesDir(const std::string &packageName)
        {
            return getAppExternalDataDir(packageName) + "/files";
        }



        inline std::string getAppExternalCacheDir(const std::string &packageName)
        {
            return getAppExternalDataDir(packageName) + "/cache";
        }



        inline std::string getAppExternalMediaDir(const std::string &packageName)
        {
            return getExternalStorage() + "/Android/media/" + packageName;
        }



        inline std::string getAppObbDir(const std::string &packageName)
        {
            return getExternalStorage() + "/Android/obb/" + packageName;
        }
    }
#endif



    inline uintptr_t untagHeepPtr(uintptr_t p)
    {
#if defined(__LP64__) && defined(__ANDROID__)
        return (p & ((static_cast<uintptr_t>(1) << 56) - 1));
#else
        return p;
#endif
    }

    inline void *untagHeepPtr(void *p)
    {
        return reinterpret_cast<void *>(untagHeepPtr(uintptr_t(p)));
    }

    inline const void *untagHeepPtr(const void *p)
    {
        return reinterpret_cast<const void *>(untagHeepPtr(uintptr_t(p)));
    }



    namespace Path
    {


        std::string fileName(const std::string &filePath);



        std::string fileDirectory(const std::string &filePath);



        std::string fileExtension(const std::string &filePath);
    }



    namespace String
    {


        inline bool charEqualsIgnoreCase(char a, char b)
        {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        }



        bool startsWith(const std::string &str, const std::string &prefix, bool sensitive = true);



        bool startsWith(const std::string &str, const std::vector<std::string> &prefixes, bool sensitive = true);



        bool contains(const std::string &str, const std::string &substring, bool sensitive = true);



        bool contains(const std::string &str, const std::vector<std::string> &substrings, bool sensitive = true);



        bool endsWith(const std::string &str, const std::string &suffix, bool sensitive = true);



        bool endsWith(const std::string &str, const std::vector<std::string> &suffixes, bool sensitive = true);



        void trim(std::string &str);



        bool isValidHex(const std::string &hex);



        bool validateHex(std::string &hex);



        std::string fmt(const char *fmt, ...);
    }



    template <typename T>
    T randInt(T min, T max)
    {
        using param_type = typename std::uniform_int_distribution<T>::param_type;

        static std::mutex mtx;
        std::lock_guard<std::mutex> lock(mtx);

        static std::mt19937 gen{std::random_device{}()};

        std::uniform_int_distribution<T> dist;
        return dist(gen, param_type{min, max});
    }



    std::vector<uint8_t> randomBytes(std::size_t length);



    std::string randomString(size_t length);



    namespace Data
    {


        bool fromHex(std::string in, void *data);



        std::string toHex(const void *data, const size_t dataLength);



        template <typename T>
        std::string toHex(const T &data)
        {
            return toHex(&data, sizeof(T));
        }



        template <size_t rowSize = 8, bool showASCII = true>
        std::string hexDump(const void *address, size_t len)
        {
            if (!address || len == 0 || rowSize == 0)
                return "";

            const unsigned char *data = static_cast<const unsigned char *>(address);

            std::stringstream ss;
            ss << std::hex << std::uppercase << std::setfill('0');

            size_t i, j;

            for (i = 0; i < len; i += rowSize)
            {

                ss << std::setw(8) << i << ": ";


                for (j = 0; (j < rowSize) && ((i + j) < len); j++)
                    ss << std::setw(2) << static_cast<unsigned int>(data[i + j]) << " ";


                for (; j < rowSize; j++)
                    ss << "   ";


                if (showASCII)
                {
                    ss << " ";

                    for (j = 0; (j < rowSize) && ((i + j) < len); j++)
                    {
                        if (std::isprint(data[i + j]))
                            ss << data[i + j];
                        else
                            ss << '.';
                    }
                }

                ss << std::endl;
            }

            return ss.str();
        }
    }



    namespace Zip
    {


        struct ZipEntryInfo
        {
            std::string fileName;
            uint64_t compressedSize = 0;
            uint64_t uncompressedSize = 0;
            uint16_t compressionMethod = 0;
            uint32_t crc32 = 0;
            uint16_t modTime = 0;
            uint16_t modDate = 0;
            uint64_t dataOffset = 0;
        };



        struct ZipEntryMMap
        {
            void *mappingBase = nullptr;
            size_t mappingSize = 0;
            uint8_t *data = nullptr;
            uint64_t size = 0;
        };



        bool findCentralDirectory(const uint8_t *data, uint64_t fileSize, uint64_t *cdOffset, uint64_t *totalEntries);



        std::vector<ZipEntryInfo> listEntriesInZip(const std::string &zipPath);



        bool findEntryInfoByDataOffset(const std::string &zipPath, uint64_t dataOffset, ZipEntryInfo *out);



        bool mmapEntryByDataOffset(const std::string &zipPath, uint64_t dataOffset, ZipEntryMMap *out);
    }

}
