// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long GDB_CORE_ADDR;

struct gdb_object;
struct gdb_symtab;
struct gdb_block;
struct gdb_unwind_callbacks;
struct gdb_reader_funcs;

struct gdb_line_mapping {
  int line;
  GDB_CORE_ADDR pc;
};

enum gdb_status {
  GDB_FAIL = 0,
  GDB_SUCCESS = 1,
};

struct gdb_frame_id {
  GDB_CORE_ADDR code_address;
  GDB_CORE_ADDR stack_address;
};

struct gdb_symbol_callbacks {
  struct gdb_object* (*object_open)(struct gdb_symbol_callbacks* self);
  void (*object_close)(struct gdb_symbol_callbacks* self, struct gdb_object* object);
  struct gdb_symtab* (*symtab_open)(struct gdb_symbol_callbacks* self, struct gdb_object* object, const char* name);
  void (*symtab_close)(struct gdb_symbol_callbacks* self, struct gdb_symtab* symtab);
  struct gdb_block* (*block_open)(struct gdb_symbol_callbacks* self, struct gdb_symtab* symtab, struct gdb_block* parent,
                                  GDB_CORE_ADDR start, GDB_CORE_ADDR end, const char* name);
  void (*block_close)(struct gdb_symbol_callbacks* self, struct gdb_block* block);
  void (*line_mapping_add)(struct gdb_symbol_callbacks* self, struct gdb_symtab* symtab, int nlines,
                           struct gdb_line_mapping* lines);
};

struct gdb_reader_funcs {
  int reader_version;
  void* priv_data;
  enum gdb_status (*read)(struct gdb_reader_funcs* self, struct gdb_symbol_callbacks* callbacks, void* memory, long memory_sz);
  enum gdb_status (*unwind)(struct gdb_reader_funcs* self, struct gdb_unwind_callbacks* callbacks);
  struct gdb_frame_id (*get_frame_id)(struct gdb_reader_funcs* self, struct gdb_unwind_callbacks* callbacks);
  void (*destroy)(struct gdb_reader_funcs* self);
};

#define GDB_READER_INTERFACE_VERSION 1
#define GDB_DECLARE_GPL_COMPATIBLE_READER extern int plugin_is_GPL_compatible

#ifdef __cplusplus
}
#endif
