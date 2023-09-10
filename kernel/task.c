#include "task.h"
#include "arch.h"
#include "buffer.h"
#include "common.h"
#include "memory.h"
#include "message.h"
#include "module.h"
#include "vm.h"
#include "list.h"
#include "ipc.h"
#include <stdint.h>

struct task tasks[NUM_TASK_MAX];
static list_t  runqueue = (list_t) {
    .prev   = &runqueue,
    .next   = &runqueue
};

static int alloc_tid(void) {
    int i;
    for (i = 0; i < NUM_TASK_MAX; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            return i + 1;
        }
    }
    return 0;
}

static void init_task_struct(struct task *task, const char *name, int tid, uint32_t ip, uint32_t *arg) {
    // init message_box
    task->message_box.has_message = false;

    // init malloc_pool
    // In the current implementation, the top of the page is used as the header.
    LIST_INIT(&task->malloc_pool.pages);
    struct page *page = pmalloc(1);
    task->page_top = page;
    list_push_back(&task->malloc_pool.pages, &page->link);
    task->malloc_pool.next_ptr = align_up(page->base, 0x10);
   
    // init stack
    arch_task_init(task, ip, arg);

    // set task name
    memcpy(task->name, name, TASK_NAME_LEN);

    // set tid
    task->tid = tid;
}

void task_resume(struct task *task) {
    task->state = TASK_RUNNABLE;
    list_push_back(&runqueue, &task->next);
}

void task_block(struct task *task) {
    task->state = TASK_BLOCKED;
}

int task_create(const char *name, uint32_t ip, uint32_t *arg) {
    // alloc tid
    int tid = alloc_tid();
    if(!tid) {
        // todo: return error code
        PANIC("no free task slots");
    }

    // init task struct
    struct task *task = &tasks[tid - 1];
    init_task_struct(task, name, tid, ip, arg);

    // push to runqueue
    task_resume(task);

    return tid;
}

// todo: impl file system and fix this
// this is entry point of vm_task
static void launch_vm_task(struct buffer *buf) {
    module *m = new_module(buf);
    struct context *ctx = create_context(m);
    run_vm(ctx);
}

int vm_create(const char *name, void *image, int size) {
    struct buffer *buf = newbuffer(image, size);
    return task_create(name, (uint32_t)launch_vm_task, (uint32_t *)buf);
}

struct task *current_task;

struct task *schedule(void) {
    struct task *next = LIST_POP_TAIL(&runqueue, struct task, next);

    if(next)
        return next;

    if(current_task->state == TASK_RUNNABLE)
        return current_task;

    __builtin_unreachable();
}

// create idle task
void task_init(void) {
    int tid = alloc_tid();

    struct task *idle_task = &tasks[tid - 1];
    init_task_struct(idle_task,"idle", -1, 0, NULL);

    task_resume(idle_task);

    current_task = idle_task;
}

void task_switch(void) {
    struct task *prev = current_task;
    struct task *next = schedule();

    if(prev == next)
        return;
    
    //todo: push back to runqueue if not idle_task?

    current_task = next;
    arch_task_switch(prev, next);
}

int task_lookup(const char *name) {
    LIST_FOR_EACH(task, &runqueue, struct task, next) {
        if(strcmp(name, task->name) == 0)
            return task->tid;
    }

    return 0;
}

__attribute__((noreturn))
void task_exit(int32_t code) {
    // free memories
    pfree(current_task->page_top);

    // init state
    current_task->state = TASK_UNUSED;

    // send message to vm task
    struct message msg = {
        .type = EXIT_TASK_MSG,
        .exit_task = {.tid = current_task->tid}
    };
    ipc_send(3, &msg);

    // never reach here
    PANIC("unreachable");
    __builtin_unreachable();
}