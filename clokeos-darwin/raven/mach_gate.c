// Spike: ein Kindprozess führt x86_64-Maschinencode aus, "API-Aufrufe" (int3 mit rax = Nummer) werden vom Elternprozess
// über Mach-Ausnahmen beantwortet. Prüft task_for_pid, Ausnahmeports, thread_get/set_state und mach_vm_* auf PureDarwin.
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/exception.h>
#include <mach/thread_status.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#pragma pack(4)
typedef struct { mach_msg_header_t Head; mach_msg_body_t msgh_body; mach_msg_port_descriptor_t thread, task; NDR_record_t NDR;
  exception_type_t exception; mach_msg_type_number_t codeCnt; int64_t code[2]; } exc_request_t;
typedef struct { mach_msg_header_t Head; NDR_record_t NDR; kern_return_t RetCode; } exc_reply_t;
#pragma pack()
static int api_calls;

kern_return_t catch_mach_exception_raise(mach_port_t p, mach_port_t thread, mach_port_t task, exception_type_t type,
                                         mach_exception_data_t code, mach_msg_type_number_t n) {
  x86_thread_state64_t st; mach_msg_type_number_t cnt = x86_THREAD_STATE64_COUNT;
  kern_return_t kr = thread_get_state(thread, x86_THREAD_STATE64, (thread_state_t)&st, &cnt);
  printf("Ausnahme type=%d code0=%llx kr=%d rax=%llx rip=%llx\n", type, n ? code[0] : 0, kr, st.__rax, st.__rip);
  if (type == EXC_BREAKPOINT && kr == KERN_SUCCESS) {
    api_calls++;
    st.__rax = st.__rax * 2 + 1;      // "Antwort" der API
    // rip zeigt hinter das int3 (0xCC); bei EXC_BREAKPOINT auf x86 ist rip bereits nach dem Befehl
    thread_set_state(thread, x86_THREAD_STATE64, (thread_state_t)&st, cnt);
    return KERN_SUCCESS;
  }
  return KERN_FAILURE;
}
kern_return_t catch_mach_exception_raise_state(mach_port_t p, exception_type_t t, mach_exception_data_t c, mach_msg_type_number_t n,
    int *flavor, thread_state_t in, mach_msg_type_number_t incnt, thread_state_t out, mach_msg_type_number_t *outcnt) { return KERN_FAILURE; }
kern_return_t catch_mach_exception_raise_state_identity(mach_port_t p, mach_port_t th, mach_port_t task, exception_type_t t,
    mach_exception_data_t c, mach_msg_type_number_t n, int *flavor, thread_state_t in, mach_msg_type_number_t incnt,
    thread_state_t out, mach_msg_type_number_t *outcnt) { return KERN_FAILURE; }

int main(void) {
  int go[2]; pipe(go);
  pid_t pid = fork();
  if (pid == 0) {
    char c; read(go[0], &c, 1);                 // warten, bis der Elternprozess die Ausnahmeports gesetzt hat
    // Gastcode: mov eax,5 ; int3 ; mov ebx,eax ; mov eax,10 ; int3 ; ret
    unsigned char code[] = {0xb8,5,0,0,0, 0xcc, 0x89,0xc3, 0xb8,10,0,0,0, 0xcc, 0xc3};
    void *m = mmap(0, 4096, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANON, -1, 0);
    memcpy(m, code, sizeof code);
    long r; asm volatile("call *%1" : "=a"(r) : "r"(m) : "rbx", "memory");
    _exit((int)r);
  }
  mach_port_t task;
  kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);
  printf("task_for_pid: %d (%s)\n", kr, mach_error_string(kr));
  if (kr) { kill(pid, SIGKILL); return 1; }
  mach_port_t port;
  mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
  mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
  kr = task_set_exception_ports(task, EXC_MASK_BREAKPOINT | EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION, port,
                                EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, x86_THREAD_STATE64);
  printf("task_set_exception_ports: %d\n", kr);
  write(go[1], "x", 1);
  for (;;) {
    char buf[512], rep[512];
    mach_msg_header_t *h = (mach_msg_header_t *)buf;
    kr = mach_msg(h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof buf, port, 3000, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS) { printf("mach_msg: %d (%s)\n", kr, mach_error_string(kr)); break; }
    exc_request_t *rq = (exc_request_t *)buf; exc_reply_t *rp = (exc_reply_t *)rep;
    kern_return_t rc = catch_mach_exception_raise(port, rq->thread.name, rq->task.name, rq->exception, rq->code, rq->codeCnt);
    memset(rp, 0, sizeof *rp);
    rp->Head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(rq->Head.msgh_bits), 0);
    rp->Head.msgh_size = sizeof *rp; rp->Head.msgh_remote_port = rq->Head.msgh_remote_port;
    rp->Head.msgh_local_port = MACH_PORT_NULL; rp->Head.msgh_id = rq->Head.msgh_id + 100;
    rp->NDR = NDR_record; rp->RetCode = rc;
    mach_msg(&rp->Head, MACH_SEND_MSG, sizeof *rp, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
  }
  int status = 0; waitpid(pid, &status, 0);
  printf("API-Aufrufe: %d, Kind beendet: exit=%d signal=%d\n", api_calls, WEXITSTATUS(status), WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  return 0;
}
