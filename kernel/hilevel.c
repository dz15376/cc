#include "hilevel.h"

/* Since we *know* there will be 2 processes, stemming from the 2 user
 * programs, we can
 *
 * - allocate a fixed-size process table (of PCBs), and use a pointer
 *   to keep track of which entry is currently executing, and
 * - employ a fixed-case of round-robin scheduling: no more processes
 *   can be created, and neither is able to complete.
 */

pcb_t pcb[ 10 ], *current = NULL;
int num_pcb = 0;


void scheduler( ctx_t* ctx ) {
  if      ( current == &pcb[ 0 ] ) {
    memcpy( &pcb[ 0 ].ctx, ctx, sizeof( ctx_t ) ); // preserve P_1
    memcpy( ctx, &pcb[ 0 ].ctx, sizeof( ctx_t ) ); // restore  P_2
    current = &pcb[ 0 ];
  }
  /*else if ( current == &pcb[ 1 ] ) {
    memcpy( &pcb[ 1 ].ctx, ctx, sizeof( ctx_t ) ); // preserve P_2
    memcpy( ctx, &pcb[ 2 ].ctx, sizeof( ctx_t ) ); // restore  P_1
    current = &pcb[ 2 ];
  }

  else {
    memcpy( &pcb[ 2 ].ctx, ctx, sizeof( ctx_t ) ); // preserve P_2
    memcpy( ctx, &pcb[ 0 ].ctx, sizeof( ctx_t ) ); // restore  P_1
    current = &pcb[ 0 ];
  }*/
  return;
}

extern void main_console();
extern uint32_t tos_console;


void hilevel_handler_rst(ctx_t* ctx) {
  /* Configure the mechanism for interrupt handling by
   *
   * - configuring timer st. it raises a (periodic) interrupt for each
   *   timer tick,
   * - configuring GIC st. the selected interrupts are forwarded to the
   *   processor via the IRQ interrupt signal, then
   * - enabling IRQ interrupts.
   */
  for( int i = 0; i<10; i++){
    pcb[i].ctx.active=0;
  }

  memset( &pcb[ 0 ], 0, sizeof( pcb_t ) );
  pcb[ 0 ].pid      = 1;
  pcb[ 0 ].ctx.cpsr = 0x50;
  pcb[ 0 ].ctx.pc   = ( uint32_t )( &main_console );
  pcb[ 0 ].ctx.sp   = ( uint32_t )( &tos_console  );
  pcb[ 0 ].ctx.active = 1;

/*memset( &pcb[ 1 ], 0, sizeof( pcb_t ) );
  pcb[ 1 ].pid      = 2;
  pcb[ 1 ].ctx.cpsr = 0x50;
  pcb[ 1 ].ctx.pc   = ( uint32_t )( &main_P4 );
  pcb[ 1 ].ctx.sp   = ( uint32_t )( &tos_P4  );

  memset( &pcb[ 2 ], 0, sizeof( pcb_t ) );
  pcb[ 2 ].pid      = 2;
  pcb[ 2 ].ctx.cpsr = 0x50;
  pcb[ 2 ].ctx.pc   = ( uint32_t )( &main_P5 );
  pcb[ 2 ].ctx.sp   = ( uint32_t )( &tos_P5 );*/

  TIMER0->Timer1Load  = 0x00100000; // select period = 2^20 ticks ~= 1 sec
  TIMER0->Timer1Ctrl  = 0x00000002; // select 32-bit   timer
  TIMER0->Timer1Ctrl |= 0x00000040; // select periodic timer
  TIMER0->Timer1Ctrl |= 0x00000020; // enable          timer interrupt
  TIMER0->Timer1Ctrl |= 0x00000080; // enable          timer

  GICC0->PMR          = 0x000000F0; // unmask all            interrupts
  GICD0->ISENABLER1  |= 0x00000010; // enable timer          interrupt
  GICC0->CTLR         = 0x00000001; // enable GIC interface
  GICD0->CTLR         = 0x00000001; // enable GIC distributor

  int_enable_irq();

  current = &pcb[ 0 ]; memcpy( ctx, &current->ctx, sizeof( ctx_t ) );

  return;
}

void hilevel_handler_svc( ctx_t* ctx, uint32_t id ) {
  /* Based on the identified encoded as an immediate operand in the
   * instruction,
   *
   * - read  the arguments from preserved usr mode registers,
   * - perform whatever is appropriate for this system call,
   * - write any return value back to preserved usr mode registers.
   */

  switch( id ) {
    case 0x00 : { // 0x00 => yield()
      scheduler( ctx );
      break;
    }
    case 0x01 : { // 0x01 => write( fd, x, n )
      int   fd = ( int   )( ctx->gpr[ 0 ] );
      char*  x = ( char* )( ctx->gpr[ 1 ] );
      int    n = ( int   )( ctx->gpr[ 2 ] );

      for( int i = 0; i < n; i++ ) {
        PL011_putc( UART0, *x++, true );
      }

      ctx->gpr[ 0 ] = n;
      break;
    }
    case 0x03   : { // 0x3 => fork
      for( int i = 0; i<10; i++){
        if (pcb[i].ctx.active == 0)
          num_pcb = i;
          break;
      }
      memset( &pcb[num_pcb], 0, sizeof( pcb_t ) );
      pcb[num_pcb].pid      = num_pcb; //set id for process
      memcpy( &pcb[num_pcb].ctx, ctx, sizeof( ctx_t ) );
      pcb[num_pcb].ctx.gpr[0] = 0; //return id 0 for child
      ctx->gpr[0] = num_pcb; //return the child id to parent
      pcb[num_pcb].ctx.sp = &tos_console + (0x1000 * num_pcb); //copy stack from parent
      memcpy ( &pcb[num_pcb].ctx.sp, ctx->sp, 0x1000);
      pcb[num_pcb].ctx.pc = ctx->pc; //copy pc from parent
      pcb[num_pcb].ctx.active = 1; //activate the process
      break;
    }

    case 0x05 : { // => exec
      ctx->pc = ctx->gpr[0];
      break;
    }

    case 0x04 : { // => kill
      pcb[ctx->gpr[0]].ctx.active = 0;
      break;
    }
  }

  return;
}


void hilevel_handler_irq(ctx_t* ctx) {
  // Step 2: read  the interrupt identifier so we know the source.

  uint32_t id = GICC0->IAR;

  // Step 4: handle the interrupt, then clear (or reset) the source.

  if( id == GIC_SOURCE_TIMER0 ) {
    scheduler(ctx); TIMER0->Timer1IntClr = 0x01;
  }

  // Step 5: write the interrupt identifier to signal we're done.

  GICC0->EOIR = id;

  return;
}
