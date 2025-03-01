#include <fcntl.h>
#include <ncurses.h> // needed for some definitions, see terminfo(3ncurses)
#include "internal.h"

// we found Sixel support -- set up the API
static inline void
setup_sixel_bitmaps(tinfo* ti){
  ti->bitmap_supported = true;
  ti->color_registers = 256;  // assumed default [shrug]
  ti->pixel_init = sixel_init;
  ti->pixel_draw = sixel_draw;
  ti->sixel_maxx = 4096; // whee!
  ti->sixel_maxy = 4096;
  ti->pixel_remove = NULL;
  ti->pixel_destroy = sixel_destroy;
  ti->pixel_wipe = sixel_wipe;
  ti->pixel_shutdown = sixel_shutdown;
  ti->pixel_rebuild = sixel_rebuild;
  ti->sprixel_scale_height = 6;
}

static inline void
setup_kitty_bitmaps(tinfo* ti, int fd){
  ti->pixel_wipe = kitty_wipe;
  ti->pixel_destroy = kitty_destroy;
  ti->pixel_remove = kitty_remove;
  ti->pixel_draw = kitty_draw;
  ti->pixel_shutdown = kitty_shutdown;
  ti->sprixel_scale_height = 1;
  ti->pixel_rebuild = kitty_rebuild;
  ti->pixel_clear_all = kitty_clear_all;
  set_pixel_blitter(kitty_blit);
  sprite_init(ti, fd);
}

static bool
query_rgb(void){
  bool rgb = tigetflag("RGB") == 1;
  if(!rgb){
    // RGB terminfo capability being a new thing (as of ncurses 6.1), it's not commonly found in
    // terminal entries today. COLORTERM, however, is a de-facto (if imperfect/kludgy) standard way
    // of indicating TrueColor support for a terminal. The variable takes one of two case-sensitive
    // values:
    //
    //   truecolor
    //   24bit
    //
    // https://gist.github.com/XVilka/8346728#true-color-detection gives some more information about
    // the topic
    //
    const char* cterm = getenv("COLORTERM");
    rgb = cterm && (strcmp(cterm, "truecolor") == 0 || strcmp(cterm, "24bit") == 0);
  }
  return rgb;
}

int terminfostr(char** gseq, const char* name){
  char* seq;
  if(gseq == NULL){
    gseq = &seq;
  }
  *gseq = tigetstr(name);
  if(*gseq == NULL || *gseq == (char*)-1){
    *gseq = NULL;
    return -1;
  }
  // terminfo syntax allows a number N of milliseconds worth of pause to be
  // specified using $<N> syntax. this is then honored by tputs(). but we don't
  // use tputs(), instead preferring the much faster stdio+tiparm(). to avoid
  // dumping "$<N>" sequences all over stdio, we chop them out.
  char* pause;
  if( (pause = strchr(*gseq, '$')) ){
    *pause = '\0';
  }
  return 0;
}

// Qui si convien lasciare ogne sospetto; ogne viltà convien che qui sia morta.
static int
apply_term_heuristics(tinfo* ti, const char* termname, int fd){
  if(!termname){
    // setupterm interprets a missing/empty TERM variable as the special value “unknown”.
    termname = "unknown";
  }
  ti->braille = true; // most everyone has working braille, even from fonts
  if(strstr(termname, "kitty")){ // kitty (https://sw.kovidgoyal.net/kitty/)
    // see https://sw.kovidgoyal.net/kitty/protocol-extensions.html
    // FIXME detect the actual default background color; this assumes it to
    // be RGB(0, 0, 0) (the default). we could also just set it, i guess.
    ti->bg_collides_default = 0x1000000;
    ti->sextants = true; // work since bugfix in 0.19.3
    ti->quadrants = true;
    ti->pixel_query_done = true;
    ti->bitmap_supported = true;
    setup_kitty_bitmaps(ti, fd);
  }else if(strstr(termname, "alacritty")){
    ti->alacritty_sixel_hack = true;
    ti->quadrants = true;
    // ti->sextants = true; // alacritty https://github.com/alacritty/alacritty/issues/4409 */
  }else if(strstr(termname, "vte") || strstr(termname, "gnome") || strstr(termname, "xfce")){
    ti->sextants = true; // VTE has long enjoyed good sextant support
    ti->quadrants = true;
  }else if(strncmp(termname, "foot", 4) == 0){
    ti->sextants = true;
    ti->quadrants = true;
  }else if(strncmp(termname, "st", 2) == 0){
    // st had neithersextants nor quadrants last i checked (0.8.4)
  }else if(strstr(termname, "mlterm")){
    ti->quadrants = true; // good quadrants, no sextants as of 3.9.0
    ti->sprixel_cursor_hack = true;
  }else if(strstr(termname, "xterm")){
    // xterm has nothing beyond halfblocks. this is going to catch all kinds
    // of people using xterm when they shouldn't be, or even real database
    // entries like "xterm-kitty" (if we don't catch them above), giving a
    // pretty minimal (but safe) experience. set your TERM correctly!
  }else if(strcmp(termname, "linux") == 0){
    ti->braille = false; // no braille, no sextants in linux console
    // FIXME if the NCOPTION_NO_FONT_CHANGES, this isn't true
    // FIXME we probably want to do this based off ioctl()s in linux.c
    ti->quadrants = true; // we program quadrants on the console
  }
  // run a wcwidth(⣿) to guarantee libc Unicode 3 support, independent of term
  if(wcwidth(L'⣿') < 0){
    ti->braille = false;
  }
  // run a wcwidth(🬸) to guarantee libc Unicode 13 support, independent of term
  if(wcwidth(L'🬸') < 0){
    ti->sextants = false;
  }
  return 0;
}

void free_terminfo_cache(tinfo* ti){
  pthread_mutex_destroy(&ti->pixel_query);
}

// termname is just the TERM environment variable. some details are not
// exposed via terminfo, and we must make heuristic decisions based on
// the detected terminal type, yuck :/.
int interrogate_terminfo(tinfo* ti, int fd, const char* termname, unsigned utf8){
  memset(ti, 0, sizeof(*ti));
  ti->utf8 = utf8;
  ti->RGBflag = query_rgb();
  int colors = tigetnum("colors");
  if(colors <= 0){
    ti->colors = 1;
    ti->CCCflag = false;
    ti->RGBflag = false;
    ti->initc = NULL;
  }else{
    ti->colors = colors;
    terminfostr(&ti->initc, "initc");
    if(ti->initc){
      ti->CCCflag = tigetflag("ccc") == 1;
    }else{
      ti->CCCflag = false;
    }
  }
  // check that the terminal provides cursor addressing (absolute movement)
  terminfostr(&ti->cup, "cup");
  if(ti->cup == NULL){
    fprintf(stderr, "Required terminfo capability 'cup' not defined\n");
    return -1;
  }
  // check that the terminal provides automatic margins
  ti->AMflag = tigetflag("am") == 1;
  if(!ti->AMflag){
    fprintf(stderr, "Required terminfo capability 'am' not defined\n");
    return -1;
  }
  ti->BCEflag = tigetflag("bce") == 1;
  terminfostr(&ti->civis, "civis"); // cursor invisible
  if(ti->civis == NULL){
    terminfostr(&ti->civis, "chts");// hard-to-see cursor
  }
  terminfostr(&ti->cnorm, "cnorm"); // cursor normal (undo civis/cvvis)
  terminfostr(&ti->standout, "smso"); // begin standout mode
  terminfostr(&ti->uline, "smul");    // begin underline mode
  terminfostr(&ti->reverse, "rev");   // begin reverse video mode
  terminfostr(&ti->blink, "blink");   // turn on blinking
  terminfostr(&ti->dim, "dim");       // turn on half-bright mode
  terminfostr(&ti->bold, "bold");     // turn on extra-bright mode
  terminfostr(&ti->italics, "sitm");  // begin italic mode
  terminfostr(&ti->italoff, "ritm");  // end italic mode
  terminfostr(&ti->sgr, "sgr");       // define video attributes
  terminfostr(&ti->sgr0, "sgr0");     // turn off all video attributes
  terminfostr(&ti->op, "op");         // restore defaults to default pair
  terminfostr(&ti->oc, "oc");         // restore defaults to all colors
  terminfostr(&ti->home, "home");     // home the cursor
  terminfostr(&ti->clearscr, "clear");// clear screen, home cursor
  terminfostr(&ti->cuu, "cuu"); // move N up
  terminfostr(&ti->cud, "cud"); // move N down
  terminfostr(&ti->hpa, "hpa"); // set horizontal position
  terminfostr(&ti->vpa, "vpa"); // set verical position
  terminfostr(&ti->cuf, "cuf"); // n non-destructive spaces
  terminfostr(&ti->cub, "cub"); // n non-destructive backspaces
  terminfostr(&ti->cuf1, "cuf1"); // non-destructive space
  terminfostr(&ti->sc, "sc"); // push ("save") cursor
  terminfostr(&ti->rc, "rc"); // pop ("restore") cursor
  // Some terminals cannot combine certain styles with colors. Don't advertise
  // support for the style in that case.
  int nocolor_stylemask = tigetnum("ncv");
  if(nocolor_stylemask > 0){
    if(nocolor_stylemask & A_STANDOUT){ // ncv is composed of terminfo bits, not ours
      ti->standout = NULL;
    }
    if(nocolor_stylemask & A_UNDERLINE){
      ti->uline = NULL;
    }
    if(nocolor_stylemask & A_REVERSE){
      ti->reverse = NULL;
    }
    if(nocolor_stylemask & A_BLINK){
      ti->blink = NULL;
    }
    if(nocolor_stylemask & A_DIM){
      ti->dim = NULL;
    }
    if(nocolor_stylemask & A_BOLD){
      ti->bold = NULL;
    }
    if(nocolor_stylemask & A_ITALIC){
      ti->italics = NULL;
    }
    // can't do anything about struck! :/
  }
  terminfostr(&ti->getm, "getm"); // get mouse events
  // Not all terminals support setting the fore/background independently
  terminfostr(&ti->setaf, "setaf"); // set forground color
  terminfostr(&ti->setab, "setab"); // set background color
  terminfostr(&ti->smkx, "smkx");   // enable keypad transmit
  terminfostr(&ti->rmkx, "rmkx");   // disable keypad transmit
  terminfostr(&ti->struck, "smxx"); // strikeout
  terminfostr(&ti->struckoff, "rmxx"); // cancel strikeout
  // if the keypad neen't be explicitly enabled, smkx is not present
  if(ti->smkx){
    if(fd >= 0){
      if(tty_emit(tiparm(ti->smkx), fd) < 0){
        fprintf(stderr, "Error entering keypad transmit mode\n");
        return -1;
      }
    }
  }
  // if op is defined as ansi 39 + ansi 49, make the split definitions
  // available. this ought be asserted by extension capability "ax", but
  // no terminal i've found seems to do so. =[
  if(ti->op && strcmp(ti->op, "\x1b[39;49m") == 0){
    ti->fgop = "\x1b[39m";
    ti->bgop = "\x1b[49m";
  }
  pthread_mutex_init(&ti->pixel_query, NULL);
  ti->pixel_query_done = false;
  if(apply_term_heuristics(ti, termname, fd)){
    return -1;
  }
  return 0;
}

// FIXME need unit tests on this
// FIXME can read a character not intended for it
static int
read_xtsmgraphics_reply(int fd, int* val2){
  char in;
  // return is of the form CSI ? Pi ; 0 ; Pv S
  enum {
    WANT_CSI,
    WANT_QMARK,
    WANT_SEMI1,
    WANT_SEMI2,
    WANT_PV1,
    WANT_PV2,
    DONE
  } state = WANT_CSI;
  int pv = 0;
  while(read(fd, &in, 1) == 1){
//fprintf(stderr, "READ: %c 0x%02x\n", in, in);
    switch(state){
      case WANT_CSI:
        if(in == NCKEY_ESC){
          state = WANT_QMARK;
        }
        break;
      case WANT_QMARK:
        if(in == '?'){
          state = WANT_SEMI1;
        }
        break;
      case WANT_SEMI1:
        if(in == ';'){
          state = WANT_SEMI2;
        }
        break;
      case WANT_SEMI2:
        if(in == ';'){
          state = WANT_PV1;
        }
        break;
      case WANT_PV1:
        if(in == 'S'){
          state = DONE;
        }else if(val2 && in == ';'){
          *val2 = 0;
          state = WANT_PV2;
        }else if(isdigit(in)){
          pv *= 10;
          pv += in - '0';
        }
        break;
      case WANT_PV2:
        if(in == 'S'){
          state = DONE;
        }else if(isdigit(in)){
          *val2 *= 10;
          *val2 += in - '0';
        }
        break;
      case DONE:
      default:
        break;
    }
    if(state == DONE){
      if(pv >= 0 && (!val2 || *val2 >= 0)){
        return pv;
      }
      break;
    }
  }
  return -1;
}

static int
query_xtsmgraphics(int fd, const char* seq, int* val, int* val2){
  ssize_t w = writen(fd, seq, strlen(seq));
  if(w < 0 || (size_t)w != strlen(seq)){
    return -1;
  }
  int r = read_xtsmgraphics_reply(fd, val2);
  if(r <= 0){
    return -1;
  }
  *val = r;
  return 0;
}

// query for Sixel details including the number of color registers and, one day
// perhaps, maximum geometry. xterm binds its return by the current geometry,
// making it useless for a one-time query.
static int
query_sixel_details(tinfo* ti, int fd){
  if(query_xtsmgraphics(fd, "\x1b[?2;4;0S", &ti->sixel_maxx, &ti->sixel_maxy)){
    return -1;
  }
  if(query_xtsmgraphics(fd, "\x1b[?1;1;0S", &ti->color_registers, NULL)){
    return -1;
  }
//fprintf(stderr, "Sixel ColorRegs: %d Max_x: %d Max_y: %d\n", ti->color_registers, ti->sixel_maxx, ti->sixel_maxy);
  return 0;
}

// query for Sixel support
static int
query_sixel(tinfo* ti, int fd){
  // Send Device Attributes (see decTerminalID resource)
  if(writen(fd, "\x1b[c", 3) != 3){
    return -1;
  }
  char in;
  // perhaps the most lackadaisical response is that of st, which returns a
  // bare ESC[?6c (note no semicolon). this is equivalent to alacritty's
  // return, both suggesting a VT102. alacritty's miraculous technicolor VT102
  // can display sixel, but real VT102s don't even reply to XTSMGRAPHICS, so we
  // detect VT102 + TERM including alacritty, and special-case that.
  // FIXME need unit tests on this
  enum {
    WANT_CSI,
    WANT_QMARK,
    WANT_SEMI,
    WANT_C4, // want '4' to indicate sixel or 'c' to terminate
    WANT_VT102_C,
    DONE
  } state = WANT_CSI;
  bool in4 = false; // set true after seeing ";4", clear on semi
  // we're looking for a 4 following a semicolon
  while(state != DONE && read(fd, &in, 1) == 1){
    switch(state){
      case WANT_CSI:
        if(in == NCKEY_ESC){
          state = WANT_QMARK;
        }
        break;
      case WANT_QMARK:
        if(in == '?'){
          state = WANT_SEMI;
        }
        break;
      case WANT_SEMI:
        if(in == ';'){
          if(in4){
            setup_sixel_bitmaps(ti);
          }
          state = WANT_C4;
        }else if(in == 'c'){
          state = DONE;
        }else if(in == '6'){
          state = WANT_VT102_C;
        }
        in4 = false;
        break;
      case WANT_VT102_C:
        if(in == 'c'){
          // until graphics/ayosec is merged, alacritty doesn't actually
          // have sixel support. enable this then. FIXME
          /*if(ti->alacritty_sixel_hack){
            setup_sixel_bitmaps(ti);
          }*/
          state = DONE;
        }else if(in == ';'){
          state = WANT_C4;
        }
        break;
      case WANT_C4:
        if(in == 'c'){
          state = DONE;
        }else if(in == '4'){
          in4 = true;
          state = WANT_SEMI;
        }else{
          in4 = false;
        }
        break;
      case DONE:
      default:
        break;
    }
  }
  if(ti->bitmap_supported){
    query_sixel_details(ti, fd);
  }
  return 0;
}

// fd must be a real terminal. uses the query lock of |ti| to only act once.
// we ought already have performed a TIOCGWINSZ ioctl() to verify that the
// terminal reports cell area in pixels, as that's necessary for our use of
// sixel (or any other bitmap protocol).
int query_term(tinfo* ti, int fd){
  if(fd < 0){
    return -1;
  }
  int flags = fcntl(fd, F_GETFL, 0);
  if(flags < 0){
    return -1;
  }
  int ret = 0;
  pthread_mutex_lock(&ti->pixel_query);
  if(!ti->pixel_query_done){
    // if the terminal reported 0 pixels for cell dimensions, bypass any
    // interrogation, and assume no bitmap support.
    if(!ti->cellpixx || !ti->cellpixy){
      ti->pixel_query_done = true;
    }else{
      if(flags & O_NONBLOCK){
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
      }
      ret = query_sixel(ti, fd);
      ti->pixel_query_done = true;
      if(ti->bitmap_supported){
        ti->pixel_init(fd);
      }
      if(flags & O_NONBLOCK){
        fcntl(fd, F_SETFL, flags);
      }
    }
  }
  pthread_mutex_unlock(&ti->pixel_query);
  return ret;
}
