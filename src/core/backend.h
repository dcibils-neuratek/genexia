/*
 * backend.h - what the Genexia front end needs from a console.
 *
 * Everything else (window, filters, options menu, controls, speed, frame
 * pacing, run-ahead, save states) lives in core/ and works for every backend.
 * Reusable chips live in chips/; backends/<console>/ holds only what is unique
 * to that console. To add a console: implement this struct, tag its state
 * EMU_STATE, render into vdp_framebuffer, and register it in core/main.c.
 */
#ifndef BACKEND_H
#define BACKEND_H
typedef struct {
    const char *name;
    int    sample_rate;           /* rate of the audio the backend produces */
    double gain;                  /* output level */
    void (*reset)(void);
    void (*run_frame)(void);      /* one video frame; audio goes to the front end's buffer */
} Backend;
#endif
