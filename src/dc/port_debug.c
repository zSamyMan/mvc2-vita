/* The PS Vita port's debug menu, its game side (DEBUG_OPTIONS=1 only; the menu itself, L + DOWN + SELECT, is the
   host's, port/vita/vita_debug_menu.c): the actions it asks for, done here at the start of a game frame
   (System_Main), never from the host's thread, and the stage it may set for the next fight. Not Capcom's. */
#include "player.h"

#ifdef HOST_PORT
extern void load_stop( void ) ;
extern void fight_load_start( int no ) ;

/* the host's (vita_debug_menu.c): the game only calls out, the program keeping its own names to itself */
extern int host_dbg_take( void ) ;			/* the menu's action, once, else 0 */
extern int host_dbg_stage( void ) ;		/* the next fight's stage, -1 the game's own */
extern void host_dbg_report( int task, int scene, int stage, int fight ) ;
extern void host_dbg_burst( void ) ;		/* a burst of pictures from this frame (the renderer's SELECT + DOWN) */

static int staff_logo_pending, staff_logo_hold ;

/* the stage chosen by the select, or the menu's (sel_decide.c's stage_decide calls it last) */
void port_dbg_stage_apply( void )
{
	int st = host_dbg_stage() ;

	if( st >= 0 && st < 17 ){
		extern void host_log( const char *fmt, ... ) ;

		host_log( "dbg: the next fight on stage %d (the game's choice %d)", st, stage_no ) ;
		stage_no = st ;
		Sys->boss_flag = st == 8 ;
	}
}

static void routines_clear( void )
{
	Sys->routine_0 = Sys->routine_1 = Sys->routine_2 = Sys->routine_3 = 0 ;
	Sys->exit_flag = 0 ;
}

/* what the training menu's restart stops before it leaves a fight (pl_sw.c's training_restart) */
static void scene_leave( void )
{
	work_init() ;
	back_color_set( 0xff000000 ) ;
	sound_all_off() ;
	se_all_off() ;
	stream_stop() ;
	load_stop() ;
}

/* each game frame, first: the menu's request, if there is one */
void port_dbg_frame( void )
{
	int a ;

	host_dbg_report( Sys->task_no, Sys->scene_no, stage_no, Sys->stage_cnt + 1 ) ;
	if( !( a = host_dbg_take() ) ) return ;
	switch( a ){
	case 1:	/* back to the select, the next fight on the menu's stage: as the training menu's CHARACTER CHANGE does it
			   (pl_sw.c's training_restart): the works, the sounds, the stream and the loads under way stopped first.
			   Going back without them (straight to the versus step or the select) crashed mid-fight */
		if( Sys->task_no != 2 ) break ;
		Sys->play_side = 0 ;
		Sys->new_side = Sys->entry_side ;
		Sys->join_side = Sys->entry_side ;
		Sys->scene_no = 0 ;
		routines_clear() ;
		Sys->join_flag = 0 ;
		Sys->heart_step = 0 ;
		Sys->entry_stop = 1 ;
		scene_leave() ;
		fight_load_start( 12 ) ;
		break ;
	case 2:	/* the next fight the arcade's last: Abyss */
		Sys->stage_cnt = 7 ;
		pl_work[0].stage_cnt = pl_work[1].stage_cnt = 7 ;
		break ;
	case 3:	/* the staff roll (a game must be running) */
	case 4:	/* or straight to its end: the logo, then THANK YOU FOR PLAYING */
		if( Sys->task_no != 2 ) break ;
		scene_leave() ;
		Sys->scene_no = 4 ;
		routines_clear() ;
		staff_logo_pending = a == 4 ;
		break ;
	case 5:	/* back to the attract: the game's end, or the attract from its start */
		if( Sys->task_no == 2 ){
			Sys->scene_no = 6 ;
			routines_clear() ;
		} else if( Sys->task_no == 1 ){
			Sys->task_step = 0 ;
			Sys->scene_no = 0 ;
			routines_clear() ;
		}
		break ;
	}
}

/* staff.c's staff_main, first: once its set-up is done, the skip to the logo the menu asked for */
void port_dbg_staff_hook( void )
{
	if( staff_logo_pending && Sys->routine_1 == 1 ){
		staff_logo_pending = 0 ;
		staff_logo_hold = 1 ;
		Sys->routine_2 = 7 ;	/* the logo and its flash after a second of black */
		Sys->timer0 = 60 ;
	}
	/* the logo and THANK YOU FOR PLAYING held for a look, until START (which the game takes as its way out) */
	if( staff_logo_hold ){
		if( Sys->routine_2 == 7 && Sys->timer0 == 2 ) host_dbg_burst() ;	/* two seconds of pictures from the flash's start */
		if( Sys->routine_1 != 1 ) staff_logo_hold = 0 ;
		else if( Sys->routine_2 == 6 && Sys->timer0 < 10 ) Sys->timer0 = 10 ;
	}
}
#endif
