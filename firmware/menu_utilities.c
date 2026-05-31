/*
 * Copyright (c) 2019-2026 Kim Jørgensen
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

static u8 utilities_sort_dir(OPTIONS_STATE *state, OPTIONS_ELEMENT *element, u8 flags)
{
	format_path(scratch_buf, false);
	c64_send_cancel_message(scratch_buf,
		"Sorting dir.\r\n\r\n\r\n\r\n\r\nScanned/sorted: 0 / 0");
    c64_interface(false);

    dat_file.boot_type = DAT_NONE;
    save_dat();

    FSORTDIR sort;
    if(!sort_dir(&sort, dat_buffer, crt_ram_buf))
    {
        c64_interface_sync();
        fail_to_read_sd();
    }

    cyccnt_timer_start();
    while (sort_next(&sort) && !sort.done)
    {
        if (!cyccnt_timer_elapsed_ms(125))
        {
            continue;
        }

        u8 reply = c64_interface_sync();

        sprint(scratch_buf, "%u / %u",
            (unsigned int)sort.scanned_entries, (unsigned int)sort.sorted_entries);
        c64_send_text_wait(COLOR_LIGHTGREEN, 16, 8, scratch_buf);
        c64_interface(false);

        if (reply != REPLY_OK)
        {
            break;
        }

        cyccnt_timer_start();
    }

    if (!sort_close(&sort))
    {
        c64_interface_sync();
        fail_to_read_sd();
    }

    restart_to_menu();
    return CMD_NONE;
}

static u8 handle_utilities(void)
{
    if (menu != &sd_menu)
    {
        return CMD_NONE;
    }

    OPTIONS_STATE *options = options_init("Utilities");
    options_add_text_element(options, utilities_sort_dir, "Sort directory");
    options_add_dir(options, "Cancel");
    return handle_options();
}
