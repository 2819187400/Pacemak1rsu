#include "ksu.h"
#include "linux/cred.h"
#include "linux/namei.h"
#include "util.h"
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/version.h>

#include "policy/allowlist.h"
#include "manager/apk_sign.h"
#include "manager/dynamic_manager.h"
#include "klog.h" // IWYU pragma: keep
#include "manager/manager_identity.h"
#include "manager/throne_tracker.h"

static DEFINE_MUTEX(throne_tracker_mutex);

uid_t ksu_manager_appid = KSU_INVALID_APPID;

#define DATA_PATH_LEN 384 // 384 is enough for /data/app/<package>/base.apk


#define SYSTEM_PACKAGES_LIST_PATH "/data/system/packages.list"

struct uid_data {
    struct list_head list;
    u32 uid;
    char package[KSU_MAX_PACKAGE_NAME];
};

static void crown_manager(const char *apk, struct list_head *uid_data)
{
    struct list_head *list = (struct list_head *)uid_data;
    struct uid_data *np;

    list_for_each_entry (np, list, list) {
        if (strncmp(np->package, KSU_PACKAGE_NAME, KSU_MAX_PACKAGE_NAME) == 0) {
            pr_info("Crowning manager: uid=%d\n", np->uid);
            ksu_set_manager_appid(np->uid);
            break;
        }
    }
}

struct data_path {
    char dirpath[DATA_PATH_LEN];
    int depth;
    struct list_head list;
};

struct my_dir_context {
    struct dir_context ctx;
    struct list_head *data_path_list;
    char *parent_dir;
    void *private_data;
    int depth;
};
// https://docs.kernel.org/filesystems/porting.html
// filldir_t (readdir callbacks) calling conventions have changed. Instead of returning 0 or -E... it returns bool now. false means "no more" (as -E... used to) and true - "keep going" (as 0 in old calling conventions). Rationale: callers never looked at specific -E... values anyway. -> iterate_shared() instances require no changes at all, all filldir_t ones in the tree converted.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
#define FILLDIR_RETURN_TYPE bool
#define FILLDIR_ACTOR_CONTINUE true
#define FILLDIR_ACTOR_STOP false
#else
#define FILLDIR_RETURN_TYPE int
#define FILLDIR_ACTOR_CONTINUE 0
#define FILLDIR_ACTOR_STOP -EINVAL
#endif

static FILLDIR_RETURN_TYPE my_actor(struct dir_context *ctx, const char *name, int namelen, loff_t off, u64 ino,
                                    unsigned int d_type)
{
    struct my_dir_context *my_ctx = container_of(ctx, struct my_dir_context, ctx);

    // we put the apk path we collected here
    char *candidate_path = (char *)my_ctx->private_data;

    char dirpath[DATA_PATH_LEN];

    if (!strncmp(name, "..", namelen) || !strncmp(name, ".", namelen))
        return FILLDIR_ACTOR_CONTINUE; // Skip "." and ".."

    if (d_type == DT_DIR && namelen >= 8 && !strncmp(name, "vmdl", 4) && !strncmp(name + namelen - 4, ".tmp", 4)) {
        pr_info("Skipping directory: %.*s\n", namelen, name);
        return FILLDIR_ACTOR_CONTINUE; // Skip staging package
    }

    if (snprintf(dirpath, DATA_PATH_LEN, "%s/%.*s", my_ctx->parent_dir, namelen, name) >= DATA_PATH_LEN) {
        pr_err("Path too long: %s/%.*s\n", my_ctx->parent_dir, namelen, name);
        return FILLDIR_ACTOR_CONTINUE;
    }

    if (d_type == DT_DIR && my_ctx->depth > 0) {
        struct data_path *data = kzalloc(sizeof(struct data_path), GFP_KERNEL);

        if (!data) {
            pr_err("Failed to allocate memory for %s\n", dirpath);
            return FILLDIR_ACTOR_CONTINUE;
        }

        strscpy(data->dirpath, dirpath, DATA_PATH_LEN);
        data->depth = my_ctx->depth - 1;
        list_add_tail(&data->list, my_ctx->data_path_list);

        return FILLDIR_ACTOR_CONTINUE;
    }

    // now put this on candidate_path
    if (d_type == DT_REG && namelen == 8 && !memcmp(name, "base.apk", 8)) {
        snprintf(candidate_path, DATA_PATH_LEN, "%s/%.*s", my_ctx->parent_dir, namelen, name);
        // As soon as we met the base.apk, we can stop
        return FILLDIR_ACTOR_STOP;
    }

    return FILLDIR_ACTOR_CONTINUE;
}

static void search_manager(const char *path, int depth, struct list_head *uid_data)
{
    int i;
    struct list_head data_path_list;
    INIT_LIST_HEAD(&data_path_list);
    unsigned long data_app_magic = 0;

    // First depth
    struct data_path data;
    strscpy(data.dirpath, path, DATA_PATH_LEN);
    data.depth = depth;
    list_add_tail(&data.list, &data_path_list);

    // we put the apk path we collected here
    char candidate_path[DATA_PATH_LEN];
    candidate_path[0] = 0;

    for (i = depth; i >= 0; i--) {
        struct data_path *pos, *n;

        list_for_each_entry_safe (pos, n, &data_path_list, list) {
            candidate_path[0] = 0;
            struct my_dir_context ctx = { .ctx.actor = my_actor,
                                          .data_path_list = &data_path_list,
                                          .parent_dir = pos->dirpath,
                                          .private_data = candidate_path,
                                          .depth = pos->depth };
            struct file *file = ksu_filp_open_nonotify(pos->dirpath, O_RDONLY | O_NOFOLLOW | O_NOATIME | O_DIRECTORY);
            if (IS_ERR(file)) {
                pr_err("Failed to open directory: %s, err: %ld\n", pos->dirpath, PTR_ERR(file));
                goto skip_iterate;
            }

            // grab magic on first folder, which is /data/app
            if (!data_app_magic) {
                if (file->f_inode->i_sb->s_magic) {
                    data_app_magic = file->f_inode->i_sb->s_magic;
                    pr_info("%s: dir: %s got magic! 0x%lx\n", __func__, pos->dirpath, data_app_magic);
                } else {
                    filp_close(file, NULL);
                    goto skip_iterate;
                }
            }

            if (file->f_inode->i_sb->s_magic != data_app_magic) {
                pr_info("%s: skip: %s magic: 0x%lx expected: 0x%lx\n", __func__, pos->dirpath,
                        file->f_inode->i_sb->s_magic, data_app_magic);
                filp_close(file, NULL);
                goto skip_iterate;
            }

            iterate_dir(file, &ctx.ctx);
            filp_close(file, NULL);

            if (!candidate_path[0])
                goto skip_iterate;

            struct apk_sign_match match = { .index = -1 };
            if (match_apk_signature(candidate_path, &match)) {
                char pkg[KSU_MAX_PACKAGE_NAME];
                if (get_pkg_from_apk_path(pkg, candidate_path) < 0)
                    goto skip_iterate;
                if (match.index == 0 && match.trusted && !strcmp(pkg, KSU_PACKAGE_NAME)) {
                    crown_manager(candidate_path, uid_data);
                } else {
#ifdef CONFIG_KSU_DYNAMIC_MANAGER
                    struct uid_data *app;
                    list_for_each_entry (app, uid_data, list) {
                        if (!strcmp(app->package, pkg)) {
                            ksu_dynamic_manager_note_scanned(app->uid, pkg, &match);
                            break;
                        }
                    }
#endif
                }
            }

        skip_iterate:
            list_del(&pos->list);
            if (pos != &data)
                kfree(pos);
        }
    }
}

static bool is_uid_exist(uid_t uid, char *package, void *data)
{
    struct list_head *list = (struct list_head *)data;
    struct uid_data *np;

    bool exist = false;
    list_for_each_entry (np, list, list) {
        if (np->uid == uid % PER_USER_RANGE && strncmp(np->package, package, KSU_MAX_PACKAGE_NAME) == 0) {
            exist = true;
            break;
        }
    }
    return exist;
}

static int read_package_uid(struct file *fp, loff_t line_start, loff_t line_end, struct list_head *uid_list)
{
    /* Keep the complete package and u32 UID, but ignore optional trailing fields. */
    char buf[KSU_MAX_PACKAGE_NAME + 12];
    size_t len = min_t(loff_t, line_end - line_start, sizeof(buf) - 1);
    ssize_t count = kernel_read(fp, buf, len, &line_start);
    struct uid_data *data;
    char *tmp, *package, *uid;
    u32 res;

    if (count < 0)
        return count;
    if ((size_t)count != len || !len || memchr(buf, '\0', len))
        return -EINVAL;
    buf[len] = '\0';

    tmp = buf;
    package = strsep(&tmp, " \t\r");
    while (tmp && (*tmp == ' ' || *tmp == '\t'))
        tmp++;
    uid = strsep(&tmp, " \t\r");
    /* A truncated prefix must contain the delimiter after the complete UID. */
    if (!package || !*package || strlen(package) >= KSU_MAX_PACKAGE_NAME || !uid || !*uid ||
        (!tmp && line_start < line_end) || kstrtou32(uid, 10, &res))
        return -EINVAL;

    data = kzalloc(sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;
    data->uid = res;
    strscpy(data->package, package, sizeof(data->package));
    list_add_tail(&data->list, uid_list);
    return 0;
}

void track_throne(bool prune_only)
{
    mutex_lock(&throne_tracker_mutex);
    const struct cred *old_cred = override_creds(ksu_cred);
    struct file *fp = filp_open(SYSTEM_PACKAGES_LIST_PATH, O_RDONLY, 0);
    if (IS_ERR(fp)) {
        pr_err("%s: open " SYSTEM_PACKAGES_LIST_PATH " failed: %ld\n", __func__, PTR_ERR(fp));
        goto out_revert_cred;
    }

    struct list_head uid_list;
    INIT_LIST_HEAD(&uid_list);

    char chr = 0;
    loff_t pos = 0;
    loff_t line_start = 0;
    bool parse_ok = true;
    for (;;) {
        ssize_t count = kernel_read(fp, &chr, sizeof(chr), &pos);
        if (count < 0) {
            parse_ok = false;
            break; // read error: the list is incomplete
        }
        if (count == 0 && pos == line_start)
            break; // normal EOF
        if (count > 0 && chr != '\n')
            continue;

        /* Do not read into the next record, including for two-field shared UIDs. */
        int rc = read_package_uid(fp, line_start, count > 0 ? pos - 1 : pos, &uid_list);
        if (rc) {
            pr_err("update_uid: invalid package record at offset %lld: %d\n", (long long)line_start, rc);
            parse_ok = false;
            break;
        }
        line_start = pos;
        if (count == 0)
            break; // the final record need not end with a newline
    }
    filp_close(fp, 0);

    /*
     * A truncated parse (allocation or format error) yields an incomplete uid
     * list. Acting on it could revoke the fixed manager by mistake and prune
     * allowlist entries for apps that simply were not parsed yet, so bail out
     * and leave the previous state untouched.
     */
    if (!parse_ok) {
        pr_warn("%s: incomplete packages list, skipping manager check and prune\n", __func__);
        goto out;
    }

    // now update uid list
    struct uid_data *np;
    struct uid_data *n;

    if (prune_only)
        goto prune;

    // Every package update can change a dynamic manager's signature or UID.
    ksu_invalidate_manager_uid();
#ifdef CONFIG_KSU_DYNAMIC_MANAGER
    ksu_dynamic_manager_begin_scan();
#endif
    search_manager("/data/app", 2, &uid_list);
#ifdef CONFIG_KSU_DYNAMIC_MANAGER
    ksu_dynamic_manager_end_scan();
#endif

prune:
    // then prune the allowlist
    ksu_prune_allowlist(is_uid_exist, &uid_list);
out:
    // free uid_list
    list_for_each_entry_safe (np, n, &uid_list, list) {
        list_del(&np->list);
        kfree(np);
    }
out_revert_cred:
    revert_creds(old_cred);
    mutex_unlock(&throne_tracker_mutex);
}

void __init ksu_throne_tracker_init()
{
    // nothing to do
}

void __exit ksu_throne_tracker_exit()
{
    // nothing to do
}

void track_throne_force(void)
{
    track_throne(false);
}
