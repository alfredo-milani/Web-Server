//
// Created by alfredo on 18/06/16.
//

#include <stdio.h>
#include <pthread.h>
#include <sys/param.h>
#include <unistd.h>
#include <errno.h>
#include <memory.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "structs.h"



// Log's file pointer
FILE *LOG = NULL;
// Pointer to html files; 1st: root, 2nd: 404, 3rd 400.
char *HTML[3];
// Port's number
int PORT = 11502;
// Minimun number of running threads
int MINTH = 10;
// Maximum number of active connection
int MAXCONN = 1000;
// Listening socket
int LISTENsd;
// Number of cached images
volatile int CACHE_N = -1;
// Path of images to share
char IMG_PATH[DIM / 2];
// tmp files
char tmp_resized[DIM2] = "/tmp/RESIZED.XXXXXX";
// tmp files cached
char tmp_cache[DIM2] = "/tmp/CACHE.XXXXXX";
// Usage string
char *usage_str = "Usage: %s\n"
        "\t\t\t[-l log's file path]\n"
        "\t\t\t[-p port]\n"
        "\t\t\t[-i image's path]\n"
        "\t\t\t[-t number_of_initial_threads]\n"
        "\t\t\t[-c maximum connection's number]\n"
        "\t\t\t[-r percentage to resize HTML images]\n"
        "\t\t\t[-n cache size (number of images)]\n"
        "\t\t\t[-h help]\n";
// User's command
char *user_command = "-Enter 'q'/'Q' to close the server, "
        "'s'/'S' to know server's state or "
        "'f'/'F' to force Log file write";

struct image *img;
struct th_sync thds;



// Used to write on a FILE *file, recording
//  information about all client
void write_on_stream(char *s, FILE *file) {
    size_t el;
    size_t len = strlen(s);

    while ((el = fwrite(s, sizeof(char), len, file)) < len) {
        if (ferror(file)) {
            fprintf(stderr, "Error in fwrite\n");
            exit(EXIT_FAILURE);
        }
        len -= el;
        s += el;
    }
}

// To close the process on error
void exit_on_error(char *s) {
    fprintf(stderr, "%s\n", s);
    exit(EXIT_FAILURE);
}

// Used to remove file from file system
void rm_link(char *path) {
    if (unlink(path)) {
        errno = 0;
        switch (errno) {
            case EBUSY:
                exit_on_error("File can not be unlinked: It is being use by the system\n");

            case EIO:
                exit_on_error("File can not be unlinked: An I/O error occurred\n");

            case ENAMETOOLONG:
                exit_on_error("File can not be unlinked: Pathname was too long\n");

            case ENOMEM:
                exit_on_error("File can not be unlinked: Insufficient kernel memory was available\n");

            case EPERM:
                exit_on_error("File can not be unlinked: The file system does not allow unlinking of files\n");

            case EROFS:
                exit_on_error("File can not be unlinked: Pathname refers to a file on a read-only file system\n");

            default:
                exit_on_error("File can not be unlinked: Error in unlink\n");
        }
    }
}

// Used to remove directory from file system
void rm_dir(char *directory) {
    DIR *dir;
    struct dirent *ent;

    fprintf(stdout, "-Removing '%s'\n", directory);
    char *verify = strrchr(directory, '/') + 1;
    if (!verify)
        exit_on_error("rm_dir: Unexpected error in strrchr\n");
    verify = strrchr(directory, '.') + 1;
    if (!strncmp(verify, "XXXXXX", 7))
        return;

    errno = 0;
    dir = opendir(directory);
    if (!dir) {
        if (errno == EACCES)
            exit_on_error("Permission denied\n");
        exit_on_error("rm_dir: Error in opendir\n");
    }

    while ((ent = readdir(dir)) != NULL) {
        if (ent -> d_type == DT_REG) {
            char buf[DIM];
            memset(buf, (int) '\0', DIM);
            sprintf(buf, "%s/%s", directory, ent -> d_name);
            rm_link(buf);
        }
    }

    if (closedir(dir))
        exit_on_error("Error in closedir\n");

    errno = 0;
    if (rmdir(directory)) {
        switch (errno) {
            case EBUSY:
                exit_on_error("Directory not removed: resource busy\n");

            case ENOMEM:
                exit_on_error("Directory not removed: Insufficient kernel memory\n");

            case EROFS:
                exit_on_error("Directory not removed: Pathname refers to a directory on a read-only file system\n");

            case ENOTEMPTY:
                exit_on_error("Directory not removed: Directory not empty!\n");

            default:
                exit_on_error("Error in rmdir\n");
        }
    }
}

// Used to free memory allocated from malloc/realloc funcitions
void free_mem() {
    free(HTML[0]);
    free(HTML[1]);
    free(HTML[2]);
    free(thds.clients);
    free(thds.new_c);
    if (CACHE_N >= 0 && thds.cache_hit_head && thds.cache_hit_tail) {
        struct cache_hit *to_be_removed;
        while (thds.cache_hit_tail) {
            to_be_removed = thds.cache_hit_tail;
            thds.cache_hit_tail = thds.cache_hit_tail->next_hit;
            free(to_be_removed);
        }
    }

    rm_dir(tmp_resized);
    rm_dir(tmp_cache);
}

// To close the process on error
void error_found(char *s) {
    fprintf(stderr, "%s", s);
    if (LOG)
        write_on_stream(s, LOG);

    free_mem();

    exit(EXIT_FAILURE);
}

// Used to block SIGPIPE sent from send function
void catch_signal(void) {
    struct sigaction sa;

    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) == -1)
        error_found("Error in sigaction\n");
}

// Used to get mutex to access a memory
//  area shared by multiple execution flows
void lock(pthread_mutex_t *m) {
    if (pthread_mutex_lock(m) != 0)
        error_found("Error in pthread_mutex_lock\n");
}

// Used to release mutex
void unlock(pthread_mutex_t *m) {
    if (pthread_mutex_unlock(m) != 0)
        error_found("Error in pthread_mutex_unlock\n");
}

// Used waiting for the occurrence of an event
void wait_t(pthread_cond_t *c, pthread_mutex_t *m) {
    if (pthread_cond_wait(c, m) != 0)
        error_found("Error in pthread_cond_wait\n");
}

// Used to send a signal to a thread
void signal_t(pthread_cond_t *c) {
    if (pthread_cond_signal(c) != 0)
        error_found("Error in pthread_cond_signal\n");
}

// Used to open a file from file system
FILE *open_file(const char *path) {
    errno = 0;
    char s[strlen(path) + 4 + 1];
    memset(s, (int) '\0', strlen(path) + 4 + 1);
    sprintf(s, "%s/LOG", path);
    FILE *f = fopen(s, "a");
    if (!f) {
        if (errno == EACCES)
            error_found("Missing permission\n");
        error_found("Error in fopen\n");
    }

    fprintf(stdout, "-File correctly created in: '%s'\n", s);

    return f;
}

// Used to map in memory HTML files which respond with
//  error 400 or error 404
void map_html_error(char *HTML[3]) {
    char *s = "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\"><html><head><title>%s</title></head><body><h1>%s</h1><p>%s</p></body></html>\0";
    size_t len = strlen(s) + 2 * DIM2 * sizeof(char);

    char *mm1 = malloc(len);
    char *mm2 = malloc(len);
    if (!mm1 || !mm2)
        error_found("Error in malloc\n");
    memset(mm1, (int) '\0', len); memset(mm2, (int) '\0', len);
    sprintf(mm1, s, "404 Not Found", "404 Not Found", "The requested URL was not found on this server.");
    sprintf(mm2, s, "400 Bad Request", "Bad Request", "Your browser sent a request that this server could not understand.");
    HTML[1] = mm1;
    HTML[2] = mm2;
}

// Used to get information from a file on the file system
//  check values: 1 for check directory
//                0 for check regular files
void get_info(struct stat *buf, char *path, int check) {
    memset(buf, (int) '\0', sizeof(struct stat));
    errno = 0;
    if (stat(path, buf) != 0) {
        if (errno == ENAMETOOLONG)
            error_found("Path too long\n");
        error_found("alloc_r_img: Invalid path\n");
    }
    if (check) {
        if (!S_ISDIR((*buf).st_mode)) {
            error_found("Argument -l: The path is not a directory!\n");
        }
    } else {
        if (!S_ISREG((*buf).st_mode)) {
            error_found("Non-regular files can not be analysed!\n");
        }
    }
}

// Used to get current time
char *get_time(void) {
    time_t now = time(NULL);
    char *k = malloc(sizeof(char) * DIM2);
    if (!k)
        error_found("Error in malloc\n");
    memset(k, (int) '\0', sizeof(char) * DIM2);
    strcpy(k, ctime(&now));
    if (!k)
        error_found("Error in ctime\n");
    if (k[strlen(k) - 1] == '\n')
        k[strlen(k) - 1] = '\0';
    return k;
}

// Used to write on LOG's file for logging session
void write_log(char *s) {
    char *t = get_time();
    write_on_stream(t, LOG);
    write_on_stream(s, LOG);
    free(t);
}

// Used to send HTTP messages to clients
ssize_t send_http_msg(int sd, char *s, ssize_t dim) {
    ssize_t sent = 0;
    char *msg = s;
    while (sent < dim) {
        sent = send(sd, msg, (size_t) dim, MSG_NOSIGNAL);

        if (sent <= 0)
            break;

        msg += sent;
        dim -= sent;
    }

    return sent;
}

// Used to get image from file system
char *get_img(char *name, size_t img_dim, char *directory) {
    ssize_t left = 0;
    int fd;
    char *buf;
    char path[strlen(name) + strlen(directory) + 1];
    memset(path, (int) '\0', strlen(name) + strlen(directory) + 1);
    sprintf(path, "%s/%s", directory, name);
    if (path[strlen(path)] != '\0')
        path[strlen(path)] = '\0';

    errno = 0;
    if ((fd = open(path, O_RDONLY)) == -1) {
        switch (errno) {
            case EACCES:
                fprintf(stderr, "get_img: Permission denied\n");
                break;

            case EISDIR:
                fprintf(stderr, "get_img: '%s' is a directory\n", name);
                break;

            case ENFILE:
                fprintf(stderr, "get_img: The maximum allowable number of files is currently open in the system\n");
                break;

            case EMFILE:
                fprintf(stderr, "get_img: File descriptors are currently open in the calling process\n");
                break;

            default:
                fprintf(stderr, "Error in get_img\n");
        }
        return NULL;
    }

    errno = 0;
    if (!(buf = malloc(img_dim))) {
        fprintf(stderr, "errno: %d\t\timg_dim: %d\tget_img: Error in malloc\n", errno, (int) img_dim);
        return buf;
    } else {
        memset(buf, (int) '\0', img_dim);
    }

    while ((left = read(fd, buf + left, img_dim)))
        img_dim -= left;

    if (close(fd)) {
        fprintf(stderr, "get_img: Error closing file\t\tFile Descriptor: %d\n", fd);
    }

    return buf;
}

void free_time_http(char *time, char *http) {
    free(time);
    free(http);
}

// Used to split HTTP message
void split_str(char *s, char **d) {
    char *msg_type[4];
    msg_type[0] = "Connection: ";
    msg_type[1] = "User-Agent: ";
    msg_type[2] = "Accept: ";
    msg_type[3] = "Cache-Control: ";
    // HTTP message type
    d[0] = strtok(s, " ");
    // Requested object
    d[1] = strtok(NULL, " ");
    // HTTP version
    d[2] = strtok(NULL, "\n");
    if (d[2]) {
        if (d[2][strlen(d[2]) - 1] == '\r')
            d[2][strlen(d[2]) - 1] = '\0';
    }
    char *k;
    while ((k = strtok(NULL, "\n"))) {
        // Connection type
        if (!strncmp(k, msg_type[0], strlen(msg_type[0]))) {
            d[3] = k + strlen(msg_type[0]);
            if (d[3][strlen(d[3]) - 1] == '\r')
                d[3][strlen(d[3]) - 1] = '\0';
        }
            // User-Agent type
        else if (!strncmp(k, msg_type[1], strlen(msg_type[1]))) {
            d[4] = k + strlen(msg_type[1]);
            if (d[4][strlen(d[4]) - 1] == '\r')
                d[4][strlen(d[4]) - 1] = '\0';
        }
            // Accept format
        else if (!strncmp(k, msg_type[2], strlen(msg_type[2]))) {
            d[5] = k + strlen(msg_type[2]);
            if (d[5][strlen(d[5]) - 1] == '\r')
                d[5][strlen(d[5]) - 1] = '\0';
        }
            // Cache-Control
        else if (!strncmp(k, msg_type[3], strlen(msg_type[3]))) {
            d[6] = k + strlen(msg_type[3]);
            if (d[6][strlen(d[6]) - 1] == '\r')
                d[6][strlen(d[6]) - 1] = '\0';
        }
    }
}

void create_th(void * (*routine) (void *), void *k) {
    pthread_t tid;
    errno = 0;
    if (pthread_create(&tid, NULL, routine, k) != 0) {
        if (errno == EAGAIN || errno == ENOMEM)
            error_found("Insufficient resources to create another thread\n");
        else
            error_found("Error in pthread_create\n");
    }
}

void usage(const char *p) {
    fprintf(stderr, usage_str, p);
    exit(EXIT_SUCCESS);
}

// Used to fill img dynamic structure
void alloc_r_img(struct image **h, char *path) {
    char new_path[DIM];
    memset(new_path, (int) '\0', DIM);
    struct image *k = malloc(sizeof(struct image));
    if (!k)
        error_found("Error in malloc\n");
    memset(k, (int) '\0', sizeof(struct image));

    char *name = strrchr(path, '/');
    if (!name) {
        if (!strncmp(path, "favicon.ico", 11)) {
            sprintf(new_path, "%s/%s", IMG_PATH, path);
            strcpy(k->name, path);
            path = new_path;
        } else {
            error_found("alloc_r_img: Error analyzing file");
        }
    } else {
        strcpy(k->name, ++name);
    }

    struct stat statbuf;
    get_info(&statbuf, path, 0);

    k->size_r = (size_t) statbuf.st_size;
    k->img_c = NULL;

    if (!*h) {
        k->next_img = *h;
        *h = k;
    } else {
        k->next_img = (*h)->next_img;
        (*h)->next_img = k;
    }
}

// Used to analyze user's options
void get_opt(int argc, char **argv, char **path, int *perc) {
    int i;
    for (i = 1; argv[i] != NULL; ++i)
        if (strcmp(argv[i], "-h") == 0)
            usage(argv[0]);

    int c; char *e; char mod_c = 0, mod_t = 0;
    struct stat statbuf;
    // Parsing the command line arguments
    // -p := port number;
    // -l := directory to store Log files;
    // -i := directory of files to send;
    // -t := minimum number of thread's pool;
    // -c := maximum number of connections.
    // -r := percentage of resized images which belong to HTML file
    while ((c = getopt(argc, argv, "p:l:i:c:t:r:n:")) != -1) {
        switch (c) {
            case 'p':
                if (strlen(optarg) > 5)
                    error_found("Argument -p: Port's number too high\n");

                errno = 0;
                int p_arg = (int) strtol(optarg, &e, 10);
                if (errno != 0 || *e != '\0')
                    error_found("Argument -p: Error in strtol: Invalid number port\n");
                if (p_arg > 65535)
                    error_found("Argument -p: Port's number too high\n");
                PORT = p_arg;
                break;

            case 'l':
                get_info(&statbuf, optarg, 1);

                if (optarg[strlen(optarg) - 1] != '/') {
                    strncpy(path[0], optarg, strlen(optarg));
                } else {
                    strncpy(path[0], optarg, strlen(optarg) - 1);
                }

                if (path[0][strlen(path[0])] != '\0')
                    path[0][strlen(path[0])] = '\0';
                break;

            case 'i':
                get_info(&statbuf, optarg, 1);

                if (optarg[strlen(optarg) - 1] != '/') {
                    strncpy(path[1], optarg, strlen(optarg));
                } else {
                    strncpy(path[1], optarg, strlen(optarg) - 1);
                }

                if (path[1][strlen(path[1])] != '\0')
                    path[1][strlen(path[1])] = '\0';
                break;

            case 't':
                errno = 0;
                int t_arg = (int) strtol(optarg, &e, 10);
                if (errno != 0 || *e != '\0')
                    error_found("Argument -t: Error in strtol: Invalid number\n");
                if (t_arg < 2)
                    error_found("Argument -t: Attention: due to performance problem, thread's numbers must be >= 2!\n");
                MINTH = t_arg;
                mod_t = 1;
                break;

            case 'c':
                errno = 0;
                int c_arg = (int) strtol(optarg, &e, 10);
                if (errno != 0 || *e != '\0')
                    error_found("Argument -c: Error in strtol: Invalid number\n");
                if (c_arg < 1)
                    error_found("Argument -c: Error: maximum connections' number must be > 0!");
                MAXCONN = c_arg;
                mod_c = 1;
                break;

            case 'r':
                errno = 0;
                *perc = (int) strtol(optarg, &e, 10);
                if (errno != 0 || *e != '\0')
                    error_found("Argument -r: Error in strtol: Invalid number\n");
                if (*perc < 1 || *perc > 100)
                    error_found("Argument -r: The number must be >=1 and <= 100\n");
                break;

            case 'n':
                errno = 0;
                int cache_size = (int) strtol(optarg, &e, 10);
                if (errno != 0 || *e != '\0')
                    error_found("Argument -n: Error in strtol: Invalid number\n");
                if (cache_size)
                    CACHE_N = cache_size;
                break;

            case '?':
                error_found("Invalid argument\n");

            default:
                error_found("Unknown error in getopt\n");
        }
    }
    if (mod_c && !mod_t && MAXCONN < MINTH)
        MINTH = MAXCONN;
    else if (mod_t && !mod_c && MINTH > MAXCONN)
        MAXCONN = MINTH;
    if (MINTH > MAXCONN)
        error_found("Error: number of maximum connections is lower then minimum number of the threads!\n");
}

// Used to dynamically fill the HTML file just created
void check_and_build(char *s, char **html, size_t *dim) {
    char *k = "<b>%s</b><br><br><a href=\"%s\"><img src=\"%s/%s\" height=\"130\" weight=\"100\"></a><br><br><br><br>";

    size_t len = strlen(*html);
    if (len + DIM >= *dim * DIM) {
        ++*dim;
        *html = realloc(*html, *dim * DIM);
        if (!*html)
            error_found("Check and build: Error in realloc\n");
        memset(*html + len, (int) '\0', *dim * DIM - len);
    }

    char *w;
    if (!(w = strrchr(tmp_resized, '/')))
        error_found("Unexpected error creating HTML root file\n");
    ++w;

    char *q = *html + len;
    sprintf(q, k, s, s, w, s);
}

/*
 * NOTE: "imagemagick" package required
 * */
// Used to build the dynamic structure and fill it with the images
//  in the current folder unless otherwise specified by the user.
void check_images(int perc) {
    DIR *dir;
    struct dirent *ent;
    char *k;

    errno = 0;
    dir = opendir(IMG_PATH);
    if (!dir) {
        if (errno == EACCES)
            error_found("Permission denied\n");
        error_found("check_images: Error in opendir\n");
    }

    size_t dim = 4;
    char *html = malloc((size_t) dim * DIM * sizeof(char));
    if (!html)
        error_found("Error in malloc\n");
    memset(html, (int) '\0', (size_t) dim * DIM * sizeof(char));

    // %s page's title; %s header; %s text.
    char *h = "<!DOCTYPE html><html><head><meta charset=\"utf-8\" /><title>%s</title><style type=\"text/css\"></style><script type=\"text/javascript\"></script></head><body background=\"\"><h1>%s</h1><br><br><h3>%s</h3><hr><br>";
    sprintf(html, h, "WebServerProject", "Welcome", "Select an image below");
    // %s image's path; %d resizing percentage
    char *convert = "convert %s -resize %d%% %s;exit";
    size_t len_h = strlen(html), new_len_h;

    struct image **i = &img;
    char input[DIM], output[DIM];
    memset(input, (int) '\0', DIM); memset(output, (int) '\0', DIM);

    fprintf(stdout, "-Please wait while resizing images...\n");
    while ((ent = readdir(dir)) != NULL) {
        if (ent -> d_type == DT_REG) {
            if (strrchr(ent -> d_name, '~')) {
                fprintf(stderr, "File '%s' was skipped\n", ent -> d_name);
                continue;
            }

            if (!strcmp(ent -> d_name, "favicon.ico")) {
                fprintf(stdout, "-favicon.ico was setted\n");
                alloc_r_img(i, ent -> d_name);
                i = &(*i) -> next_img;
                continue;
            } else {
                if ((k = strrchr(ent -> d_name, '.'))) {
                    if (strcmp(k, ".db") == 0) {
                        fprintf(stderr, "File '%s' was skipped\n", ent -> d_name);
                        continue;
                    }
                    if (strcmp(k, ".gif") != 0 && strcmp(k, ".GIF") != 0 &&
                        strcmp(k, ".jpg") != 0 && strcmp(k, ".JPG") != 0 &&
                        strcmp(k, ".png") != 0 && strcmp(k, ".PNG") != 0)
                        fprintf(stderr, "Warning: file '%s' may have an unsupported format\n", ent -> d_name);
                } else {
                    fprintf(stderr, "Warning: file '%s' may have an unsupported format\n", ent -> d_name);
                }
            }

            char command[DIM * 2];
            memset(command, (int) '\0', DIM * 2);
            sprintf(input, "%s/%s", IMG_PATH, ent -> d_name);
            sprintf(output, "%s/%s", tmp_resized, ent -> d_name);
            sprintf(command, convert, input, perc, output);

            /**
             * NOTE: "imagemagick" package required
            **/
            if (system(command))
                error_found("check_image: Error resizing images\n");

            alloc_r_img(i, output);
            i = &(*i) -> next_img;
            check_and_build(ent -> d_name, &html, &dim);
        }
    }

    new_len_h = strlen(html);
    if (len_h == new_len_h)
        error_found("There are no images in the specified directory\n");

    h = "</body></html>";
    if (new_len_h + DIM2 / 4 > dim * DIM) {
        ++dim;
        html = realloc(html, (size_t) dim * DIM);
        if (!html)
            error_found("Checking images: Error in realloc\n");
        memset(html + new_len_h, (int) '\0', (size_t) dim * DIM - new_len_h);
    }
    k = html;
    k += strlen(html);
    strcpy(k, h);

    HTML[0] = html;

    if (closedir(dir))
        error_found("Error in closedir\n");

    fprintf(stdout, "-Images correctly resized in: '%s' with percentage: %d%%\n", tmp_resized, perc);
}