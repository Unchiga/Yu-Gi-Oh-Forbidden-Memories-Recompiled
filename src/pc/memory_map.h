#ifndef MEMORIES_PC_MEMORY_MAP_H
#define MEMORIES_PC_MEMORY_MAP_H
/* Console address constants shared by native adapters and the IR compiler.
 * This header contains no host types or layouts, including in the mod SDK. */
#define MEMORIES_RAM_SIZE 0x200000u
#define MEMORIES_SCRATCHPAD_SIZE 0x400u
#define MEMORIES_GUEST_RAM 0x80000000u
#define MEMORIES_GUEST_RAM_UNCACHED 0xa0000000u
#define MEMORIES_GUEST_RAM_SIZE MEMORIES_RAM_SIZE
#define MEMORIES_GUEST_SCRATCHPAD 0x9f800000u
#define MEMORIES_GUEST_SCRATCHPAD_RETAIL 0x1f800000u
#define MEMORIES_GUEST_SCRATCHPAD_SIZE MEMORIES_SCRATCHPAD_SIZE
#define MEMORIES_GUEST_PHYSICAL_MASK 0x1fffffffu
#define MEMORIES_GUEST_PHYSICAL_END 0x20000000u
#define MEMORIES_GUEST_DIRECT_END 0xc0000000u
/* Guest tokens are below MEMORIES_GUEST_PHYSICAL_END or from
 * MEMORIES_GUEST_RAM up, never between: where a host maps its own memory
 * below 4 GiB (the Switch: Horizon places a process's stacks and heap
 * anywhere in its 39-bit space), addresses in this window are the host's. */
#define MEMORIES_HOST_LOW_START MEMORIES_GUEST_PHYSICAL_END
#define MEMORIES_HOST_LOW_END MEMORIES_GUEST_RAM
#endif
