#include "threadpool.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>



// Create threadpool
threadpool* create_threadpool(int num_threads_in_pool, int max_queue_size) {
    threadpool *tp = (threadpool*)malloc(sizeof(threadpool));
    if (!tp) {
        perror("Failed to allocate threadpool");
        return NULL;
    }

    tp->num_threads = num_threads_in_pool;
    tp->qsize = 0;
    tp->max_qsize = max_queue_size;
    tp->threads = (pthread_t*)malloc(num_threads_in_pool * sizeof(pthread_t));
    if (!tp->threads) {
        perror("Failed to allocate threads");
        free(tp);
        return NULL;
    }

    tp->qhead = NULL;
    tp->qtail = NULL;
    pthread_mutex_init(&tp->qlock, NULL);
    pthread_cond_init(&tp->q_not_empty, NULL);
    pthread_cond_init(&tp->q_empty, NULL);
    pthread_cond_init(&tp->q_not_full, NULL);
    tp->shutdown = 0;
    tp->dont_accept = 0;

    // Create worker threads
    for (int i = 0; i < num_threads_in_pool; i++) {
        if (pthread_create(&tp->threads[i], NULL, do_work, tp) != 0) {
            perror("Failed to create a thread");
            destroy_threadpool(tp);
            return NULL;
        }
    }

    return tp;
}

// Dispatch task to queue
void dispatch(threadpool* from_me, dispatch_fn dispatch_to_here, void *arg) {

    work_t *work = (work_t*)malloc(sizeof(work_t));
    if (!work) {
        perror("Failed to allocate task");
        return;
    }
    work->routine = dispatch_to_here;
    work->arg = arg;
    work->next = NULL;

    pthread_mutex_lock(&from_me->qlock);

    // Wait if the queue is full
    while (from_me->qsize == from_me->max_qsize && !from_me->shutdown) {


        pthread_cond_wait(&from_me->q_not_full, &from_me->qlock);
    }

    if (from_me->shutdown || from_me->dont_accept) {
        free(work);
        pthread_mutex_unlock(&from_me->qlock);
        return;
    }

    // Add task to the queue
    if (from_me->qsize == 0) {
        from_me->qhead = work;
        from_me->qtail = work;
        pthread_cond_signal(&from_me->q_not_empty);  // Signal workers
    } else {
        from_me->qtail->next = work;
        from_me->qtail = work;
    }
    from_me->qsize++;

    pthread_mutex_unlock(&from_me->qlock);
}

// Worker thread function
void* do_work(void* p) {
    threadpool *tp = (threadpool*)p;

    while (1) {

        pthread_mutex_lock(&tp->qlock);

        // Wait for tasks if the queue is empty
        while (tp->qsize == 0 && !tp->shutdown) {
            pthread_cond_wait(&tp->q_not_empty, &tp->qlock);
        }

        if (tp->shutdown) {
            pthread_mutex_unlock(&tp->qlock);
            break;  // Exit thread loop
        }

        // Get the task from the queue
        work_t *work = tp->qhead;
        tp->qhead = tp->qhead->next;
        tp->qsize--;
        if (tp->qsize == 0) {
            tp->qtail = NULL;
            pthread_cond_signal(&tp->q_empty);  // Signal the queue is empty
        }


        pthread_cond_signal(&tp->q_not_full);  // Signal producers queue is not full
        pthread_mutex_unlock(&tp->qlock);

        // Execute task
        work->routine(work->arg);
        free(work);
    }

    return NULL;
}

// Destroy threadpool
void destroy_threadpool(threadpool* destroyme) {

    pthread_mutex_lock(&destroyme->qlock);

    destroyme->dont_accept = 1;
    // Wait for the queue to empty
    while (destroyme->qsize > 0) {
        pthread_cond_wait(&destroyme->q_empty, &destroyme->qlock);
    }

    destroyme->shutdown = 1;
    pthread_cond_broadcast(&destroyme->q_not_empty);  // Wake all worker threads
    pthread_mutex_unlock(&destroyme->qlock);

    // Join all threads
    for (int i = 0; i < destroyme->num_threads; i++) {
        pthread_join(destroyme->threads[i], NULL);
    }

    // Free resources
    free(destroyme->threads);
    pthread_mutex_destroy(&destroyme->qlock);
    pthread_cond_destroy(&destroyme->q_not_empty);
    pthread_cond_destroy(&destroyme->q_empty);
    pthread_cond_destroy(&destroyme->q_not_full);
    free(destroyme);
}