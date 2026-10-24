; Host stubs: the Amiga port layer's entry points as seen by sz2.s
; (assembled with -D__AMIGA__), for the C port layer in src/crs. Its
; instruction hook does the work when the PC reaches one of the labels it
; knows (crs_symbols.h, made from the link map): frame ends, VBLs, the
; sprite list, the music; the 68000 code here only keeps the register and
; flag contracts of the Amiga versions. Every label the hook knows has its
; own address.

	xdef amiga_clear_text_planes,amiga_escape_clear,amiga_escape_menu
	xdef amiga_escape_pressed,amiga_exit_game,amiga_graphics_changed
	xdef amiga_mcdrv_probe,amiga_music_loaded,amiga_printf
	xdef amiga_samples_loaded,amiga_ship_control,amiga_text_character
	xdef amiga_text_cleared,amiga_text_row_written,amiga_wait_vbl
	xdef amiga_xsp_vsync,compile_characters,compile_sprite
	xdef prepare_sprite_infos,text_bitmaps,text_matrix
	xdef host_frame_end,host_vbl,host_stubs_start,host_stubs_end

	xref VBL_VWAIT_COUNTER,L_00000118

	text

host_stubs_start:

; WAIT_VBL: d1.w = frames to wait minus one. Runs d1 + 1 VBLs (none for a
; negative d1), lockstep. Preserves all registers.

amiga_wait_vbl:
	movem.l	d0-d7/a0-a6,-(sp)

	move	d1,d7
	bmi		.done

.loop:
	bsr		host_vbl
	dbf		d7,.loop

.done:
	bsr		host_frame_end

	movem.l	(sp)+,d0-d7/a0-a6

	rts

; XSP_VSYNC: d0.w = VBLs this game frame lasts. Runs them, then returns the
; game's VBL counter in d0.w and clears it.

amiga_xsp_vsync:
	movem.l	d1-d7/a0-a6,-(sp)

	move	d0,d7
	subq	#1,d7
	bmi		.done

.loop:
	bsr		host_vbl
	dbf		d7,.loop

.done:
	bsr		host_frame_end

	movem.l	(sp)+,d1-d7/a0-a6

	move	VBL_VWAIT_COUNTER,d0
	clr		VBL_VWAIT_COUNTER

	rts

; One VBL (the host counts it). The game's VBL handler ends with RTE: a
; format 0 frame, as vbl_server.

host_vbl:
	movem.l	d0-d7/a0-a6,-(sp)

	move.w	#0,-(sp)
	pea		.return(pc)
	move.w	sr,-(sp)
	move.l	L_00000118,-(sp)
	rts

.return:
	movem.l	(sp)+,d0-d7/a0-a6

	rts

; The host ends the frame here: crs_run_frame returns.

host_frame_end:
	rts

; No ESC (START is the X68000's ESC key, through _BITSNS): Z set, d0 kept.

amiga_escape_pressed:
	move.l	d0,-(sp)
	moveq	#0,d0
	movem.l	(sp)+,d0

	rts

; The host sets d0: $69 (MCDRV 0.69) with the music streams, else -1 (no
; driver: the game loads no songs and never calls trap #4).

amiga_mcdrv_probe:
	rts

; Called with jmp from CLEAR_TEXT_PLANE: the host clears the four planes.

amiga_clear_text_planes:
	rts

; The host keeps the sprite list and resets the game's, as the Amiga version.

prepare_sprite_infos:
	moveq	#0,d0

	rts

amiga_exit_game:
	rts

amiga_printf:
	rts

; The game runs its ship control: a stage frame (auto fire).

amiga_ship_control:
	rts

; a0 = song data, a1 = its file name.

amiga_music_loaded:
	rts

; GVRAM has been built (make_crs takes the backgrounds after it).

amiga_graphics_changed:
	rts

amiga_escape_clear:
amiga_escape_menu:
amiga_samples_loaded:
amiga_text_character:
amiga_text_cleared:
amiga_text_row_written:
compile_characters:
compile_sprite:
	rts

host_stubs_end:

	bss

text_bitmaps:
	ds.l	2*32 ; Text clear bitmap infos followed by text draw bitmap infos.

text_matrix:
	ds.b	32*32

	end
