/*
 * uf2flash -- UF2 files into a 16 MB flash image, as the RP2350's BOOTSEL
 * drive would write them.
 *
 *   uf2flash FLASH.img FILE.uf2 [FILE.uf2 ...]
 *
 * The image is created (all 0xFF, erased) if it does not exist, else only
 * the 4 KB sectors the UF2 touches change: the bootloader erases each
 * sector it writes, the rest of the flash (games on the drive, settings)
 * stays. Blocks for the RP2350 in ARM mode (family 0xE48BFF59), "data"
 * blocks (0xE48BFF58, game data such as Tyrian's or Doom's WAD) and
 * "absolute" blocks (0xE48BFF57) are taken, others (RP2040, RISC-V)
 * skipped. The single absolute block picotool puts at 0x10FFFF00 (a
 * workaround for erratum RP2350-E10) is not flash content and skipped too.
 *
 * MIT License, part of PBConPC.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLASH_SIZE   (16u * 1024 * 1024)
#define FLASH_BASE   0x10000000u
#define SECTOR       4096u

#define UF2_MAGIC0   0x0A324655u
#define UF2_MAGIC1   0x9E5D5157u
#define UF2_MAGIC_END 0x0AB16F30u
#define UF2_FLAG_NOT_MAIN_FLASH 0x00000001u
#define UF2_FLAG_FAMILY 0x00002000u

#define FAMILY_RP2350_ARM_S 0xE48BFF59u
#define FAMILY_ABSOLUTE     0xE48BFF57u
#define FAMILY_DATA         0xE48BFF58u
#define E10_BLOCK_ADDR      0x10FFFF00u

typedef struct {
    uint32_t magic0, magic1, flags, addr, size, block, nblocks, family;
    uint8_t data[476];
    uint32_t magic_end;
} uf2_block_t;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "uf2flash FLASH.img FILE.uf2 [FILE.uf2 ...]\n");
        return 2;
    }
    static uint8_t flash[FLASH_SIZE];
    static uint8_t touched[FLASH_SIZE / SECTOR];
    memset(flash, 0xFF, sizeof flash);
    FILE *f = fopen(argv[1], "rb");
    if (f) {
        size_t n = fread(flash, 1, sizeof flash, f);
        fclose(f);
        if (n != sizeof flash) {
            fprintf(stderr, "%s: %zu bytes, not a 16 MB image\n", argv[1], n);
            return 1;
        }
    }

    for (int a = 2; a < argc; a++) {
        FILE *u = fopen(argv[a], "rb");
        if (!u) { perror(argv[a]); return 1; }
        uint8_t raw[512];
        unsigned taken = 0, skipped = 0;
        uint32_t lo = UINT32_MAX, hi = 0;
        while (fread(raw, 1, sizeof raw, u) == sizeof raw) {
            uf2_block_t b;
            b.magic0 = rd32(raw); b.magic1 = rd32(raw + 4); b.flags = rd32(raw + 8);
            b.addr = rd32(raw + 12); b.size = rd32(raw + 16);
            b.family = rd32(raw + 28); b.magic_end = rd32(raw + 508);
            if (b.magic0 != UF2_MAGIC0 || b.magic1 != UF2_MAGIC1 || b.magic_end != UF2_MAGIC_END) {
                fprintf(stderr, "%s: not a UF2 block\n", argv[a]);
                return 1;
            }
            int ok = !(b.flags & UF2_FLAG_NOT_MAIN_FLASH) && b.size <= 476 &&
                     (!(b.flags & UF2_FLAG_FAMILY) || b.family == FAMILY_RP2350_ARM_S ||
                      b.family == FAMILY_ABSOLUTE || b.family == FAMILY_DATA) &&
                     !(b.family == FAMILY_ABSOLUTE && b.addr == E10_BLOCK_ADDR) &&
                     b.addr >= FLASH_BASE && b.addr + b.size <= FLASH_BASE + FLASH_SIZE;
            if (!ok) { skipped++; continue; }
            uint32_t off = b.addr - FLASH_BASE;
            /* the sector is erased before its first block lands */
            for (uint32_t s = off / SECTOR; s <= (off + b.size - 1) / SECTOR; s++)
                if (!touched[s]) {
                    memset(flash + s * SECTOR, 0xFF, SECTOR);
                    touched[s] = 1;
                }
            memcpy(flash + off, raw + 32, b.size);
            if (b.addr < lo) lo = b.addr;
            if (b.addr + b.size > hi) hi = b.addr + b.size;
            taken++;
        }
        fclose(u);
        printf("%s: %u blocks written (0x%08X..0x%08X), %u skipped\n",
               argv[a], taken, taken ? lo : 0, taken ? hi : 0, skipped);
    }

    f = fopen(argv[1], "wb");
    if (!f || fwrite(flash, 1, sizeof flash, f) != sizeof flash) {
        perror(argv[1]);
        return 1;
    }
    fclose(f);
    printf("vector table: SP 0x%08X, reset 0x%08X\n", rd32(flash), rd32(flash + 4));
    return 0;
}
