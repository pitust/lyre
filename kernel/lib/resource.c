#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/alloc.h>
#include <lib/errno.h>
#include <lib/resource.h>
#include <lib/print.h>
#include <sched/proc.h>
#include <abi-bits/fcntl.h>
#include <abi-bits/seek-whence.h>
#include <abi-bits/stat.h>
#include <abi-bits/signal.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <fs/vfs/vfs.h>
#include <time/time.h>

int resource_default_ioctl(struct resource *this, struct f_description *description, uint64_t request, uint64_t arg) {
    (void)this;
    (void)description;
    (void)arg;

    switch (request) {
        case TCGETS:
        case TCSETS:
        case TIOCSCTTY:
        case TIOCGWINSZ:
            errno = ENOTTY;
            return -1;
    }

    errno = EINVAL;
    return -1;
}

static ssize_t stub_read(struct resource *this, struct f_description *description, void *buf, off_t offset, size_t count) {
    (void)this;
    (void)description;
    (void)buf;
    (void)offset;
    (void)count;
    errno = ENOSYS;
    return -1;
}

static ssize_t stub_write(struct resource *this, struct f_description *description, const void *buf, off_t offset, size_t count) {
    (void)this;
    (void)description;
    (void)buf;
    (void)offset;
    (void)count;
    errno = ENOSYS;
    return -1;
}

static void *stub_mmap(struct resource *this, size_t file_page, int flags) {
    (void)this;
    (void)file_page;
    (void)flags;
    return NULL;
}

static bool stub_unref(struct resource *this, struct f_description *description) {
    (void)this;
    (void)description;
    this->refcount--;
    return true;
}

static bool stub_truncate(struct resource *this, struct f_description *description, size_t length) {
    (void)this;
    (void)description;
    (void)length;
    errno = ENOSYS;
    return false;
}

void *resource_create(size_t size) {
    struct resource *res = alloc(size, ALLOC_RESOURCE);
    if (res == NULL) {
        return NULL;
    }

    res->res_size = size;
    res->read = stub_read;
    res->write = stub_write;
    res->ioctl = resource_default_ioctl;
    res->mmap = stub_mmap;
    res->unref = stub_unref;
    res->truncate = stub_truncate;
    return res;
}

void resource_free(struct resource *res) {
    free(res, res->res_size, ALLOC_RESOURCE);
}

dev_t resource_create_dev_id(void) {
    static dev_t dev_id_counter = 1;
    static spinlock_t lock = SPINLOCK_INIT;
    spinlock_acquire(&lock);
    dev_t ret = dev_id_counter++;
    spinlock_release(&lock);
    return ret;
}

bool fdnum_close(struct process *proc, int fdnum) {
    if (proc == NULL) {
        proc = sched_current_thread()->process;
    }

    bool ok = false;
    spinlock_acquire(&proc->fds_lock);

    if (fdnum < 0 || fdnum >= MAX_FDS) {
        errno = EBADF;
        goto cleanup;
    }

    struct f_descriptor *fd = proc->fds[fdnum];
    if (fd == NULL) {
        errno = EBADF;
        goto cleanup;
    }

    fd->description->res->unref(fd->description->res, fd->description);

    if (fd->description->refcount-- == 1) {
        FREE(fd->description, ALLOC_RESOURCE);
    }

    FREE(fd, ALLOC_RESOURCE);

    ok = true;
    proc->fds[fdnum] = NULL;

cleanup:
    spinlock_release(&proc->fds_lock);
    return ok;
}

int fdnum_create_from_fd(struct process *proc, struct f_descriptor *fd, int old_fdnum, bool specific) {
    if (proc == NULL) {
        proc = sched_current_thread()->process;
    }

    int res = -1;
    spinlock_acquire(&proc->fds_lock);

    if (old_fdnum < 0 || old_fdnum >= MAX_FDS) {
        errno = EBADF;
        goto cleanup;
    }

    if (!specific) {
        for (int i = old_fdnum; i < MAX_FDS; i++) {
            if (proc->fds[i] == NULL) {
                proc->fds[i] = fd;
                res = i;
                goto cleanup;
            }
        }
    } else {
        // TODO: Close an existing descriptor without deadlocking :^)
        // fdnum_close(proc, old_fdnum);
        proc->fds[old_fdnum] = fd;
        res = old_fdnum;
    }

cleanup:
    spinlock_release(&proc->fds_lock);
    return res;
}

int fdnum_create_from_resource(struct process *proc, struct resource *res, int flags,
                               int old_fdnum, bool specific) {
    struct f_descriptor *fd = fd_create_from_resource(res, flags);
    if (fd == NULL) {
        return -1;
    }

    return fdnum_create_from_fd(proc, fd, old_fdnum, specific);
}

int fdnum_dup(struct process *old_proc, int old_fdnum, struct process *new_proc, int new_fdnum,
              int flags, bool specific, bool cloexec) {
    if (old_proc == NULL) {
        old_proc = sched_current_thread()->process;
    }

    if (new_proc == NULL) {
        new_proc = sched_current_thread()->process;
    }

    if (specific && old_fdnum == new_fdnum && old_proc == new_proc) {
        errno = EINVAL;
        return -1;
    }

    struct f_descriptor *old_fd = fd_from_fdnum(old_proc, old_fdnum);
    if (old_fd == NULL) {
        return -1;
    }

    struct f_descriptor *new_fd = ALLOC(struct f_descriptor, ALLOC_RESOURCE);
    if (new_fd == NULL) {
        errno = ENOMEM;
        return -1;
    }

    memcpy(new_fd, old_fd, sizeof(struct f_descriptor));

    new_fdnum = fdnum_create_from_fd(new_proc, new_fd, new_fdnum, specific);
    if (new_fdnum < 0) {
        FREE(new_fd, ALLOC_RESOURCE);
        return -1;
    }

    new_fd->flags = flags & FILE_DESCRIPTOR_FLAGS_MASK;
    if (cloexec) {
        new_fd->flags &= O_CLOEXEC;
    }

    old_fd->description->refcount++;
    old_fd->description->res->refcount++;

    return new_fdnum;
}

struct f_descriptor *fd_create_from_resource(struct resource *res, int flags) {
    res->refcount++;

    struct f_description *description = ALLOC(struct f_description, ALLOC_RESOURCE);
    if (description == NULL) {
        goto fail;
    }

    description->refcount = 1;
    description->flags = flags & FILE_STATUS_FLAGS_MASK;
    description->lock = SPINLOCK_INIT;
    description->res = res;

    struct f_descriptor *fd = ALLOC(struct f_descriptor, ALLOC_RESOURCE);
    if (fd == NULL) {
        goto fail;
    }

    fd->description = description;
    fd->flags = flags & FILE_DESCRIPTOR_FLAGS_MASK;
    return fd;

fail:
    res->refcount--;
    if (description != NULL) {
        FREE(description, ALLOC_RESOURCE);
    }
    return NULL;
}

struct f_descriptor *fd_from_fdnum(struct process *proc, int fdnum) {
    if (proc == NULL) {
        proc = sched_current_thread()->process;
    }

    struct f_descriptor *ret = NULL;
    spinlock_acquire(&proc->fds_lock);

    if (fdnum < 0 || fdnum >= MAX_FDS) {
        errno = EBADF;
        goto cleanup;
    }

    ret = proc->fds[fdnum];
    if (ret == NULL) {
        errno = EBADF;
        goto cleanup;
    }

    ret->description->refcount++;

cleanup:
    spinlock_release(&proc->fds_lock);
    return ret;
}

int syscall_close(void *_, int fdnum) {
    (void)_;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    debug_print("syscall (%d %s): close(%d)", proc->pid, proc->name, fdnum);

    return fdnum_close(proc, fdnum) ? 0 : -1;
}

ssize_t syscall_read(void *_, int fdnum, void *buf, size_t count) {
    (void)_;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    debug_print("syscall (%d %s): read(%d, %lx, %lu)", proc->pid, proc->name, fdnum, buf, count);

    struct f_descriptor *fd = fd_from_fdnum(proc, fdnum);
    if (fd == NULL) {
        return -1;
    }

    struct f_description *description = fd->description;
    struct resource *res = description->res;

    ssize_t read = res->read(res, description, buf, description->offset, count);
    if (read < 0) {
        return -1;
    }

    description->offset += read;
    return read;
}

ssize_t syscall_write(void *_, int fdnum, const void *buf, size_t count) {
    (void)_;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    debug_print("syscall (%d %s): write(%d, %lx, %lu)", proc->pid, proc->name, fdnum, buf, count);

    struct f_descriptor *fd = fd_from_fdnum(proc, fdnum);
    if (fd == NULL) {
        return -1;
    }

    struct f_description *description = fd->description;
    struct resource *res = description->res;

    ssize_t written = res->write(res, description, buf, description->offset, count);
    if (written < 0) {
        return -1;
    }

    description->offset += written;
    return written;
}

off_t syscall_seek(void *_, int fdnum, off_t offset, int whence) {
    (void)_;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    debug_print("syscall (%d %s): seek(%d, %ld, %d)", proc->pid, proc->name, fdnum, offset, whence);

    struct f_descriptor *fd = fd_from_fdnum(proc, fdnum);
    if (fd == NULL) {
        return -1;
    }

    struct f_description *description = fd->description;
    switch (description->res->stat.st_mode & S_IFMT) {
        case S_IFCHR:
        case S_IFIFO:
        case S_IFSOCK:
            errno = ESPIPE;
            return -1;
    }

    off_t curr_offset = description->offset;
    off_t new_offset = 0;

    switch (whence) {
        case SEEK_CUR:
            new_offset = curr_offset + offset;
            break;
        case SEEK_END:
            new_offset = curr_offset + description->res->stat.st_size;
            break;
        case SEEK_SET:
            new_offset = offset;
            break;
        default:
            errno = EINVAL;
            return -1;
    }

    if (new_offset < 0) {
        errno = EINVAL;
        return -1;
    }

    // TODO: Implement res->grow
    // if (new_offset >= fd->description->res->stat.st_size) {
    //     description->res->grow(description->res, new_offset);
    // }

    description->offset = new_offset;
    return new_offset;
}

int syscall_fcntl(void *_, int fdnum, uint64_t request, uint64_t arg) {
    (void)_;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    debug_print("syscall (%d %s): fcntl(%d, %lu, %lx)", proc->pid, proc->name, fdnum, request, arg);

    struct f_descriptor *fd = fd_from_fdnum(proc, fdnum);
    if (fd == NULL) {
        return -1;
    }

    switch (request) {
        case F_DUPFD:
            return fdnum_dup(proc, fdnum, proc, (int)arg, 0, false, false);
        case F_DUPFD_CLOEXEC:
            return fdnum_dup(proc, fdnum, proc, (int)arg, 0, false, true);
        case F_GETFD:
            if ((fd->flags & O_CLOEXEC) != 0) {
                return O_CLOEXEC;
            } else {
                return 0;
            }
        case F_SETFD:
            if ((arg & O_CLOEXEC) != 0) {
                fd->flags = O_CLOEXEC;
            } else {
                fd->flags = 0;
            }
            return 0;
        case F_GETFL:
            return fd->description->flags;
        case F_SETFL:
            fd->description->flags = (int)arg;
            return 0;
        default:
            debug_print("fcntl: Unhandled request %lx\n", request);
            errno = EINVAL;
            return -1;
    }
}

int syscall_ioctl(void *_, int fdnum, uint64_t request, uint64_t arg) {
    (void)_;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    debug_print("syscall (%d %s): ioctl(%d, %lu, %lx)", proc->pid, proc->name, fdnum, request, arg);

    struct f_descriptor *fd = fd_from_fdnum(proc, fdnum);
    if (fd == NULL) {
        return -1;
    }

    struct f_description *description = fd->description;
    struct resource *res = description->res;
    return res->ioctl(res, description, request, arg);
}

int syscall_dup3(void *_, int old_fdnum, int new_fdnum, int flags) {
    (void)_;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    debug_print("syscall (%d %s): dup3(%d, %d, %x)", proc->pid, proc->name, old_fdnum, new_fdnum, flags);

    return fdnum_dup(proc, old_fdnum, proc, new_fdnum, flags, true, false);
}

int syscall_fchmodat(void *_, int dir_fdnum, const char *path, mode_t mode, int flags) {
    (void)_;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    debug_print("syscall (%d %s): fchmodat(%d, %s, %x, %x)", proc->pid, proc->name, dir_fdnum, path, mode, flags);

    struct vfs_node *parent = NULL, *node = NULL;
    if (!vfs_fdnum_path_to_node(dir_fdnum, path, true, true, &parent, &node, NULL)) {
        return -1;
    }

    struct vfs_node *target = node;
    if (target == NULL) {
        target = parent;
    }

    target->resource->stat.st_mode &= ~0777;
    target->resource->stat.st_mode |= mode & 0777;
    return 0;
}

#define MAX_PPOLL_FDS 32

int syscall_ppoll(void *_, struct pollfd *fds, nfds_t nfds, const struct timespec *timeout, sigset_t *sigmask) {
    (void)_;

    struct thread *thread = sched_current_thread();
    struct process *proc = thread->process;

    debug_print("syscall (%d %s): ppoll(%lx, %lu, %lx, %lx)", proc->pid, proc->name, fds, nfds, timeout, sigmask);

    int fd_count = 0, event_count = 0, ret = 0;
    int fd_nums[MAX_PPOLL_FDS];
    struct f_descriptor *fd_list[MAX_PPOLL_FDS];
    struct event *events[MAX_PPOLL_FDS];
    struct timer *timer = NULL;

    // XXX no signals yet

    if (nfds > MAX_PPOLL_FDS) {
        ret = -1;
        errno = EINVAL;
        goto cleanup;
    }

    for (size_t i = 0; i < nfds; i++) {
        struct pollfd *pollfd = &fds[i];

        pollfd->revents = 0;
        if (pollfd->fd < 0) {
            continue;
        }

        struct f_descriptor *fd = fd_from_fdnum(proc, pollfd->fd);
        if (fd == NULL) {
            pollfd->revents = POLLNVAL;
            ret++;
            continue;
        }

        struct resource *res = fd->description->res;
        int status = res->status;

        if (((uint16_t)status & pollfd->events) != 0) {
            pollfd->revents = (uint16_t)status & pollfd->events;
            ret++;
            res->unref(res, fd->description);
            continue;
        }

        fd_list[fd_count] = fd;
        fd_nums[fd_count] = i;
        events[event_count] = &res->event;

        fd_count++;
        event_count++;
    }

    if (ret != 0) {
        goto cleanup;
    }

    if (timeout != NULL) {
        timer = timer_new(*timeout);
        if (timer == NULL) {
            errno = ENOMEM;
            ret = -1;
            goto cleanup;
        }

        events[event_count++] = &timer->event;
    }

    for (;;) {
        ssize_t which = event_await(events, event_count, true);
        if (which == -1) {
            ret = -1;
            errno = EINTR;
            goto cleanup;
        }

        if (timer != NULL && which == event_count - 1) {
            ret = 0;
            goto cleanup;
        }

        struct pollfd *pollfd = &fds[fd_nums[which]];
        struct f_descriptor *fd = fd_list[which];
        struct resource *res = fd->description->res;

        int status = res->status;
        if (((uint16_t)status & pollfd->events) != 0) {
            pollfd->revents = (uint16_t)status & pollfd->events;
            ret++;
            break;
        }
    }

cleanup:
    for (int i = 0; i < fd_count; i++) {
        struct f_descriptor *fd = fd_list[i];
        struct resource *res = fd->description->res;

        res->unref(res, fd->description);
    }

    if (timer != NULL) {
        timer_disarm(timer);
        FREE(timer, ALLOC_MISC);
    }

    return ret;
}
