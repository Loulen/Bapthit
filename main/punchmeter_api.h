/**
 * @file punchmeter_api.h
 * @brief Forward declarations for the PunchMeter library.
 *
 * This header avoids including punchmeter.h (which pulls in Arduino.h)
 * so that main.cpp does not depend on the full Arduino header chain.
 * The declarations here must stay in sync with punchmeter.h.
 */
#ifndef PUNCHMETER_API_H
#define PUNCHMETER_API_H

// Mirrors PunchmeterConfig from punchmeter.h
struct PunchmeterConfig {
    unsigned long maxScore;  // microseconds, scores faster than this get 999 (default 20000)
    unsigned long minScore;  // microseconds, scores slower than this get 0 (default 500000)
};

void punchmeter_setup();
void punchmeter_loop();
int  punchmeter_get_last_score();
void punchmeter_set_config(const PunchmeterConfig *cfg);

#endif // PUNCHMETER_API_H
