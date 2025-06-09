#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);

void sys_exit (int);

struct file *process_get_file_by_fd(int fd);

#endif /* userprog/syscall.h */
