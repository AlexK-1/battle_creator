#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include "logging.h"

#define MAX_LOG_MESSAGE_LEN 1024

const char *log_prefixes[] = {
    [L_DEBUG] = "[.] ",
    [L_INFO] = "[*] ",
    [L_JOIN] = "[+] ",
    [L_DISCONNECT] = "[-] ",
    [L_QUESTION] = "[?] ",
    [L_INPUT] = "> ",
    [L_WARNING] = "[!] ",
    [L_ERROR] = "ERROR! ",
    [L_NONE] = ""
};

LogConfig log_conf = {.print_time = false, .log_file = NULL, .stdout_log_level = L_INFO, .file_log_level = L_DEBUG};

void set_log_config(FILE *log_file, bool print_time, LogType stdout_log_level, LogType file_log_level) {
    log_conf.log_file = log_file;
    log_conf.print_time = print_time;
    log_conf.stdout_log_level = stdout_log_level;
    log_conf.file_log_level = file_log_level;
}

// Write a log message to console/file
void write_log(LogType type, const char *format, ...) {
    if (type == L_NONE) return;
    if (type < log_conf.stdout_log_level && type < log_conf.file_log_level && log_conf.log_file != NULL) return;

    char buf[MAX_LOG_MESSAGE_LEN];
    
    // Get time string
    time_t current_time = time(NULL);
    char *str_time = asctime(localtime(&current_time));
    char *c = strchr(str_time, '\n');
    *c = '\0';
    
    strcpy(buf, str_time);
    char *msg_type = buf+strlen(buf);

    sprintf(msg_type, "  %s", log_prefixes[type]);
    char *msg_text = buf+strlen(buf);
    
    va_list args;
    va_start(args, format);
    vsnprintf(msg_text, sizeof(buf) - (msg_text - buf), format, args);
    va_end(args);

    if (log_conf.log_file != NULL && type >= log_conf.file_log_level)
        fprintf(log_conf.log_file, "%s", buf);
    if (type >= log_conf.stdout_log_level)
        printf("%s", log_conf.print_time ? buf : msg_type+2);
}

