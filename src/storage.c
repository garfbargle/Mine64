#include "cart.h"
#include "ff/ff.h"

#include "storage.h"
#include "blocks.h"
#include "graphics.h"
#include "items.h"
#include "player.h"
#include "trees.h"
#include "world.h"
#include "home.h"
#include "mods.h"
#include "day_cycle.h"

static FATFS fs;

// "MI64"
#define MAGIC_NUM 0x4D493634
#define LEGACY_HEADER_PAGE_SIZE 128
#define LEGACY_COOP_HEADER_SIZE 256
#define LEGACY_MAX_PLAYERS 2
#define BUFFER_LEN 512
#define SAVE_VERSION 12

/*
 * v11 appends the world seed, and changes nothing else.
 *
 * Terrain is a pure function of the coordinate and the seed, which is what
 * lets an unmodified column be dropped and regenerated instead of saved.  But
 * no version before this one recorded the seed, so a loaded world generated
 * everything past the saved extent from whatever seed the session happened to
 * be carrying -- the title screen's last preview, or nothing at all on a cold
 * boot straight into Load.  The saved 112x112 extent was never affected,
 * because it arrives as blocks; everything the player streamed in by walking
 * out of it was a different world on every load.  Biomes are what makes that
 * impossible to miss: a desert becomes a forest between one session and the
 * next.
 *
 * The seed goes after the player array, in space the fixed 512-byte header
 * page already zero-filled, so every v10 offset is untouched and one struct
 * serves both.  Only the checksum and the seed restore are version-gated.
 */
#define SAVE_VERSION_SEED 11

/*
 * v12 appends the world mod mask, on exactly the same terms as v11's seed.
 *
 * The mods are chosen once, on the setup card, and the generator reads them
 * for every column it produces -- including the columns regenerated when the
 * player walks back out past the saved extent, years of sessions later.  A
 * world whose mask was not recorded would grow classic terrain onto the edge
 * of an archipelago, which is the same failure the seed had and just as
 * visible.
 *
 * Also after the player array, in space the fixed 512-byte header page
 * already zero-filled, so every v10 and v11 offset is untouched and one
 * struct still serves all three.  A pre-v12 file reads as mask zero, which
 * setWorldMods turns into the classic default -- the only world those
 * versions could have made.
 */
#define SAVE_VERSION_MODS 12
/* The first version using the four-player Header layout.  v10 onwards share
   it, so the load path keys on this rather than on SAVE_VERSION. */
#define SAVE_VERSION_HEADER 10

typedef struct {
  u32 magic_num;
  u32 version;
  u32 file_num;
  u32 save_count;
  Vector3 position;
  float pitch;
  float yaw;
  u32 held_block;
} LegacyHeader;

typedef struct {
  Vector3 position;
  float pitch;
  float yaw;
  u8 selected_hotbar_slot;
  u8 reserved[3];
  ItemStack inventory[INVENTORY_SIZE];
  ItemStack crafting[CRAFTING_SIZE];
  ItemStack carried_item;
} SavedPlayer;

typedef struct {
  u32 magic_num;
  u32 version;
  u32 file_num;
  u32 save_count;
  u32 player_count;
  u32 checksum;
  u32 world_ticks;
  SavedPlayer player[MAX_PLAYERS];
  /* Appended by v11; see SAVE_VERSION_SEED.  A v10 file's zero-filled tail
     reads as 0 here, which is why the version -- not the value -- decides
     whether it means anything. */
  u32 world_seed;
  /* Appended by v12; see SAVE_VERSION_MODS.  Same rule. */
  u32 world_mods;
} Header;

/* Version 8 is the current four-player header before the world clock.  Keep
   it separate so its packed world data and checksum remain readable. */
typedef struct {
  u32 magic_num;
  u32 version;
  u32 file_num;
  u32 save_count;
  u32 player_count;
  u32 checksum;
  SavedPlayer player[MAX_PLAYERS];
} HeaderV8;

/* Versions 4-7 stored exactly two SavedPlayer records in a 256-byte header.
   Keep this ABI frozen so expanding the live player array never makes an old
   save appear malformed or shifts the packed world data. */
typedef struct {
  u32 magic_num;
  u32 version;
  u32 file_num;
  u32 save_count;
  u32 player_count;
  u32 checksum;
  SavedPlayer player[LEGACY_MAX_PLAYERS];
} HeaderV7;

/* Save v5 introduced tree records.  Its compact layout pre-dates the falling
   animation fields in v6, so v5 records are checksummed then reconstructed
   from the saved world rather than being interpreted as current records. */
typedef struct {
  u8 x;
  u8 z;
  u8 base_y;
  u8 trunk_mask;
  u8 canopy_y;
  u8 falling;
  u8 debris_cursor;
  u8 leaf_mask[13];
} TreeRecordV5;

/* v3 stored aggregate item counts rather than exact inventory slots. */
typedef struct {
  Vector3 position;
  float pitch;
  float yaw;
  u32 held_block;
  u32 wood_count;
  u32 planks_count;
  u32 crafting_table_count;
  u32 stick_count;
  u32 sword_count;
  u32 pickaxe_count;
} SavedPlayerV3;

typedef struct {
  u32 magic_num;
  u32 version;
  u32 file_num;
  u32 save_count;
  u32 player_count;
  SavedPlayerV3 player[LEGACY_MAX_PLAYERS];
} HeaderV3;

/* v2 saved just the log stack.  Keep its layout separate so existing worlds
   can be loaded before their new crafting materials are added. */
typedef struct {
  Vector3 position;
  float pitch;
  float yaw;
  u32 held_block;
  u32 wood_count;
} SavedPlayerV2;

typedef struct {
  u32 magic_num;
  u32 version;
  u32 file_num;
  u32 save_count;
  u32 player_count;
  SavedPlayerV2 player[LEGACY_MAX_PLAYERS];
} HeaderV2;

/* The first co-op save format did not contain an inventory count.  Its
   separate definition keeps old save files readable after the v2 extension. */
typedef struct {
  Vector3 position;
  float pitch;
  float yaw;
  u32 held_block;
} SavedPlayerV1;

typedef struct {
  u32 magic_num;
  u32 version;
  u32 file_num;
  u32 save_count;
  u32 player_count;
  SavedPlayerV1 player[LEGACY_MAX_PLAYERS];
} HeaderV1;

static u32 save_count = 0;

typedef char TreeRecordV5MustBeCompact[
  sizeof(TreeRecordV5) == 20 ? 1 : -1
];

typedef char HeaderMustFitBuffer[
  sizeof(Header) <= BUFFER_LEN ? 1 : -1
];

typedef char LegacyHeaderMustRemainOnePage[
  sizeof(HeaderV7) == LEGACY_COOP_HEADER_SIZE ? 1 : -1
];

static u8 file_buffer[BUFFER_LEN] __attribute__((aligned(16)));
static u32 cursor_pos = 0;
static UINT buffer_bytes_read = 0;

/*
 * Everything a sliced load has to remember between callbacks.
 *
 * The header pointers below all alias file_buffer, and the very first payload
 * read overwrites it -- so every header field the tail of the load still
 * needs is copied in here while it is still readable.  That is not merely a
 * convenience for slicing: reading header->world_ticks after the block loop
 * was folding whatever bytes the file's last page happened to contain into
 * the checksum, which failed verification on every v10 and v11 save, sent
 * each load through backup recovery and then through a full blocking world
 * generation, and left the player with a fresh random world in place of the
 * one they saved.
 */
#define LOAD_STAGE_IDLE 0
#define LOAD_STAGE_BLOCKS 1
#define LOAD_STAGE_TREES 2

static struct {
  FIL file;
  u8 stage;
  u8 read_ok;
  u8 full_inventory_saved;
  u8 tree_records_saved;
  u8 legacy_tree_records_saved;
  u32 version;
  u32 saved_world_ticks;
  u32 saved_world_seed;
  u32 saved_world_mods;
  u32 expected_checksum;
  u32 checksum;
  int block_x;      /* next x-slab of packed blocks to read */
  u32 tree_byte;    /* next byte of the trailing tree payload */
} load_job;

/* The reserved bytes of player zero record the chunk dimensions. */
static void saveWorldDimensions(Header *header) {
  /* Low five bits hold dimensions; the high bits are gameplay state.  The
     original dimensions fit in five bits, so this remains byte-compatible
     with every v10 header that wrote zeroes above them. */
  header->player[0].reserved[0] =
    (header->player[0].reserved[0] & 0xE0) | (CHUNKS_X & 0x1F);
  header->player[0].reserved[1] =
    (header->player[0].reserved[1] & 0xF0) | (CHUNKS_Y & 0x0F);
  header->player[0].reserved[2] =
    (header->player[0].reserved[2] & 0xE0) | (CHUNKS_Z & 0x1F);
}

static u8 savedWorldDimensionsMatch(SavedPlayer *saved_players) {
  return (saved_players[0].reserved[0] & 0x1F) == CHUNKS_X &&
    (saved_players[0].reserved[1] & 0x0F) == CHUNKS_Y &&
    (saved_players[0].reserved[2] & 0x1F) == CHUNKS_Z;
}

static u32 checksumByte(u32 checksum, u8 value) {
  return (checksum ^ value) * 16777619UL;
}

static u32 checksumPlayers(SavedPlayer *players_to_save, u32 player_count,
    u8 stored_player_slots) {
  u8 *bytes = (u8 *) players_to_save;
  u32 checksum = checksumByte(2166136261UL, player_count);
  u32 index;

  for (index = 0; index < sizeof(SavedPlayer) * stored_player_slots; index++) {
    checksum = checksumByte(checksum, bytes[index]);
  }
  return checksum;
}

/*
 * There is no whole-world checksum function any more, on either side.
 *
 * Both directions now total the payload as they stream it, in the order the
 * bytes go past -- players, then blocks in x-major order, then trees, then
 * the trailing scalars.  That order is the format, and it is the only thing
 * the two paths have to agree on; the separate blocks-and-trees passes that
 * used to exist here were a second full walk of 401,408 blocks whose only
 * product was a number the write pass could have counted for free.
 */

/* Fold one word into the running checksum.  The world clock was the first
   scalar the header carried outside the player records; the world seed is the
   second, and both are covered the same way. */
static u32 checksumWord(u32 checksum, u32 word) {
  u8 byte;
  for (byte = 0; byte < 4; byte++) {
    checksum = checksumByte(checksum, word >> (byte * 8));
  }
  return checksum;
}

/*
 * Point generation at the seed this world was built from.
 *
 * Only v11 and later recorded one; see SAVE_VERSION_SEED.  An older save
 * cannot be made to remember what it never wrote, but it can at least be made
 * to answer the same way twice: deriving the fallback from the slot turns
 * "the outskirts are different every load" into "the outskirts are wrong but
 * stable", which is the difference between a world you can build in and one
 * you cannot.  The saved extent is unaffected either way -- it arrives as
 * blocks, not as a seed.
 */
static void restoreWorldSeed(u32 version, const Header *saved) {
  if (version >= SAVE_VERSION_SEED) {
    world_seed = saved->world_seed;
  } else {
    world_seed = MAGIC_NUM ^ ((u32) game_file_num * 2654435761UL);
  }
  /* Nothing reproducible reads the gameplay RNG, but starting it from the
     same place a new world would keeps the two paths alike. */
  seed = world_seed;
}

/*
 * Point generation at the mods this world was made with, before a single
 * column of it is claimed.  A pre-v12 save records nothing, and there is
 * nothing to guess: those versions could only produce the classic world, so
 * a zero mask is exactly right and setWorldMods fills in the default.
 */
static void restoreWorldMods(u32 version, const Header *saved) {
  setWorldMods(version >= SAVE_VERSION_MODS ? (u16) saved->world_mods : 0);
}

/* The on-disk tree payload is frozen at the original 96 records even though
   the live pool grew for the wider streaming ring;
   treesDropOutsideFixedExtent packs every savable record below this line
   before a save is written. */
#define TREE_SAVE_BYTES (TREE_SAVE_COUNT * sizeof(TreeRecord))

u8 saving_available;
u8 storage_status = STORAGE_NO_CART;
u8 files_present[3];
u32 game_file_num;
char world_names[3][WORLD_NAME_LENGTH + 1];

/*
 * Every path here must be a legal 8.3 short name: ffconf.h sets FF_USE_LFN to
 * 0, so FatFs has no long-name support at all and create_name() rejects any
 * basename past eight characters or extension past three with
 * FR_INVALID_NAME.  The former "world_112_1.m64" overran both limits, so
 * f_open and f_stat failed on every save path before touching the card --
 * saving could never succeed, and f_stat's failure also made every slot read
 * as empty on the title screen.  The extent tag is kept, shortened, and the
 * four files now share one basename so the limit is obvious at a glance.
 *
 * Nothing needs migrating: no build could ever have written the old names.
 */
static char *file_names[] = {
  "mine64/w112_1.m64",
  "mine64/w112_2.m64",
  "mine64/w112_3.m64"
};

static char *temporary_file_names[] = {
  "mine64/w112_1.tmp",
  "mine64/w112_2.tmp",
  "mine64/w112_3.tmp"
};

static char *backup_file_names[] = {
  "mine64/w112_1.bak",
  "mine64/w112_2.bak",
  "mine64/w112_3.bak"
};

static char *name_file_names[] = {
  "mine64/w112_1.nam",
  "mine64/w112_2.nam",
  "mine64/w112_3.nam"
};

/* Where a file that failed validation goes instead of being destroyed.  See
   discardInvalidCurrent.  Nothing reads these; initStorage looks only at the
   world, backup and temporary names, so a rejected file is inert. */
static char *rejected_file_names[] = {
  "mine64/w112_1.bad",
  "mine64/w112_2.bad",
  "mine64/w112_3.bad"
};

static void setDefaultWorldName(u8 slot) {
  char *name = world_names[slot];

  name[0] = 'W';
  name[1] = 'o';
  name[2] = 'r';
  name[3] = 'l';
  name[4] = 'd';
  name[5] = ' ';
  name[6] = '1' + slot;
  name[7] = 0;
}

void setWorldName(u8 slot, const char *name) {
  u8 i;

  if (slot >= 3) {
    return;
  }
  for (i = 0; i < WORLD_NAME_LENGTH && name[i]; i++) {
    world_names[slot][i] = name[i] >= ' ' && name[i] <= '~' ?
      name[i] : ' ';
  }
  while (i > 0 && world_names[slot][i - 1] == ' ') {
    i--;
  }
  if (i == 0) {
    setDefaultWorldName(slot);
  } else {
    world_names[slot][i] = 0;
  }
}

static void loadWorldName(u8 slot) {
  FIL file;
  UINT read;
  char name[WORLD_NAME_LENGTH + 1];

  if (f_open(&file, name_file_names[slot], FA_READ) != FR_OK) {
    return;
  }
  if (f_read(&file, name, WORLD_NAME_LENGTH, &read) == FR_OK && read > 0) {
    name[read] = 0;
    setWorldName(slot, name);
  }
  f_close(&file);
}

static void saveWorldName(u8 slot) {
  FIL file;
  UINT written;
  u8 length = 0;

  while (length < WORLD_NAME_LENGTH && world_names[slot][length]) {
    length++;
  }
  if (f_open(&file, name_file_names[slot],
      FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
    return;
  }
  if (f_write(&file, world_names[slot], length, &written) == FR_OK &&
      written == length) {
    f_sync(&file);
  }
  f_close(&file);
}

static u8 recoverBackup(u8 slot) {
  FILINFO info;

  if (f_stat(backup_file_names[slot], &info) != FR_OK ||
      (info.fattrib & AM_DIR)) {
    return FALSE;
  }
  f_unlink(temporary_file_names[slot]);
  if (f_rename(file_names[slot], temporary_file_names[slot]) != FR_OK) {
    return FALSE;
  }
  if (f_rename(backup_file_names[slot], file_names[slot]) != FR_OK) {
    f_rename(temporary_file_names[slot], file_names[slot]);
    return FALSE;
  }
  f_unlink(temporary_file_names[slot]);
  return TRUE;
}

static void discardInvalidCurrent(u8 slot) {
  /*
   * A file that failed format, state, or checksum validation must not keep
   * occupying the final filename.  Otherwise the fresh fallback world cannot
   * complete its first transactional rename when no usable backup exists.
   *
   * Freeing the name is the requirement; deleting the bytes never was.  This
   * used to unlink, which meant one false rejection was the end of a world --
   * and the tree pool the load path forgot to reset made that rejection
   * certain, so the deletion is how a recoverable bug ate every save on the
   * card.  Moving the file aside costs one rename and keeps the evidence: a
   * .bad file can be pulled off the SD card and read, and if the rejection
   * was the game's fault rather than the file's, the world is still there.
   * Only the last rejection per slot is kept; unlinking is the fallback when
   * the rename itself will not go through.
   */
  f_unlink(rejected_file_names[slot]);
  if (f_rename(file_names[slot], rejected_file_names[slot]) != FR_OK) {
    f_unlink(file_names[slot]);
  }
}

static void restoreInventoryItem(Player *player, u8 item, u32 count) {
  while (count > 0) {
    u8 chunk = count > MAX_ITEM_STACK ? MAX_ITEM_STACK : count;
    if (addItemToInventory(player, item, chunk) != chunk) {
      return;
    }
    count -= chunk;
  }
}

static void savePlayerState(SavedPlayer *saved, Player *player) {
  u8 slot;
  u8 hunger_code;

  saved->position = player->position;
  saved->pitch = player->pitch;
  saved->yaw = player->yaw;
  saved->selected_hotbar_slot = player->selected_hotbar_slot;
  for (slot = 0; slot < sizeof(saved->reserved); slot++) {
    saved->reserved[slot] = 0;
  }
  /* Save v10 uses the spare high nibble beside the chunk-height marker for
     objective progress without growing the fixed 512-byte header. */
  saved->reserved[1] = (player->objective_stage & 0x0F) << 4;
  /* Encode hunger + 1 across the two unused high three-bit fields.  A zero
     code therefore means an older v10 save and restores a full food bar. */
  hunger_code = min(PLAYER_MAX_HUNGER, player->hunger) + 1;
  saved->reserved[0] |= (hunger_code & 0x07) << 5;
  saved->reserved[2] |= ((hunger_code >> 3) & 0x07) << 5;
  for (slot = 0; slot < INVENTORY_SIZE; slot++) {
    saved->inventory[slot] = player->inventory[slot];
  }
  for (slot = 0; slot < CRAFTING_SIZE; slot++) {
    saved->crafting[slot] = player->crafting[slot];
  }
  saved->carried_item = player->carried_item;
}

static void restoreSavedInventory(Player *player, SavedPlayer *saved) {
  u8 slot;
  u8 hunger_code;

  for (slot = 0; slot < INVENTORY_SIZE; slot++) {
    ItemStack stack = saved->inventory[slot];
    if (!ITEM_IS_VALID(stack.item) || stack.item == AIR) {
      stack.item = AIR;
      stack.count = 0;
    } else if (stack.count > itemMaxStack(stack.item)) {
      stack.count = itemMaxStack(stack.item);
    }
    player->inventory[slot] = stack;
  }
  for (slot = 0; slot < CRAFTING_SIZE; slot++) {
    ItemStack stack = saved->crafting[slot];
    if (!ITEM_IS_VALID(stack.item) || stack.item == AIR) {
      stack.item = AIR;
      stack.count = 0;
    } else if (stack.count > itemMaxStack(stack.item)) {
      stack.count = itemMaxStack(stack.item);
    }
    player->crafting[slot] = stack;
  }
  player->carried_item = saved->carried_item;
  if (!ITEM_IS_VALID(player->carried_item.item) ||
      player->carried_item.item == AIR) {
    player->carried_item.item = AIR;
    player->carried_item.count = 0;
  } else if (player->carried_item.count >
      itemMaxStack(player->carried_item.item)) {
    player->carried_item.count = itemMaxStack(player->carried_item.item);
  }
  player->selected_hotbar_slot =
    saved->selected_hotbar_slot < INVENTORY_COLUMNS ?
    saved->selected_hotbar_slot : 0;
  player->inventory_cursor = INVENTORY_HOTBAR_START +
    player->selected_hotbar_slot;
  player->crafting_cursor = 0;
  player->inventory_area = INVENTORY_AREA_ITEMS;
  player->crafting_table_open = FALSE;
  player->held_block = player->inventory[player->inventory_cursor].item;
  player->objective_stage = saved->reserved[1] >> 4;
  if (player->objective_stage > PLAYER_OBJECTIVE_COUNT) {
    player->objective_stage = 0;
  }
  player->objective_time = 180.f;
  hunger_code = (saved->reserved[0] >> 5) |
    ((saved->reserved[2] >> 5) << 3);
  player->hunger = hunger_code == 0 ? PLAYER_MAX_HUNGER :
    min(PLAYER_MAX_HUNGER, hunger_code - 1);
  player->hunger_progress = 0;
  player->survival_time = 240.f;
}

static void restorePlayerState(Player *player, Vector3 position, float pitch,
    float yaw, u32 held_block) {
  player->position = position;
  player->pitch = pitch;
  player->yaw = yaw;
  player->body_yaw = yaw;
  player->stick_turns = TRUE;
  player->look_rate_yaw = 0;
  player->look_rate_pitch = 0;
  player->walk_time = 0;
  player->walk_swing = 0;
  player->vault_time = 0;
  player->camera_y_offset = 0;
  player->knockback_velocity = (Vector3) {0, 0, 0};
  player->attack_time = 0;
  player->hurt_time = 0;
  player->objective_stage = 0;
  player->objective_time = 180.f;
  player->health = PLAYER_MAX_HEALTH;
  player->hunger = PLAYER_MAX_HUNGER;
  player->hunger_progress = 0;
  player->survival_time = 240.f;
  player->camera_mode = CAMERA_FIRST_PERSON;
  player->held_block = held_block;
  player->y_velocity = 0;
  player->fall_distance = 0;
  player->active = TRUE;
  /* A save never records a death, and this is the function that says what a
     loaded player is: alive, here, at full health. */
  player->dead = FALSE;
  player->death_time = 0;
  player->target_present = FALSE;
  player->breaking = FALSE;
  player->break_progress = 0;
  player->break_time = WOOD_BREAK_TIME;
}

static u8 playerStateValid(Player *player) {
  return player->position.x == player->position.x &&
    player->position.y == player->position.y &&
    player->position.z == player->position.z &&
    player->pitch == player->pitch && player->yaw == player->yaw &&
    player->position.x >= 0 &&
    player->position.x < MAX_X * BLOCK_SIZE &&
    player->position.y >= 0 &&
    player->position.y < (MAX_Y + 8) * BLOCK_SIZE &&
    player->position.z >= 0 &&
    player->position.z < MAX_Z * BLOCK_SIZE &&
    player->pitch >= 0 && player->pitch < 360 &&
    player->yaw >= 0 && player->yaw < 360;
}

const char *storageStatusText(void) {
  switch (storage_status) {
    case STORAGE_NO_CART:
      return "NO CART SAVE DEVICE";
    case STORAGE_CARD_NOT_READY:
      return "SD CARD NOT READY";
    case STORAGE_BAD_FILESYSTEM:
      return "SD MUST BE FAT32";
    case STORAGE_MOUNT_FAILED:
      return "SD MOUNT FAILED";
    case STORAGE_NO_DIRECTORY:
      return "CANNOT WRITE SD CARD";
    default:
      return "SAVING UNAVAILABLE";
  }
}

void initStorage() {
  FRESULT res;
  FILINFO info;
  int i;

  saving_available = FALSE;
  for (i = 0; i < 3; i++) {
    files_present[i] = FALSE;
    setDefaultWorldName(i);
  }

  if (cart_init() != 0) {
    storage_status = STORAGE_NO_CART;
    return;
  }
  /* f_mount with the mount-now flag runs disk_initialize, so it reports the
     card and the filesystem separately: FR_NOT_READY is the SD card itself
     failing to come up, FR_NO_FILESYSTEM is a card that reads fine but is not
     FAT16/FAT32 -- exFAT, the default for cards over 32 GB, lands here
     because ffconf.h leaves FF_FS_EXFAT at 0. */
  res = f_mount(&fs, "", 1);
  if (res != FR_OK) {
    storage_status = res == FR_NOT_READY ? STORAGE_CARD_NOT_READY :
      res == FR_NO_FILESYSTEM ? STORAGE_BAD_FILESYSTEM :
      STORAGE_MOUNT_FAILED;
    return;
  }
  res = f_stat("mine64", &info);

  if (res == FR_NO_FILE) {
    res = f_mkdir("mine64");
    if (res != FR_OK) {
      storage_status = STORAGE_NO_DIRECTORY;
      return;
    }
  } else if (res != FR_OK || !(info.fattrib & AM_DIR)) {
    storage_status = STORAGE_NO_DIRECTORY;
    return;
  }

  storage_status = STORAGE_OK;
  saving_available = TRUE;
  for (i = 0; i < 3; i++) {
    res = f_stat(file_names[i], &info);
    if (res != FR_OK &&
        f_stat(backup_file_names[i], &info) == FR_OK &&
        !(info.fattrib & AM_DIR)) {
      /* Recover the previous complete file if power was lost between the
         backup and final rename of a transactional save. */
      f_rename(backup_file_names[i], file_names[i]);
      res = f_stat(file_names[i], &info);
    }
    if (res != FR_OK &&
        f_stat(temporary_file_names[i], &info) == FR_OK &&
        !(info.fattrib & AM_DIR)) {
      /* This is the first-save power-loss case: there was no older file to
         rotate, but the synced temporary file may already be complete. */
      f_rename(temporary_file_names[i], file_names[i]);
      res = f_stat(file_names[i], &info);
    }
    files_present[i] = res == FR_OK && !(info.fattrib & AM_DIR);
    if (files_present[i]) {
      loadWorldName(i);
    }
    f_unlink(temporary_file_names[i]);
  }
}

static u8 readPage(FIL *file) {
  FRESULT result;
  UINT n_read;

  osInvalDCache(file_buffer, BUFFER_LEN);
  result = f_read(file, file_buffer, BUFFER_LEN, &n_read);
  cursor_pos = 0;
  buffer_bytes_read = n_read;
  return result == FR_OK && n_read > 0;
}

static u8 writePage(FIL *file) {
  FRESULT result;
  UINT written;

  cursor_pos = 0;
  osWritebackDCache(file_buffer, BUFFER_LEN);
  result = f_write(file, file_buffer, BUFFER_LEN, &written);
  return result == FR_OK && written == BUFFER_LEN;
}

u8 storageWriteFreezeReport(const char *text, u32 length) {
  FIL file;
  UINT written;
  u8 ok;

  if (!saving_available) {
    return FALSE;
  }
  if (f_open(&file, "mine64/freeze.txt", FA_WRITE | FA_CREATE_ALWAYS)
      != FR_OK) {
    return FALSE;
  }
  ok = f_write(&file, text, length, &written) == FR_OK && written == length;
  f_sync(&file);
  f_close(&file);
  return ok;
}

/*
 * The sliced save.
 *
 * The write was the last thing in the game that stopped the console outright:
 * ~200 KB of packed blocks pushed through a 512-byte window, from a graphics
 * callback, with no frames and no controller polling in between -- long
 * enough on a slow card to outlast the freeze watchdog's two-second patience
 * and make a successful save look exactly like a crash.  It runs on the same
 * terms the load does now: a bounded number of x-slabs per callback, with the
 * caller free to draw a frame between them.
 *
 * The checksum is what makes this less obvious than the read.  It covers the
 * players, then every block, then the trees, then the trailing scalars -- and
 * it lives in the header, which is the first thing in the file.  Rather than
 * walk all 401,408 blocks once to total them and again to write them, the
 * payload pass accumulates the checksum in exactly the order it writes, and
 * the header page is rewritten in place at the end.  One pass, and the file
 * is self-consistent even if the player mines a block halfway through their
 * own save: the total is taken from the bytes that actually went out.
 */
#define SAVE_STAGE_IDLE 0
#define SAVE_STAGE_BLOCKS 1
#define SAVE_STAGE_TREES 2

static struct {
  FIL file;
  u8 stage;
  u8 slot;
  u8 had_previous_file;
  u32 checksum;
  int block_x;    /* next x-slab of packed blocks to write */
  u32 tree_byte;
} save_job;

/*
 * The header as it was at the start of the save, kept out of file_buffer
 * because the payload overwrites that page hundreds of times before the
 * header is written again.  The player records in particular have to be the
 * ones the checksum was opened with, or the total will not match the bytes.
 */
static Header save_header;

static u8 writeSaveHeaderPage(void) {
  const u8 *bytes = (const u8 *) &save_header;

  for (cursor_pos = 0; cursor_pos < sizeof(Header); cursor_pos++) {
    file_buffer[cursor_pos] = bytes[cursor_pos];
  }
  while (cursor_pos < BUFFER_LEN) {
    file_buffer[cursor_pos++] = 0;
  }
  return writePage(&save_job.file);
}

/*
 * Close the file and either publish it or throw it away.  Split out so the
 * normal end of the payload and a write failure part-way through it land in
 * one place, exactly as finishLoadGame does for the read.
 */
static u8 finishSaveGame(u8 write_ok) {
  u8 slot = save_job.slot;

  save_job.stage = SAVE_STAGE_IDLE;
  if (write_ok && cursor_pos > 0) {
    while (cursor_pos < BUFFER_LEN) {
      file_buffer[cursor_pos++] = 0;
    }
    write_ok = writePage(&save_job.file);
  }
  if (write_ok) {
    /* Only now is the total known.  Seeking back beats the alternative of
       totalling the world before writing it, which is a second full pass
       over every block for a number four bytes long. */
    save_header.checksum = checksumWord(checksumWord(checksumWord(
      save_job.checksum, save_header.world_ticks), save_header.world_seed),
      save_header.world_mods);
    write_ok = f_lseek(&save_job.file, 0) == FR_OK && writeSaveHeaderPage();
  }
  if (write_ok) {
    write_ok = f_sync(&save_job.file) == FR_OK;
  }
  if (f_close(&save_job.file) != FR_OK) {
    write_ok = FALSE;
  }
  if (write_ok) {
    f_unlink(backup_file_names[slot]);
    if (save_job.had_previous_file &&
        f_rename(file_names[slot], backup_file_names[slot]) != FR_OK) {
      write_ok = FALSE;
    }
    if (write_ok &&
        f_rename(temporary_file_names[slot], file_names[slot]) != FR_OK) {
      if (save_job.had_previous_file) {
        f_rename(backup_file_names[slot], file_names[slot]);
      }
      write_ok = FALSE;
    }
  }
  if (write_ok) {
    save_count++;
    files_present[slot] = TRUE;
    saveWorldName(slot);
  } else {
    f_unlink(temporary_file_names[slot]);
  }
  return write_ok ? SAVE_DONE : SAVE_FAILED;
}

u8 beginSaveGame(void) {
  u8 player_num;
  u8 *bytes = (u8 *) &save_header;
  u32 index;

  save_job.stage = SAVE_STAGE_IDLE;
  if (!saving_available || game_file_num < 1 || game_file_num > 3) {
    return SAVE_FAILED;
  }
  /*
   * The payload comes out of the home store, which holds the whole extent
   * whether or not it is currently in the block window.  This is what retired
   * the old "too far from spawn to save" refusal: the format still writes the
   * whole fixed extent, but it no longer needs all of it resident to do it.
   *
   * A resident column may hold edits made since it was last flushed, so fold
   * those in before the payload pass reads the store.  Nothing can evict a
   * column out from under it afterwards: the job driver runs this stage in
   * place of the streaming step, not beside it.
   */
  homeFlushResident();
  /* The residency ring is wider than the world, so live tree records can sit
     in columns past the extent -- coordinates treesValid rejects on load.
     Retire them before the checksum sees them, or this save reads as corrupt
     on the next boot and silently falls back to the backup. */
  treesDropOutsideFixedExtent();
  save_job.slot = game_file_num - 1;
  save_job.had_previous_file = files_present[save_job.slot];
  if (f_open(&save_job.file, temporary_file_names[save_job.slot],
      FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
    return SAVE_FAILED;
  }

  /* Inactive slots are included in the checksummed fixed-size header.  Clear
     them first so a new single-player save is deterministic and recoverable. */
  for (index = 0; index < sizeof(Header); index++) {
    bytes[index] = 0;
  }
  save_header.magic_num = MAGIC_NUM;
  save_header.version = SAVE_VERSION;
  save_header.file_num = game_file_num;
  save_header.save_count = save_count + 1;
  save_header.player_count = active_player_count;
  save_header.world_ticks = dayCycleWorldTicks();
  /* What every column outside the saved extent will be regenerated from when
     this world is loaded again -- the seed alone is not enough, because the
     mods decide what the generator does with it. */
  save_header.world_seed = world_seed;
  save_header.world_mods = world_mods;
  for (player_num = 0; player_num < active_player_count; player_num++) {
    savePlayerState(&save_header.player[player_num], &players[player_num]);
  }
  saveWorldDimensions(&save_header);

  /* The players' share of the total, which is all of it that can be known
     before the payload goes out.  Written with a zero checksum for now; the
     page is rewritten from finishSaveGame with the real one. */
  save_job.checksum = checksumPlayers(save_header.player,
    save_header.player_count, MAX_PLAYERS);
  if (!writeSaveHeaderPage()) {
    return finishSaveGame(FALSE);
  }
  save_job.block_x = 0;
  save_job.tree_byte = 0;
  save_job.stage = SAVE_STAGE_BLOCKS;
  return SAVE_BUSY;
}

/*
 * `slabs` x-slabs of packed blocks per call, then the trailing tree payload.
 * The same granularity the read uses, for the same reason: a slab is 1792
 * bytes, three and a half pages of cart traffic, which is small enough to
 * disappear inside a frame.
 */
u8 stepSaveGame(u16 slabs) {
  int block_y, block_z;

  if (save_job.stage == SAVE_STAGE_IDLE) {
    return SAVE_DONE;
  }

  /* Blocks stay nibble-packed on disk in the original x-major order, so this
     re-packs pairs out of the home store rather than copying a flat array.
     MAX_Z is even, so a pair never straddles a row and the byte stream is
     identical to what earlier versions wrote. */
  while (save_job.stage == SAVE_STAGE_BLOCKS && slabs > 0) {
    for (block_y = 0; block_y < MAX_Y; block_y++) {
      for (block_z = 0; block_z < MAX_Z; block_z += 2) {
        u8 high = homeBlockGet(save_job.block_x, block_y, block_z);
        u8 low = homeBlockGet(save_job.block_x, block_y, block_z + 1);

        file_buffer[cursor_pos++] = (u8) ((high << 4) | (low & 0x0F));
        /* Folded in here, in the order the loader will read them back. */
        save_job.checksum = checksumByte(save_job.checksum, high);
        save_job.checksum = checksumByte(save_job.checksum, low);
        if (cursor_pos >= BUFFER_LEN && !writePage(&save_job.file)) {
          return finishSaveGame(FALSE);
        }
      }
    }
    slabs--;
    if (++save_job.block_x >= MAX_X) {
      save_job.stage = SAVE_STAGE_TREES;
    }
  }

  if (save_job.stage != SAVE_STAGE_TREES) {
    return SAVE_BUSY;
  }

  /* The tree payload is 2 KB at most -- a single slice's worth on its own,
     and the last thing in the file either way. */
  {
    const u8 *trees_bytes = (const u8 *) trees;

    while (save_job.tree_byte < TREE_SAVE_BYTES) {
      u8 byte = trees_bytes[save_job.tree_byte++];

      file_buffer[cursor_pos++] = byte;
      save_job.checksum = checksumByte(save_job.checksum, byte);
      if (cursor_pos >= BUFFER_LEN && !writePage(&save_job.file)) {
        return finishSaveGame(FALSE);
      }
    }
  }
  return finishSaveGame(TRUE);
}

u8 saveGameProgress(void) {
  if (save_job.stage == SAVE_STAGE_IDLE) {
    return 100;
  }
  if (save_job.stage == SAVE_STAGE_TREES) {
    return 99;
  }
  return (u8) ((save_job.block_x * 99) / MAX_X);
}

u8 saveGame() {
  u8 status = beginSaveGame();

  while (status == SAVE_BUSY) {
    status = stepSaveGame(MAX_X);
  }
  return status == SAVE_DONE;
}

/*
 * The header pass: open the file, validate it, restore the players from the
 * single 512-byte page it occupies, and claim the extent the payload will be
 * written into.  One page of cart traffic, so it stays whole.
 */
u8 beginLoadGame(void) {
  Header *header = (Header *) file_buffer;
  HeaderV8 *header_v8 = (HeaderV8 *) file_buffer;
  HeaderV7 *header_v7 = (HeaderV7 *) file_buffer;
  HeaderV3 *header_v3 = (HeaderV3 *) file_buffer;
  HeaderV2 *header_v2 = (HeaderV2 *) file_buffer;
  HeaderV1 *header_v1 = (HeaderV1 *) file_buffer;
  LegacyHeader *legacy_header = (LegacyHeader *) file_buffer;
  FRESULT result;
  u8 read_ok;
  u32 version;
  u32 saved_world_ticks = 0;
  u32 saved_world_seed = 0;
  u32 saved_world_mods = 0;
  u32 wood_count[2] = {0, 0};
  u32 planks_count[2] = {0, 0};
  u32 crafting_table_count[2] = {0, 0};
  u32 stick_count[2] = {0, 0};
  u32 sword_count[2] = {0, 0};
  u32 pickaxe_count[2] = {0, 0};
  u32 expected_checksum = 0;
  u32 computed_checksum = 0;
  u8 full_inventory_saved = FALSE;
  u8 tree_records_saved = FALSE;
  u8 legacy_tree_records_saved = FALSE;
  u8 player_num;

  load_job.stage = LOAD_STAGE_IDLE;
  if (game_file_num < 1 || game_file_num > 3) {
    return LOAD_GENERATE;
  }
  result = f_open(&load_job.file, file_names[game_file_num - 1], FA_READ);
  if (result != FR_OK) {
    files_present[game_file_num - 1] = FALSE;
    return LOAD_GENERATE;
  }
  read_ok = readPage(&load_job.file);
  if (!read_ok || legacy_header->magic_num != MAGIC_NUM ||
      legacy_header->version > SAVE_VERSION ||
      (legacy_header->version >= SAVE_VERSION_HEADER &&
       buffer_bytes_read < sizeof(Header)) ||
      (legacy_header->version == 8 &&
       buffer_bytes_read < sizeof(HeaderV8)) ||
      (legacy_header->version == 7 &&
       !savedWorldDimensionsMatch(header_v7->player)) ||
      (legacy_header->version >= SAVE_VERSION_HEADER &&
       !savedWorldDimensionsMatch(header->player)) ||
      (legacy_header->version == 8 &&
       !savedWorldDimensionsMatch(header_v8->player)) ||
      (legacy_header->version >= 1 &&
       ((legacy_header->version >= SAVE_VERSION_HEADER &&
         (header->player_count < 1 || header->player_count > MAX_PLAYERS)) ||
        (legacy_header->version == 8 &&
         (header_v8->player_count < 1 || header_v8->player_count > MAX_PLAYERS)) ||
        (legacy_header->version < SAVE_VERSION_HEADER &&
         (header_v1->player_count < 1 ||
          header_v1->player_count > LEGACY_MAX_PLAYERS))))) {
    f_close(&load_job.file);
    if (recoverBackup(game_file_num - 1)) {
      /* The backup is now the current file; start its header pass over.
         Recovery consumes the backup, so this can only happen once. */
      return beginLoadGame();
    }
    discardInvalidCurrent(game_file_num - 1);
    files_present[game_file_num - 1] = FALSE;
    return LOAD_GENERATE;
  }

  /* Past validation, so the version is trustworthy -- and it must be read out
     of the buffer now, before the payload overwrites the page it lives on. */
  version = legacy_header->version;
  save_count = legacy_header->save_count;
  players[0].position = legacy_header->position;
  players[0].pitch = legacy_header->pitch;
  players[0].yaw = legacy_header->yaw;
  players[0].body_yaw = players[0].yaw;
  players[0].walk_time = 0;
  players[0].walk_swing = 0;
  players[0].knockback_velocity = (Vector3) {0, 0, 0};
  players[0].attack_time = 0;
  players[0].hurt_time = 0;
  players[0].health = PLAYER_MAX_HEALTH;
  players[0].hunger = PLAYER_MAX_HUNGER;
  players[0].hunger_progress = 0;
  players[0].survival_time = 240.f;
  players[0].camera_mode = CAMERA_FIRST_PERSON;
  players[0].held_block = legacy_header->held_block;
  players[0].y_velocity = 0;
  players[0].active = TRUE;
  players[0].target_present = FALSE;
  players[0].breaking = FALSE;
  players[0].break_progress = 0;
  for (player_num = 1; player_num < MAX_PLAYERS; player_num++) {
    players[player_num].active = FALSE;
    players[player_num].camera_mode = CAMERA_FIRST_PERSON;
    players[player_num].knockback_velocity = (Vector3) {0, 0, 0};
    players[player_num].attack_time = 0;
    players[player_num].hurt_time = 0;
    players[player_num].health = PLAYER_MAX_HEALTH;
    players[player_num].hunger = PLAYER_MAX_HUNGER;
    players[player_num].hunger_progress = 0;
    players[player_num].survival_time = 240.f;
    players[player_num].target_present = FALSE;
    players[player_num].breaking = FALSE;
    players[player_num].break_progress = 0;
  }
  active_player_count = 1;
  restoreWorldSeed(version, header);
  /* Before windowClaimFixedExtent below, and long before any streaming: the
     mask decides what every regenerated column outside the save will be. */
  restoreWorldMods(version, header);

  if (version >= SAVE_VERSION_HEADER) {
    /* The scalars the checksum folds in after the world data.  Copy them
       out with the rest of the header, not at the end of the load, when the
       struct they came from is long since overwritten. */
    saved_world_ticks = header->world_ticks;
    saved_world_seed = header->world_seed;
    saved_world_mods = header->world_mods;
    expected_checksum = header->checksum;
    computed_checksum = checksumPlayers(header->player, header->player_count,
      MAX_PLAYERS);
    for (player_num = 0; player_num < header->player_count; player_num++) {
      restorePlayerState(&players[player_num], header->player[player_num].position,
        header->player[player_num].pitch, header->player[player_num].yaw, AIR);
      restoreSavedInventory(&players[player_num], &header->player[player_num]);
    }
    active_player_count = header->player_count;
    full_inventory_saved = TRUE;
    tree_records_saved = TRUE;
    cursor_pos = BUFFER_LEN;
    setDayCycleWorldTicks(header->world_ticks);
  } else if (version == 8) {
    expected_checksum = header_v8->checksum;
    computed_checksum = checksumPlayers(header_v8->player,
      header_v8->player_count, MAX_PLAYERS);
    for (player_num = 0; player_num < header_v8->player_count; player_num++) {
      restorePlayerState(&players[player_num], header_v8->player[player_num].position,
        header_v8->player[player_num].pitch, header_v8->player[player_num].yaw, AIR);
      restoreSavedInventory(&players[player_num], &header_v8->player[player_num]);
    }
    active_player_count = header_v8->player_count;
    full_inventory_saved = TRUE;
    tree_records_saved = TRUE;
    cursor_pos = BUFFER_LEN;
    setDayCycleWorldTicks(DAY_CYCLE_START_TICK);
  } else if (version >= 4) {
    expected_checksum = header_v7->checksum;
    computed_checksum = checksumPlayers(header_v7->player,
      header_v7->player_count, LEGACY_MAX_PLAYERS);
    restorePlayerState(&players[0], header_v7->player[0].position,
      header_v7->player[0].pitch, header_v7->player[0].yaw, AIR);
    restoreSavedInventory(&players[0], &header_v7->player[0]);
    if (header_v7->player_count == 2) {
      restorePlayerState(&players[1], header_v7->player[1].position,
        header_v7->player[1].pitch, header_v7->player[1].yaw, AIR);
      restoreSavedInventory(&players[1], &header_v7->player[1]);
      active_player_count = 2;
    }
    full_inventory_saved = TRUE;
    tree_records_saved = version >= 6;
    legacy_tree_records_saved = version == 5;
    cursor_pos = LEGACY_COOP_HEADER_SIZE;
  } else if (version >= 3 && header_v3->player_count == 2) {
    restorePlayerState(&players[0], header_v3->player[0].position,
      header_v3->player[0].pitch, header_v3->player[0].yaw,
      header_v3->player[0].held_block);
    restorePlayerState(&players[1], header_v3->player[1].position,
      header_v3->player[1].pitch, header_v3->player[1].yaw,
      header_v3->player[1].held_block);
    wood_count[0] = header_v3->player[0].wood_count;
    planks_count[0] = header_v3->player[0].planks_count;
    crafting_table_count[0] = header_v3->player[0].crafting_table_count;
    stick_count[0] = header_v3->player[0].stick_count;
    sword_count[0] = header_v3->player[0].sword_count;
    pickaxe_count[0] = header_v3->player[0].pickaxe_count;
    wood_count[1] = header_v3->player[1].wood_count;
    planks_count[1] = header_v3->player[1].planks_count;
    crafting_table_count[1] = header_v3->player[1].crafting_table_count;
    stick_count[1] = header_v3->player[1].stick_count;
    sword_count[1] = header_v3->player[1].sword_count;
    pickaxe_count[1] = header_v3->player[1].pickaxe_count;
    active_player_count = 2;
    /*
     * Versions 0-3 padded their header to a complete 128-byte page.  World
     * bytes always begin there, not at sizeof(the versioned header).
     */
    cursor_pos = LEGACY_HEADER_PAGE_SIZE;
  } else if (version >= 3) {
    restorePlayerState(&players[0], header_v3->player[0].position,
      header_v3->player[0].pitch, header_v3->player[0].yaw,
      header_v3->player[0].held_block);
    wood_count[0] = header_v3->player[0].wood_count;
    planks_count[0] = header_v3->player[0].planks_count;
    crafting_table_count[0] = header_v3->player[0].crafting_table_count;
    stick_count[0] = header_v3->player[0].stick_count;
    sword_count[0] = header_v3->player[0].sword_count;
    pickaxe_count[0] = header_v3->player[0].pickaxe_count;
    cursor_pos = LEGACY_HEADER_PAGE_SIZE;
  } else if (version >= 2 && header_v2->player_count == 2) {
    restorePlayerState(&players[0], header_v2->player[0].position,
      header_v2->player[0].pitch, header_v2->player[0].yaw,
      header_v2->player[0].held_block);
    restorePlayerState(&players[1], header_v2->player[1].position,
      header_v2->player[1].pitch, header_v2->player[1].yaw,
      header_v2->player[1].held_block);
    wood_count[0] = header_v2->player[0].wood_count;
    wood_count[1] = header_v2->player[1].wood_count;
    active_player_count = 2;
    cursor_pos = LEGACY_HEADER_PAGE_SIZE;
  } else if (version >= 2) {
    restorePlayerState(&players[0], header_v2->player[0].position,
      header_v2->player[0].pitch, header_v2->player[0].yaw,
      header_v2->player[0].held_block);
    wood_count[0] = header_v2->player[0].wood_count;
    cursor_pos = LEGACY_HEADER_PAGE_SIZE;
  } else if (version >= 1 && header_v1->player_count == 2) {
    players[0].position = header_v1->player[0].position;
    players[0].pitch = header_v1->player[0].pitch;
    players[0].yaw = header_v1->player[0].yaw;
    players[0].body_yaw = players[0].yaw;
    players[0].walk_time = 0;
    players[0].walk_swing = 0;
    players[0].held_block = header_v1->player[0].held_block;
    players[1].position = header_v1->player[1].position;
    players[1].pitch = header_v1->player[1].pitch;
    players[1].yaw = header_v1->player[1].yaw;
    players[1].body_yaw = players[1].yaw;
    players[1].walk_time = 0;
    players[1].walk_swing = 0;
    players[1].held_block = header_v1->player[1].held_block;
    players[1].y_velocity = 0;
    players[1].active = TRUE;
    active_player_count = 2;
    cursor_pos = LEGACY_HEADER_PAGE_SIZE;
  } else if (version >= 1) {
    players[0].position = header_v1->player[0].position;
    players[0].pitch = header_v1->player[0].pitch;
    players[0].yaw = header_v1->player[0].yaw;
    players[0].body_yaw = players[0].yaw;
    players[0].walk_time = 0;
    players[0].walk_swing = 0;
    players[0].held_block = header_v1->player[0].held_block;
    cursor_pos = LEGACY_HEADER_PAGE_SIZE;
  } else {
    cursor_pos = LEGACY_HEADER_PAGE_SIZE;
  }

  if (version < 8) {
    setDayCycleWorldTicks(DAY_CYCLE_START_TICK);
  }

  if (!full_inventory_saved) {
    resetPlayerInventory(&players[0]);
    restoreInventoryItem(&players[0], WOOD, wood_count[0]);
    restoreInventoryItem(&players[0], PLANKS, planks_count[0]);
    restoreInventoryItem(&players[0], CRAFTING_TABLE, crafting_table_count[0]);
    restoreInventoryItem(&players[0], STICK, stick_count[0]);
    restoreInventoryItem(&players[0], WOOD_SWORD, sword_count[0]);
    restoreInventoryItem(&players[0], WOOD_PICKAXE, pickaxe_count[0]);
    if (players[1].active) {
      resetPlayerInventory(&players[1]);
      restoreInventoryItem(&players[1], WOOD, wood_count[1]);
      restoreInventoryItem(&players[1], PLANKS, planks_count[1]);
      restoreInventoryItem(&players[1], CRAFTING_TABLE, crafting_table_count[1]);
      restoreInventoryItem(&players[1], STICK, stick_count[1]);
      restoreInventoryItem(&players[1], WOOD_SWORD, sword_count[1]);
      restoreInventoryItem(&players[1], WOOD_PICKAXE, pickaxe_count[1]);
    }
  }

  for (player_num = 0; player_num < active_player_count; player_num++) {
    if (!playerStateValid(&players[player_num])) {
      read_ok = FALSE;
      break;
    }
  }
  if (!read_ok) {
    f_close(&load_job.file);
    if (recoverBackup(game_file_num - 1)) {
      return beginLoadGame();
    }
    discardInvalidCurrent(game_file_num - 1);
    files_present[game_file_num - 1] = FALSE;
    return LOAD_GENERATE;
  }

  /*
   * Clear the derived pools this load is about to refill.
   *
   * beginWorldGeneration does this for a fresh world and the load path never
   * did, which cost every save on the cartridge.  The file carries only the
   * frozen TREE_SAVE_COUNT records, so trees[TREE_SAVE_COUNT..] keeps
   * whatever was in RAM -- and on a cold boot that is bss, where base_y is 0
   * rather than TREE_INACTIVE_Y, so every one of those records reads as a
   * live tree rooted at (0, 0).  treesValid walks the whole live pool, so the
   * second of them fails the duplicate-root test, the load calls the file
   * corrupt, and discardInvalidCurrent takes the world away.  The menu
   * previews the highlighted slot as soon as it comes up, so the first boot
   * after the first save was enough: saving worked, and nothing ever survived
   * being loaded back.
   *
   * The home store has the milder version of the same problem -- the payload
   * pass overwrites every cell of the extent, so it only shows if a load
   * fails part-way and leaves another world's blocks behind as this one's.
   */
  initTrees();
  initHome();

  /* Every column of the saved extent needs a slot before blockSet will take
     the incoming terrain, and each one counts as fully built once it has.
     Saved worlds live entirely inside the fixed extent, so the render origin
     returns to zero with them. */
  windowClaimFixedExtent();
  worldMarkFixedExtentBuilt();
  graphicsSetRenderOrigin(0, 0);

  /* Hand the header's findings to the payload pass before the page they came
     from is overwritten, and let the caller draw a frame. */
  load_job.version = version;
  load_job.saved_world_ticks = saved_world_ticks;
  load_job.saved_world_seed = saved_world_seed;
  load_job.saved_world_mods = saved_world_mods;
  load_job.expected_checksum = expected_checksum;
  load_job.checksum = computed_checksum;
  load_job.full_inventory_saved = full_inventory_saved;
  load_job.tree_records_saved = tree_records_saved;
  load_job.legacy_tree_records_saved = legacy_tree_records_saved;
  load_job.read_ok = TRUE;
  load_job.block_x = 0;
  load_job.tree_byte = 0;
  load_job.stage = LOAD_STAGE_BLOCKS;
  return LOAD_BUSY;
}

/*
 * Close the file, fold in the trailing header scalars, and decide whether
 * what arrived is a world or a wreck.  Split out so both the normal end of
 * the payload and a read failure part-way through it land in one place.
 */
static u8 finishLoadGame(void) {
  f_close(&load_job.file);
  load_job.stage = LOAD_STAGE_IDLE;

  /* Each scalar the header grew is folded in only from the version that
     started writing it, so an older save still verifies against exactly the
     bytes it was written with.  These are the copies taken during the header
     pass: the structs they came from live in file_buffer, which the payload
     read has long since overwritten. */
  if (load_job.version >= SAVE_VERSION_HEADER) {
    load_job.checksum = checksumWord(load_job.checksum,
      load_job.saved_world_ticks);
  }
  if (load_job.version >= SAVE_VERSION_SEED) {
    load_job.checksum = checksumWord(load_job.checksum,
      load_job.saved_world_seed);
  }
  if (load_job.version >= SAVE_VERSION_MODS) {
    load_job.checksum = checksumWord(load_job.checksum,
      load_job.saved_world_mods);
  }
  if (load_job.read_ok &&
      (!load_job.full_inventory_saved ||
        load_job.checksum == load_job.expected_checksum) &&
      (!load_job.tree_records_saved || treesValid())) {
    /* The payload pass filled every extent column, so the store is now the
       loaded world rather than a generated one.  Without this, the first
       column to regenerate would capture generated terrain over it. */
    homeMarkAllPresent();
    return LOAD_DONE;
  }
  if (recoverBackup(game_file_num - 1)) {
    return beginLoadGame();
  }
  discardInvalidCurrent(game_file_num - 1);
  files_present[game_file_num - 1] = FALSE;
  return LOAD_GENERATE;
}

/* One byte of payload, or FALSE when the file ran out under us. */
static u8 nextPayloadByte(u8 *out) {
  if (cursor_pos >= buffer_bytes_read) {
    if (!readPage(&load_job.file)) {
      load_job.read_ok = FALSE;
      return FALSE;
    }
  }
  *out = file_buffer[cursor_pos++];
  return TRUE;
}

/*
 * `slabs` x-slabs of packed blocks per call, then the trailing tree payload.
 * A slab is MAX_Y * MAX_Z / 2 bytes -- 1792 here, or three and a half pages
 * of cart traffic -- which is the granularity the progress bar moves at.
 */
u8 stepLoadGame(u16 slabs) {
  int block_y, block_z;
  u8 packed;

  if (load_job.stage == LOAD_STAGE_IDLE) {
    return LOAD_DONE;
  }

  while (load_job.stage == LOAD_STAGE_BLOCKS && slabs > 0) {
    for (block_y = 0; block_y < MAX_Y; block_y++) {
      for (block_z = 0; block_z < MAX_Z; block_z += 2) {
        if (!nextPayloadByte(&packed)) {
          return finishLoadGame();
        }
        if (!BLOCK_IS_VALID(packed >> 4)) {
          packed &= 0x0F;
        }
        if (!BLOCK_IS_VALID(packed & 0x0F)) {
          packed &= 0xF0;
        }
        /* Into the window so the world is visible immediately, and into the
           home store because that is what the next save reads and what a
           column regenerating later is restored from. */
        blockSet(load_job.block_x, block_y, block_z, packed >> 4);
        blockSet(load_job.block_x, block_y, block_z + 1, packed & 0x0F);
        homeBlockSet(load_job.block_x, block_y, block_z, packed >> 4);
        homeBlockSet(load_job.block_x, block_y, block_z + 1, packed & 0x0F);
        if (load_job.full_inventory_saved) {
          load_job.checksum = checksumByte(load_job.checksum, packed >> 4);
          load_job.checksum = checksumByte(load_job.checksum, packed & 0x0F);
        }
      }
    }
    slabs--;
    if (++load_job.block_x >= MAX_X) {
      load_job.stage = LOAD_STAGE_TREES;
    }
  }

  if (load_job.stage != LOAD_STAGE_TREES) {
    return LOAD_BUSY;
  }

  /* The tree payload is 2 KB at most -- a single slice's worth on its own,
     and the last thing in the file either way. */
  if (load_job.tree_records_saved) {
    /* Only the frozen 96-record payload is on disk; the rest of the larger
       live pool stays inactive from initTrees. */
    u8 *trees_ptr = (u8 *) trees;

    while (load_job.tree_byte < TREE_SAVE_BYTES) {
      if (!nextPayloadByte(&trees_ptr[load_job.tree_byte])) {
        return finishLoadGame();
      }
      load_job.checksum = checksumByte(load_job.checksum,
        trees_ptr[load_job.tree_byte]);
      load_job.tree_byte++;
    }
  } else {
    if (load_job.legacy_tree_records_saved) {
      /* V5 files carried the original 96-record pool. */
      while (load_job.tree_byte < sizeof(TreeRecordV5) * TREE_SAVE_COUNT) {
        if (!nextPayloadByte(&packed)) {
          return finishLoadGame();
        }
        load_job.checksum = checksumByte(load_job.checksum, packed);
        load_job.tree_byte++;
      }
    }
    recoverTreesFromWorld();
  }
  return finishLoadGame();
}

void cancelLoadGame(void) {
  if (load_job.stage == LOAD_STAGE_IDLE) {
    return;
  }
  f_close(&load_job.file);
  load_job.stage = LOAD_STAGE_IDLE;
}

u8 loadGameProgress(void) {
  if (load_job.stage == LOAD_STAGE_IDLE) {
    return 100;
  }
  if (load_job.stage == LOAD_STAGE_TREES) {
    return 99;
  }
  return (u8) ((load_job.block_x * 99) / MAX_X);
}

void loadGame() {
  u8 status = beginLoadGame();

  while (status == LOAD_BUSY) {
    status = stepLoadGame(MAX_X);
  }
  if (status == LOAD_GENERATE) {
    initWorld();
    initPlayers();
  }
}
