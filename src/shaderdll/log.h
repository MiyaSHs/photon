/* File logger for photon_shaders.dll: %PHOTON_LOG_DIR%\photon_shaders.log (same scheme as the proxy's log);
 * disabled when the variable is unset. Never called per draw (baseshader.c logs the 1st and every 600th). */
#ifndef PHOTON_SHADERS_LOG_H
#define PHOTON_SHADERS_LOG_H
void photon_log(const char *fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
#endif
