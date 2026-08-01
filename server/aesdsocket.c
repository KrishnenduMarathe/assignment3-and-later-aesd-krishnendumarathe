#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/socket.h>

#define N_BACKLOG 10

// async safe variable for signals
volatile sig_atomic_t grace_exit = 0;

// pthread parameter and mutex structure
struct thread_struct {
    int index;
    int fd; // data file descriptor
    int cfd; // client socket id
    char *cip; // client ip address
    int status; // thread status. 0 for success
    pthread_mutex_t *mutex; // write mutex
};

// timer paramter structure
struct timer_struct {
    int* fd;
    pthread_mutex_t *mutex;
};

// linked list node
struct node {
    pthread_t thread_id;
    struct thread_struct thread_param;
    STAILQ_ENTRY(node) entries;
};

STAILQ_HEAD(stailhead, node); // init head

// signal handler
void handle_termination(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        grace_exit = 1;
    }
}

// timer callback
void timer_callback(union sigval sv) {
    int ret;
    int timer_locked_mutex = 0;

    struct timer_struct *ts = (struct timer_struct *) sv.sival_ptr;

    // get timestamp
    char timestamp[200] = "timestamp:";
    time_t t = time(NULL);
    struct tm *tmp = localtime(&t);
    ret = strftime(timestamp+10, 200 * sizeof(char), "%a, %d %b %Y %T %z", tmp);
    if (ret < 1) {
        if (!grace_exit) syslog(LOG_ERR, "(timer) Failed to change time format");

        goto timer_cleanup;
    }

    int len = strlen(timestamp);
    if (len + 1 > 200) {
        syslog(LOG_ERR, "(timer) Failed to add new line to timestamp");

        goto timer_cleanup;
    }

    timestamp[len] = '\n';
    timestamp[len+1] = '\0';

    // lock mutex
    ret = pthread_mutex_lock(ts->mutex);
    if (ret != 0) {
        if (!grace_exit) syslog(LOG_ERR, "(timer) Failed to lock the mutex");

        goto timer_cleanup;
    }
    timer_locked_mutex = 1;

    // write timestamp to file
    ret = write(*ts->fd, timestamp, len+1);;
    if (ret < 0) {
        if (!grace_exit) syslog(LOG_ERR, "(timer) Failed to write to /var/tmp/aesdsocketdata with error: %s", strerror(errno));
        
        goto timer_cleanup;
    }

    // unlock mutex
    ret = pthread_mutex_unlock(ts->mutex);
    if (ret != 0) {
        if (!grace_exit) syslog(LOG_ERR, "(timer) Failed to unlock the mutex");

        goto timer_cleanup;
    }
    timer_locked_mutex = 0;

timer_cleanup:
    if (timer_locked_mutex) {
        ret = pthread_mutex_unlock(ts->mutex);
        if (ret != 0) {
            if (!grace_exit) syslog(LOG_ERR, "(timer) cleanup: Failed to unlock the mutex");
        }
    }

    return;
}

void* threaded_function(void *thread_arg) {
    int ret;
    int mutex_locked_by_us = 0;
    struct thread_struct* thread_param = (struct thread_struct*) thread_arg;

    thread_param->status = 0;

    // interact with connection
    int buffsize = 512;
    char buf[buffsize];

    // dyanmic buffering for incoming data
    char* dynbuffer = NULL;
    unsigned int dynbuffersize = 0;

    while (grace_exit == 0) {
        // get data from client
        long int readsize = 0;
        memset(buf, 0, buffsize * sizeof(char));

        readsize = read(thread_param->cfd, buf, buffsize * sizeof(char));
        if (readsize == 0) {
            // client finished sending data
            syslog(LOG_DEBUG, "(thread %d) Closed connection from %s", thread_param->index, thread_param->cip);
            thread_param->status = 1;
            if (thread_param->cip != NULL) free(thread_param->cip);
            if (dynbuffer != NULL) free(dynbuffer);
            close(thread_param->cfd);
            break;
        }
        if (readsize < 0) {
            if (!grace_exit) syslog(LOG_ERR, "(thread %d) Failed to get data from client %s with error: %s", thread_param->index, thread_param->cip, strerror(errno));
            thread_param->status = 1;
            if (thread_param->cip != NULL) free(thread_param->cip);
            if (dynbuffer != NULL) free(dynbuffer);
            close(thread_param->cfd);
            
            goto thread_cleanup;
        }

        // stream data till \n
        char *ptr = (char *) realloc(dynbuffer, dynbuffersize + readsize + 1);
        if (ptr == NULL) {
            if (!grace_exit) syslog(LOG_ERR, "(thread %d) Failed to reallocate dynamic buffer with error: %s", thread_param->index, strerror(errno));
            thread_param->status = 1;
            if (thread_param->cip != NULL) free(thread_param->cip);
            if (dynbuffer != NULL) free(dynbuffer);
            close(thread_param->cfd);

            goto thread_cleanup;
        }
        dynbuffer = ptr;

        // copy over read data at the end
        memcpy(dynbuffer + dynbuffersize, buf, readsize);
        dynbuffersize += readsize;
        dynbuffer[dynbuffersize] = '\0';

        // check for newline character
        char* retptr;
        int newlinefound = 0;

        while (dynbuffer != NULL && (retptr = strchr(dynbuffer, '\n')) != NULL) {
            // new line found
            if (!newlinefound) newlinefound = 1;

            unsigned int length = (retptr - dynbuffer) + 1;

            // lock mutex
            ret = pthread_mutex_lock(thread_param->mutex);
            if (ret != 0) {
                if (!grace_exit) syslog(LOG_ERR, "(thread %d) client %s failed to lock the mutex", thread_param->index, thread_param->cip);
                thread_param->status = 1;
                if (thread_param->cip != NULL) free(thread_param->cip);
                if (dynbuffer != NULL) free(dynbuffer);
                close(thread_param->cfd);

                goto thread_cleanup;
            }
            mutex_locked_by_us = 1;

            // write to file
            ret = write(thread_param->fd, dynbuffer, length * sizeof(char));
            if (ret < 0) {
                if (!grace_exit) syslog(LOG_ERR, "(thread %d) Failed to write to /var/tmp/aesdsocketdata with error: %s", thread_param->index, strerror(errno));
                thread_param->status = 1;
                if (thread_param->cip != NULL) free(thread_param->cip);
                if (dynbuffer != NULL) free(dynbuffer);
                close(thread_param->cfd);
                
                goto thread_cleanup;
            }

            // unlock mutex
            ret = pthread_mutex_unlock(thread_param->mutex);
            if (ret != 0) {
                if (!grace_exit) syslog(LOG_ERR, "(thread %d) client %s failed to unlock the mutex", thread_param->index, thread_param->cip);
                thread_param->status = 1;
                if (thread_param->cip != NULL) free(thread_param->cip);
                if (dynbuffer != NULL) free(dynbuffer);
                close(thread_param->cfd);

                goto thread_cleanup;
            }
            mutex_locked_by_us = 0;

            // move remaining data to front of buffer
            unsigned int rem_length = dynbuffersize - length;

            if (rem_length > 0) {
                // reallocate and move; length already accounts for \0
                memmove(dynbuffer, dynbuffer+length, rem_length);
                dynbuffersize = rem_length;
                dynbuffer[dynbuffersize] = '\0';
            }
            else {
                if (dynbuffer != NULL) free(dynbuffer);
                dynbuffer = NULL;
                dynbuffersize = 0;
            }

        }

        if (newlinefound) {
            newlinefound = 0;

            // lock mutex
            ret = pthread_mutex_lock(thread_param->mutex);
            if (ret != 0) {
                if (!grace_exit) syslog(LOG_ERR, "(thread %d) client %s failed to lock the mutex", thread_param->index, thread_param->cip);
                thread_param->status = 1;
                if (thread_param->cip != NULL) free(thread_param->cip);
                if (dynbuffer != NULL) free(dynbuffer);
                close(thread_param->cfd);

                goto thread_cleanup;
            }
            mutex_locked_by_us = 1;

            // seek back on the file to the beginning
            lseek(thread_param->fd, 0, SEEK_SET);

            unsigned int fsize = lseek(thread_param->fd, 0, SEEK_END);

            char* data = (char*) malloc((fsize+1) * sizeof(char));
            if (data == NULL) {
                if (!grace_exit) syslog(LOG_ERR, "(thread %d) Failed to allocate for data with error: %s", thread_param->index, strerror(errno));
                thread_param->status = 1;
                if (thread_param->cip != NULL) free(thread_param->cip);
                if (dynbuffer != NULL) free(dynbuffer);
                close(thread_param->cfd);
                
                goto thread_cleanup;
            }

            // send data to client
            lseek(thread_param->fd, 0, SEEK_SET);

            ret = read(thread_param->fd, data, fsize * sizeof(char));
            if (ret < 0) {
                if (!grace_exit) syslog(LOG_ERR, "(thread %d) Failed to read from /var/tmp/aesdsocketdata with error: %s", thread_param->index, strerror(errno));
                thread_param->status = 1;
                if (thread_param->cip != NULL) free(thread_param->cip);
                if (dynbuffer != NULL) free(dynbuffer);
                if (data != NULL) free(data);
                close(thread_param->cfd);
                
                goto thread_cleanup;
            }
            data[fsize] = '\0';

            // restore file seek
            lseek(thread_param->fd, 0, SEEK_END);

            // unlock mutex
            ret = pthread_mutex_unlock(thread_param->mutex);
            if (ret != 0) {
                if (!grace_exit) syslog(LOG_ERR, "(thread %d) client %s failed to unlock the mutex", thread_param->index, thread_param->cip);
                thread_param->status = 1;
                if (thread_param->cip != NULL) free(thread_param->cip);
                if (dynbuffer != NULL) free(dynbuffer);
                if (data != NULL) free(data);
                close(thread_param->cfd);

                goto thread_cleanup;
            }
            mutex_locked_by_us = 0;

            ret = write(thread_param->cfd, data, fsize * sizeof(char));
            if (ret < 0) {
                if (!grace_exit) syslog(LOG_ERR, "(thread %d) Failed to send data to client %s with error: %s", thread_param->index, thread_param->cip, strerror(errno));
                thread_param->status = 1;
                if (thread_param->cip != NULL) free(thread_param->cip);
                if (dynbuffer != NULL) free(dynbuffer);
                if (data != NULL) free(data);
                close(thread_param->cfd);
                
                goto thread_cleanup;
            }

            if (data != NULL) free(data);
        }
    }

    if (dynbuffer != NULL) {
        free(dynbuffer);
        dynbuffer = NULL;
        dynbuffersize = 0;
    }
    close(thread_param->cfd);

thread_cleanup:
    if (mutex_locked_by_us) {
        ret = pthread_mutex_unlock(thread_param->mutex);
        if (ret != 0) {
            thread_param->status = 1;
            if (!grace_exit) syslog(LOG_ERR, "(thread %d) cleanup: client failed to unlock the mutex", thread_param->index);
        }
    }
    
    // exit thread
    pthread_exit(NULL);
}

int main(int argc, char **argv) {
    // initiate logging
    openlog(NULL, 0, LOG_USER);

    // setup signal handler
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = handle_termination;
    act.sa_flags = SA_NODEFER;

    //  register signals
    if (sigaction(SIGINT, &act, NULL) < 0) {
        syslog(LOG_ERR, "Failed to register SIGINT for handler with error: %s", strerror(errno));
        closelog();
        return -1;
    }
    if (sigaction(SIGTERM, &act, NULL) < 0) {
        syslog(LOG_ERR, "Failed to register SIGTERM for handler with error: %s", strerror(errno));
        closelog();
        return -1;
    }

    // define socket hints for localhost port 9000
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_protocol = 0;

    int ret = getaddrinfo(NULL, "9000", &hints, &result);
    if (ret != 0) {
        if (grace_exit) {
            closelog();
            return 0;
        }
        else {
            syslog(LOG_ERR, "getaddrinfo failed with error: %s", gai_strerror(ret));
            closelog();
            return -1;
        }
    }

    int fd = -1; 
    int sfd = -1;
    struct addrinfo* itr;
    for (itr = result; itr != NULL; itr = itr->ai_next) {
        sfd = socket(itr->ai_family, itr->ai_socktype, itr->ai_protocol);
        if (sfd < 0) continue;

        // successful socket initiation, bind
        int status = bind(sfd, itr->ai_addr, itr->ai_addrlen);
        if (status == 0) break;

        // bind failed, close socket
        close(sfd);
        sfd = -1;
    }

    // free allocated addrinfo result
    freeaddrinfo(result);

    // no address found
    if (itr == NULL) {
        if (!grace_exit) syslog(LOG_ERR, "No address found to bind socker to. Last error: %s", strerror(errno));

        goto cleanup;
    }

    // run in daemon mode
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {

        pid_t pid = fork();

        if (pid < 0) {
            // error creating fork
            syslog(LOG_ERR, "Failed to fork a child process with error: %s", strerror(errno));
            goto cleanup;
        }
        else if (pid == 0) {
            // child process

            // new session and process group
            if (setsid() < 0) {
                syslog(LOG_ERR, "Failed to set new session for the forked child process with error: %s", strerror(errno));
                goto cleanup;
            }

            // change dir
            if (chdir("/tmp/") < 0) {
                syslog(LOG_ERR, "Failed to changed directory of the forked child process with error: %s", strerror(errno));
                goto cleanup;
            }

            // redirect standard input/output
            int nfd = open("/dev/null", O_WRONLY);
            if (nfd < 0) {
                syslog(LOG_ERR, "Failed to set open /dev/null for the forked child process with error: %s", strerror(errno));
                goto cleanup;
            }

            // replace fds of stdout with null
            if (dup2(nfd, STDOUT_FILENO) < 0) {
                syslog(LOG_ERR, "Failed to set open /dev/null for the forked child process with error: %s", strerror(errno));
                close(nfd);
                goto cleanup;
            }
            // replace fds of stdin with null
            if (dup2(nfd, STDIN_FILENO) < 0) {
                syslog(LOG_ERR, "Failed to set open /dev/null for the forked child process with error: %s", strerror(errno));
                close(nfd);
                goto cleanup;
            }
            // replace fds of stderr with null
            if (dup2(nfd, STDERR_FILENO) < 0) {
                syslog(LOG_ERR, "Failed to set open /dev/null for the forked child process with error: %s", strerror(errno));
                close(nfd);
                goto cleanup;
            }
            close(nfd);
        }
        else {
            // parent process
            exit(EXIT_SUCCESS);
        }
    }

    // continue as child
    // on successful bind, start listening
    ret = listen(sfd, N_BACKLOG);
    if (ret < 0) {
        if (!grace_exit) syslog(LOG_ERR, "Failed to listen on the socket with error: %s", strerror(errno));

        goto cleanup;
    }

    // initiate data file
    fd = open("/var/tmp/aesdsocketdata", O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        if (!grace_exit) syslog(LOG_ERR, "Failed to create/open /var/tmp/aesdsocketdata with error: %s", strerror(errno));

        goto cleanup;
    }

    //  start queue
    unsigned int queue = 0;
    struct stailhead head;
    STAILQ_INIT(&head);

    // initialize mutex
    pthread_mutex_t mutex;
    pthread_mutex_init(&mutex, NULL);

    // initiate timer
    timer_t timerid;
    struct sigevent sev;
    struct timer_struct ts;

    memset(&sev, 0, sizeof(struct sigevent));
    memset(&ts, 0, sizeof(struct timer_struct));

    ts.fd = &fd;
    ts.mutex = &mutex;

    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_value.sival_ptr = &ts;
    sev.sigev_notify_function = timer_callback;

    ret = timer_create(CLOCK_REALTIME, &sev, &timerid);
    if (ret != 0) {
        if (!grace_exit) syslog(LOG_ERR, "Failed to create timer with error %s", strerror(errno));

        goto queue_cleanup;
    }

    // setup timer frequency
    struct itimerspec spec;
    spec.it_interval.tv_nsec = 0;
    spec.it_interval.tv_sec = 10;
    spec.it_value.tv_nsec = 0;
    spec.it_value.tv_sec = 10;

    // set alarm
    int alarm_set = 0;
    ret = timer_settime(timerid, 0, &spec, NULL);
    if (ret < 0) {
        if (!grace_exit) syslog(LOG_ERR, "Failed to set alarm with error %s", strerror(errno));

        goto queue_cleanup;
    }
    alarm_set = 1;

    while (grace_exit == 0) {
        // accept connection
        struct sockaddr_in6 caddr;
        unsigned int caddr_len = sizeof(caddr);
        int cfd = accept(sfd, (struct sockaddr *) &caddr, (socklen_t *) &caddr_len);
        if (cfd < 0) {
            if (!grace_exit) syslog(LOG_ERR, "Failed to accept connection with error: %s", strerror(errno));

            goto queue_cleanup;
        }

        // init queue element
        struct node* elm = (struct node*) malloc(sizeof(struct node));
        if (elm == NULL) {
            if (!grace_exit) syslog(LOG_ERR, "Failed to allocate element linked list");
            close(cfd);
            
            goto queue_cleanup;
        }

        // setup node and start thread
        int cip_len = 0;
        elm->thread_param.index = queue;
        elm->thread_param.fd = fd;
        elm->thread_param.cfd = cfd;
        elm->thread_param.status = -1;
        elm->thread_param.mutex = &mutex;

        // check if ipv4
        if (IN6_IS_ADDR_V4MAPPED(&caddr.sin6_addr)) {
            cip_len = INET_ADDRSTRLEN;
            elm->thread_param.cip = (char *) malloc(cip_len * sizeof(char));
            if (elm->thread_param.cip == NULL) {
                close(cfd);

                if (!grace_exit) syslog(LOG_ERR, "Failed to allocate IPv4 address string in memory with error: %s", strerror(errno));

                goto queue_cleanup;
            }

            // get ipv4 client address
            struct in_addr caddrv4;

            // last 4 bytes at 12 byte offset
            memcpy(&caddrv4.s_addr, &caddr.sin6_addr.s6_addr[12], 4);

            // get ipv4 client address
            if (inet_ntop(AF_INET, &caddrv4, elm->thread_param.cip, cip_len * sizeof(char)) == NULL) {
                if (elm->thread_param.cip != NULL) free(elm->thread_param.cip);
                close(cfd);

                if (!grace_exit) syslog(LOG_ERR, "Failed to get client IPv4 address with error: %s", strerror(errno));

                goto queue_cleanup;
            }
        }
        else {
            cip_len = INET6_ADDRSTRLEN;
            elm->thread_param.cip = (char *) malloc(cip_len * sizeof(char));
            if (elm->thread_param.cip == NULL) {
                close(cfd);

                if (!grace_exit) syslog(LOG_ERR, "Failed to allocate IPv6 address string in memory with error: %s", strerror(errno));

                goto queue_cleanup;
            }

            // get ipv6 client address
            if (inet_ntop(AF_INET6, &caddr.sin6_addr, elm->thread_param.cip, cip_len * sizeof(char)) == NULL) {
                if (elm->thread_param.cip != NULL) free(elm->thread_param.cip);
                close(cfd);

                if (!grace_exit) syslog(LOG_ERR, "Failed to get client address IPv6 with error: %s", strerror(errno));

                goto queue_cleanup;
            }
        }

        syslog(LOG_DEBUG, "Accepted connection from %s", elm->thread_param.cip);

        // start thread
        ret = pthread_create(&elm->thread_id, NULL, threaded_function, &elm->thread_param);
        if (ret != 0) {            
            close(cfd);

            if (!grace_exit) syslog(LOG_ERR, "Failed to create thread for client %s", elm->thread_param.cip);
            if (elm->thread_param.cip != NULL) free(elm->thread_param.cip);

            goto queue_cleanup;
        }

        // init element and insert in the queue
        STAILQ_INSERT_TAIL(&head, elm, entries);
        queue++;
    }

queue_cleanup:
    // disarm timer
    if (alarm_set) {
        ret = timer_delete(timerid);
        if (ret != 0) {
            syslog(LOG_ERR, "Failed to disarm Timer with error %s. Continue cleaning up...", strerror(errno));
        }
    }

    struct node *curr, *next;
    
    // stop all threads
    curr = STAILQ_FIRST(&head);
    STAILQ_FOREACH(next, &head, entries) {
        // traverse to kill threads and cleanup resources
        if (next->thread_param.status != -1) {
            ret = pthread_join(next->thread_id, NULL);
            if (ret != 0) {
                syslog(LOG_ERR, "(thread %d) Error joining thread with error %s. Skipping...", next->thread_param.index, strerror(errno));
            }
        }
    }

    // cleanup queue data
    pthread_mutex_destroy(&mutex);

    // queue deletion
    curr = STAILQ_FIRST(&head);
    if (curr != NULL) {
        next = STAILQ_NEXT(curr, entries);
        free(curr);

        curr = next;
    }

    STAILQ_INIT(&head);

cleanup:

    // gracefully exit
    if (sfd != -1) close(sfd);
    if (fd != -1) close(fd);

    // delete data file
    ret = unlink("/var/tmp/aesdsocketdata");
    if (ret < 0) {
        syslog(LOG_ERR, "Failed to remove /var/tmp/aesdsocketdata with error: %s", strerror(errno));
        closelog();
        return -1;
    }

    if (grace_exit) {
        syslog(LOG_DEBUG, "Caught signal, exiting");
        closelog();
        return 0;
    }
    else {
        closelog();
        return -1;
    }

    return 0;
}