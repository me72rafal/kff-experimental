/*
 * Copyright (c) 2019-2024 Kim Jørgensen and Holger Gryska
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

static u8  dts_ff;       // bit 0: GAME, bit 1: EXROM/A13

static u32 const dts_mode[4] =
{
    C64_GAME_LOW |C64_EXROM_LOW,
    C64_GAME_HIGH|C64_EXROM_LOW,
    C64_GAME_LOW |C64_EXROM_HIGH,
    C64_GAME_HIGH|C64_EXROM_HIGH
};

/******************************************************************************
* C64 bus read callback (early VIC-II cycle)
******************************************************************************/
FORCE_INLINE u32 dts_early_vic_handler(u32 addr)
{
    // Speculative read
    return crt_ptr[((u32)(dts_ff & 2) << 12) + (addr & 0x1fff)];
}

/******************************************************************************
* C64 bus read callback (VIC-II cycle)
******************************************************************************/
FORCE_INLINE bool dts_vic_read_handler(u32 control, u32 data)
{
    if ((control & (C64_ROML|C64_ROMH)) != (C64_ROML|C64_ROMH))
    {
        C64_DATA_WRITE(data);
        return true;
    }

    return false;
}

/******************************************************************************
* C64 bus read callback
******************************************************************************/
FORCE_INLINE bool dts_read_handler(u32 control, u32 addr)
{
    C64_CRT_CONTROL(STATUS_LED_OFF);
    if ((control & (C64_ROML|C64_ROMH)) != (C64_ROML|C64_ROMH))
    {
        C64_DATA_WRITE(crt_ptr[((u32)(dts_ff & 2) << 12) + (addr & 0x1fff)]);
        return true;
    }

    return false;
}

/******************************************************************************
* C64 bus write callback
******************************************************************************/
FORCE_INLINE void dts_write_handler(u32 control, u32 addr, u32 data)
{
    if (!(control & C64_IO1))
    {
        dts_ff = data & 0x03;
        C64_CRT_CONTROL(dts_mode[dts_ff]);
    }
}

static void dts_init(void)
{
    dts_ff = 0x02;
    C64_CRT_CONTROL(dts_mode[dts_ff]);
}

// Support MAX cartridges where the VIC-II reads character and sprite data
// directly from the cartridge
C64_VIC_BUS_HANDLER(dts)
