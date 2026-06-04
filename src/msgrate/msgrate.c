/* -*- C -*-
 *
 * Copyright 2006 Sandia Corporation. Under the terms of Contract
 * DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government
 * retains certain rights in this software.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, 
 * Boston, MA  02110-1301, USA.
 */

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * If we're using a GPU aware varient.
 */
#ifdef ENABLE_CUDA
#include <cuda_runtime.h>
#endif

#ifdef ENABLE_HIP
#include <hip/hip_runtime.h>
#endif

static void
abort_app(const char *msg)
{
    perror(msg);
    MPI_Abort(MPI_COMM_WORLD, 1);
}

/**
 * GPU Aware Helper Functions 
 */
static void* bench_alloc(size_t size);
static void  bench_free(void* ptr);
static void  bench_memset(void* ptr, int value, size_t size);
static int   bench_gpu_init(void);
static void  bench_cleanup(void);
static void bench_memcpy_to_host(void* dst, const void* src, size_t size);
static void bench_memcpy_from_host(void* dst, const void* src, size_t size);
static void bench_sync(void);

#ifdef ENABLE_CUDA
static void check_cuda(cudaError_t rc, const char* msg)
{
    if (rc != cudaSuccess) {
        fprintf(stderr, "%s failed: %s\n", msg, cudaGetErrorString(rc));
        exit(1);
    }
}
#endif

#ifdef ENABLE_HIP
static void check_hip(hipError_t rc, const char* msg)
{
    if (rc != hipSuccess) {
        fprintf(stderr, "%s failed: %s\n", msg, hipGetErrorString(rc));
        exit(1);
    }
}
#endif

static int bench_gpu_init(void)
{
#if defined(ENABLE_CUDA)
    int dev = 0;
    int dev_count = 0;
    char* s = getenv("LOCAL_RANK");

    check_cuda(cudaGetDeviceCount(&dev_count), "cudaGetDeviceCount");
    if (dev_count <= 0) {
        fprintf(stderr, "No CUDA devices found\n");
        return -1;
    }

    if (s != NULL) {
        dev = atoi(s) % dev_count;
    }

    check_cuda(cudaSetDevice(dev), "cudaSetDevice");
    return 0;

#elif defined(ENABLE_HIP)
    int dev = 0;
    int dev_count = 0;
    char* s = getenv("LOCAL_RANK");

    check_hip(hipGetDeviceCount(&dev_count), "hipGetDeviceCount");
    if (dev_count <= 0) {
        fprintf(stderr, "No HIP devices found\n");
        return -1;
    }

    if (s != NULL) {
        dev = atoi(s) % dev_count;
    }

    check_hip(hipSetDevice(dev), "hipSetDevice");
    return 0;

#else
    return 0;
#endif
}

static void* bench_alloc(size_t size)
{
    void* p = NULL;

#if defined(ENABLE_CUDA)
    check_cuda(cudaMalloc(&p, size), "cudaMalloc");
    check_cuda(cudaMemset(p, 0, size), "cudaMemset");
#elif defined(ENABLE_HIP)
    check_hip(hipMalloc(&p, size), "hipMalloc");
    check_hip(hipMemset(p, 0, size), "hipMemset");
#else
    p = malloc(size);
    if (p == NULL) {
        perror("malloc");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    memset(p, 0, size);
#endif

    return p;
}

static void bench_free(void* ptr)
{
    if (ptr == NULL) return;

#if defined(ENABLE_CUDA)
    check_cuda(cudaFree(ptr), "cudaFree");
#elif defined(ENABLE_HIP)
    check_hip(hipFree(ptr), "hipFree");
#else
    free(ptr);
#endif
}

static void bench_cleanup(void)
{
#if defined(ENABLE_CUDA)
    cudaDeviceSynchronize();
#elif defined(ENABLE_HIP)
    hipDeviceSynchronize();
#endif
}

static void bench_sync(void)
{
#if defined(ENABLE_CUDA)
    check_cuda(cudaDeviceSynchronize(), "cudaDeviceSynchronize");
#elif defined(ENABLE_HIP)
    check_hip(hipDeviceSynchronize(), "hipDeviceSynchronize");
#endif
}

static void bench_memcpy_to_host(void* dst, const void* src, size_t size)
{
#if defined(ENABLE_CUDA)
    check_cuda(cudaMemcpy(dst, src, size, cudaMemcpyDeviceToHost), "cudaMemcpy D2H");
#elif defined(ENABLE_HIP)
    check_hip(hipMemcpy(dst, src, size, hipMemcpyDeviceToHost), "hipMemcpy D2H");
#else
    memcpy(dst, src, size);
#endif
}

static void bench_memcpy_from_host(void* dst, const void* src, size_t size)
{
#if defined(ENABLE_CUDA)
    check_cuda(cudaMemcpy(dst, src, size, cudaMemcpyHostToDevice), "cudaMemcpy H2D");
#elif defined(ENABLE_HIP)
    check_hip(hipMemcpy(dst, src, size, hipMemcpyHostToDevice), "hipMemcpy H2D");
#else
    memcpy(dst, src, size);
#endif
}

/** Verification helper functions */
static unsigned char pattern_byte(int src_rank, int msg_idx, size_t byte_idx)
{
    unsigned int x = (unsigned int) src_rank;
    x = x * 131u + (unsigned int) msg_idx;
    x = x * 131u + (unsigned int) byte_idx;
    return (unsigned char) (x & 0xffu);
}

static void init_send_buffer(char* buf, int rank, int npeers, int nmsgs, size_t nbytes)
{
    size_t total = (size_t) npeers * (size_t) nmsgs * nbytes;
    unsigned char* host = (unsigned char*) malloc(total);
    if (host == NULL) abort_app("malloc");

    for (int j = 0; j < npeers; ++j) {
        for (int k = 0; k < nmsgs; ++k) {
            size_t base = ((size_t) j * (size_t) nmsgs + (size_t) k) * nbytes;
            for (size_t b = 0; b < nbytes; ++b) {
                host[base + b] = pattern_byte(rank, k, b);
            }
        }
    }

    bench_memcpy_from_host(buf, host, total);
    bench_sync();
    free(host);
}

static void verify_recv_buffer(char* buf, int rank, int* recv_peers, int npeers, int nmsgs, size_t nbytes)
{
    size_t total = (size_t) npeers * (size_t) nmsgs * nbytes;
    unsigned char* host = (unsigned char*) malloc(total);
    if (host == NULL) abort_app("malloc");

    bench_memcpy_to_host(host, buf, total);
    bench_sync();

    for (int j = 0; j < npeers; ++j) {
        int src_rank = recv_peers[j];
        for (int k = 0; k < nmsgs; ++k) {
            size_t base = ((size_t) j * (size_t) nmsgs + (size_t) k) * nbytes;
            for (size_t b = 0; b < nbytes; ++b) {
                unsigned char expected = pattern_byte(src_rank, k, b);
                if (host[base + b] != expected) {
                    fprintf(stderr,
                            "Verification failed on rank %d at recv_slot=%d msg=%d byte=%zu: got=%u expected=%u src=%d\n",
                            rank, j, k, b,
                            (unsigned int) host[base + b],
                            (unsigned int) expected,
                            src_rank);
                    free(host);
                    MPI_Abort(MPI_COMM_WORLD, 1);
                }
            }
        }
    }

    free(host);
}

/* constants */
const int magic_tag = 1;

/* configuration parameters - setable by command line arguments */
int npeers = 6;
int niters = 4096;
int nmsgs = 128;
size_t nbytes = 8;
size_t cache_size = (8 * 1024 * 1024 / sizeof(int));
int ppn = -1;
int machine_output = 0;
int verify = 0;
/* globals */
int *send_peers;
int *recv_peers;
int *cache_buf;
char *send_buf;
char *recv_buf;
MPI_Request *reqs;

int rank = -1;
int world_size = -1;

static void
cache_invalidate(void)
{
#if defined(ENABLE_CUDA) || defined(ENABLE_HIP)
    return;
#else
    int i;
    cache_buf[0] = 1;
    for (i = 1 ; i < cache_size ; ++i) {
        cache_buf[i] = cache_buf[i - 1];
    }
#endif
}


static inline double
timer(void)
{
    return MPI_Wtime();
}


void
display_result(const char *test, const double result)
{
    if (0 == rank) {
        if (machine_output) {
            printf("%.2f ", result);
        } else {
            printf("%10s: %.2f\n", test, result);
        }
    }
}


void
test_one_way(void)
{
    int i, k, nreqs;
    double tmp, total = 0;
    MPI_Comm comm;

    MPI_Barrier(MPI_COMM_WORLD);

    if (world_size % 2 == 1) {
        MPI_Comm_split(MPI_COMM_WORLD,
                       (rank == world_size - 1) ? MPI_UNDEFINED : 1,
                       rank, &comm);
    } else {
        MPI_Comm_dup(MPI_COMM_WORLD, &comm);
    }

    if (!(world_size % 2 == 1 && rank == (world_size - 1))) {
        if (rank < world_size / 2) {
            for (i = 0 ; i < niters ; ++i) {
                cache_invalidate();

                MPI_Barrier(comm);

                tmp = timer();
                nreqs = 0;
                for (k = 0 ; k < nmsgs ; ++k) {
                    MPI_Isend(send_buf + (nbytes * k),
                              nbytes, MPI_CHAR, rank + (world_size / 2), magic_tag, 
                              comm, &reqs[nreqs++]);
                }
                MPI_Waitall(nreqs, reqs, MPI_STATUSES_IGNORE);
                total += (timer() - tmp);
            }
        } else {
            for (i = 0 ; i < niters ; ++i) {
                cache_invalidate();

                MPI_Barrier(comm);

                tmp = timer();
                nreqs = 0;
                for (k = 0 ; k < nmsgs ; ++k) {
                    MPI_Irecv(recv_buf + (nbytes * k),
                              nbytes, MPI_CHAR, rank - (world_size / 2), magic_tag, 
                              comm, &reqs[nreqs++]);
                }
                MPI_Waitall(nreqs, reqs, MPI_STATUSES_IGNORE);
                total += (timer() - tmp);
            }
        }

        MPI_Allreduce(&total, &tmp, 1, MPI_DOUBLE, MPI_SUM, comm);
        display_result("single direction", (niters * nmsgs) / (tmp / world_size));

        MPI_Comm_free(&comm);
    }

    MPI_Barrier(MPI_COMM_WORLD);
}


void
test_same_direction(void)
{
    int i, j, k, nreqs;
    double tmp, total = 0;

    MPI_Barrier(MPI_COMM_WORLD);

    for (i = 0 ; i < niters ; ++i) {
        cache_invalidate();

        MPI_Barrier(MPI_COMM_WORLD);

        tmp = timer();
        for (j = 0 ; j < npeers ; ++j) {
            nreqs = 0;
            for (k = 0 ; k < nmsgs ; ++k) {
                MPI_Irecv(recv_buf + (nbytes * (k + j * nmsgs)),
                          nbytes, MPI_CHAR, recv_peers[j], magic_tag, 
                          MPI_COMM_WORLD, &reqs[nreqs++]);
            }
            for (k = 0 ; k < nmsgs ; ++k) {
                MPI_Isend(send_buf + (nbytes * (k + j * nmsgs)),
                          nbytes, MPI_CHAR, send_peers[npeers - j - 1], magic_tag, 
                          MPI_COMM_WORLD, &reqs[nreqs++]);
            }
            MPI_Waitall(nreqs, reqs, MPI_STATUSES_IGNORE);
        }
        total += (timer() - tmp);
    }

    MPI_Allreduce(&total, &tmp, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    display_result("pair-based", (niters * npeers * nmsgs * 2) / (tmp / world_size));
}


void
test_prepost(void)
{
    int i, j, k, nreqs = 0;
    double tmp, total = 0;

    MPI_Barrier(MPI_COMM_WORLD);

    tmp = timer();
    for (j = 0 ; j < npeers ; ++j) {
        for (k = 0 ; k < nmsgs ; ++k) {
            MPI_Irecv(recv_buf + (nbytes * (k + j * nmsgs)),
                      nbytes, MPI_CHAR, recv_peers[j], magic_tag, 
                      MPI_COMM_WORLD, &reqs[nreqs++]);
        }
    }
    total += (timer() - tmp);

    for (i = 0 ; i < niters - 1 ; ++i) {
        cache_invalidate();

        MPI_Barrier(MPI_COMM_WORLD);

        tmp = timer();
        for (j = 0 ; j < npeers ; ++j) {
            for (k = 0 ; k < nmsgs ; ++k) {
                MPI_Isend(send_buf + (nbytes * (k + j * nmsgs)),
                          nbytes, MPI_CHAR, send_peers[npeers - j - 1], magic_tag, 
                          MPI_COMM_WORLD, &reqs[nreqs++]);
            }
        }
        MPI_Waitall(nreqs, reqs, MPI_STATUSES_IGNORE);
        nreqs = 0;
        for (j = 0 ; j < npeers ; ++j) {
            for (k = 0 ; k < nmsgs ; ++k) {
                MPI_Irecv(recv_buf + (nbytes * (k + j * nmsgs)),
                          nbytes, MPI_CHAR, recv_peers[j], magic_tag, 
                          MPI_COMM_WORLD, &reqs[nreqs++]);
            }
        }
        total += (timer() - tmp);
    }
    tmp = timer();
    for (j = 0 ; j < npeers ; ++j) {
        for (k = 0 ; k < nmsgs ; ++k) {
            MPI_Isend(send_buf + (nbytes * (k + j * nmsgs)),
                      nbytes, MPI_CHAR, send_peers[npeers - j - 1], magic_tag, 
                      MPI_COMM_WORLD, &reqs[nreqs++]);
        }
    }
    MPI_Waitall(nreqs, reqs, MPI_STATUSES_IGNORE);
    total += (timer() - tmp);

    MPI_Allreduce(&total, &tmp, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    display_result("pre-post", (niters * npeers * nmsgs * 2) / (tmp / world_size));
}


void
test_allstart(void)
{
    int i, j, k, nreqs = 0;
    double tmp, total = 0;

    MPI_Barrier(MPI_COMM_WORLD);

    for (i = 0 ; i < niters ; ++i) {
        cache_invalidate();

        MPI_Barrier(MPI_COMM_WORLD);

        tmp = timer();
        nreqs = 0;
        for (j = 0 ; j < npeers ; ++j) {
            for (k = 0 ; k < nmsgs ; ++k) {
                MPI_Irecv(recv_buf + (nbytes * (k + j * nmsgs)),
                          nbytes, MPI_CHAR, recv_peers[j], magic_tag, 
                          MPI_COMM_WORLD, &reqs[nreqs++]);
            }
            for (k = 0 ; k < nmsgs ; ++k) {
                MPI_Isend(send_buf + (nbytes * (k + j * nmsgs)),
                          nbytes, MPI_CHAR, send_peers[npeers - j - 1], magic_tag, 
                          MPI_COMM_WORLD, &reqs[nreqs++]);
            }
        }
        MPI_Waitall(nreqs, reqs, MPI_STATUSES_IGNORE);
        total += (timer() - tmp);
    }

    MPI_Allreduce(&total, &tmp, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    display_result("all-start", (niters * npeers * nmsgs * 2) / (tmp / world_size));
}


void
usage(void)
{
    fprintf(stderr, "Usage: msgrate -n <ppn> [OPTION]...\n\n");
    fprintf(stderr, "  -h           Display this help message and exit\n");
    fprintf(stderr, "  -p <num>     Number of peers used in communication\n");
    fprintf(stderr, "  -i <num>     Number of iterations per test\n");
    fprintf(stderr, "  -m <num>     Number of messages per peer per iteration\n");
    fprintf(stderr, "  -s <size>    Number of bytes per message\n");
    fprintf(stderr, "  -c <size>    Cache size in bytes\n");
    fprintf(stderr, "  -n <ppn>     Number of procs per node\n");
    fprintf(stderr, "  -o           Format output to be machine readable\n");
    fprintf(stderr, "  -v           Verify message contents\n");
    fprintf(stderr, "\nReport bugs to <mdosanj@sandia.gov>\n");
}


int
main(int argc, char *argv[])
{
    int start_err = 0;
    int i;


    #if defined(ENABLE_CUDA) || defined(ENABLE_HIP)
    if (bench_gpu_init() != 0) {
        exit(1);
    }
    #endif

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    /* root handles arguments and bcasts answers */
    if (0 == rank) {
        int ch;
        while (start_err != 1 && 
               (ch = getopt(argc, argv, "p:i:m:s:c:n:ovh")) != -1) {
            switch (ch) {
            case 'p':
                npeers = atoi(optarg);
                break;
            case 'i':
                niters = atoi(optarg);
                break;
            case 'm':
                nmsgs = atoi(optarg);
                break;
            case 's':
                nbytes = atoi(optarg);
                break;
            case 'c':
                cache_size = atoi(optarg) / sizeof(int);
                break;
            case 'n':
                ppn = atoi(optarg);
                break;
            case 'o':
                machine_output = 1;
                break;
            case 'v':
                verify = 1;
                break;
            case 'h':
            case '?':
            default:
                start_err = 1;
                usage();
            }
        }

        /* sanity check */
        if (start_err != 1) {
            if (world_size < 3) {
                fprintf(stderr, "Error: At least three processes are required\n");
                start_err = 1;
            } else if (world_size <= npeers) {
                fprintf(stderr, "Error: job size (%d) <= number of peers (%d)\n",
                        world_size, npeers);
                start_err = 1;
            } else if (ppn < 1) {
                fprintf(stderr, "Error: must specify process per node (-n #)\n");
                start_err = 1;
            } else if (world_size / ppn <= npeers) {
                fprintf(stderr, "Error: node count <= number of peers\n");
                start_err = 1;
            }
        }
    }

    /* broadcast results */
    MPI_Bcast(&start_err, 1, MPI_INT, 0, MPI_COMM_WORLD);
    if (0 != start_err) {
        MPI_Finalize();
        exit(1);
    }
    MPI_Bcast(&npeers, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Bcast(&niters, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Bcast(&nmsgs, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Bcast(&nbytes, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Bcast(&cache_size, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Bcast(&ppn, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Bcast(&verify, 1, MPI_INT, 0, MPI_COMM_WORLD);
    if (0 == rank) {
        if (!machine_output) {
            printf("job size:   %d\n", world_size);
            printf("npeers:     %d\n", npeers);
            printf("niters:     %d\n", niters);
            printf("nmsgs:      %d\n", nmsgs);
            printf("nbytes:     %d\n", nbytes);
            printf("cache size: %d\n", cache_size * (int)sizeof(int));
            printf("ppn:        %d\n", ppn);
#if defined(ENABLE_CUDA)
            printf("CUDA\n");
#elif defined(ENABLE_HIP)
            printf("HIP\n");
#endif
        } else {
            printf("%d %d %d %d %d %d %d ", 
                   world_size, npeers, niters, nmsgs, nbytes,
                   cache_size * (int)sizeof(int), ppn);
        }
    }

    size_t buffer_size = npeers * nmsgs * nbytes;

    /* allocate buffers */
    send_peers = malloc(sizeof(int) * npeers);
    if (NULL == send_peers) abort_app("malloc");
    recv_peers = malloc(sizeof(int) * npeers);
    if (NULL == recv_peers) abort_app("malloc");
    cache_buf = malloc(sizeof(int) * cache_size);
    if (NULL == cache_buf) abort_app("malloc");
    /* Call the functions so we can determine where to allocate.*/
    send_buf = (char*) bench_alloc(buffer_size);
    recv_buf = (char*) bench_alloc(buffer_size);
    reqs = malloc(sizeof(MPI_Request) * 2 * nmsgs * npeers);
    if (NULL == reqs) abort_app("malloc");

    /* calculate peers */
    for (i = 0 ; i < npeers ; ++i) {
        if (i < npeers / 2) {
            send_peers[i] = (rank + world_size + ((i - npeers / 2) * ppn)) % world_size;
        } else {
            send_peers[i] = (rank + world_size + ((i - npeers / 2 + 1) * ppn)) % world_size;
        }
    }
    if (npeers % 2 == 0) {
        /* even */
        for (i = 0 ; i < npeers ; ++i) {
            if (i < npeers / 2) {
                recv_peers[i] = (rank + world_size + ((i - npeers / 2) *ppn)) % world_size;
            } else {
                recv_peers[i] = (rank + world_size + ((i - npeers / 2 + 1) * ppn)) % world_size;
            }
        } 
    } else {
        /* odd */
        for (i = 0 ; i < npeers ; ++i) {
            if (i < npeers / 2 + 1) {
                recv_peers[i] = (rank + world_size + ((i - npeers / 2 - 1) * ppn)) % world_size;
            } else {
                recv_peers[i] = (rank + world_size + ((i - npeers / 2) * ppn)) % world_size;
            }
        }
    }

    /* BWB: FIX ME: trash the free lists / malloc here */

    /* sync, although tests will do this on their own (in theory) */
    MPI_Barrier(MPI_COMM_WORLD);

    /* run tests */
    test_one_way();
    test_same_direction();
    test_prepost();
    test_allstart();

    if (rank == 0 && machine_output) printf("\n");
    
    // Clean-up
    free(send_peers);
    free(recv_peers);
    free(cache_buf);
    free(reqs);
    bench_free(send_buf);
    bench_free(recv_buf);
    bench_cleanup();

    /* done */
    MPI_Finalize();
    return 0;
}
