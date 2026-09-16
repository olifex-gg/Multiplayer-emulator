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
#define EMPTY_FILE     "empty.bin"   /* ACNET_RESIDENT_BLOB_SIZE: an unused character block + house block */
#define ROSTER_DIR     "roster"      /* <name>.bin, same layout: a resident who had to give up their seat */
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

static void fix_payload_layout(uint8_t* payload, size_t main_off, size_t save_t_size, size_t aligned_size) {
    uint8_t* main_save = payload + main_off;
    uint8_t* chk = main_save + ACNET_SAVE_CHECKSUM_OFFSET;
    uint16_t sum;
    chk[0] = 0;
    chk[1] = 0;
    sum = town_checksum_be(main_save, save_t_size);
    chk[0] = (uint8_t)(sum >> 8);
    chk[1] = (uint8_t)(sum & 0xFF);
    memcpy(main_save + aligned_size, main_save, aligned_size);
}

void town_fix_payload(uint8_t* payload) {
    fix_payload_layout(payload, ACNET_SAVE_MAIN_OFFSET, ACNET_SAVE_T_SIZE, ACNET_SAVE_ALIGNED_SIZE);
}

/* --- Two layouts: eight residents, and the four-resident files older
 * builds wrote (served as they are until a game saves them anew). --- */
int town_is_legacy(const town_t* t) {
    return t->data != NULL && t->data_len == ACNET_LEGACY_TOWN_SIZE;
}

/* Slots that have blocks in the stored town at all. */
static int town_slot_stored(const town_t* t, int slot) {
    return slot >= 0 && slot < (town_is_legacy(t) ? ACNET_LEGACY_MAX_PLAYERS : ACNET_MAX_PLAYERS);
}

static size_t town_private_off(const town_t* t, int slot) {
    if (town_is_legacy(t)) {
        return ACNET_GCI_HEADER_SIZE + ACNET_LEGACY_SAVE_MAIN_OFFSET + ACNET_LEGACY_PRIVATE_ARRAY_OFFSET +
               (size_t)slot * ACNET_PRIVATE_SIZE;
    }
    return ACNET_PRIVATE_OFFSET(slot);
}

static size_t town_home_off(const town_t* t, int slot) {
    if (town_is_legacy(t)) {
        return ACNET_GCI_HEADER_SIZE + ACNET_LEGACY_SAVE_MAIN_OFFSET + ACNET_LEGACY_HOME_ARRAY_OFFSET +
               (size_t)slot * ACNET_HOME_SIZE;
    }
    return ACNET_HOME_OFFSET(slot);
}

static size_t town_fg_abs(const town_t* t) {
    if (town_is_legacy(t)) {
        return ACNET_GCI_HEADER_SIZE + ACNET_LEGACY_SAVE_MAIN_OFFSET + ACNET_LEGACY_FG_OFFSET;
    }
    return ACNET_FG_ABS;
}

static void town_fix_stored(town_t* t) {
    if (town_is_legacy(t)) {
        fix_payload_layout(t->data + ACNET_GCI_HEADER_SIZE, ACNET_LEGACY_SAVE_MAIN_OFFSET, ACNET_LEGACY_SAVE_T_SIZE,
                           ACNET_LEGACY_SAVE_ALIGNED_SIZE);
    } else {
        town_fix_payload(t->data + ACNET_GCI_HEADER_SIZE);
    }
}

/* ------------------------------------------------------------ houses */

static size_t town_arrangement_abs(int slot) {
    return ACNET_SAVE_MAIN_ABS + ACNET_HOUSE_ARRANGEMENT_OFFSET + (size_t)slot;
}

int town_house_of_slot(const town_t* t, int slot) {
    if (slot < 0 || slot >= ACNET_MAX_PLAYERS) return slot;
    if (!t->data || town_is_legacy(t)) return slot;
    return t->data[town_arrangement_abs(slot)] & (ACNET_MAX_PLAYERS - 1);
}

static int town_house_has_owner(const town_t* t, int house) {
    const uint8_t* id;
    if (!t->data || !town_slot_stored(t, house)) return 0;
    id = t->data + town_home_off(t, house) + ACNET_HOME_OWNER_ID_OFFSET;
    return !(id[0] == 0xFF && id[1] == 0xFF && id[2] == 0xFF && id[3] == 0xFF);
}

/* ------------------------------------------------------------ seats */

static int load_empty_template(town_t* t) {
    char path[TOWN_DIR_MAX + 64];
    uint8_t* buf;
    if (t->empty_blocks) return 1;
    path_join(path, sizeof(path), t->dir, EMPTY_FILE);
    buf = (uint8_t*)malloc(ACNET_RESIDENT_BLOB_SIZE);
    if (!buf) return 0;
    if (read_file(path, buf, ACNET_RESIDENT_BLOB_SIZE) != 0) {
        free(buf);
        return 0;
    }
    t->empty_blocks = buf;
    return 1;
}

/* Remember what an unused character block and an unused house look like in
 * this town, from a slot nobody has used, so a seat can later be blanked for
 * a newcomer. Done once; the copy is kept on disk. */
static void capture_empty_template(town_t* t) {
    int i;
    if (t->empty_blocks || !t->data || town_is_legacy(t)) return;
    for (i = 0; i < ACNET_MAX_PLAYERS; i++) {
        int h = town_house_of_slot(t, i);
        if (!town_block_has_character(t, i) && !town_house_has_owner(t, h)) {
            uint8_t* buf = (uint8_t*)malloc(ACNET_RESIDENT_BLOB_SIZE);
            if (!buf) return;
            memcpy(buf, t->data + ACNET_PRIVATE_OFFSET(i), ACNET_PRIVATE_SIZE);
            memcpy(buf + ACNET_PRIVATE_SIZE, t->data + ACNET_HOME_OFFSET(h), ACNET_HOME_SIZE);
            t->empty_blocks = buf;
            write_file_atomic(t->dir, EMPTY_FILE, EMPTY_FILE ".tmp", buf, ACNET_RESIDENT_BLOB_SIZE);
            fprintf(stderr, "[town %s] kept a copy of an empty character block and house (slot %d, house %d) for seating\n",
                    t->invite, i, h);
            return;
        }
    }
}

int town_seats_available(const town_t* t) {
    return t->data != NULL && !town_is_legacy(t) && t->empty_blocks != NULL;
}

static void roster_path(const town_t* t, const char* name, char* out, size_t out_size) {
    snprintf(out, out_size, "%s/%s/%s.bin", t->dir, ROSTER_DIR, name);
}

/* Keep the resident in `slot`'s character and house so they get them back. */
static int roster_save(town_t* t, int slot) {
    char dir[TOWN_DIR_MAX + 64];
    char file[ACNET_NAME_LEN + 8];
    uint8_t buf[ACNET_RESIDENT_BLOB_SIZE];
    int h = town_house_of_slot(t, slot);
    snprintf(dir, sizeof(dir), "%s/%s", t->dir, ROSTER_DIR);
    if (mkdir_p(dir) != 0) return 0;
    memcpy(buf, t->data + ACNET_PRIVATE_OFFSET(slot), ACNET_PRIVATE_SIZE);
    memcpy(buf + ACNET_PRIVATE_SIZE, t->data + ACNET_HOME_OFFSET(h), ACNET_HOME_SIZE);
    snprintf(file, sizeof(file), "%s.bin", t->slot_owner[slot]);
    return write_file_atomic(dir, file, "roster.tmp", buf, sizeof(buf)) == 0;
}

static int roster_load(const town_t* t, const char* name, uint8_t* buf) {
    char path[TOWN_DIR_MAX + 64];
    roster_path(t, name, path, sizeof(path));
    return read_file(path, buf, ACNET_RESIDENT_BLOB_SIZE) == 0;
}

/* Every seat is owned: pick the one to free. Never someone who is online;
 * first choice a resident who never saved (nothing to keep), otherwise the
 * one seen longest ago (ties: the lowest slot). -1 when nobody can move. */
static int pick_seat_to_free(const town_t* t, town_online_fn online, void* ctx) {
    int i, best = -1, best_saved = 0;
    time_t best_seen = 0;
    for (i = 0; i < ACNET_MAX_PLAYERS; i++) {
        int saved = t->slot_uploaded[i] ? 1 : 0;
        if (online && online(ctx, t->slot_owner[i])) continue;
        if (best < 0 || saved < best_saved || (saved == best_saved && t->slot_last_seen[i] < best_seen)) {
            best = i;
            best_saved = saved;
            best_seen = t->slot_last_seen[i];
        }
    }
    return best;
}

/* ------------------------------------------------------------ residents.txt */

static int load_residents(town_t* t) {
    char path[TOWN_DIR_MAX + 64];
    FILE* fp;
    char line[256];
    path_join(path, sizeof(path), t->dir, RESIDENTS_FILE);
    fp = fopen(path, "r");
    if (!fp) return 0; /* none yet */
    while (fgets(line, sizeof(line), fp)) {
        int slot, uploaded, n;
        long seen = 0;
        char name[ACNET_NAME_LEN + 1];
        n = sscanf(line, "%d %d %16s %ld", &slot, &uploaded, name, &seen);
        if (n >= 3 && slot >= 0 && slot < ACNET_MAX_PLAYERS) {
            snprintf(t->slot_owner[slot], sizeof(t->slot_owner[slot]), "%s", name);
            t->slot_uploaded[slot] = uploaded ? 1 : 0;
            t->slot_last_seen[slot] = n >= 4 ? (time_t)seen : 0;
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
            used += (size_t)snprintf(buf + used, sizeof(buf) - used, "%d %d %s %ld\n", i,
                                     t->slot_uploaded[i], t->slot_owner[i], (long)t->slot_last_seen[i]);
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
        size_t len = (size_t)st.st_size;
        if (len != ACNET_TOWN_SIZE && len != ACNET_LEGACY_TOWN_SIZE) {
            fprintf(stderr, "[town %s] %s has size %ld, expected %u (or %u from the four-resident builds); ignoring\n",
                    invite, path, (long)st.st_size, (unsigned)ACNET_TOWN_SIZE, (unsigned)ACNET_LEGACY_TOWN_SIZE);
        } else {
            t->data = (uint8_t*)malloc(len);
            if (!t->data) return -1;
            if (read_file(path, t->data, len) != 0 || !town_validate_blob_any(t->data, len)) {
                fprintf(stderr, "[town %s] failed to read %s\n", invite, path);
                free(t->data);
                t->data = NULL;
            } else {
                t->data_len = len;
                if (len == ACNET_LEGACY_TOWN_SIZE) {
                    fprintf(stderr, "[town %s] four-resident town file; served as it is until a game saves it in the eight-resident layout\n",
                            invite);
                }
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
    if (!load_empty_template(t)) capture_empty_template(t);
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
    free(t->empty_blocks);
    t->empty_blocks = NULL;
}

int town_block_has_character(const town_t* t, int i) {
    const uint8_t* id;
    if (!t->data || !town_slot_stored(t, i)) return 0;
    id = t->data + town_private_off(t, i) + ACNET_PRIVATE_ID_OFFSET;
    return !(id[0] == 0xFF && id[1] == 0xFF && id[2] == 0xFF && id[3] == 0xFF);
}

void town_touch_resident(town_t* t, int slot) {
    if (slot < 0 || slot >= ACNET_MAX_PLAYERS || !t->slot_owner[slot][0]) return;
    t->slot_last_seen[slot] = time(NULL);
    save_residents(t);
}

/* All eight seats are owned: free one for `name`. See town.h. */
static int town_free_a_seat(town_t* t, const char* name, town_online_fn online, void* ctx, char* evicted) {
    uint8_t back[ACNET_RESIDENT_BLOB_SIZE];
    int i, h, victim_saved, returning;
    if (!town_seats_available(t)) return -1;
    i = pick_seat_to_free(t, online, ctx);
    if (i < 0) return -1;
    victim_saved = t->slot_uploaded[i];
    if (victim_saved && !roster_save(t, i)) {
        fprintf(stderr, "[town %s] could not keep %s's character and house in the roster; nobody moves\n",
                t->invite, t->slot_owner[i]);
        return -1;
    }
    snprintf(evicted, ACNET_NAME_LEN + 1, "%s", t->slot_owner[i]);
    h = town_house_of_slot(t, i);
    returning = roster_load(t, name, back);
    memcpy(t->data + ACNET_PRIVATE_OFFSET(i), returning ? back : t->empty_blocks, ACNET_PRIVATE_SIZE);
    memcpy(t->data + ACNET_HOME_OFFSET(h), returning ? back + ACNET_PRIVATE_SIZE : t->empty_blocks + ACNET_PRIVATE_SIZE,
           ACNET_HOME_SIZE);
    snprintf(t->slot_owner[i], sizeof(t->slot_owner[i]), "%s", name);
    t->slot_uploaded[i] = returning ? 1 : 0;
    t->slot_last_seen[i] = time(NULL);
    town_flush(t);
    save_residents(t);
    fprintf(stderr, "[town %s] %s moved out of seat %d (%s) so %s can move in%s\n", t->invite, evicted, i,
            victim_saved ? "their character and house are kept in the roster" : "they never saved", name,
            returning ? "; their own character and house are back from the roster" : "");
    return i;
}

int town_assign_slot(town_t* t, const char* name, int want_slot, town_online_fn online, void* ctx, char* evicted) {
    int i;
    evicted[0] = '\0';
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
            if (i == ACNET_MAX_PLAYERS) return town_free_a_seat(t, name, online, ctx, evicted);
        }
    } else {
        return -1;
    }
    snprintf(t->slot_owner[i], sizeof(t->slot_owner[i]), "%s", name);
    t->slot_uploaded[i] = 0;
    t->slot_last_seen[i] = time(NULL);
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
    cell = t->data + town_fg_abs(t) + (size_t)ACNET_FG_CELL_INDEX(fx, fz, utx, utz) * 2;
    cell[0] = (uint8_t)(item >> 8);
    cell[1] = (uint8_t)(item & 0xFF);
    return 1;
}

int town_flush(town_t* t) {
    if (!t->data) return 0;
    town_fix_stored(t);
    return write_file_atomic(t->dir, TOWN_FILE, TOWN_TMP, t->data, t->data_len) == 0;
}

int town_set_resident_blocks(town_t* t, int slot, int house, const uint8_t* blob) {
    int j;
    if (slot < 0 || slot >= ACNET_MAX_PLAYERS || !t->data) return 0;
    if (town_is_legacy(t)) {
        /* No place to put them until a game saves the town in the new
         * layout; the caller relays the blocks to the others regardless. */
        fprintf(stderr, "[town %s] blocks for slot %d not stored: the town is still a four-resident file\n",
                t->invite, slot);
        return 0;
    }
    if (house < 0 || house >= ACNET_MAX_PLAYERS) house = town_house_of_slot(t, slot);
    memcpy(t->data + ACNET_PRIVATE_OFFSET(slot), blob, ACNET_PRIVATE_SIZE);
    /* The house half goes where the pusher says their house is -- unless
     * another resident who has saved lives there, which would be a bug in
     * the pusher's game; then their house is left alone. */
    for (j = 0; j < ACNET_MAX_PLAYERS; j++) {
        if (j != slot && t->slot_owner[j][0] && t->slot_uploaded[j] && town_house_of_slot(t, j) == house) break;
    }
    if (j < ACNET_MAX_PLAYERS) {
        fprintf(stderr, "[town %s] slot %d pushed house %d, which is %s's; keeping only the character\n", t->invite,
                slot, house, t->slot_owner[j]);
    } else {
        memcpy(t->data + ACNET_HOME_OFFSET(house), blob + ACNET_PRIVATE_SIZE, ACNET_HOME_SIZE);
        t->data[town_arrangement_abs(slot)] = (uint8_t)house;
    }
    if (!t->slot_uploaded[slot]) {
        t->slot_uploaded[slot] = 1;
        save_residents(t);
    }
    capture_empty_template(t);
    return town_flush(t);
}

int town_validate_blob(const uint8_t* blob, size_t len) {
    if (len != ACNET_TOWN_SIZE) return 0;
    /* CARDDir.gameName: "GAF" + region letter (GAFE for USA) */
    if (blob[0] != 'G' || blob[1] != 'A' || blob[2] != 'F') return 0;
    return 1;
}

int town_validate_blob_any(const uint8_t* blob, size_t len) {
    if (len != ACNET_TOWN_SIZE && len != ACNET_LEGACY_TOWN_SIZE) return 0;
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
            int h;
            if (j == uploader_slot) continue;
            if (t->slot_owner[j][0] == '\0' || !t->slot_uploaded[j]) continue;
            if (!town_slot_stored(t, j)) continue; /* a four-resident file has no such block */
            /* Their house is the block the stored arrangement names, and
             * that arrangement entry is theirs too (an uploader whose game
             * has not heard where a new resident moved in must not undo it). */
            h = town_house_of_slot(t, j);
            memcpy(next + ACNET_PRIVATE_OFFSET(j), t->data + town_private_off(t, j), ACNET_PRIVATE_SIZE);
            memcpy(next + ACNET_HOME_OFFSET(h), t->data + town_home_off(t, h), ACNET_HOME_SIZE);
            if (!town_is_legacy(t)) next[town_arrangement_abs(j)] = t->data[town_arrangement_abs(j)];
        }
    }
    town_fix_payload(next + ACNET_GCI_HEADER_SIZE);

    rotate_backups(t->dir);
    if (write_file_atomic(t->dir, TOWN_FILE, TOWN_TMP, next, ACNET_TOWN_SIZE) != 0) {
        free(next);
        return 0;
    }
    if (town_is_legacy(t)) {
        fprintf(stderr, "[town %s] now stored in the eight-resident layout\n", t->invite);
    }
    free(t->data);
    t->data = next;
    t->data_len = ACNET_TOWN_SIZE;
    t->version++;
    t->slot_uploaded[uploader_slot] = 1;
    save_residents(t);
    save_version(t);
    capture_empty_template(t);
    return t->version;
}
