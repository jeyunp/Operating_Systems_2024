//locking using atomic instruction test and set
#include <stdio.h>
#include <pthread.h>
#include <sys/types.h>
#include <syscall.h>
#include <stdlib.h>

int shared_resource = 0;

#define NUM_ITERS 1
#define NUM_THREADS 100

pthread_t threads[NUM_THREADS];
int Flag[NUM_THREADS];                  //idx번째 스레드가 큐의 몇 번째 위치에 도달했는지 체크
int Turn[NUM_THREADS];                  //내가 i번째 위치에 도달한 마지막 스레드인지 확인
int lk;


void lock();
void unlock();

int searchidx(pthread_t tid){
    int i;
    for(i=0; i<NUM_THREADS; i++){
        if(threads[i] == tid)
            break;
    }
    if(i == NUM_THREADS){
        printf("error\n");
        exit(1);
    }
    return i+1;
}

void lock()
{
    int i=0, idx=0;                         //idx=변환된 tid, i=현재 스레드의 큐에서의 위치
    int j=0;
    int gobreak = 1;
    pthread_t tid = pthread_self();
    idx = searchidx(tid);
    
    for(i=1; i<=NUM_THREADS; i++){
        Flag[idx-1] = i;
        Turn[i-1] = idx;
        while(1){
            if(Turn[i-1] != idx){
                break;
            }
            for(j = 1; j<=NUM_THREADS; j++){
                if(j==idx)
                    continue;
                if(Flag[j-1]>=i){                 
                    gobreak = 0;
                }
            }
            if(gobreak==1)
                break;
            gobreak = 1;
        }
    }
}


void unlock()
{
    pthread_t tid = pthread_self();
    int idx=0;
    idx = searchidx(tid);
    Flag[idx-1] = 0;
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