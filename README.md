# Operating_Systems_2024

My solutions to the 2024 Operating Systems (ELE3021) assignments at Hanyang University.  
Assignments are based on xv6.  
They may contain errors.(I couldn't get a perfect score on some assignments)  

## Project 1
Implement a system call getgpid().  
getgipd() returns grand parent process ID.  
Also, implement an userprogram which returns PID and grand parant process ID.(project01.c)  

## Project 2
- Implement multi-level feedback queue(MLFQ) scheduling.
  - There are four level feedback queue(L0~L3, L0 has the highest priority), and when a process is lauched, it goes into L0 queue.
  - L0, L1, L2 queue follow round robin policy.
  - Process with odd number PID: When it uses its all time quantum in L0, it moves to L1 queue. When it uses its all time quantum in L1, it moves to L3.
  - Process with even number PID: When it uses its all time quantum in L0, it moves to L2 queue. When it uses its all time quantum in L1, it moves to L3.
  - In L3, processes are scheduled according to its priority. Also, implement setpriority() system call to set a priority of process. Priority decreases when the process uses its all time quantum.
  - Priority boosting: Move all processes to L0 queue every 100 global ticks.
- Implement monopoly queue(MoQ)
  - FCFS scheduling queue.
  - Processes in MoQ are not scheduled; However, when system call monopolize() is called, they are scheduled in FCFS and return to normal state(using MLFQ) after all processes in MoQ is finished.
 
## Project 3
- Implement light weight process(LWP)
  - Implement simple POXIX threads: implement system calls thread_create(), thread_join(), thread_exit() / modify system calls fork(), exec(), sbrk(), kill(), sleep(), pipe()
  - Also, to implement thread, other fuctions like allocproc(), userinit() needs to be modified.
- Implement locking
  - Implement locking without using synchronization APIs.
  - pthread_lock_linux_atomic.c : use atomic instruction(test & set)
  - pthread_lock_linux_patterson.c : use N-thread Patterson algorithm

## Project 4
- Implement copy-on-write(CoW)
