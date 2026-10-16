/* A SAMPLING PROFILER FOR THE GAME THREAD - measurement only, off by default.
 *
 * LSW2_SAMPLE_PROF=<path prefix> starts a background thread that, about once a
 * millisecond, suspends the game (emulation) thread, reads its instruction
 * pointer, and resumes it. Every ten seconds the histogram of sampled addresses
 * is written to <prefix>.<n>.txt with the module base, the game state and the
 * frame rate in its header, then cleared - so each file is one ten-second
 * window of what the frame was actually spending its time on.
 *
 * tools/sampleprof_report.py maps the addresses to the exe's own symbols.
 *
 * Why this exists: measurement found the connected arena spending 12-15 ms of every
 * frame emulating, and a frame-phase breakdown cannot say whether that is the
 * recompiled ARM9, the interpreted ARM7, the 3D rasterizer or something else.
 * A flat sample of the instruction pointer can. */
#ifndef LSW2_SAMPLEPROF_H
#define LSW2_SAMPLEPROF_H

/* Call from the game thread. No-op unless LSW2_SAMPLE_PROF is set. */
void lsw2_sampleprof_start();

/* Called once a second from the frame loop with the current game state and fps,
 * so each dump can say what the game was doing. Cheap. */
void lsw2_sampleprof_note(int game_state, double fps);

#endif /* LSW2_SAMPLEPROF_H */
