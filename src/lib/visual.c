#include <math.h>
#include <string.h>
#include "builddef.h"
#include "visual-details.h"
#include "internal.h"

const ncvisual_implementation* visual_implementation = NULL;

int ncvisual_decode(ncvisual* nc){
  if(!visual_implementation){
    return -1;
  }
  return visual_implementation->visual_decode(nc);
}

int ncvisual_blit(ncvisual* ncv, int rows, int cols, ncplane* n,
                  const struct blitset* bset, const blitterargs* barg){
  int ret = -1;
  if(visual_implementation){
    if(visual_implementation->visual_blit(ncv, rows, cols, n, bset, barg) >= 0){
      ret = 0;
    }
  }else{
    if(rgba_blit_dispatch(n, bset, ncv->rowstride, ncv->data, rows, cols, barg) >= 0){
      ret = 0;
    }
  }
  return ret;
}

// ncv constructors other than ncvisual_from_file() need to set up the
// AVFrame* 'frame' according to their own data, which is assumed to
// have been prepared already in 'ncv'.
void ncvisual_details_seed(struct ncvisual* ncv){
  if(visual_implementation){
    visual_implementation->visual_details_seed(ncv);
  }
}

int ncvisual_init(int loglevel){
  if(visual_implementation){
    return visual_implementation->visual_init(loglevel);
  }
  return 0;
}

ncvisual* ncvisual_from_file(const char* filename){
  if(!visual_implementation){
    return NULL;
  }
  return visual_implementation->visual_from_file(filename);
}

ncvisual* ncvisual_create(void){
  if(visual_implementation){
    return visual_implementation->visual_create();
  }
  ncvisual* ret = malloc(sizeof(*ret));
  memset(ret, 0, sizeof(*ret));
  return ret;
}

void ncvisual_printbanner(const notcurses* nc){
  if(visual_implementation){
    visual_implementation->visual_printbanner(nc);
  }
}

static inline void
ncvisual_origin(const struct ncvisual_options* vopts, int* restrict begy, int* restrict begx){
  *begy = vopts ? vopts->begy : 0;
  *begx = vopts ? vopts->begx : 0;
}

// 'leny' and 'lenx' get the number of pixels to actually be rendered, 'y' and
// 'x' get the original size of the visual in pixels, and 'scaley' and 'scalex'
// get the number of pixels per cell with the selected 'blitter'.
// FIXME we ought also do the output calculations here (how many rows x cols,
//   given the input plane vopts->n and scaling vopts->scaling)--but do not
//   perform any actual scaling, nor create any planes!
static int
ncvisual_blitset_geom(const notcurses* nc, const ncvisual* n,
                      const struct ncvisual_options* vopts,
                      int* y, int* x, int* scaley, int* scalex,
                      int* leny, int* lenx, const struct blitset** blitter){
  int fakeleny, fakelenx;
  if(leny == NULL){
    leny = &fakeleny;
  }
  if(lenx == NULL){
    lenx = &fakelenx;
  }
  if(vopts && vopts->flags >= (NCVISUAL_OPTION_ADDALPHA << 1u)){
    logwarn(nc, "Warning: unknown ncvisual options %016jx\n", (uintmax_t)vopts->flags);
  }
  int begy, begx;
  ncvisual_origin(vopts, &begy, &begx);
  *lenx = vopts ? vopts->lenx : 0;
  *leny = vopts ? vopts->leny : 0;
//fprintf(stderr, "blit %dx%d+%dx%d %p\n", begy, begx, *leny, *lenx, ncv->data);
  if(begy < 0 || begx < 0 || *lenx <= -1 || *leny <= -1){
    logerror(nc, "Invalid geometry for visual %d %d %d %d\n", begy, begx, *leny, *lenx);
    return -1;
  }
  if(n){
//fprintf(stderr, "OUR DATA: %p rows/cols: %d/%d\n", n->data, n->pixy, n->pixx);
    if(n->data == NULL){
      logerror(nc, "No data in visual\n");
      return -1;
    }
//fprintf(stderr, "blit %d/%d to %dx%d+%dx%d scaling: %d\n", n->pixy, n->pixx, begy, begx, *leny, *lenx, vopts ? vopts->scaling : 0);
    if(begx >= n->pixx || begy >= n->pixy){
      logerror(nc, "Visual too large %d > %d or %d > %d\n", begy, n->pixy, begx, n->pixx);
      return -1;
    }
    if(*lenx == 0){ // 0 means "to the end"; use all available source material
      *lenx = n->pixx - begx;
    }
    if(*leny == 0){
      *leny = n->pixy - begy;
    }
//fprintf(stderr, "blit %d/%d to %dx%d+%dx%d scaling: %d flags: 0x%016lx\n", n->pixy, n->pixx, begy, begx, *leny, *lenx, vopts ? vopts->scaling : 0, vopts ? vopts->flags : 0);
    if(*lenx <= 0 || *leny <= 0){ // no need to draw zero-size object, exit
      logerror(nc, "Zero-size object %d %d\n", *leny, *lenx);
      return -1;
    }
    if(begx + *lenx > n->pixx || begy + *leny > n->pixy){
      logerror(nc, "Geometry too large %d > %d or %d > %d\n", begy + *leny, n->pixy, begx + *lenx, n->pixx);
      return -1;
    }
  }
  const struct blitset* bset = rgba_blitter(nc, vopts);
  if(!bset){
    logerror(nc, "Couldn't get a blitter for %d\n", vopts ? vopts->blitter : NCBLIT_DEFAULT);
    return -1;
  }
  if(blitter){
    *blitter = bset;
  }
  if(bset->geom == NCBLIT_PIXEL && vopts){
    if(vopts->n){
      if(vopts->n == notcurses_stdplane_const(nc)){
        logerror(nc, "Won't blit bitmaps to the standard plane\n");
        return -1;
      }
      if(vopts->y && !(vopts->flags & NCVISUAL_OPTION_VERALIGNED)){
        logerror(nc, "Non-origin y placement %d for sprixel\n", vopts->y);
        return -1;
      }
      if(vopts->x && !(vopts->flags & NCVISUAL_OPTION_HORALIGNED)){
        logerror(nc, "Non-origin x placement %d for sprixel\n", vopts->x);
        return -1;
      }
      // FIXME clamp to sprixel limits
      if(vopts->scaling == NCSCALE_NONE || vopts->scaling == NCSCALE_NONE_HIRES){
        int rows = (*leny + nc->tcache.cellpixy - 1) / nc->tcache.cellpixy;
        if(rows > ncplane_dim_y(vopts->n)){
          logerror(nc, "Sprixel too tall %d for plane %d\n", *leny, ncplane_dim_y(vopts->n) * nc->tcache.cellpixy);
          return -1;
        }
        int cols = (*lenx + nc->tcache.cellpixx - 1) / nc->tcache.cellpixx;
        if(cols > ncplane_dim_x(vopts->n)){
          logerror(nc, "Sprixel too wide %d for plane %d\n", *lenx, ncplane_dim_x(vopts->n) * nc->tcache.cellpixx);
          return -1;
        }
      }
    }
  }
  if(n){
    if(y){
      *y = n->pixy;
    }
    if(x){
      *x = n->pixx;
    }
  }
  if(scaley){
    *scaley = encoding_y_scale(&nc->tcache, bset);
  }
  if(scalex){
    *scalex = encoding_x_scale(&nc->tcache, bset);
  }
  if(vopts && vopts->flags & NCVISUAL_OPTION_HORALIGNED){
    if(vopts->x < NCALIGN_UNALIGNED || vopts->x > NCALIGN_RIGHT){
      logerror(nc, "Bad x %d for horizontal alignment\n", vopts->x);
      return -1;
    }
  }
  if(vopts && vopts->flags & NCVISUAL_OPTION_VERALIGNED){
    if(vopts->y < NCALIGN_UNALIGNED || vopts->y > NCALIGN_RIGHT){
      logerror(nc, "Bad y %d for vertical alignment\n", vopts->y);
      return -1;
    }
  }
  return 0;
}

int ncvisual_blitter_geom(const notcurses* nc, const ncvisual* n,
                          const struct ncvisual_options* vopts,
                          int* y, int* x, int* scaley, int* scalex,
                          ncblitter_e* blitter){
  const struct blitset* bset;
  int ret = ncvisual_blitset_geom(nc, n, vopts, y, x, scaley, scalex,
                                  NULL, NULL, &bset);
  if(ret == 0 && blitter){
    *blitter = bset->geom;
  }
  return ret;
}

void* bgra_to_rgba(const void* data, int rows, int rowstride, int cols){
  if(rowstride % 4){ // must be a multiple of 4 bytes
    return NULL;
  }
  uint32_t* ret = malloc(rowstride * rows);
  if(ret){
    for(int y = 0 ; y < rows ; ++y){
      for(int x = 0 ; x < cols ; ++x){
        const uint32_t* src = (const uint32_t*)data + (rowstride / 4) * y + x;
        uint32_t* dst = ret + (rowstride / 4) * y + x;
        ncpixel_set_a(dst, 0xff);
        ncpixel_set_r(dst, ncpixel_b(*src));
        ncpixel_set_g(dst, ncpixel_g(*src));
        ncpixel_set_b(dst, ncpixel_r(*src));
      }
    }
  }
  return ret;
}

// Inspects the visual to find the minimum rectangle that can contain all
// "real" pixels, where "real" pixels are, by convention, all zeroes.
// Placing this box at offyXoffx relative to the visual will encompass all
// pixels. Returns the area of the box (0 if there are no pixels).
int ncvisual_bounding_box(const ncvisual* ncv, int* leny, int* lenx,
                          int* offy, int* offx){
  int trow, lcol = -1, rcol = INT_MAX; // FIXME shouldn't need lcol init
  // first, find the topmost row with a real pixel. if there is no such row,
  // there are no such pixels. if we find one, we needn't look in this region
  // for other extrema, so long as we keep the leftmost and rightmost through
  // this row (from the top). said leftmost and rightmost will be the leftmost
  // and rightmost pixel of whichever row has the topmost valid pixel. unlike
  // the topmost, they'll need be further verified.
  for(trow = 0 ; trow < ncv->pixy ; ++trow){
    int x;
    for(x = 0 ; x < ncv->pixx ; ++x){
      uint32_t rgba = ncv->data[trow * ncv->rowstride / 4 + x];
      if(rgba){
        lcol = x; // leftmost pixel of topmost row
        // now find rightmost pixel of topmost row
        int xr;
        for(xr = ncv->pixx - 1 ; xr > x ; --xr){
          rgba = ncv->data[trow * ncv->rowstride / 4 + xr];
          if(rgba){ // rightmost pixel of topmost row
            break;
          }
        }
        rcol = xr;
        break;
      }
    }
    if(rcol < INT_MAX){
      break;
    }
  }
  if(trow == ncv->pixy){ // no real pixels
    *leny = 0;
    *lenx = 0;
    *offy = 0;
    *offx = 0;
  }else{
    assert(lcol >= 0);
    assert(rcol < ncv->pixx);
    // we now know topmost row, and left/rightmost through said row. now we must
    // find the bottommost row, checking left/rightmost throughout.
    int brow;
    for(brow = ncv->pixy - 1 ; brow > trow ; --brow){
      int x;
      for(x = 0 ; x < ncv->pixx ; ++x){
        uint32_t rgba = ncv->data[brow * ncv->rowstride / 4 + x];
        if(rgba){
          if(x < lcol){
            lcol = x;
          }
          int xr;
          for(xr = ncv->pixx - 1 ; xr > x && xr > rcol ; --xr){
            rgba = ncv->data[brow * ncv->rowstride / 4 + xr];
            if(rgba){ // rightmost pixel of bottommost row
              if(xr > rcol){
                rcol = xr;
              }
              break;
            }
          }
          break;
        }
      }
      if(x < ncv->pixx){
        break;
      }
    }
    // we now know topmost and bottommost row, and left/rightmost within those
    // two sections. now check the rest for left and rightmost.
    for(int y = trow + 1 ; y < brow ; ++y){
      for(int x = 0 ; x < lcol ; ++x){
        uint32_t rgba = ncv->data[y * ncv->rowstride / 4 + x];
        if(rgba){
          lcol = x;
          break;
        }
      }
      for(int x = ncv->pixx - 1 ; x > rcol ; --x){
        uint32_t rgba = ncv->data[y * ncv->rowstride / 4 + x];
        if(rgba){
          rcol = x;
          break;
        }
      }
    }
    *offy = trow;
    *leny = brow - trow + 1;
    *offx = lcol;
    *lenx = rcol - lcol + 1;
  }
  return *leny * *lenx;
}

// find the "center" cell of a visual. in the case of even rows/columns, we
// place the center on the top/left. in such a case there will be one more
// cell to the bottom/right of the center.
static inline void
ncvisual_center(const ncvisual* n, int* RESTRICT y, int* RESTRICT x){
  *y = n->pixy;
  *x = n->pixx;
  center_box(y, x);
}

// rotate the 0-indexed (origin-indexed) ['y', 'x'] through 'ctheta' and
// 'stheta' around the centerpoint at ['centy', 'centx']. write the results
// back to 'y' and 'x'.
static void
rotate_point(int* y, int* x, double stheta, double ctheta, int centy, int centx){
  // convert coordinates from origin to left-handed cartesian
  const int convx = *x - centx;
  const int convy = *y - centy;
//fprintf(stderr, "%d, %d -> conv %d, %d\n", *y, *x, convy, convx);
  *x = round(convx * ctheta - convy * stheta);
  *y = round(convx * stheta + convy * ctheta);
}

// rotate the specified bounding box by the specified sine and cosine of some
// theta radians, enlarging or shrinking it as necessary. returns the area.
// 'leny', 'lenx', 'offy', and 'offx' describe the bounding box to be rotated,
// and might all be updated (in either direction).
static int
rotate_bounding_box(double stheta, double ctheta, int* leny, int* lenx,
                    int* offy, int* offx){
//fprintf(stderr, "Incoming bounding box: %dx%d @ %dx%d rotate s(%f) c(%f)\n", *leny, *lenx, *offy, *offx, stheta, ctheta);
  int xs[4], ys[4]; // x and y locations of rotated coordinates
  int centy = *leny;
  int centx = *lenx;
  center_box(&centy, &centx);
  ys[0] = 0;
  xs[0] = 0;
  rotate_point(ys, xs, stheta, ctheta, centy, centx);
//fprintf(stderr, "rotated %d, %d -> %d %d\n", 0, 0, ys[0], xs[0]);
  ys[1] = 0;
  xs[1] = *lenx - 1;
  rotate_point(ys + 1, xs + 1, stheta, ctheta, centy, centx);
//fprintf(stderr, "rotated %d, %d -> %d %d\n", 0, *lenx - 1, ys[1], xs[1]);
  ys[2] = *leny - 1;
  xs[2] = *lenx - 1;
  rotate_point(ys + 2, xs + 2, stheta, ctheta, centy, centx);
//fprintf(stderr, "rotated %d, %d -> %d %d\n", *leny - 1, *lenx - 1, ys[2], xs[2]);
  ys[3] = *leny - 1;
  xs[3] = 0;
  rotate_point(ys + 3, xs + 3, stheta, ctheta, centy, centx);
//fprintf(stderr, "rotated %d, %d -> %d %d\n", *leny - 1, 0, ys[3], xs[3]);
  int trow = ys[0];
  int brow = ys[0];
  int lcol = xs[0];
  int rcol = xs[0];
  for(size_t i = 1 ; i < sizeof(xs) / sizeof(*xs) ; ++i){
    if(xs[i] < lcol){
      lcol = xs[i];
    }
    if(xs[i] > rcol){
      rcol = xs[i];
    }
    if(ys[i] < trow){
      trow = ys[i];
    }
    if(ys[i] > brow){
      brow = ys[i];
    }
  }
  *offy = trow;
  *leny = brow - trow + 1;
  *offx = lcol;
  *lenx = rcol - lcol + 1;
//fprintf(stderr, "Rotated bounding box: %dx%d @ %dx%d\n", *leny, *lenx, *offy, *offx);
  return *leny * *lenx;
}

int ncvisual_rotate(ncvisual* ncv, double rads){
  // done to force conversion into RGBA
  int err = ncvisual_resize(ncv, ncv->pixy, ncv->pixx);
  if(err){
    return err;
  }
  assert(ncv->rowstride / 4 >= ncv->pixx);
  rads = -rads; // we're a left-handed Cartesian
  if(ncv->data == NULL){
    return -1;
  }
  int centy, centx;
  ncvisual_center(ncv, &centy, &centx); // pixel center (center of 'data')
  double stheta, ctheta; // sine, cosine
  stheta = sin(rads);
  ctheta = cos(rads);
  // bounding box for real data within the ncvisual. we must only resize to
  // accommodate real data, lest we grow without band as we rotate.
  // see https://github.com/dankamongmen/notcurses/issues/599.
  int bby = ncv->pixy;
  int bbx = ncv->pixx;
  int bboffy = 0;
  int bboffx = 0;
  if(ncvisual_bounding_box(ncv, &bby, &bbx, &bboffy, &bboffx) <= 0){
    return -1;
  }
  int bbarea;
  bbarea = rotate_bounding_box(stheta, ctheta, &bby, &bbx, &bboffy, &bboffx);
  if(bbarea <= 0){
    return -1;
  }
  int bbcentx = bbx, bbcenty = bby;
  center_box(&bbcenty, &bbcentx);
//fprintf(stderr, "stride: %d height: %d width: %d\n", ncv->rowstride, ncv->pixy, ncv->pixx);
  assert(ncv->rowstride / 4 >= ncv->pixx);
  uint32_t* data = malloc(bbarea * 4);
  if(data == NULL){
    return -1;
  }
  memset(data, 0, bbarea * 4);
//fprintf(stderr, "bbarea: %d bby: %d bbx: %d centy: %d centx: %d bbcenty: %d bbcentx: %d\n", bbarea, bby, bbx, centy, centx, bbcenty, bbcentx);
  for(int y = 0 ; y < ncv->pixy ; ++y){
      for(int x = 0 ; x < ncv->pixx ; ++x){
      int targx = x, targy = y;
      rotate_point(&targy, &targx, stheta, ctheta, centy, centx);
      const int deconvx = targx - bboffx;
      const int deconvy = targy - bboffy;
      if(deconvy >= 0 && deconvx >= 0 && deconvy < bby && deconvx < bbx){
        data[deconvy * bbx + deconvx] = ncv->data[y * (ncv->rowstride / 4) + x];
      }
//fprintf(stderr, "CW: %d/%d (%08x) -> %d/%d (stride: %d)\n", y, x, ncv->data[y * (ncv->rowstride / 4) + x], targy, targx, ncv->rowstride);
//fprintf(stderr, "wrote %08x to %d (%d)\n", data[targy * ncv->pixy + targx], targy * ncv->pixy + targx, (targy * ncv->pixy + targx) * 4);
    }
  }
  ncvisual_set_data(ncv, data, true);
  ncv->pixx = bbx;
  ncv->pixy = bby;
  ncv->rowstride = bbx * 4;
  ncvisual_details_seed(ncv);
  return 0;
}

ncvisual* ncvisual_from_rgba(const void* rgba, int rows, int rowstride, int cols){
  if(rowstride % 4){
    return NULL;
  }
  ncvisual* ncv = ncvisual_create();
  if(ncv){
    ncv->rowstride = rowstride;
    ncv->pixx = cols;
    ncv->pixy = rows;
    uint32_t* data = memdup(rgba, rowstride * ncv->pixy);
//fprintf(stderr, "COPY US %zu (%d)\n", rowstride * ncv->pixy, ncv->pixy);
    if(data == NULL){
      ncvisual_destroy(ncv);
      return NULL;
    }
//fprintf(stderr, "ROWS: %d STRIDE: %d (%d) COLS: %d\n", rows, rowstride, rowstride / 4, cols);
    ncvisual_set_data(ncv, data, true);
    ncvisual_details_seed(ncv);
  }
  return ncv;
}

ncvisual* ncvisual_from_bgra(const void* bgra, int rows, int rowstride, int cols){
  if(rowstride % 4){
    return NULL;
  }
  ncvisual* ncv = ncvisual_create();
  if(ncv){
    ncv->rowstride = rowstride;
    ncv->pixx = cols;
    ncv->pixy = rows;
    uint32_t* data = memdup(bgra, rowstride * ncv->pixy);
    for(int p = 0 ; p < rowstride / 4 * ncv->pixy ; ++p){
      const unsigned r = (data[p] & 0xffllu) << 16u;
      const unsigned b = (data[p] & 0xff0000llu) >> 16u;
      data[p] = (data[p] & 0xff00ff00llu) | r | b;
    }
    if(data == NULL){
      ncvisual_destroy(ncv);
      return NULL;
    }
    ncvisual_set_data(ncv, data, true);
    ncvisual_details_seed(ncv);
  }
  return ncv;
}

// by the end, disprows/dispcols refer to the number of source rows/cols (in
// pixels), which will be mapped to a region of cells scaled by the encodings).
// the blit will begin at placey/placex (in terms of cells). begy/begx define
// the origin of the source region to draw (in pixels). leny/lenx defined the
// geometry of the source region to draw, again in pixels. ncv->pixy and
// ncv->pixx define the source geometry in pixels.
ncplane* ncvisual_render_cells(notcurses* nc, ncvisual* ncv, const struct blitset* bset,
                               int placey, int placex, int begy, int begx,
                               ncplane* n, ncscale_e scaling,
                               uint64_t flags, uint32_t transcolor){
  int disprows, dispcols;
  ncplane* createdn = NULL;
//fprintf(stderr, "INPUT N: %p\n", n);
  if(n == NULL){ // create plane
    if(scaling == NCSCALE_NONE || scaling == NCSCALE_NONE_HIRES){
      dispcols = ncv->pixx;
      disprows = ncv->pixy;
    }else{
      notcurses_term_dim_yx(nc, &disprows, &dispcols);
      dispcols *= encoding_x_scale(&nc->tcache, bset);
      disprows *= encoding_y_scale(&nc->tcache, bset);
      if(scaling == NCSCALE_SCALE || scaling == NCSCALE_SCALE_HIRES){
        scale_visual(ncv, &disprows, &dispcols);
      } // else stretch
    }
//fprintf(stderr, "PLACING NEW PLANE: %d/%d @ %d/%d %d/%d\n", disprows, dispcols, placey, placex, begy, begx);
    struct ncplane_options nopts = {
      .y = placey,
      .x = placex,
      .rows = disprows / encoding_y_scale(&nc->tcache, bset) +
              !!(disprows % encoding_y_scale(&nc->tcache, bset)),
      .cols = dispcols / encoding_x_scale(&nc->tcache, bset) +
              !!(dispcols % encoding_x_scale(&nc->tcache, bset)),
      .userptr = NULL,
      .name = "cvis",
      .resizecb = NULL,
      .flags = 0,
    };
    if(flags & NCVISUAL_OPTION_HORALIGNED){
      nopts.flags |= NCPLANE_OPTION_HORALIGNED;
    }
    if(flags & NCVISUAL_OPTION_VERALIGNED){
      nopts.flags |= NCPLANE_OPTION_VERALIGNED;
    }
    if((n = ncplane_create(notcurses_stdplane(nc), &nopts)) == NULL){
      return NULL;
    }
    createdn = n;
    placey = 0;
    placex = 0;
  }else{
    if(scaling == NCSCALE_NONE || scaling == NCSCALE_NONE_HIRES){
      dispcols = ncv->pixx;
      disprows = ncv->pixy;
    }else{
      ncplane_dim_yx(n, &disprows, &dispcols);
      dispcols *= encoding_x_scale(&nc->tcache, bset);
      disprows *= encoding_y_scale(&nc->tcache, bset);
      if(!(flags & NCVISUAL_OPTION_HORALIGNED)){
        dispcols -= placex;
      }
      if(!(flags & NCVISUAL_OPTION_VERALIGNED)){
        disprows -= placey;
      }
      if(scaling == NCSCALE_SCALE || scaling == NCSCALE_SCALE_HIRES){
        scale_visual(ncv, &disprows, &dispcols);
      } // else stretch
    }
    if(flags & NCVISUAL_OPTION_HORALIGNED){
      placex = ncplane_halign(n, placex, dispcols / encoding_x_scale(&nc->tcache, bset));
    }
    if(flags & NCVISUAL_OPTION_VERALIGNED){
      placey = ncplane_valign(n, placey, disprows / encoding_y_scale(&nc->tcache, bset));
    }
  }
//fprintf(stderr, "blit: %dx%d:%d+%d of %d/%d stride %u %p\n", begy, begx, leny, lenx, ncv->pixy, ncv->pixx, ncv->rowstride, ncv->data);
  blitterargs bargs;
  if(flags & NCVISUAL_OPTION_ADDALPHA){
    bargs.transcolor = transcolor | 0x1000000ul;
  }else{
    bargs.transcolor = 0;
  }
  bargs.begy = begy;
  bargs.begx = begx;
  bargs.u.cell.placey = placey;
  bargs.u.cell.placex = placex;
  bargs.u.cell.blendcolors = flags & NCVISUAL_OPTION_BLEND;
  if(ncvisual_blit(ncv, disprows, dispcols, n, bset, &bargs)){
    ncplane_destroy(createdn);
    return NULL;
  }
  return n;
}

// create a plane in which to blit the sprixel. |disppixx| and |disppixy| are
// scaled pixel geometry on output, and unused on input. |placey| and |placex|
// are used to position the new plane, and reset to 0 on output. |outy| is
// output height on output, and unused on input.
static ncplane*
make_sprixel_plane(notcurses* nc, ncvisual* ncv, ncscale_e scaling,
                   int* disppixy, int* disppixx, uint64_t flags,
                   int* outy, int* placey, int* placex){
  if(scaling != NCSCALE_NONE && scaling != NCSCALE_NONE_HIRES){
    // FIXME this ought be based off a specified pile
    notcurses_term_dim_yx(nc, disppixy, disppixx);
    if(!(flags & NCVISUAL_OPTION_VERALIGNED)){
      *disppixy -= *placey;
    }
    if(!(flags & NCVISUAL_OPTION_HORALIGNED)){
      *disppixx -= *placex;
    }
    --*disppixy; // most terminals will scroll if we blit to the last row
    *disppixx *= nc->tcache.cellpixx;
    *disppixy *= nc->tcache.cellpixy;
    clamp_to_sixelmax(&nc->tcache, disppixy, disppixx, outy, scaling);
    if(scaling == NCSCALE_SCALE || scaling == NCSCALE_SCALE_HIRES){
      scale_visual(ncv, disppixy, disppixx); // can only shrink
      clamp_to_sixelmax(&nc->tcache, disppixy, disppixx, outy, scaling);
    }
  }else{
    *disppixx = ncv->pixx;
    *disppixy = ncv->pixy;
    clamp_to_sixelmax(&nc->tcache, disppixy, disppixx, outy, scaling);
  }
  struct ncplane_options nopts = {
    .y = *placey,
    .x = *placex,
    .rows = (*outy + nc->tcache.cellpixy - 1) / nc->tcache.cellpixy,
    .cols = (*disppixx + nc->tcache.cellpixx - 1) / nc->tcache.cellpixx,
    .userptr = NULL,
    .name = "bmap",
    .resizecb = NULL,
    .flags = ((flags & NCVISUAL_OPTION_HORALIGNED) ? NCPLANE_OPTION_HORALIGNED : 0)
           | ((flags & NCVISUAL_OPTION_VERALIGNED) ? NCPLANE_OPTION_VERALIGNED : 0),
  };
//fprintf(stderr, "PLACING NEW PLANE: %d/%d @ %d/%d 0x%016lx\n", nopts.rows, nopts.cols, nopts.y, nopts.x, nopts.flags);
  // FIXME might need some particular pile
  ncplane* n = ncplane_create(notcurses_stdplane(nc), &nopts);
  if(n == NULL){
    return NULL;
  }
  // we always actually blit to the origin of the plane
  *placey = 0;
  *placex = 0;
  return n;
}

// when a sprixel is blitted to a plane, that plane becomes a sprixel plane. it
// must not be used with other output mechanisms unless erased. the plane will
// be shrunk to fit the output, and the output is always placed at the origin.
// sprixels cannot be blitted to the standard plane.
//
// the placey/placex arguments thus refer to the position of the *plane*, not
// the sprixel. if creating a new plane, they will be used to place it. if
// using an existing plane, the plane will be moved. they are interpreted
// relative to the parent plane, as they would be in ncplane_create().
//
// by the end, disppixy/disppixx refer to the number of target rows/cols (in
// pixels), aka the scaled geometry. outy refers to the output height, subject
// to Sixel considerations. leny/lenx refer to the number of source rows/cols
// (likewise in pixels). begy/begx refer to the starting offset within the
// source. the sum of begy+leny must not exceed ncv->rows; the sum of begx+lenx
// must not exceed ncv->cols. these sums define the selected geometry. the
// output width is always equal to the scaled width; it has no distinct name.
ncplane* ncvisual_render_pixels(notcurses* nc, ncvisual* ncv, const struct blitset* bset,
                                int placey, int placex, int begy, int begx,
                                ncplane* n, ncscale_e scaling, uint64_t flags,
                                uint32_t transcolor){
  ncplane* stdn = notcurses_stdplane(nc);
  if(n == stdn){
    logerror(nc, "Won't blit bitmaps to the standard plane\n");
    return NULL;
  }
  int disppixy = 0, disppixx = 0, outy = 0;
  ncplane* createdn = NULL;
//fprintf(stderr, "INPUT N: %p rows: %d cols: %d 0x%016lx\n", n ? n : NULL, disppixy, disppixx, flags);
  if(n == NULL){ // create plane
    if((createdn = make_sprixel_plane(nc, ncv, scaling, &disppixy, &disppixx,
                                      flags, &outy, &placey, &placex)) == NULL){
      return NULL;
    }
    n = createdn;
  }else{
    if(scaling != NCSCALE_NONE && scaling != NCSCALE_NONE_HIRES){
      ncplane_dim_yx(n, &disppixy, &disppixx);
      disppixx *= nc->tcache.cellpixx;
      disppixy *= nc->tcache.cellpixy;
      clamp_to_sixelmax(&nc->tcache, &disppixy, &disppixx, &outy, scaling);
      int absplacex = 0, absplacey = 0;
      if(!(flags & NCVISUAL_OPTION_HORALIGNED)){
        absplacex = placex;
      }
      if(!(flags & NCVISUAL_OPTION_VERALIGNED)){
        absplacey = placey;
      }
      disppixx -= absplacex * nc->tcache.cellpixx;
      disppixy -= absplacey * nc->tcache.cellpixy;
    }else{
      disppixx = ncv->pixx;
      disppixy = ncv->pixy;
    }
    if(scaling == NCSCALE_SCALE || scaling == NCSCALE_SCALE_HIRES){
      clamp_to_sixelmax(&nc->tcache, &disppixy, &disppixx, &outy, scaling);
      scale_visual(ncv, &disppixy, &disppixx);
    }
    clamp_to_sixelmax(&nc->tcache, &disppixy, &disppixx, &outy, scaling);
    // FIXME use a closed form
    while((outy + nc->tcache.cellpixy - 1) / nc->tcache.cellpixy > ncplane_dim_y(n)){
      outy -= nc->tcache.sprixel_scale_height;
      disppixy = outy;
    }
  }
//fprintf(stderr, "pblit: %dx%d <- %dx%d of %d/%d stride %u @%dx%d %p %u\n", disppixy, disppixx, begy, begx, ncv->pixy, ncv->pixx, ncv->rowstride, placey, placex, ncv->data, nc->tcache.cellpixx);
  blitterargs bargs;
  if(flags & NCVISUAL_OPTION_ADDALPHA){
    bargs.transcolor = transcolor | 0x1000000ul;
  }else{
    bargs.transcolor = 0;
  }
  bargs.begy = begy;
  bargs.begx = begx;
  bargs.u.pixel.celldimx = nc->tcache.cellpixx;
  bargs.u.pixel.celldimy = nc->tcache.cellpixy;
  bargs.u.pixel.colorregs = nc->tcache.color_registers;
  if(n->sprite == NULL){
    int cols = disppixx / bargs.u.pixel.celldimx + !!(disppixx % bargs.u.pixel.celldimx);
    int rows = outy / bargs.u.pixel.celldimy + !!(outy % bargs.u.pixel.celldimy);
    if((n->sprite = sprixel_alloc(n, rows, cols)) == NULL){
      ncplane_destroy(createdn);
      return NULL;
    }
  }else{
    n->sprite = sprixel_recycle(n);
  }
  bargs.u.pixel.spx = n->sprite;
  // FIXME only set this if cursor is indeed hidden
  if(nc->tcache.sprixel_cursor_hack){
    bargs.u.pixel.cursor_hack = nc->tcache.civis;
  }else{
    bargs.u.pixel.cursor_hack = NULL;
  }
  // FIXME need to pull off the ncpile's sprixellist if anything below fails!
  // at this point, disppixy/disppixx are the output geometry (might be larger
  // than scaledy/scaledx for sixel), while scaledy/scaledx are the scaled
  // geometry. cells occupied are determined based off disppixy/disppixx.
  if(ncvisual_blit(ncv, disppixy, disppixx, n, bset, &bargs)){
    ncplane_destroy(createdn);
    return NULL;
  }
  // if we created the plane earlier, placex/placey were taken into account, and
  // zeroed out, thus neither of these will have any effect.
  if(flags & NCVISUAL_OPTION_HORALIGNED){
    if(placex == NCALIGN_CENTER){
      placex = (ncplane_dim_x(ncplane_parent_const(n)) * nc->tcache.cellpixx - disppixx) / 2 / nc->tcache.cellpixx;
    }else if(placex == NCALIGN_RIGHT){
      placex = (ncplane_dim_x(ncplane_parent_const(n)) * nc->tcache.cellpixy - disppixx) / nc->tcache.cellpixx;
    }
    if(placex < 0){
      return NULL;
    }
  }
  if(flags & NCVISUAL_OPTION_VERALIGNED){
    if(placey == NCALIGN_CENTER){
      placey = (ncplane_dim_y(ncplane_parent_const(n)) * nc->tcache.cellpixy - disppixy) / 2 / nc->tcache.cellpixy;
    }else if(placey == NCALIGN_BOTTOM){
      placey = (ncplane_dim_y(ncplane_parent_const(n)) * nc->tcache.cellpixy - disppixy) / nc->tcache.cellpixy;
    }
    if(placey < 0){
      return NULL;
    }
  }
  // ncplane_resize() hides any attached sprixel, so lift it (the sprixel) out
  // for a moment as we shrink the plane to fit. we keep the origin and move to
  // the intended location.
  sprixel* s = n->sprite;
  n->sprite = NULL;
  // FIXME might need shrink down the TAM and kill unnecessary auxvecs
  if(ncplane_resize(n, 0, 0, s->dimy, s->dimx, placey - ncplane_y(n),
                    placex - ncplane_x(n), s->dimy, s->dimx)){
    sprixel_hide(bargs.u.pixel.spx);
    ncplane_destroy(createdn);
    return NULL;
  }
  n->sprite = bargs.u.pixel.spx;
//fprintf(stderr, "RESIZED: %d/%d at %d/%d %p\n", ncplane_dim_y(n), ncplane_dim_x(n), ncplane_y(n), ncplane_x(n), n->sprite);
  return n;
}

ncplane* ncvisual_render(notcurses* nc, ncvisual* ncv, const struct ncvisual_options* vopts){
  const struct blitset* bset;
  int leny, lenx;
  if(ncvisual_blitset_geom(nc, ncv, vopts, NULL, NULL, NULL, NULL,
                           &leny, &lenx, &bset) < 0){
    // ncvisual_blitset_geom() emits its own diagnostics, no need for an error here
    return NULL;
  }
  int begy, begx;
  ncvisual_origin(vopts, &begy, &begx);
  int placey = vopts ? vopts->y : 0;
  int placex = vopts ? vopts->x : 0;
//fprintf(stderr, "beg/len: %d %d %d %d place: %d/%d scale: %d/%d\n", begy, leny, begx, lenx, placey, placex, encoding_y_scale(&nc->tcache, bset), encoding_x_scale(&nc->tcache, bset));
  ncplane* n = (vopts ? vopts->n : NULL);
//fprintf(stderr, "%p tacache: %p\n", n, n->tacache);
  ncscale_e scaling = vopts ? vopts->scaling : NCSCALE_NONE;
  uint32_t transcolor = 0;
  if(vopts && vopts->flags & NCVISUAL_OPTION_ADDALPHA){
    transcolor = 0x1000000ull | vopts->transcolor;
  }
  if(bset->geom != NCBLIT_PIXEL){
    n = ncvisual_render_cells(nc, ncv, bset, placey, placex, begy, begx,
                              n, scaling, vopts ? vopts->flags : 0, transcolor);
  }else{
    n = ncvisual_render_pixels(nc, ncv, bset, placey, placex, begy, begx, n, scaling,
                               vopts ? vopts->flags : 0, transcolor);
  }
  return n;
}

ncvisual* ncvisual_from_plane(const ncplane* n, ncblitter_e blit, int begy, int begx,
                              int leny, int lenx){
  uint32_t* rgba = ncplane_as_rgba(n, blit, begy, begx, leny, lenx, NULL, NULL);
//fprintf(stderr, "snarg: %d/%d @ %d/%d (%p)\n", leny, lenx, begy, begx, rgba);
//fprintf(stderr, "RGBA %p\n", rgba);
  if(rgba == NULL){
    return NULL;
  }
  int dimy, dimx;
  ncplane_dim_yx(n, &dimy, &dimx);
  if(lenx == -1){
    lenx = n->lenx - begx;
  }
  if(leny == -1){
    leny = (n->leny - begy);
  }
  ncvisual* ncv = ncvisual_from_rgba(rgba, leny * 2, lenx * 4, lenx);
  free(rgba);
//fprintf(stderr, "RETURNING %p\n", ncv);
  return ncv;
}

void ncvisual_destroy(ncvisual* ncv){
  if(visual_implementation){
    visual_implementation->visual_destroy(ncv);
  }
}

int ncvisual_simple_streamer(ncvisual* ncv, struct ncvisual_options* vopts,
                             const struct timespec* tspec, void* curry){
  if(notcurses_render(ncplane_notcurses(vopts->n))){
    return -1;
  }
  int ret = 0;
  if(curry){
    ncplane* subncp = curry;
    char* subtitle = ncvisual_subtitle(ncv);
    if(subtitle){
      if(ncplane_putstr_yx(subncp, 0, 0, subtitle) < 0){
        ret = -1;
      }
      free(subtitle);
    }
  }
  clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, tspec, NULL);
  return ret;
}

int ncvisual_set_yx(const struct ncvisual* n, int y, int x, uint32_t pixel){
  if(y >= n->pixy || y < 0){
    return -1;
  }
  if(x >= n->pixx || x < 0){
    return -1;
  }
  n->data[y * (n->rowstride / 4) + x] = pixel;
  return 0;
}

int ncvisual_at_yx(const ncvisual* n, int y, int x, uint32_t* pixel){
  if(y >= n->pixy || y < 0){
    return -1;
  }
  if(x >= n->pixx || x < 0){
    return -1;
  }
  *pixel = n->data[y * (n->rowstride / 4) + x];
  return 0;
}

int ncvisual_polyfill_recurse(ncvisual* n, int y, int x, uint32_t rgba, uint32_t match){
  if(y < 0 || y >= n->pixy){
    return 0;
  }
  if(x < 0 || x >= n->pixx){
    return 0;
  }
  uint32_t* pixel = &n->data[y * (n->rowstride / 4) + x];
  if(*pixel != match || *pixel == rgba){
    return 0;
  }
// fprintf(stderr, "%d/%d: setting %08x to %08x\n", y, x, *pixel, rgba);
  *pixel = rgba;
  int ret = 1;
  ret += ncvisual_polyfill_recurse(n, y - 1, x, rgba, match);
  ret += ncvisual_polyfill_recurse(n, y + 1, x, rgba, match);
  ret += ncvisual_polyfill_recurse(n, y, x - 1, rgba, match);
  ret += ncvisual_polyfill_recurse(n, y, x + 1, rgba, match);
  return ret;
}

int ncvisual_polyfill_yx(ncvisual* n, int y, int x, uint32_t rgba){
  if(y >= n->pixy || y < 0){
    return -1;
  }
  if(x >= n->pixx || x < 0){
    return -1;
  }
  uint32_t* pixel = &n->data[y * (n->rowstride / 4) + x];
  return ncvisual_polyfill_recurse(n, y, x, rgba, *pixel);
}

bool notcurses_canopen_images(const notcurses* nc __attribute__ ((unused))){
  if(!visual_implementation){
    return false;
  }
  return visual_implementation->canopen_images;
}

bool notcurses_canopen_videos(const notcurses* nc __attribute__ ((unused))){
  if(!visual_implementation){
    return false;
  }
  return visual_implementation->canopen_videos;
}

int ncvisual_decode_loop(ncvisual* nc){
  if(!visual_implementation){
    return -1;
  }
  return visual_implementation->visual_decode_loop(nc);
}

int ncvisual_stream(notcurses* nc, ncvisual* ncv, float timescale,
                    ncstreamcb streamer, const struct ncvisual_options* vopts,
                    void* curry){
  if(!visual_implementation){
    return -1;
  }
  return visual_implementation->visual_stream(nc, ncv, timescale, streamer, vopts, curry);
}

char* ncvisual_subtitle(const ncvisual* ncv){
  if(!visual_implementation){
    return NULL;
  }
  return visual_implementation->visual_subtitle(ncv);
}

int ncvisual_resize(ncvisual* nc, int rows, int cols){
  if(!visual_implementation){
    return -1;
  }
  if(visual_implementation->visual_resize(nc, rows, cols)){
    return -1;
  }
  return 0;
}

// Inflate each pixel of 'bmap' to 'scale'x'scale' pixels square, using the
// same color as the original pixel.
static inline void*
inflate_bitmap(const uint32_t* bmap, int scale, int rows, int stride, int cols){
  size_t size = rows * cols * scale * scale * sizeof(*bmap);
  uint32_t* ret = malloc(size);
  if(ret){
    for(int y = 0 ; y < rows ; ++y){
      const uint32_t* src = bmap + y * (stride / sizeof(*bmap));
      for(int yi = 0 ; yi < scale ; ++yi){
        uint32_t* dst = ret + (y * scale + yi) * cols * scale;
        for(int x = 0 ; x < cols ; ++x){
          for(int xi = 0 ; xi < scale ; ++xi){
            dst[x * scale + xi] = src[x];
          }
        }
      }
    }
  }
  return ret;
}

int ncvisual_inflate(ncvisual* n, int scale){
  if(scale <= 0){
    return -1;
  }
  void* inflaton = inflate_bitmap(n->data, scale, n->pixy, n->rowstride, n->pixx);
  if(inflaton == NULL){
    return -1;
  }
  ncvisual_set_data(n, inflaton, true);
  n->pixy *= scale;
  n->pixx *= scale;
  n->rowstride = 4 * n->pixx;
  ncvisual_details_seed(n);
  return 0;
}
