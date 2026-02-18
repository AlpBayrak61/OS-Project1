// Compile: g++ -std=c++17 -o my-sum mysum.cpp
// Usage:   ./my-sum n m input.dat output.dat
//   n = number of elements, m = number of processes (workers)
//   Main process forks m children; children compute; main waits and writes output.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <iostream>
#include <fstream>
#include <vector>

using namespace std;

static void usage(const char* prog) {
    cerr << "Usage: " << prog << " n m input.dat output.dat\n"
         << "  n = number of elements\n"
         << "  m = number of processes\n"
         << "  input.dat  = file containing numbers\n"
         << "  output.dat = file to write prefix sum\n";
}

// Validate and exit safely on error.
static int validate(int n, int m, const char* input_path) {
    if (n <= 0) {
        cerr << "Error: n must be > 0 (got " << n << ")\n";
        return -1;
    }
    if (m <= 0) {
        cerr << "Error: m must be > 0 (got " << m << ")\n";
        return -1;
    }
    if (n < m) {
        cerr << "Error: n must be >= m (n=" << n << ", m=" << m << ")\n";
        return -1;
    }
    ifstream in(input_path);
    if (!in) {
        cerr << "Error: input file does not exist or cannot be opened: " << input_path << "\n";
        return -1;
    }
    long long x;
    int count = 0;
    while (in >> x && count < n)
        count++;
    in.close();
    if (count < n) {
        cerr << "Error: input file contains " << count << " numbers; need at least " << n << "\n";
        return -1;
    }
    return 0;
}

static void load_input(const char* input_path, int n, vector<long long>& out) {
    out.resize(n);
    ifstream in(input_path);
    for (int i = 0; i < n; i++)
        in >> out[i];
    in.close();
}

// Shared memory layout (shmget):
//   int n, m;
//   int next_rank, rank_lock;  (worker rank assignment, spinlock in shared memory)
//   padding to 16
//   long long A[n], buf0[n], buf1[n]
//   int barrier_count, barrier_phase (barrier variables in shared memory)
#define OFF_N(sz)            0
#define OFF_M(sz)            4
#define OFF_NEXTRANK(sz)     8
#define OFF_RANKLOCK(sz)     12
#define OFF_A(sz)            16
#define OFF_BUF0(sz)         (16 + (size_t)(sz) * 8)
#define OFF_BUF1(sz)          (16 + (size_t)(sz) * 16)
#define OFF_BARRIER_COUNT(sz) (16 + (size_t)(sz) * 24)
#define OFF_BARRIER_PHASE(sz) (16 + (size_t)(sz) * 24 + 4)
#define SHM_SIZE(sz)          (OFF_BARRIER_PHASE(sz) + 8)

// Barrier using only shared memory (no threads, no pthread, no semaphores).
// barrier_count and barrier_phase are in shared memory. Count increment is atomic.
static void barrier(volatile int* barrier_count, volatile int* barrier_phase, int m) {
    int my_phase = *barrier_phase;
    int arrived = __sync_fetch_and_add((int*)barrier_count, 1) + 1;
    if (arrived == m) {
        *barrier_count = 0;
        *barrier_phase = my_phase + 1;
        __sync_synchronize();
    } else {
        while (*barrier_phase == my_phase)
            ;
    }
}

int main(int argc, char* argv[]) {
    if (argc != 5) {
        usage(argv[0]);
        return 1;
    }

    int n = atoi(argv[1]);
    int m = atoi(argv[2]);
    const char* input_path  = argv[3];
    const char* output_path = argv[4];

    if (validate(n, m, input_path) != 0)
        return 1;

    vector<long long> input_data;
    load_input(input_path, n, input_data);

    // Shared memory: array, temporary arrays, barrier variables
    size_t shm_size = SHM_SIZE(n);
    int shmid = shmget(IPC_PRIVATE, shm_size, IPC_CREAT | 0666);
    if (shmid == -1) {
        cerr << "Error: shmget failed: " << strerror(errno) << "\n";
        return 1;
    }

    char* shm = (char*)shmat(shmid, nullptr, 0);
    if (shm == (char*)-1) {
        cerr << "Error: shmat failed: " << strerror(errno) << "\n";
        shmctl(shmid, IPC_RMID, nullptr);
        return 1;
    }

    *(int*)(shm + OFF_N(n)) = n;
    *(int*)(shm + OFF_M(n)) = m;
    *(int*)(shm + OFF_NEXTRANK(n)) = 0;
    *(int*)(shm + OFF_RANKLOCK(n)) = 0;

    long long* A    = (long long*)(shm + OFF_A(n));
    long long* buf0 = (long long*)(shm + OFF_BUF0(n));
    long long* buf1 = (long long*)(shm + OFF_BUF1(n));
    for (int i = 0; i < n; i++)
        A[i] = buf0[i] = input_data[i];

    volatile int* barrier_count = (volatile int*)(shm + OFF_BARRIER_COUNT(n));
    volatile int* barrier_phase  = (volatile int*)(shm + OFF_BARRIER_PHASE(n));
    *barrier_count = 0;
    *barrier_phase = 0;

    // Main process: fork m children (workers). Main does not compute.
    for (int i = 0; i < m; i++) {
        pid_t pid = fork();
        if (pid == -1) {
            cerr << "Error: fork failed: " << strerror(errno) << "\n";
            shmdt(shm);
            shmctl(shmid, IPC_RMID, nullptr);
            return 1;
        }
        if (pid == 0) {
            // Child: worker process. All use same shared memory (already attached).
            volatile int* rank_lock = (volatile int*)(shm + OFF_RANKLOCK(n));
            while (__sync_lock_test_and_set((int*)rank_lock, 1))
                ;
            int rank = (*(int*)(shm + OFF_NEXTRANK(n)))++;
            __sync_lock_release((int*)rank_lock);

            int n_elems = *(int*)(shm + OFF_N(n));
            int m_proc  = *(int*)(shm + OFF_M(n));
            int start   = rank * n_elems / m_proc;
            int end     = (rank + 1) * n_elems / m_proc;

            // Hillis-Steele: log n rounds; each round O(n/m) per process + barrier O(m) -> total O((n log n)/m + m log n)
            int iter = 0;
            for (int offset = 1; offset < n_elems; offset *= 2) {
                long long* in_buf  = (iter % 2 == 0) ? buf0 : buf1;
                long long* out_buf = (iter % 2 == 0) ? buf1 : buf0;
                for (int i = start; i < end; i++) {
                    if (i < offset)
                        out_buf[i] = in_buf[i];
                    else
                        out_buf[i] = in_buf[i] + in_buf[i - offset];
                }
                barrier(barrier_count, barrier_phase, m_proc);
                iter++;
            }
            shmdt(shm);
            exit(0);
        }
    }

    // Main process: wait for all children
    for (int i = 0; i < m; i++)
        wait(nullptr);

    // Main process: write final result (result is in buf0 or buf1 after last iteration)
    int num_iters = 0;
    for (int offset = 1; offset < n; offset *= 2)
        num_iters++;
    long long* result = (num_iters % 2 == 0) ? buf0 : buf1;

    FILE* out = fopen(output_path, "w");
    if (!out) {
        cerr << "Error: cannot open output file: " << output_path << "\n";
        shmdt(shm);
        shmctl(shmid, IPC_RMID, nullptr);
        return 1;
    }
    for (int i = 0; i < n; i++)
        fprintf(out, "%s%lld", i ? " " : "", result[i]);
    fprintf(out, "\n");
    fclose(out);

    // Clean shared memory and exit cleanly
    shmdt(shm);
    shmctl(shmid, IPC_RMID, nullptr);
    return 0;
}
