/* -*- C -*-
 *
 * Copyright 2006 & 2016 Sandia Corporation. Under the terms of Contract
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
#include <pthread.h>
#include <shmem.h>
#include <sys/time.h>
/* constants */
const int magic_tag = 1;

/* configuration parameters - setable by command line arguments */
int DEBUG = 0;
int npeers = 6;
int niters = 4096;
int nmsgs = 128;
int nbytes = 8;
int cache_size = (8 * 1024 * 1024 / sizeof(int));
int ppn = -1;
int machine_output = 0;
int threads = 1;
int rma_mode = 2;
int rma_op   = 0;
/* globals */
int *send_peers;
int *recv_peers;
int *cache_buf;
char *send_buf;
char *recv_buf;
MPI_Request *reqs;

//int rank = -1;
int world_size = -1;

int nthreads, *nth;
int *npeers_buf;
long pSync[SHMEM_BCAST_SYNC_SIZE], qSync[SHMEM_BCAST_SYNC_SIZE];
double  pWrk[SHMEM_BCAST_SYNC_SIZE];
int nprocs;

typedef struct {
    int thread_id;
    int iters;
    MPI_Request *local_reqs;
    int rank;
    char *send_buf;
    //  MPI_Comm *comm;
} thread_input;

static void
abort_app(const char *msg)
{
    perror(msg);
    //MPI_Abort(MPI_COMM_WORLD, 1);
    exit(-1);
}


static void
cache_invalidate(void)
{
    int i;

    cache_buf[0] = 1;
    for (i = 1 ; i < cache_size ; ++i) {
        cache_buf[i] = cache_buf[i - 1];
    }
}


static inline double
timer(void)
{
    
    struct timeval t;
    double dt;
    double dut;
    gettimeofday(&t, NULL);
    dt = (double) t.tv_sec;
    dut = (double) t.tv_usec;
    dt = dt + (dut /1000000.0);
    return dt;
}

void display_result(const char *test, const double result)
{
    int me = shmem_my_pe();
    if (0 == me) {
        if (machine_output) {
            printf("%.2f ", result);
        } else {
            printf("%10s: %.2f\n", test, result);
        }
    }
}


/*********************Start Time FUNCS*************************/

double* thread_etimes;

double find_max(){
    double max = 0;
    int i;
    for (i = 0; i < threads; i++)
	if(max < thread_etimes[i]) max=thread_etimes[i];

    return max;
}

/*********************End Time FUNCS***************************/

volatile int count = 0;
pthread_mutex_t cntlk;
pthread_cond_t cntcond;

/*********************Start RMA FUNCS**************************/
MPI_Win win;
MPI_Win win2;

MPI_Group send_comm_group, send_group;
MPI_Group recv_comm_group, recv_group;

void setupwindow(int single_dir)
{
    int me;

    //shmem_init_thread(SHMEM_THREAD_MULTIPLE);
    //nprocs = shmem_n_pes();
    me = shmem_my_pe();

    ////////////////////////////////////////////////////////////////////////////////
    nth=shmem_malloc(sizeof(int));
    for (int i = 0; i < SHMEM_BCAST_SYNC_SIZE; i++) {
	pSync[i] = SHMEM_SYNC_VALUE;
    }
    shmem_barrier_all();
    *nth = threads;
    //shmem_broadcast32(nth, nth, 1, 0, 0, 0, 2, pSync);
    shmem_broadcast32(nth, nth, nprocs, 0, 0, 0, nprocs, pSync);
    threads = *nth;
    
    npeers_buf=shmem_malloc(sizeof(int));
    shmem_broadcast32(npeers_buf, &npeers, 1, 0, 0, 0, nprocs, pSync);
    ////////////////////////////////////////////////////////////////////////////////

    //printf("shmem_malloc send_buf: %d x %d x %d\n", npeers, nmsgs, nbytes);
    send_buf = (char *)shmem_malloc(npeers * nmsgs * nbytes);
    //send_buf = (char *)shmalloc(npeers * nmsgs * nbytes);
    if (NULL == send_buf) abort_app("malloc");
    
    recv_buf = (char *)shmem_malloc(npeers * nmsgs * nbytes);
    //recv_buf = (char *)shmalloc(npeers * nmsgs * nbytes);
    if (NULL == recv_buf) abort_app("malloc");
}


void starttransfer(int single_dir)
{
    if(rma_mode == 0){
	shmem_fence();
	shmem_barrier_all();    
    }
    else if(rma_mode == 1){
	shmem_quiet();
	shmem_barrier_all();
    }
    else if(rma_mode == 2){
	shmem_barrier_all();
    }

}

void transfer(int offset, int dest0)//TODO remove dest0 when shmem calcs address correctly
{
    char *src = send_buf + offset;
    char *dest = recv_buf + offset;
    int nextpe = (shmem_my_pe() +1)%world_size;

    ///shmem_put
    if(rma_op){
	shmem_putmem(dest, src, nbytes * sizeof(char), nextpe);
    }

    ///shmem_get
    else{
	shmem_getmem(dest, src, nbytes * sizeof(char), nextpe);
    }
}

void endtransfer(int single_dir)
{
  
    if(rma_mode == 0){
	shmem_fence();
	shmem_barrier_all();    
    }
    else if(rma_mode == 1){
	shmem_quiet();
	shmem_barrier_all();
    }
    else if(rma_mode == 2){
	shmem_barrier_all();
    }

}

void destroywindow()
{
    /* shfree */
    shmem_free(send_buf);
    shmem_free(recv_buf);
}
/***********************End RMA FUNCS**************************/


void *sendmsg(void * input)
{
    int nreqs= 0;
    int i, j;
    thread_input *t_info = (thread_input *)input;

    for (i = 0 ; i < niters ; ++i) 
	{
	    pthread_mutex_lock(&cntlk);
	    count++;
	    pthread_cond_wait(&cntcond, &cntlk);
	    pthread_mutex_unlock(&cntlk);

	    for (j= 0; j < t_info->iters; j++)
		{
		    transfer(((t_info->thread_id * t_info->iters * nbytes) + (nbytes * j)), t_info->rank + (world_size / 2));
		}

	    pthread_mutex_lock(&cntlk);
	    count--;
	    pthread_cond_wait(&cntcond, &cntlk);
	    pthread_mutex_unlock(&cntlk);
	}
    //printf("Thread %d finished!\n", t_info->thread_id);
    return NULL;
}

void
test_one_way(void)
{
    int i, k, nreqs;
    double tmp, total = 0;
    double stt, ttt=0;
    int me = shmem_n_pes();
    int rank = me;
    setupwindow(1);

    shmem_barrier_all();
    
    if(DEBUG) printf("entering one_way\n");

    thread_input *info;
    info = (thread_input *) malloc(threads*sizeof(thread_input)*2);
    pthread_t *pthreads;
    pthreads = (pthread_t*) malloc(threads*sizeof(pthreads)*5);
    //reqs = malloc(sizeof(MPI_Request) * 2 * nmsgs * npeers * threads);
    reqs = malloc(2 * nmsgs *npeers * threads);
    char *local_sendbuf = malloc(npeers * nmsgs * nbytes);
    if (!(world_size % 2 == 1 && rank == (world_size - 1))) {
        if (rank < world_size / 2) {
            int thread_count = 0;
            for (thread_count = 0; thread_count < threads; thread_count++){
                info[thread_count].thread_id = thread_count;
                info[thread_count].iters = nmsgs/threads;
                info[thread_count].local_reqs = &reqs[(nmsgs/threads)*thread_count];
                info[thread_count].rank = rank;
                pthread_create(&pthreads[thread_count], NULL, sendmsg, (void *)&info[thread_count]);
            }

            for (i = 0 ; i < niters ; ++i) {
                cache_invalidate();
                //nreqs = 0;
                while (count != thread_count) {
                }
                pthread_mutex_lock(&cntlk);
                tmp = timer();
                starttransfer(1);
                pthread_cond_broadcast(&cntcond);
                pthread_mutex_unlock(&cntlk);

                while (count != 0) {
                }
                pthread_mutex_lock(&cntlk);
                endtransfer(1);
                total += (timer() - tmp);
                pthread_cond_broadcast(&cntcond);
                pthread_mutex_unlock(&cntlk);
            }

        } else {
            for (i = 0 ; i < niters ; ++i) {
                cache_invalidate();
                tmp = timer();
                starttransfer(1);
                endtransfer(1);
                total += (timer() - tmp);
            }
        }
	
	int nred=1;

	shmem_barrier_all();
	//shmem_int_sum_to_all(dest, source, nred, 0, 0, 4, pWrk, pSync);
	shmem_double_sum_to_all(&tmp, &total, 1, 0, 0, 1, pWrk, pSync);

        //MPI_Allreduce(&total, &tmp, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
        display_result("single direction ", (niters * nmsgs * ((world_size)/2)) / (tmp / world_size));
        //display_result("single dir-TO    ", (niters * nmsgs * ((world_size)/2)) / ((tmp - ttt) / world_size));
    }
    
    //compare buffers for match

    if(me==0){
	int diff = memcmp(recv_buf, send_buf, (npeers * nmsgs * nbytes));
	if(diff){
	    printf("buffers differ\n");
	}
    }
    
    free(info);
    free(pthreads);
    free(reqs);
    free(local_sendbuf);
    destroywindow();
    shmem_barrier_all();
}

void *all_run(void * input)
{
    int nreqs= 0;
    int i,j,k;
    thread_input *t_info = (thread_input *)input;
    for (i = 0 ; i < niters ; ++i) {
        pthread_mutex_lock(&cntlk);
        count++;
        pthread_cond_wait(&cntcond, &cntlk);
        pthread_mutex_unlock(&cntlk);
        for (j = 0 ; j < npeers ; ++j) {
            for (k = 0 ; k < t_info->iters ; ++k) {
		transfer(nbytes * (k + t_info->thread_id * t_info->iters + j * nmsgs), send_peers[npeers - j - 1]);
            }
        }
        pthread_mutex_lock(&cntlk);
        count--;
        pthread_cond_wait(&cntcond, &cntlk);
        pthread_mutex_unlock(&cntlk);
    }
    return NULL;
}

void
test_all(void)
{
    int i, k, nreqs;
    double tmp, total = 0;
    double stt, ttt=0;
    int me = shmem_n_pes();
    int rank=me;
    setupwindow(0);
    shmem_barrier_all();

    if(DEBUG) printf("entering test_all\n");

    thread_input *info;
    info = (thread_input *) malloc(threads*sizeof(thread_input)*2);
    pthread_t *pthreads;
    pthreads = (pthread_t*) malloc(threads*sizeof(pthreads)*5);
    //reqs = malloc(sizeof(MPI_Request) * 2 * nmsgs * npeers * threads);
    char *local_sendbuf = malloc(npeers * nmsgs * nbytes);
    int thread_count = 0;
    for (thread_count = 0; thread_count < threads; thread_count++){
        info[thread_count].thread_id = thread_count;
        info[thread_count].iters = nmsgs/threads;
        info[thread_count].local_reqs = &reqs[2*(nmsgs/threads)*npeers*thread_count];
        info[thread_count].rank = rank;
        pthread_create(&pthreads[thread_count], NULL, all_run, (void *)&info[thread_count]);
    }    
    

    for (i = 0 ; i < niters ; ++i) {
        cache_invalidate();

        while (count != thread_count) {
        }
        pthread_mutex_lock(&cntlk);
        tmp = timer();
        starttransfer(0);
        pthread_cond_broadcast(&cntcond);
        pthread_mutex_unlock(&cntlk);

        while (count != 0) {
        }
        pthread_mutex_lock(&cntlk);
        endtransfer(0);
        total += (timer() - tmp);
        pthread_cond_broadcast(&cntcond);
        pthread_mutex_unlock(&cntlk);
    }

    shmem_barrier_all();
    shmem_double_sum_to_all(&tmp, &total, 1, 0, 0, 1, 0, pSync);
    //MPI_Allreduce(&total, &tmp, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    
    display_result("halo-exchange    ", (niters * npeers * nmsgs * 2) / (tmp / world_size));
    free(info);
    free(pthreads);
    free(reqs);
    free(local_sendbuf);
    shmem_barrier_all();
    destroywindow();
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
    //fprintf(stderr, "  -r           RMA Sych: 0-Fence, 1-Lock Unlock, 2-PSWC\n");
    fprintf(stderr, "  -u           SHMEM Op: 0-Get, 1-Put\n");
    fprintf(stderr, "\nReport bugs to <bwbarre@sandia.gov>\n");
}


int
main(int argc, char *argv[])
{
    int start_err = 0;
    int i;
    int prov;
    
    /*
      MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &prov);
      MPI_Comm_rank(MPI_COMM_WORLD, &rank);
      MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    */

    pthread_mutex_init(&cntlk, NULL);
    pthread_cond_init(&cntcond, NULL);

    /* root handles arguments and bcasts answers */
    int ch;
    while (start_err != 1 && 
	   (ch = getopt(argc, argv, "p:i:m:s:c:n:o:t:r:u:h")) != -1) {
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
	case 't':
	    threads = atoi(optarg);
	    break;
	case 'r':
	    rma_mode = atoi(optarg);
	    break;
	case 'u':
	    rma_op = atoi(optarg);
	    break;
	case 'h':
	case '?':
	default:
	    start_err = 1;
	    usage();
	}
    }

    /* initialize SHMEM */
    shmem_init_thread(SHMEM_THREAD_MULTIPLE);
    nprocs = shmem_n_pes();
    world_size=shmem_n_pes();
    int me = shmem_my_pe();

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
	} else if ((double)world_size / (double)ppn <= (double)npeers) {
	    fprintf(stderr, "Error: node count <= number of peers %i / %i <= %i\n",world_size,ppn,npeers);
	    start_err = 1;
	} else if (world_size % 2 == 1) {
	    fprintf(stderr, "Error: node count of %d isn't even.\n", world_size);
	    start_err = 1;
	} else if (threads < 1) {
            fprintf(stderr, "Error: thread count %d is less than 1\n", threads);
            start_err = 1;
        }
    }

    /* broadcast results */
    //TODO get rid of ranks, all niters will match?
    //MPI_Bcast(&npeers, 1, MPI_INT, 0, MPI_COMM_WORLD);
    //MPI_Bcast(&niters, 1, MPI_INT, 0, MPI_COMM_WORLD);
    //niters=128;
    //printf("niters = %d\n", niters);
    /*
      MPI_Bcast(&nmsgs, 1, MPI_INT, 0, MPI_COMM_WORLD);
      MPI_Bcast(&nbytes, 1, MPI_INT, 0, MPI_COMM_WORLD);
      MPI_Bcast(&cache_size, 1, MPI_INT, 0, MPI_COMM_WORLD);
      MPI_Bcast(&ppn, 1, MPI_INT, 0, MPI_COMM_WORLD);
      //MPI_Bcast(&threads, 1, MPI_INT, 0, MPI_COMM_WORLD);
      MPI_Bcast(&rma_mode, 1, MPI_INT, 0, MPI_COMM_WORLD);
      MPI_Bcast(&rma_op, 1, MPI_INT, 0, MPI_COMM_WORLD);
    */

    if(me == 0){
        if (!machine_output) {
            printf("job size         : %d\n", world_size);
            printf("npeers           : %d\n", npeers);
            printf("niters           : %d\n", niters);
            printf("nmsgs            : %d\n", nmsgs);
            printf("nbytes           : %d\n", nbytes);
            printf("cache size       : %d\n", cache_size * (int)sizeof(int));
            printf("ppn              : %d\n", ppn);
            printf("threads          : %d\n", threads);
            printf("synch mode       : ");
            switch(rma_mode)
		{
                case 0:
		    //printf("fence\n");
		    printf("shmem fence\n");
		    break;
                case 1:
		    //printf("lock/unlock\n");
		    printf("shmem quiet\n");
		    break;
                case 2:
		    //printf("post start wait complete\n");
		    printf("shmem barrier all\n");
		    break;
		}
            printf("SHMEM Op         : ");
            switch(rma_op)
		{
                case 0:
		    printf("shmem get\n");
		    break;
                case 1:
		    printf("shmem put\n");
		    break;
		}
        } else {
            printf("%d %d %d %d %d %d %d %d %d %d ", 
                   world_size, npeers, niters, nmsgs, nbytes,
                   cache_size * (int)sizeof(int), ppn, threads, rma_mode, rma_op);
        }
    }

    /* allocate buffers */
    send_peers = malloc(sizeof(int) * npeers);
    if (NULL == send_peers) abort_app("malloc");
    recv_peers = malloc(sizeof(int) * npeers);
    if (NULL == recv_peers) abort_app("malloc");
    cache_buf = malloc(sizeof(int) * cache_size);
    if (NULL == cache_buf) abort_app("malloc");
    thread_etimes = malloc(sizeof(double)*threads);    
    //reqs = malloc(sizeof(MPI_Request) * 2 * nmsgs * npeers);
    reqs = malloc(2 * nmsgs *npeers * threads);
    if (NULL == reqs) abort_app("malloc");
    /**/
    send_buf = malloc(npeers * nmsgs * nbytes);
    if (NULL == send_buf) abort_app("malloc");
    
    recv_buf = malloc(npeers * nmsgs * nbytes);
    if (NULL == recv_buf) abort_app("malloc");

    /* calculate peers */
    for (i = 0 ; i < npeers ; ++i) {
        if (i < npeers / 2) {
            //send_peers[i] = (rank + world_size + ((i - npeers / 2) * ppn)) % world_size;
	    send_peers[i] = (me + world_size + ((i - npeers / 2) * ppn)) % world_size;
	    
        } else {
            //send_peers[i] = (rank + world_size + ((i - npeers / 2 + 1) * ppn)) % world_size;
	    send_peers[i] = (me + world_size + ((i - npeers / 2 + 1) * ppn)) % world_size;
        }
    }
    if (npeers % 2 == 0) {
        /* even */
        for (i = 0 ; i < npeers ; ++i) {
            if (i < npeers / 2) {
                //recv_peers[i] = (rank + world_size + ((i - npeers / 2) *ppn)) % world_size;
		recv_peers[i] = (me + world_size + ((i - npeers / 2) *ppn)) % world_size;
            } else {
                //recv_peers[i] = (rank + world_size + ((i - npeers / 2 + 1) * ppn)) % world_size;
		recv_peers[i] = (me + world_size + ((i - npeers / 2 + 1) * ppn)) % world_size;
            }
        } 
    } else {
        /* odd */
        for (i = 0 ; i < npeers ; ++i) {
            if (i < npeers / 2 + 1) {
                //recv_peers[i] = (rank + world_size + ((i - npeers / 2 - 1) * ppn)) % world_size;
		recv_peers[i] = (me + world_size + ((i - npeers / 2 - 1) * ppn)) % world_size;
            } else {
                //recv_peers[i] = (rank + world_size + ((i - npeers / 2) * ppn)) % world_size;
		recv_peers[i] = (me + world_size + ((i - npeers / 2) * ppn)) % world_size;
            }
        }
    }

    /* BWB: FIX ME: trash the free lists / malloc here */

    /* sync, although tests will do this on their own (in theory) */

    /* run tests */
    test_one_way();
    test_all();
    if (me == 0 && machine_output) printf("\n");
    /* done */
    //MPI_Finalize();
    shmem_finalize();
    return 0;
}
