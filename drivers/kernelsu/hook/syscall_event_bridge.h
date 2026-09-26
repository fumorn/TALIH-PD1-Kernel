#ifndef __KSU_H_SYSCALL_EVENT_BRIDGE
#define __KSU_H_SYSCALL_EVENT_BRIDGE

#include <linux/version.h>
#include <asm/ptrace.h>

long ksu_hook_newfstatat(int orig_nr, const struct pt_regs *regs);
long ksu_hook_faccessat(int orig_nr, const struct pt_regs *regs);
long ksu_hook_execve(int orig_nr, const struct pt_regs *regs);
long ksu_hook_execveat(int orig_nr, const struct pt_regs *regs);
long ksu_hook_setresuid(int orig_nr, const struct pt_regs *regs);
long ksu_hook_clone(int orig_nr, const struct pt_regs *regs);

void ksu_stop_ksud_execve_hook(void);

#endif // __KSU_H_SYSCALL_EVENT_BRIDGE

#if defined(__aarch64__) && LINUX_VERSION_CODE < KERNEL_VERSION(4, 17, 0)
void ksu_bridge_table_patch_init(void);
#endif
