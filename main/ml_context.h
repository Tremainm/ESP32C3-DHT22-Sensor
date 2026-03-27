#ifndef ML_CONTEXT_H
#define ML_CONTEXT_H

// Initialise the TFLM interpreter with the context classifier model.
// Must be called once before ml_context_run().
// Returns true on success.
bool ml_context_init();

// Run inference on a (temperature, humidity) reading.
// Returns: 0 = HEATING_ON, 1 = NORMAL, 2 = WINDOW_OPEN
// Returns -1 on failure.
int ml_context_run(float temperature, float humidity);

// Convert integer class ID to human-readable string for logging.
const char* ml_context_label(int class_id);

#endif // !ML_CONTEXT_H