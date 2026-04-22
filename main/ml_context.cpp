#include "ml_context.h"
#include "context_model_data.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include <esp_log.h>

static const char *TAG = "ml_context";

// ── Scaler constants from training output ─────────────────────────────────────
// These must exactly match what the training script printed.
// MinMaxScaler formula: scaled = (x * scale_) + min_
static constexpr float SCALER_MIN_TEMP   = -0.68181818f;
static constexpr float SCALER_MIN_HUM    = -0.89473684f;
static constexpr float SCALER_SCALE_TEMP =  0.05681818f;
static constexpr float SCALER_SCALE_HUM  =  0.02288330f;

// ── Class labels ──────────────────────────────────────────────────────────────
// LabelEncoder sorts alphabetically, confirmed from training output:
// 0 = HEATING_ON, 1 = NORMAL, 2 = WINDOW_OPEN
static const char* kLabels[] = { "HEATING_ON", "NORMAL", "WINDOW_OPEN" };
static constexpr int kNumClasses = 3;

// ── TFLM tensor arena ─────────────────────────────────────────────────────────
// A static block of RAM the interpreter uses for all intermediate tensors.
// 4KB is generous for a 51-parameter model — init log will show actual usage.
static constexpr int kTensorArenaSize = 4 * 1024;
static uint8_t tensor_arena[kTensorArenaSize];

// ── TFLM object pointers (static — live for firmware lifetime) ────────────────
static const tflite::Model *s_model = nullptr;
static tflite::MicroInterpreter *s_interpreter = nullptr;
static TfLiteTensor *s_input = nullptr;
static TfLiteTensor *s_output = nullptr;

bool ml_context_init()
{
    tflite::InitializeTarget();

    // Load the model from the C array in context_model_data.h
    s_model = tflite::GetModel(context_model_tflite);
    if (s_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Model schema version mismatch: got %d, expected %d",
                 s_model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }

    // Register only the ops this model uses.
    // The classifier needs FullyConnected (Dense layers), Relu, and Softmax.
    // Explicitly listing ops avoids linking the entire TFLM op library,
    // keeping firmware flash usage down.
    static tflite::MicroMutableOpResolver<3> resolver;
    resolver.AddFullyConnected();
    resolver.AddRelu();
    resolver.AddSoftmax();

    // Allocate the interpreter statically so it doesn't go on the stack
    static tflite::MicroInterpreter static_interpreter(
        s_model, resolver, tensor_arena, kTensorArenaSize);
    s_interpreter = &static_interpreter;

    // Allocate all input/output tensors inside the arena
    if (s_interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors() failed — arena may be too small");
        return false;
    }

    s_input  = s_interpreter->input(0);
    s_output = s_interpreter->output(0);

    ESP_LOGI(TAG, "TFLM context classifier init OK. Arena used: %d / %d bytes",
             s_interpreter->arena_used_bytes(), kTensorArenaSize);
    return true;
}

int ml_context_run(float temperature, float humidity)
{
    if (!s_interpreter) {
        ESP_LOGE(TAG, "Interpreter not initialised — call ml_context_init() first");
        return -1;
    }

    // ── 1. Apply MinMaxScaler normalisation ───────────────────────────────────
    // Must exactly match sklearn's MinMaxScaler: X_scaled = X * scale_ + min_
    float t_scaled = temperature * SCALER_SCALE_TEMP + SCALER_MIN_TEMP;
    float h_scaled = humidity * SCALER_SCALE_HUM  + SCALER_MIN_HUM;

    // ── 2. Quantise float inputs to int8 ──────────────────────────────────────
    // TFLite quantisation formula: q = float / scale + zero_point
    // scale and zero_point are embedded in the model by the TFLite converter
    // and read directly from the input tensor metadata.
    float in_scale = s_input->params.scale;
    int32_t in_zero_point = s_input->params.zero_point;
    s_input->data.int8[0] = static_cast<int8_t>(t_scaled / in_scale + in_zero_point);
    s_input->data.int8[1] = static_cast<int8_t>(h_scaled / in_scale + in_zero_point);

    // ── 3. Run inference ──────────────────────────────────────────────────────
    if (s_interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "Invoke() failed");
        return -1;
    }

    // ── 4. Find predicted class ───────────────────────────────────────────────
    // The output tensor has one int8 value per class after softmax.
    // The class with the highest value has the highest probability.
    // We compare raw int8 values directly — no dequantisation needed
    // since we only care about which index is largest, not the actual probability.
    float out_scale = s_output->params.scale;
    int32_t out_zero_point = s_output->params.zero_point;

    float max_prob = (s_output->data.int8[0] - out_zero_point) * out_scale;  
    int predicted  = 0;

    for (int i = 1; i < kNumClasses; i++) {
        float prob = (s_output->data.int8[i] - out_zero_point) * out_scale;
        if (prob > max_prob) {
            max_prob  = prob;
            predicted = i;
        }
    }

    ESP_LOGI(TAG, "Context inference: %s (class %d) confidence=%.2f",
            kLabels[predicted], predicted, max_prob);

    return predicted;
}

const char* ml_context_label(int class_id)
{
    if (class_id < 0 || class_id >= kNumClasses) return "UNKNOWN";
    return kLabels[class_id];
}