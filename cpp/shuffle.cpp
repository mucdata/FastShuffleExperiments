// g++ -mavx2 -march=native -std=c++11 -O3 -o shuffle shuffle.cpp -Wall -Wextra

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <algorithm>
#include <random>


#include "pcg.h"
#include "lehmer64.h"
#include "xorshift128plus.h"
#include "splitmix64.h"

static inline uint32_t lehmer64_32(void) {
  return (uint32_t) lehmer64();
}

static inline uint32_t xorshift128plus_32(void) {
  return (uint32_t) xorshift128plus();
}

std::mt19937 mersenne;

static inline uint32_t twister(void) {
  return (uint32_t) mersenne();
}

typedef uint32_t(*randfnc32)(void);

// wrapper to satisfy fancy C++
template <randfnc32 RandomBitGenerator>
struct UniformRandomBitGenerator {
   typedef uint32_t result_type;
   static constexpr result_type min() {return 0;}
  static constexpr result_type max() {return UINT32_MAX;}
   uint32_t operator()() {
   return RandomBitGenerator();
   }
 };

// return value in [0,bound)
// as per the PCG implementation , uses two 32-bit divisions
template <randfnc32 RandomBitGenerator>
static inline uint32_t random_bounded(uint32_t bound) {
    uint32_t threshold = (~bound + 1) % bound;// -bound % bound
    for (;;) {
        uint32_t r = RandomBitGenerator();
        if (r >= threshold)
            return r % bound;
    }
}

template <randfnc32 RandomBitGenerator>
static inline uint32_t floatmult_random_bounded(uint32_t bound) {
    return (uint32_t)((float)(RandomBitGenerator() & 0xffffff) / (float) (1 << 24) * bound );
}

// return value in [0,bound)
// as per the Java implementation , uses one or more 32-bit divisions
template <randfnc32 RandomBitGenerator>
static inline uint32_t java_random_bounded(uint32_t bound) {
    uint32_t rkey = RandomBitGenerator();
    uint32_t candidate = rkey % bound;
    while(rkey - candidate  > UINT32_MAX - bound + 1 ) { // will be predicted as false
        rkey = RandomBitGenerator();
        candidate = rkey % bound;
    }
    return candidate;
}

// return value in [0,bound)
// as per the Go implementation
template <randfnc32 RandomBitGenerator>
static inline uint32_t go_random_bounded(uint32_t bound) {
  uint32_t bits;
  // optimizing for powers of two is harmful
  //if((bound & (bound - 1)) == 0) {
  //    return pcg32_random() & (bound - 1);
  //}
  uint32_t t = 0xFFFFFFFF % bound;
  do {
    bits = RandomBitGenerator();
  } while(bits <= t);
  return bits % bound;
}

// map random value to [0,range) with slight bias, redraws to avoid bias if needed
template <randfnc32 RandomBitGenerator>
static inline uint32_t random_bounded_divisionless(uint32_t range) {
    uint64_t random32bit, multiresult;
    uint32_t leftover;
    uint32_t threshold;
    random32bit =  RandomBitGenerator();
    multiresult = random32bit * range;
    leftover = (uint32_t) multiresult;
    if(leftover < range ) {
        threshold = -range % range ;
        while (leftover < threshold) {
            random32bit =  RandomBitGenerator();
            multiresult = random32bit * range;
            leftover = (uint32_t) multiresult;
        }
    }
    return multiresult >> 32; // [0, range)
}









#define RDTSC_START(cycles)                                                   \
    do {                                                                      \
         unsigned cyc_high, cyc_low;                                  \
        __asm volatile(                                                       \
            "cpuid\n\t"                                                       \
            "rdtsc\n\t"                                                       \
            "mov %%edx, %0\n\t"                                               \
            "mov %%eax, %1\n\t"                                               \
            : "=r"(cyc_high), "=r"(cyc_low)::"%rax", "%rbx", "%rcx", "%rdx"); \
        (cycles) = ((uint64_t)cyc_high << 32) | cyc_low;                      \
    } while (0)

#define RDTSC_FINAL(cycles)                                                   \
    do {                                                                      \
         unsigned cyc_high, cyc_low;                                  \
        __asm volatile(                                                       \
            "rdtscp\n\t"                                                      \
            "mov %%edx, %0\n\t"                                               \
            "mov %%eax, %1\n\t"                                               \
            "cpuid\n\t"                                                       \
            : "=r"(cyc_high), "=r"(cyc_low)::"%rax", "%rbx", "%rcx", "%rdx"); \
        (cycles) = ((uint64_t)cyc_high << 32) | cyc_low;                      \
    } while (0)


/**
Sorting stuff

*/

int qsort_compare_uint32_t(const void *a, const void *b) {
    return ( *(uint32_t *)a - *(uint32_t *)b );
}
uint32_t *create_sorted_array(size_t length) {
    uint32_t *array = (uint32_t *) malloc(length * sizeof(uint32_t));
    for (size_t i = 0; i < length; i++) array[i] = (uint32_t) pcg32_random();
    qsort(array, length, sizeof(*array), qsort_compare_uint32_t);
    return array;
}

uint32_t *create_random_array(size_t count) {
    uint32_t *targets = (uint32_t *)  malloc(count * sizeof(uint32_t));
    for (size_t i = 0; i < count; i++) targets[i] = (uint32_t) (pcg32_random() & 0x7FFFFFF);
    return targets;
}




// flushes the array from cache
void array_cache_flush(uint32_t* B, int32_t length) {
    const int32_t CACHELINESIZE = 64;// 64 bytes per cache line
    for(int32_t  k = 0; k < length; k += CACHELINESIZE/sizeof(uint32_t)) {
        __builtin_ia32_clflush(B + k);
    }
}

// tries to put the array in cache
void array_cache_prefetch(uint32_t* B, int32_t length) {
    const int32_t CACHELINESIZE = 64;// 64 bytes per cache line
    for(int32_t  k = 0; k < length; k += CACHELINESIZE/sizeof(uint32_t)) {
        __builtin_prefetch(B + k);
    }
}



/*
 * Prints the best number of operations per cycle where
 * test is the function call, answer is the expected answer generated by
 * test, repeat is the number of times we should repeat and size is the
 * number of operations represented by test.
 */
#define BEST_TIME(test, pre, repeat, size)                         \
        do {                                                              \
            printf("%-60s: ", #test);                                        \
            fflush(NULL);                                                 \
            uint64_t cycles_start, cycles_final, cycles_diff;             \
            uint64_t min_diff = (uint64_t)-1;                             \
            for (int i = 0; i < repeat; i++) {                            \
                pre;                                                       \
                __asm volatile("" ::: /* pretend to clobber */ "memory"); \
                RDTSC_START(cycles_start);                                \
                test;                     \
                RDTSC_FINAL(cycles_final);                                \
                cycles_diff = (cycles_final - cycles_start);              \
                if (cycles_diff < min_diff) min_diff = cycles_diff;       \
            }                                                             \
            uint64_t S = size;                                            \
            float cycle_per_op = (min_diff) / (double)S;                  \
            printf(" %.2f cycles per input key ", cycle_per_op);           \
            printf("\n");                                                 \
            fflush(NULL);                                                 \
 } while (0)

#include <chrono>

typedef std::chrono::high_resolution_clock Clock;

#define BEST_TIME_NS(test, pre, repeat, size)                         \
        do {                                                              \
            printf("%-60s: ", #test);                                        \
            fflush(NULL);                                                 \
            int64_t min_diff = INT64_MAX;                             \
            for (int i = 0; i < repeat; i++) {                            \
                pre;                                                       \
                __asm volatile("" ::: /* pretend to clobber */ "memory"); \
                 auto t0 = Clock::now();                                \
                test;                     \
                auto t1 = Clock::now();                                \
                auto cycles_diff = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();              \
                if (cycles_diff < min_diff) min_diff = cycles_diff;       \
            }                                                             \
            uint64_t S = size;                                            \
            printf(" %d ns total, ",(int)min_diff);                       \
            float cycle_per_op = (min_diff) / (double)S;                  \
            printf(" %.2f ns per input key ", cycle_per_op);           \
            printf("\n");                                                 \
            fflush(NULL);                                                 \
 } while (0)



int sortAndCompare(uint32_t * shuf, uint32_t * orig, uint32_t size) {
    qsort(shuf, size, sizeof(uint32_t), qsort_compare_uint32_t);
    qsort(orig, size, sizeof(uint32_t), qsort_compare_uint32_t);
    for(uint32_t k = 0; k < size; ++k)
        if(shuf[k] != orig[k]) {
            printf("[bug]\n");
            return -1;
        }
    return 0;
}


// good old Fisher-Yates shuffle, shuffling an array of integers, uses java-like ranged rng
template <randfnc32 UniformRandomBitGenerator>
void  shuffle_java(uint32_t *storage, uint32_t size) {
    uint32_t i;
    for (i=size; i>1; i--) {
        uint32_t nextpos = java_random_bounded<UniformRandomBitGenerator>(i);
        uint32_t tmp = storage[i-1];// likely in cache
        uint32_t val = storage[nextpos]; // could be costly
        storage[i - 1] = val;
        storage[nextpos] = tmp; // you might have to read this store later
    }
}


// good old Fisher-Yates shuffle, shuffling an array of integers
template <randfnc32 UniformRandomBitGenerator>
void  shuffle_floatmult(uint32_t *storage, uint32_t size) {
    uint32_t i;
    for (i=size; i>1; i--) {
        uint32_t nextpos = floatmult_random_bounded<UniformRandomBitGenerator>(i);
        uint32_t tmp = storage[i-1];// likely in cache
        uint32_t val = storage[nextpos]; // could be costly
        storage[i - 1] = val;
        storage[nextpos] = tmp; // you might have to read this store later
    }
}


// good old Fisher-Yates shuffle, shuffling an array of integers, uses go-like ranged rng
template <randfnc32 UniformRandomBitGenerator>
void  shuffle_go(uint32_t *storage, uint32_t size) {
    uint32_t i;
    for (i=size; i>1; i--) {
        uint32_t nextpos = go_random_bounded<UniformRandomBitGenerator>(i);
        uint32_t tmp = storage[i-1];// likely in cache
        uint32_t val = storage[nextpos]; // could be costly
        storage[i - 1] = val;
        storage[nextpos] = tmp; // you might have to read this store later
    }
}




// good old Fisher-Yates shuffle, shuffling an array of integers, without division
template <randfnc32 UniformRandomBitGenerator>
void  shuffle_divisionless(uint32_t *storage, uint32_t size) {
    uint32_t i;
    for (i=size; i>1; i--) {
        uint32_t nextpos = random_bounded_divisionless<UniformRandomBitGenerator>(i);
        uint32_t tmp = storage[i-1];// likely in cache
        uint32_t val = storage[nextpos]; // could be costly
        storage[i - 1] = val;
        storage[nextpos] = tmp; // you might have to read this store later
    }
}

template <randfnc32 rfnc32>
void demo(int size) {
    printf(" %s\n", __PRETTY_FUNCTION__);
    printf("Shuffling arrays of size %d \n",size);
    printf("Time reported in number of cycles per array element.\n");
    printf("Tests assume that array is in cache as much as possible.\n");
    int repeat = 500;
    uint32_t * testvalues = create_random_array(size);
    uint32_t * pristinecopy = (uint32_t *) malloc(size * sizeof(uint32_t));
    memcpy(pristinecopy,testvalues,sizeof(uint32_t) * size);
    if(sortAndCompare(testvalues, pristinecopy, size)!=0) return;


    std::random_device rd;
    std::mt19937 gmt19937(rd());

    UniformRandomBitGenerator<rfnc32> gen;

    BEST_TIME(std::shuffle(testvalues,testvalues+size,gen), array_cache_prefetch(testvalues,size), repeat, size);
    BEST_TIME_NS(std::shuffle(testvalues,testvalues+size,gen), array_cache_prefetch(testvalues,size), repeat, size);
    if(sortAndCompare(testvalues, pristinecopy, size)!=0) return;

    BEST_TIME(shuffle_go<rfnc32>(testvalues,size), array_cache_prefetch(testvalues,size), repeat, size);
    BEST_TIME_NS(shuffle_go<rfnc32>(testvalues,size), array_cache_prefetch(testvalues,size), repeat, size);
    if(sortAndCompare(testvalues, pristinecopy, size)!=0) return;

    BEST_TIME(shuffle_java<rfnc32>(testvalues,size), array_cache_prefetch(testvalues,size), repeat, size);
    BEST_TIME_NS(shuffle_java<rfnc32>(testvalues,size), array_cache_prefetch(testvalues,size), repeat, size);
    if(sortAndCompare(testvalues, pristinecopy, size)!=0) return;

    BEST_TIME(shuffle_floatmult<rfnc32>(testvalues,size), array_cache_prefetch(testvalues,size), repeat, size);
    BEST_TIME_NS(shuffle_floatmult<rfnc32>(testvalues,size), array_cache_prefetch(testvalues,size), repeat, size);
    if(sortAndCompare(testvalues, pristinecopy, size)!=0) return;

    BEST_TIME(shuffle_divisionless<rfnc32>(testvalues,size), array_cache_prefetch(testvalues,size), repeat, size);
    BEST_TIME_NS(shuffle_divisionless<rfnc32>(testvalues,size), array_cache_prefetch(testvalues,size), repeat, size);
    if(sortAndCompare(testvalues, pristinecopy, size)!=0) return;

    free(testvalues);
    free(pristinecopy);
    printf("\n");
}

int main() {
    const size_t N = 1000;
    xorshift128plus_seed(1234);
    splitmix64_seed(1234);
    lehmer64_seed(1234);

    demo<pcg32_random>(N);
    demo<lehmer64_32>(N);
    demo<xorshift128plus_32>(N);
    demo<splitmix64_cast32>(N);
    demo<twister>(N);



    return 0;
}