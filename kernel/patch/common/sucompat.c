/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 
 * Copyright (C) 2023 bmax121. All Rights Reserved.
 */

#include <linux/list.h>
#include <ktypes.h>
#include <compiler.h>
#include <stdbool.h>
#include <linux/syscall.h>
#include <ksyms.h>
#include <hook.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <stdbool.h>
#include <asm/current.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <uapi/scdefs.h>
#include <kputils.h>
#include <linux/ptrace.h>
#include <accctl.h>
#include <linux/string.h>
#include <linux/err.h>
#include <uapi/asm-generic/errno.h>
#include <taskob.h>
#include <linux/kernel.h>
#include <linux/rculist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <syscall.h>
#include <predata.h>
#include <predata.h>
#include <kconfig.h>
#include <linux/vmalloc.h>
#include <sucompat.h>
#include <symbol.h>
#include <uapi/linux/limits.h>
#include <predata.h>
#include <kstorage.h>

const char sh_path[] = SH_PATH;
const char default_su_path[] = SU_PATH;

#ifdef ANDROID
const char legacy_su_path[] = LEGACY_SU_PATH;
const char apd_path[] = APD_PATH;
#endif

static const char *current_su_path = 0;

struct su_profile
{
    uid_t uid;
    uid_t to_uid;
    char scontext[SUPERCALL_SCONTEXT_LEN];
};

int is_su_allow_uid(uid_t uid)
{
    rcu_read_lock();
    const struct kstorage *ks = get_kstorage(KSTORAGE_SU_LIST_GROUP, uid);
    struct su_profile *profile = (struct su_profile *)ks->data;
    int rc = profile != 0;
    rcu_read_unlock();
    return rc;
}
KP_EXPORT_SYMBOL(is_su_allow_uid);

int su_add_allow_uid(uid_t uid, uid_t to_uid, const char *scontext)
{
    if (!scontext) scontext = "";
    struct su_profile profile = { uid, to_uid, scontext };
    int rc = write_kstorage(KSTORAGE_SU_LIST_GROUP, uid, &profile, 0, sizeof(struct su_profile), false);
    logkfd("uid: %d, to_uid: %d, sctx: %s, rc: %d\n", uid, to_uid, scontext, rc);
    return rc;
}
KP_EXPORT_SYMBOL(su_add_allow_uid);

int su_remove_allow_uid(uid_t uid)
{
    return remove_kstorage(KSTORAGE_SU_LIST_GROUP, uid);
}
KP_EXPORT_SYMBOL(su_remove_allow_uid);

int su_allow_uid_nums()
{
    return kstorage_group_size(KSTORAGE_SU_LIST_GROUP);
}
KP_EXPORT_SYMBOL(su_allow_uid_nums);

static int allow_uids_cb(struct kstorage *kstorage, void *udata)
{
    struct
    {
        int is_user;
        uid_t *out_uids;
        int out_num;
    } *up = (typeof(up))udata;

    if (up->is_user) {
        int cplen = compat_copy_to_user(up->out_uids + num, &uid, sizeof(uid));
        logkfd("uid: %d\n", uid);
        if (cplen <= 0) {
            logkfd("compat_copy_to_user error: %d", cplen);
            rc = cplen;
            goto out;
        }
    } else {
        out_uids[num] = uid;
    }

    num++;

    return 0;
}

int su_allow_uids(int is_user, uid_t *out_uids, int out_num)
{
    int rc = 0;
    struct
    {
        int iu;
        uid_t *up;
        int un;
    } udata = { is_user, out_uids, out_num };
    on_each_kstorage_elem(KSTORAGE_SU_LIST_GROUP, allow_uids_cb, &udata);
    return rc;
}
KP_EXPORT_SYMBOL(su_allow_uids);

int su_allow_uid_profile(int is_user, uid_t uid, struct su_profile *profile)
{
    int rc = -ENOENT;
    rcu_read_lock();
    struct allow_uid *pos;
    list_for_each_entry(pos, &allow_uid_list, list)
    {
        if (pos->profile.uid != uid) continue;
        if (is_user) {
            int cplen = compat_copy_to_user(profile, &pos->profile, sizeof(struct su_profile));
            logkfd("profile: %d %d %s\n", uid, pos->profile.to_uid, pos->profile.scontext);
            if (cplen <= 0) {
                logkfd("compat_copy_to_user error: %d", cplen);
                rc = cplen;
                goto out;
            }
        } else {
            memcpy(profile, &pos->profile, sizeof(struct su_profile));
        }
        rc = 0;
        goto out;
    }
out:
    rcu_read_unlock();
    return rc;
}
KP_EXPORT_SYMBOL(su_allow_uid_profile);

// no free, no lock
int su_reset_path(const char *path)
{
    if (!path) return -EINVAL;
    if (IS_ERR(path)) return PTR_ERR(path);
    current_su_path = path;
    logkfd("%s\n", current_su_path);
    dsb(ish);
    return 0;
}
KP_EXPORT_SYMBOL(su_reset_path);

const char *su_get_path()
{
    if (!current_su_path) current_su_path = default_su_path;
    return current_su_path;
}
KP_EXPORT_SYMBOL(su_get_path);

static void handle_before_execve(char **__user u_filename_p, char **__user uargv, void *udata)
{
    char __user *ufilename = *u_filename_p;
    char filename[SU_PATH_MAX_LEN];
    int flen = compat_strncpy_from_user(filename, ufilename, sizeof(filename));
    if (flen <= 0) return;

    if (!strcmp(current_su_path, filename)) {
        uid_t uid = current_uid();
        if (!is_su_allow_uid(uid)) return;
        struct su_profile profile = profile_su_allow_uid(uid);

        uid_t to_uid = profile.to_uid;
        const char *sctx = profile.scontext;
        commit_su(to_uid, sctx);

#ifdef ANDROID
        struct file *filp = filp_open(apd_path, O_RDONLY, 0);
        if (!filp || IS_ERR(filp)) {
#endif
            void *uptr = copy_to_user_stack(sh_path, sizeof(sh_path));
            if (uptr && !IS_ERR(uptr)) {
                *u_filename_p = (char *__user)uptr;
            }
            logkfi("call su uid: %d, to_uid: %d, sctx: %s, uptr: %llx\n", uid, to_uid, sctx, uptr);
#ifdef ANDROID
        } else {
            filp_close(filp, 0);

            // command
            uint64_t sp = 0;
            sp = current_user_stack_pointer();
            sp -= sizeof(apd_path);
            sp &= 0xFFFFFFFFFFFFFFF8;
            int cplen = compat_copy_to_user((void *)sp, apd_path, sizeof(apd_path));
            if (cplen > 0) {
                *u_filename_p = (char *)sp;
            }

            // argv
            int argv_cplen = 0;
            if (strcmp(legacy_su_path, filename)) {
                if (argv_cplen <= 0) {
                    sp = sp ?: current_user_stack_pointer();
                    sp -= sizeof(legacy_su_path);
                    sp &= 0xFFFFFFFFFFFFFFF8;
                    argv_cplen = compat_copy_to_user((void *)sp, legacy_su_path, sizeof(legacy_su_path));
                    if (argv_cplen > 0) {
                        int rc = set_user_arg_ptr(0, *uargv, 0, sp);
                        if (rc < 0) { // todo: modify entire argv
                            logkfi("call apd argv error, uid: %d, to_uid: %d, sctx: %s, rc: %d\n", uid, to_uid, sctx,
                                   rc);
                        }
                    }
                }
            }
            logkfi("call apd uid: %d, to_uid: %d, sctx: %s, cplen: %d, %d\n", uid, to_uid, sctx, cplen, argv_cplen);
        }
#endif // ANDROID
    } else if (!strcmp(SUPERCMD, filename)) {
        handle_supercmd(u_filename_p, uargv);
        return;
    }
}

// https://elixir.bootlin.com/linux/v6.1/source/fs/exec.c#L2107
// COMPAT_SYSCALL_DEFINE3(execve, const char __user *, filename,
// 	const compat_uptr_t __user *, argv,
// 	const compat_uptr_t __user *, envp)

// https://elixir.bootlin.com/linux/v6.1/source/fs/exec.c#L2087
// SYSCALL_DEFINE3(execve, const char __user *, filename, const char __user *const __user *, argv,
//                 const char __user *const __user *, envp)

static void before_execve(hook_fargs3_t *args, void *udata)
{
    void *arg0p = syscall_argn_p(args, 0);
    void *arg1p = syscall_argn_p(args, 1);
    handle_before_execve((char **)arg0p, (char **)arg1p, udata);
}

// https://elixir.bootlin.com/linux/v6.1/source/fs/exec.c#L2114
// COMPAT_SYSCALL_DEFINE5(execveat, int, fd,
// 		       const char __user *, filename,
// 		       const compat_uptr_t __user *, argv,
// 		       const compat_uptr_t __user *, envp,
// 		       int,  flags)

// https://elixir.bootlin.com/linux/v6.1/source/fs/exec.c#L2095
// SYSCALL_DEFINE5(execveat, int, fd, const char __user *, filename, const char __user *const __user *, argv,
//                 const char __user *const __user *, envp, int, flags)
__maybe_unused static void before_execveat(hook_fargs5_t *args, void *udata)
{
    void *arg1p = syscall_argn_p(args, 1);
    void *arg2p = syscall_argn_p(args, 2);
    handle_before_execve((char **)arg1p, (char **)arg2p, udata);
}

// https://elixir.bootlin.com/linux/v6.1/source/fs/stat.c#L431
// SYSCALL_DEFINE4(newfstatat, int, dfd, const char __user *, filename,
// 		struct stat __user *, statbuf, int, flag)

// https://elixir.bootlin.com/linux/v6.1/source/fs/open.c#L492
// SYSCALL_DEFINE3(faccessat, int, dfd, const char __user *, filename, int, mode)

// https://elixir.bootlin.com/linux/v6.1/source/fs/open.c#L497
// SYSCALL_DEFINE4(faccessat2, int, dfd, const char __user *, filename, int, mode, int, flags)

// https://elixir.bootlin.com/linux/v6.1/source/fs/stat.c#L661
// SYSCALL_DEFINE5(statx,
// 		int, dfd, const char __user *, filename, unsigned, flags,
// 		unsigned int, mask,
// 		struct statx __user *, buffer)
static void su_handler_arg1_ufilename_before(hook_fargs6_t *args, void *udata)
{
    uid_t uid = current_uid();
    if (!is_su_allow_uid(uid)) return;

    char __user **u_filename_p = (char __user **)syscall_argn_p(args, 1);

    char filename[SU_PATH_MAX_LEN];
    int flen = compat_strncpy_from_user(filename, *u_filename_p, sizeof(filename));
    if (flen <= 0) return;

    if (!strcmp(current_su_path, filename)) {
        void *uptr = copy_to_user_stack(sh_path, sizeof(sh_path));
        if (uptr && !IS_ERR(uptr)) {
            *u_filename_p = uptr;
        } else {
            logkfi("su uid: %d, cp stack error: %d\n", uid, uptr);
        }
    }
}

int su_compat_init()
{
    current_su_path = default_su_path;

#ifdef ANDROID
    // default shell
    if (!all_allow_sctx[0]) strcpy(all_allow_sctx, ALL_ALLOW_SCONTEXT_MAGISK);
    su_add_allow_uid(2000, 0, all_allow_sctx, 1);
    su_add_allow_uid(0, 0, all_allow_sctx, 1);
#endif

    hook_err_t rc = HOOK_NO_ERR;

    uint8_t su_config = patch_config->patch_su_config;
    bool enable = su_config & PATCH_CONFIG_SU_ENABLE;
    bool wrap = su_config & PATCH_CONFIG_SU_HOOK_NO_WRAP;
    log_boot("su config, enable: %d, wrap: %d\n");

    if (!enable) return;

    rc = hook_syscalln(__NR_execve, 3, before_execve, 0, (void *)0);
    log_boot("hook __NR_execve rc: %d\n", rc);

    rc = hook_syscalln(__NR3264_fstatat, 4, su_handler_arg1_ufilename_before, 0, (void *)0);
    log_boot("hook __NR3264_fstatat rc: %d\n", rc);

    rc = hook_syscalln(__NR_faccessat, 3, su_handler_arg1_ufilename_before, 0, (void *)0);
    log_boot("hook __NR_faccessat rc: %d\n", rc);

    // __NR_execve 11
    rc = hook_compat_syscalln(11, 3, before_execve, 0, (void *)1);
    log_boot("hook 32 __NR_execve rc: %d\n", rc);

    // __NR_fstatat64 327
    rc = hook_compat_syscalln(327, 4, su_handler_arg1_ufilename_before, 0, (void *)0);
    log_boot("hook 32 __NR_fstatat64 rc: %d\n", rc);

    //  __NR_faccessat 334
    rc = hook_compat_syscalln(334, 3, su_handler_arg1_ufilename_before, 0, (void *)0);
    log_boot("hook 32 __NR_faccessat rc: %d\n", rc);

    return 0;
}