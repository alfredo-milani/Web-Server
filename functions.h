//
// Created by alfredo on 18/06/16.
//

#ifndef WEB_SERVER_FUNCTIONS_H
#define WEB_SERVER_FUNCTIONS_H

#include "structs.h"



extern FILE *LOG;
extern char *HTML[3];
extern int PORT;
extern int MINTH;
extern int MAXCONN;
extern int LISTENsd;
extern volatile int CACHE_N;
extern char IMG_PATH[DIM / 2];
extern char tmp_resized[DIM2];
extern char tmp_cache[DIM2];
extern char *usage_str;
extern char *user_command;

extern struct image *img;
extern struct th_sync thds;

void write_on_stream(char *s, FILE *file);

void exit_on_error(char *s);

void rm_link(char *path);

void rm_dir(char *directory);

void free_mem(void);

void error_found(char *s);

void catch_signal(void);

void lock(pthread_mutex_t *m);

void unlock(pthread_mutex_t *m);

void wait_t(pthread_cond_t *c, pthread_mutex_t *m);

void signal_t(pthread_cond_t *c);

FILE *open_file(const char *path);

void map_html_error(char *HTML[3]);

void get_info(struct stat *buf, char *path, int check);

char *get_time(void);

void write_log(char *s);

ssize_t send_http_msg(int sd, char *s, ssize_t dim);

char *get_img(char *name, size_t img_dim, char *directory);

void free_time_http(char *time, char *http);

void split_str(char *s, char **d);

void create_th(void * (*routine) (void *), void *k);

void usage(const char *p);

void alloc_r_img(struct image **h, char *path);

void get_opt(int argc, char **argv, char **path, int *perc);

void check_and_build(char *s, char **html, size_t *dim);

void check_images(int perc);

#endif //WEB_SERVER_FUNCTIONS_H