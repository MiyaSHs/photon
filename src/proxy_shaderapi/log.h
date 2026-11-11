/* Minimal file logger for the proxy. Path: %PHOTON_LOG_DIR%\photon_proxy.log; disabled if the variable is unset. */
#ifndef PHOTON_LOG_H
#define PHOTON_LOG_H
void photon_log(const char *fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
#endif
