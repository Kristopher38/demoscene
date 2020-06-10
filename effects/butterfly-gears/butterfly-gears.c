#include "startup.h"

#include "hardware.h"
#include "interrupts.h"
#include "blitter.h"
#include "color.h"
#include "copper.h"
#include "bitmap.h"
#include "palette.h"
#include "pixmap.h"
#include "memory.h"
#include "sprite.h"
#include "fx.h"
#include "mouse.h"
#include "event.h"
#include "random.h"

#define DEPTH 5
#define BALLS 3
#define WIDTH 320
#define BALL_PADDING_TOP 6
#define BALL_PADDING_BOTTOM 11
#define SMALL_BALL_Y_INC 1
#define SMALL_BALL_WIDTH 58
#define SMALL_BALL_HEIGHT 64
#define SMALL_BALL_CENTER ((WIDTH-SMALL_BALL_WIDTH)/2+16)
#define LARGE_BALL_Y_INC 2
#define LARGE_BALL_WIDTH 110
#define LARGE_BALL_HEIGHT 112
#define LARGE_BALL_CENTER ((WIDTH-LARGE_BALL_WIDTH)/2+16)
#define STATIC_Y_AREA_PADDING (LARGE_BALL_HEIGHT + 1)
#define ROTZOOM_W 24
#define ROTZOOM_H 24
#define COPWAIT_X 0
#define COPWAIT_X_BALLSTART 160
#define Y0 Y((256-280)/2)
#define STATIC_Y_AREA 40
#define BOOK_Y 256
#define STATIC_Y_START (BOOK_Y-STATIC_Y_AREA)
#define STATIC_Y_COMMANDS (STATIC_Y_AREA + STATIC_Y_AREA_PADDING * 2)

// Copper texture layout. Goal: Have 1 spare copper move per screen row for
// sprite fades, bplcon2 at stable Y positions.
//
// Small ball (tx line every Y)     |  Large ball (tx line every 2 Y)
//
// W C C C C C C C C C C C C X N N  |  W C C C C C C C C C C C C X w X
// W C C C C C C C C C C C C X N N  |  W C C C C C C C C C C C C X w X
// W C C C C C C C C C C C C X N N  |  W C C C C C C C C C C C C X w X
// ...                                 ...
//
// W - wait
// w - extra wait between W and next W (Y and Y+2)
// C - set color
// X - spare copper move
// N - copper NO-OP

#define COPPER_HALFROW_INSTRUCTIONS (ROTZOOM_W/2+4)
#define INSTRUCTIONS_PER_BALL (COPPER_HALFROW_INSTRUCTIONS * ROTZOOM_H * 3 + 500) // ?!
#define DEBUG_COLOR_WRITES 0
#define ZOOM_SHIFT 4
#define NORM_ZOOM (1 << ZOOM_SHIFT)
#define MIN_ZOOM (NORM_ZOOM / 5)
#define MAX_ZOOM (NORM_ZOOM * 8)
#define NO_OP 0x1fe

#if DEBUG_COLOR_WRITES // only set background color for debugging
#define SETCOLOR(x) CopMove16(cp, color[0], 0xf00)
#else
#define SETCOLOR(x) CopMove16(cp, color[x], 0xf00)
#endif
#define SETBG(vp, rgb) CopWait(cp, vp, 7); \
                       CopSetColor(cp, 0, rgb)


// Internally, u/v coordinates use 8 fractional bits
#define f2short(f) \
  (short)((float)(f) * 256.0)

typedef struct {
  PixmapT texture;
  short height;
  short angle;
  short angleDelta;
  short zoom;
  short zoomSinPos;
  short zoomSinStep;
  short zoomSinAmp;
  int u;
  int v;
  short uDelta;
  short vDelta;
  short screenX;
  short screenY;
  short screenLineHeight;
} BallT;

typedef struct {
  CopInsT *copperJumpTarget;
  CopInsT *waitBefore;
  CopInsT *paddingTop;
  CopInsT *paddingBottom;
  CopInsT *ballCopper;
  CopInsT *bplptr[DEPTH];
  CopInsT *bplcon1ins;
} BallCopInsertsT;

typedef struct {
  CopListT *cp;
  BallCopInsertsT inserts[BALLS];
  CopInsT *staticAreaCopperStart;
} BallCopListT;

static int active = 0;
static BallCopListT ballCopList[2];
static CopListT *bottomCp;
static CopInsT *bottomCpStart;
static BallT ball1;
static BallT ball2;
static BallT ball3;
static BallT *balls[BALLS] = { &ball1, &ball2, &ball3 };
static bool mouseControlled = false;
static bool mouseMoved = false;
static CopInsT staticYCommands[STATIC_Y_COMMANDS];

#include "data/texture_butterfly.c"
#include "data/ball_small.c"
#include "data/ball_large.c"
#include "data/book_bottom.c"

extern void PlotTextureAsm(char *copperDst asm("a0"),
                           char *texture   asm("a1"),
                           int  u          asm("d0"),
                           int  v          asm("d2"),
                           int  uDeltaCol  asm("d1"),
                           int  vDeltaCol  asm("d3"),
                           int  uDeltaRow  asm("d5"),
                           int  vDeltaRow  asm("d6"));

static void InitStaticYCommands(void) {
  short i, idx;
  idx = 0;
  for (i = 0; i < STATIC_Y_AREA_PADDING; i++) {
    staticYCommands[idx++] = (CopInsT) { .move = { .reg = NO_OP, .data = 0 } };
  }
  for (i = 0; i < STATIC_Y_AREA; i++) {
    staticYCommands[idx++] = (CopInsT) { .move = { .reg = CSREG(color[1]), .data = i * 0x100 } };
  }
  for (i = 0; i < STATIC_Y_AREA_PADDING; i++) {
    staticYCommands[idx++] = (CopInsT) { .move = { .reg = NO_OP, .data = 0 } };
  }
  idx = STATIC_Y_AREA_PADDING;
  staticYCommands[idx++] = (CopInsT) { .move = { .reg = CSREG(color[1]), .data = 0x0ff } };
  staticYCommands[idx++] = (CopInsT) { .move = { .reg = CSREG(color[1]), .data = 0xe70 } };
  staticYCommands[idx++] = (CopInsT) { .move = { .reg = CSREG(color[1]), .data = 0xd60 } };
  staticYCommands[idx++] = (CopInsT) { .move = { .reg = CSREG(color[1]), .data = 0xc50 } };
  staticYCommands[idx++] = (CopInsT) { .move = { .reg = CSREG(color[1]), .data = 0xb40 } };
  staticYCommands[idx++] = (CopInsT) { .move = { .reg = CSREG(color[1]), .data = 0xa30 } };
  idx = STATIC_Y_AREA_PADDING + STATIC_Y_AREA - 1;
  staticYCommands[--idx] = (CopInsT) { .move = { .reg = CSREG(color[1]), .data = 0xff0 } };
  staticYCommands[--idx] = (CopInsT) { .move = { .reg = CSREG(color[1]), .data = 0xe70 } };
  staticYCommands[--idx] = (CopInsT) { .move = { .reg = CSREG(color[1]), .data = 0xd60 } };
  staticYCommands[--idx] = (CopInsT) { .move = { .reg = CSREG(color[1]), .data = 0xc50 } };
  staticYCommands[--idx] = (CopInsT) { .move = { .reg = CSREG(color[1]), .data = 0xb40 } };
  staticYCommands[--idx] = (CopInsT) { .move = { .reg = CSREG(color[1]), .data = 0xa30 } };
}

// Create copper writes to color registers, leave out colors needed for sprites
static void InsertTextureCopperWrites(CopListT *cp) {
  short i;
  for (i=0; i<ROTZOOM_H; i++) {
    CopWait(cp, 0, COPWAIT_X);
    SETCOLOR(3);
    SETCOLOR(5);
    SETCOLOR(7);
    SETCOLOR(9);
    SETCOLOR(11);
    SETCOLOR(13);
    SETCOLOR(15);
    SETCOLOR(18);
    SETCOLOR(20);
    SETCOLOR(23);
    SETCOLOR(27);
    SETCOLOR(31);
    CopNoOpData(cp, i); // store row for copperlist debugging
    CopNoOpData(cp, i);
    CopNoOpData(cp, i);
    CopWait(cp, 0, COPWAIT_X);
    SETCOLOR(2);
    SETCOLOR(4);
    SETCOLOR(6);
    SETCOLOR(8);
    SETCOLOR(10);
    SETCOLOR(12);
    SETCOLOR(14);
    SETCOLOR(16);
    SETCOLOR(19);
    SETCOLOR(22);
    SETCOLOR(24);
    SETCOLOR(28);
    CopNoOpData(cp, i);
    CopNoOpData(cp, i);
    CopNoOpData(cp, i);
  }
}

static void InitStaticCommandsCopperList(CopListT *cp) {
  short y;

  CopInit(cp);
  for (y = STATIC_Y_START; y < BOOK_Y; y++) {
    CopWait(cp, y, COPWAIT_X);
    CopNoOp(cp);
  }
  CopMove32(cp, cop2lc, bottomCpStart);
  CopMove16(cp, copjmp2, 0);
}

static void InitBottomCopperList(CopListT *cp) {
  cp = NewCopList(80 + STATIC_Y_AREA * 2);
  CopInit(cp);
  bottomCpStart = cp->curr;
  CopWaitSafe(cp, BOOK_Y, 0);
  CopMove16(cp, bplcon0, BPLCON0_COLOR | BPLCON0_BPU(1));

  CopSetupBitplanes(cp, NULL, &book_bottom, book_bottom.depth);
  CopMove16(cp, bplcon0, BPLCON0_COLOR | BPLCON0_BPU(book_bottom.depth));
  CopMove16(cp, bplcon1, 0);

  CopLoadPal(cp, &book_pal, 0);
  SETBG(BOOK_Y + 5,  0x133);
  SETBG(BOOK_Y + 10, 0x124);
  SETBG(BOOK_Y + 15, 0x123);
  SETBG(BOOK_Y + 20, 0x122);
  SETBG(BOOK_Y + 25, 0x112);
  SETBG(BOOK_Y + 30, 0x012);
  SETBG(BOOK_Y + 35, 0x001);
  SETBG(BOOK_Y + 40, 0x002);
  CopWait(cp, BOOK_Y + book_bottom.height, COPWAIT_X);
  CopMove16(cp, bplcon0, BPLCON0_COLOR);
  CopEnd(cp);
}

static void InitCopperList(BallCopListT *ballCp) {
  short i, k;

  CopListT *cp           = NewCopList(INSTRUCTIONS_PER_BALL * 3 + 100);
  CopListT *staticAreaCp = NewCopList(10 + STATIC_Y_AREA * 2);

  ballCp->cp = cp;
  CopInit(cp);
  CopSetupDisplayWindow(cp, MODE_LORES, X(0), Y0, 320, 280);
  CopMove16(cp, dmacon, DMAF_SETCLR | DMAF_RASTER);
  CopSetupMode(cp, MODE_LORES, 0);
  CopSetColor(cp, 0, 0x134);
  CopSetColor(cp, 1, 0x000);
  CopSetupBitplaneFetch(cp, MODE_LORES, X(0), ball_large.width);
  CopMove32(cp, bplpt[0], ball_large.planes[0]);
  CopMove16(cp, bpl1mod, -WIDTH/8);
  CopMove16(cp, bpl2mod, -WIDTH/8);
  CopMove16(cp, bplcon0, BPLCON0_COLOR | BPLCON0_BPU(1));

  InitStaticCommandsCopperList(staticAreaCp);
  ballCp->staticAreaCopperStart = staticAreaCp->entry;

  // Copper structure per ball.
  //
  // <<CMD>> = instructions from static Y area
  // <<NOP>> = copper no-op or wait + static Y instruction (depending on ball size)
  //
  // [copperJumpTarget] COP2LCH  COP2LCL
  // [waitBefore]       WAIT_YY  <<CMD>>  BPLCON0
  // [bplptr]           BPL1PTH  BPL1PTL  BPL2PTH  BPL2PTL  BPL3PTH  BPL3PTL  BPL4PTH  BPL4PTL  BPL5PTH  BPL5PTL
  // [bplcon1ins]       BPLCON1  BPLCON0
  // [paddingTop]       WAIT_YY  <<CMD>>  [repeat BALL_PADDING_TOP times]
  // [ballCopper]       WAIT_YY  COLOR03  COLOR03  COLOR07    ...    COLOR27  COLOR31  <<CMD>>  <<NOP>>  <<NOP>>
  //                    WAIT_YY  COLOR02  COLOR04  COLOR06    ...    COLOR24  COLOR28  <<CMD>>  <<NOP>>  <<NOP>>
  //                      ...
  //                    WAIT_YY  COLOR03  COLOR03  COLOR07    ...    COLOR27  COLOR31  <<CMD>>  <<NOP>>  <<NOP>>
  //                    WAIT_YY  COLOR02  COLOR04  COLOR06    ...    COLOR24  COLOR28  <<CMD>>  <<NOP>>  <<NOP>>
  // [paddingBottom]    WAIT_YY  <<CMD>>  [repeat BALL_PADDING_BOTTOM times]
  //                    BPLCON0  BPL1MOD  BPL2MOD  COPJMP2
  //

  for (i = 0; i < BALLS; i++) {
    ballCp->inserts[i].copperJumpTarget = CopMove32(cp, cop2lc, bottomCpStart);
    ballCp->inserts[i].waitBefore = CopWait(cp, 0, COPWAIT_X_BALLSTART);
    CopNoOp(cp);
    CopMove16(cp, bplcon0, BPLCON0_COLOR | BPLCON0_BPU(0));
    CopSetupBitplanes(cp, ballCp->inserts[i].bplptr, &ball_large, ball_large.depth);
    ballCp->inserts[i].bplcon1ins = CopMove16(cp, bplcon1, 0);
    CopMove16(cp, bplcon0, BPLCON0_COLOR | BPLCON0_BPU(DEPTH));
    ballCp->inserts[i].paddingTop = cp->curr;
    for (k = 0; k < BALL_PADDING_TOP; k++) {
      CopWait(cp, 0, COPWAIT_X_BALLSTART);
      CopNoOp(cp);
    }
    ballCp->inserts[i].ballCopper = cp->curr;
    InsertTextureCopperWrites(cp);
    ballCp->inserts[i].paddingBottom = cp->curr;
    for (k = 0; k < BALL_PADDING_BOTTOM; k++) {
      CopWait(cp, 0, COPWAIT_X_BALLSTART);
      CopNoOp(cp);
    }
    CopMove16(cp, bplcon0, BPLCON0_COLOR | BPLCON0_BPU(1));
    CopMove16(cp, bpl1mod, -WIDTH/8);
    CopMove16(cp, bpl2mod, -WIDTH/8);
    CopMove16(cp, copjmp2, 0);
  }
}

static inline void SkipBall(BallCopInsertsT inserts, CopInsT *jumpTo) {
  CopInsSet32(inserts.copperJumpTarget, jumpTo);
  inserts.waitBefore->move.reg = CSREG(copjmp2);
}

static inline void DrawCopperBallTexture(BallT *ball, BallCopInsertsT inserts) {
  short sin, cos;
  int u, v;
  short zoom = ball->zoom + ((ball->zoomSinAmp * SIN(ball->zoomSinPos)) >> 12);

  sin = (zoom*SIN(ball->angle)) >> (4 + ZOOM_SHIFT);
  cos = (zoom*COS(ball->angle)) >> (4 + ZOOM_SHIFT);
  u = ball->u - sin * (ROTZOOM_W / 2) - cos * (ROTZOOM_H / 2);
  v = ball->v - cos * (ROTZOOM_W / 2) + sin * (ROTZOOM_H / 2);

  PlotTextureAsm((char *) inserts.ballCopper, (char *) ball->texture.pixels, u, v, sin, cos, cos, -sin);

  ball->angle += ball->angleDelta;
  ball->u += ball->uDelta;
  ball->v += ball->vDelta;
  ball->zoomSinPos += ball->zoomSinStep;
}

static void DrawCopperBall(BallT *ball, BallCopInsertsT inserts) {
  bool small = ball->height == SMALL_BALL_HEIGHT;
  short y = ball->screenY;
  short staticYPos = y - STATIC_Y_START + STATIC_Y_AREA_PADDING + 1;
  CopInsT *staticYSource;

  if (staticYPos < 0) {
    staticYPos = 0;
  } else if (staticYPos > STATIC_Y_AREA_PADDING + STATIC_Y_AREA) {
    staticYPos = STATIC_Y_AREA_PADDING + STATIC_Y_AREA - 1;
  }

  staticYSource = staticYCommands + staticYPos;

  // Set X
  {
    short x = (small ? SMALL_BALL_CENTER : LARGE_BALL_CENTER) - ball->screenX;
    short bplSkip = (x / 8) & 0xfe;
    short shift = 15 - (x & 0xf);
    BitmapT bitmap = small ? ball_small : ball_large;
    if (ball->screenY <= Y0) {
      bplSkip += (Y0 - 1 - ball->screenY) * WIDTH / 8;
    }
    CopInsSet32(inserts.bplptr[0], bitmap.planes[0]+bplSkip);
    CopInsSet32(inserts.bplptr[1], bitmap.planes[1]+bplSkip);
    CopInsSet32(inserts.bplptr[2], bitmap.planes[2]+bplSkip);
    CopInsSet32(inserts.bplptr[3], bitmap.planes[3]+bplSkip);
    CopInsSet32(inserts.bplptr[4], bitmap.planes[4]+bplSkip);
    CopInsSet16(inserts.bplcon1ins, (shift << 4) | shift);
  }

  // Update copper waits according to Y
  {
    short i;
    short yInc = small ? SMALL_BALL_Y_INC : LARGE_BALL_Y_INC;
    CopInsT *textureCopper = inserts.ballCopper;
    CopInsT *paddingTop    = inserts.paddingTop;
    CopInsT *paddingBottom = inserts.paddingBottom;

    if (y > Y0) {
      inserts.waitBefore->wait.vp = y;
    } else {
      inserts.waitBefore->wait.vp = 0;
    }
    inserts.waitBefore->wait.hp = COPWAIT_X_BALLSTART | 1;

    for (i = 0; i < BALL_PADDING_TOP; i++) {
      y++;
      if (y > Y0 && y < BOOK_Y - 1) {
        paddingTop->wait.vp = y;
      } else {
        paddingTop->wait.vp = 0;
      }
      paddingTop++;
      if (y > BOOK_Y - 1) {
        paddingTop->move.reg = CSREG(copjmp2);
      } else {
        paddingTop->move.reg  = staticYSource->move.reg;
        paddingTop->move.data = staticYSource->move.data;
        staticYSource++;
      }
      paddingTop++;
    }
    for (i = 0; i < ROTZOOM_H * 2 && i < BOOK_Y - 1; i++) {
      if (y > Y0) {
        textureCopper->wait.vp = y;
      } else {
        textureCopper->wait.vp = 0;
      }
      textureCopper += ROTZOOM_W / 2 + 1;
      y += yInc;

      // Abort ball when the bottom book area starts
      if (y < BOOK_Y - 1) {
        textureCopper->move.reg  = staticYSource->move.reg;
        textureCopper->move.data = staticYSource->move.data;
        staticYSource++;
      } else {
        textureCopper->move.reg = CSREG(copjmp2);
      }
      textureCopper++;

      if (yInc == SMALL_BALL_Y_INC) {
        textureCopper->move.reg = NO_OP;
        textureCopper++;
        textureCopper->move.reg = NO_OP;
        textureCopper++;
      } else {
        textureCopper->wait.vp = (y > Y0) ? y - 1 : 0;
        textureCopper->wait.hp = COPWAIT_X | 1;
        textureCopper->wait.vpmask = 0xff;
        textureCopper->wait.hpmask = 0x00;
        textureCopper++;
        textureCopper->move.reg  = staticYSource->move.reg;
        textureCopper->move.data = staticYSource->move.data;
        staticYSource++;
        textureCopper++;
      }
    }
    for (i = 0; i < BALL_PADDING_BOTTOM; i++) {
      if (y > Y0 && y < BOOK_Y - 1) {
        paddingBottom->wait.vp = y;
      } else {
        paddingBottom->wait.vp = 0;
      }
      paddingBottom++;
      if (y > BOOK_Y - 1) {
        paddingBottom->move.reg = CSREG(copjmp2);
      } else {
        paddingBottom->move.reg  = staticYSource->move.reg;
        paddingBottom->move.data = staticYSource->move.data;
        staticYSource++;
      }
      paddingBottom++;
      y++;
    }
  }

  DrawCopperBallTexture(ball, inserts);
}

static void Randomize(BallT *ball) {
  ball->height = (random() & 1) ? SMALL_BALL_HEIGHT : LARGE_BALL_HEIGHT;
  ball->zoom = NORM_ZOOM;
  ball->zoomSinPos = random();
  ball->zoomSinAmp = NORM_ZOOM * 6;
  ball->zoomSinStep = random() & 0xf;
  ball->uDelta = random() & 0x1ff;
  ball->vDelta = random() & 0x1ff;
  ball->angle = random();
  ball->angleDelta = random() & 0x3f;
  if (ball->angle & 1) ball->angleDelta = -ball->angleDelta;
  ball->screenX = -64 + (random() & 0x7f);
}

static void Init(void) {
  MouseInit(-100, -200, 100, 256);

  InitStaticYCommands();
  InitBottomCopperList(bottomCp);
  InitCopperList(&ballCopList[0]);
  InitCopperList(&ballCopList[1]);

  Randomize(balls[0]);
  balls[0]->height = LARGE_BALL_HEIGHT;
  balls[0]->screenY = BOOK_Y;
  balls[0]->texture = texture_butterfly;

  Randomize(balls[1]);
  balls[1]->height = SMALL_BALL_HEIGHT;
  balls[1]->screenY = balls[0]->screenY + LARGE_BALL_HEIGHT + 1;
  balls[1]->texture = texture_butterfly;

  Randomize(balls[2]);
  balls[2]->height = SMALL_BALL_HEIGHT;
  balls[2]->screenY = balls[1]->screenY + SMALL_BALL_HEIGHT + 1;
  balls[2]->texture = texture_butterfly;

  custom->dmacon = DMAF_MASTER | DMAF_COPPER | DMAF_SETCLR;
}

static bool MoveBallAndIsStillVisible(BallT *ball) {
  short y = mouseControlled ? ball->screenY : ball->screenY--;
  short lastVisibleY = Y0 - ball->height;
  return y > lastVisibleY;
}

static void Kill(void) {
  MouseKill();
  DeleteCopList(ballCopList[0].cp);
  DeleteCopList(ballCopList[1].cp);
  DeleteCopList(bottomCp);
}

static void HandleEvent(void) {
  EventT ev[1];

  mouseMoved = false;

  if (!PopEvent(ev))
    return;

  if (ev->type == EV_MOUSE) {
    balls[0]->screenX = ev->mouse.x;
    balls[0]->screenY = ev->mouse.y;
    balls[1]->screenY = balls[0]->screenY + LARGE_BALL_HEIGHT + 50;
    balls[2]->screenY = balls[0]->screenY + LARGE_BALL_HEIGHT * 3;
    mouseControlled = true;
    mouseMoved = true;
  }

}

static inline bool IsVisible(BallT *ball) {
  return (ball->screenY + ball->height >= Y0) && (ball->screenY < BOOK_Y - 1);
}

static void DrawBalls(void) {
  BallT *top, *middle, *bottom;

  if (!IsVisible(balls[0])) {
    top    = NULL;
    middle = NULL;
    bottom = NULL;
  } else if (!IsVisible(balls[1])) {
    DrawCopperBall(balls[0], ballCopList[active].inserts[2]);
    top    = NULL;
    middle = NULL;
    bottom = balls[0];
  } else if (!IsVisible(balls[2])) {
    DrawCopperBall(balls[0], ballCopList[active].inserts[1]);
    DrawCopperBall(balls[1], ballCopList[active].inserts[2]);
    top    = NULL;
    middle = balls[0];
    bottom = balls[1];
  } else {
    DrawCopperBall(balls[0], ballCopList[active].inserts[0]);
    DrawCopperBall(balls[1], ballCopList[active].inserts[1]);
    DrawCopperBall(balls[2], ballCopList[active].inserts[2]);
    top    = balls[0];
    middle = balls[1];
    bottom = balls[2];
  }

  // Bottom ball

  if (bottom == NULL) {
    SkipBall(ballCopList[active].inserts[0], ballCopList[active].staticAreaCopperStart);
  } else {
    CopInsT *bottomBallCopperStart;
    short bottomBallYEnd;
    bottomBallYEnd = bottom->screenY + bottom->height;
    if (bottom->screenY > STATIC_Y_START) {
      bottomBallCopperStart = ballCopList[active].staticAreaCopperStart;
      bottomBallYEnd = bottom->screenY + bottom->height;
      CopInsSet32(ballCopList[active].inserts[2].copperJumpTarget, bottomCpStart);
    } else {
      short staticYCovered = bottomBallYEnd - STATIC_Y_START;
      bottomBallCopperStart = ballCopList[active].inserts[2].copperJumpTarget;
      if (staticYCovered > 0) {
        if (staticYCovered > STATIC_Y_AREA) staticYCovered = STATIC_Y_AREA;
        CopInsSet32(ballCopList[active].inserts[2].copperJumpTarget, ballCopList[active].staticAreaCopperStart + staticYCovered * 2);
      } else {
        CopInsSet32(ballCopList[active].inserts[2].copperJumpTarget, ballCopList[active].staticAreaCopperStart);
      }
    }

    // Middle ball

    if (middle == NULL) {
      // No middle ball: jump to bottom ball directly
      SkipBall(ballCopList[active].inserts[0], bottomBallCopperStart);
    } else {
      // Middle ball start, exit
      short middleBallYEnd = middle->screenY + middle->height;
      short staticYCovered = middleBallYEnd - STATIC_Y_START;
      if (staticYCovered > 0) {
        if (staticYCovered > STATIC_Y_AREA) staticYCovered = STATIC_Y_AREA;
        CopInsSet32(ballCopList[active].inserts[1].copperJumpTarget, ballCopList[active].staticAreaCopperStart + staticYCovered * 2);
      } else {
        CopInsSet32(ballCopList[active].inserts[1].copperJumpTarget, bottomBallCopperStart);
      }

      // Top ball

      if (top == NULL) {
        SkipBall(ballCopList[active].inserts[0], ballCopList[active].inserts[1].copperJumpTarget);
      } else {
        CopInsSet32(ballCopList[active].inserts[0].copperJumpTarget, ballCopList[active].inserts[1].copperJumpTarget);
      }
    }
  }

  // Static Y area, possibly containing a COP2JMP before the end

  {
    short y;
    short bottomBallY = bottom == NULL ? -1 : bottom->screenY;
    CopInsT *rewriteStaticCp = ballCopList[active].staticAreaCopperStart;
    short sourcePos = STATIC_Y_AREA_PADDING;
    for (y = STATIC_Y_START; y < BOOK_Y; y++) {
      if (y == bottomBallY) {
        void *data = ballCopList[active].inserts[2].copperJumpTarget;
        u_short *ins = (u_short *) rewriteStaticCp;
        *ins++ = CSREG(cop2lc);
        *ins++ = (u_int) data >> 16;
        *ins++ = CSREG(cop2lc) + 2;
        *ins++ = (u_int) data;
        *ins++ = CSREG(copjmp2);
        goto staticRewriteDone;
      }
      rewriteStaticCp->wait.vp = y;
      rewriteStaticCp->wait.hp = COPWAIT_X | 1;
      rewriteStaticCp->wait.vpmask = 0xff;
      rewriteStaticCp->wait.hpmask = 0xfe;
      rewriteStaticCp++;
      *rewriteStaticCp++ = staticYCommands[sourcePos++];
    }
    {
      void *data = bottomCpStart;
      u_short *ins = (u_short *) rewriteStaticCp;
      *ins++ = CSREG(cop2lc);
      *ins++ = (u_int) data >> 16;
      *ins++ = CSREG(cop2lc) + 2;
      *ins++ = (u_int)data;
      *ins++ = CSREG(copjmp2);
    }
    staticRewriteDone:
  }
}

static void Render(void) {
  HandleEvent();
  MoveBallAndIsStillVisible(balls[1]);
  MoveBallAndIsStillVisible(balls[2]);

  if (!MoveBallAndIsStillVisible(balls[0]) && !mouseControlled) {
    short lastY = balls[2]->screenY + balls[2]->height;
    BallT *tmp = balls[0];
    balls[0] = balls[1];
    balls[1] = balls[2];
    balls[2] = tmp;
    balls[2]->screenY = lastY + 1 + (random() &0x3f);
    if (balls[2]->screenY < BOOK_Y) balls[2]->screenY = BOOK_Y + (random() & 0x3f);
    Randomize(balls[2]);
  }
  // Make sure inserts[2] is always used for the bottom-most ball
  DrawBalls();
  TaskWaitVBlank();
  active ^= 1;
  CopListRun(ballCopList[active].cp);
}

EffectT Effect = { NULL, NULL, Init, Kill, Render };
