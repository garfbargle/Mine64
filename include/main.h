#ifndef MAIN_H
#define MAIN_H

#include <nusys.h>

/* A world is being generated and compiled in slices.  The picker keeps
   orbiting the outgoing world while this runs, so the menu shows progress to
   explain why the highlighted slot has not changed yet. */
u8 worldJobActive();
u8 worldJobProgress();
/* Ask for the world to be written to the cart.  The write is sliced across
   callbacks like every other long job, so the caller carries on and hears
   back through menuSaveFinished. */
void requestWorldSave(void);

#endif /* MAIN_H */