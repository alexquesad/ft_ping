#include "ft_ping.h"

t_global_state g_state = {1, {0, 0, -1.0, 0.0, 0.0, 0.0}};


//Signal handler pour SIGINT (Ctrl+C)
void    sig_handler(int sig)
{
    (void)sig;
    g_state.ping_loop = 0;
}

/*
** Calcul du checksum Internet (RFC 1071)
** Additionne tous les mots de 16 bits, replie les retenues,
** puis retourne le complément à 1
*/
unsigned short  calculate_checksum(void *data, int len)
{
    unsigned short  *buf = data;
    unsigned int    sum = 0;
    unsigned short  result;

    while (len > 1)
    {
        sum += *buf++;
        len -= 2;
    }
    if (len == 1)
        sum += *(unsigned char *)buf;

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);

    result = ~sum;
    return (result);
}

/*
** Construit un paquet ICMP Echo Request dans le buffer pointé par icmp_hdr.
** Le timestamp gettimeofday() est stocké en début de données pour pouvoir
** calculer le RTT à la réception
*/
void    create_icmp_packet(struct icmp *icmp_hdr, int sequence)
{
    char            *data;
    struct timeval  tv;
    int             i;

    icmp_hdr->icmp_type = ICMP_ECHO;
    icmp_hdr->icmp_code = 0;
    icmp_hdr->icmp_id   = getpid() & 0xFFFF;
    icmp_hdr->icmp_seq  = htons(sequence);

    // Zéro la zone de données avant de la remplir
    data = (char *)icmp_hdr + sizeof(struct icmp);
    memset(data, 0, PING_DATA_SIZE);

    // Timestamp en début de données (memcpy évite les problèmes d'alignement)
    gettimeofday(&tv, NULL);
    memcpy(data, &tv, sizeof(struct timeval));

    // Pattern de remplissage après le timestamp
    i = sizeof(struct timeval);
    while (i < PING_DATA_SIZE)
    {
        data[i] = (0x10 + i) & 0xFF;
        i++;
    }

    // Checksum en dernier, après que tout le reste est en place
    icmp_hdr->icmp_cksum = 0;
    icmp_hdr->icmp_cksum = calculate_checksum(icmp_hdr,
                               sizeof(struct icmp) + PING_DATA_SIZE);
}


//Calcule la différence entre deux timeval en ms
double  time_diff(struct timeval *start, struct timeval *end)
{
    double diff;

    diff  = (end->tv_sec  - start->tv_sec)  * 1000.0;
    diff += (end->tv_usec - start->tv_usec) / 1000.0;
    return (diff);
}

/*
** Résolution DNS IPv4 uniquement (pas besoin de getaddrinfo pour du ping v4).
** Si hostname est déjà une IP (inet_aton réussit), pas besoin de resolution DNS
** canonical_name reçoit le vrai nom de l'hôte (après alias DNS)
*/
int resolve_hostname(const char *hostname, struct sockaddr_in *addr,
                     char *resolved_ip, char *canonical_name)
{
    struct hostent  *host_entry;
    struct in_addr  addr_buf;

    // Cas 1 : c'est déjà une adresse IP littérale
    if (inet_aton(hostname, &addr_buf))
    {
        addr->sin_addr = addr_buf;
        strncpy(resolved_ip, hostname, INET_ADDRSTRLEN - 1);
        resolved_ip[INET_ADDRSTRLEN - 1] = '\0';
        strncpy(canonical_name, hostname, 255);
        canonical_name[255] = '\0';
        return (0);
    }

    // Cas 2 : résolution DNS
    host_entry = gethostbyname(hostname);
    if (host_entry == NULL)
        return (-1);

    addr->sin_addr = *((struct in_addr *)host_entry->h_addr_list[0]);
    strncpy(resolved_ip, inet_ntoa(addr->sin_addr), INET_ADDRSTRLEN - 1);
    resolved_ip[INET_ADDRSTRLEN - 1] = '\0';
    strncpy(canonical_name, host_entry->h_name, 255);
    canonical_name[255] = '\0';
    return (0);
}

/*
** Attend qu'un paquet soit disponible sur le socket avant la deadline.
** Retourne 1 si un paquet est prêt, 0 si timeout, -1 si erreur/signal.
*/
static int  wait_for_packet(int sockfd, struct timeval *deadline)
{
    struct timeval  now;
    struct timeval  timeout;
    fd_set          read_fds;
    int             ret;

    gettimeofday(&now, NULL);
    timeout.tv_sec  = deadline->tv_sec  - now.tv_sec;
    timeout.tv_usec = deadline->tv_usec - now.tv_usec;
    if (timeout.tv_usec < 0)
    {
        timeout.tv_sec--;
        timeout.tv_usec += 1000000;
    }
    if (timeout.tv_sec < 0)
        return (0);
    FD_ZERO(&read_fds);
    FD_SET(sockfd, &read_fds);
    ret = select(sockfd + 1, &read_fds, NULL, NULL, &timeout);
    if (ret < 0 && errno == EINTR)
        return (-1);
    return (ret);
}

/*
** Parse le buffer reçu, vérifie que c'est notre Echo Reply (numero de seq correspondant),
** calcule le RTT et met à jour les statistiques si tous les filtres passent.
** On a 3 filtres:
**   1. Type ICMP + identifiant (getpid) -> élimine les paquets
**      d'autres processus et les erreurs réseau.
**      --> (option --ttl) Cas spécial : ICMP_TIME_EXCEEDED → affiche le message
**      "Time to live exceeded" et retourne 0 (TTL épuisé en route)
**   2. Numéro de séquence -> élimine les réponses tardives
**      d'une itération précédente.
**   3. Sanity check sur le RTT
** Retourne  0 : c'est notre paquet, traité avec succès
**           1 : paquet ignoré — receive_ping reboucle et attend le suivant
**          -1 : RTT invalide — receive_ping sort sans afficher de timeout
*/
static int  parse_packet(char *buf, ssize_t len, struct sockaddr_in *from,
                         int sequence, t_ping_opts *opts,
                         struct timeval *recv_time)
{
    struct iphdr    *ip_hdr;
    struct icmp     *icmp_hdr;
    struct timeval  sent_time;
    double          rtt;
    char            ip_str[INET_ADDRSTRLEN];

    (void)len;
    ip_hdr   = (struct iphdr *)buf;
    icmp_hdr = (struct icmp *)(buf + ip_hdr->ihl * 4);

    if (icmp_hdr->icmp_type != ICMP_ECHOREPLY
        || icmp_hdr->icmp_id != (getpid() & 0xFFFF))
    {   
        //cas specifique option --ttl ttl epuise
        if (icmp_hdr->icmp_type == ICMP_TIME_EXCEEDED)
        {
            struct hostent *he = gethostbyaddr(&from->sin_addr,
                                    sizeof(from->sin_addr), AF_INET);
            strncpy(ip_str, inet_ntoa(from->sin_addr), INET_ADDRSTRLEN - 1);
            ip_str[INET_ADDRSTRLEN - 1] = '\0';
            printf("%d bytes from %s (%s): Time to live exceeded\n",
                (int)(sizeof(struct iphdr) + sizeof(struct icmp)
                    + sizeof(struct iphdr) + 8),
                he ? he->h_name : ip_str,
                ip_str);
            return (0);
        }
        else if (opts->verbose)
        {
            if (icmp_hdr->icmp_type == ICMP_DEST_UNREACH)
                printf("Destination Unreachable from %s\n",
                       inet_ntoa(from->sin_addr));
            else if (icmp_hdr->icmp_type != ICMP_ECHO)
                printf("Received ICMP type %d from %s\n",
                       icmp_hdr->icmp_type, inet_ntoa(from->sin_addr));
        }
        return (1);
    }
    if (ntohs(icmp_hdr->icmp_seq) != sequence)
    {
        if (opts->verbose)
            printf("Received late reply for icmp_seq=%d (expecting %d)\n",
                   ntohs(icmp_hdr->icmp_seq), sequence);
        return (1);
    }
    memcpy(&sent_time, (char *)icmp_hdr + sizeof(struct icmp),
           sizeof(struct timeval));
    rtt = time_diff(&sent_time, recv_time);
    if (rtt < 0.0 || rtt > 10000.0)
        return (-1);
    g_state.stats.packets_received++;
    g_state.stats.total_time += rtt;
    g_state.stats.sum_sq     += rtt * rtt;
    if (g_state.stats.min_time < 0.0 || rtt < g_state.stats.min_time)
        g_state.stats.min_time = rtt;
    if (rtt > g_state.stats.max_time)
        g_state.stats.max_time = rtt;
    if (!opts->quiet)
        printf("64 bytes from %s: icmp_seq=%d ttl=%d time=%.3f ms\n",
               inet_ntoa(from->sin_addr),
               ntohs(icmp_hdr->icmp_seq), ip_hdr->ttl, rtt);
    return (0);
}

/*
** Attend et traite la réponse ICMP correspondant à (sequence).
** Boucle tant qu'on reçoit des paquets qui ne nous appartiennent pas,
** jusqu'à trouver le nôtre ou atteindre la deadline.
*/
int receive_ping(int sockfd, struct sockaddr_in *addr, int sequence,
                 t_ping_opts *opts, struct timeval *deadline)
{
    char            buf[MAX_PACKET_SIZE];
    struct sockaddr_in from;
    socklen_t       fromlen;
    struct timeval  recv_time;
    ssize_t         n;
    int             ret;

    (void)addr;
    while (1)
    {
        ret = wait_for_packet(sockfd, deadline);
        if (ret <= 0)
        {
            if (ret == 0)
                printf("Request timeout for icmp_seq %d\n", sequence);
            return (-1);
        }
        fromlen = sizeof(from);
        n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        gettimeofday(&recv_time, NULL);
        if (n < 0)
            continue;
        ret = parse_packet(buf, n, &from, sequence, opts, &recv_time);
        if (ret == 0)
            return (0);
    }
}

/*
** Envoie un paquet ICMP Echo Request
** Le buffer est alloué sur la stack (pas de malloc)
** packets_sent est incrémenté uniquement si sendto() réussit
*/
int send_ping(int sockfd, struct sockaddr_in *addr, int sequence)
{
    char        send_buffer[sizeof(struct icmp) + PING_DATA_SIZE];
    struct icmp *icmp_hdr;
    ssize_t     bytes_sent;

    memset(send_buffer, 0, sizeof(send_buffer));
    icmp_hdr = (struct icmp *)send_buffer;
    create_icmp_packet(icmp_hdr, sequence);

    bytes_sent = sendto(sockfd, send_buffer, sizeof(send_buffer), 0,
                        (struct sockaddr *)addr, sizeof(*addr));
    if (bytes_sent < 0)
    {
        perror("sendto");
        return (-1);
    }
    g_state.stats.packets_sent++;
    return (0);
}

/*
** Affiche le résumé final des statistiques.
** mdev = sqrt(E[X²] - E[X]²) 
** La variance peut être légèrement négative à cause des erreurs
** d'arrondi flottant on la met a 0 avant le sqrt
*/
void    print_statistics(const char *hostname)
{
    double  avg;
    double  variance;
    double  mdev;

    printf("--- %s ping statistics ---\n", hostname);
    printf("%d packets transmitted, %d packets received, %.0f%% packet loss\n",
           g_state.stats.packets_sent,
           g_state.stats.packets_received,
           g_state.stats.packets_sent > 0
           ? (double)(g_state.stats.packets_sent - g_state.stats.packets_received)
             * 100.0 / g_state.stats.packets_sent
           : 0.0);
    if (g_state.stats.packets_received > 0)
    {
        avg  = g_state.stats.total_time / g_state.stats.packets_received;
        mdev = 0.0;
        if (g_state.stats.packets_received > 1)
        {
            variance = (g_state.stats.sum_sq / g_state.stats.packets_received)
                       - (avg * avg);
            mdev = sqrt(variance < 0.0 ? 0.0 : variance);
        }
        printf("round-trip min/avg/max/stddev = %.3f/%.3f/%.3f/%.3f ms\n",
               g_state.stats.min_time, avg, g_state.stats.max_time, mdev);
    }
}

/*
** Boucle principale : envoie un ping par seconde jusqu'à SIGINT.
**
** La deadline de chaque itération est calculée ici (iter_start + interval)
** et passée à receive_ping(). Ainsi receive + usleep restant = interval exactement,
** que le paquet soit reçu ou perdu.
** L'option -w ajoute une deadline globale : si le temps total écoulé
** depuis le début dépasse N secondes, on sort de la boucle.
*/
void    ping_loop(int sockfd, struct sockaddr_in *addr, t_ping_opts *opts)
{
    int             sequence;
    struct timeval  iter_start;
    struct timeval  deadline;
    struct timeval  now;
    struct timeval  loop_start;
    double          remaining_ms;
    double          elapsed_ms;

    sequence = 0;
    gettimeofday(&loop_start, NULL);
    while (g_state.ping_loop
           && (opts->count == 0 || sequence < opts->count))
    {
        if (opts->deadline > 0)
        {
            gettimeofday(&now, NULL);
            elapsed_ms = time_diff(&loop_start, &now);
            if (elapsed_ms >= opts->deadline * 1000.0)
                break;
        }
        gettimeofday(&iter_start, NULL);
        deadline.tv_sec  = iter_start.tv_sec  + (long)opts->interval;
        deadline.tv_usec = iter_start.tv_usec
                           + (long)((opts->interval - (long)opts->interval)
                           * 1e6);
        if (deadline.tv_usec >= 1000000)
        {
            deadline.tv_sec++;
            deadline.tv_usec -= 1000000;
        }

        if (send_ping(sockfd, addr, sequence) == 0)
            receive_ping(sockfd, addr, sequence, opts, &deadline);
        else if (opts->verbose)
            printf("Failed to send ping for icmp_seq=%d\n", sequence);

        sequence++;

        if (!g_state.ping_loop)
            break;

        gettimeofday(&now, NULL);
        remaining_ms  = (deadline.tv_sec  - now.tv_sec)  * 1000.0;
        remaining_ms += (deadline.tv_usec - now.tv_usec) / 1000.0;
        if (remaining_ms > 0.0)
            usleep((useconds_t)(remaining_ms * 1000.0));
    }
}

void    print_usage(const char *prog_name)
{
    printf("Usage: %s [-v] [-q] [-c count] [--ttl N] [-i interval] [-w deadline] [-?] destination\n",
           prog_name);
    printf("Options:\n");
    printf("  -v           verbose output\n");
    printf("  -q           quiet output (only statistics)\n");
    printf("  -c count     stop after sending count packets\n");
    printf("  --ttl N      set time-to-live to packets we send (1-255)\n");
    printf("  -i interval  wait interval seconds between packets (default: 1)\n");
    printf("  -w deadline  stop after N seconds regardless of packets sent\n");
    printf("  -?           display this help and exit\n");
}

int main(int argc, char **argv)
{
    t_ping_opts         opts;
    char                *hostname;
    char                resolved_ip[INET_ADDRSTRLEN];
    char                canonical_name[256];
    struct sockaddr_in  addr;
    int                 sockfd;
    int                 opt;
    int                 i;
    static struct option long_opts[] = {
        {"ttl", required_argument, 0, 't'},
        {0, 0, 0, 0}
    };

    opts.verbose   = 0;
    opts.quiet     = 0;
    opts.count     = 0;
    opts.ttl       = 0;
    opts.interval  = 1.0;
    opts.deadline  = 0;

    i = 1;
    while (i < argc)
    {
        if (strcmp(argv[i], "-?") == 0)
        {
            print_usage(argv[0]);
            return (0);
        }
        i++;
    }

    while ((opt = getopt_long(argc, argv, "vqc:i:w:", long_opts, NULL)) != -1)
    {
        if (opt == 'v')
            opts.verbose = 1;
        else if (opt == 'q')
            opts.quiet = 1;
        else if (opt == 'c')
            opts.count = atoi(optarg);
        else if (opt == 't')
        {
            opts.ttl = atoi(optarg);
            if (opts.ttl < 1 || opts.ttl > 255)
            {
                fprintf(stderr,
                        "ft_ping: invalid TTL value %d (must be 1-255)\n",
                        opts.ttl);
                return (1);
            }
        }
        else if (opt == 'i')
        {
            opts.interval = atof(optarg);
            if (opts.interval <= 0.0)
            {
                fprintf(stderr,
                        "ft_ping: invalid interval value %s (must be > 0)\n",
                        optarg);
                return (1);
            }
        }
        else if (opt == 'w')
        {
            opts.deadline = atoi(optarg);
            if (opts.deadline <= 0)
            {
                fprintf(stderr,
                        "ft_ping: invalid deadline value %s\n",
                        optarg);
                return (1);
            }
        }
        else
        {
            print_usage(argv[0]);
            return (1);
        }
    }

    if (optind >= argc)
    {
        fprintf(stderr, "ft_ping: usage error: Destination address required\n");
        print_usage(argv[0]);
        return (1);
    }
    hostname = argv[optind];

    if (getuid() != 0)
    {
        fprintf(stderr, "ft_ping: Operation not permitted\n");
        return (1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;

    if (resolve_hostname(hostname, &addr, resolved_ip, canonical_name) < 0)
    {
        fprintf(stderr, "ft_ping: %s: Name or service not known\n", hostname);
        return (1);
    }

    sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sockfd < 0)
    {
        perror("socket");
        return (1);
    }

    if (opts.ttl > 0)
    {
        if (setsockopt(sockfd, IPPROTO_IP, IP_TTL,
                       &opts.ttl, sizeof(opts.ttl)) < 0)
        {
            perror("setsockopt TTL");
            close(sockfd);
            return (1);
        }
    }

    signal(SIGINT, sig_handler);

    printf("PING %s (%s): %d data bytes\n",
           canonical_name, resolved_ip, PING_DATA_SIZE);

    ping_loop(sockfd, &addr, &opts);
    print_statistics(canonical_name);

    close(sockfd);
    return (g_state.stats.packets_received == 0) ? 1 : 0;
}