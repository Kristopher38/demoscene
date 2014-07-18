#include <stdarg.h>

#include <proto/exec.h>

#include "console.h"

void ConsoleInit(ConsoleT *console, BitmapT *bitmap, TextFontT *font) {
  console->bitmap = bitmap;
  console->font = font;
  console->width = bitmap->width / 8;
  console->height = bitmap->height / 8;
  console->cursor.x = 0;
  console->cursor.y = 0;
}

__regargs void ConsoleSetCursor(ConsoleT *console, UWORD x, UWORD y) {
  console->cursor.x = x;
  console->cursor.y = y;
}

static __regargs void ConsoleNextLine(ConsoleT *console) {
  console->cursor.x = 0;

  if (++console->cursor.y >= console->height)
    console->cursor.y = 0;
}

static __regargs void ConsoleNextChar(ConsoleT *console) {
  if (++console->cursor.x >= console->width) {
    ConsoleNextLine(console);
  }
}

static __regargs void BitmapPutChar(BitmapT *bitmap, UBYTE plane, TextFontT *font,
                                    UWORD x, UWORD y, char c)
{
  UBYTE *src = font->tf_CharData;
  UBYTE *dst = bitmap->planes[plane];
  UWORD swidth = font->tf_Modulo;
  UWORD dwidth = bitmap->width / 8;
  WORD h = 8;

  src += c - 32;
  dst += dwidth * y * 8 + x;

  do {
    *dst = *src;
    src += swidth;
    dst += dwidth;
  } while (--h > 0);
}

__regargs void ConsoleDrawChar(ConsoleT *console, UWORD x, UWORD y, char c) {
  BitmapPutChar(console->bitmap, 0, console->font, x, y, c);
}

__regargs void ConsoleDrawBox(ConsoleT *console, UWORD x, UWORD y, UWORD w, UWORD h) {
  int i;

  w += x - 1;
  h += y - 1;

  ConsoleDrawChar(console, x, y, '+');
  ConsoleDrawChar(console, x, h, '+');
  ConsoleDrawChar(console, w, y, '+');
  ConsoleDrawChar(console, w, h, '+');

  for (i = x + 1; i < w; i++) {
    ConsoleDrawChar(console, i, y, '-');
    ConsoleDrawChar(console, i, h, '-');
  }

  for (i = y + 1; i < h; i++) {
    ConsoleDrawChar(console, x, i, '|');
    ConsoleDrawChar(console, w, i, '|');
  }
}

__regargs void ConsolePutChar(ConsoleT *console, char c) {
  switch (c) {
    case '\r':
      break;
    case '\n':
      ConsoleNextLine(console);
      break;
    default:
      if (c < 32)
        return;
      BitmapPutChar(console->bitmap, 0, console->font, console->cursor.x, console->cursor.y, c);
      ConsoleNextChar(console);
      break;
  }
}

__regargs void ConsolePutStr(ConsoleT *console, const char *str)
{
  char c;

  while ((c = *str++))
    ConsolePutChar(console, c);
}

static void OutputToConsole(char c asm("d0"), ConsoleT *console asm("a3")) {
  ConsolePutChar(console, c);
}

void ConsolePrint(ConsoleT *console, const char *format, ...) {
  va_list args;

  va_start(args, format);
  RawDoFmt(format, args, (void (*)())OutputToConsole, console);
  va_end(args);
}
