#include "linux/compiler.h"
#include "linux/cred.h"
#include "linux/jump_label.h"
#include "linux/printk.h"
#include "linux/string.h"
#include "linux/uidgid.h"
#include "selinux/selinux.h"
#include <asm/syscall.h>
#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <linux/static_key.h>

#include "arch.h"
#include "klog.h" // IWYU pragma: keep
#include "hook/tp_marker.h"
#include "feature/sucompat.h"
#include "hook/setuid_hook.h"
#include "policy/app_profile.h"
#include "runtime/ksud.h"
#include "sulog/event.h"
#include "hook/syscall_hook.h"
#include "hook/syscall_event_bridge.h"
#include "feature/adb_root.h"

static int ksu_handle_init_mark_tracker(const char __user **filename_user)
{
    char path[64];
    unsigned long addr;
    const char __user *fn;
    long ret;

    if (unlikely(!filename_user))
        return 0;

    addr = untagged_addr((unsigned long)*filename_user);
    fn = (const char __user *)addr;
    ret = strncpy_from_user(path, fn, sizeof(path));
    if (ret < 0)
        return 0;

    path[sizeof(path) - 1] = '\0';
    if (unlikely(strcmp(path, KSUD_PATH) == 0)) {
        pr_info("hook_manager: escape to root for init executing ksud: %d\n", current->pid);
        escape_to_root_for_init();
    } else if (likely(strstr(path, "/app_process") == NULL && strstr(path, "/adbd") == NULL &&
                      strstr(path, "/stub_zygote") == NULL)) {
        pr_info("hook_manager: unmark %d exec %s\n", current->pid, path);
        ksu_clear_task_tracepoint_flag_if_needed(current);
    }

    return 0;
}

long __nocfi ksu_hook_newfstatat(int orig_nr, const struct pt_regs *regs)
{
    if (!ksu_su_compat_enabled)
        return ksu_invoke_orig_syscall(orig_nr, (const struct pt_regs *)regs);

    return ksu_handle_stat_sucompat(orig_nr, (struct pt_regs *)regs);
}

long __nocfi ksu_hook_faccessat(int orig_nr, const struct pt_regs *regs)
{
    if (!ksu_su_compat_enabled)
        return ksu_invoke_orig_syscall(orig_nr, (const struct pt_regs *)regs);

    return ksu_handle_faccessat_sucompat(orig_nr, (struct pt_regs *)regs);
}

DEFINE_STATIC_KEY_TRUE(ksud_execve_key);

void ksu_stop_ksud_execve_hook()
{
    static_branch_disable(&ksud_execve_key);
}

static long __nocfi ksu_hook_execve_common(int orig_nr, const struct pt_regs *regs, bool execveat)
{
    const char __user **filename_user =
        execveat ? (const char __user **)&PT_REGS_PARM2(regs) : (const char __user **)&PT_REGS_PARM1(regs);
    const char __user *const __user *argv_user = execveat ? (const char __user *const __user *)PT_REGS_PARM3(regs) :
                                                            (const char __user *const __user *)PT_REGS_PARM2(regs);
    bool current_is_init = is_init(current_cred());
    struct ksu_sulog_pending_event *pending_root_execve = NULL;
    long ret;

    if (static_branch_unlikely(&ksud_execve_key)) {
        if (execveat) {
            ksu_execveat_hook_ksud(regs);
        } else {
            ksu_execve_hook_ksud(regs);
        }
    }

    if (current_euid().val == 0)
        pending_root_execve = ksu_sulog_capture_root_execve(*filename_user, argv_user, GFP_KERNEL);

    if (current->pid != 1 && current_is_init) {
        ksu_handle_init_mark_tracker(filename_user);
        ret = execveat ? ksu_adb_root_handle_execveat((struct pt_regs *)regs) :
                         ksu_adb_root_handle_execve((struct pt_regs *)regs);
        if (ret) {
            pr_err("adb root failed: %ld\n", ret);
        }
    } else if (ksu_su_compat_enabled) {
        ret = execveat ? ksu_handle_execveat_sucompat(filename_user, orig_nr, (struct pt_regs *)regs) :
                         ksu_handle_execve_sucompat(filename_user, orig_nr, (struct pt_regs *)regs);
        ksu_sulog_emit_pending(pending_root_execve, ret, GFP_KERNEL);
        return ret;
    }

    ret = ksu_invoke_orig_syscall(orig_nr, (const struct pt_regs *)regs);
    ksu_sulog_emit_pending(pending_root_execve, ret, GFP_KERNEL);
    return ret;
}

long __nocfi ksu_hook_execve(int orig_nr, const struct pt_regs *regs)
{
    return ksu_hook_execve_common(orig_nr, regs, false);
}

long __nocfi ksu_hook_execveat(int orig_nr, const struct pt_regs *regs)
{
    return ksu_hook_execve_common(orig_nr, regs, true);
}

long __nocfi ksu_hook_setresuid(int orig_nr, const struct pt_regs *regs)
{
    uid_t old_uid = current_uid().val;
    long ret = ksu_invoke_orig_syscall(orig_nr, (const struct pt_regs *)regs);

    if (ret < 0)
        return ret;

    ksu_handle_setresuid(old_uid, current_uid().val);
    return ret;
}

#if defined(__aarch64__) && LINUX_VERSION_CODE < KERNEL_VERSION(4, 17, 0)
/*
 * 4.14 direct table-patch entry points (scattered-argument prototypes,
 * matching entry.S's blr convention). The GKI-oriented dispatcher +
 * sys_enter redirect + tp_marker machinery is not used on 4.14 -- these
 * entry points are patched straight into sys_call_table and every
 * process hits them; the hook logic itself decides what to do.
 *
 * A minimal pt_regs is built on the stack from the real arguments --
 * this is safe here (unlike the dispatcher thunk) because the values
 * come from the function arguments, not from clobbered registers.
 */
static inline void ksu_regs_from_args(struct pt_regs *regs, int nr,
                                      unsigned long x0, unsigned long x1, unsigned long x2,
                                      unsigned long x3, unsigned long x4, unsigned long x5)
{
    memset(regs, 0, sizeof(*regs));
    regs->regs[0] = x0;
    regs->regs[1] = x1;
    regs->regs[2] = x2;
    regs->regs[3] = x3;
    regs->regs[4] = x4;
    regs->regs[5] = x5;
    regs->syscallno = nr;
}

long ksu_sys_execve_414(const char __user *filename,
                        const char __user *const __user *argv,
                        const char __user *const __user *envp)
{
    struct pt_regs regs;
    ksu_regs_from_args(&regs, __NR_execve, (unsigned long)filename,
                       (unsigned long)argv, (unsigned long)envp, 0, 0, 0);
    return ksu_hook_execve_common(__NR_execve, &regs, false);
}

long ksu_sys_execveat_414(int fd, const char __user *filename,
                          const char __user *const __user *argv,
                          const char __user *const __user *envp, int flags)
{
    struct pt_regs regs;
    ksu_regs_from_args(&regs, __NR_execveat, (unsigned long)fd,
                       (unsigned long)filename, (unsigned long)argv,
                       (unsigned long)envp, (unsigned long)flags, 0);
    return ksu_hook_execve_common(__NR_execveat, &regs, true);
}

long ksu_sys_newfstatat_414(int dfd, const char __user *filename,
                            struct stat __user *statbuf, int flag)
{
    struct pt_regs regs;
    ksu_regs_from_args(&regs, __NR_newfstatat, (unsigned long)dfd,
                       (unsigned long)filename, (unsigned long)statbuf,
                       (unsigned long)flag, 0, 0);
    return ksu_hook_newfstatat(__NR_newfstatat, &regs);
}

long ksu_sys_faccessat_414(int dfd, const char __user *filename, int mode)
{
    struct pt_regs regs;
    ksu_regs_from_args(&regs, __NR_faccessat, (unsigned long)dfd,
                       (unsigned long)filename, (unsigned long)mode, 0, 0, 0);
    return ksu_hook_faccessat(__NR_faccessat, &regs);
}

long ksu_sys_setresuid_414(uid_t ruid, uid_t euid, uid_t suid)
{
    struct pt_regs regs;
    ksu_regs_from_args(&regs, __NR_setresuid, (unsigned long)ruid,
                       (unsigned long)euid, (unsigned long)suid, 0, 0, 0);
    return ksu_hook_setresuid(__NR_setresuid, &regs);
}

/* Patch the five hooked syscalls directly into the table (4.14 mode). */
void __init ksu_bridge_table_patch_init(void)
{
    ksu_syscall_table_hook(__NR_execve, (syscall_fn_t)ksu_sys_execve_414, NULL);
    ksu_syscall_table_hook(__NR_execveat, (syscall_fn_t)ksu_sys_execveat_414, NULL);
    ksu_syscall_table_hook(__NR_newfstatat, (syscall_fn_t)ksu_sys_newfstatat_414, NULL);
    ksu_syscall_table_hook(__NR_faccessat, (syscall_fn_t)ksu_sys_faccessat_414, NULL);
    ksu_syscall_table_hook(__NR_setresuid, (syscall_fn_t)ksu_sys_setresuid_414, NULL);
    pr_info("bridge: 4.14 direct table patches installed (execve/execveat/newfstatat/faccessat/setresuid)\n");
}
#endif
