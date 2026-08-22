#pragma once

#include <cstdlib>
#include <string>
#include <unistd.h>
#include <functional>
#include <array>
#include <memory>
#include <cstdio>
#include <thread>
#include <chrono>

#include <sys/system_properties.h>
#include <sys/inotify.h>

#include <android/log.h>

namespace Utils
{
    bool android_launch_app(const std::string &pkg);

    bool android_stop_app(const std::string &pkg);

    bool android_restart_app(const std::string &pkg);

    bool inotify_watch_directory(int fd,
                                 const std::string &path,
                                 uint32_t mask,
                                 std::function<bool(int wd, struct inotify_event *event)> cb);
}

extern "C"
{
    struct logger_entry
    {
        uint16_t len;
        uint16_t hdr_size;
        int32_t pid;
        uint32_t tid;
        uint32_t sec;
        uint32_t nsec;
        uint32_t lid;
        uint32_t uid;
    };

#define LOGGER_ENTRY_MAX_LEN (5 * 1024)
    struct log_msg
    {
        union [[gnu::aligned(4)]]
        {
            unsigned char buf[LOGGER_ENTRY_MAX_LEN + 1];
            struct logger_entry entry;
        };
    };

    [[gnu::weak]] struct logger_list *android_logger_list_alloc(int mode, unsigned int tail, pid_t pid);
    [[gnu::weak]] void android_logger_list_free(struct logger_list *list);
    [[gnu::weak]] int android_logger_list_read(struct logger_list *list, struct log_msg *log_msg);
    [[gnu::weak]] struct logger *android_logger_open(struct logger_list *list, log_id_t id);

    typedef struct [[gnu::packed]]
    {
        int32_t tag;
    } android_event_header_t;

    typedef struct [[gnu::packed]]
    {
        int8_t type;
        int32_t data;
    } android_event_int_t;

    typedef struct [[gnu::packed]]
    {
        int8_t type;
        int32_t length;
        char data[];
    } android_event_string_t;

    typedef struct [[gnu::packed]]
    {
        int8_t type;
        int8_t element_count;
    } android_event_list_t;



    typedef struct [[gnu::packed]]
    {
        android_event_header_t tag;
        android_event_list_t list;
        android_event_int_t user;
        android_event_int_t pid;
        android_event_int_t uid;
        android_event_string_t process_name;


    } android_event_am_proc_start;
}

namespace Utils
{
    bool am_process_start_callback(std::function<void()> init_cb, std::function<bool(const android_event_am_proc_start *)> cb);
}
