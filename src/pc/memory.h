#ifndef MEMORIES_PC_MEMORY_H
#define MEMORIES_PC_MEMORY_H
#include <stddef.h>
#include <stdint.h>

#include "memory_map.h"

/* Guest-layout storage for adapters/fixtures, not a host address-space mapping.
 * MMIO, BIOS, overlay execution and RAM mirrors above 2 MiB are not supported. */
typedef struct MemoriesMemory {
    _Alignas(16) uint8_t ram[MEMORIES_RAM_SIZE];
    _Alignas(16) uint8_t scratchpad[MEMORIES_SCRATCHPAD_SIZE];
} MemoriesMemory;

/* Return NULL for an invalid span/alignment, even if only its final byte is bad.
 * Zero-length spans are allowed only at addresses inside a supported region. */
void *Memories_Resolve(MemoriesMemory *memory, uint32_t address,
                       size_t length, size_t alignment);
uint32_t Memories_ReadLE32(const uint8_t *bytes);
void Memories_WriteLE32(uint8_t *bytes, uint32_t value);

/* The mods' packet area (the mod API's packet_area, mod_packets.c): memory
 * past guest RAM's 8 MiB of mirrors but inside the 24 bits an ordering-table
 * link holds, at physical MEMORIES_MOD_PACKETS_FIRST.., mapped at the guest's
 * direct address for it only when a mod first asks (src/pc/mods). A mod's
 * polygons go there instead of the game's 140 KB frame buffer
 * (GRAPHICS_PACKET_BUFFER_SIZE); the GPU's walker (render/packets.c) and
 * Memories_Resolve (guest/resolve.c) take its links. Handed out per frame (Memories_TakeModPackets)
 * and free again once DrawOTag has read the frame's list
 * (Memories_ModPacketsFrame). Never mapped in a translated build. */
#define MEMORIES_MOD_PACKETS_FIRST 0x00800000u
#define MEMORIES_MOD_PACKETS_LAST 0x01000000u /* the 24-bit link space's end */
extern uint8_t *Memories_ModPacketBase;
extern uint32_t Memories_ModPacketPhysical, Memories_ModPacketSize;
void Memories_SetModPackets(void *base, uint32_t physical, uint32_t size);
/* `bytes` of this frame's area, 4-aligned; NULL when there is none or no room. */
void *Memories_TakeModPackets(size_t bytes);
void Memories_ModPacketsFrame(void);
#endif
