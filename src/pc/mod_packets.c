/* The mods' packet area (memory.h): where it is once a mod asked for it
 * (src/pc/mods, packet_area), and the frame's share of it handed out so far.
 * Its own unit, linked wherever the GPU's walker is (build_config.py's
 * common files). Only the fixed-address builds map it, and resolve its
 * addresses (guest/resolve.c); the translated ones never have one. */
#include "memory.h"

uint8_t *Memories_ModPacketBase;
uint32_t Memories_ModPacketPhysical, Memories_ModPacketSize;
static uint32_t used;

void Memories_SetModPackets(void *base, uint32_t physical, uint32_t size)
{
    Memories_ModPacketBase = base;
    Memories_ModPacketPhysical = physical;
    Memories_ModPacketSize = base ? size : 0;
    used = 0;
}

void *Memories_TakeModPackets(size_t bytes)
{
    uint8_t *at;
    bytes = (bytes + 3u) & ~(size_t)3u;
    if (!Memories_ModPacketBase || !bytes || bytes > Memories_ModPacketSize - used) return NULL;
    at = Memories_ModPacketBase + used;
    used += (uint32_t)bytes;
    return at;
}

void Memories_ModPacketsFrame(void)
{
    used = 0;
}
