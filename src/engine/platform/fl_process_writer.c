#include "fl_process_writer.h"
#include "core/fl_clock.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
struct Fl_ProcessWriter {HANDLE process,job,input; bool reaped;};

Fl_ProcessWriter *fl_process_writer_open(const char *command)
{
    if (!command || !command[0]) return NULL;
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,command,-1,NULL,0);
    if (n<=0 || n>32700) return NULL;
    wchar_t *line=calloc((size_t)n+32,sizeof(*line));
    Fl_ProcessWriter *p=calloc(1,sizeof(*p)); HANDLE input=NULL,output=NULL;
    SECURITY_ATTRIBUTES security={.nLength=sizeof(security),.bInheritHandle=TRUE};
    bool ok=line && p && CreatePipe(&input,&output,&security,0) && SetHandleInformation(output,HANDLE_FLAG_INHERIT,0);
    if (ok) {
        memcpy(line,L"cmd.exe /d /s /c \"",18*sizeof(*line));
        ok=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,command,-1,line+18,n)>0;
        line[18+n-1]=L'"'; line[18+n]=0;
        p->job=CreateJobObjectW(NULL,NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0}; limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ok=ok && p->job && SetInformationJobObject(p->job,JobObjectExtendedLimitInformation,&limits,sizeof(limits));
    }
    PROCESS_INFORMATION child={0};
    if (ok) {
        STARTUPINFOW startup={.cb=sizeof(startup),.dwFlags=STARTF_USESTDHANDLES,.hStdInput=input,
            .hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE),.hStdError=GetStdHandle(STD_ERROR_HANDLE)};
        ok=CreateProcessW(NULL,line,NULL,NULL,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,NULL,NULL,&startup,&child)!=0;
        if (ok) {
            ok=AssignProcessToJobObject(p->job,child.hProcess) && ResumeThread(child.hThread)!=(DWORD)-1;
            if (!ok) {TerminateProcess(child.hProcess,1); WaitForSingleObject(child.hProcess,INFINITE); CloseHandle(child.hProcess);}
            else {p->process=child.hProcess; p->input=output; output=NULL;}
            CloseHandle(child.hThread);
        }
    }
    if (input) CloseHandle(input); if (output) CloseHandle(output); free(line);
    if (!ok) {if (p && p->job) CloseHandle(p->job); free(p); return NULL;}
    return p;
}
void fl_process_writer_cancel(Fl_ProcessWriter *p) {if (p && p->job) TerminateJobObject(p->job,1);}
bool fl_process_writer_write(Fl_ProcessWriter *p, const void *bytes, size_t size, const atomic_bool *cancel)
{
    const unsigned char *at=bytes;
    while (size) {
        if (cancel && atomic_load_explicit(cancel,memory_order_relaxed)) {fl_process_writer_cancel(p); return false;}
        DWORD written=0,chunk=(DWORD)(size>1048576 ? 1048576 : size);
        if (!WriteFile(p->input,at,chunk,&written,NULL) || !written) return false;
        at+=written; size-=written;
    }
    return true;
}
bool fl_process_writer_wait_timeout(Fl_ProcessWriter *p, const atomic_bool *cancel, uint32_t timeout_ms)
{
    uint64_t deadline=timeout_ms ? fl_clock_monotonic_ns()+(uint64_t)timeout_ms*UINT64_C(1000000) : 0;
    if (p->input) {CloseHandle(p->input); p->input=NULL;}
    for (;;) {
        if ((cancel && atomic_load_explicit(cancel,memory_order_relaxed)) || (deadline && fl_clock_monotonic_ns()>=deadline)) fl_process_writer_cancel(p);
        DWORD result=WaitForSingleObject(p->process,50);
        if (result==WAIT_OBJECT_0) break;
        if (result!=WAIT_TIMEOUT) return false;
    }
    DWORD status=1; p->reaped=true; return GetExitCodeProcess(p->process,&status) && status==0;
}
void fl_process_writer_destroy(Fl_ProcessWriter *p)
{
    if (!p) return;
    if (!p->reaped) {fl_process_writer_cancel(p); fl_process_writer_wait(p,NULL);}
    if (p->input) CloseHandle(p->input); CloseHandle(p->process); CloseHandle(p->job); free(p);
}
#else
#include <spawn.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <pthread.h>
#include "core/fl_thread.h"
extern char **environ;
struct Fl_ProcessWriter {pid_t pid; int input; bool reaped; Fl_Mutex mutex;};

Fl_ProcessWriter *fl_process_writer_open(const char *command)
{
    if (!command || !command[0]) return NULL;
    Fl_ProcessWriter *p=calloc(1,sizeof(*p)); if (!p) return NULL;
    if (!fl_mutex_init(&p->mutex)) {free(p); return NULL;}
    int descriptors[2]; if (pipe(descriptors)) {fl_mutex_destroy(&p->mutex); free(p); return NULL;}
    bool ok=true;
    for (size_t i=0;i<2;++i) if (descriptors[i]<3) {
        int promoted=fcntl(descriptors[i],F_DUPFD,3);
        if (promoted<0) {ok=false; break;}
        close(descriptors[i]); descriptors[i]=promoted;
    }
    ok=ok && fcntl(descriptors[0],F_SETFD,FD_CLOEXEC)==0 && fcntl(descriptors[1],F_SETFD,FD_CLOEXEC)==0;
    posix_spawn_file_actions_t actions; posix_spawnattr_t attributes;
    bool have_actions=posix_spawn_file_actions_init(&actions)==0,have_attributes=posix_spawnattr_init(&attributes)==0;
    ok=ok && have_actions && have_attributes;
    if (ok) ok=posix_spawn_file_actions_adddup2(&actions,descriptors[0],STDIN_FILENO)==0 &&
        posix_spawn_file_actions_addclose(&actions,descriptors[0])==0 && posix_spawn_file_actions_addclose(&actions,descriptors[1])==0 &&
        posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETPGROUP)==0 && posix_spawnattr_setpgroup(&attributes,0)==0;
    char *argv[]={"sh","-c",(char *)command,NULL};
    if (ok) ok=posix_spawn(&p->pid,"/bin/sh",&actions,&attributes,argv,environ)==0;
    if (have_actions) posix_spawn_file_actions_destroy(&actions);
    if (have_attributes) posix_spawnattr_destroy(&attributes);
    close(descriptors[0]); p->input=descriptors[1];
    if (ok) ok=fcntl(p->input,F_SETFL,O_NONBLOCK)==0;
    if (!ok) {if (p->pid>0) {kill(-p->pid,SIGKILL); while (waitpid(p->pid,NULL,0)<0 && errno==EINTR) {}} close(p->input); fl_mutex_destroy(&p->mutex); free(p); return NULL;}
    return p;
}

void fl_process_writer_cancel(Fl_ProcessWriter *p)
{
    if (!p) return;
    fl_mutex_lock(&p->mutex); if (!p->reaped && p->pid>0) kill(-p->pid,SIGKILL); fl_mutex_unlock(&p->mutex);
}

bool fl_process_writer_write(Fl_ProcessWriter *p, const void *bytes, size_t size, const atomic_bool *cancel)
{
    sigset_t blocked,previous,pending; sigemptyset(&blocked); sigaddset(&blocked,SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK,&blocked,&previous)) return false;
    sigpending(&pending); bool had_signal=sigismember(&pending,SIGPIPE)==1,ok=true;
    const unsigned char *at=bytes;
    while (size) {
        if (cancel && atomic_load_explicit(cancel,memory_order_relaxed)) {fl_process_writer_cancel(p); ok=false; break;}
        struct pollfd descriptor={.fd=p->input,.events=POLLOUT};
        int ready=poll(&descriptor,1,50);
        if (ready<0 && errno==EINTR) continue;
        if (ready<0 || (ready>0 && (descriptor.revents&(POLLERR|POLLHUP|POLLNVAL)))) {ok=false; break;}
        if (!ready) continue;
        ssize_t n=write(p->input,at,size>1048576 ? 1048576 : size);
        if (n<0 && (errno==EINTR || errno==EAGAIN)) continue;
        if (n<=0) {ok=false; break;}
        at+=(size_t)n; size-=(size_t)n;
    }
    if (!had_signal) {
        sigpending(&pending);
        if (sigismember(&pending,SIGPIPE)==1) {int signal_number; sigwait(&blocked,&signal_number);}
    }
    pthread_sigmask(SIG_SETMASK,&previous,NULL); return ok;
}

bool fl_process_writer_wait_timeout(Fl_ProcessWriter *p, const atomic_bool *cancel, uint32_t timeout_ms)
{
    uint64_t deadline=timeout_ms ? fl_clock_monotonic_ns()+(uint64_t)timeout_ms*UINT64_C(1000000) : 0;
    if (p->input>=0) {close(p->input); p->input=-1;}
    for (;;) {
        if ((cancel && atomic_load_explicit(cancel,memory_order_relaxed)) || (deadline && fl_clock_monotonic_ns()>=deadline)) fl_process_writer_cancel(p);
        fl_mutex_lock(&p->mutex);
        int status=0; pid_t result=waitpid(p->pid,&status,WNOHANG); int failure=errno;
        bool done=result==p->pid || (result<0 && failure!=EINTR);
        if (done) p->reaped=true;
        fl_mutex_unlock(&p->mutex);
        if (done) return result==p->pid && WIFEXITED(status) && WEXITSTATUS(status)==0;
        poll(NULL,0,20);
    }
}
void fl_process_writer_destroy(Fl_ProcessWriter *p)
{
    if (!p) return;
    if (!p->reaped) {fl_process_writer_cancel(p); fl_process_writer_wait(p,NULL);}
    if (p->input>=0) close(p->input); fl_mutex_destroy(&p->mutex); free(p);
}
#endif

/* Unbounded successful encoding still observes cancellation on its worker. */
bool fl_process_writer_wait(Fl_ProcessWriter *p, const atomic_bool *cancel)
{
    return fl_process_writer_wait_timeout(p,cancel,0);
}
