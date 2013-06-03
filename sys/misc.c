/*
  Copyright 2009-2013 Lianqi Wang <lianqiw@gmail.com> <lianqiw@tmt.org>
  
  This file is part of Multithreaded Adaptive Optics Simulator (MAOS).

  MAOS is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version.

  MAOS is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  MAOS.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef __linux__
#define _GNU_SOURCE 
#include <sched.h>
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <utime.h>
#include <fcntl.h>           /* For O_* constants */
#include <limits.h>
#include <errno.h>
#include <stdarg.h>
#include <dirent.h>
#include <time.h>
#include <ctype.h>
#include "common.h"
#include "thread.h"
#include "process.h"
#include "misc.h"
#include "path.h"

/**
   Obtain the basename of a file. The returnned string must be freed.
*/
char *mybasename(const char *fn){
    if(!fn || strlen(fn)==0) return NULL;
    char fn2[PATH_MAX];
    strncpy(fn2,fn, PATH_MAX);
    /*If this is a folder, remove the last / */
    if(fn2[strlen(fn2)-1]=='/')
	fn2[strlen(fn2)-1]='\0';
    char* sep=strrchr(fn2,'/');
    if(!sep){
	sep=fn2; 
    }else{
	sep++;
    }
    char *bn=malloc(strlen(sep)+1);
    strcpy(bn,sep);
    return bn;
}
/**
   Copy a file from file stream src to dest.
*/
static void copyfile_fp(FILE *dest, FILE *src){
    char buffer[4096];
    size_t br,bw;
    while(!feof(src)){
	br=fread(buffer, 1, 4096, src);
	if((bw=fwrite(buffer, 1, br, dest))!=br){
	    error("copyfile: Write failed %ld of %ld written.\n", 
		  (long)bw,(long)br);
	}
    }
}
/**
   Copy a file from src to dest
*/
void copyfile(const char *dest, const char *src){
    FILE *psrc=fopen(src,"rb");
    if(!psrc){
	error("Open source file failed\n");
    }
    FILE *pdest=fopen(dest,"wb");
    if(!pdest){
	error("Open destination file failed\n");
    }
    copyfile_fp(pdest, psrc);
}


/**
   Check the suffix of a file.
*/
int check_suffix(const char *fn, const char *suffix){
    if(!fn || !suffix) return 0;
    int lfn=strlen(fn);
    int lsu=strlen(suffix);
    if(lfn < lsu) return 0;
    if(mystrcmp(fn+lfn-lsu,suffix)){
	return 0;
    }else{
	return 1;
    }
}
/**
   Convert argc, argv to a single string, prefixed by the current directory.
*/
char *argv2str(int argc, const char *argv[], const char* delim){
    if(!argc) return NULL;
    char *cwd=mygetcwd();
    int slen=strlen(cwd)+2+strlen(HOME);
    if(!delim) delim=" ";
    for(int iarg=0; iarg<argc; iarg++){
	slen+=strlen(delim)+strlen(argv[iarg]);
    }
    char *scmd=calloc(slen, sizeof(char));
    if(!mystrcmp(cwd,HOME)){
	strcpy(scmd,"~");
	strcat(scmd,cwd+strlen(HOME));
    }else{
	strncpy(scmd,cwd, slen);
    }
    strcat(scmd,"/");
    char *exename=mybasename(argv[0]);
    strcat(scmd, exename);
    strcat(scmd, delim);
    free(exename);
    for(int iarg=1; iarg<argc; iarg++){
	if(argv[iarg] && strlen(argv[iarg])>0){
	    strcat(scmd, argv[iarg]);
	    strcat(scmd, delim);
	}
    }
    if(strlen(scmd)>slen-1) error("Overflow\n");
    free(cwd);
    char *scmdend=scmd+strlen(scmd);
    if(delim[0]!='\n'){
	for(char *p=scmd; p<scmdend; p++){
	    if(p[0]=='\n') p[0]=' ';
	}
    }
    while(scmdend>scmd+1 && scmdend[0]==delim[0] && scmdend[-1]==delim[0]){
	scmdend[0]='\0';
	scmdend--;
    }
    return scmd;
}
/**
   Print the content of a file.
*/
void print_file(const char *fnin){
    char *fn=search_file(fnin);
    if(!fn){
	warning("%s not found\n", fnin);
	return;
    }
    FILE *fp;
    if(!(fp=fopen(fn,"r"))){
	error("Open %s failed\n",fn);
    }
    copyfile_fp(stderr, fp);
    fflush(stderr);
    fclose(fp);
    free(fn);
}

/**
   Get current time in seconds as an integer.
*/
int myclocki(){
    time_t a;
    return time(&a);
}
/**
   Get current time in ascii string for easy print. The string
   contains spaces and is not suitable to use in filename. The
   returned string should not be modified.  */
const char *myasctime(void){
    static char st[64];
    time_t a;
    time(&a);
    ctime_r(&a, st);
    st[strlen(st)-1]='\0';/*remove final \n */
    return st;
}
/**
   Get furrent time in ascii string that doesn't contain
   spaces. Suitable for use in filenames. The returnned string
   must be freed. */
char *strtime(void){
    char str[64];
    time_t t=myclocki();
    struct tm tmp;
    localtime_r(&t,&tmp);/*don't free tmp */
    strftime(str,64,"%F-%H%M%S",&tmp);
    char pid[20];
    snprintf(pid, 20, "-%05d", (int)getpid());
    char *dir=stradd(str, pid, NULL);
    return dir;
}
/**
   Obtain the hostname of the current machine.
 */
const char *myhostname(void){
    static int inited=0;
    static char host[255];
    PNEW(lock);
    LOCK(lock);
    if(!inited){
	if(gethostname(host,255)){
	    warning("Unable to get hostname, set to localhost\n");
	    sprintf(host,"localhost");
	}
	inited=1;
    }
    UNLOCK(lock);
    return host;
}

/**
   Get current time in milli-second resolution.
*/
double myclockd(void){
#if defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec+(double)t.tv_nsec*1e-9;
#else
    struct timeval tk;
    gettimeofday(&tk,NULL);
    return (double)tk.tv_sec+(double)tk.tv_usec*1e-6;
#endif
}
/**
   Get current directory. The returnned string must be freed.
*/
char *mygetcwd(void){
    char cwd0[PATH_MAX];
    if(!getcwd(cwd0,PATH_MAX)) 
	error("Error getting current directory\n");
    return strdup(cwd0);
}
/**
   Translate a path into absolute path.
*/
char *myabspath(const char *path){
#if _BSD_SOURCE || _XOPEN_SOURCE >= 500
    return realpath(path, NULL);
#else
    char *cpath=mygetcwd();
    if(chdir(path)){
       error("path %s doesn't exist\n",path);
    }
    char *abspath=mygetcwd();
    if(chdir(cpath)){
	error("Unable to cd back to %s\n",cpath);
    }
    free(cpath);
    return abspath;
#endif
}

void mysymlink(const char *fn, const char *fnlink){
    if(!exist(fn)) return;
    remove(fnlink);
    if(symlink(fn, fnlink)){
	warning("Unable to make symlink %s->%s\n",fnlink,fn);
    }
}
/**
   Test whether a file exists.
*/
int exist(const char *fn){
    if(!fn) return 0;
    struct stat buf;
    return !stat(fn, &buf);
}

/**
   Test whether fn is directory
*/
int isdir(const char *fn){
    if(!fn) return 0;
    struct stat buf;
    return !stat(fn, &buf) && S_ISDIR(buf.st_mode);
}

/**
   Test whether fn is ordinary file
*/
int isfile(const char *fn){
    if(!fn) return 0;
    struct stat buf;
    return !stat(fn, &buf) && S_ISREG(buf.st_mode);
}

/**
   Test whether fn is a symbolic link
*/
int islink(const char *fn){
    if(!fn) return 0;
    struct stat buf;
    return !stat(fn, &buf) && S_ISLNK(buf.st_mode);
}
/**
   Test whether fd is a socket
*/
int issock(int fd){
    if(fd==-1) return 0;
    struct stat buf;
    return !fstat(fd, &buf) && S_ISSOCK(buf.st_mode);
}
/**
 * Compute length of file in Bytes
 */
size_t flen(const char *fn){
    if(!fn) return 0;
    struct stat buf;
    return stat(fn, &buf)?0:buf.st_size;
}
/**
   Return the modification time of the file
 */
time_t fmtime(const char *fn){
    struct stat buf;
    if(!fn || stat(fn, &buf)) return 0;
    return buf.st_ctime;
}
/**
   Concatenate many strings. Argument list must end with NULL.
*/
char *stradd(const char* a, ...){
    char *out;
    int n=0;
    va_list ap;
    va_start(ap,a);
    if(a){
	n=strlen(a)+1;
    }
    for(const char *arg=va_arg(ap,const char*); arg; arg=va_arg(ap,const char*)){
	n+=strlen(arg);
    }
    va_end(ap);
    out=calloc(n, sizeof(char));
    if(a){
	strcpy(out,a);
    }
    va_start(ap,a);
    for(const char *arg=va_arg(ap,const char*); arg; arg=va_arg(ap,const char*)){
	strcat(out,arg);
    }
    va_end(ap);
    return out;
}
/**
   Concatenate many strings, like stradd, but arguments are an array of char*
*/
char *strnadd(int argc, const char *argv[], const char* delim){
    if(!argc) return NULL;
    int slen=1;
    for(int iarg=0; iarg<argc; iarg++){
	slen+=strlen(delim)+strlen(argv[iarg]);
    }
    char *scmd=calloc(slen, sizeof(char));
    for(int iarg=0; iarg<argc; iarg++){
	if(argv[iarg] && strlen(argv[iarg])>0){
	    strcat(scmd,argv[iarg]);
	    strcat(scmd,delim);
	}
    }
    if(strlen(scmd)>slen-1) error("Overflow\n");
    char *scmdend=scmd+strlen(scmd)-1;
    while(scmdend>scmd+1 && scmdend[0]==delim[0] && scmdend[-1]==delim[0]){
	scmdend[0]='\0';
	scmdend--;
    }
    return scmd;
}
/**
   translate a filename into absolute file name that starts with /
*/
char *expand_filename(const char *fn){
    char *out;
    if(fn[0]=='~'){
	out=stradd(HOME,fn+1,NULL);
    }else{
	out=strdup(fn);
    }
    return out;
}
/**
   Duplicate a string. Check for NULL.
 */
char *mystrndup(const char *A, int len){
    int len2=strlen(A);
    if(len2<len) len=len2;
    char *B=malloc(len+1);
    memcpy(B,A,len);
    B[len]='\0';
    return B;
}

#if USE_MEM == 1
#undef strdup
/**
   declare strdup so my memory mangement mem.c is happy when DEBUG=1. Handles
NULL pointer correctly.  */
char *mystrdup(const char *A){
    if(!A){
	return NULL;
    }else{
	int nlen=strlen(A);
	char *B=malloc(nlen+1);
	memcpy(B,A,nlen+1);
	return B;
    }
}
#endif
/**
   Record the address of system strdup.
 */
char* (*strdup0)(const char *)=strdup;


/**
   Remove files that are older than sec seconds in folder fndir. If sec==0,
   remove everything.
*/
void remove_file_older(const char *fndir, long sec){
    DIR *dir=opendir(fndir);
    if(!dir){
	error("Unable to open directory %s\n",fndir);
    }
    struct dirent *dp;
    struct stat buf;
    char fnfull[PATH_MAX];
    long sec2=myclocki()-sec;
    while((dp=readdir(dir))){
	snprintf(fnfull,PATH_MAX,"%s/%s",fndir,dp->d_name);
	if(!stat(fnfull,&buf) && S_ISREG(buf.st_mode) && (buf.st_mtime<=sec2 || sec==0)){
	    remove(fnfull);
	    info2("Remove %s. %ld days old\n", fnfull, (long)(myclocki()-buf.st_mtime)/3600/24);
	}
    }
    closedir(dir);
}
/**
   Make dirs recursively. like mkdir -p in bash
*/
void mymkdir(const char *format, ...){
    format2fn;
    if(!fn) return;
    if(fn[strlen(fn)-1]=='/')
	fn[strlen(fn)-1]='/';
    if(mkdir(fn, 0700)==-1){
	if(errno==EEXIST){
	    return;
	}else if(errno==ENOENT){
	    char *tmp=strrchr(fn,'/');
	    if(!tmp){
		error("Unable to mkdir '%s'\n",fn);
	    }
	    tmp[0]='\0';
	    mymkdir("%s",fn);
	    tmp[0]='/';
	    if(mkdir(fn,0700)==-1&&errno!=EEXIST){
		error("Unable to mkdir '%s'\n",fn);
	    }
	}
    }
}
/**
   Compare two strings upto the length of b. if length of a is less than b,
   return false. 1 means not equal.
 */
int mystrcmp(const char *a, const char *b){
    if(!a || !b) return 1;
    int la=strlen(a);
    int lb=strlen(b);
    if(la==0 || lb==0 || la<lb){
	return 1;
    }else{
	return strncmp(a,b,lb);
    }
}

/**
   Make the fd close on exec.
*/
void cloexec(int fd){
    int oldflag=fcntl(fd, F_GETFD, 0);
    if(oldflag!=-1){
	oldflag |= FD_CLOEXEC;
	fcntl(fd, F_SETFD, oldflag);
    }
}
/**
   wrap of nanosleep
*/
void mysleep(double sec){
    struct timespec ts;
    ts.tv_sec=(time_t)trunc(sec);
    ts.tv_nsec=(long)((sec-ts.tv_sec)*1e9);
    nanosleep(&ts, NULL);
}

/**
   Pause
*/
void mypause(void){
    info2("Press ENTER key to continue.\n");
    int ans;
    while((ans=getchar())!='\n');
}
/**
   Return available space of mounted file system in bytes.
*/
long available_space(const char *path){
    struct statvfs buf;
    if(statvfs(path, &buf)){
	perror("statvfs");
	return 0;
    }else{
	return (long)buf.f_bsize * (long)buf.f_bavail;
    }
}
/**
   Extract a string constant from the command line, and output the position
   where the string terminates.*/
static char *cmd_string(char *input, char **end2){
    char *end;
    while(isspace((int)input[0]) || input[0]=='\n') input++;
    if(input[0]=='\'' || input[0]== '"'){
	end=strchr(input+1, input[0]);/*find matching quote. */
	input[0]=' ';
	input++;
	if(!end){
	    error("String does not end\n");
	}
    }else{
	end=strchr(input, ' ');//string end at space
    }
    end[0]='\0';
    char *out=strdup(input);
    memset(input, ' ', strlen(input));
    end[0]=' ';
    *end2=end+1;
    return out;
}
/**
   Parse command line arguments. Returns whatever is not yet parsed. Need to
   free the returned string. This is more relaxed than the built in getopd
*/
char *parse_argopt(int argc, const char *argv[], ARGOPT_T *options){
    char *cmds=strnadd(argc-1, argv+1, " ");
    char *cmds_end=cmds+(cmds?strlen(cmds):0);
    char *start=cmds;
    while(start<cmds_end){
        if(isspace((int)start[0]) || start[0]=='\n'){
	    start[0]=' ';
	    start++;
	    continue;
	}
	if(start[0]=='-'){
	    char *start0=start;
	    char key='0';
	    char *value;
	    int iopt=-1;
	    start++;
	    if(start[0]=='-'){/*long option, replace with short ones. */
		start++;
		for(int i=0; (options[i].name); i++){
		    if(!mystrcmp(start, options[i].name)){
			key=options[i].key;
			start+=strlen(options[i].name);
			while(isspace((int)start[0]) || start[0]=='\n'){
			    start[0]=' ';
			    start++;
			}
			if(start[0]=='=') {
			    start[0]=' ';
			    start++;
			}
			iopt=i;
			break;
		    }
		}
	    }else{
		key=start[0];
		start++;
		for(int i=0; (options[i].name); i++){
		    if(key==options[i].key){
			iopt=i;
			break;
		    }
		}
	    }
	    if(iopt==-1){
		continue;/*what don't want this key. */
	    }
	    if((options[iopt].opt & 1) == 1){
		value=start;
		while(value[0]=='\n' || isspace((int)value[0])){
		    value[0]=' ';
		    value++;
		}
	    }else{
		value=NULL;
	    }
	    int isfun=((options[iopt].opt&2)==2);
	    switch(options[iopt].type){
	    case 0:/*no result needed */
		break;
	    case T_INT:{
		int val=value?strtol(value, &start, 10):1;
		if(isfun){/*Is function */
		    void (*tmp)(int)=(void (*)(int))options[iopt].val;
		    tmp(val);
		}else{
		    int *tmp=options[iopt].val;
		    *tmp=val;
		}
	    }
		break;
	    case T_DBL:{
		double val=value?strtod(value, &start):1;
		if(isfun){/*Is function */
		    void (*tmp)(double)=(void (*)(double))options[iopt].val;
		    tmp(val);
		}else{
		    double *tmp=options[iopt].val;
		    *tmp=val;
		}
	    }
		break;
	    case T_STR:{
		char *val=value?cmd_string(value, &start):"Unknown";
		if(isfun){
		    void (*tmp)(char*)=(void (*)(char*))options[iopt].val;
		    tmp(val); free(val);
		}else{
		    char **tmp=options[iopt].val;
		    free(*tmp); *tmp=val;
		}
	    }
		break;
	    case T_INTARR:{
		if(isfun) error("Not implemented yet\n");
		int val=strtol(value, &start, 10);
		int **tmp=options[iopt].val;
		int *nval=options[iopt].nval;
		int i;
		for(i=0; i<*nval; i++){
		    if((*tmp)[i]==val) break;
		}
		if(i==*nval){
		    (*nval)++;
		    *tmp=realloc(*tmp, *nval*sizeof(int));
		    (*tmp)[(*nval)-1]=val;
		}
	    }
		break;
	    case T_DBLARR:{
		if(isfun) error("Not implemented yet\n");
		double val=strtod(value, &start);
		int **tmp=options[iopt].val;
		int *nval=options[iopt].nval;
		(*nval)++;
		*tmp=realloc(*tmp, *nval*sizeof(double));
		(*tmp)[(*nval)-1]=val;
	    }
		break;
	    }/*switch */
	    /*Empty the string that we already parsed. */
	    memset(start0, ' ',start-start0);
	}else if(start[0]=='='){/*equal sign found, key=value */
	    /*create a \n before the key. */
	    int skipspace=1;
	    for(char *start2=start-1; start2>=cmds; start2--){
  	        if(isspace((int)*start2) || *start2=='\n'){
		    if(!skipspace){
			*start2='\n';
			break;
		    }
		}else{
		    skipspace=0;
		}
	    }
	    start++;
	}else if(!mystrcmp(start, ".conf")){ /*.conf found. */
	    /*create a \n before the key. and \n after .conf */
	    for(char *start2=start-1; start2>=cmds; start2--){
	        if(isspace((int)*start2) || *start2=='\n'){
		    *start2='\n'; 
		    break;
		}
	    }
	    start+=5;
	    start[0]='\n';
	    start++;
	}else if(start[0]=='['){/*make sure we don't split brackets that are part of value. */
	    char *bend=strchr(start+1, ']');
	    char *bnextstart=strchr(start+1, '[');
	    if(bend && (!bnextstart || bend<bnextstart)){/*There is a closing bracket */
		for(; start<bend+1; start++){
		    if(start[0]=='\n') start[0]=' ';
		}
	    }else{
		error("Bracked is not closed\n");
		start++;
	    }
	}else if(start[0]=='\'' || start[0]=='"'){/*make sure we don't split strings that are part of value. */
	    char *quoteend=strchr(start, start[0]);
	    if(quoteend){
		start=quoteend+1;
	    }else{
		warning("Quote is not closed\n");
		start++;
	    }
	}else{
	    start++;
	}
    }
    return cmds;
}
#include <semaphore.h>
/**
   Block signals in critical region.
*/
int sig_block(int block){
    sigset_t set;
    sigfillset(&set);
    if(block){
	return sigprocmask(SIG_BLOCK, &set, NULL);
    }else{
	return sigprocmask(SIG_UNBLOCK, &set, NULL);
    }
}

int sem_lock(const char *key){
    sem_t *sem=sem_open(key, O_CREAT, 00700, 1);
    if(sem==SEM_FAILED){
	warning("sem_open failed\n");
	return -1;
    }else{
	int value;
	sem_getvalue(sem, &value);
	info2("Trying to lock %p (value=%d) ... ", sem, value);
	sem_wait(sem);
	if(sig_block(1)){
	    //block signal delivery in critical region.
	    warning("block signal failed\n");
	}
	info2("done\n");
	return 0;
    }
}
int sem_unlock(const char *key){
    sem_t *sem=sem_open(key, 0);
    if(sem==SEM_FAILED){
	warning("sem_open failed\n");
	return -1;
    }else{
	int value;
	sem_getvalue(sem, &value);
	info2("Trying to unlock %p (value=%d) ... ", sem, value);
	sem_post(sem);
	if(sig_block(0)){
	    warning("unblock signal failed\n");
	}
	info2("done\n");
	return 0;
    }
}
void maos_version(void){
    info2("MAOS Version %s. Compiled on %s %s by %s, %d bit", PACKAGE_VERSION, __DATE__, __TIME__, __VERSION__, (int)sizeof(long)*8);
#if USE_CUDA
    info2(", w/t CUDA");
#else
    info2(", w/o CUDA");
#endif
#ifdef __OPTIMIZE__
    info2(", w/t optimization.\n");
#else
    info2(", w/o optimization\n");
#endif
    info2("Source: %s\n", SRCDIR);
    info2("BUILD: %s\n", BUILDDIR);
    info2("Launched at %s in %s.\n",myasctime(),myhostname());
}

/**
   Set scheduling priorities for the process to enable real time behavior.
*/
void set_realtime(int icpu, int niceness){
    //Set CPU affinity.
#ifdef __linux__    
    cpu_set_t cpuset={{0}};
    CPU_SET(0, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
#endif
    //lock data in memory, avoid swapping.
    mlockall(MCL_FUTURE | MCL_CURRENT);
    //faile stack
    struct rlimit rl;
    if(!getrlimit(RLIMIT_STACK, &rl)){
	const int NSTACK=rl.rlim_cur/2;
	char tmp[NSTACK];
	memset(tmp, 0, NSTACK);
    }
    //Set only if we are root.
    if(getuid()==0){
	info2("Set priority to -20\n");
	setpriority(PRIO_PROCESS, getpid(), -20);
#ifdef __linux__
	struct sched_param param;
	sched_getparam(getpid(), &param);
	param.sched_priority=sched_get_priority_max(SCHED_FIFO);
	sched_setscheduler(getpid(), SCHED_FIFO, &param);
#endif
    }else{
	warning("Please run program as setsid or as root to lift priority\n");
    }
}