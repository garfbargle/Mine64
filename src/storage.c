#include "cart.h"
#include "ff/ff.h"

#include "storage.h"
#include "blocks.h"
#include "player.h"
#include "world.h"

static FATFS fs;

// "MI64"
#define MAGIC_NUM 0x4D493634
#define BUFFER_LEN 128
#define SAVE_VERSION 2

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
  u32 held_block;
  u32 wood_count;
} SavedPlayer;

typedef struct {
  u32 magic_num;
  u32 version;
  u32 file_num;
  u32 save_count;
  u32 player_count;
  SavedPlayer player[MAX_PLAYERS];
} Header;

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
  SavedPlayerV1 player[MAX_PLAYERS];
} HeaderV1;

static u32 save_count = 0;

static u8 file_buffer[BUFFER_LEN];
static int cursor_pos = 0;

u8 saving_available;
u8 files_present[3];
u32 game_file_num;

static char *file_names[] = {
  "mine64/world_1.m64",
  "mine64/world_2.m64",
  "mine64/world_3.m64"
};

void initStorage() {
  FRESULT res;
  FILINFO info;
  int i;

  saving_available = cart_init() == 0;

  res = f_mount(&fs, "", 1);
  res = f_stat("mine64", &info);

  if (res == FR_NO_FILE) {
    res = f_mkdir("mine64");
  } else {
    res = info.fattrib & AM_DIR;
  }

  for (i = 0; i < 3; i++) {
    res = f_stat(file_names[i], &info);
    files_present[i] = res == FR_OK;
  }
}

void readPage(FIL *file, u32 page_num) {
  UINT n_read;

  osInvalDCache(file_buffer, BUFFER_LEN);
  f_read(file, file_buffer, BUFFER_LEN, &n_read);
  
  cursor_pos = 0;
}

void writePage(FIL *file, u32 page_num) {
  UINT written;
  cursor_pos = 0;

  osWritebackDCache(file_buffer, BUFFER_LEN);
  f_write(file, file_buffer, BUFFER_LEN, &written);
}

void saveGame() {
  FIL file;
  Header *header = (Header *) file_buffer;
  int page_num = 0;
  u8 packed;
  u8 *blocks_ptr;
  const u8 *blocks_end = blocks + NUM_BLOCKS;

  f_open(&file, file_names[game_file_num - 1], FA_WRITE | FA_CREATE_ALWAYS);

  save_count++;

  header->magic_num = MAGIC_NUM;
  header->version = SAVE_VERSION;
  header->file_num = game_file_num;
  header->save_count = save_count;
  header->player_count = active_player_count;
  header->player[0].position = players[0].position;
  header->player[0].pitch = players[0].pitch;
  header->player[0].yaw = players[0].yaw;
  header->player[0].held_block = players[0].held_block;
  header->player[0].wood_count = players[0].inventory[
    INVENTORY_HOTBAR_START + (WOOD - FIRST_PLACEABLE_BLOCK)].count;
  header->player[1].position = players[1].position;
  header->player[1].pitch = players[1].pitch;
  header->player[1].yaw = players[1].yaw;
  header->player[1].held_block = players[1].held_block;
  header->player[1].wood_count = players[1].inventory[
    INVENTORY_HOTBAR_START + (WOOD - FIRST_PLACEABLE_BLOCK)].count;

  cursor_pos = sizeof(Header);
  while (cursor_pos < BUFFER_LEN) {
    file_buffer[cursor_pos++] = 0;
  }
  writePage(&file, page_num++);

  for (blocks_ptr = blocks; blocks_ptr < blocks_end; blocks_ptr += 2) {
    packed = (blocks_ptr[0] << 4) | blocks_ptr[1];
    file_buffer[cursor_pos++] = packed;

    if (cursor_pos >= BUFFER_LEN) {
      writePage(&file, page_num++);
    }
  }

  f_close(&file);
}

void loadGame() {
  FIL file;
  Header *header = (Header *) file_buffer;
  HeaderV1 *header_v1 = (HeaderV1 *) file_buffer;
  LegacyHeader *legacy_header = (LegacyHeader *) file_buffer;
  int page_num = 0;
  u8 packed;
  u8 wood_count[2] = {0, 0};
  u8 *blocks_ptr;
  const u8 *blocks_end = blocks + NUM_BLOCKS;

  f_open(&file, file_names[game_file_num - 1], FA_READ);
  readPage(&file, page_num++);

  save_count = legacy_header->save_count;
  players[0].position = legacy_header->position;
  players[0].pitch = legacy_header->pitch;
  players[0].yaw = legacy_header->yaw;
  players[0].body_yaw = players[0].yaw;
  players[0].walk_time = 0;
  players[0].walk_swing = 0;
  players[0].held_block = legacy_header->held_block;
  players[0].y_velocity = 0;
  players[0].active = TRUE;
  players[0].target_present = FALSE;
  players[0].breaking = FALSE;
  players[0].break_progress = 0;
  players[1].active = FALSE;
  players[1].target_present = FALSE;
  players[1].breaking = FALSE;
  players[1].break_progress = 0;
  active_player_count = 1;

  if (legacy_header->version >= SAVE_VERSION && header->player_count == 2) {
    players[0].position = header->player[0].position;
    players[0].pitch = header->player[0].pitch;
    players[0].yaw = header->player[0].yaw;
    players[0].body_yaw = players[0].yaw;
    players[0].walk_time = 0;
    players[0].walk_swing = 0;
    players[0].held_block = header->player[0].held_block;
    wood_count[0] = header->player[0].wood_count > MAX_ITEM_STACK ?
      MAX_ITEM_STACK : header->player[0].wood_count;
    players[1].position = header->player[1].position;
    players[1].pitch = header->player[1].pitch;
    players[1].yaw = header->player[1].yaw;
    players[1].body_yaw = players[1].yaw;
    players[1].walk_time = 0;
    players[1].walk_swing = 0;
    players[1].held_block = header->player[1].held_block;
    wood_count[1] = header->player[1].wood_count > MAX_ITEM_STACK ?
      MAX_ITEM_STACK : header->player[1].wood_count;
    players[1].y_velocity = 0;
    players[1].active = TRUE;
    active_player_count = 2;
    cursor_pos = sizeof(Header);
  } else if (legacy_header->version >= SAVE_VERSION) {
    players[0].position = header->player[0].position;
    players[0].pitch = header->player[0].pitch;
    players[0].yaw = header->player[0].yaw;
    players[0].body_yaw = players[0].yaw;
    players[0].walk_time = 0;
    players[0].walk_swing = 0;
    players[0].held_block = header->player[0].held_block;
    wood_count[0] = header->player[0].wood_count > MAX_ITEM_STACK ?
      MAX_ITEM_STACK : header->player[0].wood_count;
    cursor_pos = sizeof(Header);
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
    cursor_pos = sizeof(HeaderV1);
  } else if (legacy_header->version >= 1) {
    players[0].position = header_v1->player[0].position;
    players[0].pitch = header_v1->player[0].pitch;
    players[0].yaw = header_v1->player[0].yaw;
    players[0].body_yaw = players[0].yaw;
    players[0].walk_time = 0;
    players[0].walk_swing = 0;
    players[0].held_block = header_v1->player[0].held_block;
    cursor_pos = sizeof(HeaderV1);
  } else {
    cursor_pos = sizeof(LegacyHeader);
  }

  resetPlayerInventory(&players[0]);
  players[0].inventory[INVENTORY_HOTBAR_START + (WOOD - FIRST_PLACEABLE_BLOCK)].count = wood_count[0];
  if (players[1].active) {
    resetPlayerInventory(&players[1]);
    players[1].inventory[INVENTORY_HOTBAR_START + (WOOD - FIRST_PLACEABLE_BLOCK)].count = wood_count[1];
  }

  for (blocks_ptr = blocks; blocks_ptr < blocks_end; blocks_ptr += 2) {
    if (cursor_pos >= BUFFER_LEN) {
      readPage(&file, page_num++);
    }

    packed = file_buffer[cursor_pos++];
    blocks_ptr[0] = packed >> 4;
    blocks_ptr[1] = packed & 0xF;
  }

  f_close(&file);
}
