#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>

using namespace std;

vector<long long> readAllNumbers(const string &filename) {
    ifstream in(filename);
    if (!in) {
        throw std::runtime_error("Cannot open file: " + filename);
    }

    vector<long long> A;
    long long x;

    while (in >> x) {          
        A.push_back(x);
    }

    return A;                 
}

void writeToFile(const string &filename, const long long *B, int n){
    ofstream out(filename);

    if(!out){
        cerr << "Error: cannot output file " << filename << endl;
        return;
    }

    for(int i = 0; i < n; i++){
        out << B[i];
        if(i < n - 1) out << " ";
    }

    out << endl;
}

void hillis_steele_prefix_sum(const long long *A, long long *B, int n) {
    if (n <= 0) return;

    // Two buffers to ping-pong between iterations
    long long *prev = (long long *)malloc((size_t)n * sizeof(long long));
    long long *curr = (long long *)malloc((size_t)n * sizeof(long long));

    // x[0] = A
    for (int i = 0; i < n; i++) {
        prev[i] = A[i];
    }

    for (int offset = 1; offset < n; offset = offset * 2) {
        for (int i = 0; i < n; i++) {
            if (i < offset){
                curr[i] = prev[i];
            }
            else {
                curr[i] = prev[i] + prev[i - offset];
            }
        }

        // swap(prev, curr)
        long long *tmp = prev;
        prev = curr;
        curr = tmp;
    }

    // final result is in prev
    for (int i = 0; i < n; i++)
    { 
        B[i] = prev[i];
    }

}

int main() {
    vector<long long> A;

    A = readAllNumbers("input.dat");

    int n = (int)A.size();
    if (n == 0) {
        cout << "no size for n" << endl;
        return 1; 
    }

    long long *B = (long long *)malloc((size_t)n * sizeof(long long));

    hillis_steele_prefix_sum(A.data(), B, n);

    for (int i = 0; i < n; i++) {
        printf("%lld%c", B[i], (i + 1 == n) ? '\n' : ' ');
    }

    writeToFile("output.dat", B, n);

    free(B);
    return 0;
}