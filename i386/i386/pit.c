/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989 Carnegie Mellon University
 * Copyright (c) 1991 IBM Corporation 
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation,
 * and that the name IBM not be used in advertising or publicity 
 * pertaining to distribution of the software without specific, written
 * prior permission.
 * 
 * CARNEGIE MELLON AND IBM ALLOW FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON AND IBM DISCLAIM ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
  Copyright 1988, 1989 by Intel Corporation, Santa Clara, California.

		All Rights Reserved

Permission to use, copy, modify, and distribute this software and
its documentation for any purpose and without fee is hereby
granted, provided that the above copyright notice appears in all
copies and that both the copyright notice and this permission notice
appear in supporting documentation, and that the name of Intel
not be used in advertising or publicity pertaining to distribution
of the software without specific, written prior permission.

INTEL DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE
INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS,
IN NO EVENT SHALL INTEL BE LIABLE FOR ANY SPECIAL, INDIRECT, OR
CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT,
NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include <platforms.h>
#include <kern/time_out.h>
#include <i386/ipl.h>
#include <i386/pit.h>

int pitctl_port  = PITCTL_PORT;		/* For 386/20 Board */
int pitctr0_port = PITCTR0_PORT;	/* For 386/20 Board */
int pitctr1_port = PITCTR1_PORT;	/* For 386/20 Board */
int pitctr2_port = PITCTR2_PORT;	/* For 386/20 Board */
/* We want PIT 0 in square wave mode */

int pit0_mode = PIT_C0|PIT_SQUAREMODE|PIT_READMODE ;


unsigned int clknumb = CLKNUM;		/* interrupt interval for timer 0 */

#ifdef PS2
extern int clock_int_handler();

#include <sys/types.h>
#include <i386ps2/abios.h>
static struct generic_request *clock_request_block;
static int     clock_flags;
char cqbuf[200];        /*XXX temporary.. should use kmem_alloc or whatever..*/
#endif  /* PS2 */

clkstart()
{
	unsigned int	flags;
	unsigned char	byte;
	int s;

	intpri[0] = SPLHI;
	form_pic_mask();

	s = sploff();         /* disable interrupts */

#ifdef	PS2
        abios_clock_start();
#endif /* PS2 */

	/* Since we use only timer 0, we program that.
	 * 8254 Manual specifically says you do not need to program
	 * timers you do not use
	 */
	outb(pitctl_port, pit0_mode);
	clknumb = CLKNUM/hz;
	byte = clknumb;
	outb(pitctr0_port, byte);
	byte = clknumb>>8;
	outb(pitctr0_port, byte); 
	splon(s);         /* restore interrupt state */
}

#define COUNT   10000   /* should be a multiple of 1000! */

#ifdef PS2

abios_clock_start()
{
        struct generic_request  temp_request_block;
        int rc;

        nmi_enable();   /* has to happen somewhere! */
        temp_request_block.r_current_req_blck_len = ABIOS_MIN_REQ_SIZE;
        temp_request_block.r_logical_id = abios_next_LID(SYSTIME_ID,
                                                        ABIOS_FIRST_LID);
        temp_request_block.r_unit = 0;
        temp_request_block.r_function = ABIOS_LOGICAL_PARAMETER;
        temp_request_block.r_return_code = ABIOS_UNDEFINED;

        abios_common_start(&temp_request_block,0);
        if (temp_request_block.r_return_code != ABIOS_DONE) {
                panic("couldn init abios time code!\n");
	      }

        /*
         * now build the clock request for the hardware system clock
         */
        clock_request_block = (struct generic_request *)cqbuf;
        clock_request_block->r_current_req_blck_len =
                                temp_request_block.r_request_block_length;
        clock_request_block->r_logical_id = temp_request_block.r_logical_id;
        clock_request_block->r_unit = 0;
        clock_request_block->r_function = ABIOS_DEFAULT_INTERRUPT;
        clock_request_block->r_return_code = ABIOS_UNDEFINED;
        clock_flags = temp_request_block.r_logical_id_flags;
}

ackrtclock()
{
        if (clock_request_block) {
	  clock_request_block->r_return_code = ABIOS_UNDEFINED;
	  abios_common_interrupt(clock_request_block,clock_flags);
	}
      }
#endif /* PS2 */