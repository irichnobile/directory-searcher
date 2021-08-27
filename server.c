/*******************************************************************************
 * server.c      Author: Ian Nobile
 *
 * Server of a client-server-based search application processing requests from
 * multiple clients and searching for keywords in all files located in a
 * specified directory. Employs the use of shared memory, semaphores, readers/
 * writers locks,forking child processes and multi-threading. This program is
 * leak free and consistently yields 26 results when run through the
 * assignment Blackbox testing.
 *
*******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
//shmem:
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
// truncate:
#include <unistd.h>
#include <sys/types.h>
#include <semaphore.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/wait.h>

#define MAXDIRPATH 1024
#define MAXKEYWORD 256
#define MAXLINESIZE 1024
#define MAXOUTSIZE 2048

#define SEM_CSEMPTY "/csEmpty"// if there is no request in the queue, server process should go into sleep (block -- sem!).    SERVER
#define SEM_CSFULL "/csFull"// if a client process finds the request queue full, then it should go into sleep (block -- sem!) until there is space for one more request. CLIENT
#define SEM_CSMUTEX "/csMutex"// daily lock   CLIENT/SERVER

#define SEM_THREADEMPTY "/threadEmpty"
#define SEM_THREADFULL "/threadFull"
#define SEM_THREADMUTEX "/threadMutex"


//------------------------------------------------------------------------------
//  Structs and semaphores
//------------------------------------------------------------------------------

struct Request {
    char request[MAXDIRPATH + 1 + MAXKEYWORD];
};

struct threadBufLegend {
    char **threadBuffer;
    int iBufferSize;
    int iThIn;
    int iThOut;
};

struct Search {
    char dirpath[MAXDIRPATH];
    char keyword[MAXKEYWORD];
    char filename[256];
    int lineNo;
    char fileLine[MAXOUTSIZE - MAXKEYWORD];
    char findText[MAXOUTSIZE];
    struct threadBufLegend *legend;
};

sem_t *csEmpty;
sem_t *csFull;
sem_t *csMutex;

sem_t threadEmpty;
sem_t threadFull;
sem_t threadMutex;


//------------------------------------------------------------------------------
//  Function prototypes
//------------------------------------------------------------------------------

void *workerFunction(void *newSearch);

void *printerFunction(void *bufLegend);


//------------------------------------------------------------------------------
//  The Main Function
//------------------------------------------------------------------------------
int main(int argc, char *argv[]) {

    //  handle command line args:
    // ./server <req-queue-size> <buffersize>
    int iReqQSize;
    int iBufferSize;
    if (argc > 3) {
        printf("Sorry, but something's not quite right about your invocation.\n");
        return 1;
    } else {
        iReqQSize = atoi(argv[1]);
        iBufferSize = atoi(argv[2]);
    }

    const int SIZE = (2 * sizeof(int)) + ((MAXKEYWORD + 1 + MAXDIRPATH) * iReqQSize);
    const char *name = "shmpath";

    int shm_fd;
    void *ptr;

    /* create the shared memory segment */
    shm_fd = shm_open(name, O_CREAT | O_RDWR, 0666);

    /* configure the size of the shared memory segment */
    ftruncate(shm_fd, SIZE);

    /* now map the shared memory segment in the address space of the process */
    ptr = mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED) {
        printf("Map failed\n");
        return -1;
    }

    // csBuffer starts after in & out
    struct Request *csBuffer;
    csBuffer = (struct Request *)(2 * sizeof(int) + ptr);
    int *in = (int *)ptr;
    int *out = (int *)(ptr + 1 * sizeof(int));
    *in = 0; // next free position
    *out = 0; // first full position

    // create and initialize cs- and thread-Buffer semaphores
    if ((csEmpty = sem_open(SEM_CSEMPTY, O_CREAT, 0666, iReqQSize)) == SEM_FAILED) {
        printf("There was an error opening \"csEmpty\".\n");
    }   // iReqSize - 1?
    if ((csFull = sem_open(SEM_CSFULL, O_CREAT, 0666, 0)) == SEM_FAILED) {
        printf("There was an error opening \"csFull\".\n");
    }
    if ((csMutex = sem_open(SEM_CSMUTEX, O_CREAT, 0666, 1)) == SEM_FAILED) {
        printf("There was an error opening \"csMutex\".\n");
    }

    struct Request *newRequest = malloc(sizeof(struct Request));
    pid_t pid;
    int iReqCount = 0;

    // read from csBuffer
    while (1) {
        sem_wait(csFull);
        sem_wait(csMutex);

        // critical section: remove an item from the csBuffer
        *newRequest = csBuffer[*out];
        *out = (*out + 1) % iReqQSize;

        // check for parent exit cond:
        if (strcmp(newRequest->request, "exit") == 0) {
            sem_post(csMutex);
            sem_post(csEmpty);

            free(newRequest);
            newRequest = NULL;

            if (sem_close(csEmpty) == -1) {
                printf("There was an error closing \"csEmpty\".\n");
            }
            if (sem_close(csFull) == -1) {
                printf("There was an error closing \"csFull\".\n");
            }
            if (sem_close(csMutex) == -1) {
                printf("There was an error closing \"csMutex\".\n");
            }
            sem_destroy(csEmpty);
            sem_destroy(csFull);
            sem_destroy(csMutex);

            break;
        }
        
        sem_post(csMutex);
        sem_post(csEmpty);

        // remainder section
        iReqCount++;
        pid = fork();
        if (pid < 0) { // if forking doesn't work:
            fprintf(stderr, "Fork failed\n");
            exit(-1);
        } else if (pid == 0) { // if child,
            break;
        } else { // if parent,
            continue;
        }
    } // end pt1 of parent

    // CHILD processes all begin here:
    if (pid == 0) {
        // initialise semaphores:
        sem_init(&threadEmpty, 0, iBufferSize);
        sem_init(&threadFull, 0, 0);
        sem_init(&threadMutex, 0, 1);

        // a legend to use for the thread buffer:
        struct threadBufLegend *legend = malloc(sizeof(struct threadBufLegend));
        // create bounded buffer of char[2048] for threads to fill:
        legend->threadBuffer = malloc(sizeof(char *) * iBufferSize);
        for (int i = 0; i < iBufferSize; i++) {
            legend->threadBuffer[i] = malloc(sizeof(char) * MAXOUTSIZE);
        }
        legend->iBufferSize = iBufferSize;
        legend->iThIn = 0; // next free position
        legend->iThOut = 0;    // first full position

        // extract path and key w/o modifying orig vars:
        char counterCopy[MAXDIRPATH + 1 + MAXKEYWORD];
        strcpy(counterCopy, newRequest->request);
        char counterPath[MAXDIRPATH];
        strcpy(counterPath, strtok(counterCopy, " "));
        char counterKeyword[MAXKEYWORD];
        strcpy(counterKeyword, strtok(NULL, " "));

        int iFileCount = 0;

        DIR *dirp;
        struct dirent *entry;

        dirp = opendir(counterPath);
        if (dirp == NULL) {
            printf("Something's gone awry!\n");
        }
        // crawl files to count them
        while ((entry = readdir(dirp)) != 0) {
            //  skip over current/hidden . and root .. directories
            if (strcmp(entry->d_name, ".") == 0
                || strcmp(entry->d_name, "..") == 0
                || strncmp(entry->d_name, ".", 1) == 0) {
                continue;
            }

            //	note unique path of current file
            char filePath[256];
            strcpy(filePath, counterPath);
            strcat(filePath, "/");
            strcat(filePath, entry->d_name);

            //	fill stat variable with analysis of current entry
            struct stat status;
            stat(filePath, &status);

            //	if current entry is a file increment fileCount
            if (S_ISREG(status.st_mode)) {
                iFileCount++;
            }
        }
        closedir(dirp);

        // now that we know how many threads to create:
        pthread_t worker[iFileCount];  // n worker threads
        pthread_t printer;  // + one printer thread
        pthread_attr_t attr; // set of attributes for the threads

        // get the default attributes
        pthread_attr_init(&attr);   // default state is PTHREAD_CREATE_JOINABLE

        // crawl again, this time to create threads:
        dirp = opendir(counterPath);
        if (dirp == NULL) {
            printf("Something's gone awry!\n");
        }

        int iWorkerCount = iFileCount;

        while ((entry = readdir(dirp)) != 0) {
            //  skip over current ., hidden and root .. directories
            if (strcmp(entry->d_name, ".") == 0
                || strcmp(entry->d_name, "..") == 0
                || strncmp(entry->d_name, ".", 1) == 0) {
                continue;
            }

            //	note unique path of current file
            char filePath[256];
            strcpy(filePath, counterPath);
            strcat(filePath, "/");
            strcat(filePath, entry->d_name);

            //	fill stat variable with analysis of current entry
            struct stat status;
            stat(filePath, &status);

            //	increment fileCount if current entry is a target folder
            if (S_ISREG(status.st_mode)) {
                struct Search *newSearch = malloc(sizeof(struct Search));
                strcpy(newSearch->dirpath, strtok(newRequest->request, " "));
                strcpy(newSearch->keyword, counterKeyword);
                strcpy(newSearch->filename, entry->d_name);
                newSearch->lineNo = 0;
                newSearch->legend = legend;
                pthread_create(&worker[--iWorkerCount], &attr, &workerFunction, (void *)newSearch);
            }
        }   // end readdir while
        closedir(dirp);

        pthread_create(&printer, &attr, &printerFunction, (void *)legend);

        // now wait for the threads to exit
        for (int i = 0; i < iFileCount; i++) {
            pthread_join(worker[i], NULL);  // join worker threads
        }

        // to indicate end of searching, insert dummy item for printer:
        sem_wait(&threadEmpty);
        sem_wait(&threadMutex);
        strcpy(legend->threadBuffer[legend->iThIn], "EndofSearch:-1:EndofSearch");
        legend->iThIn = (legend->iThIn + 1) % legend->iBufferSize;
        sem_post(&threadMutex);
        sem_post(&threadFull);

        pthread_join(printer, NULL);

        //frees
        free(newRequest);
        newRequest = NULL;
        if (sem_close(csEmpty) == -1) {
            printf("There was an error closing \"csEmpty\".\n");
        }
        if (sem_close(csFull) == -1) {
            printf("There was an error closing \"csFull\".\n");
        }
        if (sem_close(csMutex) == -1) {
            printf("There was an error closing \"csMutex\".\n");
        }
        sem_destroy(csEmpty);
        sem_destroy(csFull);
        sem_destroy(csMutex);
        for (int i = 0; i < iBufferSize; i++) {
            free(legend->threadBuffer[i]);
            legend->threadBuffer[i] = NULL;
        }
        free(legend->threadBuffer);
        legend->threadBuffer = NULL;
        free(legend);
        legend = NULL;

    } else { // PARENT continues here:

        // wait for all child processes to exit:
        int iWaitStatus = 0;
        for (int i = 0; i < iReqCount;i++) {
            wait(&iWaitStatus);
        }

        // free new request
        free(newRequest);
        newRequest = NULL;

        // unlink semaphores
        if (sem_unlink(SEM_CSEMPTY) == -1) {
            printf("There was an error unlinking \"csEmpty\".\n");
        }
        if (sem_unlink(SEM_CSFULL) == -1) {
            printf("There was an error unlinking \"csFull\".\n");
        }
        if (sem_unlink(SEM_CSMUTEX) == -1) {
            printf("There was an error unlinking \"csMutex\".\n");
        }

        // remove the shared memory segment
        if (shm_unlink(name) == -1) {
            printf("Error removing %s\n", name);
            exit(-1);
        }
    } // end PARENT

    //  stop Valgrind's "FILE DESCRIPTORS open at exit" error:
    fclose(stdin);
    fclose(stdout);
    fclose(stderr);

    //  obtain user confirmation before exiting
    getchar();
    return 0;

}

//------------------------------------------------------------------------------
//  Function Definitions
//------------------------------------------------------------------------------

void *workerFunction(void *newSearch) {
    struct Search *search = (struct Search *)newSearch;
    char filePath[256];
    strcpy(filePath, search->dirpath);
    strcat(filePath, "/");
    strcat(filePath, search->filename);

    //  open search file in question
    FILE *file;
    file = fopen(filePath, "r");
    if (file == NULL) {
        printf("Sorry, but there seems to be no such file at %s.\n", filePath);
        return NULL;
    }

    //  search line by line for first instance of keyword.  strcmp?
    // then ++ line;
    char lineFromFile[MAXLINESIZE];
    char wordFromLine[MAXKEYWORD];
    char delimiters[] = ".,; \t\r\n\v\f";
    char *temp;
    char *tokTemp;

    while (fgets(lineFromFile, MAXLINESIZE, file) != 0) {
        if (strcmp(lineFromFile, "\n\0") == 0) { break; }
        strtok_r(lineFromFile, "\n", &temp);   // remove newline character
        strcpy(search->fileLine, lineFromFile); // save to search
        search->lineNo++;   // increment line number
        // peel first word from line:
        strcpy(wordFromLine, strtok_r(lineFromFile, delimiters, &temp));

        while (1) {  // wordFromLine lineFromFile != NULL?
            //if match found
            if (strcmp(search->keyword, wordFromLine) == 0) {
                snprintf(search->findText, MAXOUTSIZE, "%s:%d:%s", search->filename, search->lineNo, search->fileLine);

                //(safely) insert search->fileLine into the thread-common buffer
                sem_wait(&threadEmpty);
                sem_wait(&threadMutex);

                // critical section
                strcpy(search->legend->threadBuffer[search->legend->iThIn], search->findText);
                search->legend->iThIn = (search->legend->iThIn + 1) % search->legend->iBufferSize;

                sem_post(&threadMutex);
                sem_post(&threadFull);

                // remainder section
                break;

            } else {
                tokTemp = strtok_r(NULL, delimiters, &temp);
                if (tokTemp == NULL) {
                    break;
                }
                strcpy(wordFromLine, tokTemp);
                continue;
            }
        }   // end wordbywordWhile
    }   // end linebylineWhile

    fclose(file);
    free(newSearch);
    newSearch = NULL;
    return NULL;
}   // end workers


void *printerFunction(void *bufLegend) {
    struct threadBufLegend *legend = (struct threadBufLegend *)bufLegend;
    char printLine[MAXOUTSIZE];
    char copyLine[MAXOUTSIZE];
    char *temp;
    int lineNo = 0;

    //creates and initialises lock
    struct flock fl = { F_WRLCK, SEEK_SET, 0, 0, 0 };
    fl.l_pid = getpid();
    int fileDescriptor;

    while (1) {
        //access buffer
        sem_wait(&threadFull);
        sem_wait(&threadMutex);

        // critical section: remove item from threadBuffer
        strcpy(printLine, legend->threadBuffer[legend->iThOut]);
        //increment bufferout
        legend->iThOut = (legend->iThOut + 1) % legend->iBufferSize;

        //close buffer
        sem_post(&threadMutex);
        sem_post(&threadEmpty);

        //check for negative lineNo
        strcpy(copyLine, printLine);
        strtok_r(copyLine, ":", &temp);
        lineNo = atoi(strtok_r(NULL, ":", &temp));
        if (lineNo < 0) {
            break;
        }

        //access file; open output file
        if ((fileDescriptor = open("./output.txt", O_CREAT | O_APPEND | O_RDWR, 0666)) == -1) {
            perror("open");
            exit(-1);
        }

        //attempt to get hold of lock
        fl.l_type = F_WRLCK;
        if (fcntl(fileDescriptor, F_SETLKW, &fl) == -1) {
            perror("fcntl");
            exit(-1);
        }

        //critical section: print string to file
        dprintf(fileDescriptor, "%s\n", printLine);

        //unlock and leave
        fl.l_type = F_UNLCK;
        if (fcntl(fileDescriptor, F_SETLKW, &fl) == -1) {
            perror("fcntl");
            exit(-1);
        }

        // remainder section
        //close file
        close(fileDescriptor);
    }   // end while(1)

    return NULL;

}   //end printer
