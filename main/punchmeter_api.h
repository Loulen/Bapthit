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

// Mirrors PunchmeterConfig from punchmeter.h — must stay in sync
struct PunchmeterConfig {
    unsigned long maxScore;
    unsigned long minScore;
    int defaultRollDelay;
    int rollDelayMod;
    int rollThresh;
    int slowRollThresh;
    int slowRollDelayMod;
    int defaultIncrement;
    int blinkDelay;
    int waveDuration;
    int waveDelay;
};

void punchmeter_setup();
void punchmeter_loop();
int  punchmeter_get_last_score();
void punchmeter_set_config(const PunchmeterConfig *cfg);

#endif // PUNCHMETER_API_H
