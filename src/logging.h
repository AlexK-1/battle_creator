#include <stdio.h>
#include <stdbool.h>

#ifndef LOGGING_H
#define LOGGING_H

typedef enum {
    L_DEBUG = 0,
    L_INFO,
    L_JOIN,
    L_DISCONNECT,
    L_CHAT,
    L_QUESTION,
    L_INPUT,
    L_WARNING,
    L_ERROR,
    L_NONE
} LogType;

extern const char *log_prefixes[];

typedef struct {
    FILE *log_file;
    bool print_time;
    LogType stdout_log_level, file_log_level;
} LogConfig;

extern LogConfig log_conf;

void set_log_config(FILE *log_file, bool print_time, LogType stdout_log_level, LogType file_log_level);
void write_log(LogType type, const char *format, ...);

#endif // LOGGING_H
