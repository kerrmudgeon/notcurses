#include "internal.h"

// Kitty has its own bitmap graphics protocol, rather superior to DEC Sixel.
// A header is written with various directives, followed by a number of
// chunks. Each chunk carries up to 4096B of base64-encoded pixels. Bitmaps
// can be ordered on a z-axis, with text at a logical z=0. A bitmap at a
// positive coordinate will be drawn above text; a negative coordinate will
// be drawn below text. It is not possible for a single bitmap to be under
// some text and above other text; since we need both, we draw at a positive
// coordinate (above all text), and cut out sections by setting their alpha
// values to 0. We thus require RGBA, meaning 768 pixels per 4096B chunk
// (768pix * 4Bpp * 4/3 base64 overhead == 4096B).
//
// How to reclaim a section once we no longer want to draw text above it is
// an open question; we'd presumably need to store the original alphas in
// the T-A matrix.
//
// It has some interesting features of which we do not yet take advantage:
//  * in-terminal scaling of image data (we prescale)
//  * subregion display of a transmitted bitmap
//  * an animation protocol we should probably use for video, and definitely
//     ought use for cell wiping
//  * movement (redisplay at another position) of loaded bitmaps
//
// https://sw.kovidgoyal.net/kitty/graphics-protocol.html
//
// We are unlikely to ever use several features: direct PNG support (only
// works for PNG), transfer via shared memory (only works locally),
// compression (only helps on easy graphics), or offsets within a cell.

// lookup table for base64
static unsigned const char b64subs[] =
 "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// convert a base64 character into its equivalent integer 0..63
static inline int
b64idx(char b64){
  if(b64 >= 'A' && b64 <= 'Z'){
    return b64 - 'A';
  }else if(b64 >= 'a' && b64 <= 'z'){
    return b64 - 'a' + 26;
  }else if(b64 >= '0' && b64 <= '9'){
    return b64 - '0' + 52;
  }else if(b64 == '+'){
    return 62;
  }else{
    return 63;
  }
}

// every 3 RGBA pixels (96 bits) become 16 base64-encoded bytes (128 bits). if
// there are only 2 pixels available, those 64 bits become 12 bytes. if there
// is only 1 pixel available, those 32 bits become 8 bytes. (pcount + 1) * 4
// bytes are used, plus a null terminator. we thus must receive 17.
static inline void
base64_rgba3(const uint32_t* pixels, size_t pcount, char* b64, bool wipe[static 3],
             uint32_t transcolor){
  uint32_t pixel = *pixels++;
  unsigned r = ncpixel_r(pixel);
  unsigned g = ncpixel_g(pixel);
  unsigned b = ncpixel_b(pixel);
  // we go ahead and take advantage of kitty's ability to reproduce 8-bit
  // alphas by copying it in directly, rather than mapping to {0, 255}.
  unsigned a = ncpixel_a(pixel);
  if(wipe[0] || rgba_trans_p(pixel, transcolor)){
    a = 0;
  }
  b64[0] = b64subs[(r & 0xfc) >> 2];
  b64[1] = b64subs[(r & 0x3 << 4) | ((g & 0xf0) >> 4)];
  b64[2] = b64subs[((g & 0xf) << 2) | ((b & 0xc0) >> 6)];
  b64[3] = b64subs[b & 0x3f];
  b64[4] = b64subs[(a & 0xfc) >> 2];
  if(pcount == 1){
    b64[5] = b64subs[(a & 0x3) << 4];
    b64[6] = '=';
    b64[7] = '=';
    b64[8] = '\0';
    return;
  }
  b64[5] = (a & 0x3) << 4;
  pixel = *pixels++;
  r = ncpixel_r(pixel);
  g = ncpixel_g(pixel);
  b = ncpixel_b(pixel);
  a = wipe[1] ? 0 : rgba_trans_p(pixel, transcolor) ? 0 : 255;
  b64[5] = b64subs[b64[5] | ((r & 0xf0) >> 4)];
  b64[6] = b64subs[((r & 0xf) << 2) | ((g & 0xc0) >> 6u)];
  b64[7] = b64subs[g & 0x3f];
  b64[8] = b64subs[(b & 0xfc) >> 2];
  b64[9] = b64subs[((b & 0x3) << 4) | ((a & 0xf0) >> 4)];
  if(pcount == 2){
    b64[10] = b64subs[(a & 0xf) << 2];
    b64[11] = '=';
    b64[12] = '\0';
    return;
  }
  b64[10] = (a & 0xf) << 2;
  pixel = *pixels;
  r = ncpixel_r(pixel);
  g = ncpixel_g(pixel);
  b = ncpixel_b(pixel);
  a = wipe[2] ? 0 : rgba_trans_p(pixel, transcolor) ? 0 : 255;
  b64[10] = b64subs[b64[10] | ((r & 0xc0) >> 6)];
  b64[11] = b64subs[r & 0x3f];
  b64[12] = b64subs[(g & 0xfc) >> 2];
  b64[13] = b64subs[((g & 0x3) << 4) | ((b & 0xf0) >> 4)];
  b64[14] = b64subs[((b & 0xf) << 2) | ((a & 0xc0) >> 6)];
  b64[15] = b64subs[a & 0x3f];
  b64[16] = '\0';
}

// null out part of a triplet (a triplet is 3 pixels, which map to 12 bytes, which map to
// 16 bytes when base64 encoded). skip the initial |skip| pixels, and null out a maximum
// of |max| pixels after that. returns the number of pixels nulled out. |max| must be
// positive. |skip| must be non-negative, and less than 3. |pleft| is the number of pixels
// available in the chunk. the RGB is 24 bits, and thus 4 base64 bytes, but
// unfortunately don't always start on a byte boundary =[.
// 0: R1(0..5)
// 1: R1(6..7), G1(0..3)
// 2: G1(4..7), B1(0..1)
// 3: B1(2..7)
// 4: A1(0..5)
// 5: A1(6..7), R2(0..3)
// 6: R2(4..7), G2(0..1)
// 7: G2(2..7)
// 8: B2(0..5)
// 9: B2(6..7), A2(0..3)
// A: A2(4..7), R3(0..1)
// B: R3(2..7)
// C: G3(0..5)
// D: G3(6..7), B3(0..3)
// E: B3(4..7), A3(0..1)
// F: A3(2..7)
// so we will only ever zero out bytes 4, 5, 9, A, E, and F

// get the first alpha from the triplet
static inline uint8_t
triplet_alpha1(const char* triplet){
  uint8_t c1 = b64idx(triplet[0x4]);
  uint8_t c2 = b64idx(triplet[0x5]);
  return (c1 << 2u) | ((c2 & 0x3) >> 4);
}

static inline uint8_t
triplet_alpha2(const char* triplet){
  uint8_t c1 = b64idx(triplet[0x9]);
  uint8_t c2 = b64idx(triplet[0xA]);
  return ((c1 & 0xf) << 4u) | ((c2 & 0x3c) >> 2);
}

static inline uint8_t
triplet_alpha3(const char* triplet){
  uint8_t c1 = b64idx(triplet[0xE]);
  uint8_t c2 = b64idx(triplet[0xF]);
  return ((c1 & 0x3) << 6u) | c2;
}

static inline int
kitty_null(char* triplet, int skip, int max, int pleft, uint8_t* auxvec){
//fprintf(stderr, "SKIP/MAX/PLEFT %d/%d/%d\n", skip, max, pleft);
  if(pleft > 3){
    pleft = 3;
  }
  if(max + skip > pleft){
    max = pleft - skip;
  }
//fprintf(stderr, "alpha-nulling %d after %d\n", max, skip);
  if(skip == 0){
    auxvec[0] = triplet_alpha1(triplet);
    triplet[0x4] = b64subs[0];
    triplet[0x5] = b64subs[b64idx(triplet[0x5]) & 0xf];
    if(max > 1){
      auxvec[1] = triplet_alpha2(triplet);
      triplet[0x9] = b64subs[b64idx(triplet[0x9]) & 0x30];
      triplet[0xA] = b64subs[b64idx(triplet[0xA]) & 0x3];
    }
    if(max == 3){
      auxvec[2] = triplet_alpha3(triplet);
      triplet[0xE] = b64subs[b64idx(triplet[0xE]) & 0x3c];
      triplet[0xF] = b64subs[0];
    }
  }else if(skip == 1){
    auxvec[0] = triplet_alpha2(triplet);
    triplet[0x9] = b64subs[b64idx(triplet[0x9]) & 0x30];
    triplet[0xA] = b64subs[b64idx(triplet[0xA]) & 0x3];
    if(max == 2){
      auxvec[1] = triplet_alpha3(triplet);
      triplet[0xE] = b64subs[b64idx(triplet[0xE]) & 0x3c];
      triplet[0xF] = b64subs[0];
    }
  }else{ // skip == 2
    auxvec[0] = triplet_alpha3(triplet);
    triplet[0xE] = b64subs[b64idx(triplet[0xE]) & 0x3c];
    triplet[0xF] = b64subs[0];
  }
  return max;
}

// restore part of a triplet (a triplet is 3 pixels, which map to 12 bytes,
// which map to 16 bytes when base64 encoded). skip the initial |skip| pixels,
// and restore a maximum of |max| pixels after that. returns the number of
// pixels restored. |max| must be positive. |skip| must be non-negative, and
// less than 3. |pleft| is the number of pixels available in the chunk.
// |state| is set to MIXED if we find transparent pixels.
static inline int
kitty_restore(char* triplet, int skip, int max, int pleft,
              const uint8_t* auxvec, sprixcell_e* state){
//fprintf(stderr, "SKIP/MAX/PLEFT %d/%d/%d auxvec %p\n", skip, max, pleft, auxvec);
  if(pleft > 3){
    pleft = 3;
  }
  if(max + skip > pleft){
    max = pleft - skip;
  }
  if(skip == 0){
    int a = auxvec[0];
    if(a == 0){
      *state = SPRIXCELL_MIXED_KITTY;
    }
    triplet[0x4] = b64subs[(a & 0xfc) >> 2];
    triplet[0x5] = b64subs[((a & 0x3) << 4) | (b64idx(triplet[0x5]) & 0xf)];
    if(max > 1){
      a = auxvec[1];
      if(a == 0){
        *state = SPRIXCELL_MIXED_KITTY;
      }
      triplet[0x9] = b64subs[(b64idx(triplet[0x9]) & 0x30) | ((a & 0xf0) >> 4)];
      triplet[0xA] = b64subs[((a & 0xf) << 2) | (b64idx(triplet[0xA]) & 0x3)];
    }
    if(max == 3){
      a = auxvec[2];
      if(a == 0){
        *state = SPRIXCELL_MIXED_KITTY;
      }
      triplet[0xE] = b64subs[((a & 0xc0) >> 6) | (b64idx(triplet[0xE]) & 0x3c)];
      triplet[0xF] = b64subs[(a & 0x3f)];
    }
  }else if(skip == 1){
    int a = auxvec[0];
    if(a == 0){
      *state = SPRIXCELL_MIXED_KITTY;
    }
    triplet[0x9] = b64subs[(b64idx(triplet[0x9]) & 0x30) | ((a & 0xf0) >> 4)];
    triplet[0xA] = b64subs[((a & 0xf) << 2) | (b64idx(triplet[0xA]) & 0x3)];
    if(max == 2){
      a = auxvec[1];
      if(a == 0){
        *state = SPRIXCELL_MIXED_KITTY;
      }
      triplet[0xE] = b64subs[((a & 0xc0) >> 6) | (b64idx(triplet[0xE]) & 0x3c)];
      triplet[0xF] = b64subs[(a & 0x3f)];
    }
  }else{ // skip == 2
    int a = auxvec[0];
    if(a == 0){
      *state = SPRIXCELL_MIXED_KITTY;
    }
    triplet[0xE] = b64subs[((a & 0xc0) >> 6) | (b64idx(triplet[0xE]) & 0x3c)];
    triplet[0xF] = b64subs[(a & 0x3f)];
  }
  return max;
}

#define RGBA_MAXLEN 768 // 768 base64-encoded pixels in 4096 bytes
// restore an annihilated sprixcell by copying the alpha values from the
// auxiliary vector back into the actual data. we then free the auxvector.
int kitty_rebuild(sprixel* s, int ycell, int xcell, uint8_t* auxvec){
  const int totalpixels = s->pixy * s->pixx;
  const int xpixels = s->cellpxx;
  const int ypixels = s->cellpxy;
  int targx = xpixels;
  if((xcell + 1) * xpixels > s->pixx){
    targx = s->pixx - xcell * xpixels;
  }
  int targy = ypixels;
  if((ycell + 1) * ypixels > s->pixy){
    targy = s->pixy - ycell * ypixels;
  }
  char* c = s->glyph + s->parse_start;
  int nextpixel = (s->pixx * ycell * ypixels) + (xpixels * xcell);
  int thisrow = targx;
  int chunkedhandled = 0;
  sprixcell_e state = SPRIXCELL_OPAQUE_KITTY;
  const int chunks = totalpixels / RGBA_MAXLEN + !!(totalpixels % RGBA_MAXLEN);
  int auxvecidx = 0;
  while(targy && chunkedhandled < chunks){ // need to null out |targy| rows of |targx| pixels, track with |thisrow|
    int inchunk = totalpixels - chunkedhandled * RGBA_MAXLEN;
    if(inchunk > RGBA_MAXLEN){
      inchunk = RGBA_MAXLEN;
    }
    const int curpixel = chunkedhandled * RGBA_MAXLEN;
    // a full chunk is 4096 + 2 + 7 (5005)
    while(nextpixel - curpixel < RGBA_MAXLEN && thisrow){
      // our next pixel is within this chunk. find the pixel offset of the
      // first pixel (within the chunk).
      int pixoffset = nextpixel - curpixel;
      int triples = pixoffset / 3;
      int tripbytes = triples * 16;
      int tripskip = pixoffset - triples * 3;
      // we start within a 16-byte chunk |tripbytes| into the chunk. determine
      // the number of bits.
//fprintf(stderr, "pixoffset: %d next: %d tripbytes: %d tripskip: %d thisrow: %d\n", pixoffset, nextpixel, tripbytes, tripskip, thisrow);
      // the maximum number of pixels we can convert is the minimum of the
      // pixels remaining in the target row, and the pixels left in the chunk.
//fprintf(stderr, "inchunk: %d total: %d triples: %d\n", inchunk, totalpixels, triples);
      int chomped = kitty_restore(c + tripbytes, tripskip, thisrow,
                                  inchunk - triples * 3, auxvec + auxvecidx,
                                  &state);
      assert(chomped >= 0);
      auxvecidx += chomped;
      thisrow -= chomped;
//fprintf(stderr, "POSTCHIMP CHOMP: %d pixoffset: %d next: %d tripbytes: %d tripskip: %d thisrow: %d\n", chomped, pixoffset, nextpixel, tripbytes, tripskip, thisrow);
      if(thisrow == 0){
//fprintf(stderr, "CLEARED ROW, TARGY: %d\n", targy - 1);
        if(--targy == 0){
          s->n->tam[s->dimx * ycell + xcell].state = state;
          return 0;
        }
        thisrow = targx;
//fprintf(stderr, "BUMP IT: %d %d %d %d\n", nextpixel, s->pixx, targx, chomped);
        nextpixel += s->pixx - targx + chomped;
      }else{
        nextpixel += chomped;
      }
    }
    c += RGBA_MAXLEN * 4 * 4 / 3; // 4bpp * 4/3 for base64, 4096b per chunk
    c += 8; // new chunk header
    ++chunkedhandled;
//fprintf(stderr, "LOOKING NOW AT %u [%s]\n", c - s->glyph, c);
    while(*c != ';'){
      ++c;
    }
    ++c;
  }
  return -1;
}

int kitty_wipe(sprixel* s, int ycell, int xcell){
//fprintf(stderr, "WIPING %d/%d\n", ycell, xcell);
  if(s->n->tam[s->dimx * ycell + xcell].state == SPRIXCELL_ANNIHILATED){
//fprintf(stderr, "CACHED WIPE %d %d/%d\n", s->id, ycell, xcell);
    return 0; // already annihilated, needn't draw glyph in kitty
  }
  uint8_t* auxvec = sprixel_auxiliary_vector(s);
//fprintf(stderr, "NEW WIPE %d %d/%d\n", s->id, ycell, xcell);
  const int totalpixels = s->pixy * s->pixx;
  const int xpixels = s->cellpxx;
  const int ypixels = s->cellpxy;
  // if the cell is on the right or bottom borders, it might only be partially
  // filled by actual graphic data, and we need to cap our target area.
  int targx = xpixels;
  if((xcell + 1) * xpixels > s->pixx){
    targx = s->pixx - xcell * xpixels;
  }
  int targy = ypixels;
  if((ycell + 1) * ypixels > s->pixy){
    targy = s->pixy - ycell * ypixels;
  }
  char* c = s->glyph + s->parse_start;
//fprintf(stderr, "TARGET AREA: %d x %d @ %dx%d of %d/%d (%d/%d) len %zu\n", targy, targx, ycell, xcell, s->dimy, s->dimx, s->pixy, s->pixx, strlen(c));
  // every pixel was 4 source bytes, 32 bits, 6.33 base64 bytes. every 3 input pixels is
  // 12 bytes (96 bits), an even 16 base64 bytes. there is chunking to worry about. there
  // are up to 768 pixels in a chunk.
  int nextpixel = (s->pixx * ycell * ypixels) + (xpixels * xcell);
  int thisrow = targx;
  int chunkedhandled = 0;
  const int chunks = totalpixels / RGBA_MAXLEN + !!(totalpixels % RGBA_MAXLEN);
  int auxvecidx = 0;
  while(targy && chunkedhandled < chunks){ // need to null out |targy| rows of |targx| pixels, track with |thisrow|
//fprintf(stderr, "PLUCKING FROM [%s]\n", c);
    int inchunk = totalpixels - chunkedhandled * RGBA_MAXLEN;
    if(inchunk > RGBA_MAXLEN){
      inchunk = RGBA_MAXLEN;
    }
    const int curpixel = chunkedhandled * RGBA_MAXLEN;
    // a full chunk is 4096 + 2 + 7 (5005)
    while(nextpixel - curpixel < RGBA_MAXLEN && thisrow){
      // our next pixel is within this chunk. find the pixel offset of the
      // first pixel (within the chunk).
      int pixoffset = nextpixel - curpixel;
      int triples = pixoffset / 3;
      int tripbytes = triples * 16;
      // we start within a 16-byte chunk |tripbytes| into the chunk. determine
      // the number of bits.
      int tripskip = pixoffset - triples * 3;
//fprintf(stderr, "pixoffset: %d next: %d tripbytes: %d tripskip: %d thisrow: %d\n", pixoffset, nextpixel, tripbytes, tripskip, thisrow);
      // the maximum number of pixels we can convert is the minimum of the
      // pixels remaining in the target row, and the pixels left in the chunk.
//fprintf(stderr, "inchunk: %d total: %d triples: %d\n", inchunk, totalpixels, triples);
      int chomped = kitty_null(c + tripbytes, tripskip, thisrow,
                               inchunk - triples * 3, auxvec + auxvecidx);
      assert(chomped >= 0);
      auxvecidx += chomped;
      assert(auxvecidx <= s->cellpxy * s->cellpxx);
      thisrow -= chomped;
//fprintf(stderr, "POSTCHIMP CHOMP: %d pixoffset: %d next: %d tripbytes: %d tripskip: %d thisrow: %d\n", chomped, pixoffset, nextpixel, tripbytes, tripskip, thisrow);
      if(thisrow == 0){
//fprintf(stderr, "CLEARED ROW, TARGY: %d\n", targy - 1);
        if(--targy == 0){
          s->n->tam[s->dimx * ycell + xcell].auxvector = auxvec;
          s->invalidated = SPRIXEL_INVALIDATED;
          return 1;
        }
        thisrow = targx;
//fprintf(stderr, "BUMP IT: %d %d %d %d\n", nextpixel, s->pixx, targx, chomped);
        nextpixel += s->pixx - targx + chomped;
      }else{
        nextpixel += chomped;
      }
    }
    c += RGBA_MAXLEN * 4 * 4 / 3; // 4bpp * 4/3 for base64, 4096b per chunk
    c += 8; // new chunk header
    ++chunkedhandled;
//fprintf(stderr, "LOOKING NOW AT %u [%s]\n", c - s->glyph, c);
    while(*c != ';'){
      ++c;
    }
    ++c;
  }
  free(auxvec);
  return -1;
}

// we can only write 4KiB at a time. we're writing base64-encoded RGBA. each
// pixel is 4B raw (32 bits). each chunk of three pixels is then 12 bytes, or
// 16 base64-encoded bytes. 4096 / 16 == 256 3-pixel groups, or 768 pixels.
// closes |fp| on all paths.
static int
write_kitty_data(FILE* fp, int linesize, int leny, int lenx,
                 int cols, const uint32_t* data, int cdimy, int cdimx,
                 int sprixelid, tament* tam, int* parse_start,
                 uint32_t transcolor){
  if(linesize % sizeof(*data)){
    fclose(fp);
    return -1;
  }
  int total = leny * lenx; // total number of pixels (4 * total == bytecount)
  // number of 4KiB chunks we'll need
  int chunks = (total + (RGBA_MAXLEN - 1)) / RGBA_MAXLEN;
  int totalout = 0; // total pixels of payload out
  int y = 0; // position within source image (pixels)
  int x = 0;
  int targetout = 0; // number of pixels expected out after this chunk
//fprintf(stderr, "total: %d chunks = %d, s=%d,v=%d\n", total, chunks, lenx, leny);
  while(chunks--){
    if(totalout == 0){
      *parse_start = fprintf(fp, "\e_Gf=32,s=%d,v=%d,i=%d,a=T,%c=1;",
                             lenx, leny, sprixelid, chunks ? 'm' : 'q');
    }else{
      fprintf(fp, "\e_G%sm=%d;", chunks ? "" : "q=1,", chunks ? 1 : 0);
    }
    if((targetout += RGBA_MAXLEN) > total){
      targetout = total;
    }
    while(totalout < targetout){
      int encodeable = targetout - totalout;
      if(encodeable > 3){
        encodeable = 3;
      }
      uint32_t source[3]; // we encode up to 3 pixels at a time
      bool wipe[3];
      for(int e = 0 ; e < encodeable ; ++e){
        if(x == lenx){
          x = 0;
          ++y;
        }
        const uint32_t* line = data + (linesize / sizeof(*data)) * y;
        source[e] = line[x];
//fprintf(stderr, "%u/%u/%u -> %c%c%c%c %u %u %u %u\n", r, g, b, b64[0], b64[1], b64[2], b64[3], b64[0], b64[1], b64[2], b64[3]);
        int xcell = x / cdimx;
        int ycell = y / cdimy;
        int tyx = xcell + ycell * cols;
//fprintf(stderr, "Tyx: %d y: %d (%d) * %d x: %d (%d) state %d\n", tyx, y, y / cdimy, cols, x, x / cdimx, tam[tyx].state);
        if(tam[tyx].state == SPRIXCELL_ANNIHILATED || tam[tyx].state == SPRIXCELL_ANNIHILATED_TRANS){
          wipe[e] = 1;
        }else{
          wipe[e] = 0;
          if(rgba_trans_p(source[e], transcolor)){
            if(x % cdimx == 0 && y % cdimy == 0){
              tam[tyx].state = SPRIXCELL_TRANSPARENT;
            }else if(tam[tyx].state == SPRIXCELL_OPAQUE_KITTY){
              tam[tyx].state = SPRIXCELL_MIXED_KITTY;
            }
          }else{
            if(x % cdimx == 0 && y % cdimy == 0){
              tam[tyx].state = SPRIXCELL_OPAQUE_KITTY;
            }else if(tam[tyx].state == SPRIXCELL_TRANSPARENT){
              tam[tyx].state = SPRIXCELL_MIXED_KITTY;
            }
          }
        }
        ++x;
      }
      totalout += encodeable;
      char out[17];
      base64_rgba3(source, encodeable, out, wipe, transcolor);
      ncfputs(out, fp);
    }
    fprintf(fp, "\e\\");
  }
  if(fclose(fp) == EOF){
    return -1;
  }
  scrub_tam_boundaries(tam, leny, lenx, cdimy, cdimx);
  return 0;
}
#undef RGBA_MAXLEN

// Kitty graphics blitter. Kitty can take in up to 4KiB at a time of (optionally
// deflate-compressed) 24bit RGB. Returns -1 on error, 1 on success.
int kitty_blit(ncplane* n, int linesize, const void* data,
               int leny, int lenx, const blitterargs* bargs){
  int cols = bargs->u.pixel.spx->dimx;
  int rows = bargs->u.pixel.spx->dimy;
  char* buf = NULL;
  size_t size = 0;
  FILE* fp = open_memstream(&buf, &size);
  if(fp == NULL){
    return -1;
  }
  tament* tam = NULL;
  bool reuse = false;
  // if we have a sprixel attached to this plane, see if we can reuse it
  // (we need the same dimensions) and thus immediately apply its T-A table.
  if(n->tam){
    if(n->leny == rows && n->lenx == cols){
      tam = n->tam;
      reuse = true;
    }
  }
  int parse_start = 0;
  if(!reuse){
    tam = malloc(sizeof(*tam) * rows * cols);
    if(tam == NULL){
      fclose(fp);
      free(buf);
      return -1;
    }
    memset(tam, 0, sizeof(*tam) * rows * cols);
  }
  // closes fp on all paths
  if(write_kitty_data(fp, linesize, leny, lenx, cols, data,
                      bargs->u.pixel.celldimy, bargs->u.pixel.celldimx,
                      bargs->u.pixel.spx->id, tam, &parse_start,
                      bargs->transcolor)){
    if(!reuse){
      free(tam);
    }
    free(buf);
    return -1;
  }
  // take ownership of |buf| and |tam| on success
  if(plane_blit_sixel(bargs->u.pixel.spx, buf, size, rows, cols,
                      leny, lenx, parse_start, tam) < 0){
    if(!reuse){
      free(tam);
    }
    free(buf);
    return -1;
  }
  return 1;
}

int kitty_remove(int id, FILE* out){
//fprintf(stderr, "DESTROYING KITTY %d\n", id);
  if(fprintf(out, "\e_Ga=d,d=i,i=%d\e\\", id) < 0){
    return -1;
  }
  return 0;
}

// removes the kitty bitmap graphic identified by s->id, and damages those
// cells which weren't SPRIXCEL_OPAQUE
int kitty_destroy(const notcurses* nc, const ncpile* p, FILE* out, sprixel* s){
  if(kitty_remove(s->id, out)){
    return -1;
  }
//fprintf(stderr, "FROM: %d/%d state: %d s->n: %p\n", s->movedfromy, s->movedfromx, s->invalidated, s->n);
  for(int yy = s->movedfromy ; yy < s->movedfromy + s->dimy && yy < p->dimy ; ++yy){
    for(int xx = s->movedfromx ; xx < s->movedfromx + s->dimx && xx < p->dimx ; ++xx){
      struct crender *r = &p->crender[yy * p->dimx + xx];
      if(!r->sprixel){
        if(s->n){
          const ncplane* stdn = notcurses_stdplane_const(nc);
//fprintf(stderr, "CHECKING %d/%d\n", yy - s->movedfromy, xx - s->movedfromx);
          sprixcell_e state = sprixel_state(s, yy - s->movedfromy + s->n->absy - stdn->absy,
                                              xx - s->movedfromx + s->n->absx - stdn->absx);
          if(state == SPRIXCELL_OPAQUE_KITTY){
            r->s.damaged = 1;
          }else if(s->invalidated == SPRIXEL_MOVED){
            // ideally, we wouldn't damage our annihilated sprixcells, but if
            // we're being annihilated only during this cycle, we need to go
            // ahead and damage it.
            r->s.damaged = 1;
          }
        }else{
          r->s.damaged = 1;
        }
      }
    }
  }
  return 0;
}

int kitty_draw(const ncpile* p, sprixel* s, FILE* out){
//fprintf(stderr, "DRAWING %d\n", s->id);
  (void)p;
  int ret = 0;
  if(fwrite(s->glyph, s->glyphlen, 1, out) != 1){
    ret = -1;
  }
  s->invalidated = SPRIXEL_QUIESCENT;
  return ret;
}

// clears all kitty bitmaps
int kitty_clear_all(int fd){
//fprintf(stderr, "KITTY UNIVERSAL ERASE\n");
  return tty_emit("\e_Ga=d\e\\", fd);
}

int kitty_shutdown(int fd){
  // FIXME need to close off any open kitty bitmap emission, or we will
  // lock up the terminal
  (void)fd;
  return 0;
}
