/* xscreensaver, Copyright (c) 2007
 *  John Lindal <xs@jafl.my.speedingbits.com>
 *
 * Some code taken from
 *  xscreensaver, Copyright (c) 1992, 1995, 1997 
 *  Jamie Zawinski <jwz@jwz.com>
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation.  No representations are made about the suitability of this
 * software for any purpose.  It is provided "as is" without express or 
 * implied warranty.
 */

#include "screenhack.h"
#include <limits.h>

#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
# include "xdbe.h"
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */

#ifndef NULL
#define NULL ((void *) 0x0)
#endif /* NULL */

/* mode */
#define MODE_RANDOM    0
#define MODE_BALL_DROP 1
#define MODE_LIGHTNING 2
#define MODE_CONVOLVE  3

#define MODE_LIGHTNING_ERASE 1000

/* parameters */
#define LIGHTNING_SEGMENT 2
#define BALL_COLOR_COUNT  4
#define BALL_SIZE         10  /* must be even */
#define PIN_SIZE          4   /* must be even */
#define BIN_MARGIN        1

/* ball behavior */
#define ACTION_DROP 0
#define ACTION_ROLL 1
#define ACTION_BIN  2

/* ball colors */
#define COLOR_RED    0
#define COLOR_GREEN  1
#define COLOR_BLUE   2
#define COLOR_YELLOW 3

static XColor ball_color[ BALL_COLOR_COUNT ] =
{
  { 0, 0xFFFF,      0,      0, DoRed|DoGreen|DoBlue, 0 }, /* red */
  { 0,      0, 0x9999,      0, DoRed|DoGreen|DoBlue, 0 }, /* medium green */
  { 0,      0, 0x9999, 0xFFFF, DoRed|DoGreen|DoBlue, 0 }, /* light blue */
  { 0, 0xFFFF, 0xFFFF,      0, DoRed|DoGreen|DoBlue, 0 }, /* yellow */
};

struct ball_state
{
  int action;  /* current behavior */
  int dx;      /* horizontal velocity */
  int dy;      /* vertical velocity */
  int ylim;    /* position where behavior will change */
  int bin;     /* bin into which ball is dropping */
};

struct state
{
  Display *display;
  Window window;
  Drawable drawable;
  GC gc, erase_gc, ball_gc[ BALL_COLOR_COUNT ];

  int mode;
  int delay;
  int dbuf;
  unsigned long fg, bg;
  int xlim, ylim, depth;
  Colormap cmap;
  unsigned long color[ BALL_COLOR_COUNT ];

  XSegment* seg; /* used by several different animations */

#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
  XdbeBackBuffer back_buf;
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
  Pixmap pix_buf;

  /* ball drop */

  unsigned long b_live_max;                       /* maximum number of active balls */
  int b_live_wait;                                /* number of frames to wait before releasing another ball */
  unsigned long b_live_sum;                       /* total number of active balls */
  unsigned long b_live_count[ BALL_COLOR_COUNT ]; /* number of active balls of each color */
  XArc* b_live[ BALL_COLOR_COUNT ];               /* drawing info for each active ball, grouped by color */
  struct ball_state* b_state[ BALL_COLOR_COUNT ]; /* state information for each ball */
  unsigned long b_done_count[ BALL_COLOR_COUNT ]; /* number of inactive balls of each color */
  XArc* b_done[ BALL_COLOR_COUNT ];               /* drawing info for each inactive ball, grouped by color */
  unsigned long b_pin_row_count;                  /* number of pin rows */
  unsigned long b_pin_count;                      /* total number of pins */
  XArc* b_pin;                                    /* drawing information for all pins */
  int b_top_pin_y;                                /* top of top pin */
  unsigned long b_bin_count;                      /* number of bins */
  unsigned long* b_bin_content;                   /* number of balls in each bin */
  int b_drain;                                    /* true if draining bins */
  Pixmap b_bkgd_pix_buf;                          /* background for falling balls */

  /* lightning */

  unsigned long l_strike_count; /* number of lightning strikes */
  int l_pt_count;               /* number of lightning segments */
  XPoint* l_pt;                 /* lightning segments */

  /* convolve */

  double unit_density[3];           /* unit density: 0.5,0,0.5 */
  unsigned long c_count1, c_count2; /* number of points in density */
  double *c_f1, *c_f2;              /* input and output densities */
};


static void
gaussian_init1 (struct state* st)
{
  int x, y, i, j, k, x0, dx, dy;
  XGCValues gcv;

  st->seg = (XSegment*) calloc(st->xlim, sizeof(XSegment));

  if (st->dbuf
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
      && !st->back_buf
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
     )
  {
    st->pix_buf = XCreatePixmap(st->display, st->window, st->xlim, st->ylim, st->depth);
  }
  st->drawable =
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
    st->back_buf ? st->back_buf :
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
    st->pix_buf  ? st->pix_buf  :
    st->window;

  gcv.foreground = st->fg;
  gcv.background = st->bg;
  st->gc = XCreateGC(st->display, st->drawable, GCForeground|GCBackground, &gcv);

  gcv.foreground = st->bg;
  st->erase_gc = XCreateGC(st->display, st->drawable, GCForeground, &gcv);

  for (i=0; i<BALL_COLOR_COUNT; i++)
  {
    gcv.foreground = st->color[i];
    st->ball_gc[i] = XCreateGC(st->display, st->drawable, GCForeground|GCBackground, &gcv);
  }

  if (st->mode == MODE_BALL_DROP)
  {
    st->b_bkgd_pix_buf = None;

    dx = BALL_SIZE + PIN_SIZE + 2*BIN_MARGIN;
    dy = BALL_SIZE + PIN_SIZE + 2*BIN_MARGIN;

    st->b_live_max = (st->ylim/2) / (BALL_SIZE + 2*BIN_MARGIN);
    st->b_live_wait = 0;
    st->b_drain = 0;
    st->b_bin_count = (st->xlim/dx) + 1;
    st->b_pin_row_count = (st->ylim/2) / dy;
    if (st->b_pin_row_count % 2 == 0)
    {
      st->b_pin_row_count--;
    }
    st->b_pin_count = (2 * st->b_bin_count + 1) * st->b_pin_row_count/2;
    x0 = (st->xlim/2) % dx;

    st->b_live_sum = 0;
    for (i=0; i<BALL_COLOR_COUNT; i++)
    {
      st->b_live_count[i] = 0;
      st->b_done_count[i] = 0;
      st->b_live[i] = (XArc*) calloc(st->b_live_max, sizeof(XArc));
      st->b_state[i] = (struct ball_state*) calloc(st->b_live_max, sizeof(struct ball_state));

      for (j=0; j<st->b_live_max; j++)
      {
        st->b_live[i][j].width = BALL_SIZE;
        st->b_live[i][j].height = BALL_SIZE;
        st->b_live[i][j].angle1 = 0;
        st->b_live[i][j].angle2 = 360 * 64;
      }

      st->b_done[i] = (XArc*) calloc((st->ylim/BALL_SIZE) * (st->xlim/BALL_SIZE) / 2, sizeof(XArc));
    }
    st->b_pin = (XArc*) calloc(st->b_pin_count, sizeof(XArc));

    i = j = k = 0;
    for (y = st->ylim/2;
         y > BALL_SIZE/2 && j < st->b_pin_count && k < st->b_pin_row_count;
         y -= dy, k++)
    {
      st->b_top_pin_y = y;
      for (x = x0 - (i ? dx/2 : 0);
           x < st->xlim + dx/2 && j < st->b_pin_count;
           x += dx)
      {
        st->b_pin[j].x = x - PIN_SIZE/2;
        st->b_pin[j].y = y;
        st->b_pin[j].width = PIN_SIZE;
        st->b_pin[j].height = PIN_SIZE;
        st->b_pin[j].angle1 = 0;
        st->b_pin[j].angle2 = 360 * 64;
        j++;
      }
      i = (i+1) % 2;
    }

    for (x = x0, i = 0; i <= st->b_bin_count; x += dx, i++)
    {
      st->seg[i].x1 = x;
      st->seg[i].y1 = st->ylim;
      st->seg[i].x2 = x;
      st->seg[i].y2 = st->ylim/2;
    }

    st->b_bin_content = (unsigned long*) calloc(st->b_bin_count+1, sizeof(unsigned long));
  }
  else if (st->mode == MODE_LIGHTNING || st->mode == MODE_LIGHTNING_ERASE)
  {
    st->mode = MODE_LIGHTNING;

    st->l_strike_count = 0;
    st->l_pt_count = st->ylim / LIGHTNING_SEGMENT;
    st->l_pt = (XPoint*) calloc(st->l_pt_count+1, sizeof(XPoint));
  }
  else if (st->mode == MODE_CONVOLVE)
  {
    st->unit_density[0] = 0.5;
    st->unit_density[1] = 0.0;
    st->unit_density[2] = 0.5;
    st->c_count1 = 0;
    st->c_f1 = NULL;
    st->c_count2 = 0;
    st->c_f2 = NULL;
  }
}

static void
gaussian_free1 (struct state* st)
{
  int i;

  if (!st->seg)
  {
    return;
  }

  free(st->seg);
  st->seg = NULL;

  XFreeGC(st->display, st->gc);
  XFreeGC(st->display, st->erase_gc);

  for (i=0; i<BALL_COLOR_COUNT; i++)
  {
    XFreeGC(st->display, st->ball_gc[i]);
  }

  if (st->pix_buf)
  {
    XFreePixmap(st->display, st->pix_buf);
  }

  if (st->mode == MODE_BALL_DROP)
  {
    for (i=0; i<BALL_COLOR_COUNT; i++)
    {
      free(st->b_live[i]);
      free(st->b_state[i]);
      free(st->b_done[i]);
    }

    free(st->b_pin);
    free(st->b_bin_content);

    if (st->b_bkgd_pix_buf)
    {
      XFreePixmap(st->display, st->b_bkgd_pix_buf);
    }
  }
  else if (st->mode == MODE_LIGHTNING || st->mode == MODE_LIGHTNING_ERASE)
  {
    free(st->l_pt);
  }
  else if (st->mode == MODE_CONVOLVE)
  {
    free(st->c_f1);
    free(st->c_f2);
  }
}

static void *
gaussian_init (Display *display, Window window)
{
  struct state *st = (struct state *) calloc (1, sizeof(*st));
  XWindowAttributes xgwa;
  int i;
  st->dbuf = get_boolean_resource(display, "doubleBuffer", "Boolean");

  st->display = display;
  st->window = window;
  XGetWindowAttributes(st->display, st->window, &xgwa);

#ifdef HAVE_COCOA	/* Don't second-guess Quartz's double-buffering */
  st->dbuf = False;
#endif

  if (st->dbuf)
  {
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
    st->back_buf = xdbe_get_backbuffer(st->display, st->window, XdbeUndefined);
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
  }

  st->xlim = xgwa.width;
  st->ylim = xgwa.height;
  st->cmap = xgwa.colormap;
  st->depth = xgwa.depth;
  st->fg = get_pixel_resource(st->display, st->cmap, "foreground","Foreground");
  st->bg = get_pixel_resource(st->display, st->cmap, "background","Background");

  st->mode  = get_integer_resource(st->display, "mode", "Integer");
  if (st->mode == 0) st->mode = 1 + (random() % 3);
  st->delay = get_integer_resource(st->display, "delay", "Integer");
  if (st->delay < 0) st->delay = 0;

  for (i=0; i<BALL_COLOR_COUNT; i++)
  {
    if (!mono_p && XAllocColor(st->display, st->cmap, ball_color+i))
    {
      st->color[i] = ball_color[i].pixel;
    }
    else
    {
      st->color[i] = st->fg;
    }
  }

  gaussian_init1(st);
  return st;
}

static void
gaussian_free (Display *display, Window window, void *closure)
{
  struct state *st = (struct state *) closure;

#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
  if (st->back_buf)
  {
    XdbeDeallocateBackBufferName(st->display, st->back_buf);
  }
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */

  if (!mono_p)
  {
    XFreeColors(st->display, st->cmap, st->color, BALL_COLOR_COUNT, 0);
  }

  gaussian_free1(st);
  free(st);
}

static void
gaussian_reshape (Display *display, Window window, void *closure, 
                 unsigned int w, unsigned int h)
{
  struct state *st = (struct state *) closure;
  st->xlim = w;
  st->ylim = h;

  gaussian_free1(st);
  gaussian_init1(st);
}

static void
gaussian_init_bkgd (struct state* st, Drawable d)
{
  XFillRectangle(st->display, d, st->erase_gc, 0, 0, st->xlim, st->ylim);
  XFillArcs(st->display, d, st->gc, st->b_pin, st->b_pin_count);
  XDrawSegments(st->display, d, st->gc, st->seg, st->b_bin_count+1);
}

static unsigned long
gaussian_draw (Display *display, Window window, void *closure)
{
  struct state *st = (struct state *) closure;
  int delay = st->delay;

  if (st->mode == MODE_BALL_DROP)
  {
    int i, j, k;

    if (!st->b_bkgd_pix_buf)
    {
      Drawable d = st->b_bkgd_pix_buf = XCreatePixmap(st->display, st->window, st->xlim, st->ylim, st->depth);
      if (!d)
      {
        d = st->drawable;
      }
      gaussian_init_bkgd(st, d);
    }
    else
    {
      XCopyArea(st->display, st->b_bkgd_pix_buf, st->drawable, st->gc,
                0, 0, st->xlim, st->ylim, 0, 0);
    }

    if (!st->b_drain && st->b_live_wait <= 0 && st->b_live_sum < st->b_live_max)
    {
      do
      {
        i = random() % BALL_COLOR_COUNT;
      }
        while (st->b_live_count[i] >= st->b_live_max);

      j = st->b_live_count[i];
      st->b_live_count[i]++;
      st->b_live_sum++;
      st->b_live[i][j].x = st->xlim/2 - BALL_SIZE/2;
      st->b_live[i][j].y = -BALL_SIZE-1;
      st->b_state[i][j].action = ACTION_DROP;
      st->b_state[i][j].dx = 0;
      st->b_state[i][j].dy = +1;
      st->b_state[i][j].ylim = st->b_top_pin_y - BALL_SIZE - 2;
      st->b_state[i][j].bin = -1;

      st->b_live_wait = (BALL_SIZE + 2*BIN_MARGIN) + (st->delay / 250000.0) * (st->ylim/2 - BALL_SIZE - 2*BIN_MARGIN) + random() % 10;
    }
    st->b_live_wait--;

    for (i=0; i<BALL_COLOR_COUNT; i++)
    {
      for (j=0; j<st->b_live_count[i]; j++)
      {
        st->b_live[i][j].x += st->b_state[i][j].dx;
        st->b_live[i][j].y += st->b_state[i][j].dy;

        if (st->b_state[i][j].action == ACTION_DROP &&
            st->b_live[i][j].y >= st->ylim/2)
        {
          st->b_state[i][j].action = ACTION_BIN;
          st->b_state[i][j].dx     = 0;

          st->b_state[i][j].bin = 0;
          for (k=1; k<=st->b_bin_count; k++)
          {
            if (st->b_live[i][j].x < st->seg[k].x1)
            {
              st->b_state[i][j].bin = k-1;
              break;
            }
          }
        }
        else if (st->b_state[i][j].action == ACTION_DROP &&
                 st->b_live[i][j].y >= st->b_state[i][j].ylim)
        {
          st->b_state[i][j].action = ACTION_ROLL;
          st->b_state[i][j].dx     = (random() < ULONG_MAX/2) ? -1 : +1;
          st->b_state[i][j].ylim  += BALL_SIZE/2 + BIN_MARGIN + PIN_SIZE/2;
        }
        else if (st->b_state[i][j].action == ACTION_ROLL &&
                 st->b_live[i][j].y >= st->b_state[i][j].ylim)
        {
          st->b_state[i][j].action = ACTION_DROP;
          st->b_state[i][j].dx     = 0;
          st->b_state[i][j].ylim  += BALL_SIZE/2 + BIN_MARGIN + PIN_SIZE/2;
        }
        else if (st->b_state[i][j].action == ACTION_BIN)
        {
          int y = st->ylim - (BALL_SIZE + 2*BIN_MARGIN) * (1+st->b_bin_content[ st->b_state[i][j].bin ]);
          st->b_state[i][j].dy++;
          if (st->b_live[i][j].y >= y)
          {
            k = st->b_done_count[i];
            st->b_done[i][k] = st->b_live[i][j];
            st->b_done[i][k].y = y;
            st->b_done_count[i]++;
            st->b_bin_content[ st->b_state[i][j].bin ]++;
            if (st->b_live_count[i] > j+1)
            {
              memmove(st->b_live[i]+j, st->b_live[i]+j+1,
                      (st->b_live_count[i]-j-1) * sizeof(XArc));
              memmove(st->b_state[i]+j, st->b_state[i]+j+1,
                      (st->b_live_count[i]-j-1) * sizeof(struct ball_state));
            }
            st->b_live_count[i]--;
            st->b_live_sum--;
            j--;

            if (st->b_bkgd_pix_buf)
            {
              XDrawArcs(st->display, st->b_bkgd_pix_buf, st->ball_gc[i], st->b_done[i], st->b_done_count[i]);
              XFillArcs(st->display, st->b_bkgd_pix_buf, st->ball_gc[i], st->b_done[i], st->b_done_count[i]);
            }

            if (y <= st->ylim/2)
            {
              st->b_drain = 1;
            }
          }
        }
      }

      XDrawArcs(st->display, st->drawable, st->ball_gc[i], st->b_live[i], st->b_live_count[i]);
      XFillArcs(st->display, st->drawable, st->ball_gc[i], st->b_live[i], st->b_live_count[i]);

      if (!st->b_bkgd_pix_buf)
      {
        XDrawArcs(st->display, st->drawable, st->ball_gc[i], st->b_done[i], st->b_done_count[i]);
        XFillArcs(st->display, st->drawable, st->ball_gc[i], st->b_done[i], st->b_done_count[i]);
      }
    }

    if (st->b_drain)
    {
      if (st->b_bkgd_pix_buf)
      {
        gaussian_init_bkgd(st, st->b_bkgd_pix_buf);

        for (i=0; i<BALL_COLOR_COUNT; i++)
        {
          for (j=0; j<st->b_done_count[i]; j++)
          {
            st->b_done[i][j].y += BALL_SIZE + 2*BIN_MARGIN;
          }

          XDrawArcs(st->display, st->b_bkgd_pix_buf, st->ball_gc[i], st->b_done[i], st->b_done_count[i]);
          XFillArcs(st->display, st->b_bkgd_pix_buf, st->ball_gc[i], st->b_done[i], st->b_done_count[i]);
        }
      }

      k = 0;
      for (i=0; i<=st->b_bin_count; i++)
      {
        if (st->b_bin_content[i] > 0)
        {
          st->b_bin_content[i]--;
          k += st->b_bin_content[i];
        }
      }

      if (k == 0)
      {
        st->b_drain = 0;

        for (i=0; i<BALL_COLOR_COUNT; i++)
        {
          st->b_done_count[i] = 0;
        }
      }
    }

    delay = 5000;
  }
  else if (st->mode == MODE_LIGHTNING)
  {
    int i, x;

    if (st->l_strike_count == 0 || st->l_strike_count > st->xlim * st->ylim / 4)
    {
      for (i=0; i<st->xlim; i++)
      {
        st->seg[i].x1 = i;
        st->seg[i].y1 = st->ylim;
        st->seg[i].x2 = i;
        st->seg[i].y2 = st->ylim;
      }
    }

    st->l_pt[0].x = st->xlim/2;
    st->l_pt[0].y = 0;

    x = st->l_pt[0].x;
    for (i=1; i<=st->l_pt_count; i++)
    {
      st->l_pt[i].x = (random() < ULONG_MAX/2) ? -LIGHTNING_SEGMENT : LIGHTNING_SEGMENT;
      st->l_pt[i].y = LIGHTNING_SEGMENT;

      x += st->l_pt[i].x;
    }

    st->l_strike_count++;
    st->seg[x].y2--;

    XDrawLines(st->display, st->drawable, st->ball_gc[ COLOR_YELLOW ], st->l_pt, st->l_pt_count+1, CoordModePrevious);

    st->mode = MODE_LIGHTNING_ERASE;

    delay += 20000;
  }
  else if (st->mode == MODE_LIGHTNING_ERASE)
  {
    XFillRectangle(st->display, st->drawable, st->erase_gc, 0, 0, st->xlim, st->ylim);
    XDrawSegments(st->display, st->drawable, st->gc, st->seg, st->xlim);

    st->mode = MODE_LIGHTNING;
  }
  else if (st->mode == MODE_CONVOLVE)
  {
    int i, j, mid;
    int x0 = st->xlim/2;

    if (st->c_f1 == NULL || st->c_count1 >= st->xlim)
    {
      free(st->c_f1);

      st->c_count1 = 7;
      st->c_f1 = (double*) calloc(st->c_count1, sizeof(double));
      st->c_f1[0] = 0.0;
      st->c_f1[1] = 0.0;
      st->c_f1[2] = 0.5;
      st->c_f1[3] = 0.0;
      st->c_f1[4] = 0.5;
      st->c_f1[5] = 0.0;
      st->c_f1[6] = 0.0;
    }

    if (st->c_f2 == NULL)
    {
      st->c_count2 = st->c_count1+2;
      st->c_f2 = (double*) calloc(st->c_count2, sizeof(double));
      st->c_f2[0] = 0.0;
      st->c_f2[ st->c_count2-1 ] = 0.0;
    }

    mid = st->c_count2/2;
    j   = 0;
    for (i=1; i<st->c_count2-1; i++)
    {
      int x = x0 + (i - mid);

      st->c_f2[i] =
      	st->c_f1[i-2]*st->unit_density[2] +
      	st->c_f1[i-1]*st->unit_density[1] +
      	st->c_f1[i]*st->unit_density[0];

      if (st->c_f2[i] > 1e-5)
      {
        st->seg[j].x1 = x;
        st->seg[j].y1 = st->ylim;
        st->seg[j].x2 = x;
        st->seg[j].y2 = st->ylim * (1.0 - st->c_f2[i] / 0.5);
        j++;
      }
    }

    XFillRectangle(st->display, st->drawable, st->erase_gc, 0, 0, st->xlim, st->ylim);
    XDrawSegments(st->display, st->drawable, st->gc, st->seg, j);

    free(st->c_f1);
    st->c_count1 = st->c_count2;
    st->c_f1 = st->c_f2;
    st->c_count2 = 0;
    st->c_f2 = NULL;
  }

#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
  if (st->back_buf)
  {
    XdbeSwapInfo info[1];
    info[0].swap_window = st->window;
    info[0].swap_action = XdbeUndefined;
    XdbeSwapBuffers(st->display, info, 1);
  }
  else
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
  if (st->pix_buf)
  {
    XCopyArea(st->display, st->pix_buf, st->window, st->gc,
              0, 0, st->xlim, st->ylim, 0, 0);
  }

  return delay;
}

static Bool
gaussian_event (Display *display, Window window, void *closure, XEvent *event)
{
  return False;
}

static const char *gaussian_defaults [] = {
  ".background:		black",
  ".foreground:		gray80",
  "*delay:		10000",
  "*mode:		0",

  "*doubleBuffer:	True",
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
  "*useDBE:		True", /* use double buffering extension */
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */

  0
};

static XrmOptionDescRec gaussian_options [] = {
  { "-delay",		".delay",		XrmoptionSepArg, 0 },
  { "-mode",		".mode",		XrmoptionSepArg, 0 },
  { "-db",		".doubleBuffer",	XrmoptionNoArg,  "True" },
  { 0, 0, 0, 0 }
};

XSCREENSAVER_MODULE ("Gaussian", gaussian)

