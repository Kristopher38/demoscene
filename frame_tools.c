#include <clib/alib_stdio_protos.h>
#include <clib/graphics_protos.h>
#include <inline/graphics_protos.h>
#include <proto/graphics.h>

#include "frame_tools.h"

void RenderFrameNumber(int frameNumber) {
  struct RastPort *rastPort = GetCurrentRastPort();

  SetDrMd(rastPort, JAM1);

  char number[5];

  sprintf(number, "%04d", frameNumber);

  Move(rastPort, 2, 8);
  Text(rastPort, number, 4);
}
