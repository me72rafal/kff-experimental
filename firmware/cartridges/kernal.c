/*
 * Copyright (c) 2019-2025 Kim Jørgensen and Holger Gryska
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
 *
 * 1.10 (gcc-arm-none-eabi-10.3)   tested on PAL  250407, 250425, 250466 and 250469 with original PLA
 *                                           PAL  250407 with XCPLA, N82S100N and GAL20V8B PLA
 *                                           NTSC 250407 with original and EPROM PLA
 * each GPIO str access takes 21.5 ns even with non-load/store instruction inbetween
 */

//GPIOA base address register offsets (maximum 0xFFF)
#define GPIOA_OTYPER             0x04                               // RESET, IRQ, NMI, DMA, GAME, EXROM
#define GPIOA_BSRR               0x18
#define GPIOB_IDR                GPIOB_BASE-GPIOA_BASE+0x10         // R/W, IO1, IO2, BA, ROML, ROMH
//GPIOD base address register offsets
#define GPIOD_MODER              0x00                               // D0 - D8
#define GPIOD_ODR                0x14
#define GPIOE_MODER              GPIOE_BASE-GPIOD_BASE              // A0 - A15
#define GPIOE_IDR                GPIOE_BASE-GPIOD_BASE+0x10

static u32 kernal_check;         // 0: HIRAM detection, 1: HIRAM L and no detection, 2: HIRAM H and no detection
static u32 gpioa_config;         // GPIOA output configuration

/******************************************************************************
* C64 bus write callback (CPU cycle)
******************************************************************************/
FORCE_INLINE void kernal_write_handler(u32 control, u32 addr, u32 data)
{
    if (addr <= 0x0001) {                                           // if $0000 or $0001 write access
        kernal_check = 0;                                           //   enable HIRAM detection
    }
}

static void kernal_init(void)
{
    kernal_check = 2;                                               // enable ROM read without HIRAM detection for PLA identification
    crt_rom_ptr = dat_buffer + 0x2000 - 0xe000;                        // KFF2 stores ROMH in 16k banks with 8k offset - C64 address offset
    GPIOE->ODR = 0;                                                 // prepare outputs on address bus as zero
    MODIFY_REG(GPIOE->OSPEEDR, 1<<14, 1<<14);                       // set address bus output speed to medium (01)
    C64_CRT_CONTROL(STATUS_LED_OFF|CRT_PORT_NONE);
}

__attribute__((optimize("Os")))                                                 
static void kernalo_handler(void)                                   // timing for original PLA
{                                                                               
    register u32 hiram      asm("r5");                              // registers used for constant in WAIT_UNTIL()
    register u32 cpu_end    asm("r6");

    register u32 control    asm("r8");                              // registers used for operands in functional calls
    register u32 addr       asm("r9");

    register u32 gpioa_base asm("r10") = GPIOA_BASE;                // GPIOA base address (C64 CONTROL)
    register u32 gpiod_base asm("r11") = GPIOD_BASE;                // GPIOD base address (C64 ADDR and DATA)
    register u32 dwt_base   asm("r12") = DWT_BASE;                  // DWT base address (CYCCNT register address offset #4) */

    hiram   = phi2_hiram;
    cpu_end = phi2_cpu_end;
    
    while (true)                                                                
    {                                                                           
        DWT->CYCCNT = DWT->COMP3 + TIM1->CNT;                       // use debug cycle counter which is faster to access than timer        
        COMPILER_BARRIER();
        gpioa_config = GPIOA->OTYPER;                               // load GPIOA configuration
        asm (
            ".balign 4 \n\t"
            "wait_%=: \n\t"                                         // WAIT_UNTIL(PAL_PHI2_HIRAM);
            "ldr  r7, [%[dwt_base], #4] \n\t"                       // C64 address & control stable 170 ns after PHI2 L->H (35 ns jitter)
            "cmp  r7, %[hiram] \n\t"
            "bcc  wait_%= \n\t"

            "ldr  %[control], [%[gpioa_base], %[gpiob_idr]] \n\t"   // control = C64_CONTROL_READ(); BA & R/W valid 30 ns after PHI2 L->H
            "ldr  %[addr], [%[gpiod_base], %[gpioe_idr]] \n\t"      // addr = C64_ADDR_READ(); valid 45 ns after PHI2 L->H
            :  [control] "+r" (control), [addr] "+r" (addr)
            :  [kernal_rom] "r" (crt_rom_ptr), [kernal_check] "r" (kernal_check), [gpioa_config] "r" (gpioa_config),
               [dwt_base] "r" (dwt_base), [hiram] "r" (hiram),
               [gpioa_base] "r" (gpioa_base), [gpioa_bsrr] "i" (GPIOA_BSRR), [gpiob_idr] "i" (GPIOB_IDR),
               [gpiod_base] "r" (gpiod_base), [gpioe_idr] "i" (GPIOE_IDR)
            : "r7", "cc" );

        asm goto (                                                  // no output operands supported for goto in gcc-arm-none-eabi-10.3
            "tst  %[control], %[c64_write] \n\t"                    // if ((control & C64_WRITE) &&
            "beq  %l[kernalo_write] \n\t"
            "tst  %[control], %[c64_ba] \n\t"                       //    (control & C64_BA) &&
            "beq  %l[kernalo_vic] \n\t"
            "cmp  %[addr], #0xe000 \n\t"                            //    (addr >= 0xe000) &&
            "bcc  %l[kernalo_vic] \n\t"
            "cbnz %[kernal_check], kernalo_no_check \n\t"           //    (kernal_check == 0))
            :: [control] "r" (control), [addr] "r" (addr), [kernal_check] "r" (kernal_check),
               [kernal_rom] "r" (crt_rom_ptr), [gpioa_config] "r" (gpioa_config),
               [dwt_base] "r" (dwt_base), [gpioa_base] "r" (gpioa_base), [gpiod_base] "r" (gpiod_base),
               [c64_write] "i" (C64_WRITE), [c64_ba] "i" (C64_BA)
            : "cc" : kernalo_write, kernalo_vic );
        
        // kernal_read: kernal_check must modify address bus earliest 280ns after phi2 L->H for the A14 hack
        asm (
            "ldrb r7, [%[kernal_rom], %[addr]] \n\t"                // speculatively read kernal data \n\t"
            "strb r7, [%[gpiod_base], %[gpiod_odr]] \n\t"
            "mov  r7, %[c64_game_exrom_low] \n\t"
            "str  r7, [%[gpioa_base], %[gpioa_bsrr]] \n\t"          // GAME and EXROM H->L 260-295 ns after PHI2 L->H (16k CRT mode)
            "mov  r7, #1<<(14*2) \n\t"
            "str  r7, [%[gpiod_base], %[gpioe_moder]] \n\t"         // C64 address line A14 regular output L 22 ns after EXROM H->L
            "bic  %[gpioa_config], %[gpioa_config], %[c64_game_exrom_high] \n\t"
            "str  %[gpioa_config], [%[gpioa_base], %[gpioa_otyper]] \n\t" // change GAME and EXROM outputs to push-pull for faster rise time
            "mov  r7, #0 \n\t"
            "str  r7, [%[gpiod_base], %[gpioe_moder]] \n\t"         // C64 address line A14 input again after 20 or 40 ns
            "ldr  %[control], [%[gpioa_base], %[gpiob_idr]] \n\t"   // control = C64_CONTROL_READ(); valid 25-35 ns after A14 H->L
            "tst  %[control], %[c64_romh] \n\t"                     // test ROMH
            "bne  kernalo_hiram_low \n\t"

            "kernalo_hiram_high: \n\t"                              // Kernal read
            "mov  r7, %[c64_exrom_high] \n\t"
            "str  r7, [%[gpioa_base], %[gpioa_bsrr]] \n\t"          // EXROM L->H 130 ns after EXROM H->L (Ultimax CRT mode)
            "movw r7, #0x5555 \n\t"
            "strh r7, [%[gpiod_base]] \n\t"                         // enable C64 data bus latest 100 ns before PHI2 H->L
            "orr  %[gpioa_config], %[gpioa_config], %[c64_exrom_high] \n\t"
            "str  %[gpioa_config], [%[gpioa_base], %[gpioa_otyper]] \n\t"  // change EXROM output back to open-drain
            "mov  %[kernal_check], #2 \n\t"                         // disable HIRAM detection and store HIRAM state
            "b    kernalo_read \n\t"
            
            "kernalo_hiram_low: \n\t"                               // RAM read
            "mov  r7, %[c64_game_exrom_high] \n\t"
            "str  r7, [%[gpioa_base], %[gpioa_bsrr]] \n\t"          // GAME and EXROM L->H (none CRT mode for RAM access)
            "orr  %[gpioa_config], %[gpioa_config], %[c64_game_exrom_high] \n\t"
            "str  %[gpioa_config], [%[gpioa_base], %[gpioa_otyper]] \n\t" // change GAME and EXROM output back to open-drain
            "mov  %[kernal_check], #1 \n\t"                         // disable HIRAM detection and store HIRAM state
            "b    kernalo_read_end \n\t"
          
            "kernalo_no_check: \n\t"
            "cmp  %[kernal_check], #1 \n\t"                         // if (HIRAM L) skip for RAM read, else
            "beq  kernalo_read_end \n\t"
            "mov  r7, %[c64_game_low] \n\t"
            "str  r7, [%[gpioa_base], %[gpioa_bsrr]] \n\t"          //   GAME H->L 260 ns after PHI2 L->H (Ultimax CRT mode)
            "bic  %[gpioa_config], %[gpioa_config], %[c64_game_high] \n\t"
            "str  %[gpioa_config], [%[gpioa_base], %[gpioa_otyper]] \n\t" // change GAME output to push-pull for faster rise time
            "ldrb r7, [%[kernal_rom], %[addr]] \n\t"                //   read kernal data \n\t"
            "strb r7, [%[gpiod_base], %[gpiod_odr]] \n\t"
            "movw r7, #0x5555 \n\t"
            "strh r7, [%[gpiod_base]] \n\t"                         //   enable C64 data bus latest 100 ns before PHI2 H->L
            
            "kernalo_read: \n\t"
            "wait_%=: \n\t"                                         // WAIT_UNTIL(PAL_PHI2_CPU_END);
            "ldr  r7, [%[dwt_base], #4] \n\t"
            "cmp  r7, %[cpu_end] \n\t"
            "bcc  wait_%= \n\t"

            "mov  r7, #0 \n\t"
            "strh r7, [%[gpiod_base]] \n\t"                         // release C64 data bus 10-20 ns after PHI2 H->L
            "mov  r7, %[c64_game_high] \n\t"
            "str  r7, [%[gpioa_base], %[gpioa_bsrr]] \n\t"          // GAME L->H 30-42 ns after PHI2 H->L (none CRT mode)
            "orr  %[gpioa_config], %[gpioa_config], %[c64_game_exrom_high] \n\t"
            "str  %[gpioa_config], [%[gpioa_base], %[gpioa_otyper]] \n\t"  // change GAME and EXROM output to back to open-drain
            "kernalo_read_end: "
            : [kernal_check] "+r" (kernal_check)
            : [addr] "r" (addr), [control] "r" (control), [kernal_rom] "r" (crt_rom_ptr), 
              [dwt_base] "r" (dwt_base), [gpioa_config] "r" (gpioa_config), [cpu_end] "r" (cpu_end),
              [gpioa_base] "r" (gpioa_base), [gpioa_otyper] "i" (GPIOA_OTYPER), [gpioa_bsrr] "i" (GPIOA_BSRR), [gpiob_idr] "i" (GPIOB_IDR),
              [gpiod_base] "r" (gpiod_base), [gpioe_idr] "i" (GPIOE_IDR), [gpiod_odr] "i" (GPIOD_ODR), [gpioe_moder] "i" (GPIOE_MODER),
              [c64_game_high] "i" (C64_GAME_HIGH), [c64_exrom_high] "i" (C64_EXROM_HIGH), [c64_game_exrom_high] "i" (C64_GAME_HIGH|C64_EXROM_HIGH),
              [c64_game_low] "i" (C64_GAME_LOW), [c64_game_exrom_low] "i" (C64_GAME_LOW|C64_EXROM_LOW), [c64_romh] "i" (C64_ROMH)
            : "r7", "cc" );
        goto kernalo_vic;

    kernalo_write:
        WAIT_UNTIL(phi2_cpu_start);                                 // read C64 data bus 230 ns after PHI2 L->H
        u32 data = C64_DATA_READ();
        kernal_write_handler(control, addr, data);

    kernalo_vic:
        /*if ((control & (C64_RESET|MENU_BTN)) != C64_RESET) {                                                                       
           break;                                                   // allow reset or menu button interrupt handler to run
        }*/
        WAIT_UNTIL(phi2_vic_start);
        
        COMPILER_BARRIER();                                                     
    }                                                                           
    C64_INTERFACE_DISABLE();                                                    
}

__attribute__((optimize("Os")))                                                 
static void kernalf_handler(void)                                   // Timing for fast replacement PLA
{                                                                               
    register u32 hiram      asm("r5");                              // registers used for constant in WAIT_UNTIL()
    register u32 cpu_end    asm("r6");

    register u32 control    asm("r8");                              // registers used for operands in functional calls
    register u32 addr       asm("r9");

    register u32 gpioa_base asm("r10") = GPIOA_BASE;                // GPIOA base address (C64 CONTROL)
    register u32 gpiod_base asm("r11") = GPIOD_BASE;                // GPIOD base address (C64 ADDR and DATA)
    register u32 dwt_base   asm("r12") = DWT_BASE;                  // DWT base address (CYCCNT register address offset #4) */

    hiram   = phi2_hiram;
    cpu_end = phi2_cpu_end;
    
    while (true)                                                                
    {                                                                           
        DWT->CYCCNT = DWT->COMP3 + TIM1->CNT;                       // use debug cycle counter which is faster to access than timer        
        COMPILER_BARRIER();
        gpioa_config = GPIOA->OTYPER;                               // load GPIOA configuration
        asm (
            ".balign 4 \n\t"
            "wait_%=: \n\t"                                         // WAIT_UNTIL(PAL_PHI2_HIRAM);
            "ldr  r7, [%[dwt_base], #4] \n\t"                       // C64 address & control stable 170 ns after PHI2 L->H (35 ns jitter)
            "cmp  r7, %[hiram] \n\t"
            "bcc  wait_%= \n\t"

            "ldr  %[control], [%[gpioa_base], %[gpiob_idr]] \n\t"   // control = C64_CONTROL_READ(); BA & R/W valid 30 ns after PHI2 L->H
            "ldr  %[addr], [%[gpiod_base], %[gpioe_idr]] \n\t"      // addr = C64_ADDR_READ(); valid 45 ns after PHI2 L->H
            :  [control] "+r" (control), [addr] "+r" (addr)
            :  [kernal_rom] "r" (crt_rom_ptr), [kernal_check] "r" (kernal_check), [gpioa_config] "r" (gpioa_config),
               [dwt_base] "r" (dwt_base), [hiram] "r" (hiram),
               [gpioa_base] "r" (gpioa_base), [gpioa_bsrr] "i" (GPIOA_BSRR), [gpiob_idr] "i" (GPIOB_IDR),
               [gpiod_base] "r" (gpiod_base), [gpioe_idr] "i" (GPIOE_IDR)
            : "r7", "cc" );

        asm goto (                                                  // no output operands supported for goto in gcc-arm-none-eabi-10.3
            "tst  %[control], %[c64_write] \n\t"                    // if ((control & C64_WRITE) &&
            "beq  %l[kernalf_write] \n\t"
            "tst  %[control], %[c64_ba] \n\t"                       //    (control & C64_BA) &&
            "beq  %l[kernalf_vic] \n\t"
            "cmp  %[addr], #0xe000 \n\t"                            //    (addr >= 0xe000) &&
            "bcc  %l[kernalf_vic] \n\t"
            "cbnz %[kernal_check], kernalf_no_check \n\t"           //    (kernal_check == 0))
            :: [control] "r" (control), [addr] "r" (addr), [kernal_check] "r" (kernal_check),
               [kernal_rom] "r" (crt_rom_ptr), [gpioa_config] "r" (gpioa_config),
               [dwt_base] "r" (dwt_base), [gpioa_base] "r" (gpioa_base), [gpiod_base] "r" (gpiod_base),
               [c64_write] "i" (C64_WRITE), [c64_ba] "i" (C64_BA)
            : "cc" : kernalf_write, kernalf_vic );
        
        // kernal_pal_read: kernal_check must modify address bus earliest 280ns after phi2 L->H for the A14 hack
        asm (
            "ldrb r7, [%[kernal_rom], %[addr]] \n\t"                // speculatively read kernal data \n\t"
            "strb r7, [%[gpiod_base], %[gpiod_odr]] \n\t"
            "mov  r7, %[c64_game_exrom_low] \n\t"
            "str  r7, [%[gpioa_base], %[gpioa_bsrr]] \n\t"          // GAME and EXROM H->L 260-295 ns after PHI2 L->H (16k CRT mode)
            "mov  r7, #1<<(14*2) \n\t"
            "str  r7, [%[gpiod_base], %[gpioe_moder]] \n\t"         // C64 address line A14 regular output L 22 ns after EXROM H->L
            "bic  %[gpioa_config], %[gpioa_config], %[c64_game_exrom_high] \n\t"
            "str  %[gpioa_config], [%[gpioa_base], %[gpioa_otyper]] \n\t" // change GAME and EXROM outputs to push-pull for faster rise time
            "mov  r7, #0 \n\t"
            "ldr  %[control], [%[gpioa_base], %[gpiob_idr]] \n\t"   // control = C64_CONTROL_READ(); valid 10-15 ns after A14 H->L
            "str  r7, [%[gpiod_base], %[gpioe_moder]] \n\t"         // C64 address line A14 input again after 20 or 40 ns
            "tst  %[control], %[c64_romh] \n\t"                     // test ROMH
            "bne  kernalf_hiram_low \n\t"

            "kernalf_hiram_high: \n\t"                              // Kernal read
            "mov  r7, %[c64_exrom_high] \n\t"
            "str  r7, [%[gpioa_base], %[gpioa_bsrr]] \n\t"          // EXROM L->H 130 ns after EXROM H->L (Ultimax CRT mode)
            "movw r7, #0x5555 \n\t"
            "strh r7, [%[gpiod_base]] \n\t"                         // enable C64 data bus latest 100 ns before PHI2 H->L
            "orr  %[gpioa_config], %[gpioa_config], %[c64_exrom_high] \n\t"
            "str  %[gpioa_config], [%[gpioa_base], %[gpioa_otyper]] \n\t"  // change EXROM output back to open-drain
            "mov  %[kernal_check], #2 \n\t"                         // disable HIRAM detection and store HIRAM state
            "b    kernalf_read \n\t"
            
            "kernalf_hiram_low: \n\t"                               // RAM read
            "mov  r7, %[c64_game_exrom_high] \n\t"
            "str  r7, [%[gpioa_base], %[gpioa_bsrr]] \n\t"          // GAME and EXROM L->H (none CRT mode for RAM access)
            "orr  %[gpioa_config], %[gpioa_config], %[c64_game_exrom_high] \n\t"
            "str  %[gpioa_config], [%[gpioa_base], %[gpioa_otyper]] \n\t" // change GAME and EXROM output back to open-drain
            "mov  %[kernal_check], #1 \n\t"                         // disable HIRAM detection and store HIRAM state
            "b    kernalf_read_end \n\t"
          
            "kernalf_no_check: \n\t"
            "cmp  %[kernal_check], #1 \n\t"                         // if (HIRAM L) skip for RAM read, else
            "beq  kernalf_read_end \n\t"
            "mov  r7, %[c64_game_low] \n\t"
            "str  r7, [%[gpioa_base], %[gpioa_bsrr]] \n\t"          //   GAME H->L 260 ns after PHI2 L->H (Ultimax CRT mode)
            "bic  %[gpioa_config], %[gpioa_config], %[c64_game_high] \n\t"
            "str  %[gpioa_config], [%[gpioa_base], %[gpioa_otyper]] \n\t" // change GAME output to push-pull for faster rise time
            "ldrb r7, [%[kernal_rom], %[addr]] \n\t"                //   read kernal data \n\t"
            "strb r7, [%[gpiod_base], %[gpiod_odr]] \n\t"
            "movw r7, #0x5555 \n\t"
            "strh r7, [%[gpiod_base]] \n\t"                         //   enable C64 data bus latest 100 ns before PHI2 H->L
            
            "kernalf_read: \n\t"
            "wait_%=: \n\t"                                         // WAIT_UNTIL(PAL_PHI2_CPU_END);
            "ldr  r7, [%[dwt_base], #4] \n\t"
            "cmp  r7, %[cpu_end] \n\t"
            "bcc  wait_%= \n\t"

            "mov  r7, #0 \n\t"
            "strh r7, [%[gpiod_base]] \n\t"                         // release C64 data bus 10-20 ns after PHI2 H->L
            "mov  r7, %[c64_game_high] \n\t"
            "str  r7, [%[gpioa_base], %[gpioa_bsrr]] \n\t"          // GAME L->H 30-42 ns after PHI2 H->L (none CRT mode)
            "orr  %[gpioa_config], %[gpioa_config], %[c64_game_exrom_high] \n\t"
            "str  %[gpioa_config], [%[gpioa_base], %[gpioa_otyper]] \n\t"  // change GAME and EXROM output to back to open-drain
            "kernalf_read_end: "
            : [kernal_check] "+r" (kernal_check)
            : [addr] "r" (addr), [control] "r" (control), [kernal_rom] "r" (crt_rom_ptr), 
              [dwt_base] "r" (dwt_base), [gpioa_config] "r" (gpioa_config), [cpu_end] "r" (cpu_end),
              [gpioa_base] "r" (gpioa_base), [gpioa_otyper] "i" (GPIOA_OTYPER), [gpioa_bsrr] "i" (GPIOA_BSRR), [gpiob_idr] "i" (GPIOB_IDR),
              [gpiod_base] "r" (gpiod_base), [gpioe_idr] "i" (GPIOE_IDR), [gpiod_odr] "i" (GPIOD_ODR), [gpioe_moder] "i" (GPIOE_MODER),
              [c64_game_high] "i" (C64_GAME_HIGH), [c64_exrom_high] "i" (C64_EXROM_HIGH), [c64_game_exrom_high] "i" (C64_GAME_HIGH|C64_EXROM_HIGH),
              [c64_game_low] "i" (C64_GAME_LOW), [c64_game_exrom_low] "i" (C64_GAME_LOW|C64_EXROM_LOW), [c64_romh] "i" (C64_ROMH)
            : "r7", "cc" );
        goto kernalf_vic;

    kernalf_write:
        WAIT_UNTIL(phi2_cpu_start);                                 // read C64 data bus 230 ns after PHI2 L->H
        u32 data = C64_DATA_READ();
        kernal_write_handler(control, addr, data);

    kernalf_vic:
        /*
        if ((control & (C64_RESET|MENU_BTN)) != C64_RESET) {                                                                       
           break;                                                   // allow reset or menu button interrupt handler to run
        }*/
        WAIT_UNTIL(phi2_vic_start);
        
        COMPILER_BARRIER();                                                     
    }                                                                           
    C64_INTERFACE_DISABLE();                                                    
}
