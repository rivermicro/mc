/** \file dirwatch.h
 *  \brief Header: dynamic directory watch (inotify-based)
 */

#ifndef MC__DIRWATCH_H
#define MC__DIRWATCH_H

#include "lib/global.h"
#include "panel.h"  // WPanel

/* Enable or disable dynamic directory watching. */
void dirwatch_set_enabled (gboolean enabled);

/* Notify watcher that a panel's directory has changed. */
void dirwatch_panel_dir_changed (WPanel *panel);

#endif
