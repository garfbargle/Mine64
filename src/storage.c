#include "cart.h"
#include "ff/ff.h"

#include "storage.h"
#include "blocks.h"
#include "graphics.h"
#include "items.h"
#include "player.h"
#include "trees.h"
#include "world.h"
#include "day_cycle.h"

static FATFS fs;

// "MI64"
#define MAGIC_NUM 0x4D493634
#define LEGACY_HEADER_PAGE_SIZE 128
#define LEGACY_COOP_HEADER_SIZE 256
#define LEGACY_MAX_PLAYERS 2
#define BUFFER_LEN 512
#define SAVE_VERSION 10

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

static u32 checksumSave(SavedPlayer *players_to_save, u32 player_count,
    u8 stored_player_slots) {
  u32 checksum = checksumPlayers(players_to_save, player_count,
    stored_player_slots);
  int x, y, z;

  /* Blocks are stored per column now, but the checksum has to keep visiting
     them in the original x-major order or every world written before the
     window would fail verification. */
  for (x = 0; x < MAX_X; x++) {
    for (y = 0; y < MAX_Y; y++) {
      for (z = 0; z < MAX_Z; z++) {
        checksum = checksumByte(checksum, blockGet(x, y, z));
      }
    }
  }
  return checksum;
}

static u32 checksumWorldTicks(u32 checksum, u32 world_ticks) {
  u8 byte;
  for (byte = 0; byte < 4; byte++) {
    checksum = checksumByte(checksum, world_ticks >> (byte * 8));
  }
  return checksum;
}

/* The on-disk tree payload is frozen at the original 96 records even though
   the live pool grew for the wider streaming ring;
   treesDropOutsideFixedExtent packs every savable record below this line
   before a save is written. */
#define TREE_SAVE_BYTES (TREE_SAVE_COUNT * sizeof(TreeRecord))

static u32 checksumTrees(u32 checksum) {
  u8 *bytes = (u8 *) trees;
  u32 index;

  for (index = 0; index < TREE_SAVE_BYTES; index++) {
    checksum = checksumByte(checksum, bytes[index]);
  }
  return checksum;
}

u8 saving_available;
u8 files_present[3];
u32 game_file_num;
char world_names[3][WORLD_NAME_LENGTH + 1];

static char *file_names[] = {
  "mine64/world_112_1.m64",
  "mine64/world_112_2.m64",
  "mine64/world_112_3.m64"
};

static char *temporary_file_names[] = {
  "mine64/temp_112_1.tmp",
  "mine64/temp_112_2.tmp",
  "mine64/temp_112_3.tmp"
};

static char *backup_file_names[] = {
  "mine64/world_112_1.bak",
  "mine64/world_112_2.bak",
  "mine64/world_112_3.bak"
};

static char *name_file_names[] = {
  "mine64/world_112_1.name",
  "mine64/world_112_2.name",
  "mine64/world_112_3.name"
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
   */
  f_unlink(file_names[slot]);
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
    return;
  }
  res = f_mount(&fs, "", 1);
  if (res != FR_OK) {
    return;
  }
  res = f_stat("mine64", &info);

  if (res == FR_NO_FILE) {
    res = f_mkdir("mine64");
    if (res != FR_OK) {
      return;
    }
  } else if (res != FR_OK || !(info.fattrib & AM_DIR)) {
    return;
  }

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

u8 saveGame() {
  FIL file;
  Header *header = (Header *) file_buffer;
  FRESULT result;
  u8 write_ok = TRUE;
  u8 slot;
  u8 player_num;
  u8 had_previous_file;
  int block_x, block_y, block_z;
  u8 *trees_ptr;
  const u8 *trees_end = (const u8 *) trees + TREE_SAVE_BYTES;

  if (!saving_available || game_file_num < 1 || game_file_num > 3) {
    return FALSE;
  }
  /*
   * The v10 format writes the whole fixed extent out of live block data.
   * Once streaming has evicted any of it, blockGet answers BLOCK_NOT_RESIDENT
   * there and this loop would pack 0xF garbage over the player's world --
   * silent corruption discovered only on the next load.  Refuse instead.
   * (The caller checks this first and shows its own message; this is the
   * backstop for any other path.)  Task 6 replaces the format with per-chunk
   * diffs and removes the restriction.
   */
  if (!worldFixedExtentResident()) {
    return FALSE;
  }
  /* The residency ring is wider than the world, so live tree records can sit
     in columns past the extent -- coordinates treesValid rejects on load.
     Retire them before the checksum sees them, or this save reads as corrupt
     on the next boot and silently falls back to the backup. */
  treesDropOutsideFixedExtent();
  slot = game_file_num - 1;
  had_previous_file = files_present[slot];
  result = f_open(&file, temporary_file_names[slot],
    FA_WRITE | FA_CREATE_ALWAYS);
  if (result != FR_OK) {
    return FALSE;
  }

  /* Inactive slots are included in the checksummed fixed-size header.  Clear
     them first so a new single-player save is deterministic and recoverable. */
  for (cursor_pos = 0; cursor_pos < sizeof(Header); cursor_pos++) {
    file_buffer[cursor_pos] = 0;
  }
  header->magic_num = MAGIC_NUM;
  header->version = SAVE_VERSION;
  header->file_num = game_file_num;
  header->save_count = save_count + 1;
  header->player_count = active_player_count;
  header->world_ticks = dayCycleWorldTicks();
  for (player_num = 0; player_num < active_player_count; player_num++) {
    savePlayerState(&header->player[player_num], &players[player_num]);
  }
  saveWorldDimensions(header);
  header->checksum = checksumWorldTicks(checksumTrees(
    checksumSave(header->player, header->player_count, MAX_PLAYERS)),
    header->world_ticks);

  cursor_pos = sizeof(Header);
  while (cursor_pos < BUFFER_LEN) {
    file_buffer[cursor_pos++] = 0;
  }
  write_ok = writePage(&file);

  /* Blocks stay nibble-packed on disk in the original x-major order, so this
     re-packs pairs out of the column window rather than copying a flat array.
     MAX_Z is even, so a pair never straddles a row and the byte stream is
     identical to what earlier versions wrote. */
  for (block_x = 0; write_ok && block_x < MAX_X; block_x++) {
    for (block_y = 0; write_ok && block_y < MAX_Y; block_y++) {
      for (block_z = 0; write_ok && block_z < MAX_Z; block_z += 2) {
        file_buffer[cursor_pos++] =
          (u8) ((blockGet(block_x, block_y, block_z) << 4) |
            (blockGet(block_x, block_y, block_z + 1) & 0x0F));

        if (cursor_pos >= BUFFER_LEN) {
          write_ok = writePage(&file);
        }
      }
    }
  }

  for (trees_ptr = (u8 *) trees; write_ok && trees_ptr < trees_end;
      trees_ptr++) {
    file_buffer[cursor_pos++] = *trees_ptr;
    if (cursor_pos >= BUFFER_LEN) {
      write_ok = writePage(&file);
    }
  }
  if (write_ok && cursor_pos > 0) {
    while (cursor_pos < BUFFER_LEN) {
      file_buffer[cursor_pos++] = 0;
    }
    write_ok = writePage(&file);
  }

  if (write_ok) {
    write_ok = f_sync(&file) == FR_OK;
  }
  if (f_close(&file) != FR_OK) {
    write_ok = FALSE;
  }
  if (write_ok) {
    f_unlink(backup_file_names[slot]);
    if (had_previous_file &&
        f_rename(file_names[slot], backup_file_names[slot]) != FR_OK) {
      write_ok = FALSE;
    }
    if (write_ok &&
        f_rename(temporary_file_names[slot], file_names[slot]) != FR_OK) {
      if (had_previous_file) {
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
  return write_ok;
}

void loadGame() {
  FIL file;
  Header *header = (Header *) file_buffer;
  HeaderV8 *header_v8 = (HeaderV8 *) file_buffer;
  HeaderV7 *header_v7 = (HeaderV7 *) file_buffer;
  HeaderV3 *header_v3 = (HeaderV3 *) file_buffer;
  HeaderV2 *header_v2 = (HeaderV2 *) file_buffer;
  HeaderV1 *header_v1 = (HeaderV1 *) file_buffer;
  LegacyHeader *legacy_header = (LegacyHeader *) file_buffer;
  FRESULT result;
  u8 read_ok;
  u8 packed;
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
  u8 *trees_ptr;
  u32 tree_byte;
  u8 player_num;
  int block_x, block_y, block_z;

  if (game_file_num < 1 || game_file_num > 3) {
    initWorld();
    initPlayers();
    return;
  }
  result = f_open(&file, file_names[game_file_num - 1], FA_READ);
  if (result != FR_OK) {
    initWorld();
    initPlayers();
    files_present[game_file_num - 1] = FALSE;
    return;
  }
  read_ok = readPage(&file);
  if (!read_ok || legacy_header->magic_num != MAGIC_NUM ||
      legacy_header->version > SAVE_VERSION ||
      (legacy_header->version == SAVE_VERSION &&
       buffer_bytes_read < sizeof(Header)) ||
      (legacy_header->version == 8 &&
       buffer_bytes_read < sizeof(HeaderV8)) ||
      (legacy_header->version == 7 &&
       !savedWorldDimensionsMatch(header_v7->player)) ||
      (legacy_header->version == SAVE_VERSION &&
       !savedWorldDimensionsMatch(header->player)) ||
      (legacy_header->version == 8 &&
       !savedWorldDimensionsMatch(header_v8->player)) ||
      (legacy_header->version >= 1 &&
       ((legacy_header->version == SAVE_VERSION &&
         (header->player_count < 1 || header->player_count > MAX_PLAYERS)) ||
        (legacy_header->version == 8 &&
         (header_v8->player_count < 1 || header_v8->player_count > MAX_PLAYERS)) ||
        (legacy_header->version < SAVE_VERSION &&
         (header_v1->player_count < 1 ||
          header_v1->player_count > LEGACY_MAX_PLAYERS))))) {
    f_close(&file);
    if (recoverBackup(game_file_num - 1)) {
      loadGame();
      return;
    }
    discardInvalidCurrent(game_file_num - 1);
    initWorld();
    initPlayers();
    files_present[game_file_num - 1] = FALSE;
    return;
  }

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

  if (legacy_header->version == SAVE_VERSION) {
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
  } else if (legacy_header->version == 8) {
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
  } else if (legacy_header->version >= 4) {
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
    tree_records_saved = legacy_header->version >= 6;
    legacy_tree_records_saved = legacy_header->version == 5;
    cursor_pos = LEGACY_COOP_HEADER_SIZE;
  } else if (legacy_header->version >= 3 && header_v3->player_count == 2) {
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
  } else if (legacy_header->version >= 3) {
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
  } else if (legacy_header->version >= 2 && header_v2->player_count == 2) {
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
  } else if (legacy_header->version >= 2) {
    restorePlayerState(&players[0], header_v2->player[0].position,
      header_v2->player[0].pitch, header_v2->player[0].yaw,
      header_v2->player[0].held_block);
    wood_count[0] = header_v2->player[0].wood_count;
    cursor_pos = LEGACY_HEADER_PAGE_SIZE;
  } else if (legacy_header->version >= 1 && header_v1->player_count == 2) {
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
  } else if (legacy_header->version >= 1) {
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

  if (legacy_header->version < 8) {
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
    f_close(&file);
    if (recoverBackup(game_file_num - 1)) {
      loadGame();
      return;
    }
    discardInvalidCurrent(game_file_num - 1);
    initWorld();
    initPlayers();
    files_present[game_file_num - 1] = FALSE;
    return;
  }

  /* Every column of the saved extent needs a slot before blockSet will take
     the incoming terrain, and each one counts as fully built once it has.
     Saved worlds live entirely inside the fixed extent, so the render origin
     returns to zero with them. */
  windowClaimFixedExtent();
  worldMarkFixedExtentBuilt();
  graphicsSetRenderOrigin(0, 0);
  for (block_x = 0; read_ok && block_x < MAX_X; block_x++) {
    for (block_y = 0; read_ok && block_y < MAX_Y; block_y++) {
      for (block_z = 0; read_ok && block_z < MAX_Z; block_z += 2) {
        if (cursor_pos >= buffer_bytes_read) {
          if (!readPage(&file)) {
            read_ok = FALSE;
            break;
          }
        }

        packed = file_buffer[cursor_pos++];
        if (!BLOCK_IS_VALID(packed >> 4)) {
          packed &= 0x0F;
        }
        if (!BLOCK_IS_VALID(packed & 0x0F)) {
          packed &= 0xF0;
        }
        blockSet(block_x, block_y, block_z, packed >> 4);
        blockSet(block_x, block_y, block_z + 1, packed & 0x0F);
        if (full_inventory_saved) {
          computed_checksum = checksumByte(computed_checksum, packed >> 4);
          computed_checksum = checksumByte(computed_checksum, packed & 0x0F);
        }
      }
    }
  }

  if (tree_records_saved) {
    /* Only the frozen 96-record payload is on disk; the rest of the larger
       live pool stays inactive from initTrees. */
    for (trees_ptr = (u8 *) trees; trees_ptr < (u8 *) trees + TREE_SAVE_BYTES;
        trees_ptr++) {
      if (cursor_pos >= buffer_bytes_read) {
        if (!readPage(&file)) {
          read_ok = FALSE;
          break;
        }
      }
      *trees_ptr = file_buffer[cursor_pos++];
      computed_checksum = checksumByte(computed_checksum, *trees_ptr);
    }
  } else {
    if (legacy_tree_records_saved) {
      /* V5 files carried the original 96-record pool. */
      for (tree_byte = 0;
          tree_byte < sizeof(TreeRecordV5) * TREE_SAVE_COUNT; tree_byte++) {
        if (cursor_pos >= buffer_bytes_read) {
          if (!readPage(&file)) {
            read_ok = FALSE;
            break;
          }
        }
        computed_checksum = checksumByte(computed_checksum,
          file_buffer[cursor_pos++]);
      }
    }
    recoverTreesFromWorld();
  }

  f_close(&file);
  if (legacy_header->version == SAVE_VERSION) {
    computed_checksum = checksumWorldTicks(computed_checksum,
      header->world_ticks);
  }
  if (!read_ok ||
      (full_inventory_saved && computed_checksum != expected_checksum) ||
      (tree_records_saved && !treesValid())) {
    if (recoverBackup(game_file_num - 1)) {
      loadGame();
      return;
    }
    discardInvalidCurrent(game_file_num - 1);
    initWorld();
    initPlayers();
    files_present[game_file_num - 1] = FALSE;
  }
}
