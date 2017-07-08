//
// Created by alfredo on 18/06/16.
//

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <dirent.h>

#include "functions.h"


// Find q factor from Accept header
// Return values: -1 --> error
//                -2 --> factor quality not specified in the header
// NOTE: This server DOES NOT consider the extensions of the images,
//  so this function will analyze the resource type and NOT the subtype.
int quality(char *h_accept) {
    double images, others, q;
    images = others = q = -2.0;
    char *chr, *t1 = strtok(h_accept, ",");
    if (!h_accept || !t1)
        return (int) (q *= 100);

    do {
        while (*t1 == ' ')
            ++t1;

        if (!strncmp(t1, "image", strlen("image"))) {
            chr = strrchr(t1, '=');
            // If not specified the 'q' value or if there was
            //  an error in transmission, the default
            //  value of 'q' is 1.0
            if (!chr) {
                images = 1.0;
                break;
            } else {
                errno = 0;
                double tmp = strtod(++chr, NULL);
                if (tmp > images)
                    images = tmp;
                if (errno != 0)
                    return -1;
            }
        } else if (!strncmp(t1, "*", strlen("*"))) {
            chr = strrchr(t1, '=');
            if (!chr) {
                others = 1.0;
            } else {
                errno = 0;
                others = strtod(++chr, NULL);
                if (errno != 0)
                    return -1;
            }
        }
    } while ((t1 = strtok(NULL, ",")));

    if (images > others || (others > images && images != -2.0))
        q = images;
    else if (others > images && images == -2.0)
        q = others;
    else
        fprintf(stderr, "string: %s\t\tquality: Unexpected error\n", h_accept);

    return (int) (q *= 100);
}

// Find and send resource for client
int data_to_send(int sock, char **line) {
    char *http_rep = malloc(DIM * DIM * 2 * sizeof(char));
    if (!http_rep)
        error_found("Error in malloc\n");
    memset(http_rep, (int) '\0',DIM * DIM * 2);

    // %d status code; %s status code; %s date; %s server; %s content type; %d content's length; %s connection type
    char *header = "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: %s\r\nAccept-Ranges: bytes\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: %s\r\n\r\n";
    char *t = get_time();
    char *server = "WebServerProject";
    char *h;

    if (!line[0] || !line[1] || !line[2] ||
        ((strncmp(line[0], "GET", 3) && strncmp(line[0], "HEAD", 4)) ||
         (strncmp(line[2], "HTTP/1.1", 8) && strncmp(line[2], "HTTP/1.0", 8)))) {
        sprintf(http_rep, header, 400, "Bad Request", t, server, "text/html", strlen(HTML[2]), "close");
        h = http_rep;
        h += strlen(http_rep);
        memcpy(h, HTML[2], strlen(HTML[2]));

        if (send_http_msg(sock, http_rep, strlen(http_rep)) == -1) {
            fprintf(stderr, "Error while sending data to client\n");
            free_time_http(t, http_rep);
            return -1;
        }
        return 0;
    }

    if (strncmp(line[1], "/", strlen(line[1])) == 0) {
        sprintf(http_rep, header, 200, "OK", t, server, "text/html", strlen(HTML[0]), "keep-alive");
        if (strncmp(line[0], "HEAD", 4)) {
            h = http_rep;
            h += strlen(http_rep);
            memcpy(h, HTML[0], strlen(HTML[0]));
        }

        if (send_http_msg(sock, http_rep, strlen(http_rep)) == -1) {
            fprintf(stderr, "Error while sending data to client\n");
            free_time_http(t, http_rep);
            return -1;
        }
    } else {
        struct image *i = img;
        char *p_name;
        if (!(p_name = strrchr(line[1], '/')))
            i = NULL;
        ++p_name;
        char *p = tmp_resized + strlen("/tmp");

        // Finding image in the image structure
        while (i) {
            if (!strncmp(p_name, i->name, strlen(i->name))) {
                ssize_t dim = 0;
                char *img_to_send = NULL;

                int favicon = 1;
                if (!strncmp(p, line[1], strlen(p) - strlen(".XXXXXX")) ||
                    !(favicon = strncmp(p_name, "favicon.ico", strlen("favicon.ico")))) {
                    // Looking for resized image or favicon.ico
                    if (strncmp(line[0], "HEAD", 4)) {
                        img_to_send = get_img(p_name, i->size_r, favicon ? tmp_resized : IMG_PATH);
                        if (!img_to_send) {
                            fprintf(stderr, "data_to_send: Error in get_img\n");
                            free_time_http(t, http_rep);
                            return -1;
                        }
                    }
                    dim = i->size_r;
                } else {
                    // Looking for image in memory cache
                    char name_cached_img[DIM / 2];
                    memset(name_cached_img, (int) '\0', sizeof(char) * DIM / 2);
                    struct cache *c;
                    int def_val = 70;
                    int processing_accept = quality(line[5]);
                    if (processing_accept == -1)
                        fprintf(stderr, "data_to_send: Unexpected error in strtod\n");
                    int q = processing_accept < 0 ? def_val : processing_accept;

                    lock(thds.mtx_c);
                    c = i->img_c;
                    while (c) {
                        if (c->q == q) {
                            strcpy(name_cached_img, c->img_q);
                            // If an image has been accessed, move it on top of the list
                            //  in order to keep the image with less hit in the bottom of the list
                            if (CACHE_N >= 0 && strncmp(thds.cache_hit_head->cache_name,
                                                        name_cached_img, strlen(name_cached_img))) {
                                struct cache_hit *prev_node, *node;
                                prev_node = NULL;
                                node = thds.cache_hit_tail;
                                while (node) {
                                    if (!strncmp(node->cache_name, name_cached_img, strlen(name_cached_img))) {
                                        if (prev_node) {
                                            prev_node->next_hit = node->next_hit;
                                        } else {
                                            thds.cache_hit_tail = thds.cache_hit_tail->next_hit;
                                        }
                                        node->next_hit = thds.cache_hit_head->next_hit;
                                        thds.cache_hit_head->next_hit = node;
                                        thds.cache_hit_head = thds.cache_hit_head->next_hit;
                                        break;
                                    }
                                    prev_node = node;
                                    node = node->next_hit;
                                }
                            }
                            break;
                        }
                        c = c->next_img_c;
                    }

                    if (!c) {
                        // %s = image's name; %d = factor quality (between 1 and 99)
                        sprintf(name_cached_img, "%s_%d", p_name, q);
                        char path[DIM / 2];
                        memset(path, (int) '\0', DIM / 2);
                        sprintf(path, "%s/%s", tmp_cache, name_cached_img);

                        if (CACHE_N > 0) {
                            // Cache of limited size
                            // If it has not yet reached
                            //  the maximum cache size
                            // %s/%s = path/name_image; %d = factor quality
                            char *format = "convert %s/%s -quality %d %s/%s;exit";
                            char command[DIM];
                            memset(command, (int) '\0', DIM);
                            sprintf(command, format, IMG_PATH, p_name, q, tmp_cache, name_cached_img);
                            if (system(command)) {
                                fprintf(stderr, "data_to_send: Unexpected error while refactoring image\n");
                                free_time_http(t, http_rep);
                                unlock(thds.mtx_c);
                                return -1;
                            }

                            struct stat buf;
                            memset(&buf, (int) '\0', sizeof(struct stat));
                            errno = 0;
                            if (stat(path, &buf) != 0) {
                                if (errno == ENAMETOOLONG) {
                                    fprintf(stderr, "Path too long\n");
                                    free_time_http(t, http_rep);
                                    unlock(thds.mtx_c);
                                    return -1;
                                }
                                fprintf(stderr, "data_to_send: Invalid path\n");
                                free_time_http(t, http_rep);
                                unlock(thds.mtx_c);
                                return -1;
                            } else if (!S_ISREG(buf.st_mode)) {
                                fprintf(stderr, "Non-regular files can not be analysed!\n");
                                free_time_http(t, http_rep);
                                unlock(thds.mtx_c);
                                return -1;
                            }

                            struct cache *new_entry = malloc(sizeof(struct cache));
                            struct cache_hit *new_hit = malloc(sizeof(struct cache_hit));
                            memset(new_entry, (int) '\0', sizeof(struct cache));
                            memset(new_hit, (int) '\0', sizeof(struct cache_hit));
                            if (!new_entry || !new_hit) {
                                fprintf(stderr, "data_to_send: Error in malloc\n");
                                free_time_http(t, http_rep);
                                unlock(thds.mtx_c);
                                return -1;
                            }
                            new_entry->q = q;
                            strcpy(new_entry->img_q, name_cached_img);
                            new_entry->size_q = (size_t) buf.st_size;
                            new_entry->next_img_c = i->img_c;
                            i->img_c = new_entry;
                            c = i->img_c;

                            strncpy(new_hit->cache_name, name_cached_img, strlen(name_cached_img));
                            if (!thds.cache_hit_head && !thds.cache_hit_tail) {
                                new_hit->next_hit = thds.cache_hit_head;
                                thds.cache_hit_tail = thds.cache_hit_head = new_hit;
                            } else {
                                new_hit->next_hit = thds.cache_hit_head->next_hit;
                                thds.cache_hit_head->next_hit = new_hit;
                                thds.cache_hit_head = thds.cache_hit_head->next_hit;
                            }
                            --CACHE_N;
                        } else if (!CACHE_N){
                            // Cache full.
                            // You have to delete an item.
                            // You choose to delete the oldest requested element.
                            char name_to_remove[DIM / 2];
                            memset(name_to_remove, (int) '\0', DIM / 2);
                            sprintf(name_to_remove, "%s/%s", tmp_cache, thds.cache_hit_tail->cache_name);

                            DIR *dir;
                            struct dirent *ent;
                            errno = 0;
                            dir = opendir(tmp_cache);
                            if (!dir) {
                                if (errno == EACCES) {
                                    fprintf(stderr, "data_to_send: Error in opendir: Permission denied\n");
                                    free_time_http(t, http_rep);
                                    unlock(thds.mtx_c);
                                    return -1;
                                }
                                fprintf(stderr, "data_to_send: Error in opendir\n");
                                free_time_http(t, http_rep);
                                unlock(thds.mtx_c);
                                return -1;
                            }

                            while ((ent = readdir(dir)) != NULL) {
                                if (ent->d_type == DT_REG) {
                                    if (!strncmp(ent->d_name, thds.cache_hit_tail->cache_name,
                                                 strlen(thds.cache_hit_tail->cache_name))) {
                                        rm_link(name_to_remove);
                                        break;
                                    }
                                }
                            }
                            if (!ent) {
                                fprintf(stderr, "File: '%s' not removed\n", name_to_remove);
                            }

                            if (closedir(dir)) {
                                fprintf(stderr, "data_to_send: Error in closedir\n");
                                free(img_to_send);
                                free_time_http(t, http_rep);
                                unlock(thds.mtx_c);
                                return -1;
                            }

                            // %s/%s = path/name_image; %d = factor quality
                            char *format = "convert %s/%s -quality %d %s/%s;exit";
                            char command[DIM];
                            memset(command, (int) '\0', DIM);
                            sprintf(command, format, IMG_PATH, p_name, q, tmp_cache, name_cached_img);
                            if (system(command)) {
                                fprintf(stderr, "data_to_send: Unexpected error while refactoring image\n");
                                free_time_http(t, http_rep);
                                unlock(thds.mtx_c);
                                return -1;
                            }

                            struct stat buf;
                            memset(&buf, (int) '\0', sizeof(struct stat));
                            errno = 0;
                            if (stat(path, &buf) != 0) {
                                if (errno == ENAMETOOLONG) {
                                    fprintf(stderr, "Path too long\n");
                                    free_time_http(t, http_rep);
                                    unlock(thds.mtx_c);
                                    return -1;
                                }
                                fprintf(stderr, "data_to_send: Invalid path\n");
                                free_time_http(t, http_rep);
                                unlock(thds.mtx_c);
                                return -1;
                            } else if (!S_ISREG(buf.st_mode)) {
                                fprintf(stderr, "Non-regular files can not be analysed!\n");
                                free_time_http(t, http_rep);
                                unlock(thds.mtx_c);
                                return -1;
                            }

                            struct cache *new_entry = malloc(sizeof(struct cache));
                            memset(new_entry, (int) '\0', sizeof(struct cache));
                            if (!new_entry) {
                                fprintf(stderr, "data_to_send: Error in malloc\n");
                                free_time_http(t, http_rep);
                                unlock(thds.mtx_c);
                                return -1;
                            }
                            new_entry->q = q;
                            strcpy(new_entry->img_q, name_cached_img);
                            new_entry->size_q = (size_t) buf.st_size;
                            new_entry->next_img_c = i->img_c;
                            i->img_c = new_entry;
                            c = i->img_c;

                            // To find and delete oldest requested
                            //  element from cache structure
                            struct image *img_ptr = img;
                            struct cache *cache_ptr, *cache_prev = NULL;
                            char *ext = strrchr(thds.cache_hit_tail->cache_name, '_');
                            size_t dim_fin = strlen(ext);
                            char name_i[DIM / 2];
                            memset(name_i, (int) '\0', DIM / 2);
                            strncpy(name_i, thds.cache_hit_tail->cache_name,
                                    strlen(thds.cache_hit_tail->cache_name) - dim_fin);
                            while (img_ptr) {
                                if (!strncmp(img_ptr->name, name_i, strlen(name_i))) {
                                    cache_ptr = img_ptr->img_c;
                                    while (cache_ptr) {
                                        if (!strncmp(cache_ptr->img_q, thds.cache_hit_tail->cache_name,
                                                     strlen(thds.cache_hit_tail->cache_name))) {
                                            if (!cache_prev)
                                                img_ptr->img_c = cache_ptr->next_img_c;
                                            else
                                                cache_prev->next_img_c = cache_ptr->next_img_c;

                                            free(cache_ptr);
                                            break;
                                        }
                                        cache_prev = cache_ptr;
                                        cache_ptr = cache_ptr->next_img_c;
                                    }
                                    if (!cache_ptr) {
                                        fprintf(stderr, "data_to_send: Error! struct cache compromised\n"
                                                "-Cache size automatically set to Unlimited\n\t\tfinding: %s\n", name_i);
                                        free_time_http(t, http_rep);
                                        CACHE_N = -1;
                                        unlock(thds.mtx_c);
                                        return -1;
                                    }
                                    break;
                                }
                                img_ptr = img_ptr->next_img;
                            }
                            if (!img_ptr) {
                                CACHE_N = -1;
                                fprintf(stderr, "data_to_send: Unexpected error while looking for image in struct image\n"
                                        "-Cache size automatically set to Unlimited\n\t\tfinding: %s\n", name_i);
                                free_time_http(t, http_rep);
                                unlock(thds.mtx_c);
                                return -1;
                            }

                            struct cache_hit *new_hit = malloc(sizeof(struct cache_hit));
                            memset(new_hit, (int) '\0', sizeof(struct cache_hit));
                            if (!new_hit) {
                                fprintf(stderr, "data_to_send: Error in malloc\n");
                                free_time_http(t, http_rep);
                                unlock(thds.mtx_c);
                                return -1;
                            }

                            strncpy(new_hit->cache_name, name_cached_img, strlen(name_cached_img));
                            struct cache_hit *to_be_removed = thds.cache_hit_tail;
                            new_hit->next_hit = thds.cache_hit_head->next_hit;
                            thds.cache_hit_head->next_hit = new_hit;
                            thds.cache_hit_head = thds.cache_hit_head->next_hit;
                            thds.cache_hit_tail = thds.cache_hit_tail->next_hit;
                            free(to_be_removed);
                        } else {
                            // In the case where it is not place
                            //  a limit on the size of the cache
                            // %s/%s = path/name_image; %d = factor quality
                            char *format = "convert %s/%s -quality %d %s/%s;exit";
                            char command[DIM];
                            memset(command, (int) '\0', DIM);
                            sprintf(command, format, IMG_PATH, p_name, q, tmp_cache, name_cached_img);
                            if (system(command)) {
                                fprintf(stderr, "data_to_send: Unexpected error while refactoring image\n");
                                free_time_http(t, http_rep);
                                unlock(thds.mtx_c);
                                return -1;
                            }

                            struct stat buf;
                            memset(&buf, (int) '\0', sizeof(struct stat));
                            errno = 0;
                            if (stat(path, &buf) != 0) {
                                if (errno == ENAMETOOLONG) {
                                    fprintf(stderr, "Path too long\n");
                                    free_time_http(t, http_rep);
                                    unlock(thds.mtx_c);
                                    return -1;
                                }
                                fprintf(stderr, "data_to_send: Invalid path\n");
                                free_time_http(t, http_rep);
                                unlock(thds.mtx_c);
                                return -1;
                            } else if (!S_ISREG(buf.st_mode)) {
                                fprintf(stderr, "Non-regular files can not be analysed!\n");
                                free_time_http(t, http_rep);
                                unlock(thds.mtx_c);
                                return -1;
                            }

                            struct cache *new_entry = malloc(sizeof(struct cache));
                            memset(new_entry, (int) '\0', sizeof(struct cache));
                            if (!new_entry) {
                                fprintf(stderr, "data_to_send: Error in malloc\n");
                                free_time_http(t, http_rep);
                                unlock(thds.mtx_c);
                                return -1;
                            }
                            new_entry->q = q;
                            strcpy(new_entry->img_q, name_cached_img);
                            new_entry->size_q = (size_t) buf.st_size;
                            new_entry->next_img_c = i->img_c;
                            i->img_c = new_entry;
                            c = i->img_c;
                        }
                    }

                    unlock(thds.mtx_c);

                    if (strncmp(line[0], "HEAD", 4)) {
                        DIR *dir;
                        struct dirent *ent;
                        errno = 0;
                        dir = opendir(tmp_cache);
                        if (!dir) {
                            if (errno == EACCES) {
                                fprintf(stderr, "data_to_send: Error in opendir: Permission denied\n");
                                free_time_http(t, http_rep);
                                return -1;
                            }
                            fprintf(stderr, "data_to_send: Error in opendir\n");
                            free_time_http(t, http_rep);
                            return -1;
                        }

                        while ((ent = readdir(dir)) != NULL) {
                            if (ent->d_type == DT_REG) {
                                if (!strncmp(ent->d_name, name_cached_img, strlen(name_cached_img))) {
                                    img_to_send = get_img(name_cached_img, c->size_q, tmp_cache);
                                    if (!img_to_send) {
                                        fprintf(stderr, "data_to_send: Error in get_img\n");
                                        free_time_http(t, http_rep);
                                        return -1;
                                    }
                                    break;
                                }
                            }
                        }

                        if (closedir(dir)) {
                            fprintf(stderr, "data_to_send: Error in closedir\n");
                            free(img_to_send);
                            free_time_http(t, http_rep);
                            return -1;
                        }
                    }
                    dim = c->size_q;
                }

                sprintf(http_rep, header, 200, "OK", t, server, "image/gif", dim, "keep-alive");
                ssize_t dim_tot = (size_t) strlen(http_rep);
                if (strncmp(line[0], "HEAD", 4)) {
                    if (dim_tot + dim > DIM * DIM * 2) {
                        http_rep = realloc(http_rep, (dim_tot + dim) * sizeof(char));
                        if (!http_rep) {
                            fprintf(stderr, "data_to_send: Error in realloc\n");
                            free_time_http(t, http_rep);
                            free(img_to_send);
                            return -1;
                        }
                        memset(http_rep + dim_tot, (int) '\0', (size_t) dim);
                    }
                    h = http_rep;
                    h += dim_tot;
                    memcpy(h, img_to_send, (size_t) dim);
                    dim_tot += dim;
                }
                if (send_http_msg(sock, http_rep, dim_tot) == -1) {
                    fprintf(stderr, "data_to_send: Error while sending data to client\n");
                    free_time_http(t, http_rep);
                    return -1;
                }

                free(img_to_send);
                break;
            }
            i = i->next_img;
        }

        if (!i) {
            sprintf(http_rep, header, 404, "Not Found", t, server, "text/html", strlen(HTML[1]), "close");
            if (strncmp(line[0], "HEAD", 4)) {
                h = http_rep;
                h += strlen(http_rep);
                memcpy(h, HTML[1], strlen(HTML[1]));
            }
            if (send_http_msg(sock, http_rep, strlen(http_rep)) == -1) {
                fprintf(stderr, "Error while sending data to client\n");
                free_time_http(t, http_rep);
                return -1;
            }
        }
    }
    free_time_http(t, http_rep);
    return 0;
}

// Analyzes HTTP message
void respond(int sock, struct sockaddr_in client) {
    char http_req[DIM * DIM];
    char *line_req[7];
    ssize_t tmp;
    int i;

    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(struct timeval)) < 0)
        fprintf(stderr, "respond: Error in setsockopt\n");

    do {
        memset(http_req, (int) '\0', 5 * DIM);
        for (i = 0; i < 7; ++i)
            line_req[i] = NULL;

        errno = 0;
        tmp = recv(sock, http_req, 5 * DIM, 0);

        if (tmp == -1) {
            switch (errno) {
                case EFAULT:
                    fprintf(stderr, "The receive  buffer  pointer(s)  point  outside  the  process's address space");
                    break;

                case EBADF:
                    fprintf(stderr, "The argument of recv() is an invalid descriptor: %d\n", sock);
                    break;

                case ECONNREFUSED:
                    fprintf(stderr, "Remote host refused to allow the network connection\n");
                    break;

                case ENOTSOCK:
                    fprintf(stderr, "The argument of recv() does not refer to a socket\n");
                    break;

                case EINVAL:
                    fprintf(stderr, "Invalid argument passed\n");
                    break;

                case EINTR:
                    fprintf(stderr, "Timeout receiving from socket\n");
                    break;

                case EWOULDBLOCK:
                    fprintf(stderr, "Timeout receiving from socket\n");
                    break;

                default:
                    fprintf(stderr, "Error in recv: error while receiving data from client\n");
                    break;
            }
            break;
        } else if (tmp == 0) {
            fprintf(stderr, "Client disconnected\n");
            break;
        } else {
            split_str(http_req, line_req);

            char log_string[DIM / 2];
            memset(log_string, (int) '\0', DIM / 2);
            sprintf(log_string, "\tClient:\t%s\tRequest: '%s %s %s'\n",
                    inet_ntoa(client.sin_addr), line_req[0], line_req[1], line_req[2]);
            write_log(log_string);

            if (data_to_send(sock, line_req))
                break;
        }
    } while (line_req[3] && !strncmp(line_req[3], "keep-alive", 10));
}

// Start server
void startServer(void) {
    struct sockaddr_in server_addr;

    if ((LISTENsd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        error_found("Error in socket\n");

    memset((void *) &server_addr, (int) '\0', sizeof(server_addr));
    (server_addr).sin_family = AF_INET;
    // All available interface
    (server_addr).sin_addr.s_addr = htonl(INADDR_ANY);
    (server_addr).sin_port = htons((unsigned short) PORT);

    // To reuse a socket
    int flag = 1;
    if (setsockopt(LISTENsd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) != 0)
        error_found("Error in setsockopt\n");

    errno = 0;
    if (bind(LISTENsd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        switch (errno) {
            case EACCES:
                error_found("Choose another socket\n");

            case EADDRINUSE:
                error_found("Address in use\n");

            case EINVAL:
                error_found("The socket is already bound to an address\n");

            default:
                error_found("Error in bind\n");
        }
    }

    // listen for incoming connections
    if (listen(LISTENsd, MAXCONN) != 0)
        error_found("Error in listen\n");

    fprintf(stdout, "-Server's socket correctly created with number: %d\n", PORT);
}

// Initialize threads
void init_th(int n, void *(*routine) (void *), void *arg) {
    struct th_sync *k = (struct th_sync *) arg;

    int i, j;
    lock(k -> mtx_s_c);
    for (i = j = 0; i < n && j < MAXCONN; ++j) {
        if (k -> clients[j] == -2) {
            k -> slot_c = j;
            create_th(routine, arg);

            k -> clients[j] = -3;
            wait_t(k -> th_start, k -> mtx_s_c);
            ++i;
        }
    }
    k -> th_act += n;
    unlock(k -> mtx_s_c);
}

// Used by kill_th function
void for_kill(int n_th, struct th_sync *k) {
    int i, j;

    for (i = j = 0; i < n_th && j < MAXCONN; ++j)
        if (k -> clients[j] == -1) {
            k -> clients[j] = -2;
            signal_t(k -> new_c + j);
            ++i;
        }

    // If there are not enough threads ready (with -1 flag)
    if (i < n_th)
        k -> to_kill = n_th - i;
}

// Used to kill threads
void kill_th(struct th_sync *k) {
    int n_th = 0;

    if (k -> th_act_thr > MINTH) {
        int old_thr = (k -> th_act_thr - MINTH / 2) * 2 / 3;
        // To bring system to the default thread count
        if (!k -> connections) {
            k -> th_act_thr = MINTH;
            n_th = k -> th_act - k -> th_act_thr;
            k -> to_kill = 0;
            for_kill(n_th, k);
            return;
        } else if (k -> connections < old_thr) {
            // Gradual deallocation
            if (k -> th_act_thr == MAXCONN) {
                if (!(n_th = (MAXCONN - MINTH) % (MINTH / 2)))
                    n_th = MINTH / 2;
            } else {
                n_th = MINTH / 2;
            }
            k -> th_act_thr -= n_th;
        }
    }

    if (k -> to_kill) {
        n_th += k -> to_kill;
        k -> to_kill = 0;
    }

    for_kill(n_th, k);
}

void *manage_connection(void *arg);

// used to create other threads in the case
//  in which the server load is rising
void spawn_th(struct th_sync *k) {
    // Threads are created dynamically in need with the number of connections.
    // If the number of connections decreases, the number of active threads
    // 	is reduced in a phased manner so as to cope with a possible peak of connections.
    if (k -> connections >= k -> th_act_thr * 2 / 3 &&
        k -> th_act <= k -> th_act_thr) {
        int n_th;
        if (k -> th_act_thr + MINTH / 2 <= MAXCONN) {
            n_th = MINTH / 2;
        } else {
            n_th = MAXCONN - k -> th_act_thr;
        }
        if (n_th) {
            k -> th_act_thr += n_th;
            init_th(n_th, manage_connection, k);
        }
    }
}

// This is the main threads' routine. This function is used to manage
//  client's connection
void *manage_connection(void *arg) {
    if (pthread_detach(pthread_self()) != 0)
        error_found("Error in pthread_detach\n");

    struct th_sync *k = (struct th_sync *) arg;
    struct sockaddr_in client;
    int slot_c, sock;

    lock(k -> mtx_s_c);
    slot_c = k -> slot_c;
    signal_t(k -> th_start);
    unlock(k -> mtx_s_c);

    lock(k -> mtx_t);
    if (k -> clients[slot_c] == -3) {
        // Thread ready for incoming connections
        k -> clients[slot_c] = -1;
    } else {
        fprintf(stderr, "Unknown error: slot[%d]: %d\n", slot_c, k -> clients[slot_c]);
        pthread_exit(NULL);
    }
    // Deal connections
    while (1) {
        memset(&client, (int) '\0', sizeof(struct sockaddr_in));
        wait_t(k->new_c + slot_c, k->mtx_t);
        // sock values:
        // -1 -> thread ready for incoming connections
        // -2 -> thread killed by kill_th function or thread not yet created
        // -3 -> newly created thread
        sock = k->clients[slot_c];
        if (sock < 0) {
            if (sock != -2) {
                fprintf(stderr, "Unknown error trying to access sock array: %d\n", sock);
                continue;
            }
            --k->th_act;
            unlock(k->mtx_t);
            break;
        }
        memcpy(&client, &k->client_addr, sizeof(struct sockaddr_in));
        ++k -> connections;
        spawn_th(k);
        unlock(k -> mtx_t);

        respond(sock, client);

        errno = 0;
        if (close(sock) != 0) {
            switch (errno) {
                case EIO:
                    fprintf(stderr, "I/O error occurred\n");
                    break;

                case EBADF:
                    fprintf(stderr, "Bad file number: %d. Probably client has disconnected\n", sock);
                    break;

                default:
                    fprintf(stderr, "Error in close\n");
            }
        }

        lock(k -> mtx_t);
        --k -> connections;
        kill_th(k);
        k -> clients[slot_c] = -1;

        signal_t(k -> full);
    }

    pthread_exit(EXIT_SUCCESS);
}

// Thread which control stdin to recognize user's input
void *catch_command(void *arg) {
    struct th_sync *k = (struct th_sync *) arg;

    printf("\n%s\n", user_command);
    while (1) {
        char cmd[2];
        int conn, n_thds;
        memset(cmd, (int) '\0', 2);
        if (fscanf(stdin, "%s", cmd) != 1)
            error_found("Error in fscanf\n");

        if (strlen(cmd) != 1) {
            printf("%s\n", user_command);
        } else {
            if (cmd[0] == 's' || cmd[0] == 'S') {
                lock(thds.mtx_t);
                conn = thds.connections; n_thds = thds.th_act;
                unlock(thds.mtx_t);
                fprintf(stdout, "\nConnections' number: %d\n"
                        "Threads running: %d\n\n", conn, n_thds);
                continue;
            } else if (cmd[0] == 'f' || cmd[0] == 'F') {
                errno = 0;
                if (fflush(LOG)) {
                    if (errno == EBADF)
                        fprintf(stderr, "Error in fflush: Stream is not an open stream, or is not open for writing.\n");
                    fprintf(stderr, "catch_command: Unexpected error in fflush\n");
                }
                fprintf(stdout, "Log file updated\n");
                continue;
            } else if (cmd[0] == 'q' || cmd[0] == 'Q') {
                fprintf(stdout, "-Closing server\n");

                errno = 0;
                // Kernel may still hold some resources for a period (TIME_WAIT)
                if (close(LISTENsd) != 0) {
                    if (errno == EIO)
                        error_found("I/O error occurred\n");
                    error_found("Error in close\n");
                }

                int i = 0;
                for (; i < MAXCONN; ++i) {
                    if (k -> clients[i] >= 0) {
                        if (close(k -> clients[i]) != 0) {
                            switch (errno) {
                                case EIO:
                                    error_found("I/O error occurred\n");

                                case ENOTCONN:
                                    error_found("The socket is not connected\n");

                                case EBADF:
                                    fprintf(stderr, "Bad file number. Probably client has disconnected\n");
                                    break;

                                default:
                                    error_found("Error in close or shutdown\n");
                            }
                        }
                    }
                }
                write_log("\t\tServer closed.\n\n\n");

                errno = 0;
                if (fflush(LOG)) {
                    if (errno == EBADF)
                        fprintf(stderr, "Error in fflush: Stream is not an open stream, or is not open for writing.\n");
                    exit(EXIT_FAILURE);
                }

                if (fclose(LOG) != 0)
                    error_found("Error in fclose\n");

                free_mem();

                exit(EXIT_SUCCESS);
            }
            printf("%s\n\n", user_command);
        }
    }
}

// This is the main threads which manage all incoming connections.
// Once a client send request to the server, this thread checks if it can
//  process the connection or not. If so assigns the connection management
//  to a child thread, otherwise it waits on a pthread_cond_t condition,
//  until the system load is not lowered.
void *manage_threads(void *arg) {
    struct th_sync *k = (struct th_sync *) arg;

    create_th(catch_command, arg);
    init_th(MINTH, manage_connection, arg);

    int connsocket, i = 0, j;
    struct sockaddr_in client;
    socklen_t socksize = sizeof(struct sockaddr_in);

    fprintf(stdout, "\n\n\n-Waiting for incoming connection...\n");
    // Accept connections
    while (1) {
        lock(k -> mtx_t);
        if (k -> connections + 1 > MAXCONN) {
            wait_t(k -> full, k -> mtx_t); }
        unlock(k -> mtx_t);

        memset(&client, (int) '\0', socksize);
        errno = 0;
        connsocket = accept(LISTENsd, (struct sockaddr *) &client, &socksize);
        memset(&k->client_addr, (int) '\0', socksize);
        memcpy(&k->client_addr, &client, socksize);

        lock(k -> mtx_t);
        if (connsocket == -1) {
            switch (errno) {
                case ECONNABORTED:
                    fprintf(stderr, "The connection has been aborted\n");
                    unlock(k -> mtx_t);
                    continue;

                case ENOBUFS:
                    error_found("Not enough free memory\n");

                case ENOMEM:
                    error_found("Not enough free memory\n");

                case EMFILE:
                    fprintf(stderr, "Too many open files!\n");
                    wait_t(k -> full, k -> mtx_t);
                    unlock(k -> mtx_t);
                    continue;

                case EPROTO:
                    fprintf(stderr, "Protocol error\n");
                    unlock(k -> mtx_t);
                    continue;

                case EPERM:
                    fprintf(stderr, "Firewall rules forbid connection\n");
                    unlock(k -> mtx_t);
                    continue;

                case ETIMEDOUT:
                    fprintf(stderr, "Timeout occured\n");
                    unlock(k -> mtx_t);
                    continue;

                case EBADF:
                    fprintf(stderr, "Bad file number\n");
                    unlock(k -> mtx_t);
                    continue;

                default:
                    error_found("Error in accept\n");
            }
        }

        //printf("\nNUM CONN: %d\t\tTH_ACT: %d\t\tTH_THR: %d\n\n", k -> connections, k -> th_act, k -> th_act_thr);
        j = 1;
        while (k -> clients[i] != -1) {
            if (j > MAXCONN) {
                j = -1;
                break;
            }
            i = (i + 1) % MAXCONN;
            ++j;
        }
        if (j == -1) {
            unlock(k -> mtx_t);
            continue;
        }
        k -> clients[i] = connsocket;
        signal_t(k -> new_c + i);
        i = (i + 1) % MAXCONN;
        unlock(k -> mtx_t);
    }
}

// This function initialize all needed resources
void init(int argc, char **argv, pthread_mutex_t *m, pthread_mutex_t *m2,
          pthread_mutex_t *m3, pthread_cond_t *c,
          pthread_cond_t *c2, struct th_sync *d) {
    // Default Log's path;
    char LOG_PATH[DIM],
            IMAGES_PATH[DIM];
    memset(LOG_PATH, (int) '\0', DIM);
    memset(IMAGES_PATH, (int) '\0', DIM);
    strcpy(LOG_PATH, ".");
    strcpy(IMAGES_PATH, ".");
    char *PATH[2];
    PATH[0] = LOG_PATH;
    PATH[1] = IMAGES_PATH;
    int perc = 20;

    get_opt(argc, argv, PATH, &perc);

    if (pthread_mutex_init(m, NULL) != 0 ||
        pthread_mutex_init(m2, NULL) != 0 ||
        pthread_mutex_init(m3, NULL) != 0 ||
        pthread_cond_init(c, NULL) != 0 ||
        pthread_cond_init(c2, NULL) != 0)
        error_found("Error in pthread_mutex_init or pthread_cond_init\n");

    d->connections = d->slot_c = d->to_kill = d->th_act = 0;
    d->mtx_s_c = m;
    d->mtx_c = m2;
    d->mtx_t = m3;
    d->th_start = c;
    d->full = c2;
    d->th_act_thr = MINTH;
    d->cache_hit_head = d->cache_hit_tail = NULL;
    img = NULL;

    d->clients = malloc(sizeof(int) * MAXCONN);
    d->new_c = malloc(sizeof(pthread_cond_t) * MAXCONN);
    if (!d->clients || !d->new_c) {
        error_found("Error in malloc\n");
    } else {
        memset(d->clients, (int) '\0', sizeof(int) * MAXCONN);
        memset(d->new_c, (int) '\0', sizeof(pthread_cond_t) * MAXCONN);
    }
    // -1 := slot with thread initialized; -2 := empty slot.
    int i;
    for (i = 0; i < MAXCONN; ++i) {
        d->clients[i] = -2;

        pthread_cond_t cond;
        if (pthread_cond_init(&cond, NULL) != 0)
            error_found("Error in pthread_cond_init\n");
        d->new_c[i] = cond;
    }

    startServer();
    LOG = open_file(LOG_PATH);
    char start_server[DIM];
    memset(start_server, (int) '\0', DIM);
    char *k = "\t\tServer started at port:";
    sprintf(start_server, "%s %d\n", k, PORT);
    write_log(start_server);

    // Create tmp folder for resized and cached images
    if (!mkdtemp(tmp_resized) || !mkdtemp(tmp_cache))
        error_found("Error in mkdtmp\n");

    if (CACHE_N > 0) {
        fprintf(stdout, "-Cache size: %d images; located in '%s'\n", CACHE_N, tmp_cache);
    } else {
        fprintf(stdout, "-Cache size: Unlimited; located in '%s'\n", tmp_cache);
    }

    strcpy(IMG_PATH, IMAGES_PATH);
    check_images(perc);

    map_html_error(HTML);
}

int main(int argc, char **argv) {
    if (argc > 16) {
        fprintf(stderr, "Too many arguments\n\n");
        usage(*argv);
    }

    pthread_mutex_t mtx_s_c, mtx_c, mtx_t;
    pthread_cond_t th_start, full;


    init(argc, argv, &mtx_s_c, &mtx_c, &mtx_t,
         &th_start, &full, &thds);

    // To ignore SIGPIPE
    catch_signal();

    manage_threads(&thds);

    return EXIT_SUCCESS;
}
