#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdlib>
#include <algorithm>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sched.h>

using namespace std;

// Read n numbers from a file into a vector
vector<long long> ReadFromFile(const string &filename, int n){
    ifstream in(filename);
    if(!in){
        cerr << "cannot open file\n";
        exit(EXIT_FAILURE);
    }

    vector<long long> A;

    long long x;
    while(in >> x){
        A.push_back(x);
        if((int)A.size() == n){
            break;
        }
    }
    return A;
}

// Write n numbers from an array into a file
void WriteToFile(const long long *B, int n, const string &filename){
    ofstream out(filename.c_str());
    if(!out){
        cerr << "cannot output to a file\n";
        exit(EXIT_FAILURE);
    }

    for(int i = 0; i < n; i++){
        out << B[i];
        if(i < n - 1) out << " ";
    }
    out << "\n";
}

// Simple barrier so all m processes wait for each other
static inline void arriveAndWait(int id, int m, volatile int* phase_ptr, volatile int* arrived_array)
{
    int phase_nmr = *phase_ptr;

    arrived_array[id] = phase_nmr;

    while (true) {
        bool all = true;
        for (int j = 0; j < m; j++) {
            if (arrived_array[j] < phase_nmr) {
                all = false;
            }
        }
        if (all) break;
    }

    if (id == 0) {
        *phase_ptr = phase_nmr + 1;
    }

    while (*phase_ptr == phase_nmr) {
        sched_yield();
    }
}

// Small helper to compute ceil(log2(n))
int ceillog2(int n){
    int p = 0;
    int x = 1;
    while(x < n){
        x <<= 1;
        p++;
    }
    return p;
}

// Code run by each child process to help build the prefix sums
void worker(int id, int n, int m, long long* arr0, long long* arr1, volatile int* counter_ptr, volatile int* arrived_array)
{
    // Split the n elements into m chunks
    int chunk = (n + m - 1) / m;
    int start = id * chunk;
    int end   = min(start + chunk, n);

    int P = ceillog2(n);

    long long* src = arr0;
    long long* dst = arr1;

    // Do P rounds of the Hillis-Steele prefix sum algorithm
    for (int p = 1; p <= P; p++) {
        int offset = 1 << (p - 1);

        // Only work on this worker's slice [start, end)
        for (int i = start; i < end; i++) {
            if (i < offset) {
                dst[i] = src[i];
            } else {
                dst[i] = src[i] + src[i - offset];
            }
        }

        arriveAndWait(id, m, counter_ptr, arrived_array);

        long long* tmp = src;
        src = dst;
        dst = tmp;

        arriveAndWait(id, m, counter_ptr, arrived_array);
    }

    _exit(0);
}

// Main program: set up shared memory, start workers, then write result
int main(int argc, char* argv[]){
    // Expect: program n m inputfile outputfile
    if(argc != 5){
        return EXIT_FAILURE;
    }

    int n = atoi(argv[1]);
    int m = atoi(argv[2]);

    if (n <= 0 || m <= 0) {
        cerr << "m and n should be greater than 0\n";
        return EXIT_FAILURE;
    }
    if (n < m) {
        cerr << "m cannot be greater than n\n";
        return EXIT_FAILURE;
    }

    // Figure out how many bytes of shared memory we need
    size_t bytes =
        2ull * (size_t)n * sizeof(long long)  +
        1ull * sizeof(int)                    +
        (size_t)m * sizeof(int);

    int shmid = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
    if (shmid < 0) {
        perror("shmget");
        return EXIT_FAILURE;
    }

    void* shared_memory = shmat(shmid, NULL, 0);
    if (shared_memory == (void*)-1) {
        perror("shmat");
        return EXIT_FAILURE;
    }

    vector<long long> data = ReadFromFile(argv[3], n);

    long long* arr0 = (long long*) shared_memory;
    long long* arr1 = arr0 + n;

    volatile int* phase_ptr = (volatile int*) (arr1 + n);
    volatile int* arrived   = phase_ptr + 1;

    for (int i = 0; i < n; i++) {
        arr0[i] = data[i];
        arr1[i] = 0;
    }

    *phase_ptr = 0;
    for (int j = 0; j < m; j++) {
        arrived[j] = -1;
    }

    for (int id = 0; id < m; id++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            for (int k = 0; k < id; k++) wait(NULL);
            shmdt(shared_memory);
            return EXIT_FAILURE;
        }

        if (pid == 0) {
            worker(id, n, m, arr0, arr1, phase_ptr, arrived);
            _exit(0);
        }
    }

    for (int i = 0; i < m; i++) {
        wait(NULL);
    }

    int P = ceillog2(n);
    const long long* result = (P % 2 == 0) ? arr0 : arr1;

    WriteToFile(result, n, argv[4]);

    shmdt(shared_memory);

    return EXIT_SUCCESS;
}

