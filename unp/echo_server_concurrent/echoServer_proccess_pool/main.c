/**
 * 进程池回射服务器
 * 文件上锁accept
 */


#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/uio.h> 
#include <netinet/in.h> /* sockaddr_in{} and other Internet defns */
#include <arpa/inet.h> /* inet(3) functions */
#include <errno.h> /* inet(3) functions */
#include <signal.h>
#include <sys/wait.h> 
#include <poll.h>
#include <limits.h>/*2.6.24以后没有open_max了*/
#include <pthread.h>
#include <signal.h> 
#include <sys/resource.h>/*void sig_int(int signo)*/
#include <fcntl.h>


#define MAXLINE 1024
#define N_CHILDREN 1024

static int nchildren = N_CHILDREN;
static pid_t *pids;//进程数组首地址

//回射服务函数
void str_echo(int socket);

//处理信号函数
typedef void Sigfunc(int); //简化
Sigfunc *signal(int signo, Sigfunc *func);
void sig_child(int signo);
void sig_int(int);

//创建进程
pid_t child_make(int, int, int);
void child_main(int, int, int);

//文件上锁
static struct flock lock_it, unlock_it;
static int lock_fd = -1;
void my_lock_init(char * pathname);
void my_lock_wait();
void my_lock_release();

int main(int argc, char** argv) {
	int listenfd, i;
	socklen_t addrlen;
    struct sockaddr_in seraddr;
    addrlen = sizeof (seraddr);

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        fprintf(stderr, "监听套接字创建失败，退出！\n");
        exit(-1);
    }

    memset(&seraddr, 0, sizeof (seraddr));
    seraddr.sin_family = AF_INET;
    
    if(argc == 1){
    	if (inet_pton(AF_INET, "127.0.0.1", &seraddr.sin_addr) <= 0) {
        	fprintf(stderr, "字节序转换失败，或为无效的格式！inet_pton error!\n");
        	exit(-1);
   		}
   		seraddr.sin_port = htons(9999);
    } else if(argc == 3) {
    	if (inet_pton(AF_INET, argv[1], &seraddr.sin_addr) <= 0) {
        	fprintf(stderr, "字节序转换失败！inet_pton error!\n");
        	exit(-1);
   		}
   		seraddr.sin_port = htons(atoi(argv[2]));
    } else {
    	fprintf(stderr, "不正确的命令行参数格式!\n");
        exit(-1);
    }

    if (bind(listenfd, (struct sockaddr*) &seraddr, sizeof (seraddr)) < 0) {
        fprintf(stderr, "绑定地址失败，退出！\n");
        exit(-1);
    }

    listen(listenfd, 10000);

	my_lock_init("/tmp/lock.XXXXXX");//创建临时文件并初始化锁
    pids = (pid_t*) calloc(nchildren, sizeof (pid_t));
    for (int i = 0; i < nchildren; i++) {
        child_make(i, listenfd, addrlen);//创建进程池
    }
    
    signal(SIGINT, sig_int);
    signal(SIGCHLD, sig_child);
    while (1)
        pause();

    return 0;
}


void str_echo(int socket) {
    ssize_t n;
    char buf[MAXLINE + 1];
again:

    while ((n = read(socket, buf, MAXLINE)) > 0) {
        buf[n] = 0;
        //        printf("%s", buf);
        write(socket, buf, n);
    }
    if (n < 0 && errno == EINTR)
        goto again;
    if (n < 0) {
        fprintf(stderr, "read error！\n");

        return;
    }
    //    printf("读取0字节！\n");
}

Sigfunc* signal(int signo, Sigfunc *func) {
    struct sigaction act, oact;

    act.sa_handler = func;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    if (signo == SIGALRM) {
#ifdef SA_INTERRUPT
        act.sa_flags |= SA_INTERRUPT; //|=按位或之后赋值
#endif
    } else {
#ifdef SA_RESTART
        act.sa_flags |= SA_RESTART;
#endif
    }
    if (sigaction(signo, &act, &oact) < 0)
        return (SIG_ERR);
    return (oact.sa_handler);
}

void sig_child(int signo) {
    pid_t pid;
    int stat;

    //    pid = wait(&stat);
    while ((pid = waitpid(-1, &stat, WNOHANG)) > 0)//正确应该调用waitpid,unp书上p110
        fprintf(stderr, "child %d terminated（子进程结束）\n", pid);
    return;
}

void sig_int(int signo) {
    void pr_cpu_time(void);

	int i;
	for( i =0; i < nchildren; i++)
		kill(pids[i], SIGTERM);

    pr_cpu_time();
    exit(0);
}

void pr_cpu_time(void) {/*for void sig_int(int signo)*/
    double user, sys;
    struct rusage myusage, childusage;

    if (getrusage(RUSAGE_SELF, &myusage) < 0) {
        fprintf(stdout, "getusage error\n");
        exit(1);
    }
    if (getrusage(RUSAGE_CHILDREN, &childusage) < 0) {
        fprintf(stdout, "getusage error\n");
        exit(1);
    }

    user = (double) myusage.ru_utime.tv_sec + myusage.ru_utime.tv_usec / 1000000.0;
    user += (double) childusage.ru_utime.tv_sec + childusage.ru_utime.tv_usec / 1000000.0;

    sys = (double) myusage.ru_utime.tv_sec + myusage.ru_utime.tv_usec / 1000000.0;
    sys += (double) childusage.ru_utime.tv_sec + childusage.ru_utime.tv_usec / 1000000.0;

    printf("\nuser time = %g, sys time = %g\n", user, sys);
}


pid_t child_make(int i, int listenfd, int addrlen)//创建进程
{
	pid_t pid;
	
	if((pid = fork()) > 0)
		return pid;
	child_main(i, listenfd, addrlen);
}

void child_main(int i, int listenfd, int addrlen){
	int connfd;
    socklen_t clilen;
    struct sockaddr* cliaddr;
    cliaddr = (struct sockaddr*) malloc(sizeof (struct sockaddr));

    printf("child: %d starting...\n", getpid());
    int nf = 0;
    for (;;) {
	printf("child: %d开始第%d次等待\n", getpid(), ++nf);
    clilen = addrlen;
    
	//进程各自accept
	//等待文件锁
	//任意时刻只有一个进程阻塞在accept，其他进程阻塞在获取锁的fcntl上
	my_lock_wait();
	connfd = accept(listenfd, cliaddr, &clilen);//
	my_lock_release();

	//服务次数
	printf("child: %d第%d次服务\n", getpid(), nf);

	str_echo(connfd);//回射函数
	close(connfd);
    }
}

void my_lock_init(char * pathname){
	char lockfile[1024];
	
	strncpy(lockfile, pathname, 1024);
	lock_fd = mkstemp(lockfile);//创建临时文件并立即删除
	unlink(lockfile);			//这样退出程序，临时文件也不在硬盘中
	
	lock_it.l_type = F_WRLCK;//初始化锁
	lock_it.l_whence = SEEK_SET;
	lock_it.l_start = 0;
	lock_it.l_len = 0;
	
	unlock_it.l_type = F_UNLCK;
	unlock_it.l_whence = SEEK_SET;
	unlock_it.l_start = 0;
	unlock_it.l_len = 0;
}

void my_lock_wait(){
	int rc;
	//上锁
	while((rc = fcntl(lock_fd, F_SETLKW, &lock_it)) < 0){
		if(errno == EINTR)
			continue;
		else{
			fprintf(stderr, "fcntl error for my_lock_wait\n");
			exit(1);
		}	
	}
}

void my_lock_release(){
	int rc;
	//解锁
	while((rc = fcntl(lock_fd, F_SETLKW, &unlock_it)) < 0){
		if(errno == EINTR)
			continue;
		else{
			fprintf(stderr, "fcntl error for my_lock_release\n");
			exit(1);
		}
	}	
}

