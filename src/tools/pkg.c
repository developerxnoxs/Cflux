/**
 * src/tools/pkg.c - Flux package manager.
 *
 * A file-system-based package manager.  Packages are directories of .flx
 * files (and optional sub-directories) installed under a local `packages/`
 * folder in the project root.  A `flux.pkg` manifest (simple key=value)
 * records project metadata and dependencies.
 *
 * Supported commands:
 *   flux package init              – create flux.pkg in current directory
 *   flux package list              – list installed packages
 *   flux package install <path>    – install a local package directory
 *   flux package remove  <name>    – remove an installed package
 *   flux package info    <name>    – show package details
 *   flux package add     <name>    – stub (network install — future)
 */
#include "flux/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Manifest helpers
 * ---------------------------------------------------------------------- */
#define MANIFEST "flux.pkg"
#define PKG_DIR  "packages"

static int mkdir_p(const char *path) {
    /* Create directory (and parents) */
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755);
}

/* Read a key from the manifest.  Returns heap-allocated value or NULL. */
static char *manifest_get(const char *key) __attribute__((unused));
static char *manifest_get(const char *key) {
    FILE *f = fopen(MANIFEST, "r");
    if (!f) return NULL;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        /* Skip comments and blank lines */
        if (line[0] == '#' || line[0] == '\n') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        /* Trim key */
        char *k = line;
        while (*k == ' ') k++;
        char *ke = k + strlen(k) - 1;
        while (ke > k && (*ke == ' ' || *ke == '\r' || *ke == '\n')) *ke-- = '\0';
        if (strcmp(k, key) != 0) { *eq = '='; continue; }
        /* Trim value */
        char *v = eq + 1;
        while (*v == ' ') v++;
        char *ve = v + strlen(v) - 1;
        while (ve > v && (*ve == ' ' || *ve == '\r' || *ve == '\n')) *ve-- = '\0';
        fclose(f);
        return strdup(v);
    }
    fclose(f);
    return NULL;
}

static void manifest_set(const char *key, const char *value) {
    /* Read all lines, update or append */
    FILE *f = fopen(MANIFEST, "r");
    char lines[256][512];
    int  nlines = 0;
    bool found  = false;
    if (f) {
        while (nlines < 256 && fgets(lines[nlines], 512, f))
            nlines++;
        fclose(f);
    }
    for (int i = 0; i < nlines; i++) {
        if (lines[i][0] == '#' || lines[i][0] == '\n') continue;
        char *eq = strchr(lines[i], '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = lines[i];
        while (*k == ' ') k++;
        char *ke = k + strlen(k) - 1;
        while (ke > k && (*ke == ' ' || *ke == '\r' || *ke == '\n')) *ke-- = '\0';
        if (strcmp(k, key) == 0) {
            snprintf(lines[i], 512, "%s = %s\n", key, value);
            found = true;
        } else {
            /* Restore the '=' we clobbered */
            *eq = '=';
        }
    }
    if (!found && nlines < 256) {
        snprintf(lines[nlines++], 512, "%s = %s\n", key, value);
    }
    f = fopen(MANIFEST, "w");
    if (!f) { perror("flux package: cannot write flux.pkg"); return; }
    for (int i = 0; i < nlines; i++)
        fputs(lines[i], f);
    fclose(f);
}

/* -------------------------------------------------------------------------
 * Copy a directory recursively (src → dst)
 * ---------------------------------------------------------------------- */
static int copy_dir(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) != 0) {
        fprintf(stderr, "flux package: '%s' not found\n", src);
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "flux package: '%s' is not a directory\n", src);
        return -1;
    }

    if (mkdir_p(dst) != 0 && errno != EEXIST) {
        fprintf(stderr, "flux package: cannot create '%s': %s\n", dst, strerror(errno));
        return -1;
    }

    DIR *d = opendir(src);
    if (!d) { perror("flux package: opendir"); return -1; }

    int ok = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char srcpath[1024], dstpath[1024];
        snprintf(srcpath, sizeof(srcpath), "%s/%s", src, ent->d_name);
        snprintf(dstpath, sizeof(dstpath), "%s/%s", dst, ent->d_name);

        struct stat est;
        stat(srcpath, &est);
        if (S_ISDIR(est.st_mode)) {
            ok |= copy_dir(srcpath, dstpath);
        } else {
            /* Copy file */
            FILE *in  = fopen(srcpath, "rb");
            FILE *out = fopen(dstpath, "wb");
            if (!in || !out) {
                if (in)  fclose(in);
                if (out) fclose(out);
                fprintf(stderr, "flux package: cannot copy '%s'\n", ent->d_name);
                ok = -1;
                continue;
            }
            char buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
                fwrite(buf, 1, n, out);
            fclose(in);
            fclose(out);
        }
    }
    closedir(d);
    return ok;
}

/* Count .flx files in a directory */
static int count_flx_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        size_t len = strlen(ent->d_name);
        if (len > 4 && strcmp(ent->d_name + len - 4, ".flx") == 0) count++;
    }
    closedir(d);
    return count;
}

/* Remove a directory recursively */
static int rm_rf(const char *path) {
    DIR *d = opendir(path);
    if (!d) { return remove(path); }
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s", path, ent->d_name);
        rm_rf(sub);
    }
    closedir(d);
    return rmdir(path);
}

/* -------------------------------------------------------------------------
 * Commands
 * ---------------------------------------------------------------------- */

static int cmd_init(void) {
    struct stat st;
    if (stat(MANIFEST, &st) == 0) {
        printf("flux package: flux.pkg already exists\n");
        return 0;
    }

    /* Guess project name from cwd */
    char cwd[512] = "my-project";
    if (getcwd(cwd, sizeof(cwd))) {
        char *slash = strrchr(cwd, '/');
        if (slash) memmove(cwd, slash + 1, strlen(slash));
    }

    FILE *f = fopen(MANIFEST, "w");
    if (!f) { perror("flux package: cannot create flux.pkg"); return 1; }
    fprintf(f, "# Flux package manifest\n");
    fprintf(f, "# Generated by 'flux package init'\n");
    fprintf(f, "\n");
    fprintf(f, "name    = %s\n", cwd);
    fprintf(f, "version = 0.1.0\n");
    fprintf(f, "author  = \n");
    fprintf(f, "license = MIT\n");
    fprintf(f, "\n");
    fprintf(f, "# Dependencies (one per line: name = path_or_version)\n");
    fprintf(f, "# [dependencies]\n");
    fclose(f);

    /* Create packages/ directory */
    if (mkdir(PKG_DIR, 0755) != 0 && errno != EEXIST)
        fprintf(stderr, "flux package: warning: cannot create packages/ dir\n");

    printf("flux package: initialized project '%s'\n", cwd);
    printf("  Created: %s\n", MANIFEST);
    printf("  Created: %s/\n", PKG_DIR);
    return 0;
}

static int cmd_list(void) {
    /* Check packages/ directory exists */
    DIR *d = opendir(PKG_DIR);
    if (!d) {
        printf("flux package: no packages installed (packages/ directory not found)\n");
        printf("  Run 'flux package init' to initialize a project\n");
        return 0;
    }

    printf("Installed packages in %s/:\n\n", PKG_DIR);
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", PKG_DIR, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        int nfiles = count_flx_files(path);
        /* Read package manifest if it exists */
        char pkg_manifest[512];
        snprintf(pkg_manifest, sizeof(pkg_manifest) - 10, "%s", path);
        strncat(pkg_manifest, "/flux.pkg", 9);
        FILE *mf = fopen(pkg_manifest, "r");
        char version[64] = "(unknown)";
        if (mf) {
            char line[256];
            while (fgets(line, sizeof(line), mf)) {
                if (strncmp(line, "version", 7) == 0) {
                    char *eq = strchr(line, '=');
                    if (eq) {
                        char *v = eq + 1;
                        while (*v == ' ') v++;
                        strncpy(version, v, sizeof(version) - 1);
                        version[strcspn(version, "\r\n")] = '\0';
                    }
                    break;
                }
            }
            fclose(mf);
        }
        printf("  %-20s  v%-12s  %d file(s)\n", ent->d_name, version, nfiles);
        count++;
    }
    closedir(d);

    if (count == 0)
        printf("  (no packages installed)\n");
    printf("\nTotal: %d package(s)\n", count);
    return 0;
}

static int cmd_install(const char *src_path) {
    /* Resolve package name from path */
    const char *name = strrchr(src_path, '/');
    name = name ? name + 1 : src_path;

    /* Strip trailing slash */
    char name_buf[256];
    strncpy(name_buf, name, sizeof(name_buf) - 1);
    size_t nlen = strlen(name_buf);
    if (nlen > 0 && name_buf[nlen - 1] == '/') name_buf[--nlen] = '\0';

    /* Create packages/ if needed */
    if (mkdir(PKG_DIR, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "flux package: cannot create packages/ directory\n");
        return 1;
    }

    char dst_path[512];
    snprintf(dst_path, sizeof(dst_path), "%s/%s", PKG_DIR, name_buf);

    /* Check if already installed */
    struct stat st;
    if (stat(dst_path, &st) == 0) {
        printf("flux package: '%s' is already installed at %s\n", name_buf, dst_path);
        printf("  Use 'flux package remove %s' first to reinstall\n", name_buf);
        return 0;
    }

    printf("flux package: installing '%s' from '%s'...\n", name_buf, src_path);

    if (copy_dir(src_path, dst_path) != 0) {
        fprintf(stderr, "flux package: install failed\n");
        return 1;
    }

    int nfiles = count_flx_files(dst_path);
    printf("flux package: installed '%s' (%d .flx file(s))\n", name_buf, nfiles);
    printf("  Location: %s\n", dst_path);
    printf("  Import with: import %s\n", name_buf);

    /* Record in manifest if it exists */
    if (stat(MANIFEST, &st) == 0) {
        char dep_key[256];
        snprintf(dep_key, 255, "dep.%.250s", name_buf);
        manifest_set(dep_key, src_path);
    }

    return 0;
}

static int cmd_remove(const char *name) {
    char pkg_path[512];
    snprintf(pkg_path, sizeof(pkg_path), "%s/%s", PKG_DIR, name);

    struct stat st;
    if (stat(pkg_path, &st) != 0) {
        fprintf(stderr, "flux package: package '%s' is not installed\n", name);
        return 1;
    }

    printf("flux package: removing '%s'...\n", name);
    if (rm_rf(pkg_path) != 0) {
        fprintf(stderr, "flux package: failed to remove '%s': %s\n", name, strerror(errno));
        return 1;
    }
    printf("flux package: '%s' removed\n", name);
    return 0;
}

static int cmd_info(const char *name) {
    char pkg_path[512];
    snprintf(pkg_path, sizeof(pkg_path), "%s/%s", PKG_DIR, name);

    struct stat st;
    if (stat(pkg_path, &st) != 0) {
        fprintf(stderr, "flux package: package '%s' is not installed\n", name);
        printf("  Run 'flux package install <path>' to install it\n");
        return 1;
    }

    printf("Package: %s\n", name);
    printf("Location: %s\n", pkg_path);
    printf("Files:\n");

    DIR *d = opendir(pkg_path);
    if (d) {
        struct dirent *ent;
        int count = 0;
        while ((ent = readdir(d))) {
            if (ent->d_name[0] == '.') continue;
            printf("  %s\n", ent->d_name);
            count++;
        }
        closedir(d);
        printf("Total: %d item(s)\n", count);
    }

    /* Read package manifest */
    char mpath[512];
    snprintf(mpath, sizeof(mpath) - 10, "%s", pkg_path);
    strncat(mpath, "/flux.pkg", 9);
    FILE *mf = fopen(mpath, "r");
    if (mf) {
        printf("\nManifest (flux.pkg):\n");
        char line[256];
        while (fgets(line, sizeof(line), mf))
            printf("  %s", line);
        fclose(mf);
    }
    return 0;
}

static void cmd_help(void) {
    printf("flux package — Flux package manager\n\n");
    printf("Usage:\n");
    printf("  flux package init              Initialize a new project (creates flux.pkg)\n");
    printf("  flux package list              List installed packages\n");
    printf("  flux package install <path>    Install a package from a local directory\n");
    printf("  flux package remove  <name>    Remove an installed package\n");
    printf("  flux package info    <name>    Show package details\n");
    printf("  flux package add     <name>    [future] Install from package registry\n");
    printf("\nPackages are stored in the packages/ directory.\n");
    printf("Import them in Flux code with: import <package_name>\n");
}

/* -------------------------------------------------------------------------
 * Public entry point
 * ---------------------------------------------------------------------- */
int flux_package_cmd(int argc, char *argv[]) {
    if (argc < 1) {
        cmd_help();
        return 0;
    }

    const char *sub = argv[0];

    if (strcmp(sub, "init") == 0) {
        return cmd_init();

    } else if (strcmp(sub, "list") == 0 || strcmp(sub, "ls") == 0) {
        return cmd_list();

    } else if (strcmp(sub, "install") == 0 || strcmp(sub, "i") == 0) {
        if (argc < 2) {
            fprintf(stderr, "flux package install: expected <path>\n");
            return 1;
        }
        return cmd_install(argv[1]);

    } else if (strcmp(sub, "remove") == 0 || strcmp(sub, "rm") == 0) {
        if (argc < 2) {
            fprintf(stderr, "flux package remove: expected <name>\n");
            return 1;
        }
        return cmd_remove(argv[1]);

    } else if (strcmp(sub, "info") == 0) {
        if (argc < 2) {
            fprintf(stderr, "flux package info: expected <name>\n");
            return 1;
        }
        return cmd_info(argv[1]);

    } else if (strcmp(sub, "add") == 0) {
        /* Network registry — future feature */
        fprintf(stderr,
            "flux package add: network package registry is not yet implemented.\n"
            "  To install a local package: flux package install <path>\n");
        return 1;

    } else if (strcmp(sub, "help") == 0 || strcmp(sub, "--help") == 0) {
        cmd_help();
        return 0;

    } else {
        fprintf(stderr, "flux package: unknown subcommand '%s'\n", sub);
        cmd_help();
        return 1;
    }
}
