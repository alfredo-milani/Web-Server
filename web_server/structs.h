//
// Created by alfredo on 18/06/16.
//

#ifndef WEB_SERVER_STRUCTS_H
#define WEB_SERVER_STRUCTS_H

#include <netinet/in.h>

#define DIM 512
#define DIM2 64


// Struct which contains cache's state
struct cache {
    // Quality factor
    int q;
    // type string: "%s_%d";
    //     %s is the name of the image; %d is the factor quality (above int q)
    char img_q[DIM / 2];
    size_t size_q;
    struct cache *next_img_c;
};

// Struct to manage cache hit
struct cache_hit {
    char cache_name[DIM / 2];
    struct cache_hit *next_hit;
};

// Struct which contains all image's references
struct image {
    // Name of current image
    char name[DIM2 * 2];
    // Memory mapped of resized image
    size_t size_r;
    struct cache *img_c;
    struct image *next_img;
};

// Struct which contains all variables for synchronise threads
struct th_sync {
    struct sockaddr_in client_addr;
    struct cache_hit *cache_hit_tail,
                     *cache_hit_head;
    int *clients;
    volatile int slot_c,
                 connections,
                 th_act,
                 th_act_thr,
                 to_kill;
    // To manage thread's number and connections
    pthread_mutex_t *mtx_t;
    // To manage cache access
    pthread_mutex_t *mtx_c;
    // To sync pthread_condition variables
    pthread_mutex_t *mtx_s_c;
    // Array containing condition
    // variables of all threads
    pthread_cond_t *new_c;
    // To initialize threads
    pthread_cond_t *th_start;
    // Number of maximum connection reached
    pthread_cond_t *full;
};

#endif //WEB_SERVER_STRUCTS_H