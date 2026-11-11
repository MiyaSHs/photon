/* Per-frame accounting (T1.4). The frame boundary is IShaderDevice::Present (ShaderDevice001 slot 9), which runs on
 * the MatQueue render thread in queued mode (FEASIBILITY.md section 8.2). Writes, if PHOTON_LOG_DIR is set:
 *   photon_frames.csv: one row per frame: frame, time, dt, the Present thread, call totals per wrapped interface,
 *                      and per-frame counts of headline IShaderAPI slots (RenderPass, Bind, SetLight, ...).
 *   photon_slots.csv:  cumulative count per (interface, slot, name), rewritten every 600 frames and at exit.
 *
 * P0 CPU meters (analysis/15_lighting_plan_v2.md P0, 15c §2), only while photon_frames.csv is open (PHOTON_LOG_DIR):
 * a "photon-meter" thread samples every 500 ms and publishes 1-s rolling CPU% per thread role; each CSV row carries
 * the latest published window (the values step every 500 ms; CPU% of one thread is <= 100, sums <= 100 x cores).
 * The meter thread does all the sampling, so no game thread pays for it:
 *   queued_mode       1 if this row's Present thread is not the main thread
 *   cpu_window_ms     length of the window the CPU% columns cover (0 and the CPU% columns empty: none yet)
 *   main_cpu_pct      the main thread = the thread that first called CreateInterface (materialsystem loading us on
 *                     the engine main thread, which also runs Lua) [likely; the log lists every thread]
 *   mq_cpu_pct        the render thread = the Present thread once it differs from main (MatQueue in queued mode);
 *                     it stays that thread through short sync fallbacks; empty until queued mode was seen
 *   dxvk_cpu_pct      all DXVK threads: enumerated with NtQuerySystemInformation (Toolhelp32 + GetThreadTimes as the
 *                     fallback, PHOTON_METER_TOOLHELP=1 forces it; both read the same Wine counters), named "dxvk-*" (GetThreadDescription), else a start
 *                     address in d3d9/dxgi/d3d11.dll; dxvk_cs_cpu_pct is the "dxvk-cs" thread alone (empty if unnamed)
 *   other_cpu_pct     every other thread the Win32 side knows (the meter thread itself excluded)
 *   proc_cpu_pct      GetProcessTimes: under Wine this is times() for the whole Unix process, so it also covers
 *                     Unix-only threads (lavapipe/llvmpipe, driver) that Toolhelp32 does not list [likely]
 *   dxvk_threads      DXVK threads currently tracked
 *   mq_photon_us      QPC us this frame inside Photon's own RenderPass work (vlg.c pre_draw / flag-off, excluding
 *                     the stock RenderPass); mq_renderpass_us = the whole hooked RenderPass (ours + stock). Each
 *                     includes about one QPC call of timer overhead per RenderPass (~0.11 us under Wine/Proton)
 *   mq_photon_ours_us the part of mq_photon_us spent on RenderPasses of a snapshot marked ours (the light-block or
 *                     c56 path); mq_photon_us / vlg_rp_ours overstates the per-draw cost, since every non-ours
 *                     RenderPass (flag-off check, ~4x as many) adds to mq_photon_us too
 * CPU time comes from GetThreadTimes (QueryThreadCycleTime is a stub in Proton-GE 11, analysis/15c); its granularity
 * under Wine is the /proc clock tick, logged as "GetThreadTimes step gcd" by the meter summary. */
#ifndef PHOTON_FRAMES_H
#define PHOTON_FRAMES_H
#include <stdint.h>
void photon_frame_end(void);
void photon_frames_flush(void);   /* write photon_slots.csv now */
uint64_t photon_frame_count(void);
const char *photon_slot_name(const char *iface, size_t slot, char *buf, size_t buflen);
void photon_frames_note_main(unsigned long tid);   /* the first CreateInterface caller (proxy.c load_stock) */
int photon_perf_on(void);                          /* the meters run (CSV open): RenderPass timers wanted */
void photon_mq_photon_add(int64_t photon_ticks, int64_t renderpass_ticks, int ours);   /* QPC ticks, any thread */
/* S1: stops the meter thread and waits for it (bounded). Call at IShaderDeviceMgr::Shutdown/Disconnect, never from
 * DllMain (the thread's exit needs the loader lock). Idempotent. */
void photon_frames_shutdown(const char *why);
int photon_frames_meter_pinned(void);   /* the meter thread still holds its reference to this DLL */
#endif
