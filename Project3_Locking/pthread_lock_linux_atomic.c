//locking using atomic instruction test and set
#include <stdio.h>
#include <pthread.h>
#include <sys/types.h>
#include <syscall.h>

int shared_resource = 0;

#define NUM_ITERS 100
#define NUM_THREADS 10
#define MAX_THREADS 100

int lk;


void lock();
void unlock();


void lock()
{
    while(__sync_lock_test_and_set(&lk, 1)){
        continue;
    }

}

void unlock()
{
    __sync_lock_release(&lk);
    //lk = 0;
}

void* thread_func(void* arg) {
    int tid = *(int*)arg;
    int i=0;
    
    lock();
    
        for(i = 0; i < NUM_ITERS; i++)    shared_resource++;
    
    unlock();
    
    pthread_exit(NULL);
}

int main() {
    pthread_t threads[NUM_THREADS];
    int tids[NUM_THREADS];
    int i=0;
    
    for (i = 0; i < NUM_THREADS; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, thread_func, &tids[i]);
    }
    
    for (i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("shared: %d\n", shared_resource);
    
    return 0;
}