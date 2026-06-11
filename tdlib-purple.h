#ifndef _TDLIB_PURPLE_H
#define _TDLIB_PURPLE_H

#include "transceiver.h"
#include <purple.h>

extern "C" {
    gboolean purple_init_plugin(PurplePlugin *plugin);
    PurplePluginInfo *getPluginInfo();
    PurplePluginProtocolInfo *tdlib_purple_get_prpl_info();
};
void tgprpl_set_test_backend(ITransceiverBackend *backend);
void tgprpl_set_single_thread();

#endif
