#ifndef HWSENSOR_OLH_BACKEND_H
#define HWSENSOR_OLH_BACKEND_H

#include "sensor.h"

#define OLH_PREFIX "olh:"

/* Connect/recv timeouts for the discovery-time probe: ckb_info() (which
 * calls discovery) is killed by the GUI if it doesn't exit within 1s
 * (AnimScript::load()) -- keep worst case well under that. */
#define OLH_CONNECT_TIMEOUT_MS 150
#define OLH_RECV_TIMEOUT_MS    300

/* Fetches http://127.0.0.1:27003/api/devices/ (or $HWSENSOR_OLH_URL, for
 * tests -- accepts both http:// and file:// so parsing can be tested with
 * zero network), and appends one entry per channel field that has
 * HasTemps/HasSpeed true. Never fails loudly: unreachable service,
 * malformed JSON, or an empty device list just contribute nothing. */
size_t olh_discover(sensor_list* list);

/* id_after_prefix is "<serial>/<channelId>/<field>" (field is "temperature"
 * or "rpm"), e.g. "20A130844B4B/0/temperature". Does its own fresh fetch +
 * parse (same source as discovery) -- simplest correct option for a ~1s
 * poll cadence. Returns 1 + *out_value on success, 0 on any failure. */
int olh_read(const char* id_after_prefix, double* out_value);

#endif /* HWSENSOR_OLH_BACKEND_H */
