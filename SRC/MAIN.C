#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dos.h>
#include <conio.h>
#include <graph.h>
#include <malloc.h>
#include <math.h>
#include <i86.h>
#include <inttypes.h>
#include "midasdll.h"

#define MODE_13H 0x13
#define MODE_TXT 0x03

#define NUM_PIPES 6
#define NUM_STARS 110

#define MAX_PIPE_Y 100   // pixels
#define MAX_PIPE_X 40    // pixels
#define PIPE_HAT_Y 5     // pixels
#define BIRD_SZ    16    // pixels
#define BIRD_X     20    // pixels
#define STATUSBAR  3200  // pixels (10px top row)

#define G          2     // px? sure!

#define SFX_BG   0
#define SFX_HIT  1
#define SFX_JUMP 2
#define SFX_MAX  SFX_JUMP

#define FLAP_LOAD  0
#define FLAP_PAUSE 1
#define FLAP_GAME  2
#define FLAP_LOSE  3
#define FLAP_END   4

void (__interrupt __far *prev_timer)();

typedef struct flapstate {
	// game state enum
	unsigned char mode;
	// loader progress bar
	unsigned char load;
	// [[x,y],...]
	// where +y = top, -y = bottom, for MAX_PIPE_Y
	int pipes[NUM_PIPES][2];
	// where [y,x]
	unsigned short stars[NUM_STARS][2];
	// where [y(input),y(bird)]
	int birdie[2];
	// last updated at
	clock_t clock;
	short tick;
	// fps
	int fps_ctr;
	int fps_avg;
	int score;
	int score_mask;
	unsigned char hit;
	// sprite
	unsigned char sprite_frame;
	// sfx
	MIDASmodule sfx_mod[3];
	MIDASmodulePlayHandle sfx_play_hnd[3];
	MIDASplayStatus* sfx_play_stat[3];
} flapstate;

static flapstate *STATE = NULL;
static unsigned char *VBUF = (unsigned char *) 0xA0000;
static unsigned char *PBUF = NULL;

typedef struct vgacolor {
	unsigned char r;
	unsigned char g;
	unsigned char b;
} vgacolor;

static vgacolor VGAPAL[256];

#pragma pack(push, 1)
typedef struct pcxheader {
	// 0x0A
	unsigned char magic;
	// 0-5
	unsigned char version;
	// 0 (uncompressed), 1 (RLE)
	unsigned char encoding;
	// bits per plane
	unsigned char bpp;
	unsigned short x_min;
	unsigned short y_min;
	unsigned short x_max;
	unsigned short y_max;
	unsigned short x_dpi;
	unsigned short y_dpi;
	unsigned char ega[48];
	unsigned char _r;
	// number of color planes
	unsigned char color_planes;
	// number of bytes per scan line
	unsigned short bytes_per_sline;
	unsigned short palette_mode;
	unsigned short host_x;
	unsigned short host_y;
	unsigned char _r2[54];
} pcxheader;
#pragma pack(pop)

typedef struct spritesheet {
	// uncompressed RLE (may be unloaded!)
	unsigned char *buf_uncomp;
	// x*y chars each with closest VGA pal color
	unsigned char *buf_vga;
	pcxheader *pcx;
} spritesheet;

#define BG_COLOR 105
#define STAR_COLOR 31

void draw_progress(unsigned char tick) {
	if (STATE->mode != FLAP_LOAD) return;
	STATE->load = min(100, STATE->load+tick);
	unsigned int x = (STATE->load*320)/100;

	for (unsigned int y=190; y<=200; ++y) {
	for (unsigned int xp=0; xp<=x; ++xp)
		VBUF[(y<<8)+(y<<6)+xp] = rand() % 100;
	}
}

/*
<0xC0 = pal val
0xC0 = 2 highest MSB set in char, remaining 6 bits run length
*/
unsigned char *pcx_decode_raster(unsigned char* buf, unsigned int sz) {
	// find the total number of un-rle'd bytes
	unsigned int len_uncomp = 0;
	for (unsigned int i=sizeof(pcxheader); i<sz; ++i) {
		if (buf[i] < 0xC0) len_uncomp += 1;
		else if (buf[i] > 0xC0) {
			len_uncomp += (buf[i] & 0x3F);
			++i;
		}
	}

	printf("read pcx %upx:", len_uncomp);

	unsigned char *decoded = malloc(len_uncomp);
	unsigned int ofs = 0;

	for (unsigned int i=sizeof(pcxheader); i<sz; ++i) {
	if (i % (sz/100) == 0) draw_progress(1);
		if ((i%320)==0) printf(".");

		if (buf[i] < 0xC0) {
			decoded[ofs] = buf[i];
			ofs += 1;
			continue;
		}

		unsigned int len = buf[i] & 0x3F;
		memset(&decoded[ofs], buf[i+1], len);
		ofs += len;
		++i;
	}
	printf("\n");
	return decoded;
}

// crudely match colors to loaded vga palette
unsigned char pcx2vga(unsigned char r, unsigned char g, unsigned char b) {
	unsigned int d = UINTMAX_MAX;
	unsigned char c = 0;
	for (unsigned short i=0; i<256; ++i) {
	unsigned int diff = (
		abs(VGAPAL[i].r - r) +
		abs(VGAPAL[i].g - g) +
		abs(VGAPAL[i].b - b)
	);
	if (diff < d) {
		c=(unsigned char) i;
		d=diff;
	}
	}
	return c;
}

spritesheet *load_sprite(const char* sprite_name) {
	FILE *bird = fopen(sprite_name, "rb");
	if (!bird) {
		printf("Unable to load sprite %s!\n", sprite_name);
		return NULL;
	}
	fseek(bird, 0L, SEEK_END);
	unsigned long int sz = ftell(bird);

	char *buf = malloc(sz);

	if (!buf) {
		printf("Could not allocate buffer for sprite!\n");
		return NULL;
	}

	printf("%s %ldb...", sprite_name, sz);

	fseek(bird, 0L, SEEK_SET);
	size_t read_sz = fread(buf, sizeof(unsigned char), sz, bird);

	printf("read %ub...\n", read_sz);

	spritesheet *s = calloc(1, sizeof(spritesheet));
	s->buf_uncomp = pcx_decode_raster(buf, read_sz);
	s->pcx = calloc(1, sizeof(pcxheader));
	memcpy(s->pcx, buf, sizeof(pcxheader));

	printf(
		"%hhux%hhu@%hhu bpp, %u b/sln\n\n",
		s->pcx->x_max+1,
		s->pcx->y_max+1,
		s->pcx->bpp * s->pcx->color_planes,
		s->pcx->bytes_per_sline
	);
	free(buf);

	s->buf_vga = calloc(
		(s->pcx->bytes_per_sline)*(s->pcx->y_max+1),
		sizeof(char)
	);

	for (unsigned int y=0; y<=s->pcx->y_max; ++y) {
		unsigned int yy=y*s->pcx->color_planes;
		for (unsigned int x=0; x<s->pcx->bytes_per_sline; ++x) {
			unsigned int ofs_r = (yy+0)*s->pcx->bytes_per_sline+x;
			unsigned int ofs_g = (yy+1)*s->pcx->bytes_per_sline+x;
			unsigned int ofs_b = (yy+2)*s->pcx->bytes_per_sline+x;

			unsigned int bufofs = ((s->pcx->bytes_per_sline)*y)+x;
			s->buf_vga[bufofs] = pcx2vga(
				*(s->buf_uncomp+ofs_r),
				*(s->buf_uncomp+ofs_g),
				*(s->buf_uncomp+ofs_b)
			);
			// 55 is the sprite background color, but i want to change it
			if (s->buf_vga[bufofs]==55) s->buf_vga[bufofs]=BG_COLOR;
		}
	}

	free(s->buf_uncomp);
	return s;
}

void read_vga_pal() {
	outp(0x3C7, 0);

	// after reading 3 bytes the pal counter automatically
	// increments
	for (unsigned short c=0; c<256; ++c) {
		VGAPAL[c].r = (unsigned char) inp(0x3C9);
		VGAPAL[c].g = (unsigned char) inp(0x3C9);
		VGAPAL[c].b = (unsigned char) inp(0x3C9);
	}
}

void draw_sprite(
	spritesheet *sheet,
	// the tile row
	unsigned short ty,
	// the tile col
	unsigned short tx,
	// size of the sprite
	unsigned short tsz,
	// vga ofs y
	unsigned short vy,
	// vga ofs x
	unsigned short vx
) {
	// try to do some crude bounds checking
	if (((ty+tsz)>sheet->pcx->y_max) || ((tx+tsz)>sheet->pcx->bytes_per_sline))
		return;
	for (unsigned int y=ty; y<=ty+tsz; ++y) {
		for (unsigned int x=tx; x<tx+tsz; ++x) {
			unsigned int sbufofs = (y*(sheet->pcx->bytes_per_sline))+x;
			unsigned int vbufofs = ((vy+y)<<6)+((vy+y)<<8)+vx+x-tx;
			unsigned char c = sheet->buf_vga[sbufofs];

			if ((c==BG_COLOR) && (PBUF[vbufofs]!=BG_COLOR))
				continue;
			
			// if the target px is not a star or bg color, it must be a pipe
			if (!STATE->hit) STATE->hit = !((PBUF[vbufofs] == BG_COLOR) || (PBUF[vbufofs] == STAR_COLOR));
			PBUF[vbufofs] = sheet->buf_vga[sbufofs];
		}
	}
}

void set_video_mode(unsigned short int mode) {
	union REGS regs;
	regs.h.ah = 0;
	regs.h.al = mode;
	int386(0x10, &regs, &regs);
}

// rand int at least abs(min)
int rand_int_min(int min, int range) {
	int n = rand() % range;
	while (n < min) n = rand() % range;
	return (rand() % 2 == 0) ? n : n * -1;
}

void init_state() {
	if (PBUF != NULL) free(PBUF);

	PBUF = calloc((320*200), sizeof(unsigned char));
	flapstate *fs = STATE ? STATE : calloc(1, sizeof(flapstate));

	if (fs == NULL) {
		printf("State init failed!\n");
		exit(1);
	}
	STATE = fs;

	int i = 0;
	for (int x=15; x < 320; x+=(320/NUM_PIPES)) {
		STATE->pipes[i][0] = x;
		// avoid colliding on spawn
		STATE->pipes[i][1] = rand_int_min(20, i==0 ? 50 : MAX_PIPE_Y);
		i++;
	}

	for (int i=0; i<NUM_STARS; ++i) {
		unsigned short y = rand() % 200;
		unsigned short x = rand() % 320;
		STATE->stars[i][0] = y;
		STATE->stars[i][1] = x;
	}

	STATE->birdie[1] = 100;
	STATE->mode = FLAP_LOAD;
	STATE->fps_avg = 30; // hopefully!
	STATE->score = 0;
	STATE->score_mask = 0;
	STATE->hit = 0;
}

void print_state() {
	for (int i = 0; i < NUM_PIPES; ++i) {
		printf(
			"Pipe %d: [%d,%d]\n",
			i,
			STATE->pipes[i][0],
			STATE->pipes[i][1]
		);
	}
}

void move_pipes() {
	 for (int i=0; i < NUM_PIPES; ++i) {
		 STATE->pipes[i][0] -= (2 + STATE->score / 25);

		 if (STATE->pipes[i][0] < -MAX_PIPE_X) {
			 STATE->pipes[i][0] = 320;
			 STATE->pipes[i][1] = rand_int_min(20, MAX_PIPE_Y);
			 // allow pipe to be counted for score again
			 STATE->score_mask = ((0 << i) & STATE->score_mask);
		 }

		 // the pipe has passed the scroll threshold
		 if ((STATE->pipes[i][0] < BIRD_X) && (!((STATE->score_mask>>i) & 1))) {
			 STATE->score++;
			 STATE->score_mask = ((1 << i) | STATE->score_mask);
		 }
	 }
}

void update_state() {
	 if (STATE->mode == FLAP_GAME) {
		 move_pipes();
		 STATE->birdie[0] = max(0, STATE->birdie[0]-G);
		 STATE->birdie[1] = max(0, min(200-BIRD_SZ, STATE->birdie[1]+G-STATE->birdie[0]));
	 }

	 for (int i=0; i<NUM_STARS; ++i) {
		 unsigned short up = STATE->stars[i][1]++;
		 if (STATE->stars[i][1] >= 320) STATE->stars[i][1] = 0;
	 }
}

// columnar rainbow gradient that moves as x changes
void fill_rbow_grad(unsigned int x, unsigned long ofs, size_t len) {
	for (unsigned int i=0; i<len; ++i) {
		PBUF[ofs+i]=0x67-(i/max(1,(MAX_PIPE_X/20)))-(x/10);
	}
}

void draw_pipe_row(unsigned int y, unsigned int x, unsigned int sz) {
	unsigned long ofs = (y<<8)+(y<<6)+x;
	if (ofs >= 64000) return;
	unsigned long fin = ofs+sz;
	int len = fin-ofs;
	if (fin>64000) len = max(0, abs(64000-ofs));
	// prune crazy sizes
	if ((len > 100) || (len <= 0)) return;
	//_fmemset(&PBUF[ofs], 0x4F, len);
	fill_rbow_grad(x, ofs, len);
}

void draw_pipes() {
	for (int i=0; i<NUM_PIPES; ++i) {
		int sz_y = STATE->pipes[i][1];
		int start = STATE->pipes[i][0];
		unsigned int x = max(0, start);

		// pipe width
		int sz_x = max(
			0,
			(start < 0) ? MAX_PIPE_X + start : MAX_PIPE_X
		);
		// clip sz_x off the right
		if ((x+sz_x) > 320) sz_x = 320-x;

		// draw pipes (top, bottom), hats first
		if (sz_y >= 0) {
			for (unsigned int y=10; y<sz_y-PIPE_HAT_Y; ++y)
				draw_pipe_row(y, (sz_x>10) ? x+5 : x, max(sz_x-10,0));
			for (unsigned int y=sz_y-PIPE_HAT_Y; y<sz_y; ++y)
				draw_pipe_row(y, x, sz_x);
		} else {
			unsigned int top = 200-abs(sz_y);
			for (unsigned int y=200; y>(top+PIPE_HAT_Y); --y)
				draw_pipe_row(y, (sz_x>10) ? x+5 : x, max(sz_x-10,0));
			for (unsigned int y=top+PIPE_HAT_Y; y>top; --y)
				draw_pipe_row(y, x, sz_x);
		}
	}
}

// also advances the sprite frame with each draw
void draw_birdie(spritesheet *b) {
	int y = STATE->birdie[1];
	int x = BIRD_X;
	draw_sprite(b,16,16*STATE->sprite_frame++,16,y,x);
	if (STATE->sprite_frame >= 8) STATE->sprite_frame=0;
}

void draw_stars() {
	for (int i=0; i<NUM_STARS; ++i) {
	unsigned short y = STATE->stars[i][0];
	unsigned short x = STATE->stars[i][1];
	unsigned char cur = PBUF[(y<<8)+(y<<6)+x];
	if (cur != BG_COLOR) continue;
	PBUF[(y<<8)+(y<<6)+x] = STAR_COLOR;
	}
}

void draw_statusbar() {
	char status[40];
	if (STATE->mode == FLAP_GAME)
	snprintf(
		&status[0],
		40,
		// 40 chars
		" FPS %d                       SCORE %03d",
		min(STATE->fps_avg, 99),
		STATE->score
	);
	 else
	snprintf(
		&status[0],
		40,
		// 40 chars
		" PAUSED   ([p]ause [q]uit)    SCORE %03d",
		STATE->score
	);

	_settextposition(0,0);
	_outtext(status);
}

void handle_input() {
	if (!kbhit()) return;
	int k = getch();
	switch (k) {
		// space
		case 0x20:
			if (STATE->mode != FLAP_GAME) return;
			STATE->birdie[0] = min(STATE->birdie[0]+10, 200);

			if (!STATE->sfx_play_hnd[SFX_JUMP]) return;
			MIDASstopModule(STATE->sfx_play_hnd[SFX_JUMP]);
			STATE->sfx_play_hnd[SFX_JUMP] = MIDASplayModule(STATE->sfx_mod[SFX_JUMP], 0);
			MIDASsetMusicVolume(STATE->sfx_play_hnd[SFX_JUMP], 32);

			break;
		case 0x50:
		case 0x70:
			STATE->mode = STATE->mode == FLAP_GAME ? FLAP_PAUSE : FLAP_GAME;
			draw_statusbar();
			break;
		case 0x71:
			STATE->mode = FLAP_LOSE;
			break;
	}
}

void write_vbuf() {
	unsigned int base = STATUSBAR;
	if (!(STATE->mode == FLAP_GAME || STATE->mode == FLAP_PAUSE)) base = 0;
	memcpy(&VBUF[base], &PBUF[base], (320*200)-base);

	if (clock() - STATE->clock >= CLOCKS_PER_SEC) {
		STATE->fps_avg = (STATE->fps_avg + STATE->fps_ctr) / 2;
		STATE->fps_ctr = 0;
		STATE->clock = clock();
	}

	STATE->fps_ctr += 1;
}

void init_sfx(int needs_conf) {
	MIDASstartup();

	int conf = MIDASloadConfig("SOUND.CFG");
	if (!conf) {
		printf("No sound conf found\nAttempt autodetect...\n");
		if (needs_conf) conf = MIDASconfig();
		else conf = MIDASdetectSoundCard();
	}

	MIDASinit();

	if (conf) conf = MIDASsaveConfig("\.\SOUND.CFG");
	else {
		printf("Could not configure MIDAS!\nSound off\n");
		MIDASclose();
		return;
	}

	if (!conf) printf("WARN: Could not save sound config\n");
}

void load_sfx() {
	printf("Loading SFX...\n");
	STATE->sfx_mod[SFX_BG] = MIDASloadModule("BG.IT");
	draw_progress(10);
	STATE->sfx_mod[SFX_HIT] = MIDASloadModule("HIT.IT");
	draw_progress(10);
	STATE->sfx_mod[SFX_JUMP] = MIDASloadModule("JUMP.IT");
	draw_progress(10);

	if (!STATE->sfx_mod[SFX_BG] || !STATE->sfx_mod[SFX_HIT] ||
			!STATE->sfx_mod[SFX_JUMP]) {
		printf("Unable to load SFX!\nSound off\n");
		MIDASclose();
		return;
	}
}

void update_sfx_state() {
	if (STATE->mode != FLAP_GAME) return;
	for (unsigned char i=0; i<=SFX_MAX; ++i) {
		if (!STATE->sfx_play_hnd[i]) continue;
		if (!STATE->sfx_play_stat[i])
			STATE->sfx_play_stat[i] = calloc(1, sizeof(MIDASplayStatus));
		MIDASgetPlayStatus(STATE->sfx_play_hnd[i], STATE->sfx_play_stat[i]);
	}
}

void teardown_sfx() {
	for (unsigned char i=0; i<=SFX_MAX; ++i) {
		// if (STATE->sfx_play_stat[i]) free(STATE->sfx_play_stat[i]);
		if (STATE->sfx_play_hnd[i]) MIDASstopModule(STATE->sfx_play_hnd[i]);
		if (STATE->sfx_mod[i]) MIDASfreeModule(STATE->sfx_mod[i]);
	}
	MIDASclose();
}

void init_wipe() {
	for (unsigned int x=0; x<=320; ++x) {
		for (unsigned int y=0; y<=60 * sin(x/6); ++y) {
			PBUF[(y<<8)+(y<<6)+x] = 249;
		}
	}
	write_vbuf();
}

void draw_wipe() {
	unsigned int wipe[320];
	for (unsigned int x=0; x<=320; ++x) {
		unsigned int y=0;
		while (PBUF[(y<<8)+(y<<6)+x] == 249) ++y;
		wipe[x] = y;
	}

	while (STATE->mode == FLAP_LOSE) {
		for (unsigned int x=0; x<=320; ++x) {
			unsigned int y = ++wipe[x];
			unsigned long ofs = (y<<8)+(y<<6)+x;
			if (ofs > 63999) continue;
			PBUF[ofs] = 249;
		}

		write_vbuf();

		if (PBUF[63999] == 249) STATE->mode = FLAP_END;
	}
}

void __interrupt __far timer_game_tick() {
	//update_sfx_state();
	update_state();
	prev_timer();
}

void __interrupt __far timer_load_tick() {
	draw_progress(0);
}

void main(int argc, char *argv[]) {
	init_state();
	init_sfx(argc > 1);

	set_video_mode(MODE_13H);
	srand(clock());
	read_vga_pal();

	// bind timer interrupt
	prev_timer = _dos_getvect(0x1C);
	_dos_setvect(0x1C, timer_load_tick);

	draw_progress(1);
	printf("SPACEBIRD!\n");
	printf("Press SPACE to JUMP!\n\n");

	// load assets
	spritesheet *b = load_sprite("bird.pcx");
	if (!b) printf("Unable to load sprite\n");
	load_sfx();

	// for _outtext and friends
	_setvideomode(_MRES256COLOR);
	_settextcolor(13);

	// bind timer interrupt
	_dos_setvect(0x1C, prev_timer);
	prev_timer = _dos_getvect(0x1C);
	_dos_setvect(0x1C, timer_game_tick);

	// set up MIDAS modules
	// MIDAS opens channels on play but no concurrent playback unless
	// you open a few channels in advance?
	MIDASopenChannels(6);
game:
	STATE->sfx_play_hnd[SFX_JUMP] = MIDASplayModule(STATE->sfx_mod[SFX_JUMP], 0);
	STATE->sfx_play_hnd[SFX_BG] = MIDASplayModule(STATE->sfx_mod[SFX_BG], 1);
	MIDASsetMusicVolume(STATE->sfx_play_hnd[SFX_JUMP], 0);
	MIDASsetMusicVolume(STATE->sfx_play_hnd[SFX_BG], 16);

	// start scene
	STATE->mode=FLAP_PAUSE;

	while (STATE->mode != FLAP_END && STATE->mode != FLAP_LOSE) {
		if (clock() - STATE->clock >= CLOCKS_PER_SEC) draw_statusbar();
		write_vbuf();

		// clear page after size of statusbar
		memset(&PBUF[STATUSBAR], BG_COLOR, (320*200)-STATUSBAR);
		handle_input();
		draw_pipes();
		draw_birdie(b);
		draw_stars();

		if (STATE->hit) {
			STATE->mode = FLAP_LOSE;
			STATE->sfx_play_hnd[SFX_HIT] = MIDASplayModule(STATE->sfx_mod[SFX_HIT], 0);
			MIDASsetMusicVolume(STATE->sfx_play_hnd[SFX_HIT], 32);
			init_wipe();
		}
	}

	draw_wipe();

	_settextcolor(10);
	_settextposition(10,0);

	char end[100];
	snprintf(
		&end[0],
		100,
		"EEP!\n\nSCORE: %d\nPress r to restart...any key to exit...",
		STATE->score
	);
	_outtext(end);
	_settextcolor(3);
	_outtext("\n\nSPACEBIRD!\n(c) 2023 David Stancu\n");
	_outtext("https://dstancu.xyz\n\n");
	_outtext("Bird Sprite: ma9ici4n.itch.io\n");
	_outtext("Music:       yateoi.bandcamp.com\n");
	_outtext("SFX:         clonethirteen.itch.io\n");

	switch(getch()) {
		case 0x72:
			for (unsigned char i=0; i<=SFX_MAX; ++i) {
				if (!STATE->sfx_play_hnd[i]) continue;
				MIDASstopModule(STATE->sfx_play_hnd[i]);
			}
			init_state();
			goto game;
	}

	// restore 0x1C
	_dos_setvect(0x1C, prev_timer);

	teardown_sfx();
	free(PBUF);
	free(STATE);
	free(b);
	set_video_mode(MODE_TXT);
}
