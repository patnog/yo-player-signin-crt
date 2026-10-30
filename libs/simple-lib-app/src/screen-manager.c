// screen-manager - owns the active screen + stack and drives the active screen (see screen-manager.h).
#include "screen-manager.h"
#include <stddef.h>
#include "dbg.h"

#define SCREEN_MAX_STACK 8

static Screen *screenStack[SCREEN_MAX_STACK];
static int screenDepth = 0;
static Screen *currentScreen = NULL;

void changeScreen(Screen *newScreen)
{
   if (currentScreen && currentScreen->term) currentScreen->term();
   if (currentScreen) currentScreen->status = SCREEN_TERMINATED;
   if (screenDepth > 0) screenDepth--;
   currentScreen = newScreen;
   if (currentScreen) {
      screenStack[screenDepth++] = currentScreen;
      if (currentScreen->init) currentScreen->init();
      currentScreen->status = SCREEN_ACTIVE;
   } else {
      screenDepth = 0;
   }
}

void pushScreen(Screen *newScreen)
{
   if (!newScreen || screenDepth >= SCREEN_MAX_STACK) {
      logInfo("[diag-screen] pushScreen rejected newScreen=%p depth=%d\n", newScreen, screenDepth);
      return;
   }
   logInfo("[diag-screen] pushScreen begin depth=%d current=%p next=%p\n", screenDepth, currentScreen, newScreen);
   if (currentScreen && currentScreen->suspend) { logInfo("[diag-screen] current suspend begin\n"); currentScreen->suspend(); logInfo("[diag-screen] current suspend returned\n"); }
   if (currentScreen) currentScreen->status = SCREEN_SUSPENDED;
   currentScreen = newScreen;
   screenStack[screenDepth++] = currentScreen;
   if (currentScreen->init) { logInfo("[diag-screen] new screen init begin depth=%d\n", screenDepth); currentScreen->init(); logInfo("[diag-screen] new screen init returned\n"); }
   currentScreen->status = SCREEN_ACTIVE;
   logInfo("[diag-screen] pushScreen complete depth=%d\n", screenDepth);
}

void popScreen(void)
{
   if (screenDepth <= 0) return;
   if (currentScreen->term) currentScreen->term();
   currentScreen->status = SCREEN_TERMINATED;
   screenDepth--;
   if (screenDepth > 0) {
      currentScreen = screenStack[screenDepth - 1];
      if (currentScreen->resume) currentScreen->resume();
      currentScreen->status = SCREEN_ACTIVE;
   } else {
      currentScreen = NULL;
   }
}

void updateScreen(void) { if (currentScreen && currentScreen->update) currentScreen->update(); }
void drawScreen(void)   { if (currentScreen && currentScreen->draw) currentScreen->draw(); }
