#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <time.h>
#include "natserver.h"

#define SOURCEPORT 9001
#define MAX_PAIRS 2048 //Support up to 2048 pairs

#define TX_RETRY_US 100000
#define MAX_RETRIES 5

typedef struct {
    client_t client_A;
    client_t client_B;
    int got_ack_A;
    int got_ack_B;
    uint64_t last_tx_A;
    uint64_t last_tx_B;
    int n_retry_A;
    int n_retry_B;
}pair_t;

/* Get current wall-clock time and return it in microseconds since the Unix
 * epoch.
 *
 * CLOCK_REALTIME should be used to get the system's best guess at real time.
 * CLOCK_MONOTONIC should be used when jumps in time would cause trouble.
 */
uint64_t clock_gettime_us(clockid_t clock_id)
{
#if (! defined __MACH__ || ! defined __APPLE__)
    struct timespec ts;
    clock_gettime(clock_id, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (ts.tv_nsec + 500) / 1000;
#else
    static struct mach_timebase_info tb_info = { 0, 0 };
    uint64_t now_us = -1;
    if (tb_info.denom == 0)
        mach_timebase_info(&tb_info);
    if (clock_id == CLOCK_MONOTONIC)
    {
        now_us = (mach_absolute_time() * tb_info.numer) / (1000 * tb_info.denom);
    }
    else if (clock_id == CLOCK_REALTIME)
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        now_us = (tv.tv_sec * 1000000ULL) + tv.tv_usec;
    }
    return now_us;
#endif
}

int open_port(int *sock_fd, int port) {
    struct sockaddr_in myaddr;      /* our address */
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 50000; //50ms

    /* create a UDP socket */
    if ((*sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        printf("udp: can't create socket\n");
        return -1;
    }

#if 0
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "en0");
    if (setsockopt(sock_fd, SOL_SOCKET, SO_BINDTODEVICE, (void *)&ifr, sizeof(ifr)) < 0) {
        printf("Unable to bind to interface\n");
        return 0;
    }
#endif

    /* Set a timeout on the recv */
    setsockopt(*sock_fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

    /* Bind the socket to any IP. */
    memset((char *)&myaddr, 0, sizeof(myaddr));
    myaddr.sin_family = AF_INET;
    myaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    myaddr.sin_port = htons(port);

    if (bind(*sock_fd, (struct sockaddr *)&myaddr, sizeof(myaddr)) < 0)
    {
        printf("udp: bind: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

/* Waits for connections from two clients, A and B.  A and B will send
   the server their private IP tuple (IP:port), and this routine will
   record their public IP tuple.  The server "acks" each client with
   their public IP tuple data.

   When both clients have sent data to the server, the server sends their
   peer's data (public/private IP tuples) back.  Both clients "ack" the
   server.

   Client A will then begin sending data to B and vice-versa.  The server's
   job is done.
*/
int main(int argc, char *argv[]) {
    struct sockaddr_in srcAddr;
    pair_t *pairs[MAX_PAIRS];
    socklen_t slen;
    int sock_fd;
    int recvlen;
    char buf[1500];
    int bytesSent;
    int i;
    client_t *new_client = NULL;
    int send_client_pair_idx = -1;
    uint64_t now;
    int server_port = SOURCEPORT;

    memset(buf,0,sizeof(buf));

    if(argc > 1){
        server_port = atoi(argv[1]);
    }
    printf("Using port %i\n", server_port);

    for(i=0; i<MAX_PAIRS; ++i)
        pairs[i] = NULL;

    /* Open the port */
    if(open_port(&sock_fd, server_port) != 0)
        return -1;
   
    /* Start listening for clients.  Once we get two, send their data */ 
    slen = sizeof(srcAddr);
    while(1) {
        recvlen = recvfrom(sock_fd, buf, sizeof(buf), 0,
                           (struct sockaddr*)&srcAddr, &slen);

        if(recvlen < 0) {
            continue;
        }

        printf("Received %i bytes from %s:%i\n", recvlen, 
               inet_ntoa(srcAddr.sin_addr), ntohs(srcAddr.sin_port));
                
        /* Sanity check */
        if(recvlen != sizeof(packet_t)) {
            printf("Received data is the wrong size\n");
            continue;
        }

        send_client_pair_idx = -1;

        /* Determine the packet type */
        switch(((packet_t*)buf)->pkt_type) {
            case REQUEST:
                /* Is this a new pair id? */
                for(i=0; i<MAX_PAIRS; ++i) {
                    if(pairs[i] == NULL) {
                        /* This is a new pair ID, create a new pair */
                        printf("New pair id: %u\n", (unsigned)((packet_t*)buf)->client_data.pair_id);

                        /* Make sure we don't have too many already */
                        if(i == MAX_PAIRS-1) {
                            printf("Error: too many pairs\n");
                            new_client = NULL;
                            break;
                        }

                        pairs[i] = (pair_t*)malloc(sizeof(pair_t));
                        if(pairs[i] == NULL) {
                            printf("Error: could not create new pair\n");
                            new_client = NULL;
                        }
                        else {
                            memset(pairs[i], 0, sizeof(pair_t));
                            printf("First client %u for pair %u\n", 
                                    (unsigned)((packet_t*)buf)->client_data.uid,
                                    (unsigned)((packet_t*)buf)->client_data.pair_id);

                            new_client = &pairs[i]->client_A;
                        }
                        break;
                    }
                    else if(pairs[i]->client_A.pair_id == ((packet_t*)buf)->client_data.pair_id) {
                        /* This is an existing pair ID */
                        /* Is this a client we already have? */
                        if(pairs[i]->client_A.uid == ((packet_t*)buf)->client_data.uid) {
                            printf("Received duplicate request from %u\n", 
                                    (unsigned)((packet_t*)buf)->client_data.uid);
                            /* Regardless, we ack its info */
                        }
                        else {
                            printf("Second client %u for pair %u\n", 
                                    (unsigned)((packet_t*)buf)->client_data.uid, 
                                    (unsigned)((packet_t*)buf)->client_data.pair_id);
                            
                            new_client = &pairs[i]->client_B;

                            /* Indicate that we should send the client info to both clients */
                            send_client_pair_idx = i;
                        }
                        break;
                    }
                }

                if(new_client != NULL) {
                    memcpy(new_client, 
                           &((packet_t*)buf)->client_data, sizeof(client_t));

                    /* Populate the public ip tuple for this client */
                    memcpy(&new_client->ip_data.public_ip,
                            &srcAddr, sizeof(struct sockaddr_in));

                    /* Send the client_t back to the client as an ack */
                    memset(buf, 0, sizeof(buf));
                    ((packet_t*)buf)->pkt_type = SERVER_ACK;
                    memcpy(&((packet_t*)buf)->client_data, 
                            new_client, sizeof(client_t));

                    bytesSent = sendto(sock_fd, buf, sizeof(packet_t), 0, 
                            (struct sockaddr *)&srcAddr, sizeof(struct sockaddr_in));

                    printf("Sent a %i byte SERVER_ACK to new client\n", bytesSent);
                    new_client = NULL;
                }

                break;

            case CLIENT_ACK:
                /* Mark the client as ack'd */
                for(i=0; i<MAX_PAIRS; ++i) {
                    if(pairs[i] == NULL) {
                        printf("Error: got CLIENT_ACK from unrecognized pair\n");
                        break;
                    }

                    if(((packet_t*)buf)->client_data.pair_id == pairs[i]->client_A.pair_id) {
                        if(((packet_t*)buf)->client_data.uid == pairs[i]->client_A.uid) {
                            printf("Got ACK from client A\n");
                            pairs[i]->got_ack_A = 1;
                        }
                        else if(((packet_t*)buf)->client_data.uid == pairs[i]->client_B.uid) {
                            printf("Got ACK from client B\n");
                            pairs[i]->got_ack_B = 1;
                        }
                        else
                            printf("Got ACK from unrecognized uid\n");
                
                        /* If both are acked, we're done, remove the pair */
                        if(pairs[i]->got_ack_B == 1 && pairs[i]->got_ack_A == 1) {
                            printf("Transaction for pair %u complete\n", 
                                    (unsigned)pairs[i]->client_A.pair_id);
                            free(pairs[i]);
                            pairs[i] = NULL;
                        }
                        break;
                    }
                }
                break;

            /* We should't get these */
            case CLIENT_INFO:
            case SERVER_ACK:
            default:
                printf("Error, got incorrect or unrecognized packet type: %i\n", 
                        ((packet_t*)buf)->pkt_type);
                break;
        }
        
        if(send_client_pair_idx >= 0) {        
            /* If we have both clients, send them each their partner's info */
            memset(buf, 0, sizeof(buf));
            ((packet_t*)buf)->pkt_type = CLIENT_INFO;
            memcpy(&((packet_t*)buf)->client_data, 
                   &pairs[send_client_pair_idx]->client_B, sizeof(client_t));

            memcpy(&srcAddr,
                   &pairs[send_client_pair_idx]->client_A.ip_data.public_ip,
                   sizeof(struct sockaddr_in));

            bytesSent = sendto(sock_fd, buf, sizeof(packet_t), 0, 
                    (struct sockaddr *)&srcAddr, sizeof(struct sockaddr_in));

            pairs[i]->last_tx_A = clock_gettime_us(CLOCK_MONOTONIC);

            printf("Sent a %i byte CLIENT_INFO to client A\n", bytesSent);
            
            memset(buf, 0, sizeof(buf));
            ((packet_t*)buf)->pkt_type = CLIENT_INFO;
            memcpy(&((packet_t*)buf)->client_data, 
                   &pairs[send_client_pair_idx]->client_A, sizeof(client_t));

            memcpy(&srcAddr,
                   &pairs[send_client_pair_idx]->client_B.ip_data.public_ip,
                   sizeof(struct sockaddr_in));

            bytesSent = sendto(sock_fd, buf, sizeof(packet_t), 0, 
                    (struct sockaddr *)&srcAddr, sizeof(struct sockaddr_in));
            
            pairs[i]->last_tx_B = clock_gettime_us(CLOCK_MONOTONIC);

            printf("Sent a %i byte CLIENT_INFO to client B\n", bytesSent);
        }
    }

    /* See if its been too long since we sent data to a client and have not received
     * an ack */
    for(i=0; i<MAX_PAIRS; ++i) {
        if(pairs[i] == NULL)
            break;

        now = clock_gettime_us(CLOCK_MONOTONIC);
        if(!pairs[i]->got_ack_A && 
           ((now-pairs[i]->last_tx_A) > TX_RETRY_US)) {
            printf("Retrying transmission to client A\n");
            ++pairs[i]->n_retry_A;

            memset(buf, 0, sizeof(buf));
            ((packet_t*)buf)->pkt_type = CLIENT_INFO;
            memcpy(&((packet_t*)buf)->client_data, 
                    &pairs[send_client_pair_idx]->client_B, sizeof(client_t));

            memcpy(&srcAddr,
                    &pairs[send_client_pair_idx]->client_A.ip_data.public_ip,
                    sizeof(struct sockaddr_in));

            bytesSent = sendto(sock_fd, buf, sizeof(packet_t), 0, 
                    (struct sockaddr *)&srcAddr, sizeof(struct sockaddr_in));

            pairs[i]->last_tx_A = clock_gettime_us(CLOCK_MONOTONIC);

            printf("Sent a %i byte CLIENT_INFO to client A\n", bytesSent);
        }
        
        now = clock_gettime_us(CLOCK_MONOTONIC);
        if(!pairs[i]->got_ack_B && 
           ((now-pairs[i]->last_tx_B) > TX_RETRY_US)) {
            printf("Retrying transmission to client B\n");
            ++pairs[i]->n_retry_B;

            memset(buf, 0, sizeof(buf));
            ((packet_t*)buf)->pkt_type = CLIENT_INFO;
            memcpy(&((packet_t*)buf)->client_data, 
                    &pairs[send_client_pair_idx]->client_A, sizeof(client_t));

            memcpy(&srcAddr,
                    &pairs[send_client_pair_idx]->client_B.ip_data.public_ip,
                    sizeof(struct sockaddr_in));

            bytesSent = sendto(sock_fd, buf, sizeof(packet_t), 0, 
                    (struct sockaddr *)&srcAddr, sizeof(struct sockaddr_in));

            pairs[i]->last_tx_A = clock_gettime_us(CLOCK_MONOTONIC);

            printf("Sent a %i byte CLIENT_INFO to client B\n", bytesSent);
        }

        /* If we've retried too many times on either A or B, get rid of this pair */
        if(pairs[i]->n_retry_A > MAX_RETRIES || pairs[i]->n_retry_B > MAX_RETRIES) {
            printf("Excessive retries for pair %u\n", (unsigned)pairs[i]->client_A.pair_id);
            free(pairs[i]);
            pairs[i] = NULL;
        }
    }

    close(sock_fd);

    return 0;
}
