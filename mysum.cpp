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

/*
ReadFromFile method reads n integers from a file and returns them in a vector

filename: is the name of the file containing the number of elements.
n: is the amount given by the user that is the size of the array to be read from the file.
*/
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
/*
WriteToFile method writes the results of Hillis and Steele's algorithm 
after the program is concluded into a seperate txt file named "output.txt".

B: it is a pointer to the array containing the final values after the algorithm is completed.
n: number of elements in the array B to be written.
filename: the filename the results will be outputted into
*/
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

/*
arriveAndWait method is used for announcing that the process has reached a synchronization 
and it will wait until all m processes have reached that same point.

id: this is the method caller's process index, we are using this to store the process's arrival.
m: the total number of worker proceeses repeadtly checking this number till all have arrived before realeasing the barrier.
counter_ptr: this is a pointer to a shared integer storing the current barrier's phase.
arrived_array: this is a pointer to a shared array where the most recent barrier phase reached by the process is recorded.
*/
static inline void arriveAndWait(int id, int m, volatile int* phase_ptr, volatile int* arrived_array)
{
    // Each barrier has a "phase number" so the same barrier can be reused many times.
    int phase_nmr = *phase_ptr;

    // Announce: "worker id has arrived at phase counter"
    arrived_array[id] = phase_nmr;

    // Wait until every worker has arrived (i.e., arrived_array[j] >= counter for all j).
    while (true) {
        bool all = true;
        for (int j = 0; j < m; j++) {
            if (arrived_array[j] < phase_nmr) {
                all = false;
            }
        }
        if (all) break;
    }

    // Worker 0 acts like the "coordinator" that advances the phase number.
    if (id == 0) {
        *phase_ptr = phase_nmr + 1;
    }

    // Everyone waits until the phase number is advanced before leaving the barrier.
    while (*phase_ptr == phase_nmr) {
        // Give CPU time to other processes while spinning.
        sched_yield();
    }
}


// computing log2n 
// n: postive integer
int ceillog2(int n){
    int p = 0;
    int x = 1;
    while(x < n){
        x <<= 1;
        p++;
    }
    return p;
}

/*
Code executed by each child process to compute prefix sums in parallel in Hillis-Steele rounds.
Each Worker updates only its own chunk of the array in shared memory.

id: the worker index number
n: number of elements in the array
m: total number of worker processes
arr0: shared array buffer 1 
arr1: shared array buffer 2
counter_ptr: shared barrier phase integer used by arriveAndWait
arrived_array: shared barrier array used by arriveAndWait
*/
void worker(int id, int n, int m, long long* arr0, long long* arr1, volatile int* counter_ptr, volatile int* arrived_array)
{
    // Split the n elements into m chunks.
    // Example: if n=10 and m=3, chunk = 4, ranges are [0..3], [4..7], [8..9]
    int chunk = (n + m - 1) / m;
    int start = id * chunk;
    int end   = min(start + chunk, n);

    int P = ceillog2(n); // number of rounds

    // src is the array we read from, dst is the array we write to this round.
    long long* src = arr0;
    long long* dst = arr1;

    // Run rounds 1..P with offsets 1,2,4,8,...
    for (int p = 1; p <= P; p++) {
        int offset = 1 << (p - 1);

        // Compute only my assigned portion [start, end).
        // Each position i adds the value from i-offset (if that exists).
        for (int i = start; i < end; i++) {
            if (i < offset) {
                dst[i] = src[i];
            } else {
                dst[i] = src[i] + src[i - offset];
            }
        }

        // Barrier #1: ensure every worker finished writing dst before anyone reads it.
        arriveAndWait(id, m, counter_ptr, arrived_array);

        // Swap buffers locally: next round reads the updated results.
        long long* tmp = src;
        src = dst;
        dst = tmp;

        // Barrier #2: ensure everyone swapped before next round begins.
        arriveAndWait(id, m, counter_ptr, arrived_array);
    }

    // Child process ends here.
    // _exit avoids re-running parent cleanup/iostream flushing in the child.
    _exit(0);
}

/*
Read input array from file, create shared memory for synchronizartion variables,
for m child processes to compute prefix sum in parallel, wait for children and then write to results in filename

argc: the count of the array of argv
argv: user input of n m input output
*/
int main(int argc, char* argv[]){
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

    // Compute how much shared memory we need:
    // - Two arrays of long long (arr0 and arr1), each length n
    // - One int for the barrier phase counter
    // - One int array of length m for arrivals
    size_t bytes =
        2ull * (size_t)n * sizeof(long long)  // arr0 + arr1
      + 1ull * sizeof(int)                    // phase counter
      + (size_t)m * sizeof(int);              // arrived[m]

    // Create a private shared-memory segment (only this program’s processes can use it).
    int shmid = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
    if (shmid < 0) {
        perror("shmget");
        return EXIT_FAILURE;
    }

    // Attach shared memory into this process’s address space.
    void* shared_memory = shmat(shmid, NULL, 0);
    if (shared_memory == (void*)-1) {
        perror("shmat");
        return EXIT_FAILURE;
    }

    // Read input data from file
    vector<long long> data = ReadFromFile(argv[3], n);

    // Lay out the shared memory in this order:
    // [arr0 (n long long)] [arr1 (n long long)] [phase (int)] [arrived (m int)]
    long long* arr0 = (long long*) shared_memory;
    long long* arr1 = arr0 + n;

    volatile int* phase_ptr = (volatile int*) (arr1 + n);
    volatile int* arrived   = phase_ptr + 1;

    // Initialize shared arrays:
    // arr0 starts as the original input; arr1 is just a scratch buffer.
    for (int i = 0; i < n; i++) {
        arr0[i] = data[i];
        arr1[i] = 0;
    }

    // Initialize barrier state:
    *phase_ptr = 0;
    for (int j = 0; j < m; j++) {
        arrived[j] = -1; // “has not reached phase 0 yet”
    }

    // Fork m worker processes.
    for (int id = 0; id < m; id++) {
        pid_t pid = fork();
        if (pid < 0) {
            // Fork failed: parent cleans up and exits.
            perror("fork");
            for (int k = 0; k < id; k++) wait(NULL);
            shmdt(shared_memory);
            return EXIT_FAILURE;
        }

        if (pid == 0) {
            // Child: run worker logic for this id, then exit.
            worker(id, n, m, arr0, arr1, phase_ptr, arrived);
            _exit(0);
        }
        // Parent continues loop to fork the next child.
    }

    // Parent waits until all children finish.
    for (int i = 0; i < m; i++) {
        wait(NULL);
    }

    // Decide which shared buffer contains the final answer.
    // If we swapped an odd number of times, result ends in arr1; otherwise arr0.
    int P = ceillog2(n);
    const long long* result = (P % 2 == 0) ? arr0 : arr1;

    // Write final result
    WriteToFile(result, n, argv[4]);

    // Detach shared memory from parent
    shmdt(shared_memory);

    return EXIT_SUCCESS;
}