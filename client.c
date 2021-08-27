/*******************************************************************************
 * client.c      Author: Ian Nobile
 *
 * Client of a client-server-based search application sending requests from a 
 * text file to a single server before exiting. Employs the use of shared 
 * memory, semaphores, readers/writers locks,forking child processes and 
 * multi-threading. This program is leak free.
 *
*******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/stat.h>        /* For mode constants */
#include <fcntl.h>           /* For O_* constants */

#include <semaphore.h>

#define MAXDIRPATH 1024
#define MAXKEYWORD 256
#define MAXLINESIZE 1024
#define MAXOUTSIZE 2048

#define SEM_CSEMPTY "/csEmpty"// if there is no request in the queue, server process should go into sleep (block -- sem!).    SERVER
#define SEM_CSFULL "/csFull"// if a client process finds the request queue full, then it should go into sleep (block -- sem!) until there is space for one more request. CLIENT
#define SEM_CSMUTEX "/csMutex"// daily lock   CLIENT/SERVER


//------------------------------------------------------------------------------
//  Structs and semaphores
//------------------------------------------------------------------------------

struct Request {
    char request[MAXDIRPATH + 1 + MAXKEYWORD];
};

sem_t *csEmpty;
sem_t *csFull;
sem_t *csMutex;


//------------------------------------------------------------------------------
//  The Main Function
//------------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    
    //  handle command line args:    
    // ./client <req-queue-size> <inputfile>
    int iReqQSize;
    char cInputFilepath[256];
    if (argc > 3) {
        printf("Sorry, but something's not quite right about your invocation.");
        return 1;
    } else {
        iReqQSize = atoi(argv[1]);
        strcpy(cInputFilepath, argv[2]);
    }

    const char *name = "shmpath";
    const int SIZE = (2 * sizeof(int)) + ((MAXKEYWORD + 1 + MAXDIRPATH) * iReqQSize);

    int shm_fd;
    void *ptr;

    //open the shared memory segment
    shm_fd = shm_open(name, O_RDWR, 0666);
    if (shm_fd == -1) {
        printf("shared memory failed\n");
        exit(-1);
    }

    //now map the shared memory segment in the address space of the process
    ptr = mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED) {
        printf("Map failed\n");
        exit(-1);
    }

    // buffer starts after in & out
    struct Request *buffer = (struct Request *)(2 * sizeof(int) + ptr);
    int *in = (int *)ptr;

    //    create and initialise semaphores
    if ((csEmpty = sem_open(SEM_CSEMPTY, iReqQSize)) == SEM_FAILED) {
        printf("There was an error opening \"empty\".");
    }   // iReqSize - 1?
    if ((csFull = sem_open(SEM_CSFULL, 0)) == SEM_FAILED) {
        printf("There was an error opening \"full\".");
    }
    if ((csMutex = sem_open(SEM_CSMUTEX, 1)) == SEM_FAILED) {
        printf("There was an error opening \"mutex\".");
    }

    //  opening the input file
    FILE *file;
    file = fopen(cInputFilepath, "r");
    if (file == NULL) {
        printf("Sorry, but there seems to be no such file at %s.\n", cInputFilepath);
        return 1;
    }

    //  query importation
    struct Request *newRequest = malloc(sizeof(struct Request));

    while (fgets(newRequest->request, MAXDIRPATH + 1 + MAXKEYWORD + 1, file) != 0) {
        
        sem_wait(csEmpty);
        sem_wait(csMutex);

        // critical section
        strtok(newRequest->request, "\n");
        buffer[*in] = *newRequest;
        *in = (*in + 1) % iReqQSize;

        sem_post(csMutex);
        sem_post(csFull);

        // remainder section
        
    }

    fclose(file);

    free(newRequest);

    if (sem_close(csEmpty) == -1) {
        printf("There was an error closing \"empty\".\n");
    }
    if (sem_close(csFull) == -1) {
        printf("There was an error closing \"full\".\n");
    }
    if (sem_close(csMutex) == -1) {
        printf("There was an error closing \"mutex\".\n");
    }


    //  stop Valgrind's "FILE DESCRIPTORS open at exit" error:
    fclose(stdin);
    fclose(stdout);
    fclose(stderr);

    //  obtain user confirmation before exiting
    getchar();
    return 0;

}
