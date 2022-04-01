/**
 * @file ds_lyb.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief internal LYB datastore plugin
 *
 * @copyright
 * Copyright (c) 2021 Deutsche Telekom AG.
 * Copyright (c) 2021 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE

#include "plugins_datastore.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <libyang/libyang.h>

#include "compat.h"
#include "common_lyb.h"
#include "sysrepo.h"

#define srpds_name "LYB DS file"  /**< plugin name */

static int srpds_lyb_load(const struct lys_module *mod, sr_datastore_t ds, const char **xpaths, uint32_t xpath_count,
        struct lyd_node **mod_data);

static int srpds_lyb_candidate_modified(const struct lys_module *mod, int *modified);

static int srpds_lyb_access_get(const struct lys_module *mod, sr_datastore_t ds, char **owner, char **group,
        mode_t *perm);

static int
srpds_lyb_store_(const struct lys_module *mod, sr_datastore_t ds, const struct lyd_node *mod_data, const char *owner,
        const char *group, mode_t perm, int make_backup)
{
    int rc = SR_ERR_OK;
    struct stat st;
    char *path = NULL, *bck_path = NULL;
    int fd = -1, backup = 0;

    /* get path */
    if ((rc = srlyb_get_path(srpds_name, mod->name, ds, &path))) {
        goto cleanup;
    }

    if (make_backup && (ds == SR_DS_STARTUP)) {
        /* get original file perms */
        if (stat(path, &st) == -1) {
            if (errno == EACCES) {
                SRPLG_LOG_ERR(srpds_name, "Learning \"%s\" permissions failed.", mod->name);
                rc = SR_ERR_UNAUTHORIZED;
            } else {
                SRPLG_LOG_ERR(srpds_name, "Stat of \"%s\" failed (%s).", path, strerror(errno));
                rc = SR_ERR_SYS;
            }
            goto cleanup;
        }

        /* generate the backup path */
        if (asprintf(&bck_path, "%s%s", path, SRLYB_FILE_BACKUP_SUFFIX) == -1) {
            SRPLG_LOG_ERR(srpds_name, "Memory allocation failed.");
            rc = SR_ERR_NO_MEMORY;
            goto cleanup;
        }

        /* create backup file with same owner/group/perm */
        if ((fd = srlyb_open(bck_path, O_WRONLY | O_CREAT | O_EXCL, st.st_mode)) == -1) {
            SRPLG_LOG_ERR(srpds_name, "Opening \"%s\" failed (%s).", bck_path, strerror(errno));
            rc = SR_ERR_SYS;
            goto cleanup;
        }
        backup = 1;
        if (fchown(fd, st.st_uid, st.st_gid) == -1) {
            SRPLG_LOG_ERR(srpds_name, "Changing owner of \"%s\" failed (%s).", bck_path, strerror(errno));
            if ((errno == EACCES) || (errno == EPERM)) {
                rc = SR_ERR_UNAUTHORIZED;
            } else {
                rc = SR_ERR_INTERNAL;
            }
            goto cleanup;
        }

        /* close */
        close(fd);
        fd = -1;

        /* back up any existing file */
        if ((rc = srlyb_cp_path(srpds_name, bck_path, path))) {
            goto cleanup;
        }
    }

    /* open the file */
    if ((fd = srlyb_open(path, O_WRONLY | (perm ? O_CREAT : 0), perm)) == -1) {
        rc = srlyb_open_error(srpds_name, path);
        goto cleanup;
    }

    if (owner || group) {
        /* change the owner of the file */
        if ((rc = srlyb_chmodown(srpds_name, path, owner, group, 0))) {
            goto cleanup;
        }
    }

    /* print data */
    if (lyd_print_fd(fd, mod_data, LYD_LYB, LYD_PRINT_WITHSIBLINGS)) {
        srplyb_log_err_ly(srpds_name, LYD_CTX(mod_data));
        SRPLG_LOG_ERR(srpds_name, "Failed to store data into \"%s\".", path);
        rc = SR_ERR_INTERNAL;
        goto cleanup;
    }

cleanup:
    /* delete the backup file */
    if (backup && (unlink(bck_path) == -1)) {
        SRPLG_LOG_ERR(srpds_name, "Failed to remove backup \"%s\" (%s).", bck_path, strerror(errno));
        if (!rc) {
            rc = SR_ERR_SYS;
        }
    }

    if (fd > -1) {
        close(fd);
    }
    free(path);
    free(bck_path);
    return rc;
}

static int
srpds_lyb_init(const struct lys_module *mod, sr_datastore_t ds, const char *owner, const char *group, mode_t perm)
{
    int rc = SR_ERR_OK;
    struct lyd_node *root = NULL;
    char *path = NULL;

    assert(perm);

    if (ds == SR_DS_CANDIDATE) {
        /* no need to initialize anything */
        return rc;
    } else if ((ds == SR_DS_OPERATIONAL) || (ds == SR_DS_RUNNING)) {
        /* create empty file */
        if ((rc = srlyb_get_path(srpds_name, mod->name, ds, &path))) {
            goto cleanup;
        }
        assert(!srlyb_file_exists(srpds_name, path));
        if ((rc = srpds_lyb_store_(mod, ds, NULL, owner, group, perm, 0))) {
            goto cleanup;
        }
        goto cleanup;
    }

    /* startup data dir */
    if ((rc = srlyb_get_startup_dir(srpds_name, &path))) {
        return rc;
    }
    if (!srlyb_file_exists(srpds_name, path) && (rc = srlyb_mkpath(srpds_name, path, SRLYB_DIR_PERM))) {
        goto cleanup;
    }

    /* check whether the file does not exist */
    free(path);
    if ((rc = srlyb_get_path(srpds_name, mod->name, ds, &path))) {
        goto cleanup;
    }
    if (srlyb_file_exists(srpds_name, path)) {
        SRPLG_LOG_ERR(srpds_name, "File \"%s\" already exists.", path);
        rc = SR_ERR_EXISTS;
        goto cleanup;
    }

    /* get default values */
    if (lyd_new_implicit_module(&root, mod, LYD_IMPLICIT_NO_STATE, NULL)) {
        srplyb_log_err_ly(srpds_name, mod->ctx);
        rc = SR_ERR_LY;
        goto cleanup;
    }

    /* print them into the startup file */
    if ((rc = srpds_lyb_store_(mod, ds, root, owner, group, perm, 0))) {
        goto cleanup;
    }

cleanup:
    free(path);
    lyd_free_all(root);
    return rc;
}

static int
srpds_lyb_destroy(const struct lys_module *mod, sr_datastore_t ds)
{
    int rc = SR_ERR_OK;
    char *path;

    if ((rc = srlyb_get_path(srpds_name, mod->name, ds, &path))) {
        return rc;
    }

    if ((unlink(path) == -1) && (errno != ENOENT) &&
            ((ds == SR_DS_STARTUP) || (ds == SR_DS_RUNNING) || (ds == SR_DS_OPERATIONAL))) {
        /* startup is persistent and must always exist, running and operational should always be created */
        SRPLG_LOG_WRN("Failed to unlink \"%s\" (%s).", path, strerror(errno));
    }
    free(path);

    return rc;
}

static int
srpds_lyb_store(const struct lys_module *mod, sr_datastore_t ds, const struct lyd_node *mod_data)
{
    mode_t perm = 0;
    int rc, modified;

    if (ds == SR_DS_CANDIDATE) {
        /* check whether candidate is modified (the file exists) */
        if ((rc = srpds_lyb_candidate_modified(mod, &modified))) {
            return rc;
        }

        if (!modified) {
            /* get running permissions to use for the candidate */
            if ((rc = srpds_lyb_access_get(mod, SR_DS_RUNNING, NULL, NULL, &perm))) {
                return rc;
            }
        }
    }

    return srpds_lyb_store_(mod, ds, mod_data, NULL, NULL, perm, 1);
}

static void
srpds_lyb_recover(const struct lys_module *mod, sr_datastore_t ds)
{
    char *path = NULL, *bck_path = NULL;
    struct lyd_node *mod_data = NULL;

    /* get path */
    if (srlyb_get_path(srpds_name, mod->name, ds, &path)) {
        goto cleanup;
    }

    /* check whether the file is valid */
    if (!srpds_lyb_load(mod, ds, NULL, 0, &mod_data)) {
        /* data are valid, nothing to do */
        goto cleanup;
    }

    if (ds == SR_DS_STARTUP) {
        /* there must be a backup file for startup data */
        SRPLG_LOG_WRN("Recovering \"%s\" startup data from a backup.", mod->name);

        /* generate the backup path */
        if (asprintf(&bck_path, "%s%s", path, SRLYB_FILE_BACKUP_SUFFIX) == -1) {
            SRPLG_LOG_ERR(srpds_name, "Memory allocation failed.");
            goto cleanup;
        }

        /* restore the backup data, avoid changing permissions of the target file */
        if (srlyb_cp_path(srpds_name, path, bck_path)) {
            goto cleanup;
        }

        /* remove the backup file */
        if (unlink(bck_path) == -1) {
            SRPLG_LOG_ERR(srpds_name, "Unlinking \"%s\" failed (%s).", bck_path, strerror(errno));
            goto cleanup;
        }
    } else if (ds == SR_DS_RUNNING) {
        /* perform startup->running data file copy */
        SRPLG_LOG_WRN("Recovering \"%s\" running data from the startup data.", mod->name);

        /* generate the startup data file path */
        if (srlyb_get_path(srpds_name, mod->name, SR_DS_STARTUP, &bck_path)) {
            goto cleanup;
        }

        /* copy startup data to running */
        if (srlyb_cp_path(srpds_name, path, bck_path)) {
            goto cleanup;
        }
    } else {
        /* there is not much to do but remove the corrupted file */
        SRPLG_LOG_WRN("Recovering \"%s\" %s data by removing the corrupted data file.", mod->name, srlyb_ds2str(ds));

        if (unlink(path) == -1) {
            SRPLG_LOG_ERR(srpds_name, "Unlinking \"%s\" failed (%s).", path, strerror(errno));
            goto cleanup;
        }
    }

cleanup:
    free(path);
    free(bck_path);
    lyd_free_all(mod_data);
}

static int
srpds_lyb_load(const struct lys_module *mod, sr_datastore_t ds, const char **UNUSED(xpaths), uint32_t UNUSED(xpath_count),
        struct lyd_node **mod_data)
{
    int rc = SR_ERR_OK, fd = -1;
    char *path = NULL;
    uint32_t parse_opts;

    *mod_data = NULL;

    /* prepare correct file path */
    if ((rc = srlyb_get_path(srpds_name, mod->name, ds, &path))) {
        goto cleanup;
    }

    /* open fd */
    fd = srlyb_open(path, O_RDONLY, 0);
    if (fd == -1) {
        if (errno == ENOENT) {
            if (ds == SR_DS_CANDIDATE) {
                /* no candidate exists */
                rc = SR_ERR_NOT_FOUND;
                goto cleanup;
            } else if (!strcmp(mod->name, "sysrepo")) {
                /* fine for the internal module */
                goto cleanup;
            } else if (ds == SR_DS_OPERATIONAL) {
                /* it may not exist */
                goto cleanup;
            }
        }

        rc = srlyb_open_error(srpds_name, path);
        goto cleanup;
    }

    /* set parse options */
    if (!strcmp(mod->name, "sysrepo")) {
        /* internal module, accept an update */
        parse_opts = LYD_PARSE_LYB_MOD_UPDATE | LYD_PARSE_ONLY | LYD_PARSE_STRICT | LYD_PARSE_ORDERED;
    } else {
        parse_opts = LYD_PARSE_ONLY | LYD_PARSE_STRICT | LYD_PARSE_ORDERED;
    }

    /* load the data */
    if (lyd_parse_data_fd(mod->ctx, fd, LYD_LYB, parse_opts, 0, mod_data)) {
        srplyb_log_err_ly(srpds_name, mod->ctx);
        rc = SR_ERR_LY;
        goto cleanup;
    }

cleanup:
    if (fd > -1) {
        close(fd);
    }
    free(path);
    return rc;
}

static int
srpds_lyb_copy(const struct lys_module *mod, sr_datastore_t trg_ds, sr_datastore_t src_ds)
{
    int rc = SR_ERR_OK;
    char *src_path = NULL, *trg_path = NULL;

    /* target path */
    if ((rc = srlyb_get_path(srpds_name, mod->name, trg_ds, &trg_path))) {
        goto cleanup;
    }

    /* source path */
    if ((rc = srlyb_get_path(srpds_name, mod->name, src_ds, &src_path))) {
        goto cleanup;
    }

    /* copy contents of source to target */
    if ((rc = srlyb_cp_path(srpds_name, trg_path, src_path))) {
        goto cleanup;
    }

cleanup:
    free(src_path);
    free(trg_path);
    return rc;
}

static int
srpds_lyb_update_differ(const struct lys_module *old_mod, const struct lyd_node *old_mod_data,
        const struct lys_module *new_mod, const struct lyd_node *new_mod_data, int *differ)
{
    const struct lys_module *mod_iter, *mod_iter2;
    uint32_t idx = 0;
    LY_ARRAY_COUNT_TYPE u;
    LY_ERR lyrc;

    if (old_mod) {
        /* first check whether any modules augmenting/deviating this module were not removed or updated, in that
         * case LYB metadata have changed and the data must be stored whether they differ or not */
        while ((mod_iter = ly_ctx_get_module_iter(old_mod->ctx, &idx))) {
            if (!mod_iter->implemented) {
                /* we need data of only implemented modules */
                continue;
            }

            mod_iter2 = ly_ctx_get_module_implemented(new_mod->ctx, mod_iter->name);
            if (mod_iter2 && (mod_iter->revision == mod_iter2->revision)) {
                /* module was not removed nor updated, irrelevant */
                continue;
            }

            /* deviates */
            LY_ARRAY_FOR(old_mod->deviated_by, u) {
                if (old_mod->deviated_by[u] == mod_iter) {
                    *differ = 1;
                    return SR_ERR_OK;
                }
            }

            /* augments */
            LY_ARRAY_FOR(old_mod->augmented_by, u) {
                if (old_mod->augmented_by[u] == mod_iter) {
                    *differ = 1;
                    return SR_ERR_OK;
                }
            }
        }
    }

    /* check for data difference */
    lyrc = lyd_compare_siblings(new_mod_data, old_mod_data, LYD_COMPARE_FULL_RECURSION | LYD_COMPARE_DEFAULTS);
    if (lyrc && (lyrc != LY_ENOT)) {
        srplyb_log_err_ly(srpds_name, new_mod->ctx);
        return SR_ERR_LY;
    }

    if (lyrc == LY_ENOT) {
        *differ = 1;
    } else {
        *differ = 0;
    }
    return SR_ERR_OK;
}

static int
srpds_lyb_candidate_modified(const struct lys_module *mod, int *modified)
{
    int rc = SR_ERR_OK;
    char *path = NULL;

    /* candidate DS file cannot exist */
    if ((rc = srlyb_get_path(srpds_name, mod->name, SR_DS_CANDIDATE, &path))) {
        goto cleanup;
    }

    if (srlyb_file_exists(srpds_name, path)) {
        /* file exists so it is modified */
        *modified = 1;
    } else {
        *modified = 0;
    }

cleanup:
    free(path);
    return rc;
}

static int
srpds_lyb_candidate_reset(const struct lys_module *mod)
{
    int rc = SR_ERR_OK;
    char *path;

    if ((rc = srlyb_get_path(srpds_name, mod->name, SR_DS_CANDIDATE, &path))) {
        return rc;
    }

    if ((unlink(path) == -1) && (errno != ENOENT)) {
        SRPLG_LOG_WRN("Failed to unlink \"%s\" (%s).", path, strerror(errno));
    }
    free(path);

    return rc;
}

static int
srpds_lyb_access_set(const struct lys_module *mod, sr_datastore_t ds, const char *owner, const char *group, mode_t perm)
{
    int rc = SR_ERR_OK;
    char *path = NULL;

    assert(mod && (owner || group || perm));

    /* get path */
    if ((rc = srlyb_get_path(srpds_name, mod->name, ds, &path))) {
        return rc;
    }

    if ((ds == SR_DS_CANDIDATE) && !srlyb_file_exists(srpds_name, path)) {
        /* nothing to do if the file does not exist */
        goto cleanup;
    }

    /* update file permissions and owner */
    rc = srlyb_chmodown(srpds_name, path, owner, group, perm);

cleanup:
    free(path);
    return rc;
}

static int
srpds_lyb_access_get(const struct lys_module *mod, sr_datastore_t ds, char **owner, char **group, mode_t *perm)
{
    int rc = SR_ERR_OK, r;
    struct stat st;
    char *path;

    if (owner) {
        *owner = NULL;
    }
    if (group) {
        *group = NULL;
    }

    /* path */
    if ((rc = srlyb_get_path(srpds_name, mod->name, ds, &path))) {
        return rc;
    }

    /* stat */
    r = stat(path, &st);
    if (r == -1) {
        if (errno == EACCES) {
            SRPLG_LOG_ERR(srpds_name, "Learning \"%s\" permissions failed.", mod->name);
            rc = SR_ERR_UNAUTHORIZED;
        } else {
            SRPLG_LOG_ERR(srpds_name, "Stat of \"%s\" failed (%s).", path, strerror(errno));
            rc = SR_ERR_SYS;
        }
        free(path);
        return rc;
    }
    free(path);

    /* get owner */
    if (owner && (rc = srlyb_get_pwd(srpds_name, &st.st_uid, owner))) {
        goto error;
    }

    /* get group */
    if (group && (rc = srlyb_get_grp(srpds_name, &st.st_gid, group))) {
        goto error;
    }

    /* get perms */
    if (perm) {
        *perm = st.st_mode & 0007777;
    }

    return rc;

error:
    if (owner) {
        free(*owner);
    }
    if (group) {
        free(*group);
    }
    return rc;
}

static int
srpds_lyb_access_check(const struct lys_module *mod, sr_datastore_t ds, int *read, int *write)
{
    int rc = SR_ERR_OK;
    char *path;

retry:
    /* path */
    if ((rc = srlyb_get_path(srpds_name, mod->name, ds, &path))) {
        return rc;
    }

    /* check read */
    if (read) {
        if (eaccess(path, R_OK) == -1) {
            if ((ds == SR_DS_CANDIDATE) && (errno == ENOENT)) {
                /* special case of non-existing candidate */
                ds = SR_DS_RUNNING;
                free(path);
                goto retry;
            } else if ((ds == SR_DS_OPERATIONAL) && (errno == ENOENT)) {
                /* non-existing operational is fine */
                *read = 1;
            } else if (errno == EACCES) {
                *read = 0;
            } else {
                SRPLG_LOG_ERR(srpds_name, "Eaccess of \"%s\" failed (%s).", path, strerror(errno));
                rc = SR_ERR_SYS;
                goto cleanup;
            }
        } else {
            *read = 1;
        }
    }

    /* check write */
    if (write) {
        if (eaccess(path, W_OK) == -1) {
            if ((ds == SR_DS_CANDIDATE) && (errno == ENOENT)) {
                /* special case of non-existing candidate */
                ds = SR_DS_RUNNING;
                free(path);
                goto retry;
            } else if ((ds == SR_DS_OPERATIONAL) && (errno == ENOENT)) {
                /* non-existing operational is fine */
                *write = 1;
            } else if (errno == EACCES) {
                *write = 0;
            } else {
                SRPLG_LOG_ERR(srpds_name, "Eaccess of \"%s\" failed (%s).", path, strerror(errno));
                rc = SR_ERR_SYS;
                goto cleanup;
            }
        } else {
            *write = 1;
        }
    }

cleanup:
    free(path);
    return rc;
}

const struct srplg_ds_s srpds_lyb = {
    .name = srpds_name,
    .init_cb = srpds_lyb_init,
    .destroy_cb = srpds_lyb_destroy,
    .store_cb = srpds_lyb_store,
    .recover_cb = srpds_lyb_recover,
    .load_cb = srpds_lyb_load,
    .copy_cb = srpds_lyb_copy,
    .update_differ_cb = srpds_lyb_update_differ,
    .candidate_modified_cb = srpds_lyb_candidate_modified,
    .candidate_reset_cb = srpds_lyb_candidate_reset,
    .access_set_cb = srpds_lyb_access_set,
    .access_get_cb = srpds_lyb_access_get,
    .access_check_cb = srpds_lyb_access_check,
};