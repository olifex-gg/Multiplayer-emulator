/* town.c - town vault implementation. See town.h. */
#include "town.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <windows.h> /* MoveFileExA */
#include <direct.h>  /* _mkdir */
#include <io.h>      /* _commit, _fileno */
#define acnet_mkdir(path) _mkdir(path)
#define acnet_fsync(fp)   _commit(_fileno(fp))
/* POSIX rename() replaces an existing destination atomically; the Windows CRT
 * rename() fails if the destination exists, so use MoveFileEx with replace. */
static int acnet_replace(const char* from, const char* to) {
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}
#else
#include <unistd.h>
#define acnet_mkdir(path) mkdir((path), 0755)
#define acnet_fsync(fp)   fsync(fileno(fp))
static int acnet_replace(const char* from, const char* to) { return rename(from, to); }
#endif

#define TOWN_FILE      "town.gci"
#define TOWN_TMP       "town.gci.tmp"
#define RESIDENTS_FILE "residents.txt"
#define VERSION_FILE   "town.version"
#define TOWN_BACKUPS   3

static void path_join(char* out, size_t out_size, const char* dir, const char* file) {
    snprintf(out, out_size, "%s/%s", dir, file);
}

static int is_sep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

static int mkdir_p(const char* path) {
    char tmp[TOWN_DIR_MAX];
    size_t len = strlen(path);
    size_t i;
    if (len == 0 || len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    for (i = 1; i < len; i++) {
        if (is_sep(tmp[i])) {
            char sep = tmp[i];
            /* "D:" is a drive, not a directory; _mkdir on it fails without EEXIST. */
            if (i == 2 && tmp[1] == ':') continue;
            tmp[i] = '\0';
            if (acnet_mkdir(tmp) != 0 && errno != EEXIST) return -1;
            tmp[i] = sep;
        }
    }
    if (acnet_mkdir(tmp) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int read_file(const char* path, uint8_t* buf, size_t size) {
    FILE* fp = fopen(path, "rb");
    size_t got;
    if (!fp) return -1;
    got = fread(buf, 1, size, fp);
    fclose(fp);
    return got == size ? 0 : -1;
}

static int write_file_atomic(const char* dir, const char* final_name, const char* tmp_name,
                             const uint8_t* buf, size_t size) {
    char tmp_path[TOWN_DIR_MAX + 64];
    char final_path[TOWN_DIR_MAX + 64];
    FILE* fp;
    path_join(tmp_path, sizeof(tmp_path), dir, tmp_name);
    path_join(final_path, sizeof(final_path), dir, final_name);
    fp = fopen(tmp_path, "wb");
    if (!fp) return -1;
    if (fwrite(buf, 1, size, fp) != size) {
        fclose(fp);
        remove(tmp_path);
        return -1;
    }
    if (fflush(fp) != 0 || acnet_fsync(fp) != 0) {
        fclose(fp);
        remove(tmp_path);
        return -1;
    }
    fclose(fp);
    if (acnet_replace(tmp_path, final_path) != 0) {
        remove(tmp_path);
        return -1;
    }
    return 0;
}

static void rotate_backups(const char* dir) {
    char from[TOWN_DIR_MAX + 64];
    char to[TOWN_DIR_MAX + 64];
    int b;
    for (b = TOWN_BACKUPS; b >= 1; b--) {
        if (b == 1) {
            path_join(from, sizeof(from), dir, TOWN_FILE);
        } else {
            snprintf(from, sizeof(from), "%s/%s.bak%d", dir, TOWN_FILE, b - 1);
        }
        snprintf(to, sizeof(to), "%s/%s.bak%d", dir, TOWN_FILE, b);
        acnet_replace(from, to); /* missing files are fine */
    }
}

uint16_t town_checksum_be(const uint8_t* data, size_t size) {
    uint32_t sum = 0;
    size_t i;
    for (i = 0; i + 1 < size; i += 2) {
        sum += (uint16_t)(((uint16_t)data[i] << 8) | data[i + 1]);
    }
    return (uint16_t)((~(sum & 0xFFFFu) + 1u) & 0xFFFFu);
}

void town_fix_payload(uint8_t* payload) {
    uint8_t* main_save = payload + ACNET_SAVE_MAIN_OFFSET;
    uint8_t* chk = main_save + ACNET_SAVE_CHECKSUM_OFFSET;
    uint16_t sum;
    chk[0] = 0;
    chk[1] = 0;
    sum = town_checksum_be(main_save, ACNET_SAVE_T_SIZE);
    chk[0] = (uint8_t)(sum >> 8);
    chk[1] = (uint8_t)(sum & 0xFF);
    memcpy(payload + ACNET_SAVE_BACK_OFFSET, main_save, ACNET_SAVE_ALIGNED_SIZE);
}

static int load_residents(town_t* t) {
    char path[TOWN_DIR_MAX + 64];
    FILE* fp;
    char line[256];
    path_join(path, sizeof(path), t->dir, RESIDENTS_FILE);
    fp = fopen(path, "r");
    if (!fp) return 0; /* none yet */
    while (fgets(line, sizeof(line), fp)) {
        int slot, uploaded;
        char name[ACNET_NAME_LEN + 1];
        if (sscanf(line, "%d %d %16s", &slot, &uploaded, name) == 3 &&
            slot >= 0 && slot < ACNET_MAX_PLAYERS) {
            snprintf(t->slot_owner[slot], sizeof(t->slot_owner[slot]), "%s", name);
            t->slot_uploaded[slot] = uploaded ? 1 : 0;
        }
    }
    fclose(fp);
    return 0;
}

static int save_residents(town_t* t) {
    char buf[ACNET_MAX_PLAYERS * 64];
    size_t used = 0;
    int i;
    for (i = 0; i < ACNET_MAX_PLAYERS; i++) {
        if (t->slot_owner[i][0]) {
            used += (size_t)snprintf(buf + used, sizeof(buf) - used, "%d %d %s\n", i,
                                     t->slot_uploaded[i], t->slot_owner[i]);
        }
    }
    return write_file_atomic(t->dir, RESIDENTS_FILE, RESIDENTS_FILE ".tmp", (const uint8_t*)buf, used);
}

static int save_version(town_t* t) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%u\n", t->version);
    return write_file_atomic(t->dir, VERSION_FILE, VERSION_FILE ".tmp", (const uint8_t*)buf, (size_t)n);
}

int town_open(town_t* t, const char* data_root, const char* invite) {
    char path[TOWN_DIR_MAX + 64];
    struct stat st;
    memset(t, 0, sizeof(*t));
    snprintf(t->invite, sizeof(t->invite), "%s", invite);
    snprintf(t->dir, sizeof(t->dir), "%s/towns/%s", data_root, invite);
    if (mkdir_p(t->dir) != 0) return -1;

    path_join(path, sizeof(path), t->dir, TOWN_FILE);
    if (stat(path, &st) == 0) {
        if (st.st_size != (off_t)ACNET_TOWN_SIZE) {
            fprintf(stderr, "[town %s] %s has size %ld, expected %u; ignoring\n", invite, path,
                    (long)st.st_size, (unsigned)ACNET_TOWN_SIZE);
        } else {
            t->data = (uint8_t*)malloc(ACNET_TOWN_SIZE);
            if (!t->data) return -1;
            if (read_file(path, t->data, ACNET_TOWN_SIZE) != 0 ||
                !town_validate_blob(t->data, ACNET_TOWN_SIZE)) {
                fprintf(stderr, "[town %s] failed to read %s\n", invite, path);
                free(t->data);
                t->data = NULL;
            }
        }
    }
    path_join(path, sizeof(path), t->dir, VERSION_FILE);
    {
        FILE* fp = fopen(path, "r");
        if (fp) {
            unsigned v = 0;
            if (fscanf(fp, "%u", &v) == 1) t->version = v;
            fclose(fp);
        }
    }
    load_residents(t);
    return 0;
}

int town_exists_on_disk(const char* data_root, const char* invite) {
    char dir[TOWN_DIR_MAX];
    struct stat st;
    snprintf(dir, sizeof(dir), "%s/towns/%s", data_root, invite);
    return stat(dir, &st) == 0 && (st.st_mode & S_IFDIR) != 0;
}

void town_close(town_t* t) {
    free(t->data);
    t->data = NULL;
}

int town_block_has_character(const town_t* t, int i) {
    const uint8_t* id;
    if (!t->data || i < 0 || i >= ACNET_MAX_PLAYERS) return 0;
    id = t->data + ACNET_PRIVATE_OFFSET(i) + ACNET_PRIVATE_ID_OFFSET;
    return !(id[0] == 0xFF && id[1] == 0xFF && id[2] == 0xFF && id[3] == 0xFF);
}

int town_assign_slot(town_t* t, const char* name, int want_slot) {
    int i;
    for (i = 0; i < ACNET_MAX_PLAYERS; i++) {
        if (strcmp(t->slot_owner[i], name) == 0) return i;
    }
    if (want_slot >= 0 && want_slot < ACNET_MAX_PLAYERS && t->slot_owner[want_slot][0] == '\0') {
        i = want_slot;
    } else if (want_slot == ACNET_SLOT_ANY) {
        /* A new resident's game will put their character in the first block
         * of the downloaded town that has no character in it, so guess that
         * block; the claim that follows corrects the guess if it was wrong. */
        for (i = 0; i < ACNET_MAX_PLAYERS; i++) {
            if (t->slot_owner[i][0] == '\0' && !town_block_has_character(t, i)) break;
        }
        if (i == ACNET_MAX_PLAYERS) {
            /* Every empty block is spoken for; fall back to any unowned slot
             * (the claim will sort it out, or the game will report a full town). */
            for (i = 0; i < ACNET_MAX_PLAYERS; i++) {
                if (t->slot_owner[i][0] == '\0') break;
            }
            if (i == ACNET_MAX_PLAYERS) return -1;
        }
    } else {
        return -1;
    }
    snprintf(t->slot_owner[i], sizeof(t->slot_owner[i]), "%s", name);
    t->slot_uploaded[i] = 0;
    save_residents(t);
    return i;
}

int town_move_resident(town_t* t, const char* name, int from, int to, char* displaced) {
    uint8_t uploaded;
    displaced[0] = '\0';
    if (from < 0 || from >= ACNET_MAX_PLAYERS || to < 0 || to >= ACNET_MAX_PLAYERS) return 0;
    if (strcmp(t->slot_owner[from], name) != 0) return 0;
    if (from == to) return 1;
    if (t->slot_owner[to][0] && t->slot_uploaded[to]) return 0; /* a saved character lives there */

    /* Whoever held `to` never saved into it, so it costs them nothing to
     * take the slot the claimant is leaving. */
    snprintf(displaced, ACNET_NAME_LEN + 1, "%s", t->slot_owner[to]);
    uploaded = t->slot_uploaded[from];
    snprintf(t->slot_owner[to], sizeof(t->slot_owner[to]), "%s", name);
    t->slot_uploaded[to] = uploaded;
    snprintf(t->slot_owner[from], sizeof(t->slot_owner[from]), "%s", displaced);
    t->slot_uploaded[from] = 0;
    save_residents(t);
    return 1;
}

int town_set_land_cell(town_t* t, int fx, int fz, int utx, int utz, uint16_t item) {
    uint8_t* cell;
    if (!t->data) return 0;
    if (ACNET_LAND_ITEM_IS_RUNTIME(item)) return 0; /* a placeholder, never land; see protocol.h */
    if (fx < 0 || fx >= ACNET_FG_BLOCK_X || fz < 0 || fz >= ACNET_FG_BLOCK_Z) return 0;
    if (utx < 0 || utx >= ACNET_FG_UT || utz < 0 || utz >= ACNET_FG_UT) return 0;
    cell = t->data + ACNET_FG_CELL_ABS(fx, fz, utx, utz);
    cell[0] = (uint8_t)(item >> 8);
    cell[1] = (uint8_t)(item & 0xFF);
    return 1;
}

int town_flush(town_t* t) {
    if (!t->data) return 0;
    town_fix_payload(t->data + ACNET_GCI_HEADER_SIZE);
    return write_file_atomic(t->dir, TOWN_FILE, TOWN_TMP, t->data, ACNET_TOWN_SIZE) == 0;
}

int town_set_resident_blocks(town_t* t, int slot, const uint8_t* blob) {
    if (slot < 0 || slot >= ACNET_MAX_PLAYERS || !t->data) return 0;
    memcpy(t->data + ACNET_PRIVATE_OFFSET(slot), blob, ACNET_PRIVATE_SIZE);
    memcpy(t->data + ACNET_HOME_OFFSET(slot), blob + ACNET_PRIVATE_SIZE, ACNET_HOME_SIZE);
    if (!t->slot_uploaded[slot]) {
        t->slot_uploaded[slot] = 1;
        save_residents(t);
    }
    return town_flush(t);
}

int town_validate_blob(const uint8_t* blob, size_t len) {
    if (len != ACNET_TOWN_SIZE) return 0;
    /* CARDDir.gameName: "GAF" + region letter (GAFE for USA) */
    if (blob[0] != 'G' || blob[1] != 'A' || blob[2] != 'F') return 0;
    return 1;
}

uint32_t town_apply_upload(town_t* t, int uploader_slot, const uint8_t* blob) {
    uint8_t* next;
    int j;
    if (uploader_slot < 0 || uploader_slot >= ACNET_MAX_PLAYERS) return 0;
    if (!town_validate_blob(blob, ACNET_TOWN_SIZE)) return 0;

    next = (uint8_t*)malloc(ACNET_TOWN_SIZE);
    if (!next) return 0;
    memcpy(next, blob, ACNET_TOWN_SIZE);

    if (t->data) {
        /* Protect every other resident's own blocks: their client is the
         * only authority for those bytes. Slots nobody has uploaded for
         * yet are taken from the uploader (that is how a new resident is
         * created in a town someone else founded). */
        for (j = 0; j < ACNET_MAX_PLAYERS; j++) {
            if (j == uploader_slot) continue;
            if (t->slot_owner[j][0] == '\0' || !t->slot_uploaded[j]) continue;
            memcpy(next + ACNET_PRIVATE_OFFSET(j), t->data + ACNET_PRIVATE_OFFSET(j), ACNET_PRIVATE_SIZE);
            memcpy(next + ACNET_HOME_OFFSET(j), t->data + ACNET_HOME_OFFSET(j), ACNET_HOME_SIZE);
        }
    }
    town_fix_payload(next + ACNET_GCI_HEADER_SIZE);

    rotate_backups(t->dir);
    if (write_file_atomic(t->dir, TOWN_FILE, TOWN_TMP, next, ACNET_TOWN_SIZE) != 0) {
        free(next);
        return 0;
    }
    free(t->data);
    t->data = next;
    t->version++;
    t->slot_uploaded[uploader_slot] = 1;
    save_residents(t);
    save_version(t);
    return t->version;
}
