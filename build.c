#ifdef _WIN32
    #include <windows.h>
    
    #define mkdir(file, ...) mkdir(file)
    #define _stat stat
#else
    #include <unistd.h>
    #include <dirent.h>
#endif
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifdef _WIN32
    #define PATH_SEP "\\"
#else
    #define PATH_SEP "/"
#endif

#ifndef CC
    #define CC "cc"
#endif

// #define DEBUG
// #define USE_WAYLAND_DISPLAY
// #define RAYLIB_PATH "/path/to/raylib"

#ifdef RAYLIB_PATH
    #define RAYLIB_INCLUDE "-I " RAYLIB_PATH PATH_SEP "src "
    #define RAYLIB_LINK "-L " RAYLIB_PATH PATH_SEP "src "
#else
    #define RAYLIB_INCLUDE ""
    #define RAYLIB_LINK ""
#endif

#define CFLAGS_COMMON "-std=c99 -Wall "
#ifdef DEBUG
    #define CFLAGS CFLAGS_COMMON "-Wextra -pedantic -g -fsanitize=address -DDEBUG "
#else
    #define CFLAGS CFLAGS_COMMON "-O2 "
#endif
#define CFLAGS_SERVER CFLAGS "-Iraylib"
#define CFLAGS_CLIENT CFLAGS RAYLIB_INCLUDE

#if defined(_WIN32)
    #ifdef DEBUG
        #define LDFLAGS ""
    #else
        #define LDFLAGS "-static -static-libgcc -s "
    #endif
    #define LDFLAGS_CLIENT LDFLAGS RAYLIB_LINK "-lraylib -lopengl32 -lgdi32 -lwinmm -lkernel32 -luser32 -lshell32 -lws2_32 "
    #define LDFLAGS_SERVER LDFLAGS
#elif defined(__linux__)
    #define LDFLAGS "-lm -lpthread "
    #define LDFLAGS_SERVER LDFLAGS
    #define LDFLAGS_CLIENT_COMMON LDFLAGS RAYLIB_LINK "-lraylib -lGL -ldl -lrt "
    #ifdef USE_WAYLAND_DISPLAY
        #define LDFLAGS_CLIENT LDFLAGS_CLIENT_COMMON "-lwayland-client -lwayland-cursor -lwayland-egl -lxkbcommon "
    #else
        #define LDFLAGS_CLIENT LDFLAGS_CLIENT_COMMON "-lX11 " /* "-lXrandr -lXinerama -lXi -lXxf86vm -lXcursor " */
    #endif
#elif defined(__APPLE__)
    #define LDFLAGS_SERVER ""
    #define LDFLAGS_CLIENT RAYLIB_LINK "-lraylib -framework OpenGL -framework Cocoa -framework IOKit -framework CoreAudio -framework CoreVideo "
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    #define LDFLAGS "-lm -lpthread "
    #define LDFLAGS_SERVER LDFLAGS
    #define LDFLAGS_CLIENT LDFLAGS RAYLIB_LINK "-lraylib -lGL -lX11 -lXrandr -lXinerama -lXi -lXxf86vm -lXcursor "
#endif

#define SRC_DIR "src" PATH_SEP
#define BUILD_DIR ".build" PATH_SEP

#ifdef _WIN32
    #define SERVER "server.exe"
    #define CLIENT "client.exe"
#else
    #define SERVER "server"
    #define CLIENT "client"
#endif

#define ERR(str)                                                              \
    do {                                                                      \
        fprintf(stderr, "%s: " str, prog);                                    \
        exit(EXIT_FAILURE);                                                   \
    } while (0)

#define ERRF(format, ...)                                                     \
    do {                                                                      \
        fprintf(stderr, "%s: " format, prog, __VA_ARGS__);                    \
        exit(EXIT_FAILURE);                                                   \
    } while (0)


#define FILE_NEWER(st1, st2) ((st1).st_mtime > (st2).st_mtime)

typedef enum {
    BUILD_OK = 0,
    BUILD_COMPILATION_ERR,
    BUILD_LINK_ERR
} BuildStatus;

void clean(void) {
    // Remove all .o files from BUILD_DIR
    #ifdef _WIN32
        HANDLE hFind;
        WIN32_FIND_DATA FindFileData;

        if ((hFind = FindFirstFile(BUILD_DIR "*.o", &FindFileData)) != INVALID_HANDLE_VALUE) {
            do {
                char *file = calloc(sizeof(BUILD_DIR) + strlen(FindFileData.cFileName), sizeof(*file));
                sprintf(file, BUILD_DIR "%s", FindFileData.cFileName);
                remove(file);
                free(file);
            } while (FindNextFile(hFind, &FindFileData));
            FindClose(hFind);
        }
    #else
        DIR *dr = opendir(BUILD_DIR);
        if (dr != NULL) {
            struct dirent *de;
            while ((de = readdir(dr)) != NULL) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                    continue;

                char *file = calloc(sizeof(BUILD_DIR) + strlen(de->d_name), sizeof(*file));
                sprintf(file, BUILD_DIR "%s", de->d_name);
                
                struct stat st;
                if (stat(file, &st) == 0 && !S_ISDIR(st.st_mode)) {
                    char *ext = strrchr(file, '.');
                    if (strcmp(ext, ".o") == 0)
                        remove(file);
                }
                free(file);
            }
            closedir(dr);
        }
    #endif

    remove(SERVER);
    remove(CLIENT);
}

int create_dir(char *dir_name) {
    char *buffer = strdup(dir_name);

    // Remove a / or \ from the end of the folder path
    int len = strlen(dir_name);
    if (dir_name[len-1] == '/' || dir_name[len-1] == '\\') {
        buffer[len-1] = '\0';
    }
    
    struct stat st;

    if (stat(buffer, &st) != 0) {
        int r = mkdir(buffer, 0755);
        free(buffer);
        return r;
    } else {
        free(buffer);
        return !S_ISDIR(st.st_mode);
    }
}

int compile_file(char *src_file, char **dependency_files, char *cflags, char *out) {
    struct stat st_src, st_obj;
    if (stat(src_file, &st_src) != 0) {
        printf("source file %s does not exist\n", src_file);
        return 1;
    }
    if (stat(out, &st_obj) == 0) {
        // Recompile an object file if source or one of dependency files is newer than it
        bool recompile = false;
        if (FILE_NEWER(st_src, st_obj)) {
            recompile = true;
        } else {
            for (char **dependency = dependency_files; *dependency != NULL; dependency++) {
                struct stat st_dep;
                if (stat(*dependency, &st_dep) != 0) {
                    printf("dependency file %s does not exist\n", *dependency);
                    return 1;
                }
                if (FILE_NEWER(st_dep, st_obj)) {
                    recompile = true;
                    break;
                }
            }
        }

        if (!recompile)
            return 0;
    }

    char format[] = CC " %s -c -o \"%s\" \"%s\"";
    char *buffer = calloc(sizeof(format) + strlen(cflags) + strlen(src_file) + strlen(out), sizeof(*buffer));
    sprintf(buffer, format, cflags, out, src_file);

    printf("$ %s\n", buffer);

    int r = system(buffer);
    free(buffer);
    return r;
}

int link_prog(char **obj_files, int files_count, char *ldflags, char *out) {
    int obj_files_len = 0;
    for (int i = 0; i < files_count; i++)
        obj_files_len += /*"*/ 1 + strlen(obj_files[i]) + /*"*/ 1 + /* */ 1;
    
    char format[] = CC " -o \"%s\" ";
    char *buffer = calloc(sizeof(format) + obj_files_len + strlen(out) + strlen(ldflags), sizeof(*buffer));
    sprintf(buffer, format, out);

    for (int i = 0; i < files_count; i++) {
        strcat(buffer, "\"");
        strcat(buffer, obj_files[i]);
        strcat(buffer, "\" ");
    }

    strcat(buffer, ldflags);

    printf("$ %s\n", buffer);

    int r = system(buffer);
    free(buffer);
    return r;
}

char *src_to_build_path(char *src_file) {
    char *buffer = calloc(strlen(src_file) - sizeof(SRC_DIR) + sizeof(BUILD_DIR) + 1, sizeof(*buffer));
    strcpy(buffer, BUILD_DIR);
    strcat(buffer, src_file + strlen(SRC_DIR));

    // Change file format to .o
    char *ext = strrchr(buffer, '.');
    *ext = '\0';
    strcat(buffer, ".o");
    
    return buffer;
}

BuildStatus build_prog(char **files, int files_count, char *cflags, char *ldflags, char *out) {
    printf("building %s...\n", out);
    
    char **obj_files = calloc(files_count, sizeof(*obj_files));
    int obj_files_count = 0;

    char **file = files;
    int idx = 0;
    bool prev_null = true;
    while (idx < files_count) {
        if (prev_null) {
            char *obj_file = src_to_build_path(*file);
            obj_files[obj_files_count++] = obj_file;
            if (compile_file(*file, file+1, cflags, obj_file) != 0) {
                for (int i = 0; i < obj_files_count; i++)
                    free(obj_files[i]);
                free(obj_files);
                printf("%s compilation error!\n", out);
                return BUILD_COMPILATION_ERR;
            }
            idx++;
        }
        prev_null = (*(file++) == NULL);
    }
    
    int link_res = link_prog(obj_files, files_count, ldflags, out);

    for (int j = 0; j < files_count; j++)
        free(obj_files[j]);
    free(obj_files);

    if (link_res == 0) {
        printf("%s built successfully!\n", out);
        return BUILD_OK;
    } else {
        printf("%s linkage error!\n", out);
        return BUILD_LINK_ERR;
    }

    return (link_res == 0)? BUILD_OK : BUILD_LINK_ERR;
}

int count_src_files(char **files) {
    int files_count = 0;
    char **file = files;
    bool prev_null = true;
    while (1) {
        if (*file == NULL) {
            if (prev_null)
                break;
            else
                files_count++;
        }
        prev_null = (*file == NULL);
        
        file++;
    }
    return files_count;
}

int main(int argc, char **argv) {
    char *prog = argv[0];
    bool build_server = false, build_client = false, clean_cmd = false, show_help = false;

    while (--argc) {
        char *arg = *(++argv);

        if (arg[0] == '-') {
            if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
                show_help = true;
                break;
            } else
                ERRF("unexpected argument '%s'\n", arg);
        } else {
            if (strcmp(arg, "server") == 0) {
                build_server = true;
            } else if (strcmp(arg, "client") == 0) {
                build_client = true;
            } else if (strcmp(arg, "all") == 0) {
                build_server = true;
                build_client = true;
            } else if (strcmp(arg, "clean") == 0) {
                clean_cmd = true;
            } else
                ERRF("unexpected argument '%s'\n", arg);
        }
    }

    if (show_help) {
        printf(
            "Usage: %s COMMAND...\n"
            "\n"
            "Args:\n"
            "  COMMAND\n"
            "    Target of build or command:\n"
            "    - server\n"
            "    - client\n"
            "    - all (server+client)\n"
            "    - clean\n"
            "\n"
            "Examples:\n"
            "  %s all # equivalent of '%s server client'\n"
            "  %s clean\n"
            "  %s clean all # recompile and link all .o files\n",
            prog, prog, prog, prog, prog);
        return 0;
    }

    if (!build_server && !build_client && !clean_cmd)    
        ERR("none of the build commands are specified\n");
    
    if (clean_cmd) {
        clean();
    }
    
    if (create_dir(BUILD_DIR) != 0)
        ERRF("error creating the %s directory\n", BUILD_DIR);
    
    /* FILES FORMAT
    char *files[] = {
        "src1.c", "dependency1.h" "dependency2.h", ..., NULL
        "src2.c", "dependency1.h", ..., NULL,
        "src3.c", "dependency2.h", ..., NULL,
        ...
        NULL
    };
    // files_count - number of source files
    */

    if (build_server) {
        char *server_files[] = {
            SRC_DIR "server.c",     SRC_DIR "boids.h", SRC_DIR "network.h", SRC_DIR "logging.h", SRC_DIR "queue.h", SRC_DIR "kdtree.h", NULL,
            SRC_DIR "boids.c",      SRC_DIR "boids.h", NULL,
            SRC_DIR "network.c",    SRC_DIR "network.h", SRC_DIR "boids.h", SRC_DIR "winsupport.h", NULL,
            SRC_DIR "logging.c",    SRC_DIR "logging.h", NULL,
            SRC_DIR "kdtree.c",     SRC_DIR "kdtree.h", SRC_DIR "boids.h", NULL,
            NULL
        };

        if (build_prog(server_files, count_src_files(server_files), CFLAGS_SERVER, CFLAGS LDFLAGS_SERVER, SERVER) != BUILD_OK)
            return 1;
    }

    if (build_client) {
        char *client_files[] = {
            SRC_DIR "client.c",     SRC_DIR "boids.h", SRC_DIR "network.h", SRC_DIR "logging.h", SRC_DIR "queue.h", SRC_DIR "kdtree.h", SRC_DIR "winsupport.h", NULL,
            SRC_DIR "boids.c",      SRC_DIR "boids.h", NULL,
            SRC_DIR "network.c",    SRC_DIR "network.h", SRC_DIR "boids.h", SRC_DIR "winsupport.h", NULL,
            SRC_DIR "logging.c",    SRC_DIR "logging.h", NULL,
            SRC_DIR "kdtree.c",     SRC_DIR "kdtree.h", SRC_DIR "boids.h", NULL,
            NULL
        };

        if (build_prog(client_files, count_src_files(client_files), CFLAGS_CLIENT, CFLAGS LDFLAGS_CLIENT, CLIENT) != BUILD_OK)
            return 1;
    }

    return 0;
}
