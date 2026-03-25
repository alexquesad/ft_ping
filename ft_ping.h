#ifndef FT_PING_H
# define FT_PING_H

// Standard includes
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <math.h>
#include <getopt.h>

// Network includes
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <netdb.h>

// Constants
#define PING_DATA_SIZE    56        // Standard ping data size
#define PING_INTERVAL     1         // Seconds between pings
#define RECV_TIMEOUT_MS   1000      // Receive timeout in milliseconds
#define MAX_PACKET_SIZE   1024      // Maximum packet size

// Structures
typedef struct s_ping_opts
{
    int     verbose;                // -v flag
    int     quiet;                  // -q flag : n'affiche que les stats finales
    int     count;                  // -c flag : arrêter après N paquets (0 = infini)
    int     ttl;                    // --ttl flag : TTL des paquets envoyés (0 = défaut)
    double  interval;               // -i flag : intervalle entre paquets en secondes
    int     deadline;               // -w flag : -w N secondes (0 = pas de deadline)
} t_ping_opts;

typedef struct s_ping_stats
{
    int     packets_sent;           // Total packets sent
    int     packets_received;       // Total packets received
    double  min_time;               // Minimum RTT (-1.0 = not yet set)
    double  max_time;               // Maximum RTT
    double  total_time;             // Sum of all RTTs
    double  sum_sq;                 // Sum of squares for std dev
} t_ping_stats;

// Global state structure
typedef struct s_global_state
{
    volatile sig_atomic_t ping_loop;    // Signal handler control flag
    t_ping_stats          stats;        // Ping statistics
} t_global_state;

// Function prototypes
void            sig_handler(int sig);
unsigned short  calculate_checksum(void *data, int len);
void            create_icmp_packet(struct icmp *icmp_hdr, int sequence);
double          time_diff(struct timeval *start, struct timeval *end);
int             resolve_hostname(const char *hostname, struct sockaddr_in *addr,
                                char *resolved_ip, char *canonical_name);
int             receive_ping(int sockfd, struct sockaddr_in *addr, int sequence,
                            t_ping_opts *opts, struct timeval *deadline);
int             send_ping(int sockfd, struct sockaddr_in *addr, int sequence);
void            print_statistics(const char *hostname);
void            ping_loop(int sockfd, struct sockaddr_in *addr, t_ping_opts *opts);
void            print_usage(const char *prog_name);

// Global variable (extern declaration)
extern t_global_state g_state;

#endif