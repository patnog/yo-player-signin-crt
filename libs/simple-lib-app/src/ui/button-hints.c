// button-hints - centered "[glyph] caption" pad-hint row (see ui/button-hints.h).

#include "ui/button-hints.h"
#include <string.h>

#define ICON_GAP 8    // between a glyph and its caption
#define HINT_GAP 30   // between one hint and the next
#define PAIR_GAP 8    // between two caption-less glyphs that form a pair (L1 + R1)

static float hintFixedSizeScale(void)
{
   return isGfxSdVideoOutput() ? 1.5f : 1.0f;
}

void initButtonHints(ButtonHints *bar, Font *font, int y, int glyphHeight, int captionSize, uint32_t captionColor)
{
   memset(bar, 0, sizeof *bar);
   bar->font         = font;
   bar->y            = y;
   bar->glyphHeight  = glyphHeight;
   bar->captionSize  = captionSize;
   bar->captionColor = captionColor;
}

int addButtonHint(ButtonHints *bar, GfxTexture glyph, const char *caption)
{
   if (bar->count >= MAX_BUTTON_HINTS) return -1;
   ButtonHint *hint = &bar->hints[bar->count++];
   hint->glyph     = glyph;
   hint->drawWidth = glyph.h > 0 ? glyph.w * bar->glyphHeight / glyph.h : bar->glyphHeight;
   hint->pairNext  = (caption == NULL || caption[0] == 0);
   initLabel(&hint->caption, bar->font, 0, 0, AUTO, AUTO, bar->captionSize, bar->captionColor, TEXT_NOWRAP, caption);
   if (isGfxSdVideoOutput())
      setLabelFixedRenderScale(&hint->caption, 1.5f, 1);
   // a caption-less hint is just the glyph; drop the icon gap so it clusters with the next one
   hint->width = hint->drawWidth + (hint->pairNext ? 0 : ICON_GAP + hint->caption.tt.tex.w);
   return bar->count - 1;
}

void setButtonHintCaption(ButtonHints *bar, int index, const char *caption)
{
   if (index < 0 || index >= bar->count) return;
   ButtonHint *hint = &bar->hints[index];
   setLabelText(&hint->caption, caption);
   hint->pairNext = (caption == NULL || caption[0] == 0);
   hint->width = hint->drawWidth + (hint->pairNext ? 0 : ICON_GAP + hint->caption.tt.tex.w);
}

// UI geometry is transformed by gfx.c after these coordinates are submitted.
// Stable SD hint rows compensate their anchors and spacing before drawing so
// the enlarged glyphs/captions remain inside the screen-safe footer area.
static float hintScale(const ButtonHints *bar)
{
   float scale = bar->stableLayout ? getGfxUiScaleFactor() : 1.0f;
   return scale < 1.0f ? 1.0f : scale;
}

static int hintIconGap(const ButtonHints *bar)
{
   float scale = hintScale(bar);
   return bar->stableLayout ? (int)(ICON_GAP / scale + 0.5f) : ICON_GAP;
}

static int hintRowGap(const ButtonHints *bar, int paired)
{
   int base = paired ? PAIR_GAP : (bar->gap > 0 ? bar->gap : HINT_GAP);
   float scale = hintScale(bar);
   return bar->stableLayout ? (int)(base / scale + 0.5f) : base;
}

static int hintDrawY(const ButtonHints *bar)
{
   if (!bar->stableLayout) return bar->y;
   float scale = hintScale(bar);
   int screenH = getGfxScreenHeight();
   float center = screenH * 0.5f;
   // Keep the rendered top edge at the caller's reserved footer coordinate;
   // the glyph and caption themselves still grow with the selected zoom.
   return (int)(center + (bar->y - center) / scale + 0.5f);
}

static int drawHint(ButtonHints *bar, ButtonHint *hint, int x, int y)
{
   float fixedScale = hintFixedSizeScale();
   int glyphHeight = (int)(bar->glyphHeight * fixedScale + 0.5f);
   int glyphWidth = (int)(hint->drawWidth * fixedScale + 0.5f);
   int rowMid = y + glyphHeight / 2;
   if (hint->glyph.offset) {
      if (isGfxSdVideoOutput())
         drawGfxTextureFixedSizeScaledPosition(x, y, glyphWidth, glyphHeight, hint->glyph, 0, 0, 1, 1, 0xFFFFFFFF, GFX_FILTER_LINEAR);
      else
         drawGfxTexture(x, y, glyphWidth, glyphHeight, hint->glyph, 0, 0, 1, 1, 0xFFFFFFFF, GFX_FILTER_LINEAR);
   }
   x += glyphWidth;
   if (hint->pairNext) return x;   // glyph only - the caption belongs to the hint it pairs with
   x += hintIconGap(bar);
   moveLabel(&hint->caption, x, rowMid - hint->caption.tt.tex.h / 2);
   drawLabel(&hint->caption);
   return x + hint->caption.tt.tex.w;
}

void setButtonHintGap(ButtonHints *bar, int gap)
{
   bar->gap = gap;
}

void setButtonHintsStableLayout(ButtonHints *bar, int enabled)
{
   if (bar)
      bar->stableLayout = enabled != 0;
}

void setButtonHintShown(ButtonHints *bar, int index, int shown)
{
   if (index < 0 || index >= bar->count) return;
   bar->hints[index].hidden = !shown;
}

void drawButtonHintsAt(ButtonHints *bar, int x)
{
   int y = hintDrawY(bar);
   for (int i = 0; i < bar->count; i++) {
      if (bar->hints[i].hidden) continue;

      x = drawHint(bar, &bar->hints[i], x, y);
      x += hintRowGap(bar, bar->hints[i].pairNext);
   }
}

void drawButtonHints(ButtonHints *bar, int screenWidth)
{
   float positionScale = isGfxSdVideoOutput() ? getGfxUiScaleFactor() : 1.0f;
   if (positionScale < 1.0f) positionScale = 1.0f;
   float fixedScale = hintFixedSizeScale();
   float totalRendered = 0.0f;
   for (int i = 0; i < bar->count; i++) {
      if (bar->hints[i].hidden) continue;

      ButtonHint *hint = &bar->hints[i];
      totalRendered += hint->drawWidth * fixedScale;
      if (!hint->pairNext)
         totalRendered += (hintIconGap(bar) + hint->caption.tt.tex.w) * positionScale;
      if (i < bar->count - 1)
         totalRendered += hintRowGap(bar, hint->pairNext) * positionScale;
   }

   // The anchor still follows the selected UI zoom, but the glyphs and captions
   // have fixed 1.5x dimensions. Invert the anchor transform to keep the row centred.
   int x = (int)(screenWidth * 0.5f - totalRendered / (2.0f * positionScale) + 0.5f);
   drawButtonHintsAt(bar, x);
}

void termButtonHints(ButtonHints *bar)
{
   for (int i = 0; i < bar->count; i++) freeLabel(&bar->hints[i].caption);
   bar->count = 0;
}
