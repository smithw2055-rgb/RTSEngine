#define SOKOL_IMPL

#include <sokol_app.h>
#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <sokol_log.h>

#include "SokolComposition.h"

sapp_desc sokol_main(int, char**) {
    return rts::desktop_demo::MakePlayableDesktopDescription();
}
