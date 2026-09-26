#ifndef __KSU_H_KSU_SYSCALL_HOOK
#define __KSU_H_KSU_SYSCALL_HOOK
#include <linux/version.h>
#include <asm/syscall.h>

#if defined(__x86_64__)
typedef sys_call_ptr_t syscall_fn_t;
#elif defined(__aarch64__)
/* 4.14 has no syscall_fn_t in asm/syscall.h; upstream assumed >=5.x */
typedef long (*syscall_fn_t)(const struct pt_regs *regs);
#endif

extern syscall_fn_t *ksu_syscall_table;

// Dispatcher slot number in syscall table
extern int ksu_dispatcher_nr;

// Syscall hook handler type.
// orig_nr: the original syscall number before redirection
// regs: the original pt_regs from userspace
// Handler is responsible for calling ksu_syscall_table[orig_nr](regs) if needed.
typedef long (*ksu_syscall_hook_fn)(int orig_nr, const struct pt_regs *regs);

/* Saved-original lookup for direct table patches (avoid self-recursion). */
syscall_fn_t ksu_syscall_table_get_orig(int nr);

/*
 * Invoking the original syscall from a hook.
 * 4.17+ arm64 syscall wrappers take struct pt_regs*; pre-4.17 arm64
 * entries are called by entry.S with blr and user args in x0..x7
 * (scattered prototype). Calling a scattered prototype with a pt_regs
 * pointer corrupts the arguments, so pre-4.17 we invoke with the
 * scattered prototype taken from regs[0..5]. When the entry is hooked
 * (direct table patch), call the saved original instead of the hook.
 */
#if defined(__aarch64__) && LINUX_VERSION_CODE < KERNEL_VERSION(4, 17, 0)
static inline long ksu_invoke_orig_syscall(int nr, const struct pt_regs *regs)
{
    syscall_fn_t saved = ksu_syscall_table_get_orig(nr);
    long (*fn)(unsigned long, unsigned long, unsigned long,
               unsigned long, unsigned long, unsigned long) =
        (long (*)(unsigned long, unsigned long, unsigned long,
                  unsigned long, unsigned long, unsigned long))(saved ? saved : ksu_syscall_table[nr]);
    return fn(regs->regs[0], regs->regs[1], regs->regs[2],
              regs->regs[3], regs->regs[4], regs->regs[5]);
}
#else
static inline long ksu_invoke_orig_syscall(int nr, const struct pt_regs *regs)
{
    return ksu_syscall_table[nr](regs);
}
#endif

// --- Dispatcher-based hook API (register/unregister) ---
// Register a handler into the dispatcher's routing table for syscall @nr.
// When a marked process invokes syscall @nr, the sys_enter tracepoint redirects
// it to the unified dispatcher, which looks up @fn by @nr and calls it.
// Does NOT modify the syscall table itself — the dispatcher slot is shared.
// Returns 0 on success, -EEXIST if already registered, -EINVAL if nr invalid.
int ksu_register_syscall_hook(int nr, ksu_syscall_hook_fn fn);

// Remove a handler from the dispatcher's routing table for syscall @nr.
// The syscall table is not touched — only the dispatcher stops routing @nr.
void ksu_unregister_syscall_hook(int nr);

// Check if a handler is registered in the dispatcher for syscall @nr.
bool ksu_has_syscall_hook(int nr);

// --- Direct syscall table patching API (hook/unhook) ---
// Directly overwrite syscall_table[@nr] with @fn using fixmap + stop_machine.
// Saves the original handler to *@old (if non-NULL) and records the entry
// for restoration at module exit. Use this for boot-time hooks that replace
// a real syscall entry (e.g. ksud hooking __NR_execve/__NR_read/__NR_fstat).
void ksu_syscall_table_hook(int nr, syscall_fn_t fn, syscall_fn_t *old);

// Restore syscall_table[@nr] to its original value recorded by
// ksu_syscall_table_hook(), and remove the entry from the tracking list.
// Use this to cleanly undo a direct hook when it is no longer needed
// (e.g. ksud unhooking __NR_read after init.rc injection is done).
void ksu_syscall_table_unhook(int nr);

void ksu_syscall_hook_init(void);
void ksu_syscall_hook_exit(void);

#endif
