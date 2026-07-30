#ifndef MAIN_H
#define MAIN_H

#include <nusys.h>

/* A world is being generated and compiled in slices.  The picker keeps
   orbiting the outgoing world while this runs, so the menu shows progress to
   explain why the highlighted slot has not changed yet. */
u8 worldJobActive();
u8 worldJobProgress();

#endif /* MAIN_H */